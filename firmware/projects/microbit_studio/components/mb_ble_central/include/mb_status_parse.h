// SPDX-License-Identifier: Apache-2.0
#pragma once

// Pure parsing for the micro:bit partial-flash STATUS notification.
//
// Kept free of ESP-IDF headers on purpose: this header and its .c are compiled
// into the host test under host/ as well as into the firmware, so the same
// object proves the wire shape in both directions.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Parsed answer to a `0xEE` STATUS write.
 * Shape documented in docs/partial-flash-protocol.md: `EE <pfs version> <mode>`. */
typedef struct {
    uint8_t pfs_version; /**< Payload byte 1; 1 in the documented build. */
    uint8_t mode;        /**< Payload byte 2: 0 = pairing mode, 1 = application. */
} mb_ble_status_t;

/** Outcome of mb_ble_status_parse(). */
typedef enum {
    MB_BLE_STATUS_PARSE_OK = 0, /**< *out holds a parsed status. */
    MB_BLE_STATUS_PARSE_SHORT,  /**< Fewer than three bytes arrived. */
    MB_BLE_STATUS_PARSE_OPCODE, /**< Byte 0 is not 0xEE. */
} mb_ble_status_parse_t;

/** Parse a STATUS notification.
 *
 * Pure: no BLE state is read or written. data may be NULL only when len is 0;
 * out must be non-NULL. On any error *out is left untouched.
 *
 * Returns MB_BLE_STATUS_PARSE_OK, MB_BLE_STATUS_PARSE_SHORT or
 * MB_BLE_STATUS_PARSE_OPCODE.
 */
mb_ble_status_parse_t mb_ble_status_parse(const uint8_t *data, size_t len, mb_ble_status_t *out);

#ifdef __cplusplus
}
#endif
