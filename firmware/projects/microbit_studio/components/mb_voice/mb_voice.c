// SPDX-License-Identifier: Apache-2.0
//
// Push-to-talk capture and the call to POST /v1/speak. The WAV is sent as raw
// body with Content-Type: audio/wav; the service needs no multipart parsing.

#include "mb_voice.h"

#include <math.h>
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
/* Speech at arm's length clipped the ADC at the BSP example's 30 dB: every
 * press reported peak 32768 of 32767 while silence sat at 322, so the top of
 * the waveform was flat. 24 dB keeps the same capture well clear of full scale
 * (6 dB = half the amplitude) and costs nothing the ASR needs. */
#define MIC_GAIN_DB 24.0f
#define CAPACITY ((size_t)SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE * CONFIG_MB_VOICE_MAX_SECONDS)

static uint8_t *s_wav;          /* PSRAM: header + PCM, header written on stop */
static size_t s_pcm_len;
static volatile bool s_recording;
static SemaphoreHandle_t s_pressed;
static mb_event_fn s_event;
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

/* The response body is only available while esp_http_client_perform() is on the
 * stack: it drains the socket itself and a later esp_http_client_read() returns
 * 0, which is why the reply used to arrive empty. Collect it in the event
 * handler instead (esp_http_client_event_id_t, HTTP_EVENT_ON_DATA). */
typedef struct {
    char *buf;
    size_t capacity;
    size_t len;
    bool truncated;
} reply_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    reply_t *reply = (reply_t *)evt->user_data;
    if (reply == NULL || evt->event_id != HTTP_EVENT_ON_DATA ||
        evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    const size_t room = reply->capacity - 1u - reply->len;
    const size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    memcpy(reply->buf + reply->len, evt->data, take);
    reply->len += take;
    reply->buf[reply->len] = '\0';
    if (take < (size_t)evt->data_len) {
        reply->truncated = true;
    }
    return ESP_OK;
}

static void log_reply(int status, const reply_t *reply)
{
    if (reply == NULL || reply->len == 0) {
        ESP_LOGE(TAG, "service returned %d with an empty body", status);
        return;
    }
    /* Truncation is reported rather than hidden: a clipped JSON body is useless
     * for telling a compiler error from a provider error. */
    if (status == 200) {
        /* !include_hex=false keeps this under a kilobyte, so log it whole. */
        ESP_LOGI(TAG, "reply (%u bytes%s): %s", (unsigned)reply->len,
                 reply->truncated ? ", TRUNCATED" : "", reply->buf);
    } else {
        ESP_LOGE(TAG, "service returned %d (%u bytes%s): %s", status, (unsigned)reply->len,
                 reply->truncated ? ", TRUNCATED" : "", reply->buf);
    }
}

static void post_wav(const uint8_t *wav, size_t len)
{
    char url[192];
    snprintf(url, sizeof(url), "%s?include_hex=false", CONFIG_MB_VOICE_SERVICE_URL);

    char *body = heap_caps_malloc(CONFIG_MB_VOICE_REPLY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %d-byte reply buffer", CONFIG_MB_VOICE_REPLY_BYTES);
        return;
    }
    reply_t reply = {.buf = body, .capacity = CONFIG_MB_VOICE_REPLY_BYTES, .len = 0};
    body[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .event_handler = on_http_event,
        .user_data = &reply,
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
        free(body);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)wav, (int)len);

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
        if (s_event != NULL) {
            const mb_event_t failure = {.kind = MB_EVENT_FAILED,
                                        .detail = "I could not hear that"};
            s_event(&failure);
        }
    } else {
        log_reply(esp_http_client_get_status_code(client), &reply);
    }

    esp_http_client_cleanup(client);
    free(body);
}

static void on_ai_button(void *button, void *user_data)
{
    (void)user_data;

    const button_event_t event = iot_button_get_event((button_handle_t)button);
    if (event == BUTTON_PRESS_DOWN) {
        s_recording = true;
        if (s_event != NULL) {
            const mb_event_t started = {.kind = MB_EVENT_LISTENING_STARTED};
            s_event(&started);
        }
        xSemaphoreGive(s_pressed);
    } else if (event == BUTTON_PRESS_UP) {
        s_recording = false;
        if (s_event != NULL) {
            const mb_event_t stopped = {.kind = MB_EVENT_LISTENING_STOPPED};
            s_event(&stopped);
        }
    }
}

/* A capture can be the right length and still carry nothing: the I2S stream
 * runs whether or not the microphone is routed to it. Peak and RMS separate
 * "the codec delivered silence" from "the speech was not recognised", which
 * otherwise look identical once ASR answers with an empty transcript.
 *
 * Peak alone cannot say whether the signal is damaged, though: one sample from
 * the button thump reports the same 32768 as a waveform flat-topped by a
 * saturated input stage. So the count, the first position and the longest run
 * of near-full-scale samples are logged too — a thump is a couple of adjacent
 * samples at the start, saturation is a long run anywhere. */
#define CLIP_LEVEL 32000

static void log_capture_stats(const uint8_t *pcm, size_t len)
{
    const int16_t *samples = (const int16_t *)pcm;
    const size_t count = len / sizeof(int16_t);
    int32_t peak = 0;
    size_t nonzero = 0;
    int64_t sum_squares = 0;
    size_t clipped = 0;
    int32_t first_clip = -1;
    size_t run = 0;
    size_t longest_run = 0;

    for (size_t i = 0; i < count; i++) {
        const int32_t value = samples[i];
        const int32_t magnitude = value < 0 ? -value : value;
        if (magnitude > peak) {
            peak = magnitude;
        }
        if (value != 0) {
            nonzero++;
        }
        sum_squares += (int64_t)value * value;
        if (magnitude >= CLIP_LEVEL) {
            if (first_clip < 0) {
                first_clip = (int32_t)i;
            }
            clipped++;
            run++;
            if (run > longest_run) {
                longest_run = run;
            }
        } else {
            run = 0;
        }
    }

    const double rms = count > 0 ? sqrt((double)sum_squares / (double)count) : 0.0;
    ESP_LOGI(TAG,
             "capture: %u samples (%.2f s), peak %ld, rms %.1f, nonzero %.1f%%, "
             "clipped %u (%.2f%%) first %d run %u, head %d %d %d %d",
             (unsigned)count, (double)count / (double)SAMPLE_RATE, (long)peak, rms,
             count > 0 ? 100.0 * (double)nonzero / (double)count : 0.0,
             (unsigned)clipped, count > 0 ? 100.0 * (double)clipped / (double)count : 0.0,
             (int)first_clip, (unsigned)longest_run,
             count > 0 ? samples[0] : 0, count > 1 ? samples[1] : 0,
             count > 2 ? samples[2] : 0, count > 3 ? samples[3] : 0);
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
        log_capture_stats(s_wav + WAV_HEADER_BYTES, s_pcm_len);
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

esp_err_t mb_voice_start(mb_event_fn status)
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
    err = esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set the microphone gain: %s", esp_err_to_name(err));
    } else {
        /* The bench log has to state the gain: a capture's peak only means
         * something beside the gain that produced it. */
        ESP_LOGI(TAG, "microphone: %d Hz mono %d-bit, %.0f dB input gain",
                 SAMPLE_RATE, BYTES_PER_SAMPLE * 8, (double)MIC_GAIN_DB);
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
