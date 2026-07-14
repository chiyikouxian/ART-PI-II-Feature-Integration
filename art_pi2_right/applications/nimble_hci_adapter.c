/*
 * nimble_hci_adapter.c — NimBLE 1.0.0 transport shim for CYW43438 via UART7
 *
 * NimBLE 1.0.0 uses the LEGACY transport API (nimble/ble_hci_trans.h).
 * This file provides all required transport symbols so that the NimBLE host
 * stack can communicate with the CYW43438 hardware controller over UART7.
 *
 * ── Transport symbols provided ──────────────────────────────────────────────
 *
 *  ble_hci_trans_cfg_hs()         host registers RX callbacks here
 *  ble_hci_trans_cfg_ll()         stub (no SW controller)
 *  ble_hci_trans_hs_cmd_tx()      host → controller HCI command via UART7
 *  ble_hci_trans_hs_acl_tx()      host → controller ACL data via UART7
 *  ble_hci_trans_buf_alloc/free() flat buffer management (events + cmd)
 *  ble_hci_trans_set_acl_free_cb()
 *  ble_hci_trans_reset()
 *  ble_hci_trans_ll_evt_tx()      stub
 *  ble_hci_trans_ll_acl_tx()      stub
 *
 * ── RX path ─────────────────────────────────────────────────────────────────
 *
 *  UART7 ISR → _rx_signal() → _rx_data_sem
 *  _rx_thread_entry():
 *    reads bytes from cyw43438_bt_uart_read()
 *    H4 state machine assembles complete packets
 *    EVT → ble_hci_trans_buf_alloc() + _rx_cmd_cb()
 *    ACL → ACL mbuf alloc + _rx_acl_cb()
 *
 * ── TX path ─────────────────────────────────────────────────────────────────
 *
 *  NimBLE host calls ble_hci_trans_hs_cmd_tx() / ble_hci_trans_hs_acl_tx()
 *  → prepend H4 type byte + write to cyw43438_bt_uart_write()
 */

#include <rtthread.h>
#include <string.h>
#include "nimble_hci_adapter.h"
#include "cyw43438_bt.h"

/* NimBLE legacy transport API */
#include "nimble/ble_hci_trans.h"
/* NimBLE BLE constants (BLE_MBUF_MEMBLOCK_OVERHEAD) */
#include "nimble/ble.h"
/* HCI packet definitions (BLE_HCI_DATA_HDR_SZ) */
#include "nimble/hci_common.h"

/* OS primitives */
#include "os/os_mempool.h"
#include "os/os_mbuf.h"

/* ARMCLANG assert.h unconditionally redefines assert — override it here,
 * after all headers that may pull in <assert.h>, to avoid __aeabi_assert. */
#undef  assert
#define assert(e) RT_ASSERT(e)

/*
 * __aeabi_assert — ARM AEABI assert handler.
 *
 * ARMCLANG compiles assert(expr) into a call to __aeabi_assert() when NDEBUG
 * is not defined and the full ARM C library is not linked (MicroLib / no-stdlib).
 * All NimBLE host files that call assert() will reference this symbol.
 * Define it once here so the linker finds it without pulling in the C library.
 */
void __aeabi_assert(const char *expr, const char *file, int line)
{
    rt_kprintf("[ASSERT] %s:%d  %s\n", file, line, expr);
    RT_ASSERT(0);
}

#define DBG_SECTION_NAME  "HCI_ADAPT"
#define DBG_LEVEL          DBG_INFO
#include <rtdbg.h>

/* ── H4 packet type bytes ─────────────────────────────────────────────────── */
#define H4_CMD  0x01
#define H4_ACL  0x02
#define H4_EVT  0x04

/* ── RX thread config ─────────────────────────────────────────────────────── */
#define HCI_RX_THREAD_PRIO        18
#define HCI_RX_THREAD_STACK_SIZE  2048

static struct rt_thread    _rx_thread;
static rt_uint8_t          _rx_stack[HCI_RX_THREAD_STACK_SIZE];
static struct rt_semaphore _rx_data_sem;

/* ── Host-side RX callbacks (registered by NimBLE host) ─────────────────── */
static ble_hci_trans_rx_cmd_fn *_rx_cmd_cb  = NULL;
static void                    *_rx_cmd_arg = NULL;
static ble_hci_trans_rx_acl_fn *_rx_acl_cb  = NULL;
static void                    *_rx_acl_arg = NULL;

