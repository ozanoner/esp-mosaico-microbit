// SPDX-License-Identifier: Apache-2.0
//
// Host test for the STATUS parser. Built with -Wall -Wextra -Werror, so adding
// a parser branch without a case here is a build failure, not a silent gap.

#include <stdio.h>
#include <string.h>

#include "mb_status_parse.h"

static int failures;

static void expect_status(const char *what, const uint8_t *data, size_t len,
                          mb_ble_status_parse_t want_rc, uint8_t want_pfs, uint8_t want_mode)
{
    mb_ble_status_t status = {0xAA, 0xAA}; /* sentinel: untouched on error */
    const mb_ble_status_parse_t rc = mb_ble_status_parse(data, len, &status);

    if (rc != want_rc) {
        printf("FAIL %s: rc=%d want %d\n", what, (int)rc, (int)want_rc);
        failures++;
        return;
    }
    if (rc != MB_BLE_STATUS_PARSE_OK) {
        if (status.pfs_version != 0xAA || status.mode != 0xAA) {
            printf("FAIL %s: output written on error\n", what);
            failures++;
        }
        return;
    }
    if (status.pfs_version != want_pfs || status.mode != want_mode) {
        printf("FAIL %s: pfs=%u mode=%u want pfs=%u mode=%u\n", what, status.pfs_version,
               status.mode, want_pfs, want_mode);
        failures++;
    }
}

int main(void)
{
    /* The documented application-mode answer: `EE 01 01`. */
    const uint8_t app_mode[] = {0xEE, 0x01, 0x01};
    expect_status("application mode", app_mode, sizeof(app_mode), MB_BLE_STATUS_PARSE_OK, 1, 1);

    /* Pairing mode is the same bytes with mode 0 — gate G2 records which one. */
    const uint8_t pairing[] = {0xEE, 0x01, 0x00};
    expect_status("pairing mode", pairing, sizeof(pairing), MB_BLE_STATUS_PARSE_OK, 1, 0);

    /* A future pfs version must still parse: only the shape is asserted. */
    const uint8_t future[] = {0xEE, 0x02, 0x01};
    expect_status("future pfs", future, sizeof(future), MB_BLE_STATUS_PARSE_OK, 2, 1);

    /* Truncated and empty notifications are refusals, not zero-filled statuses. */
    const uint8_t two[] = {0xEE, 0x01};
    expect_status("two bytes", two, sizeof(two), MB_BLE_STATUS_PARSE_SHORT, 0, 0);
    expect_status("empty", NULL, 0, MB_BLE_STATUS_PARSE_SHORT, 0, 0);

    /* Wrong opcode: the region-info / write-ack notifications use other codes. */
    const uint8_t region_info[] = {0x00, 0x02, 0x01};
    expect_status("region info", region_info, sizeof(region_info), MB_BLE_STATUS_PARSE_OPCODE, 0, 0);
    const uint8_t write_ok[] = {0x01, 0xFF};
    expect_status("write ack", write_ok, sizeof(write_ok), MB_BLE_STATUS_PARSE_SHORT, 0, 0);

    /* NULL out is refused rather than dereferenced. */
    if (mb_ble_status_parse(app_mode, sizeof(app_mode), NULL) != MB_BLE_STATUS_PARSE_SHORT) {
        printf("FAIL null out\n");
        failures++;
    }

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mb_status_parse: all cases passed\n");
    return 0;
}
