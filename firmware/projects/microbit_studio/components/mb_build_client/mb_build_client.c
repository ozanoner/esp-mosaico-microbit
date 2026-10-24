// SPDX-License-Identifier: Apache-2.0
//
// Gate G4: ask the build service for one compiled image, and keep the bytes the
// flash client streams. POST /v1/build?region2=true answers with region 2 plus
// its geometry in headers, so the device parses no Intel HEX and never holds
// 1.8 MB of base64.
//
// The reply is collected in the event handler, not with esp_http_client_read():
// perform() drains the socket itself, and a later read() returns 0 — the bug
// that made mb_voice's first version see an empty reply (mb_voice.c:77-104).

#include "mb_build_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mb_build";

/* The payload is embedded, not generated: the gate builds exactly the file the
 * repository pins, so a change to the .ts is visible in what the device asks
 * for. Symbol names come from EMBED_TXTFILES in CMakeLists.txt. */
extern const uint8_t k_payload_start[] asm("_binary_hello_ts_start");
extern const uint8_t k_payload_end[] asm("_binary_hello_ts_end");

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t len;
    bool truncated;
    bool geometry_seen;
    long region_start;
    long region_end;
    long block_count;
    char template_hash[17];
    char program_hash[17];
} reply_t;

static mb_flash_image_t s_image;
static mb_flash_status_fn s_status;
static mb_build_image_fn s_on_image;
static uint8_t *s_bytes; /* PSRAM; kept for the life of the boot */

static void report(const char *text)
{
    if (s_status != NULL) {
        s_status(text);
    }
}

static bool header_is(const char *key, const char *want)
{
    return key != NULL && strcasecmp(key, want) == 0;
}

static void take_header(reply_t *reply, const char *key, const char *value)
{
    if (value == NULL) {
        return;
    }
    if (header_is(key, "X-Region-Start")) {
        reply->region_start = strtol(value, NULL, 10);
        reply->geometry_seen = true;
    } else if (header_is(key, "X-Region-End")) {
        reply->region_end = strtol(value, NULL, 10);
        reply->geometry_seen = true;
    } else if (header_is(key, "X-Block-Count")) {
        reply->block_count = strtol(value, NULL, 10);
    } else if (header_is(key, "X-Template-Hash")) {
        snprintf(reply->template_hash, sizeof(reply->template_hash), "%s", value);
    } else if (header_is(key, "X-Program-Hash")) {
        snprintf(reply->program_hash, sizeof(reply->program_hash), "%s", value);
    }
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    reply_t *reply = (reply_t *)evt->user_data;
    if (reply == NULL) {
        return ESP_OK;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        take_header(reply, evt->header_key, evt->header_value);
        return ESP_OK;

    case HTTP_EVENT_ON_DATA: {
        if (evt->data == NULL || evt->data_len <= 0) {
            return ESP_OK;
        }
        const size_t room = reply->capacity - reply->len;
        const size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        memcpy(reply->data + reply->len, evt->data, take);
        reply->len += take;
        if (take < (size_t)evt->data_len) {
            reply->truncated = true;
        }
        return ESP_OK;
    }

    default:
        return ESP_OK;
    }
}