/* ── Memory pools ─────────────────────────────────────────────────────────── */

/* HCI event buffers — high priority (connections, command complete, etc.) */
static struct os_mempool  _evt_hi_pool;
static os_membuf_t        _evt_hi_buf[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_HCI_EVT_HI_BUF_COUNT),
                    MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE))];

/* HCI event buffers — low priority (advertising reports; may be dropped) */
static struct os_mempool  _evt_lo_pool;
static os_membuf_t        _evt_lo_buf[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_HCI_EVT_LO_BUF_COUNT),
                    MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE))];

/* HCI command buffer (host allocates one before sending each command) */
static struct os_mempool  _cmd_pool;
static os_membuf_t        _cmd_buf[
    OS_MEMPOOL_SIZE(1, BLE_HCI_TRANS_CMD_SZ)];

/*
 * ACL data buffers.
 * Each block must hold the mbuf overhead + HCI ACL header (4 B) + payload.
 * BLE_MBUF_MEMBLOCK_OVERHEAD and BLE_HCI_DATA_HDR_SZ come from nimble/ble.h.
 */
#define ACL_BLOCK_SIZE  OS_ALIGN(MYNEWT_VAL(BLE_ACL_BUF_SIZE)          \
                                 + BLE_MBUF_MEMBLOCK_OVERHEAD           \
                                 + BLE_HCI_DATA_HDR_SZ,                 \
                                 OS_ALIGNMENT)

static struct os_mempool_ext  _acl_pool;
static struct os_mbuf_pool    _acl_mbuf_pool;
static os_membuf_t            _acl_buf[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_ACL_BUF_COUNT), ACL_BLOCK_SIZE)];

/* ── ble_hci_trans API — buffer allocation ───────────────────────────────── */

uint8_t *ble_hci_trans_buf_alloc(int type)
{
    uint8_t *buf;

    switch (type) {
    case BLE_HCI_TRANS_BUF_CMD:
        buf = os_memblock_get(&_cmd_pool);
        break;
    case BLE_HCI_TRANS_BUF_EVT_HI:
        buf = os_memblock_get(&_evt_hi_pool);
        if (buf == NULL) {
            /* Fall back to low-priority pool when high is exhausted */
            buf = os_memblock_get(&_evt_lo_pool);
        }
        break;
    case BLE_HCI_TRANS_BUF_EVT_LO:
        buf = os_memblock_get(&_evt_lo_pool);
        break;
    default:
        assert(0);
        buf = NULL;
        break;
    }
    return buf;
}

void ble_hci_trans_buf_free(uint8_t *buf)
{
    int rc;

    /*
     * Identify which pool the buffer came from and return it.
     * Same pattern as ble_hci_uart.c: cmd pool may hold an event buffer
     * when the controller echoes a command-complete immediately.
     */
    if (os_memblock_from(&_evt_hi_pool, buf)) {
        rc = os_memblock_put(&_evt_hi_pool, buf);
        assert(rc == 0);
    } else if (os_memblock_from(&_evt_lo_pool, buf)) {
        rc = os_memblock_put(&_evt_lo_pool, buf);
        assert(rc == 0);
    } else {
        assert(os_memblock_from(&_cmd_pool, buf));
        rc = os_memblock_put(&_cmd_pool, buf);
        assert(rc == 0);
    }
    (void)rc;
}

int ble_hci_trans_set_acl_free_cb(os_mempool_put_fn *cb, void *arg)
{
    _acl_pool.mpe_put_cb  = cb;
    _acl_pool.mpe_put_arg = arg;
    return 0;
}

/* ── ble_hci_trans API — callback registration ───────────────────────────── */

/*
 * Called by NimBLE host during init to supply its RX callbacks.
 *   cmd_cb : delivers HCI events from controller to host
 *   acl_cb : delivers ACL data from controller to host
 */
void ble_hci_trans_cfg_hs(ble_hci_trans_rx_cmd_fn *cmd_cb, void *cmd_arg,
                           ble_hci_trans_rx_acl_fn *acl_cb, void *acl_arg)
{
    _rx_cmd_cb  = cmd_cb;
    _rx_cmd_arg = cmd_arg;
    _rx_acl_cb  = acl_cb;
    _rx_acl_arg = acl_arg;
}

