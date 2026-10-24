// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the pipeline reports to whoever renders it. Deliberately UI-free: voice,
 * build client and BLE central all speak this, so the screen layer is one
 * consumer among possible others (log, tests) and no pipeline component depends
 * on the UI component. */
typedef enum {
    MB_EVENT_LISTENING_STARTED = 0, /**< the AI button went down, recording began */
    MB_EVENT_LISTENING_STOPPED,     /**< button released, WAV sent to the service */
    MB_EVENT_BUILD_STARTED,         /**< request in flight: ASR -> LLM -> mkc */
    MB_EVENT_IMAGE_READY,           /**< region-2 bytes and geometry are in PSRAM */
    MB_EVENT_AWAIT_MICROBIT,        /**< scanning for a micro:bit advertiser */
    MB_EVENT_FLASH_PROGRESS,        /**< blocks acknowledged / blocks total */
    MB_EVENT_VERIFYING,             /**< region 2 is being read back and compared */
    MB_EVENT_DONE,                  /**< the new program is on the board */
    MB_EVENT_FAILED,                /**< `detail` carries the kid-facing sentence */
} mb_event_kind_t;

typedef struct {
    mb_event_kind_t kind;
    const char *detail;       /**< only for MB_EVENT_FAILED; must outlive the call */
    uint16_t progress_done;   /**< MB_EVENT_FLASH_PROGRESS: blocks acknowledged */
    uint16_t progress_total;  /**< MB_EVENT_FLASH_PROGRESS: blocks in the transfer */
} mb_event_t;

/** Called from pipeline tasks; implementations must be quick and must not block
 * on the reporter, which may be the BLE or HTTP task. */
typedef void (*mb_event_fn)(const mb_event_t *event);

#ifdef __cplusplus
}
#endif
