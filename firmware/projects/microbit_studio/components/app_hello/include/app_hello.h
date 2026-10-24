// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque UI controller; ownership and threading contracts: ../README.md. */
typedef struct app_hello *app_hello_handle_t;

/** Value copy for diagnostics; no pointers into private controller state. */
typedef struct {
    uint32_t count;             /**< Button presses, 0..100; 100 during celebration. */
    bool celebrating;           /**< True while the 3-second animation is active. */
    esp_gsp_err_t last_error;   /**< Most recent callback/setter error, or ESP_GSP_OK. */
} app_hello_snapshot_t;

/** Create a hello controller for ui; the caller owns *out_handle.
 * ui/out_handle must be non-NULL, and ui must outlive the controller.
 * Returns ESP_GSP_OK or a GSP error; clears *out_handle on failure. */
esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle);

/** Copy diagnostics to out_snapshot; both arguments must be non-NULL.
 * Call on the UI loop or after dispatch stops; this is not a cross-thread read.
 * Returns ESP_GSP_OK or ESP_GSP_ERR_INVALID_ARG; output unchanged on error. */
esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot);

/** Detach callbacks, release the controller and clear *handle.
 * Quiesce event/timer dispatch first and keep the borrowed ui alive.
 * Returns ESP_GSP_OK or a GSP error; retains the handle if cleanup fails. */
esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle);

/** Show a short status token in the scene's text label (gate G4, temporary).
 *
 * The studio state machine (issue #7) replaces this. Until then it is the only
 * text surface the retained Hello scene has, and it is what keeps a failed
 * flash from being a silent one.
 *
 * Safe from any task: GSP applies state updates on the render task and copies
 * the string before returning (esp_gsp.h:351-362). A button press during a run
 * still rewrites the same label, so this is for bench use, not for the product.
 * Returns ESP_GSP_OK, ESP_GSP_ERR_INVALID_ARG, or the setter's error.
 */
esp_gsp_err_t app_hello_set_state(app_hello_handle_t handle, const char *text);

#ifdef __cplusplus
}
#endif
