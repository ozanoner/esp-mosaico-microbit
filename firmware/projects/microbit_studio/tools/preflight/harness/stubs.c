// Recording stubs for the preflight harness: every setter the controller calls
// lands in `panel`, so tests can assert on exactly what the screen would show.
#include "bundle_gsp.h"
#include "stubs.h"

#include <string.h>

panel_t panel;
static int g_paint_count;

typedef struct {
    esp_gsp_timer_fn fn;
    void *ctx;
    uint32_t ms;
    bool alive;
} timer_slot_t;

static timer_slot_t g_timers[8];

void panel_reset(void)
{
    memset(&panel, 0, sizeof(panel));
    g_paint_count = 0;
}

void timers_reset(void) { memset(g_timers, 0, sizeof(g_timers)); }

int paint_count(void) { return g_paint_count; }

static void copy(char *dst, size_t n, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

esp_gsp_err_t gsp_studio_state_set_text(esp_gsp_handle_t gsp, const char *text)
{
    (void)gsp;
    copy(panel.state, sizeof(panel.state), text);
    g_paint_count++;
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_line1_set_text(esp_gsp_handle_t gsp, const char *text)
{
    (void)gsp;
    copy(panel.line1, sizeof(panel.line1), text);
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_line2_set_text(esp_gsp_handle_t gsp, const char *text)
{
    (void)gsp;
    copy(panel.line2, sizeof(panel.line2), text);
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_line1_set_y(esp_gsp_handle_t gsp, int32_t y)
{
    (void)gsp;
    panel.line1_y = y;
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_line2_set_y(esp_gsp_handle_t gsp, int32_t y)
{
    (void)gsp;
    panel.line2_y = y;
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_bear_set_visible(esp_gsp_handle_t gsp, bool visible)
{
    (void)gsp;
    panel.bear = visible;
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_bear_set_y(esp_gsp_handle_t gsp, int32_t y)
{
    (void)gsp;
    panel.bear_y = y;
    return ESP_GSP_OK;
}

esp_gsp_err_t gsp_studio_success_set_visible(esp_gsp_handle_t gsp, bool visible)
{
    (void)gsp;
    panel.success = visible;
    return ESP_GSP_OK;
}

#define SEG_FN(n)                                                              \
    esp_gsp_err_t gsp_studio_seg##n##_set_visible(esp_gsp_handle_t gsp,        \
                                                  bool visible)                \
    {                                                                          \
        (void)gsp;                                                             \
        panel.seg[n - 1] = visible;                                            \
        return ESP_GSP_OK;                                                     \
    }

#define TRACK_FN(n)                                                            \
    esp_gsp_err_t gsp_studio_track##n##_set_visible(esp_gsp_handle_t gsp,      \
                                                    bool visible)              \
    {                                                                          \
        (void)gsp;                                                             \
        panel.track[n - 1] = visible;                                          \
        return ESP_GSP_OK;                                                     \
    }

SEG_FN(1) SEG_FN(2) SEG_FN(3) SEG_FN(4) SEG_FN(5)
TRACK_FN(1) TRACK_FN(2) TRACK_FN(3) TRACK_FN(4) TRACK_FN(5)

void *esp_gsp_timer_create(esp_gsp_handle_t gsp, uint32_t ms, esp_gsp_timer_fn fn,
                           void *ctx)
{
    (void)gsp;
    for (int i = 0; i < 8; i++) {
        if (!g_timers[i].alive) {
            g_timers[i] = (timer_slot_t){.fn = fn, .ctx = ctx, .ms = ms, .alive = true};
            return &g_timers[i];
        }
    }
    return NULL;
}

esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t gsp, void *timer)
{
    (void)gsp;
    if (timer != NULL) {
        ((timer_slot_t *)timer)->alive = false;
    }
    return ESP_GSP_OK;
}

int live_timers(uint32_t ms)
{
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (g_timers[i].alive && (ms == 0 || g_timers[i].ms == ms)) {
            n++;
        }
    }
    return n;
}

int fire_timer(uint32_t ms)
{
    for (int i = 0; i < 8; i++) {
        if (g_timers[i].alive && (ms == 0 || g_timers[i].ms == ms)) {
            esp_gsp_timer_fn fn = g_timers[i].fn;
            void *ctx = g_timers[i].ctx;
            g_timers[i].alive = false; /* one-shot semantics */
            fn(NULL, ctx);
            return 1;
        }
    }
    return 0;
}
