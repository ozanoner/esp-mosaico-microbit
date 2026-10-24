// SPDX-License-Identifier: Apache-2.0
//
// The STATUS parser's own header. No ESP-IDF headers here so host/ can compile
// this file unchanged.

#include "mb_status_parse.h"

#define MB_STATUS_OPCODE 0xEE

mb_ble_status_parse_t mb_ble_status_parse(const uint8_t *data, size_t len, mb_ble_status_t *out)
{
    if (out == NULL || (data == NULL && len > 0)) {
        return MB_BLE_STATUS_PARSE_SHORT;
    }
    if (len < 3) {
        return MB_BLE_STATUS_PARSE_SHORT;
    }
    if (data[0] != MB_STATUS_OPCODE) {
        return MB_BLE_STATUS_PARSE_OPCODE;
    }
    out->pfs_version = data[1];
    out->mode = data[2];
    return MB_BLE_STATUS_PARSE_OK;
}
