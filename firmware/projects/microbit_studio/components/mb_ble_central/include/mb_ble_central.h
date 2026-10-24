// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the BLE central task (gates G2 and G3).
 *
 * Scans for an advertiser whose complete or shortened name starts with
 * `BBC micro:bit`, connects, subscribes to the partial-flash characteristic
 * (`E97D3B10-…`), then runs one of two paths:
 *
 * - until a transfer has completed: read region 1 and region 2 hashes, refuse to
 *   write if the DAL differs from our image, stream region 2 in 64-byte blocks,
 *   send END_OF_TX, and verify on the next round by re-reading region 2;
 * - afterwards: write `0xEE` and log the STATUS answer, then disconnect and
 *   rescan, so repeated rounds need no reboot.
 *
 * Returns ESP_OK once the task exists. Every connection outcome — found,
 * connected, subscribed, hashes, blocks, errors — arrives on the UART log, never
 * as a return code: the radio work happens after this call returns.
 *
 * Idempotent: a second call returns ESP_ERR_INVALID_STATE.
 */
esp_err_t mb_ble_central_start(void);

#ifdef __cplusplus
}
#endif
