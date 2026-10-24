// SPDX-License-Identifier: Apache-2.0

#include "mb_flash.h"

#include <string.h>

/* Address split, DAL MicroBitPartialFlashingService.cpp:207-221:
 *   case 0: offset  = (data[1] << 8) | data[2];          // low 16 bits
 *   case 1: offset |= (data[1] << 24) | (data[2] << 16); // high 16 bits
 * Only packets 0 and 1 carry address bytes; 2 and 3 leave them at zero. */
static void stamp_address(uint8_t *packet, uint32_t address, uint8_t packet_index)
{
    switch (packet_index) {
    case 0:
        packet[1] = (uint8_t)((address >> 8) & 0xFF);
        packet[2] = (uint8_t)(address & 0xFF);
        break;
    case 1:
        packet[1] = (uint8_t)((address >> 24) & 0xFF);
        packet[2] = (uint8_t)((address >> 16) & 0xFF);
        break;
    default:
        packet[1] = 0;
        packet[2] = 0;
        break;
    }
}

void mb_flash_build_region_info(uint8_t out[MB_FLASH_PACKET_BYTES], uint8_t region)
{
    memset(out, 0, MB_FLASH_PACKET_BYTES);
    out[0] = MB_FLASH_CMD_REGION_INFO;
    out[1] = region;
}

void mb_flash_build_block(uint8_t out[MB_FLASH_PACKETS_PER_BLOCK][MB_FLASH_PACKET_BYTES],
                          uint32_t address, const uint8_t data[64])
{
    for (uint8_t index = 0; index < MB_FLASH_PACKETS_PER_BLOCK; index++) {
        uint8_t *packet = out[index];
        memset(packet, 0, MB_FLASH_PACKET_BYTES);
        packet[0] = MB_FLASH_CMD_WRITE_DATA;
        stamp_address(packet, address, index);
        packet[3] = 0; /* the caller stamps the transfer-wide counter */
        memcpy(&packet[4], &data[index * 16u], 16);
    }
}

void mb_flash_build_end_of_tx(uint8_t out[MB_FLASH_PACKET_BYTES], uint32_t offset_address)
{
    memset(out, 0, MB_FLASH_PACKET_BYTES);
    out[0] = MB_FLASH_CMD_END_OF_TX;
    /* The device answers with {0x02, 0x00} but takes no address from this packet;
     * the offset bytes are informational for logs. */
    out[1] = (uint8_t)((offset_address >> 8) & 0xFF);
    out[2] = (uint8_t)(offset_address & 0xFF);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

mb_flash_notify_kind_t mb_flash_parse_notify(const uint8_t *data, size_t length,
                                             mb_flash_notify_t *out)
{
    if (data == NULL || length < 2) {
        return MB_FLASH_NOTIFY_UNKNOWN;
    }

    switch (data[0]) {
    case MB_FLASH_CMD_WRITE_DATA:
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        if (data[1] == 0xFF) {
            if (out != NULL) {
                out->kind = MB_FLASH_NOTIFY_WRITE_ACK;
            }
            return MB_FLASH_NOTIFY_WRITE_ACK;
        }
        if (data[1] == 0xAA) {
            if (out != NULL) {
                out->kind = MB_FLASH_NOTIFY_WRITE_RESYNC;
            }
            return MB_FLASH_NOTIFY_WRITE_RESYNC;
        }
        return MB_FLASH_NOTIFY_UNKNOWN;

    case MB_FLASH_CMD_REGION_INFO:
        if (length < MB_FLASH_REGION_NOTIFY_BYTES) {
            return MB_FLASH_NOTIFY_UNKNOWN;
        }
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
            out->kind = MB_FLASH_NOTIFY_REGION;
            out->region = data[1];
            out->start = read_be32(&data[2]);
            out->end = read_be32(&data[6]);
            memcpy(out->hash, &data[10], sizeof(out->hash));
        }
        return MB_FLASH_NOTIFY_REGION;

    case MB_FLASH_CMD_STATUS:
        if (length < 3) {
            return MB_FLASH_NOTIFY_UNKNOWN;
        }
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
            out->kind = MB_FLASH_NOTIFY_STATUS;
            out->pfs_version = data[1];
            out->mode = data[2];
        }
        return MB_FLASH_NOTIFY_STATUS;

    default:
        return MB_FLASH_NOTIFY_UNKNOWN;
    }
}
