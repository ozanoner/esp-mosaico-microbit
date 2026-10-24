// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_studio.h"
#include "gsp_sim_bridge.h"

/* The bridge ABI has no caller context: keep only its owned handle here. */
static struct {
    app_studio_handle_t controller;
} backend;

/* A state named in STUDIO_SIM_STATE is applied at init so a headless run renders
 * exactly that screen, without waiting on timers. Simulation only. */
static const struct {
    const char *name;
    mb_event_kind_t kind;
    const char *detail;
    uint16_t done;
    uint16_t total;
} sim_states[] = {
    {"LISTENING", MB_EVENT_LISTENING_STARTED, NULL, 0, 0},
    {"BUILDING", MB_EVENT_BUILD_STARTED, NULL, 0, 0},
    {"AWAIT_MICROBIT", MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0},
    {"FLASHING", MB_EVENT_FLASH_PROGRESS, NULL, 62, 96},
    {"VERIFYING", MB_EVENT_VERIFYING, NULL, 0, 0},
    {"DONE", MB_EVENT_DONE, NULL, 0, 0},
    {"FAILED", MB_EVENT_FAILED, "micro:bit is not advertising", 0, 0},
};

static void apply_requested_state(app_studio_handle_t controller)
{
    const char *want = getenv("STUDIO_SIM_STATE");
    if (want == NULL || *want == '\0' || strcmp(want, "IDLE") == 0) {
        return;
    }
    for (size_t i = 0; i < sizeof(sim_states) / sizeof(sim_states[0]); ++i) {
        if (strcmp(want, sim_states[i].name) != 0) {
            continue;
        }
        const mb_event_t event = {
            .kind = sim_states[i].kind,
            .detail = sim_states[i].detail,
            .progress_done = sim_states[i].done,
            .progress_total = sim_states[i].total,
        };
        if (app_studio_apply(controller, &event) != ESP_GSP_OK) {
            fprintf(stderr, "studio_backend: state %s reported a GSP error\n", want);
        }
        return;
    }
    fprintf(stderr, "studio_backend: unknown STUDIO_SIM_STATE '%s'\n", want);
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    esp_gsp_err_t err = app_studio_create(ui, &backend.controller);
    if (err != ESP_GSP_OK) {
        return err;
    }
    apply_requested_state(backend.controller);
    return ESP_GSP_OK;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    /* bear_y proves the bob timer ran: 179/185 means it moved, 182 means it did
     * not, and last_error reports any rejected GSP property write. */
    app_studio_snapshot_t snapshot;
    if (app_studio_read(backend.controller, &snapshot) == ESP_GSP_OK) {
        fprintf(stderr, "studio_backend: state=%s bear_y=%d last_error=%d\n",
                app_studio_state_name(snapshot.state), (int)snapshot.bear_y,
                (int)snapshot.last_error);
    }
    if (app_studio_destroy(&backend.controller) != ESP_GSP_OK) {
        fprintf(stderr, "studio_backend: cleanup reported an error\n");
    }
}
