// SPDX-License-Identifier: Apache-2.0

#include "app_studio.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bundle_gsp.h"

/* Agreed in issue #7: the ritual prompt appears only when a board never
 * answered, the two terminal screens hold long enough to read, and the segment
 * being downloaded pulses at 500 ms. */
#define RITUAL_AFTER_MS 15000
#define FAILED_DWELL_MS 8000
#define DONE_DWELL_MS 10000
#define SEG_BLINK_MS 500

/* Two lines: at 40 px the sentence is 433 px and the column gives 288. */
#define RITUAL_LINE1 "hold A+B, then"
#define RITUAL_LINE2 "tap reset"

/* Bear bob: the scene declares bear.y dynamic over 186..194. */
#define BEAR_BOB_MS 600
#define BEAR_Y_HOME 190
#define BEAR_Y_HIGH 187
#define BEAR_Y_LOW 193

#define SEGMENTS 5

/* One row per screen: what the app column says and which extras are on. */
typedef struct {
    const char *headline;
    const char *line1;
    const char *line2;
    bool bear;
    bool progress;
    bool success;
} studio_screen_t;

static const studio_screen_t screens[APP_STUDIO_STATE_COUNT] = {
    [APP_STUDIO_IDLE] = {"IDLE", "", "hold and speak", true, false, false},
    [APP_STUDIO_LISTENING] = {"LISTENING", "", "listening\u2026", true, false, false},
    [APP_STUDIO_BUILDING] = {"BUILDING", "building your", "program\u2026", true, false, false},
    /* CONNECTING, not AWAIT_MICROBIT: at 40 px that name is 336 px and the column
     * gives 288, and "PAIRING" would be wrong because gate G2 measured mode=1. */
    [APP_STUDIO_AWAIT_MICROBIT] = {"CONNECTING", "waiting for", "micro:bit\u2026", true, false, false},
    [APP_STUDIO_FLASHING] = {"FLASHING", "", "", true, true, false},
    [APP_STUDIO_VERIFYING] = {"VERIFYING", "checking your", "program\u2026", true, false, false},
    /* No success text: the still frame is the whole message. */
    [APP_STUDIO_DONE] = {"", "", "", false, false, true},
    [APP_STUDIO_FAILED] = {"FAILED", "", "", false, false, false},
};

/* Failure sentences are authored at 40 px, so each is split where it reads as a
 * phrase inside a 288 px column. Unknown sentences land on one line and clip. */
typedef struct {
    const char *sentence;
    const char *line1;
    const char *line2;
} failure_line_t;

static const failure_line_t failure_lines[] = {
    {"the build box is not answering", "the build box is", "not answering"},
    {"the micro:bit stopped answering", "the micro:bit", "went quiet"},
    {"the micro:bit went quiet", "the micro:bit", "went quiet"},
    {"the program did not arrive intact", "the program", "did not arrive"},
    {"this micro:bit needs a fresh start", "micro:bit needs", "a restart"},
    {"I could not hear that", "I could not", "hear that"},
    {"I cannot reach the build box", "I cannot reach", "the build box"},
};

static const failure_line_t *find_failure(const char *sentence)
{
    for (size_t i = 0; i < sizeof(failure_lines) / sizeof(failure_lines[0]); ++i) {
        if (strcmp(failure_lines[i].sentence, sentence) == 0) {
            return &failure_lines[i];
        }
    }
    return NULL;
}

static const char *const state_names[APP_STUDIO_STATE_COUNT] = {
    "IDLE", "LISTENING", "BUILDING", "AWAIT_MICROBIT",
    "FLASHING", "VERIFYING", "DONE", "FAILED",
};

