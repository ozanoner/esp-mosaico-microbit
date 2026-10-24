// SPDX-License-Identifier: Apache-2.0
#pragma once

// Intel HEX geometry for micro:bit v1 partial flashing — C port of
// tools/hexkit/hexkit.py. That module is the reference implementation: same
// magic constants, same region map, same block packing, same fixtures. The host
// test under host/ compares both on the same hex, because divergence between
// them is the most likely way to produce an image that silently fails to boot.
//
// Sources for the constants and the region map:
//   lancaster-university/microbit-dal → source/drivers/MicroBitMemoryMap.cpp
//     (pushRegion / findHashes)
//   codal-microbit-v2 → source/bluetooth/MicroBitPartialFlashingService.cpp
//     (64-byte blocks, address split across the first two packets)
//   docs/partial-flash-protocol.md in this repository
//
// No allocation, no ESP-IDF dependencies: this header and mb_payload.c are
// compiled into the firmware and into the host test unchanged.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 16-byte DAL magic that precedes the region hashes in a MakeCode image. */
#define MB_PAYLOAD_DAL_MAGIC_INIT \
    { 0x70, 0x8E, 0x3B, 0x92, 0xC6, 0x15, 0xA8, 0x41, 0xC4, 0x98, 0x66, 0xC9, 0x75, 0xEE, 0x51, 0x97 }
/** Magic that marks the start of the embedded source; the transfer ends before it. */
#define MB_PAYLOAD_SOURCE_MAGIC_INIT \
    { 0x41, 0x14, 0x0E, 0x2F, 0xB8, 0x2F, 0xA2, 0xBB }

/** micro:bit v1 (nRF51822, 256 KB flash) region 2 boundaries. */
#define MB_PAYLOAD_REGION2_START 0x30000u
#define MB_PAYLOAD_REGION2_END 0x3BBFFu

/** One WRITE_DATA block is four 16-byte packets; the device writes 64 B per block. */
#define MB_PAYLOAD_BLOCK_BYTES 64u
#define MB_PAYLOAD_PACKET_BYTES 16u
/** Each region hash is 8 bytes, stored at magic+16 and magic+24. */
#define MB_PAYLOAD_HASH_BYTES 8u

/** Bytes of the caller-provided window that covers region 2 completely. */
#define MB_PAYLOAD_WINDOW_BYTES (MB_PAYLOAD_REGION2_END - MB_PAYLOAD_REGION2_START + 1u)

/** Region ids accepted by the REGION_INFO command. */
typedef enum {
    MB_PAYLOAD_REGION_SOFTDEVICE = 0,
    MB_PAYLOAD_REGION_DAL = 1,
    MB_PAYLOAD_REGION_PXT = 2,
} mb_payload_region_t;

typedef enum {
    MB_PAYLOAD_OK = 0,
    MB_PAYLOAD_ERR_ARG,     /**< NULL pointer, or a window smaller than region 2. */
    MB_PAYLOAD_ERR_HEX,     /**< Malformed record, bad checksum, unsupported type. */
    MB_PAYLOAD_ERR_NO_MAGIC,/**< DAL magic absent: not a MakeCode image. */
    MB_PAYLOAD_ERR_RANGE,   /**< DAL magic outside region 2. */
    MB_PAYLOAD_ERR_GAP,     /**< Payload between magic and covered end is not contiguous. */
} mb_payload_err_t;

/** Geometry and hashes of one image, as needed by the flash client. */
typedef struct {
    uint32_t magic_address;  /**< Where the DAL magic sits; region 2 starts here. */
    uint32_t region2_start;  /**< Same as magic_address, kept explicit. */
    uint32_t region2_end;    /**< Exclusive; always a whole 1 KB page boundary. */
    uint8_t template_hash[MB_PAYLOAD_HASH_BYTES]; /**< Region 1 (DAL) hash. */
    uint8_t program_hash[MB_PAYLOAD_HASH_BYTES];  /**< Region 2 (program) hash. */
} mb_payload_info_t;

/** Parse Intel HEX text and fill `info` from the region-2 window.
 *
 * `window` must hold at least MB_PAYLOAD_WINDOW_BYTES and receives region 2 in
 * place: byte 0 is 0x30000. Bytes the hex does not cover stay 0xFF, which is
 * what an erased page reads as — the same convention as hexkit's read_or_ff().
 *
 * The parse is bounded and non-throwing; it keeps no state between calls except
 * a coverage bitmap, so it is not reentrant and must not run concurrently with
 * itself.
 *
 * Returns MB_PAYLOAD_OK, or one of the errors above with `info` unspecified.
 */
mb_payload_err_t mb_payload_from_hex(const char *text, size_t text_len, uint8_t *window,
                                     size_t window_len, mb_payload_info_t *info);

/** Number of 64-byte blocks in region 2: ceil((end - start) / 64). */
uint32_t mb_payload_block_count(const mb_payload_info_t *info);

/** Copy block `index` (0-based) as 64 bytes, 0xFF where the hex has no data.
 *
 * Returns MB_PAYLOAD_OK, MB_PAYLOAD_ERR_ARG for a bad pointer or index, and
 * MB_PAYLOAD_ERR_RANGE when the block lies outside region 2.
 */
mb_payload_err_t mb_payload_block(const mb_payload_info_t *info, const uint8_t *window,
                                  uint32_t index, uint8_t out[MB_PAYLOAD_BLOCK_BYTES]);

#ifdef __cplusplus
}
#endif
