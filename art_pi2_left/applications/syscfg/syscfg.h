/*
 * applications/syscfg/syscfg.h
 *
 * NimBLE 1.0.0 configuration for ART-Pi2 — host-only, peripheral role.
 *
 * This file MUST be found before the NimBLE porting layer's own syscfg.h.
 * In Keil MDK: place "applications" first in the IncludePath list, before
 * all NimBLE package directories.
 *
 * This file is intentionally self-contained: it does NOT include
 * <config/config.h> or <rtconfig.h> to avoid the menuconfig dependency.
 * All MYNEWT_VAL_* values needed by the NimBLE host are defined here.
 */

#ifndef H_MYNEWT_SYSCFG_
#define H_MYNEWT_SYSCFG_

/* ── MYNEWT_VAL macro ─────────────────────────────────────────────────────── */
#define MYNEWT_VAL(x)                           MYNEWT_VAL_ ## x

/* ── Controller disable ───────────────────────────────────────────────────── */
/* nimble_port_rtthread.c checks: #if NIMBLE_CFG_CONTROLLER               */
#ifndef NIMBLE_CFG_CONTROLLER
#define NIMBLE_CFG_CONTROLLER                   0
#endif

/* ── Host thread (used by nimble_port_rtthread.c / ble_hs_thread_startup) ── */
#define MYNEWT_VAL_BLE_HOST_THREAD_STACK_SIZE   4096
#define MYNEWT_VAL_BLE_HOST_THREAD_PRIORITY     17

/* ── BLE host role ────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_HOST                     1
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL          1
#define MYNEWT_VAL_BLE_ROLE_CENTRAL             1   /* needed by GATT client internals */
#define MYNEWT_VAL_BLE_ROLE_BROADCASTER         1
#define MYNEWT_VAL_BLE_ROLE_OBSERVER            1
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS          1
#define MYNEWT_VAL_BLE_WHITELIST                1
#define MYNEWT_VAL_BLE_EXT_ADV                  0
#define MYNEWT_VAL_BLE_EXT_ADV_MAX_SIZE         31
#define MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES      0

