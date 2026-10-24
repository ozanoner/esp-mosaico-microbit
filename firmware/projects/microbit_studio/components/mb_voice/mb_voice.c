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

        esp_codec_dev_sample_info_t info = {
            .sample_rate = SAMPLE_RATE,
            .bits_per_sample = BYTES_PER_SAMPLE * 8,
            .channel = CHANNELS,
        };
        if (esp_codec_dev_open(s_mic, &info) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "could not open the microphone");
            continue;
        }
        esp_codec_dev_set_in_gain(s_mic, 30.0f);

        s_pcm_len = 0;
        ESP_LOGI(TAG, "recording (max %d s)", CONFIG_MB_VOICE_MAX_SECONDS);
        while (s_recording && (s_pcm_len + READ_CHUNK) <= CAPACITY) {
            const int got = esp_codec_dev_read(s_mic, s_wav + WAV_HEADER_BYTES + s_pcm_len, READ_CHUNK);
            if (got > 0) {
                s_pcm_len += (size_t)got;
            } else if (got < 0) {
                ESP_LOGE(TAG, "microphone read failed (%d)", got);
                break;
            }
        }
        esp_codec_dev_close(s_mic);

        if (s_pcm_len < READ_CHUNK) {
            ESP_LOGW(TAG, "recording too short (%u bytes), discarded", (unsigned)s_pcm_len);
            continue;
        }

        wav_header(s_wav, (uint32_t)s_pcm_len);
        ESP_LOGI(TAG, "sending %u bytes of audio", (unsigned)(WAV_HEADER_BYTES + s_pcm_len));
        post_wav(s_wav, WAV_HEADER_BYTES + s_pcm_len);
    }
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

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "microphone init failed");
        return ESP_FAIL;
    }

    s_pressed = xSemaphoreCreateBinary();
    if (s_pressed == NULL) {
        return ESP_ERR_NO_MEM;
    }

    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int count = 0;
    esp_err_t err = bsp_iot_button_create(buttons, &count, BSP_BUTTON_NUM);
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

    ESP_LOGI(TAG, "push-to-talk ready: hold the AI button, release to send to %s",
             CONFIG_MB_VOICE_SERVICE_URL);
    return ESP_OK;
}