/* Stub: no software BLE controller present */
void ble_hci_trans_cfg_ll(ble_hci_trans_rx_cmd_fn *cmd_cb, void *cmd_arg,
                           ble_hci_trans_rx_acl_fn *acl_cb, void *acl_arg)
{
    (void)cmd_cb; (void)cmd_arg; (void)acl_cb; (void)acl_arg;
}

/* ── ble_hci_trans API — TX (host → CYW43438 controller via UART7) ────────── */

/*
 * ble_hci_trans_hs_cmd_tx — send HCI command to the controller.
 *
 * buf layout (from NimBLE host): [opcode_lo][opcode_hi][param_len][params...]
 * H4 format written to UART7:   [0x01][opcode_lo][opcode_hi][param_len][params]
 *
 * The buffer is freed after transmission; it was allocated via
 * ble_hci_trans_buf_alloc(BLE_HCI_TRANS_BUF_CMD).
 */
int ble_hci_trans_hs_cmd_tx(uint8_t *cmd)
{
    uint8_t  h4    = H4_CMD;
    uint16_t total = (uint16_t)(3u + (uint16_t)cmd[2]);  /* opcode(2)+len(1)+params */

    cyw43438_bt_uart_write(&h4,  1u);
    cyw43438_bt_uart_write(cmd,  total);
    ble_hci_trans_buf_free(cmd);
    return 0;
}

/*
 * ble_hci_trans_hs_acl_tx — send ACL data to the controller.
 *
 * om contains: [handle_lo][handle_hi+flags][data_len_lo][data_len_hi][data...]
 * H4 format written to UART7: [0x02] + mbuf chain bytes
 *
 * The mbuf chain is freed after transmission.
 */
int ble_hci_trans_hs_acl_tx(struct os_mbuf *om)
{
    uint8_t        h4  = H4_ACL;
    struct os_mbuf *cur;

    cyw43438_bt_uart_write(&h4, 1u);
    for (cur = om; cur != NULL; cur = SLIST_NEXT(cur, om_next)) {
        if (cur->om_len > 0)
            cyw43438_bt_uart_write(cur->om_data, cur->om_len);
    }
    os_mbuf_free_chain(om);
    return 0;
}

/* ── ble_hci_trans API — LL-side stubs ───────────────────────────────────── */

/* No software controller — these should never be called from application code */
int ble_hci_trans_ll_evt_tx(uint8_t *hci_ev)
{
    ble_hci_trans_buf_free(hci_ev);
    return 0;
}

int ble_hci_trans_ll_acl_tx(struct os_mbuf *om)
{
    os_mbuf_free_chain(om);
    return 0;
}

/* ── ble_hci_trans API — reset ───────────────────────────────────────────── */

int ble_hci_trans_reset(void)
{
    /* UART7 is owned by cyw43438_bt.c; nothing to reset here */
    return 0;
}

/* ── RX path: H4 state machine ───────────────────────────────────────────── */

/*
 * Allocate an ACL mbuf from our pool.
 * usrhdr_len = 0 because BLE_DEVICE=0 and BLE_HS_FLOW_CTRL=0.
 */
static struct os_mbuf *_acl_mbuf_alloc(void)
{
    return os_mbuf_get_pkthdr(&_acl_mbuf_pool, 0);
}

/* ── H4 packet assembly buffer ────────────────────────────────────────────── */
#define RX_BUF_SIZE  270   /* covers max HCI EVT (1+2+255=258) and typical ACL */

static uint8_t    _rxbuf[RX_BUF_SIZE];
static uint16_t   _rxoff;          /* bytes accumulated so far            */
static uint16_t   _rxneed;         /* total bytes needed for this packet  */
static uint8_t    _rxtype;         /* H4 type byte of current packet      */

typedef enum { RX_S_TYPE, RX_S_HEADER, RX_S_PAYLOAD } rx_state_t;
static rx_state_t _rxstate;

/* Header byte count for each packet type (not counting H4 type byte) */
static inline uint8_t _hdr_len(uint8_t t)
{
    if (t == H4_EVT) return 2;   /* evt_code(1) + param_len(1)             */
    if (t == H4_ACL) return 4;   /* handle(2)   + data_len(2)              */
    return 0;
}

