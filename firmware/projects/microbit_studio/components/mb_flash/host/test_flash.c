// SPDX-License-Identifier: Apache-2.0
//
// Host test for the packet layout. The address split is the part that silently
// corrupts a flash if it is wrong, so it is asserted byte by byte against the
// DAL's own formulas rather than eyeballed in a logic trace.

#include <stdio.h>
#include <string.h>

#include "mb_flash.h"

static int failures;

static void expect_bytes(const char *what, const uint8_t *got, const uint8_t *want, size_t length)
{
    if (memcmp(got, want, length) == 0) {
        return;
    }
    printf("FAIL %s\n  got ", what);
    for (size_t index = 0; index < length; index++) {
        printf("%02X ", got[index]);
    }
    printf("\n  want ");
    for (size_t index = 0; index < length; index++) {
        printf("%02X ", want[index]);
    }
    printf("\n");
    failures++;
}

static void test_block_address_split(void)
{
    /* Region 2 of the dice build starts at 0x37400, so:
     *   packet 0 must carry the low half  0x7400 -> data[1]=0x74, data[2]=0x00
     *   packet 1 must carry the high half 0x0003 -> data[1]=0x00, data[2]=0x03
     * The DAL assembles (p0[1]<<8|p0[2]) | (p1[1]<<24|p1[2]<<16). */
    uint8_t data[64];
    for (uint8_t index = 0; index < sizeof(data); index++) {
        data[index] = index;
    }

    uint8_t packets[MB_FLASH_PACKETS_PER_BLOCK][MB_FLASH_PACKET_BYTES];
    mb_flash_build_block(packets, 0x37400u, data);

    const uint8_t p0_head[4] = {0x01, 0x74, 0x00, 0x00};
    const uint8_t p1_head[4] = {0x01, 0x00, 0x03, 0x00};
    const uint8_t p2_head[4] = {0x01, 0x00, 0x00, 0x00};
    const uint8_t p3_head[4] = {0x01, 0x00, 0x00, 0x00};
    expect_bytes("packet 0 header (low half)", packets[0], p0_head, sizeof(p0_head));
    expect_bytes("packet 1 header (high half)", packets[1], p1_head, sizeof(p1_head));
    expect_bytes("packet 2 header", packets[2], p2_head, sizeof(p2_head));
    expect_bytes("packet 3 header", packets[3], p3_head, sizeof(p3_head));

    /* Data must land contiguously in 16-byte chunks, and the assembled address
     * must equal the block address through the DAL formula. */
    for (uint8_t index = 0; index < MB_FLASH_PACKETS_PER_BLOCK; index++) {
        expect_bytes("packet payload", &packets[index][4], &data[index * 16u], 16);
    }
    const uint32_t assembled = ((uint32_t)((packets[0][1] << 8) | packets[0][2])) |
                               ((uint32_t)((packets[1][1] << 24) | (packets[1][2] << 16)));
    if (assembled != 0x37400u) {
        printf("FAIL address assembly: got 0x%05X want 0x37400\n", (unsigned)assembled);
        failures++;
    }
}

static void test_region_info_and_end_of_tx(void)
{
    uint8_t region[MB_FLASH_PACKET_BYTES];
    mb_flash_build_region_info(region, MB_FLASH_REGION_DAL);
    const uint8_t want_region[20] = {0x00, 0x01};
    expect_bytes("REGION_INFO(1)", region, want_region, sizeof(want_region));

    uint8_t end[MB_FLASH_PACKET_BYTES];
    mb_flash_build_end_of_tx(end, 0x383C0u);
    if (end[0] != MB_FLASH_CMD_END_OF_TX) {
        printf("FAIL END_OF_TX opcode: 0x%02X\n", end[0]);
        failures++;
    }
}

static void test_notify_parse(void)
{
    mb_flash_notify_t notify;

    /* `01 FF` — block acknowledged. */
    const uint8_t ack[2] = {0x01, 0xFF};
    if (mb_flash_parse_notify(ack, sizeof(ack), &notify) != MB_FLASH_NOTIFY_WRITE_ACK) {
        printf("FAIL write ack not recognised\n");
        failures++;
    }

    /* `01 AA` — out of order. */
    const uint8_t resync[2] = {0x01, 0xAA};
    if (mb_flash_parse_notify(resync, sizeof(resync), &notify) != MB_FLASH_NOTIFY_WRITE_RESYNC) {
        printf("FAIL resync not recognised\n");
        failures++;
    }

    /* REGION_INFO answer: 00 01 00037400 0003BBFF <hash>. */
    uint8_t region[MB_FLASH_REGION_NOTIFY_BYTES] = {0x00, 0x01, 0x00, 0x03, 0x74, 0x00,
                                                    0x00, 0x03, 0xBB, 0xFF};
    for (uint8_t index = 0; index < 8; index++) {
        region[10 + index] = (uint8_t)(0xA0 + index);
    }
    if (mb_flash_parse_notify(region, sizeof(region), &notify) != MB_FLASH_NOTIFY_REGION) {
        printf("FAIL region info not recognised\n");
        failures++;
        return;
    }
    if (notify.region != 1 || notify.start != 0x00037400u || notify.end != 0x0003BBFFu) {
        printf("FAIL region info values: id=%u start=0x%05X end=0x%05X\n", notify.region,
               (unsigned)notify.start, (unsigned)notify.end);
        failures++;
    }
    const uint8_t want_hash[8] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
    expect_bytes("region hash", notify.hash, want_hash, sizeof(want_hash));

    /* STATUS answer `EE 01 01`. */
    const uint8_t status[3] = {0xEE, 0x01, 0x01};
    if (mb_flash_parse_notify(status, sizeof(status), &notify) != MB_FLASH_NOTIFY_STATUS ||
        notify.mode != 1 || notify.pfs_version != 1) {
        printf("FAIL status not recognised\n");
        failures++;
    }

    /* Short, empty and unknown payloads are refusals, not silent successes. */
    if (mb_flash_parse_notify(region, 4, &notify) != MB_FLASH_NOTIFY_UNKNOWN) {
        printf("FAIL truncated region info accepted\n");
        failures++;
    }
    if (mb_flash_parse_notify(NULL, 0, &notify) != MB_FLASH_NOTIFY_UNKNOWN) {
        printf("FAIL NULL notification accepted\n");
        failures++;
    }
    const uint8_t other[2] = {0x09, 0x01};
    if (mb_flash_parse_notify(other, sizeof(other), &notify) != MB_FLASH_NOTIFY_UNKNOWN) {
        printf("FAIL unknown opcode accepted\n");
        failures++;
    }
}

int main(void)
{
    test_block_address_split();
    test_region_info_and_end_of_tx();
    test_notify_parse();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mb_flash: all cases passed\n");
    return 0;
}
