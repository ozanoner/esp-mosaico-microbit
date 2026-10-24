// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "mb_status_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the BLE central probe task (gate G2).
 *
 * Scans for an advertiser whose complete or shortened name starts with
 * `BBC micro:bit`, connects, subscribes to E97D3B10-…, writes `0xEE`, logs the
 * notification, disconnects and scans again, so a second round needs no reboot.
 *
 * Returns ESP_OK once the task exists. Every connection outcome — found,
 * connected, subscribed, answered — arrives on the UART log, never as a return
 * code: the radio work happens after this call returns.
 *
 * Idempotent: a second call returns ESP_ERR_INVALID_STATE.
 */
esp_err_t mb_ble_central_start(void);

#ifdef __cplusplus
}
#endif
