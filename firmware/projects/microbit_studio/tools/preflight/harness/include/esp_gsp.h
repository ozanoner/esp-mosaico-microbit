// Host shim for the preflight harness: just enough ESP-GSP to link app_studio.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_gsp *esp_gsp_handle_t;

typedef enum {
    ESP_GSP_OK = 0,
    ESP_GSP_ERR_INVALID_ARG,
    ESP_GSP_ERR_INVALID_STATE,
    ESP_GSP_ERR_NO_MEM,
} esp_gsp_err_t;

/* Timers: the harness hands back an opaque token and lets the test fire it. */
typedef void (*esp_gsp_timer_fn)(esp_gsp_handle_t gsp, void *ctx);

void *esp_gsp_timer_create(esp_gsp_handle_t gsp, uint32_t ms, esp_gsp_timer_fn fn,
                           void *ctx);
esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t gsp, void *timer);

#ifdef __cplusplus
}
#endif
