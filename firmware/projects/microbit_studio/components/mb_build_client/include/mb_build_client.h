// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "mb_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fetch one compiled image from the build service (gate G4).
 *
 * POSTs the checked-in payload (`microbit/payloads/hello.ts`, embedded at build
 * time) to CONFIG_MB_BUILD_SERVICE_URL with `?region2=true`, and keeps the
 * reply — the raw region-2 byte stream plus its geometry in `X-Region-Start`,
 * `X-Region-End`, `X-Template-Hash`, `X-Program-Hash` and `X-Block-Count` — in
 * one PSRAM buffer for the lifetime of the boot.
 *
 * The work runs on its own task because the compile takes tens of seconds and
 * the panel must not wait: `status` gets a short token while it happens ("b",
 * "E1"), and `on_image` is called once, on that task, with a borrowed pointer
 * that stays valid until reboot. Nothing calls it on failure; the reason is on
 * the UART log and on `status`.
 *
 * Returns ESP_OK once the task exists, an error if the task could not start.
 */
typedef void (*mb_build_image_fn)(const mb_flash_image_t *image);

esp_err_t mb_build_client_start(mb_flash_status_fn status, mb_build_image_fn on_image);

#ifdef __cplusplus
}
#endif
