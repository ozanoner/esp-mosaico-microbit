// Preflight: the studio controller's contract, exercised on the host.
//
// Every case encodes a bug or a spec point that actually reached the panel, so
// this file is the guard against repeating them (issues #7, #18).
//
// The spec it enforces (agreed one state at a time):
//   IDLE -> LISTENING -> BUILDING -> CONNECTING -> FLASHING -> VERIFYING -> DONE
//   FAILED reachable from LISTENING..VERIFYING; DONE/FAILED dwell back to IDLE.
//   The bear is on every screen. A state accepts ONLY its own next event.

#include <stdio.h>
#include <string.h>

#include "app_studio.h"
#include "bundle_gsp.h"
#include "stubs.h"

static int g_failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void ev(app_studio_handle_t h, mb_event_kind_t kind, const char *detail,
               uint16_t done, uint16_t total)
{
    const mb_event_t e = {.kind = kind, .detail = detail,
                          .progress_done = done, .progress_total = total};
    app_studio_apply(h, &e);
}

static void fresh(app_studio_handle_t *h)
{
    if (*h != NULL) {
        app_studio_destroy(h);
    }
    timers_reset();
    panel_reset();
    app_studio_create((esp_gsp_handle_t)1, h);
}

/* boot paints IDLE and nothing else */
static void test_boot(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    CHECK(strcmp(panel.state, "IDLE") == 0, "tag=%s", panel.state);
    CHECK(strcmp(panel.line1, "") == 0, "line1=%s", panel.line1);
    CHECK(strcmp(panel.line2, "hold and speak") == 0, "line2=%s", panel.line2);
    CHECK(strcmp(panel.hint, "") == 0, "hint=%s", panel.hint);
    CHECK(panel.bear, "bear hidden");
    CHECK(!panel.success, "success shown at boot");
    CHECK(paint_count() == 1, "paints=%d", paint_count());
    app_studio_destroy(&s);
}

/* press -> LISTENING, and while held NOTHING else may change the screen */
static void test_listening_holds(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    CHECK(strcmp(panel.state, "LISTENING") == 0, "tag=%s", panel.state);
    CHECK(strcmp(panel.line2, "listening\u2026") == 0, "line2=%s", panel.line2);
    const int paints = paint_count();
    /* A stale image/flash/failure from a previous cycle must not disturb it. */
    ev(s, MB_EVENT_IMAGE_READY, NULL, 0, 0);
    ev(s, MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0);
    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 10, 96);
    ev(s, MB_EVENT_DONE, NULL, 0, 0);
    ev(s, MB_EVENT_FAILED, "the micro:bit went quiet", 0, 0);
    CHECK(strcmp(panel.state, "LISTENING") == 0, "disturbed tag=%s", panel.state);
    CHECK(strcmp(panel.line2, "listening\u2026") == 0, "disturbed line2=%s", panel.line2);
    CHECK(paint_count() == paints, "paints grew %d->%d", paints, paint_count());
    app_studio_destroy(&s);
}

/* release -> BUILDING */
static void test_release_builds(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    CHECK(strcmp(panel.state, "BUILDING") == 0, "tag=%s", panel.state);
    CHECK(strcmp(panel.line1, "building your") == 0, "line1=%s", panel.line1);
    CHECK(strcmp(panel.line2, "program\u2026") == 0, "line2=%s", panel.line2);
    /* BUILD_STARTED arriving while already BUILDING is ignored */
    const int paints = paint_count();
    ev(s, MB_EVENT_BUILD_STARTED, NULL, 0, 0);
    CHECK(paint_count() == paints, "repaint on redundant BUILD_STARTED");
    app_studio_destroy(&s);
}

/* image ready -> CONNECTING with the small hint */
static void test_connecting(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    ev(s, MB_EVENT_IMAGE_READY, NULL, 0, 0);
    CHECK(strcmp(panel.state, "CONNECTING") == 0, "tag=%s", panel.state);
    CHECK(strcmp(panel.line1, "waiting for") == 0, "line1=%s", panel.line1);
    CHECK(strcmp(panel.line2, "micro:bit\u2026") == 0, "line2=%s", panel.line2);
    CHECK(strcmp(panel.hint, "(A+B+Reset)") == 0, "hint=%s", panel.hint);
    app_studio_destroy(&s);
}

