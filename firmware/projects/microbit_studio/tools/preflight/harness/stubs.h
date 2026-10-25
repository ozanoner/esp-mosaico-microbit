// What the harness records, and how a test drives the fake clock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char state[64];
    char line1[128];
    char line2[128];
    int32_t line1_y;
    int32_t line2_y;
    int32_t bear_y;
    bool bear;
    bool success;
    bool seg[5];
    bool track[5];
} panel_t;

extern panel_t panel;

void panel_reset(void);
void timers_reset(void);
int paint_count(void);

/* Timers: live count (ms == 0 matches any), and firing the next one. */
int live_timers(uint32_t ms);
int fire_timer(uint32_t ms);

#ifdef __cplusplus
}
#endif
