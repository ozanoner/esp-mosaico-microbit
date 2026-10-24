// SPDX-License-Identifier: Apache-2.0
//
// Push-to-talk capture and the call to POST /v1/speak. The WAV is sent as raw
// body with Content-Type: audio/wav; the service needs no multipart parsing.

#include "mb_voice.h"

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "bsp/audio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_button.h"

static const char *TAG = "mb_voice";

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BYTES_PER_SAMPLE 2
#define WAV_HEADER_BYTES 44u
#define READ_CHUNK 2048u
#define CAPACITY ((size_t)SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE * CONFIG_MB_VOICE_MAX_SECONDS)

static uint8_t *s_wav;          /* PSRAM: header + PCM, header written on stop */
static size_t s_pcm_len;
static volatile bool s_recording;
static SemaphoreHandle_t s_pressed;
static esp_codec_dev_handle_t s_mic;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Canonical 44-byte PCM WAV header: the service decodes with the stdlib `wave`
 * module, so the format tag must be 1 and the sizes exact. */
static void wav_header(uint8_t *p, uint32_t data_len)
{
    memcpy(p, "RIFF", 4);
    put_le32(p + 4, 36u + data_len);
    memcpy(p + 8, "WAVEfmt ", 8);
    put_le32(p + 16, 16u);
    put_le16(p + 20, 1u);
    put_le16(p + 22, CHANNELS);
    put_le32(p + 24, SAMPLE_RATE);
    put_le32(p + 28, SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE);
    put_le16(p + 32, CHANNELS * BYTES_PER_SAMPLE);
    put_le16(p + 34, BYTES_PER_SAMPLE * 8u);
    memcpy(p + 36, "data", 4);
    put_le32(p + 40, data_len);
}

static void log_reply(int status, const char *body)
{
    if (body == NULL || body[0] == 0) {
        ESP_LOGE(TAG, "service returned %d with an empty body", status);
        return;
    }
    if (status == 200) {
        /* !include_hex=false keeps this under a kilobyte, so log it whole. The
         * transcript and geometry are both in here; parsing arrives with the UI
         * work that displays them, together with a JSON dependency. */
        ESP_LOGI(TAG, "reply: %s", body);
    } else {
        ESP_LOGE(TAG, "service returned %d: %s", status, body);
    }
}

static void post_wav(const uint8_t *wav, size_t len)
{
    char url[192];
    snprintf(url, sizeof(url), "%s?include_hex=false", CONFIG_MB_VOICE_SERVICE_URL);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
    };

    /* Log where the board actually sits: an unreachable service is otherwise
     * indistinguishable from a wrong subnet or an isolating access point. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta != NULL && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        ESP_LOGI(TAG, "device IP " IPSTR " gw " IPSTR, IP2STR(&ip.ip), IP2STR(&ip.gw));
    } else {
        ESP_LOGW(TAG, "no IP yet; the POST will fail");
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "could not create the HTTP client");
        return;
    }

    char *reply = heap_caps_malloc(CONFIG_MB_VOICE_REPLY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (reply == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %d-byte reply buffer", CONFIG_MB_VOICE_REPLY_BYTES);
        esp_http_client_cleanup(client);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)wav, (int)len);

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
    } else {
        const int status = esp_http_client_get_status_code(client);
        reply[0] = 0;
        const int got = esp_http_client_read_response(client, reply, CONFIG_MB_VOICE_REPLY_BYTES - 1);
        if (got > 0) {
            reply[got] = 0;
        }
        log_reply(status, reply);
    }

    free(reply);
    esp_http_client_cleanup(client);
}

static void on_ai_button(void *button, void *user_data)
{
    (void)user_data;

    const button_event_t event = iot_button_get_event((button_handle_t)button);
    if (event == BUTTON_PRESS_DOWN) {
        s_recording = true;
        xSemaphoreGive(s_pressed);
    } else if (event == BUTTON_PRESS_UP) {
        s_recording = false;
    }
}

static void record_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(s_pressed, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_pcm_len = 0;
        ESP_LOGI(TAG, "recording (max %d s)", CONFIG_MB_VOICE_MAX_SECONDS);
        while (s_recording && (s_pcm_len + READ_CHUNK) <= CAPACITY) {
            /* esp_codec_dev_read returns a status, not a byte count:
             * ESP_CODEC_DEV_OK (0) means the chunk was filled — see
             * examples/voice_wake_cmd/main/main.c:141. Reading the return as a
             * length is what made every capture look empty. */
            const int rc = esp_codec_dev_read(s_mic, s_wav + WAV_HEADER_BYTES + s_pcm_len, READ_CHUNK);
            if (rc == ESP_CODEC_DEV_OK) {
                s_pcm_len += READ_CHUNK;
            } else {
                ESP_LOGE(TAG, "microphone read failed (%d)", rc);
                break;
            }
        }

        if (s_pcm_len < READ_CHUNK) {
            ESP_LOGW(TAG, "recording too short (%u bytes), discarded", (unsigned)s_pcm_len);
            continue;
        }

        wav_header(s_wav, (uint32_t)s_pcm_len);
        ESP_LOGI(TAG, "sending %u bytes of audio", (unsigned)(WAV_HEADER_BYTES + s_pcm_len));
        post_wav(s_wav, WAV_HEADER_BYTES + s_pcm_len);
    }
}

