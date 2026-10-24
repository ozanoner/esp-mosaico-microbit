// SPDX-License-Identifier: Apache-2.0

#include "mb_mem_probe.h"

#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MEM";

/* Single 32-bit writer (the sampler) and single reader (whoever logs a stage).
 * A torn read cannot happen on this target and the value is diagnostic only, so
 * no lock: taking one here would perturb the very number being measured. */
static size_t s_lowest_internal;
static bool s_have_lowest;
static bool s_started;

#define MB_MEM_SAMPLE_PERIOD_MS 5

static void mb_mem_sampler(void *arg)
{
    (void)arg;
    for (;;) {
        const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (!s_have_lowest || free_now < s_lowest_internal) {
            s_lowest_internal = free_now;
            s_have_lowest = true;
        }
        vTaskDelay(pdMS_TO_TICKS(MB_MEM_SAMPLE_PERIOD_MS));
    }
}

void mb_mem_probe_start(void)
{
    if (s_started) {
        return;
    }
    /* Priority 1: the sampler must never outrank the renderer or the BLE host,
     * it only needs to run between their allocations. */
    if (xTaskCreate(mb_mem_sampler, "mb_mem", 3072, NULL, 1, NULL) == pdPASS) {
        s_started = true;
    } else {
        ESP_LOGE(TAG, "sampler task could not be created; stage lines will report free/largest only");
    }
}

void mb_mem_probe_stage(const char *stage)
{
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t low = s_have_lowest ? s_lowest_internal : free_now;

    ESP_LOGI(TAG, "[MEM %s] SRAM free=%u largest=%u low=%u", stage != NULL ? stage : "?",
             (unsigned)free_now, (unsigned)largest, (unsigned)low);

    /* Reset the window: the next line reports what happened after this point. */
    s_lowest_internal = free_now;
    s_have_lowest = true;
}