/* ── Transport / HCI buffer pools ────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_ACL_BUF_COUNT            4
#define MYNEWT_VAL_BLE_ACL_BUF_SIZE             255
#define MYNEWT_VAL_BLE_HCI_EVT_HI_BUF_COUNT     4
#define MYNEWT_VAL_BLE_HCI_EVT_LO_BUF_COUNT     4
#define MYNEWT_VAL_BLE_HCI_EVT_BUF_SIZE         70
#define MYNEWT_VAL_BLE_HCI_ACL_OUT_COUNT        4

/* ── ATT server ───────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_ATT_PREFERRED_MTU        247
#define MYNEWT_VAL_BLE_ATT_SVR_FIND_INFO        1
#define MYNEWT_VAL_BLE_ATT_SVR_FIND_TYPE        1
#define MYNEWT_VAL_BLE_ATT_SVR_INDICATE         1
#define MYNEWT_VAL_BLE_ATT_SVR_MAX_PREP_ENTRIES 64
#define MYNEWT_VAL_BLE_ATT_SVR_NOTIFY           1
#define MYNEWT_VAL_BLE_ATT_SVR_QUEUED_WRITE     1
#define MYNEWT_VAL_BLE_ATT_SVR_QUEUED_WRITE_TMO 30000
#define MYNEWT_VAL_BLE_ATT_SVR_READ             1
#define MYNEWT_VAL_BLE_ATT_SVR_READ_BLOB        1
#define MYNEWT_VAL_BLE_ATT_SVR_READ_GROUP_TYPE  1
#define MYNEWT_VAL_BLE_ATT_SVR_READ_MULT        1
#define MYNEWT_VAL_BLE_ATT_SVR_READ_TYPE        1
#define MYNEWT_VAL_BLE_ATT_SVR_SIGNED_WRITE     1
#define MYNEWT_VAL_BLE_ATT_SVR_WRITE            1
#define MYNEWT_VAL_BLE_ATT_SVR_WRITE_NO_RSP     1

/* ── GAP ──────────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_GAP_MAX_PENDING_CONN_PARAM_UPDATE 1

/* ── GATT ─────────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_CHRS      (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_DSCS      (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_SVCS      (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_DISC_CHR_UUID      (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_DISC_SVC_UUID      (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_FIND_INC_SVCS      (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_INDICATE            1
#define MYNEWT_VAL_BLE_GATT_MAX_PROCS           4
#define MYNEWT_VAL_BLE_GATT_NOTIFY              1
#define MYNEWT_VAL_BLE_GATT_READ               (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_READ_LONG          (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_READ_MAX_ATTRS      8
#define MYNEWT_VAL_BLE_GATT_READ_MULT          (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_READ_UUID          (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_RESUME_RATE         1000
#define MYNEWT_VAL_BLE_GATT_SIGNED_WRITE       (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_WRITE              (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_WRITE_LONG         (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_WRITE_MAX_ATTRS     4
#define MYNEWT_VAL_BLE_GATT_WRITE_NO_RSP       (MYNEWT_VAL_BLE_ROLE_CENTRAL)
#define MYNEWT_VAL_BLE_GATT_WRITE_RELIABLE     (MYNEWT_VAL_BLE_ROLE_CENTRAL)

/* ── Host stack ───────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_HS_AUTO_START            1
#define MYNEWT_VAL_BLE_HS_DEBUG                 0
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL             0
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL_ITVL        1000
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL_THRESH      2
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL_TX_ON_DISCONNECT 0
#define MYNEWT_VAL_BLE_HS_PHONY_HCI_ACKS        0
#define MYNEWT_VAL_BLE_HS_REQUIRE_OS            1
#define MYNEWT_VAL_BLE_HS_STOP_ON_SHUTDOWN      1
#define MYNEWT_VAL_BLE_HS_SYSINIT_STAGE         200

/* ── L2CAP ────────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM        0
#define MYNEWT_VAL_BLE_L2CAP_COC_MTU            284  /* 292 - 8 */
#define MYNEWT_VAL_BLE_L2CAP_JOIN_RX_FRAGS      1
#define MYNEWT_VAL_BLE_L2CAP_MAX_CHANS          3    /* 3 * MAX_CONNECTIONS */
#define MYNEWT_VAL_BLE_L2CAP_RX_FRAG_TIMEOUT    30000
#define MYNEWT_VAL_BLE_L2CAP_SIG_MAX_PROCS      1

/* ── Security Manager — disabled (no bonding, no SM) ─────────────────────── */
#define MYNEWT_VAL_BLE_SM_BONDING               0
#define MYNEWT_VAL_BLE_SM_IO_CAP                3    /* BLE_HS_IO_NO_INPUT_OUTPUT */
#define MYNEWT_VAL_BLE_SM_KEYPRESS              0
#define MYNEWT_VAL_BLE_SM_LEGACY                0
#define MYNEWT_VAL_BLE_SM_MAX_PROCS             1
#define MYNEWT_VAL_BLE_SM_MITM                  0
#define MYNEWT_VAL_BLE_SM_OOB_DATA_FLAG         0
#define MYNEWT_VAL_BLE_SM_OUR_KEY_DIST          0
#define MYNEWT_VAL_BLE_SM_SC                    0
#define MYNEWT_VAL_BLE_SM_SC_DEBUG_KEYS         0
#define MYNEWT_VAL_BLE_SM_THEIR_KEY_DIST        0
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS          0
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS          8

/* ── Mesh — disabled ──────────────────────────────────────────────────────── */
/* MUST be 0 or mesh source files become linked; we do not compile them.    */
#define MYNEWT_VAL_BLE_MESH                     0

