// SPDX-License-Identifier: Apache-2.0
#pragma once

// micro:bit v1 partial-flash protocol — packet building and notification parsing.
//
// No BLE, no ESP-IDF: everything here is pure, so the wire layout is proven by a
// host test instead of by flashing a board and hoping. The byte layout comes from
// the DAL itself, `lancaster-university/microbit-dal`
// `source/bluetooth/MicroBitPartialFlashingService.cpp`:
//
//   packet          | 1 byte  | 2 bytes | 1 byte  | 16 bytes
//                   | COMMAND | OFFSET  | PACKET# | DATA
//
//   case 0: offset  = ((data[1] << 8) | data[2]);           // LOW 16 bits
//   case 1: offset |= ((data[1] << 24) | (data[2] << 16));  // HIGH 16 bits
//
// i.e. packet 0 carries the low half, packet 1 the high half: region 2 at
// 0x37400 is `74 00` then `00 03`. docs/partial-flash-protocol.md said the
// opposite before this was read from source.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed packet size: 1 + 2 + 1 + 16, exactly the default ATT MTU payload. */
#define MB_FLASH_PACKET_BYTES 20u
/** Packets per WRITE_DATA block; the device writes 64 B per block. */
#define MB_FLASH_PACKETS_PER_BLOCK 4u
/** Bytes per WRITE_DATA block: 4 packets x 16 data bytes. */
#define MB_FLASH_BLOCK_BYTES 64u
/** REGION_INFO notification: opcode + region + start(4) + end(4) + hash(8). */
#define MB_FLASH_REGION_NOTIFY_BYTES 18u

/** Opcodes on the characteristic (DAL `PartialFlashingServiceCommand`). */
typedef enum {
    MB_FLASH_CMD_REGION_INFO = 0x00,
    MB_FLASH_CMD_WRITE_DATA = 0x01,
    MB_FLASH_CMD_END_OF_TX = 0x02,
    MB_FLASH_CMD_STATUS = 0xEE,
    MB_FLASH_CMD_RESET = 0xFF,
} mb_flash_command_t;

/** Region ids the device accepts in REGION_INFO. */
typedef enum {
    MB_FLASH_REGION_SOFTDEVICE = 0, /**< 0x00000-0x18000 */
    MB_FLASH_REGION_DAL = 1,        /**< 0x18000-FLASH_PROGRAM_END */
    MB_FLASH_REGION_PXT = 2,        /**< user program */
} mb_flash_region_t;

/** What arrived on the notify characteristic. */
typedef enum {
    MB_FLASH_NOTIFY_UNKNOWN = 0,
    MB_FLASH_NOTIFY_WRITE_ACK,   /**< `01 FF`: the 64-byte block landed. */
    MB_FLASH_NOTIFY_WRITE_RESYNC,/**< `01 AA`: out of order; skip to blockStart+4. */
    MB_FLASH_NOTIFY_REGION,      /**< `00 <id> <start:4> <end:4> <hash:8>`. */
    MB_FLASH_NOTIFY_STATUS,      /**< `EE <pfs version> <mode>`. */
} mb_flash_notify_kind_t;

typedef struct {
    mb_flash_notify_kind_t kind;
    uint8_t region;      /**< REGION_INFO: region id. */
    uint32_t start;      /**< REGION_INFO: first address, inclusive. */
    uint32_t end;        /**< REGION_INFO: last address, inclusive. */
    uint8_t hash[8];     /**< REGION_INFO: the hash the device declares. */
    uint8_t pfs_version; /**< STATUS: partial-flashing service version. */
    uint8_t mode;        /**< STATUS: 0 = pairing, 1 = application. */
} mb_flash_notify_t;

/** Build the one-packet REGION_INFO request for `region`. */
void mb_flash_build_region_info(uint8_t out[MB_FLASH_PACKET_BYTES], uint8_t region);

/** Build the four WRITE_DATA packets for one 64-byte block at `address`.
 *
 * `out` is written as `out[4][20]`. The packet counter is left at 0 in every
 * packet; the caller stamps `packet[3]` with the transfer-wide counter, because
 * the counter runs across the whole transfer rather than restarting per block.
 */
void mb_flash_build_block(uint8_t out[MB_FLASH_PACKETS_PER_BLOCK][MB_FLASH_PACKET_BYTES],
                          uint32_t address, const uint8_t data[64]);

/** Build the single END_OF_TX packet: the device writes the remaining 16 bytes
 * at the current offset, strips the embedded-source magic and resets.
 *
 * `offset_address` is the address the device's offset register holds, i.e. the
 * last block written — it is only used to stamp the packet's offset field.
 */
void mb_flash_build_end_of_tx(uint8_t out[MB_FLASH_PACKET_BYTES], uint32_t offset_address);

/** Parse a notification. Returns the kind; `out` is filled when non-NULL.
 *
 * Unknown or short payloads return MB_FLASH_NOTIFY_UNKNOWN and leave `out`
 * untouched, so a caller can log the raw bytes rather than trust a guess.
 */
mb_flash_notify_kind_t mb_flash_parse_notify(const uint8_t *data, size_t length,
                                             mb_flash_notify_t *out);

/** The embedded region-2 image, and its length in bytes. */
const uint8_t *mb_flash_fixture_data(void);
uint32_t mb_flash_fixture_len(void);

#ifdef __cplusplus
}
#endif
