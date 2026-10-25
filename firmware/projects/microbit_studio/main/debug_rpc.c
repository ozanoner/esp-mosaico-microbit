// Debug RPC: drive and inspect the studio pipeline from the host.
//
// Service 0x4D42, methods:
//   1  press    - simulate the AI button going down
//   2  release  - simulate release (sends the request)
//   3  status   - "state=<NAME> bear_y=<n> err=<n>"
//   4  event    - payload is a numeric mb_event_kind_t, injected verbatim
//
// Registered at startup so `mosaico.py iris rpc 19778 <method>` works.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_iris.h"
#include "app_studio.h"
#include "mb_event.h"

#define DBG_SERVICE 0x4D42

static app_studio_handle_t s_studio;
static void (*s_sink)(const mb_event_t *);

static esp_err_t reply(uint8_t *resp, size_t cap, size_t *n, const char *text)
{
    const size_t len = strlen(text) < cap ? strlen(text) : cap;
    memcpy(resp, text, len);
    *n = len;
    return ESP_OK;
}

static esp_err_t handler(const esp_iris_rpc_request_t *rq, uint8_t *resp,
                         size_t cap, size_t *n, void *ctx)
{
    (void)ctx;
    mb_event_t ev = {0};

    switch (rq->method_id) {
    case 1:
        ev.kind = MB_EVENT_LISTENING_STARTED;
        break;
    case 2:
        ev.kind = MB_EVENT_LISTENING_STOPPED;
        break;
    case 4: {
        char buf[16] = {0};
        const size_t len = rq->payload_size < sizeof(buf) - 1 ? rq->payload_size : sizeof(buf) - 1;
        memcpy(buf, rq->payload, len);
        ev.kind = (mb_event_kind_t)atoi(buf);
        break;
    }
    case 3: {
        app_studio_snapshot_t snap = {0};
        if (s_studio != NULL) {
            app_studio_read(s_studio, &snap);
        }
        char out[96];
        snprintf(out, sizeof(out), "state=%s bear_y=%ld err=%d",
                 app_studio_state_name(snap.state), (long)snap.bear_y,
                 (int)snap.last_error);
        return reply(resp, cap, n, out);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_sink == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sink(&ev);
    return reply(resp, cap, n, "ok");
}

esp_err_t mb_debug_start(app_studio_handle_t studio, void (*sink)(const mb_event_t *))
{
    s_studio = studio;
    s_sink = sink;
    esp_err_t err = ESP_OK;
    for (uint16_t m = 1; m <= 4; ++m) {
        const esp_err_t one = esp_iris_rpc_register(DBG_SERVICE, m, handler, NULL);
        if (one != ESP_OK) {
            err = one;
        }
    }
    return err;
}
