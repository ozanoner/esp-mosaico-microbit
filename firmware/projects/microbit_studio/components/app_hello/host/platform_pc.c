// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "app_hello.h"
#include "gsp_sim_bridge.h"

/* The bridge ABI has no caller context. Keep only its owned opaque handle here. */
static struct {
    app_hello_handle_t controller;
} backend;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return app_hello_create(ui, &backend.controller);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    app_hello_snapshot_t snapshot;
    if (app_hello_read(backend.controller, &snapshot) == ESP_GSP_OK) {
        fprintf(stderr, "hello_backend: count=%u, last_error=%d\n",
                (unsigned)snapshot.count, (int)snapshot.last_error);
    }
    /* The bridge calls deinit after poll exits; no callbacks can use the state. */
    const esp_gsp_err_t err = app_hello_destroy(&backend.controller);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "hello_backend: cleanup failed (%d)\n", err);
    }
}
