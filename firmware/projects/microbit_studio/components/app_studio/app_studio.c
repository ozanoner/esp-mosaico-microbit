// SPDX-License-Identifier: Apache-2.0

#include "app_studio.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bundle_gsp.h"

/* Bisect: disable the 600 ms bear bob to isolate the INT WDT (issue #18). */
#define STUDIO_BOB_ENABLED 1

/* Terminal dwells and the pulsing segment cadence. */
#define FAILED_DWELL_MS 8000
#define DONE_DWELL_MS 10000
#define SEG_BLINK_MS 500

/* Bear bob: the scene declares bear.y dynamic over 186..194. */
#define BEAR_BOB_MS 600
#define BEAR_Y_HOME 190
#define BEAR_Y_HIGH 187
#define BEAR_Y_LOW 193

#define SEGMENTS 5

/* One row per screen: the small tag, the two message lines, the small hint under
 * the message, and which extras are on. The tag is what a child reads as the
 * headline of the moment, not necessarily the state's engineering name. */
typedef struct {
    const char *tag;
    const char *line1;
    const char *line2;
    const char *hint;
    bool progress;
    bool success;
} studio_screen_t;

/* The bear is on every screen: it is the mascot, not a progress cue. */
static const studio_screen_t screens[APP_STUDIO_STATE_COUNT] = {
    [APP_STUDIO_IDLE] = {"IDLE", "", "hold and speak", "", false, false},
    [APP_STUDIO_LISTENING] = {"LISTENING", "", "listening\u2026", "", false, false},
    [APP_STUDIO_BUILDING] = {"BUILDING", "building your", "program\u2026", "", false, false},
    [APP_STUDIO_AWAIT_MICROBIT] = {"CONNECTING", "waiting for", "micro:bit\u2026",
                                   "(A+B+Reset)", false, false},
    [APP_STUDIO_FLASHING] = {"flashing", "", "", "", true, false},
    [APP_STUDIO_VERIFYING] = {"verifying", "", "", "", true, false},
    [APP_STUDIO_DONE] = {"you made it!", "", "", "", false, true},
    [APP_STUDIO_FAILED] = {"FAILED", "", "", "", false, false},
};

/* Failure sentences are authored at 40 px, split where they read as a phrase
 * inside a 288 px column. Unknown sentences land on one line and clip. */
typedef struct {
    const char *sentence;
    const char *line1;
    const char *line2;
} failure_line_t;