/* ── Privacy / RPA ───────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_RPA_TIMEOUT              300

/* ── Monitor ──────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_MONITOR_RTT              0
#define MYNEWT_VAL_BLE_MONITOR_RTT_BUFFERED     1
#define MYNEWT_VAL_BLE_MONITOR_RTT_BUFFER_NAME  "monitor"
#define MYNEWT_VAL_BLE_MONITOR_RTT_BUFFER_SIZE  256
#define MYNEWT_VAL_BLE_MONITOR_UART             0
#define MYNEWT_VAL_BLE_MONITOR_UART_BAUDRATE    1000000
#define MYNEWT_VAL_BLE_MONITOR_UART_BUFFER_SIZE 64
#define MYNEWT_VAL_BLE_MONITOR_UART_DEV         "uart0"
#define MYNEWT_VAL_BLE_MONITOR_CONSOLE_BUFFER_SIZE 128

/* ── GAP service ─────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_SVC_GAP_APPEARANCE       0
#define MYNEWT_VAL_BLE_SVC_GAP_APPEARANCE_WRITE_PERM (-1)
#define MYNEWT_VAL_BLE_SVC_GAP_CENTRAL_ADDRESS_RESOLUTION (-1)
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME      "ART-Pi2-IMU"
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME_MAX_LENGTH 31
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME_WRITE_PERM (-1)
#define MYNEWT_VAL_BLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL 0
#define MYNEWT_VAL_BLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL 0
#define MYNEWT_VAL_BLE_SVC_GAP_PPCP_SLAVE_LATENCY 0
#define MYNEWT_VAL_BLE_SVC_GAP_PPCP_SUPERVISION_TMO 0

/* ── Alert Notification Service ──────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_SVC_ANS_NEW_ALERT_CAT    0
#define MYNEWT_VAL_BLE_SVC_ANS_UNR_ALERT_CAT    0

/* ── Battery service ─────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_SVC_BAS_BATTERY_LEVEL_NOTIFY_ENABLE 1
#define MYNEWT_VAL_BLE_SVC_BAS_BATTERY_LEVEL_READ_PERM 0

/* ── HCI transport selection ─────────────────────────────────────────────── */
/* All zero — our nimble_hci_adapter.c provides the transport symbols.      */
#define MYNEWT_VAL_BLE_HCI_TRANSPORT_NIMBLE_BUILTIN 0
#define MYNEWT_VAL_BLE_HCI_TRANSPORT_UART       0
#define MYNEWT_VAL_BLE_HCI_TRANSPORT_RAM        0
#define MYNEWT_VAL_BLE_HCI_TRANSPORT_EMSPI      0
#define MYNEWT_VAL_BLE_HCI_TRANSPORT_SOCKET     0

/* ── OS cputime ──────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_OS_CPUTIME_FREQ              1000000
#define MYNEWT_VAL_OS_CPUTIME_TIMER_NUM         5

/* ── MSYS memory system ──────────────────────────────────────────────────── */
#define MYNEWT_VAL_MSYS_1_BLOCK_COUNT           12
#define MYNEWT_VAL_MSYS_1_BLOCK_SIZE            292
#define MYNEWT_VAL_MSYS_2_BLOCK_COUNT           0
#define MYNEWT_VAL_MSYS_2_BLOCK_SIZE            0

/* ── OS misc ─────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_OS_CLI                       0
#define MYNEWT_VAL_OS_COREDUMP                  0

/* ── BSP — not NRF ───────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BSP_NRF51                    0
#define MYNEWT_VAL_BSP_NRF52                    0
#define MYNEWT_VAL_MCU_NRF52840                 0
#define MYNEWT_VAL_TRNG                         0

/* ── tinycrypt ───────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_TINYCRYPT_UECC_RNG_USE_TRNG  0
#define MYNEWT_VAL_TINYCRYPT_UECC_RNG_TRNG_DEV_NAME "trng"

/* ── Float ───────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_FLOAT_USER                   0

#endif /* H_MYNEWT_SYSCFG_ */
