/*
 * nimble_hci_adapter.h — NimBLE ↔ CYW43438 HCI transport shim
 *
 * Bridges the NimBLE host stack (TX/RX callbacks) to the CYW43438 BT core
 * via the UART7 device managed by cyw43438_bt.c.
 */

#ifndef NIMBLE_HCI_ADAPTER_H
#define NIMBLE_HCI_ADAPTER_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialize the NimBLE HCI transport shim.
 *
 *         - Registers the NimBLE RX ISR signal with cyw43438_bt
 *           (replaces the internal semaphore used during HCI Reset init).
 *         - Starts the nimble_hci_rx RT-Thread thread which continuously
 *           reads H4 packets from UART7 and dispatches them to the NimBLE
 *           host via ble_transport_ll_to_hs_evt() / ble_transport_ll_to_hs_acl().
 *
 * @pre    cyw43438_bt_is_ready() == RT_TRUE
 * @pre    Must be called before nimble_port_init()
 *
 * @return RT_EOK on success, negative RT error code otherwise.
 */
rt_err_t nimble_hci_adapter_init(void);

#ifdef __cplusplus
}
#endif

#endif /* NIMBLE_HCI_ADAPTER_H */
