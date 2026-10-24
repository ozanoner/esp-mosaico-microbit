// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gsp.h"
#include "mb_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Eight screens. TRANSCRIBED and GENERATING are deliberately absent: the build
 * service answers ASR -> LLM -> mkc in one request, so the device cannot see a
 * transcript to confirm or a generation phase to show (issue #7 comment). */
typedef enum {
    APP_STUDIO_IDLE = 0,
    APP_STUDIO_LISTENING,
    APP_STUDIO_BUILDING,
    APP_STUDIO_AWAIT_MICROBIT,
    APP_STUDIO_FLASHING,
    APP_STUDIO_VERIFYING,
    APP_STUDIO_DONE,
    APP_STUDIO_FAILED,
    APP_STUDIO_STATE_COUNT,
} app_studio_state_t;

/* Observable state, for adapters and tests: what the panel shows and where the
 * bear currently sits (authored scene pixels). */
typedef struct {
    app_studio_state_t state;
    int32_t bear_y;
    esp_gsp_err_t last_error;
} app_studio_snapshot_t;

typedef struct app_studio app_studio_t;
typedef app_studio_t *app_studio_handle_t;

/** Create the studio controller and show IDLE.
 * Safe from any task: GSP applies updates on the render task. Returns an
 * allocation or GSP error, or ESP_GSP_ERR_INVALID_STATE if called twice. */
esp_gsp_err_t app_studio_create(esp_gsp_handle_t ui, app_studio_handle_t *out_handle);

/** Apply one pipeline event, moving to the state it implies.
 * Unknown transitions are ignored (logged) rather than aborting: the UI must not
 * take the pipeline down. Returns the first GSP error it saw, if any. */
esp_gsp_err_t app_studio_apply(app_studio_handle_t handle, const mb_event_t *event);

/** Read the visible state. Returns ESP_GSP_ERR_INVALID_ARG for NULL. */
esp_gsp_err_t app_studio_state(app_studio_handle_t handle, app_studio_state_t *out_state);

/** Copy the observable state. Returns ESP_GSP_ERR_INVALID_ARG for NULL. */
esp_gsp_err_t app_studio_read(app_studio_handle_t handle, app_studio_snapshot_t *out_snapshot);

/** Stable upper-case name, matching the on-screen headline. Never NULL. */
const char *app_studio_state_name(app_studio_state_t state);

/** Stop timers, drop callbacks and free. NULL handle and already-NULL succeed. */
esp_gsp_err_t app_studio_destroy(app_studio_handle_t *handle);

#ifdef __cplusplus
}
#endif