/* the five-segment bar: 0/96, 48/96, 96/96 */
static void test_flash_progress(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    ev(s, MB_EVENT_IMAGE_READY, NULL, 0, 0);

    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 0, 96);
    CHECK(strcmp(panel.state, "flashing") == 0, "tag=%s", panel.state);
    CHECK(panel.track[0] && panel.track[4], "tracks hidden");
    CHECK(panel.seg[0] && !panel.seg[1] && !panel.seg[4], "seg0=%d seg1=%d seg4=%d",
          panel.seg[0], panel.seg[1], panel.seg[4]);

    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 48, 96);
    CHECK(panel.seg[0] && panel.seg[1], "seg0/1 not solid");
    CHECK(panel.seg[2], "active seg not shown");

    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 96, 96);
    CHECK(panel.seg[0] && panel.seg[1] && panel.seg[2] && panel.seg[3] && panel.seg[4],
          "not all solid at 96/96");
    CHECK(live_timers(500) == 0, "blink still running at completion");
    app_studio_destroy(&s);
}

/* verifying restarts the bar from empty */
static void test_verifying_restarts(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    ev(s, MB_EVENT_IMAGE_READY, NULL, 0, 0);
    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 96, 96);
    ev(s, MB_EVENT_VERIFYING, NULL, 0, 0);
    CHECK(strcmp(panel.state, "verifying") == 0, "tag=%s", panel.state);
    CHECK(panel.track[0] && panel.track[4], "tracks hidden");
    CHECK(!panel.seg[0] && !panel.seg[4], "bar did not restart empty");
    app_studio_destroy(&s);
}

/* DONE: trophy, no message, bear still there; stale events ignored */
static void test_done(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    ev(s, MB_EVENT_IMAGE_READY, NULL, 0, 0);
    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 96, 96);
    ev(s, MB_EVENT_VERIFYING, NULL, 0, 0);
    ev(s, MB_EVENT_DONE, NULL, 0, 0);
    CHECK(strcmp(panel.state, "you made it!") == 0, "tag=%s", panel.state);
    CHECK(panel.success, "success not shown");
    CHECK(panel.bear, "bear hidden on DONE");
    CHECK(strcmp(panel.line1, "") == 0 && strcmp(panel.line2, "") == 0, "message shown on DONE");
    const int paints = paint_count();
    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 10, 96);
    ev(s, MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0);
    CHECK(paint_count() == paints, "stale event repainted DONE");
    CHECK(panel.success, "stale event hid success");
    app_studio_destroy(&s);
}

/* FAILED: two-line reason, bear still there; stale ignored; dwell -> IDLE */
static void test_failed(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    ev(s, MB_EVENT_FAILED, "the micro:bit stopped answering", 0, 0);
    CHECK(strcmp(panel.state, "FAILED") == 0, "tag=%s", panel.state);
    CHECK(strcmp(panel.line1, "the micro:bit") == 0, "line1=%s", panel.line1);
    CHECK(strcmp(panel.line2, "went quiet") == 0, "line2=%s", panel.line2);
    CHECK(panel.bear, "bear hidden on FAILED");
    const int paints = paint_count();
    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 10, 96);
    ev(s, MB_EVENT_DONE, NULL, 0, 0);
    CHECK(paint_count() == paints, "stale event repainted FAILED");
    CHECK(fire_timer(8000) == 1, "no FAILED dwell timer");
    CHECK(strcmp(panel.state, "IDLE") == 0, "dwell did not return to IDLE: %s", panel.state);
    app_studio_destroy(&s);
}

/* a press after DONE starts a fresh cycle */
static void test_restart_from_done(void)
{
    app_studio_handle_t s = NULL;
    fresh(&s);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    ev(s, MB_EVENT_IMAGE_READY, NULL, 0, 0);
    ev(s, MB_EVENT_FLASH_PROGRESS, NULL, 96, 96);
    ev(s, MB_EVENT_VERIFYING, NULL, 0, 0);
    ev(s, MB_EVENT_DONE, NULL, 0, 0);
    ev(s, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    CHECK(strcmp(panel.state, "LISTENING") == 0, "tag=%s", panel.state);
    CHECK(!panel.success, "success still shown after restart");
    app_studio_destroy(&s);
}

int main(void)
{
    test_boot();
    test_listening_holds();
    test_release_builds();
    test_connecting();
    test_flash_progress();
    test_verifying_restarts();
    test_done();
    test_failed();
    test_restart_from_done();
    if (g_failures == 0) {
        printf("state machine: all cases passed\n");
        return 0;
    }
    printf("state machine: %d failure(s)\n", g_failures);
    return 1;
}