/* Total expected packet length (including H4 type byte) once header is in buf */
static uint16_t _total_len(uint8_t t)
{
    if (t == H4_EVT) {
        /* _rxbuf[0]=0x04, [1]=evt_code, [2]=param_len */
        return (uint16_t)(1u + 2u + _rxbuf[2]);
    }
    if (t == H4_ACL) {
        /* _rxbuf[0]=0x02, [1..2]=handle, [3..4]=data_len (little-endian) */
        return (uint16_t)(1u + 4u + ((uint16_t)_rxbuf[4] << 8 | _rxbuf[3]));
    }
    return 0;
}

/*
 * Dispatch a fully-assembled H4 packet to the NimBLE host.
 *
 * For EVT (0x04):
 *   - buf[1..] = evt_code + param_len + params
 *   - Advertising-report sub-events use EVT_LO pool; rest use EVT_HI.
 *   - Deliver to _rx_cmd_cb(evbuf, _rx_cmd_arg).
 *
 * For ACL (0x02):
 *   - buf[1..] = handle(2) + data_len(2) + data
 *   - Allocate ACL mbuf, append payload, deliver to _rx_acl_cb.
 */
static void _dispatch(void)
{
    uint16_t       payload_len = (uint16_t)(_rxoff - 1u); /* skip H4 byte */
    const uint8_t *payload     = &_rxbuf[1];

    if (_rxtype == H4_EVT && _rx_cmd_cb != NULL) {
        /*
         * Determine event priority:
         *   LE Meta (0x3E) with subevent ADV_RPT (0x02) or
         *   EXT_ADV_RPT (0x05) → low-priority (may be dropped under load).
         *   Everything else → high-priority.
         */
        int is_adv_rpt = (payload[0] == 0x3E && payload_len >= 3u &&
                          (payload[2] == 0x02u || payload[2] == 0x05u));
        int buf_type   = is_adv_rpt ? BLE_HCI_TRANS_BUF_EVT_LO
                                    : BLE_HCI_TRANS_BUF_EVT_HI;

        uint8_t *evbuf = ble_hci_trans_buf_alloc(buf_type);
        if (evbuf == NULL) {
            if (is_adv_rpt)
                LOG_D("adv rpt dropped (lo pool empty)");
            else
                LOG_W("evt alloc failed — dropped (code=0x%02X)", payload[0]);
            goto reset;
        }
        rt_memcpy(evbuf, payload, payload_len);
        int rc = _rx_cmd_cb(evbuf, _rx_cmd_arg);
        if (rc != 0)
            ble_hci_trans_buf_free(evbuf);

    } else if (_rxtype == H4_ACL && _rx_acl_cb != NULL) {
        struct os_mbuf *om = _acl_mbuf_alloc();
        if (om == NULL) {
            LOG_W("acl mbuf alloc failed — dropped");
            goto reset;
        }
        int rc = os_mbuf_append(om, payload, payload_len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            LOG_W("acl mbuf append failed");
            goto reset;
        }
        rc = _rx_acl_cb(om, _rx_acl_arg);
        if (rc != 0)
            os_mbuf_free_chain(om);
    }

reset:
    _rxoff   = 0;
    _rxneed  = 1;
    _rxstate = RX_S_TYPE;
}

/* Called from UART7 RX interrupt context — just signal the thread */
static void _rx_signal(void)
{
    rt_sem_release(&_rx_data_sem);
}

