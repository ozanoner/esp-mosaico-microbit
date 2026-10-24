// SPDX-License-Identifier: Apache-2.0

#include "app_hello.h"

#include <stdlib.h>

#include <stdio.h>

#include "bundle_gsp.h"

/* Only this implementation can observe or mutate timer/animation state. */
struct app_hello {
    esp_gsp_handle_t ui;
    uint32_t count;
    void *timer;
    unsigned celebration_tick;
    unsigned confetti_frame;
    esp_gsp_err_t last_error;
};

#define CELEBRATION_TICK_MS 100
#define CELEBRATION_TICKS 30
#define CONFETTI_FRAMES 6

/* Visibility frames keep the animation portable across device and sim_bridge. */
static esp_gsp_err_t (*const confetti_visible[])(esp_gsp_handle_t, bool) = {
    gsp_hello_confetti_0_set_visible, gsp_hello_confetti_1_set_visible,
    gsp_hello_confetti_2_set_visible, gsp_hello_confetti_3_set_visible,
    gsp_hello_confetti_4_set_visible, gsp_hello_confetti_5_set_visible,
};

static void remember_error(struct app_hello *state, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        state->last_error = err;
    }
}

static void celebrate_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    if (++state->celebration_tick >= CELEBRATION_TICKS) {
        const esp_gsp_err_t err = esp_gsp_timer_delete(ui, state->timer);
        remember_error(state, err);
        if (err != ESP_GSP_OK) {
            /* Retain a coherent celebration until a later tick can stop it. */
            return;
        }
        state->timer = NULL;
        remember_error(state, gsp_hello_count_set_text(ui, "00"));
        remember_error(state, gsp_hello_celebration_set_visible(ui, false));
        state->count = 0;
        return;
    }
    remember_error(state, confetti_visible[state->confetti_frame](ui, false));
    state->confetti_frame = (state->confetti_frame + 1) % CONFETTI_FRAMES;
    remember_error(state, confetti_visible[state->confetti_frame](ui, true));
}

static void say_hello(esp_gsp_handle_t ui, const esp_gsp_event_t *event,
                      void *ctx)
{
    struct app_hello *state = ctx;
    if (!gsp_hello_event_is_say_hello(event) || state->timer) {
        return;
    }
    if (state->count == 99) {
        state->celebration_tick = 0;
        /* Allocate before showing the overlay: no stuck celebration on failure. */
        state->timer = esp_gsp_timer_create(ui, CELEBRATION_TICK_MS,
                                            celebrate_tick, state);
        if (!state->timer) {
            remember_error(state, ESP_GSP_ERR_NO_MEM);
            return;
        }
        remember_error(state, gsp_hello_celebration_set_visible(ui, true));
        state->count = 100;
        return;
    }
    char text[3];
    const uint32_t next = state->count + 1;
    snprintf(text, sizeof(text), "%02u", (unsigned)next);
    const esp_gsp_err_t err = gsp_hello_count_set_text(ui, text);
    remember_error(state, err);
    if (err == ESP_GSP_OK) {
        state->count = next;
    }
}

esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_GSP_ERR_NO_MEM;
    }
    state->ui = ui;
    remember_error(state, gsp_hello_count_set_text(ui, "00"));
    remember_error(state, gsp_hello_celebration_set_visible(ui, false));
    for (unsigned i = 0; i < CONFETTI_FRAMES; ++i) {
        remember_error(state, confetti_visible[i](ui, i == 0));
    }
    esp_gsp_err_t err = state->last_error;
    if (err == ESP_GSP_OK) {
        /* Publish only a fully initialized context; dispatch may start now. */
        err = esp_gsp_on_event(ui, say_hello, state);
    }
    if (err != ESP_GSP_OK) {
        free(state);
        return err;
    }
    *out_handle = state;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot)
{
    if (!handle || !out_snapshot) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = (app_hello_snapshot_t) {
        .count = handle->count,
        .celebrating = handle->timer != NULL,
        .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_set_state(app_hello_handle_t handle, const char *text)
{
    if (!handle || !text) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    /* Any task may call this: GSP queues the update for the render task and
     * copies the string before returning (esp_gsp.h:351-362). That is what
     * makes it usable from the fetch and BLE tasks without marshalling. */
    const esp_gsp_err_t err = gsp_hello_count_set_text(handle->ui, text);
    remember_error(handle, err);
    return err;
}

esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = *handle;
    if (!state) {
        return ESP_GSP_OK;
    }
    esp_gsp_err_t err = esp_gsp_on_event(state->ui, NULL, NULL);
    if (err != ESP_GSP_OK) {
        return err;
    }
    if (state->timer) {
        err = esp_gsp_timer_delete(state->ui, state->timer);
        if (err != ESP_GSP_OK) {
            return err;
        }
    }
    free(state);
    *handle = NULL;
    return ESP_GSP_OK;
}