static esp_gsp_err_t track_set_visible(int index, esp_gsp_handle_t ui, bool visible)
{
    switch (index) {
    case 0: return gsp_studio_track1_set_visible(ui, visible);
    case 1: return gsp_studio_track2_set_visible(ui, visible);
    case 2: return gsp_studio_track3_set_visible(ui, visible);
    case 3: return gsp_studio_track4_set_visible(ui, visible);
    case 4: return gsp_studio_track5_set_visible(ui, visible);
    default: return ESP_GSP_ERR_INVALID_ARG;
    }
}

static esp_gsp_err_t seg_set_visible(int index, esp_gsp_handle_t ui, bool visible)
{
    switch (index) {
    case 0: return gsp_studio_seg1_set_visible(ui, visible);
    case 1: return gsp_studio_seg2_set_visible(ui, visible);
    case 2: return gsp_studio_seg3_set_visible(ui, visible);
    case 3: return gsp_studio_seg4_set_visible(ui, visible);
    case 4: return gsp_studio_seg5_set_visible(ui, visible);
    default: return ESP_GSP_ERR_INVALID_ARG;
    }
}

struct app_studio {
    esp_gsp_handle_t ui;
    app_studio_state_t state;
    void *dwell_timer;
    void *ritual_timer;
    void *bob_timer;
    void *blink_timer;
    bool bob_up;
    bool blink_on;
    int active_seg;
    int solid_segments;
    int32_t bear_y;
    esp_gsp_err_t last_error;
};

static void remember_error(struct app_studio *studio, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        studio->last_error = err;
    }
}

static void stop_timer(struct app_studio *studio, void **timer)
{
    if (*timer != NULL) {
        remember_error(studio, esp_gsp_timer_delete(studio->ui, *timer));
        *timer = NULL;
    }
}

/* Segment i is solid once fully downloaded, i == solid_segments blinks while it
 * is in flight, everything beyond is empty. */
static void paint_segments(struct app_studio *studio)
{
    const bool tracks = studio->state == APP_STUDIO_FLASHING;
    for (int i = 0; i < SEGMENTS; ++i) {
        remember_error(studio, track_set_visible(i, studio->ui, tracks));
    }
    for (int i = 0; i < SEGMENTS; ++i) {
        const bool visible = i < studio->solid_segments ||
                             (i == studio->active_seg && studio->blink_on);
        remember_error(studio, seg_set_visible(i, studio->ui, visible));
    }
}

static void blink_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_studio *studio = ctx;
    if (studio->active_seg < 0) {
        return;
    }
    studio->blink_on = !studio->blink_on;
    remember_error(studio, seg_set_visible(studio->active_seg, ui, studio->blink_on));
}

static void start_blink(struct app_studio *studio)
{
    if (studio->blink_timer != NULL || studio->active_seg < 0) {
        return;
    }
    studio->blink_on = true;
    studio->blink_timer = esp_gsp_timer_create(studio->ui, SEG_BLINK_MS, blink_tick, studio);
    if (studio->blink_timer == NULL) {
        remember_error(studio, ESP_GSP_ERR_NO_MEM);
    }
}

static void bear_bob(esp_gsp_handle_t ui, void *ctx)
{
    struct app_studio *studio = ctx;
    studio->bob_up = !studio->bob_up;
    studio->bear_y = studio->bob_up ? BEAR_Y_HIGH : BEAR_Y_LOW;
    remember_error(studio, gsp_studio_bear_set_y(ui, studio->bear_y));
}

static void show_screen(struct app_studio *studio, app_studio_state_t state)
{
    const studio_screen_t *screen = &screens[state];
    studio->state = state;  /* paint_segments reads it for the track row */
    remember_error(studio, gsp_studio_state_set_text(studio->ui, screen->headline));
    remember_error(studio, gsp_studio_line1_set_text(studio->ui, screen->line1));
    remember_error(studio, gsp_studio_line2_set_text(studio->ui, screen->line2));
    remember_error(studio, gsp_studio_bear_set_visible(studio->ui, screen->bear));
    remember_error(studio, gsp_studio_success_set_visible(studio->ui, screen->success));
    stop_timer(studio, &studio->bob_timer);
    if (screen->bear) {
        studio->bob_up = false;
        studio->bear_y = BEAR_Y_HOME;
        remember_error(studio, gsp_studio_bear_set_y(studio->ui, studio->bear_y));
        studio->bob_timer = esp_gsp_timer_create(studio->ui, BEAR_BOB_MS, bear_bob, studio);
        if (studio->bob_timer == NULL) {
            remember_error(studio, ESP_GSP_ERR_NO_MEM);
        }
    }
    if (!screen->progress) {
        stop_timer(studio, &studio->blink_timer);
        studio->active_seg = -1;
        studio->solid_segments = 0;
        paint_segments(studio);
    }
}

