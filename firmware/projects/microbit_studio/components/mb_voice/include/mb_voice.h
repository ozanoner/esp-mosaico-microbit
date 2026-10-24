// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "mb_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Push-to-talk: hold the AI button to record, release to send.
 *
 * Records 16 kHz mono 16-bit PCM into PSRAM, wraps it in a WAV header and
 * POSTs it to CONFIG_MB_VOICE_SERVICE_URL (with ?include_hex=false, because the
 * reply's geometry is what matters until the flash path needs the image).
 *
 * Failure is non-fatal by design: the UI and Recovery keep running without it.
 */
esp_err_t mb_voice_start(mb_event_fn status);

#ifdef __cplusplus
}
#endif
