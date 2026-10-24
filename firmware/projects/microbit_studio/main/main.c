// SPDX-License-Identifier: Apache-2.0
#include "app_display.h"
#include "app_studio.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "mb_ble_central.h"
#include "mb_build_client.h"
#include "mb_event.h"
#include "mb_flash.h"
#include "mb_mem_probe.h"
#include "mb_voice.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "microbit_studio";

/* The composition root retains owned handles, never controller internals. */
static struct {
    esp_gsp_handle_t ui;
    app_studio_handle_t studio;
} application;

/* One sink for every pipeline report: the panel is the only renderer today, so
 * voice, build client and BLE central all speak mb_event_t into app_studio. */
static void on_pipeline_event(const mb_event_t *event)
{
    if (event == NULL || application.studio == NULL) {
        return;
    }
    const esp_gsp_err_t err = app_studio_apply(application.studio, event);
    if (err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "event %d not fully shown (0x%x)", (int)event->kind, err);
    }
}

/* The spoken request came back with a program; fetch it and start the flash. */
static void on_captured(void)
{
    const esp_err_t err = mb_build_client_request();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start the fetch (%s)", esp_err_to_name(err));
        const mb_event_t failure = {.kind = MB_EVENT_FAILED,
                                    .detail = "the build box is not answering"};
        on_pipeline_event(&failure);
    }
}

/* Called on the fetch task once the image is in PSRAM; the bytes stay valid for
 * the rest of the boot, which is what the flash driver borrows. */
static void on_image(const mb_flash_image_t *image)
{
    const esp_err_t err = mb_ble_central_start(image, on_pipeline_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE central unavailable (%s); UI and Recovery stay up",
                 esp_err_to_name(err));
        const mb_event_t failure = {.kind = MB_EVENT_FAILED,
                                    .detail = "the micro:bit could not be reached"};
        on_pipeline_event(&failure);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    const esp_gsp_err_t studio_err = app_studio_create(application.ui, &application.studio);
    if (studio_err != ESP_GSP_OK) {
        /* No panel text, but Recovery and the pipeline can still run. */
        ESP_LOGE(TAG, "studio UI unavailable (0x%x); Recovery RPC remains active", studio_err);
        return;
    }
    ESP_LOGI(TAG, "studio UI ready, state %s",
             app_studio_state_name(APP_STUDIO_IDLE));

    /* Measure internal RAM before the pipeline claims any of it, then bring up
     * push-to-talk: it brings the WiFi station up, and the build client only
     * waits for an address. A failure here must not take the UI or Recovery
     * down. */
    mb_mem_probe_start();
    mb_mem_probe_stage("ui-ready");
    const esp_err_t voice_err = mb_voice_start(on_pipeline_event, on_captured);
    if (voice_err != ESP_OK) {
        ESP_LOGE(TAG, "push-to-talk unavailable (%s)", esp_err_to_name(voice_err));
    }

    /* The fetch takes tens of seconds, so it owns a task and the panel shows
     * BUILDING while it happens; the image then drives the flash. */
    const esp_err_t build_err = mb_build_client_start(on_pipeline_event, on_image);
    if (build_err != ESP_OK) {
        ESP_LOGE(TAG, "build client unavailable (%s); UI and Recovery stay up",
                 esp_err_to_name(build_err));
    }

    /* Publish HEALTHY only now that the product's own subsystems are up.
     * ESP-Iris never infers acceptance from Iris starting (esp_iris.h:194-202),
     * so without this call a System Update installs and boots but is reported
     * outcome_unknown, and with rollback enabled the bootloader has no
     * acceptance to act on. */
    const esp_err_t healthy_err = esp_iris_mark_healthy();
    if (healthy_err != ESP_OK) {
        ESP_LOGE(TAG, "could not publish HEALTHY (%s)", esp_err_to_name(healthy_err));
    } else {
        ESP_LOGI(TAG, "image accepted, HEALTHY published");
    }
}