static void return_to_idle(esp_gsp_handle_t ui, void *ctx)
{
    struct app_studio *studio = ctx;
    stop_timer(studio, &studio->dwell_timer);
    show_screen(studio, APP_STUDIO_IDLE);
    (void)ui;
}

static void show_ritual(esp_gsp_handle_t ui, void *ctx)
{
    struct app_studio *studio = ctx;
    stop_timer(studio, &studio->ritual_timer);
    if (studio->state != APP_STUDIO_AWAIT_MICROBIT) {
        return;
    }
    remember_error(studio, gsp_studio_line1_set_text(ui, RITUAL_LINE1));
    remember_error(studio, gsp_studio_line2_set_text(ui, RITUAL_LINE2));
}

static bool is_terminal(app_studio_state_t state)
{
    return state == APP_STUDIO_DONE || state == APP_STUDIO_FAILED;
}

/* A cycle is on screen only between the button press and its verdict. Outside
 * that window the panel owns the state: the BLE scanner keeps running after a
 * failure, so late AWAIT_MICROBIT / FLASH_PROGRESS reports would otherwise pull
 * DONE, FAILED or the IDLE after them back into CONNECTING. */
static bool cycle_on_screen(app_studio_state_t state)
{
    return state == APP_STUDIO_LISTENING || state == APP_STUDIO_BUILDING ||
           state == APP_STUDIO_AWAIT_MICROBIT || state == APP_STUDIO_FLASHING ||
           state == APP_STUDIO_VERIFYING;
}