/* ── RX thread ────────────────────────────────────────────────────────────── */
static void _rx_thread_entry(void *param)
{
    (void)param;

    _rxstate = RX_S_TYPE;
    _rxoff   = 0;
    _rxneed  = 1;

    LOG_I("HCI RX thread started");

    while (1) {
        /* Block until ISR signals bytes are available */
        rt_sem_take(&_rx_data_sem, RT_WAITING_FOREVER);

        /* Drain all bytes currently in the UART ring buffer */
        uint8_t b;
        while (cyw43438_bt_uart_read(&b, 1) == 1) {

            if (_rxoff >= RX_BUF_SIZE) {
                LOG_E("RX buf overrun — resync");
                _rxoff = 0; _rxstate = RX_S_TYPE; _rxneed = 1;
            }
            _rxbuf[_rxoff++] = b;

            switch (_rxstate) {
            case RX_S_TYPE:
                _rxtype = b;
                if (_hdr_len(b) == 0) {
                    LOG_W("unknown H4 type 0x%02X — discard", b);
                    _rxoff = 0;
                    break;
                }
                _rxneed  = (uint16_t)(1u + _hdr_len(b));
                _rxstate = RX_S_HEADER;
                if (_rxoff < _rxneed) break;
                /* FALLTHROUGH — already have type + full header */

            case RX_S_HEADER:
                if (_rxoff >= _rxneed) {
                    uint16_t total = _total_len(_rxtype);
                    if (total == 0u || total > RX_BUF_SIZE) {
                        LOG_W("bad pkt len %u — resync", total);
                        _rxoff = 0; _rxstate = RX_S_TYPE; _rxneed = 1;
                        break;
                    }
                    _rxneed  = total;
                    _rxstate = RX_S_PAYLOAD;
                    if (_rxoff < _rxneed) break;
                    /* FALLTHROUGH — already have full packet */
                } else {
                    break;
                }
                /* FALLTHROUGH */

            case RX_S_PAYLOAD:
                if (_rxoff >= _rxneed) {
                    _dispatch();
                }
                break;
            }
        }
    }
}

/* ── Public: init ─────────────────────────────────────────────────────────── */
rt_err_t nimble_hci_adapter_init(void)
{
    int      rc;
    rt_err_t ret;

    /* Initialise ACL extended mempool + mbuf pool */
    rc = os_mempool_ext_init(&_acl_pool,
                             MYNEWT_VAL(BLE_ACL_BUF_COUNT),
                             ACL_BLOCK_SIZE,
                             _acl_buf,
                             "hci_acl");
    if (rc != 0) {
        LOG_E("acl pool init failed: %d", rc);
        return -RT_ERROR;
    }

    rc = os_mbuf_pool_init(&_acl_mbuf_pool,
                           &_acl_pool.mpe_mp,
                           ACL_BLOCK_SIZE,
                           MYNEWT_VAL(BLE_ACL_BUF_COUNT));
    if (rc != 0) {
        LOG_E("acl mbuf pool init failed: %d", rc);
        return -RT_ERROR;
    }

    /* Command pool (1 outstanding command at a time) */
    rc = os_mempool_init(&_cmd_pool, 1, BLE_HCI_TRANS_CMD_SZ,
                         _cmd_buf, "hci_cmd");
    if (rc != 0) {
        LOG_E("cmd pool init failed: %d", rc);
        return -RT_ERROR;
    }

    /* Event pools */
    rc = os_mempool_init(&_evt_hi_pool,
                         MYNEWT_VAL(BLE_HCI_EVT_HI_BUF_COUNT),
                         MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE),
                         _evt_hi_buf, "hci_evth");
    if (rc != 0) {
        LOG_E("evt_hi pool init failed: %d", rc);
        return -RT_ERROR;
    }

    rc = os_mempool_init(&_evt_lo_pool,
                         MYNEWT_VAL(BLE_HCI_EVT_LO_BUF_COUNT),
                         MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE),
                         _evt_lo_buf, "hci_evtl");
    if (rc != 0) {
        LOG_E("evt_lo pool init failed: %d", rc);
        return -RT_ERROR;
    }

    /* RX semaphore */
    ret = rt_sem_init(&_rx_data_sem, "hci_rxs", 0, RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK) {
        LOG_E("sem init failed: %d", ret);
        return ret;
    }

    /* Hand UART7 RX ISR signal over to this adapter */
    cyw43438_bt_set_rx_callback(_rx_signal);

    /* Start the H4 receive thread */
    ret = rt_thread_init(&_rx_thread, "hci_rx", _rx_thread_entry, RT_NULL,
                         _rx_stack, sizeof(_rx_stack), HCI_RX_THREAD_PRIO, 10);
    if (ret != RT_EOK) {
        LOG_E("thread init failed: %d", ret);
        return ret;
    }

    ret = rt_thread_startup(&_rx_thread);
    if (ret != RT_EOK) {
        LOG_E("thread startup failed: %d", ret);
        return ret;
    }

    LOG_I("NimBLE HCI adapter ready (UART7 ↔ NimBLE host, old transport API)");
    return RT_EOK;
}
