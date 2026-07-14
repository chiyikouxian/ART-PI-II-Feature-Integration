/*
 * syscfg.h — NimBLE compile-time configuration for ART-Pi2 / CYW43438
 *
 * Hand-crafted for Keil MDK build (no newt toolchain).
 * All MYNEWT_VAL(X) references in NimBLE source expand to MYNEWT_VAL_X.
 *
 * This file must appear on the Keil include path BEFORE the NimBLE package
 * directories so it shadows any syscfg.h that the package might generate.
 * Recommended Keil IncludePath order: applications;packages\nimble-latest\...
 */

#ifndef H_MYNEWT_SYSCFG_
#define H_MYNEWT_SYSCFG_

/* ── Host role: peripheral only ──────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL              (1)
#define MYNEWT_VAL_BLE_ROLE_CENTRAL                 (0)
#define MYNEWT_VAL_BLE_ROLE_OBSERVER                (0)
#define MYNEWT_VAL_BLE_ROLE_BROADCASTER             (0)

/* ── Connections ──────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS              (1)
#define MYNEWT_VAL_BLE_MAX_PERIODIC_SYNCS           (0)

/* ── Memory pools ─────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_ACL_BUF_COUNT                (4)
#define MYNEWT_VAL_BLE_ACL_BUF_SIZE                 (255)
#define MYNEWT_VAL_BLE_HCI_EVT_COUNT                (8)
#define MYNEWT_VAL_BLE_HCI_EVT_BUF_SIZE             (70)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_DISCARDABLE_COUNT (8)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_COUNT          (4)
#define MYNEWT_VAL_MSYS_1_BLOCK_COUNT               (12)
#define MYNEWT_VAL_MSYS_1_BLOCK_SIZE                (292)
#define MYNEWT_VAL_MSYS_2_BLOCK_COUNT               (0)
#define MYNEWT_VAL_MSYS_2_BLOCK_SIZE                (0)

/* ── ATT / GATT ───────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_ATT_PREFERRED_MTU            (247)
#define MYNEWT_VAL_BLE_GATT_MAX_PROCS               (4)
#define MYNEWT_VAL_BLE_GATT_MAX_SVCS                (3)
#define MYNEWT_VAL_BLE_GATT_MAX_CHRS                (8)
#define MYNEWT_VAL_BLE_GATT_MAX_DSCS                (4)

/* ── Security (SMP) — disabled to save RAM ───────────────────────────────── */
#define MYNEWT_VAL_BLE_SM_LEGACY                    (0)
#define MYNEWT_VAL_BLE_SM_SC                        (0)
#define MYNEWT_VAL_BLE_SM_BONDING                   (0)
#define MYNEWT_VAL_BLE_SM_IO_CAP                    (BLE_SM_IO_CAP_NO_IO)
#define MYNEWT_VAL_BLE_SM_OUR_KEY_DIST              (0)
#define MYNEWT_VAL_BLE_SM_THEIR_KEY_DIST            (0)

/* ── Bonding storage — disabled ──────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS              (0)
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS              (0)

/* ── Advertising: legacy only (BT4.x) ────────────────────────────────────── */
#define MYNEWT_VAL_BLE_EXT_ADV                      (0)
#define MYNEWT_VAL_BLE_ADV_LEGACY_PDU_ONLY          (1)
#define MYNEWT_VAL_BLE_PERIODIC_ADV                 (0)

/* ── L2CAP ────────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM            (0)

/* ── Mesh — disabled ──────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_MESH                         (0)

/* ── Privacy / RPA ───────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_HOST_BASED_PRIVACY           (0)
#define MYNEWT_VAL_BLE_RPA_TIMEOUT                  (300)

/* ── Logging ──────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_HS_LOG_LVL                   (1)   /* 1=error */
#define MYNEWT_VAL_LOG_LEVEL                        (1)

/* ── NPL / OS ─────────────────────────────────────────────────────────────── */
#define MYNEWT_VAL_OS_CPUTIME_FREQ                  (1000000)
#define MYNEWT_VAL_OS_EVENTQ_MAX_EVT_COUNT          (32)

/* ── NimBLE host task (spawned by nimble_port_rtthread_init) ─────────────── */
/* Override default priority to 17 — sits between mpu6050=16 and hci_rx=18   */
#define MYNEWT_VAL_BLE_HOST_TASK_PRIO               (17)
#define MYNEWT_VAL_BLE_HOST_STOP_TIMEOUT_MS         (2000)

/* ── Misc features ───────────────────────────────────────────────────────── */
#define MYNEWT_VAL_BLE_VERSION                      (50)  /* BT 5.0 host */
#define MYNEWT_VAL_BLE_HS_AUTO_START                (1)
#define MYNEWT_VAL_BLE_MONITOR_UART                 (0)
#define MYNEWT_VAL_BLE_MONITOR_RTT                  (0)

#endif /* H_MYNEWT_SYSCFG_ */
