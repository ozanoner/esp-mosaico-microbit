// Preflight: the studio controller's contract, exercised on the host.
//
// Every case encodes a bug that actually reached the panel once, so this file is
// the guard against repeating them (issue #7).
//
// Two rules the assertions follow, because the design is what it is:
//   - the *state* is read through app_studio_state(); the on-screen headline is
//     empty on DONE, so panel.state cannot stand in for the state.
//   - a single-line message is authored on line2, so that every block occupies
//     the same two-line frame and the centring never shifts.
#include <stdio.h>
#include <string.h>

#include "app_studio.h"
#include "stubs.h"

static int g_failures;
static const char *g_test;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %-42s %s:%d  %s\n", g_test, __FILE__, __LINE__,     \
                   #cond);                                                     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void begin(const char *name)
{
    g_test = name;
    panel_reset();
    printf("- %s\n", name);
}

static void ev(app_studio_handle_t h, mb_event_kind_t kind, const char *detail,
               uint16_t done, uint16_t total);

static void fresh(app_studio_handle_t *h)
{
    if (*h != NULL) {
        app_studio_destroy(h);
    }
    timers_reset();
    panel_reset();
    app_studio_create((esp_gsp_handle_t)0x1, h);
}

static void drive_to_done(app_studio_handle_t h)
{
    ev(h, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(h, MB_EVENT_BUILD_STARTED, NULL, 0, 0);
    ev(h, MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0);
    ev(h, MB_EVENT_FLASH_PROGRESS, NULL, 96, 96);
    ev(h, MB_EVENT_VERIFYING, NULL, 0, 0);
    ev(h, MB_EVENT_DONE, NULL, 0, 0);
}

static app_studio_state_t state_of(app_studio_handle_t h)
{
    app_studio_state_t s = APP_STUDIO_STATE_COUNT;
    app_studio_state(h, &s);
    return s;
}

static void ev(app_studio_handle_t h, mb_event_kind_t kind, const char *detail,
               uint16_t done, uint16_t total)
{
    mb_event_t e = {.kind = kind, .detail = detail,
                    .progress_done = done, .progress_total = total};
    app_studio_apply(h, &e);
}

int main(void)
{
    esp_gsp_handle_t ui = (esp_gsp_handle_t)0x1; /* the stubs ignore it */
    app_studio_handle_t studio = NULL;

    /* 1. Boot paints IDLE and claims nothing else. A boot-time BUILD_STARTED is
     *    what made the panel flash BUILDING -> FAILED -> IDLE on every power-up. */
    begin("boot paints IDLE and claims nothing else");
    panel_reset();
    app_studio_create(ui, &studio);
    CHECK(studio != NULL);
    CHECK(state_of(studio) == APP_STUDIO_IDLE);
    CHECK(strcmp(panel.state, "IDLE") == 0);
    CHECK(strcmp(panel.line1, "") == 0);
    CHECK(strcmp(panel.line2, "hold and speak") == 0);
    CHECK(panel.bear == true);
    CHECK(panel.success == false);
    CHECK(panel.seg[0] == false && panel.seg[4] == false);
    CHECK(panel.track[0] == false);
    CHECK(paint_count() == 1);

    /* 2. Releasing the button must not claim BUILDING before the request exists. */
    begin("release keeps LISTENING");
    ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_LISTENING);
    const int paints_after_press = paint_count();
    ev(studio, MB_EVENT_LISTENING_STOPPED, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_LISTENING);
    CHECK(paint_count() == paints_after_press);

    /* 3. BUILDING appears when the request goes out. */
    begin("BUILD_STARTED -> BUILDING");
    ev(studio, MB_EVENT_BUILD_STARTED, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_BUILDING);
    CHECK(strcmp(panel.line1, "building your") == 0);
    CHECK(strcmp(panel.line2, "program\u2026") == 0);

    /* 4. Scan start -> CONNECTING, two centred lines. */
    begin("AWAIT_MICROBIT -> CONNECTING");
    ev(studio, MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_AWAIT_MICROBIT);
    CHECK(strcmp(panel.state, "CONNECTING") == 0);
    CHECK(strcmp(panel.line1, "waiting for") == 0);
    CHECK(strcmp(panel.line2, "micro:bit\u2026") == 0);
    CHECK(panel.line1_y == 194 && panel.line2_y == 246);

    /* 5. Five-segment progress: nothing lit at 0, the active one blinks. */
    begin("FLASHING 0/96 lights only segment 1");
    ev(studio, MB_EVENT_FLASH_PROGRESS, NULL, 0, 96);
    CHECK(state_of(studio) == APP_STUDIO_FLASHING);
    CHECK(strcmp(panel.line1, "") == 0 && strcmp(panel.line2, "") == 0);
    CHECK(panel.track[0] && panel.track[4]);
    CHECK(panel.seg[0] == true);
    CHECK(panel.seg[1] == false && panel.seg[4] == false);
    CHECK(live_timers(500) == 1);

    begin("FLASHING 48/96: two solid, third blinking");
    ev(studio, MB_EVENT_FLASH_PROGRESS, NULL, 48, 96);
    CHECK(panel.seg[0] && panel.seg[1]);
    CHECK(panel.seg[2] == true);
    CHECK(panel.seg[3] == false && panel.seg[4] == false);

    begin("FLASHING 96/96: all solid, blink stopped");
    ev(studio, MB_EVENT_FLASH_PROGRESS, NULL, 96, 96);
    CHECK(panel.seg[0] && panel.seg[1] && panel.seg[2] && panel.seg[3] && panel.seg[4]);
    CHECK(live_timers(500) == 0);

    /* 6. Verify then done: DONE carries no text at all, success art up, bear gone. */
    begin("VERIFYING -> DONE carries no message");
    ev(studio, MB_EVENT_VERIFYING, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_VERIFYING);
    CHECK(strcmp(panel.line1, "checking your") == 0);
    ev(studio, MB_EVENT_DONE, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_DONE);
    CHECK(strcmp(panel.state, "") == 0);
    CHECK(strcmp(panel.line1, "") == 0 && strcmp(panel.line2, "") == 0);
    CHECK(panel.success == true);
    CHECK(panel.bear == false);
    CHECK(panel.seg[4] == false);
    CHECK(panel.track[0] == false);

    /* 7. A finished screen must not be repainted by a leftover cycle. */
    begin("stale FLASH_PROGRESS after DONE is ignored");
    fresh(&studio);
    drive_to_done(studio);
    CHECK(state_of(studio) == APP_STUDIO_DONE);
    const int paints_at_done = paint_count();
    ev(studio, MB_EVENT_FLASH_PROGRESS, NULL, 12, 96);
    ev(studio, MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_DONE);
    CHECK(paint_count() == paints_at_done);
    CHECK(panel.success == true);

    /* 8. FAILED shows the mapped two-line sentence, and stays put. */
    begin("FAILED maps the sentence to two lines");
    fresh(&studio);
    ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(studio, MB_EVENT_FAILED, "the micro:bit stopped answering", 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_FAILED);
    CHECK(strcmp(panel.state, "FAILED") == 0);
    CHECK(strcmp(panel.line1, "the micro:bit") == 0);
    CHECK(strcmp(panel.line2, "went quiet") == 0);
    CHECK(panel.bear == false);
    CHECK(panel.line1_y == 194);

    begin("stale FLASH_PROGRESS after FAILED is ignored");
    fresh(&studio);
    ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(studio, MB_EVENT_FAILED, "the micro:bit stopped answering", 0, 0);
    ev(studio, MB_EVENT_FLASH_PROGRESS, NULL, 12, 96);
    CHECK(state_of(studio) == APP_STUDIO_FAILED);
    CHECK(strcmp(panel.line2, "went quiet") == 0);

    /* 9. FAILED dwell returns to IDLE without input. */
    begin("FAILED dwell returns to IDLE");
    fresh(&studio);
    ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(studio, MB_EVENT_FAILED, "the build box is not answering", 0, 0);
    CHECK(fire_timer(8000) == 1);
    CHECK(state_of(studio) == APP_STUDIO_IDLE);
    CHECK(panel.bear == true && panel.success == false);

    /* 10. A new press always starts a cycle, from any finished screen. */
    begin("LISTENING_STARTED from DONE starts a new cycle");
    fresh(&studio);
    drive_to_done(studio);
    CHECK(state_of(studio) == APP_STUDIO_DONE);
    ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    CHECK(state_of(studio) == APP_STUDIO_LISTENING);
    CHECK(panel.success == false);

    /* 11. Ritual prompt: 15 s in CONNECTING swaps in the A+B+reset hint, and the
     *     scan is never abandoned (the hint is a hint, not a timeout). */
    begin("CONNECTING promotes the ritual prompt at 15 s");
    fresh(&studio);
    ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
    ev(studio, MB_EVENT_AWAIT_MICROBIT, NULL, 0, 0);
    CHECK(strcmp(panel.line1, "waiting for") == 0);
    CHECK(fire_timer(15000) == 1);
    CHECK(strcmp(panel.line1, "hold A+B, then") == 0);
    CHECK(strcmp(panel.line2, "tap reset") == 0);
    CHECK(state_of(studio) == APP_STUDIO_AWAIT_MICROBIT);

    /* 12. Every failure sentence the components can emit has a mapping. */
    begin("all known failure sentences map to two lines");
    {
        static const char *sentences[] = {
            "the build box is not answering",
            "the micro:bit stopped answering",
            "the program did not arrive intact",
            "this micro:bit needs a fresh start",
            "I could not hear that",
            "I cannot reach the build box",
            "the micro:bit went quiet",
            "the micro:bit could not be reached",
        };
        for (size_t i = 0; i < sizeof(sentences) / sizeof(sentences[0]); i++) {
            fresh(&studio);
            ev(studio, MB_EVENT_LISTENING_STARTED, NULL, 0, 0);
            ev(studio, MB_EVENT_FAILED, sentences[i], 0, 0);
            if (strcmp(panel.line1, "") == 0 || panel.line2[0] == '\0') {
                printf("    unmapped sentence: %s\n", sentences[i]);
                g_failures++;
            }
        }
    }

    app_studio_destroy(&studio);
    CHECK(studio == NULL);

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