esp_gsp_err_t app_studio_create(esp_gsp_handle_t ui, app_studio_handle_t *out_handle)
{
    if (out_handle == NULL || ui == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    struct app_studio *studio = calloc(1, sizeof(*studio));
    if (studio == NULL) {
        return ESP_GSP_ERR_NO_MEM;
    }
    studio->ui = ui;
    studio->bear_y = BEAR_Y_HOME;
    studio->active_seg = -1;
    show_screen(studio, APP_STUDIO_IDLE);
    *out_handle = studio;
    return ESP_GSP_OK;
}

static void go_to(struct app_studio *studio, app_studio_state_t state,
                  const char *failure)
{
    stop_timer(studio, &studio->ritual_timer);
    stop_timer(studio, &studio->dwell_timer);
    stop_timer(studio, &studio->blink_timer);
    studio->active_seg = -1;
    studio->solid_segments = 0;
    show_screen(studio, state);
    if (state == APP_STUDIO_FAILED && failure != NULL) {
        const failure_line_t *lines = find_failure(failure);
        remember_error(studio, gsp_studio_line1_set_text(
                                  studio->ui, lines != NULL ? lines->line1 : failure));
        remember_error(studio, gsp_studio_line2_set_text(
                                  studio->ui, lines != NULL ? lines->line2 : ""));
    }
    if (state == APP_STUDIO_AWAIT_MICROBIT) {
        studio->ritual_timer = esp_gsp_timer_create(studio->ui, RITUAL_AFTER_MS,
                                                    show_ritual, studio);
        if (studio->ritual_timer == NULL) {
            remember_error(studio, ESP_GSP_ERR_NO_MEM);
        }
    }
    if (is_terminal(state)) {
        const uint32_t dwell = state == APP_STUDIO_DONE ? DONE_DWELL_MS : FAILED_DWELL_MS;
        studio->dwell_timer = esp_gsp_timer_create(studio->ui, dwell, return_to_idle, studio);
        if (studio->dwell_timer == NULL) {
            remember_error(studio, ESP_GSP_ERR_NO_MEM);
        }
    }
}

esp_gsp_err_t app_studio_apply(app_studio_handle_t handle, const mb_event_t *event)
{
    if (handle == NULL || event == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_studio *studio = handle;

    /* Only a new button press may interrupt a finished screen; everything else
     * has to belong to the cycle currently on screen. */
    if (event->kind != MB_EVENT_LISTENING_STARTED && !cycle_on_screen(studio->state)) {
        return ESP_GSP_OK;
    }
    switch (event->kind) {
    case MB_EVENT_LISTENING_STARTED:
        go_to(studio, APP_STUDIO_LISTENING, NULL);
        break;
    case MB_EVENT_LISTENING_STOPPED:
        /* Stay on LISTENING: BUILDING is reported by voice when the request is
         * actually on the wire, not when the finger comes off the button. */
        break;
    case MB_EVENT_BUILD_STARTED:
        go_to(studio, APP_STUDIO_BUILDING, NULL);
        break;
    case MB_EVENT_IMAGE_READY:
    case MB_EVENT_AWAIT_MICROBIT:
        go_to(studio, APP_STUDIO_AWAIT_MICROBIT, NULL);
        break;
    case MB_EVENT_FLASH_PROGRESS: {
        if (studio->state != APP_STUDIO_FLASHING) {
            go_to(studio, APP_STUDIO_FLASHING, NULL);
        }
        const uint32_t total = event->progress_total != 0 ? event->progress_total : 1u;
        const uint32_t done = event->progress_done;
        uint32_t solid = (done * SEGMENTS) / total;
        if (solid > SEGMENTS) {
            solid = SEGMENTS;
        }
        studio->solid_segments = (int)solid;
        studio->active_seg = solid < SEGMENTS ? (int)solid : -1;
        studio->blink_on = true;
        paint_segments(studio);
        start_blink(studio);
        if (studio->active_seg < 0) {
            stop_timer(studio, &studio->blink_timer);
        }
        break;
    }
    case MB_EVENT_VERIFYING:
        go_to(studio, APP_STUDIO_VERIFYING, NULL);
        break;
    case MB_EVENT_DONE:
        go_to(studio, APP_STUDIO_DONE, NULL);
        break;
    case MB_EVENT_FAILED:
        go_to(studio, APP_STUDIO_FAILED, event->detail);
        break;
    default:
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return studio->last_error;
}

esp_gsp_err_t app_studio_read(app_studio_handle_t handle, app_studio_snapshot_t *out_snapshot)
{
    if (handle == NULL || out_snapshot == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = (app_studio_snapshot_t){
        .state = handle->state,
        .bear_y = handle->bear_y,
        .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_studio_state(app_studio_handle_t handle, app_studio_state_t *out_state)
{
    if (handle == NULL || out_state == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_state = handle->state;
    return ESP_GSP_OK;
}

const char *app_studio_state_name(app_studio_state_t state)
{
    if ((unsigned)state >= (unsigned)APP_STUDIO_STATE_COUNT) {
        return "UNKNOWN";
    }
    return state_names[state];
}

esp_gsp_err_t app_studio_destroy(app_studio_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_studio *studio = *handle;
    if (studio == NULL) {
        return ESP_GSP_OK;
    }
    stop_timer(studio, &studio->ritual_timer);
    stop_timer(studio, &studio->dwell_timer);
    stop_timer(studio, &studio->bob_timer);
    stop_timer(studio, &studio->blink_timer);
    const esp_gsp_err_t err = studio->last_error;
    free(studio);
    *handle = NULL;
    return err;
}