#if CONFIG_MB_VOICE_SELFTEST_SECONDS > 0
static void selftest_task(void *arg);
#endif

static void wifi_start(void)
{
    /* The WiFi driver logs the SSID and, at debug, association details; keep the
     * device log free of network identifiers. Warnings and errors still show. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    /* Credentials are provisioned once through Vibe Mode
     * (`mosaico.py iris test recovery-wifi`), which stores them in NVS. The app
     * only has to bring the station up with FLASH storage so it picks them up. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "could not create the WiFi station netif");
        return;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Credentials come from .env via Kconfig, so the app does not depend on the
     * retained firmware's NVS layout. */
    if (CONFIG_MB_WIFI_SSID[0] != '\0') {
        wifi_config_t sta = {0};
        const size_t ssid_len = strnlen(CONFIG_MB_WIFI_SSID, sizeof(sta.sta.ssid));
        const size_t pass_len = strnlen(CONFIG_MB_WIFI_PASS, sizeof(sta.sta.password));
        memcpy(sta.sta.ssid, CONFIG_MB_WIFI_SSID, ssid_len);
        memcpy(sta.sta.password, CONFIG_MB_WIFI_PASS, pass_len);
        sta.sta.threshold.authmode = pass_len > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_STA, &sta);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi config failed: %s", esp_err_to_name(err));
            return;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_connect();

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    for (int i = 0; i < 40; i++) {
        if (sta_netif != NULL && esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "WiFi up: " IPSTR, IP2STR(&ip.ip));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "no IP after 20 s; the service will be unreachable");
}

static void wifi_task(void *arg)
{
    (void)arg;
    wifi_start();
    vTaskDelete(NULL);
}

esp_err_t mb_voice_start(void)
{
    if (s_wav != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_wav = heap_caps_malloc(WAV_HEADER_BYTES + CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_wav == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %u-byte recording buffer",
                 (unsigned)(WAV_HEADER_BYTES + CAPACITY));
        return ESP_ERR_NO_MEM;
    }

    /* Same sequence as the tinyml demo's working capture path
     * (devkits/mosaico/audio_mosaico.cpp:45-78): BSP audio init explicitly, then
     * one mic opened at 16 kHz mono and left open for the lifetime. */
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP audio init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "microphone init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t mic_fs = {0};
    mic_fs.bits_per_sample = BYTES_PER_SAMPLE * 8;
    mic_fs.channel = CHANNELS;
    mic_fs.sample_rate = SAMPLE_RATE;
    err = esp_codec_dev_open(s_mic, &mic_fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not open the microphone: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_codec_dev_set_in_gain(s_mic, 30.0f);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set the microphone gain: %s", esp_err_to_name(err));
    }

    /* On its own task: associating can take seconds and the panel must not wait. */
    if (xTaskCreate(wifi_task, "mb_wifi", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the WiFi task");
    }

    s_pressed = xSemaphoreCreateBinary();
    if (s_pressed == NULL) {
        return ESP_ERR_NO_MEM;
    }

    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int count = 0;
    err = bsp_iot_button_create(buttons, &count, BSP_BUTTON_NUM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_PRESS_DOWN, NULL, on_ai_button, NULL) != ESP_OK ||
        iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_PRESS_UP, NULL, on_ai_button, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "could not register the AI button callbacks");
        return ESP_FAIL;
    }

    if (xTaskCreate(record_task, "mb_voice", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_MB_VOICE_SELFTEST_SECONDS > 0
    if (xTaskCreate(selftest_task, "mb_voice_st", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the self-test task");
    }
#endif

    ESP_LOGI(TAG, "push-to-talk ready: hold the AI button, release to send to %s",
             CONFIG_MB_VOICE_SERVICE_URL);
    return ESP_OK;
}

#if CONFIG_MB_VOICE_SELFTEST_SECONDS > 0
static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    ESP_LOGW(TAG, "self-test: recording %d s without the button", CONFIG_MB_VOICE_SELFTEST_SECONDS);
    s_recording = true;
    xSemaphoreGive(s_pressed);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_MB_VOICE_SELFTEST_SECONDS * 1000));
    s_recording = false;
    vTaskDelete(NULL);
}
#endif