/* "08d9ef69355ad697" -> 8 bytes. Returns false on anything else. */
static bool parse_hash(const char *text, uint8_t out[8])
{
    if (text == NULL || strlen(text) != 16) {
        return false;
    }
    for (size_t i = 0; i < 8; i++) {
        unsigned value = 0;
        if (sscanf(text + i * 2, "%2x", &value) != 1) {
            return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static size_t json_escape(const char *src, size_t len, char *dst, size_t cap)
{
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)src[i];
        char pair[8];
        const char *piece = pair;
        size_t piece_len = 2;
        switch (c) {
        case '"': pair[0] = '\\'; pair[1] = '"'; break;
        case '\\': pair[0] = '\\'; pair[1] = '\\'; break;
        case '\n': pair[0] = '\\'; pair[1] = 'n'; break;
        case '\r': pair[0] = '\\'; pair[1] = 'r'; break;
        case '\t': pair[0] = '\\'; pair[1] = 't'; break;
        default:
            if (c < 0x20) {
                snprintf(pair, sizeof(pair), "\\u%04x", c);
                piece_len = 6;
            } else {
                pair[0] = (char)c;
                piece_len = 1;
            }
            break;
        }
        if (used + piece_len + 1u > cap) {
            return 0;
        }
        memcpy(dst + used, piece, piece_len);
        used += piece_len;
    }
    return used;
}

static esp_err_t wait_for_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    for (int i = 0; i < CONFIG_MB_BUILD_IP_TIMEOUT_S * 2; i++) {
        if (sta != NULL && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "device IP " IPSTR, IP2STR(&ip.ip));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "no WiFi address after %d s; is mb_voice up?", CONFIG_MB_BUILD_IP_TIMEOUT_S);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t fetch(mb_flash_image_t *out)
{
    const size_t src_len = (size_t)(k_payload_end - k_payload_start);
    const size_t body_cap = src_len * 2u + 64u;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %u-byte request body", (unsigned)body_cap);
        return ESP_ERR_NO_MEM;
    }

    const size_t prefix = (size_t)snprintf(body, body_cap, "{\"source\":\"");
    const size_t escaped = json_escape((const char *)k_payload_start, src_len, body + prefix,
                                       body_cap - prefix - 8u);
    if (escaped == 0) {
        ESP_LOGE(TAG, "the payload does not fit the escaped body buffer");
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t body_len = prefix + escaped + 2u;
    memcpy(body + prefix + escaped, "\"}", 3u);

    s_bytes = heap_caps_malloc(CONFIG_MB_BUILD_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_bytes == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %d-byte image buffer", CONFIG_MB_BUILD_MAX_BYTES);
        free(body);
        return ESP_ERR_NO_MEM;
    }
    reply_t reply = {.data = s_bytes, .capacity = CONFIG_MB_BUILD_MAX_BYTES};

    char url[192];
    snprintf(url, sizeof(url), "%s?region2=true", CONFIG_MB_BUILD_SERVICE_URL);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 180000,
        .event_handler = on_http_event,
        .user_data = &reply,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "could not create the HTTP client");
        free(body);
        free(s_bytes);
        s_bytes = NULL;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)body_len);

    const esp_err_t err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        goto fail;
    }
    if (http_status != 200) {
        /* A failure body is the service's JSON, not image bytes: say what it
         * said, bounded, so a compiler error is visible on the log. */
        ESP_LOGE(TAG, "service returned %d: %.*s", http_status,
                 (int)(reply.len > 240u ? 240u : reply.len), (const char *)reply.data);
        goto fail;
    }
    if (!reply.geometry_seen || reply.block_count <= 0) {
        ESP_LOGE(TAG, "reply carries no geometry (blocks=%ld)", reply.block_count);
        goto fail;
    }
    if (reply.truncated ||
        reply.len != (size_t)reply.block_count * MB_FLASH_BLOCK_BYTES) {
        ESP_LOGE(TAG, "reply is %u bytes%s; %ld blocks need %u",
                 (unsigned)reply.len, reply.truncated ? " (truncated)" : "",
                 reply.block_count, (unsigned)((size_t)reply.block_count * MB_FLASH_BLOCK_BYTES));
        goto fail;
    }
    if (reply.region_end <= reply.region_start) {
        ESP_LOGE(TAG, "reply region 0x%05lX-0x%05lX is empty", reply.region_start, reply.region_end);
        goto fail;
    }
    if (!parse_hash(reply.template_hash, out->template_hash) ||
        !parse_hash(reply.program_hash, out->program_hash)) {
        ESP_LOGE(TAG, "hashes are not 8-byte hex: template '%s' program '%s'",
                 reply.template_hash, reply.program_hash);
        goto fail;
    }

    out->data = s_bytes;
    out->len = reply.len;
    out->region2_start = (uint32_t)reply.region_start;
    out->region2_end = (uint32_t)reply.region_end;
    out->block_count = (uint32_t)reply.block_count;
    return ESP_OK;

fail:
    free(s_bytes);
    s_bytes = NULL;
    return ESP_FAIL;
}

static void build_task(void *arg)
{
    (void)arg;

    /* The panel says "building" from the moment the fetch starts: a 40 s
     * compile with a blank screen is indistinguishable from a hung app. */
    report("b");

    if (wait_for_ip() != ESP_OK) {
        report("E1");
        vTaskDelete(NULL);
        return;
    }
    if (fetch(&s_image) != ESP_OK) {
        report("E1");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "image ready: %u blocks from 0x%05X-0x%05X, template %02X%02X%02X%02X%02X%02X%02X%02X, "
                  "program %02X%02X%02X%02X%02X%02X%02X%02X",
             (unsigned)s_image.block_count, s_image.region2_start, s_image.region2_end - 1u,
             s_image.template_hash[0], s_image.template_hash[1], s_image.template_hash[2],
             s_image.template_hash[3], s_image.template_hash[4], s_image.template_hash[5],
             s_image.template_hash[6], s_image.template_hash[7], s_image.program_hash[0],
             s_image.program_hash[1], s_image.program_hash[2], s_image.program_hash[3],
             s_image.program_hash[4], s_image.program_hash[5], s_image.program_hash[6],
             s_image.program_hash[7]);

    if (s_on_image != NULL) {
        s_on_image(&s_image);
    }
    vTaskDelete(NULL);
}

esp_err_t mb_build_client_start(mb_flash_status_fn status, mb_build_image_fn on_image)
{
    if (s_bytes != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_status = status;
    s_on_image = on_image;

    ESP_LOGI(TAG, "fetching one image from %s", CONFIG_MB_BUILD_SERVICE_URL);
    if (xTaskCreate(build_task, "mb_build", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the fetch task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