static const failure_line_t failure_lines[] = {
    {"the build box is not answering", "the build box is", "not answering"},
    {"the micro:bit stopped answering", "the micro:bit", "went quiet"},
    {"the micro:bit could not be reached", "the micro:bit", "never answered"},
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

/* Segment i is solid once fully done, i == active_seg blinks while in flight,
 * everything beyond is empty. Tracks show on both the write and the verify pass. */
static void paint_segments(struct app_studio *studio)
{
    const bool tracks = studio->state == APP_STUDIO_FLASHING ||
                        studio->state == APP_STUDIO_VERIFYING;
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
#if STUDIO_BOB_ENABLED
    remember_error(studio, gsp_studio_bear_set_y(ui, studio->bear_y));
#else
    (void)ui;
#endif
}

/* The message block is centred in the mid section (y 96..396, centre 246): two
 * lines sit at 194/246, a single line at 220 so its own centre is 246. */
static void centre_message(struct app_studio *studio, bool two_lines)
{
    remember_error(studio, gsp_studio_line1_set_y(studio->ui, two_lines ? 194 : 220));
    remember_error(studio, gsp_studio_line2_set_y(studio->ui, 246));
}

static void show_screen(struct app_studio *studio, app_studio_state_t state)
{
    const studio_screen_t *screen = &screens[state];
    studio->state = state;  /* paint_segments reads it for the track row */
    remember_error(studio, gsp_studio_state_set_text(studio->ui, screen->tag));
    remember_error(studio, gsp_studio_line1_set_text(studio->ui, screen->line1));
    remember_error(studio, gsp_studio_line2_set_text(studio->ui, screen->line2));
    remember_error(studio, gsp_studio_hint_set_text(studio->ui, screen->hint));
    centre_message(studio, screen->line2[0] != '\0');
    remember_error(studio, gsp_studio_bear_set_visible(studio->ui, true));
    remember_error(studio, gsp_studio_success_set_visible(studio->ui, screen->success));
    stop_timer(studio, &studio->bob_timer);
    studio->bob_up = false;
    studio->bear_y = BEAR_Y_HOME;
    remember_error(studio, gsp_studio_bear_set_y(studio->ui, studio->bear_y));
    studio->bob_timer = esp_gsp_timer_create(studio->ui, BEAR_BOB_MS, bear_bob, studio);
    if (studio->bob_timer == NULL) {
        remember_error(studio, ESP_GSP_ERR_NO_MEM);
    }
    if (!screen->progress) {
        stop_timer(studio, &studio->blink_timer);
    }
    /* A fresh screen always starts with an empty bar; the VERIFYING pass must
     * restart from zero, not inherit the FLASHING fill. */
    studio->active_seg = -1;
    studio->solid_segments = 0;
    paint_segments(studio);
}

static void return_to_idle(esp_gsp_handle_t ui, void *ctx)
{
    struct app_studio *studio = ctx;
    stop_timer(studio, &studio->dwell_timer);
    show_screen(studio, APP_STUDIO_IDLE);
    (void)ui;
}

static bool is_terminal(app_studio_state_t state)
{
    return state == APP_STUDIO_DONE || state == APP_STUDIO_FAILED;
}

/* The strict transition table. A state accepts only the event that moves it to
 * the next step; everything else is dropped. This is what keeps a screen's
 * message from being overwritten by a stale report from a previous cycle. */
static bool accepts(app_studio_state_t state, mb_event_kind_t kind)
{
    switch (kind) {
    case MB_EVENT_LISTENING_STARTED:
        return state == APP_STUDIO_IDLE || state == APP_STUDIO_DONE ||
               state == APP_STUDIO_FAILED;
    case MB_EVENT_LISTENING_STOPPED:
    case MB_EVENT_BUILD_STARTED:
        return state == APP_STUDIO_LISTENING;
    case MB_EVENT_IMAGE_READY:
    case MB_EVENT_AWAIT_MICROBIT:
        return state == APP_STUDIO_BUILDING || state == APP_STUDIO_AWAIT_MICROBIT;
    case MB_EVENT_FLASH_PROGRESS:
        return state == APP_STUDIO_AWAIT_MICROBIT || state == APP_STUDIO_FLASHING ||
               state == APP_STUDIO_VERIFYING;
    case MB_EVENT_VERIFYING:
        return state == APP_STUDIO_FLASHING;
    case MB_EVENT_DONE:
        return state == APP_STUDIO_FLASHING || state == APP_STUDIO_VERIFYING;
    case MB_EVENT_FAILED:
        /* Not from LISTENING: while the button is held nothing is in flight, so
         * a stale failure from a previous cycle must not overwrite the screen. */
        return state == APP_STUDIO_BUILDING || state == APP_STUDIO_AWAIT_MICROBIT ||
               state == APP_STUDIO_FLASHING || state == APP_STUDIO_VERIFYING;
    default:
        return false;
    }
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
        /* an unknown sentence is one unbroken line: centre it alone */
        centre_message(studio, lines != NULL && lines->line2[0] != '\0');
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

    if (!accepts(studio->state, event->kind)) {
        return ESP_GSP_OK;  /* drop anything that is not this state's own next step */
    }
    switch (event->kind) {
    case MB_EVENT_LISTENING_STARTED:
        go_to(studio, APP_STUDIO_LISTENING, NULL);
        break;
    case MB_EVENT_LISTENING_STOPPED:
    case MB_EVENT_BUILD_STARTED:
        go_to(studio, APP_STUDIO_BUILDING, NULL);
        break;
    case MB_EVENT_IMAGE_READY:
    case MB_EVENT_AWAIT_MICROBIT:
        go_to(studio, APP_STUDIO_AWAIT_MICROBIT, NULL);
        /* Diagnostic text from the scan (e.g. "seen 20") replaces the hint. */
        if (event->detail != NULL && event->detail[0] != '\0') {
            remember_error(studio, gsp_studio_hint_set_text(studio->ui, event->detail));
        }
        break;
    case MB_EVENT_FLASH_PROGRESS: {
        if (studio->state != APP_STUDIO_FLASHING && studio->state != APP_STUDIO_VERIFYING) {
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
    stop_timer(studio, &studio->dwell_timer);
    stop_timer(studio, &studio->bob_timer);
    stop_timer(studio, &studio->blink_timer);
    const esp_gsp_err_t err = studio->last_error;
    free(studio);
    *handle = NULL;
    return err;
}
