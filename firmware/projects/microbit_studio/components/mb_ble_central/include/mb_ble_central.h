// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "mb_event.h"
#include "mb_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the BLE central and run the gate-G4 cycle loop.
 *
 * Scans for an advertiser whose complete or shortened name starts with
 * `BBC micro:bit`, connects, subscribes to the partial-flash characteristic
 * (`E97D3B10-…`), then per cycle:
 *
 *   read region 1 and region 2 hashes → refuse to write if the DAL differs from
 *   `image->template_hash` → stream region 2 in 64-byte blocks → END_OF_TX →
 *   reconnect → re-read region 2 → compare with `image->program_hash`.
 *
 * Every cycle writes, so the loop proves what the gate is about: a flash that
 * needs no A+B tap afterwards and no power cycle in between. The count comes
 * from CONFIG_MB_BLE_POC_CYCLES; after the last verified cycle the driver falls
 * back to the gate-G2 STATUS probe and stays quiet.
 *
 * `image` is borrowed: the caller owns it and it must outlive every transfer
 * (the build client keeps it in PSRAM for the lifetime of the boot).
 * `status` receives typed pipeline events (scanning, block progress, verifying,
 * done, and failures carrying a kid-facing sentence); NULL is allowed.
 *
 * Returns ESP_OK once the task exists. Every connection outcome — found,
 * connected, subscribed, hashes, blocks, errors — arrives on the UART log and
 * on `status`, never as a return code: the radio work happens after this call
 * returns.
 *
 * Idempotent: a second call returns ESP_ERR_INVALID_STATE.
 */
esp_err_t mb_ble_central_start(const mb_flash_image_t *image, mb_event_fn status);

#ifdef __cplusplus
}
#endif
