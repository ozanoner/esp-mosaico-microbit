// SPDX-License-Identifier: Apache-2.0

#include "mb_payload.h"

#include <string.h>

static const uint8_t k_dal_magic[16] = MB_PAYLOAD_DAL_MAGIC_INIT;
static const uint8_t k_source_magic[8] = MB_PAYLOAD_SOURCE_MAGIC_INIT;

#define WINDOW_ORIGIN MB_PAYLOAD_REGION2_START
#define WINDOW_SPAN MB_PAYLOAD_WINDOW_BYTES
/* Coverage is tracked per 64-byte block: one bit per block. */
#define TOUCH_BITS (WINDOW_SPAN / MB_PAYLOAD_BLOCK_BYTES)
#define TOUCH_BYTES ((TOUCH_BITS + 7) / 8)

/* Parse state kept between the per-line steps of one call. Static because the
 * parse is bounded and single-shot; documented as non-reentrant in the header. */
static uint8_t s_touched[TOUCH_BYTES];
static uint32_t s_covered_end;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void mark_touched(uint32_t address, size_t length)
{
    const uint32_t first = (address - WINDOW_ORIGIN) / MB_PAYLOAD_BLOCK_BYTES;
    const uint32_t last = (address - WINDOW_ORIGIN + (uint32_t)length - 1u) / MB_PAYLOAD_BLOCK_BYTES;

    for (uint32_t block = first; block <= last && block < TOUCH_BITS; block++) {
        s_touched[block / 8] |= (uint8_t)(1u << (block % 8));
    }
}

static bool block_touched(uint32_t block)
{
    return (s_touched[block / 8] & (uint8_t)(1u << (block % 8))) != 0;
}

/* Store one data record, clipped to the region-2 window. Records outside it are
 * dropped: this port only ever needs region 2, unlike hexkit which keeps the
 * whole sparse image. */
static void store(uint8_t *window, uint32_t address, const uint8_t *data, size_t length)
{
    if (length == 0) {
        return;
    }

    uint32_t start = address;
    uint32_t offset = 0;
    const uint32_t window_end = WINDOW_ORIGIN + WINDOW_SPAN;

    if (start < WINDOW_ORIGIN) {
        const uint32_t skip = WINDOW_ORIGIN - start;
        if (skip >= length) {
            return;
        }
        offset = skip;
        start = WINDOW_ORIGIN;
    }
    if (start >= window_end) {
        return;
    }

    size_t room = length - offset;
    if (start + room > window_end) {
        room = window_end - start;
    }
    memcpy(&window[start - WINDOW_ORIGIN], &data[offset], room);
    mark_touched(start, room);
    if (start + room > s_covered_end) {
        s_covered_end = start + room;
    }
}

mb_payload_err_t mb_payload_from_hex(const char *text, size_t text_len, uint8_t *window,
                                     size_t window_len, mb_payload_info_t *info)
{
    if (text == NULL || window == NULL || info == NULL || window_len < MB_PAYLOAD_WINDOW_BYTES) {
        return MB_PAYLOAD_ERR_ARG;
    }

    memset(window, 0xFF, MB_PAYLOAD_WINDOW_BYTES);
    memset(s_touched, 0, sizeof(s_touched));
    s_covered_end = WINDOW_ORIGIN;

    uint32_t base = 0;
    size_t cursor = 0;

    while (cursor < text_len) {
        /* Skip separators so both LF and CRLF files work. */
        while (cursor < text_len && (text[cursor] == '\r' || text[cursor] == '\n' ||
                                     text[cursor] == ' ' || text[cursor] == '\t')) {
            cursor++;
        }
        if (cursor >= text_len) {
            break;
        }
        if (text[cursor] != ':') {
            return MB_PAYLOAD_ERR_HEX;
        }
        cursor++;

        uint8_t record[260];
        size_t record_len = 0;
        while (cursor + 1 < text_len && text[cursor] != '\r' && text[cursor] != '\n' &&
               text[cursor] != ' ') {
            const int hi = hex_nibble(text[cursor]);
            const int lo = hex_nibble(text[cursor + 1]);
            if (hi < 0 || lo < 0 || record_len >= sizeof(record)) {
                return MB_PAYLOAD_ERR_HEX;
            }
            record[record_len++] = (uint8_t)((hi << 4) | lo);
            cursor += 2;
        }
        if (record_len < 5) {
            return MB_PAYLOAD_ERR_HEX;
        }

        const uint8_t count = record[0];
        if ((size_t)count + 5u > record_len) {
            return MB_PAYLOAD_ERR_HEX;
        }
        uint8_t sum = 0;
        for (size_t index = 0; index < (size_t)count + 5u; index++) {
            sum = (uint8_t)(sum + record[index]);
        }
        if (sum != 0) {
            return MB_PAYLOAD_ERR_HEX;
        }

        const uint16_t offset = (uint16_t)((record[1] << 8) | record[2]);
        const uint8_t type = record[3];
        const uint8_t *payload = &record[4];

        switch (type) {
        case 0x00:
            store(window, base + offset, payload, count);
            break;
        case 0x01:
            cursor = text_len; /* EOF record */
            break;
        case 0x02:
            if (count != 2) {
                return MB_PAYLOAD_ERR_HEX;
            }
            base = (uint32_t)((payload[0] << 8) | payload[1]) << 4;
            break;
        case 0x04:
            if (count != 2) {
                return MB_PAYLOAD_ERR_HEX;
            }
            base = (uint32_t)((payload[0] << 8) | payload[1]) << 16;
            break;
        case 0x03:
        case 0x05:
            break; /* start addresses are irrelevant for flashing */
        case 0x0A:
        case 0x0C:
        case 0x0D:
            /* MakeCode (PXT) extensions: a 0xC0DE marker block, filler, and
             * records addressed from 0 rather than the linear base. None of it is
             * flash data; hexkit records them for diagnostics and ignores them. */
            break;
        default:
            return MB_PAYLOAD_ERR_HEX;
        }
    }

    uint32_t magic = 0;
    bool found = false;
    for (uint32_t index = 0; index + sizeof(k_dal_magic) <= WINDOW_SPAN; index++) {
        if (window[index] == k_dal_magic[0] &&
            memcmp(&window[index], k_dal_magic, sizeof(k_dal_magic)) == 0) {
            magic = WINDOW_ORIGIN + index;
            found = true;
            break;
        }
    }
    if (!found) {
        return MB_PAYLOAD_ERR_NO_MAGIC;
    }
    if (magic + 32u > WINDOW_ORIGIN + WINDOW_SPAN) {
        return MB_PAYLOAD_ERR_RANGE; /* hashes would fall outside region 2 */
    }

    info->magic_address = magic;
    info->region2_start = magic;
    memcpy(info->template_hash, &window[magic + 16u - WINDOW_ORIGIN], MB_PAYLOAD_HASH_BYTES);
    memcpy(info->program_hash, &window[magic + 24u - WINDOW_ORIGIN], MB_PAYLOAD_HASH_BYTES);

    const uint32_t ceiling = MB_PAYLOAD_REGION2_END + 1u;
    uint32_t end = 0;
    for (uint32_t index = 0; index + sizeof(k_source_magic) <= WINDOW_SPAN; index++) {
        if (window[index] == k_source_magic[0] &&
            memcmp(&window[index], k_source_magic, sizeof(k_source_magic)) == 0) {
            const uint32_t page = (WINDOW_ORIGIN + index) / 1024u * 1024u + 1024u;
            end = page < ceiling ? page : ceiling;
            break;
        }
    }
    if (end == 0) {
        /* No embedded source: end at the highest byte region 2 actually covers,
         * rounded up to a whole page because the device erases before writing. */
        end = ((s_covered_end + 1023u) / 1024u) * 1024u;
        if (end > ceiling) {
            end = ceiling;
        }
    }
    if (end <= magic) {
        return MB_PAYLOAD_ERR_HEX;
    }
    info->region2_end = end;

    /* Contiguity: no wholly untouched block between the magic and the covered
     * end. hexkit checks covered(start, covered_end) — not up to region2_end,
     * because a page may legitimately end in erased 0xFF bytes. */
    const uint32_t covered_stop = s_covered_end < end ? s_covered_end : end;
    const uint32_t first_block = (magic - WINDOW_ORIGIN) / MB_PAYLOAD_BLOCK_BYTES;
    const uint32_t last_block =
        (magic - WINDOW_ORIGIN + (covered_stop - magic) - 1u) / MB_PAYLOAD_BLOCK_BYTES;
    for (uint32_t block = first_block; block <= last_block && block < TOUCH_BITS; block++) {
        if (!block_touched(block)) {
            return MB_PAYLOAD_ERR_GAP;
        }
    }
    return MB_PAYLOAD_OK;
}

uint32_t mb_payload_block_count(const mb_payload_info_t *info)
{
    if (info == NULL || info->region2_end <= info->region2_start) {
        return 0;
    }
    return (info->region2_end - info->region2_start + MB_PAYLOAD_BLOCK_BYTES - 1u) /
           MB_PAYLOAD_BLOCK_BYTES;
}

mb_payload_err_t mb_payload_block(const mb_payload_info_t *info, const uint8_t *window,
                                  uint32_t index, uint8_t out[MB_PAYLOAD_BLOCK_BYTES])
{
    if (info == NULL || window == NULL || out == NULL) {
        return MB_PAYLOAD_ERR_ARG;
    }
    if (info->region2_start < WINDOW_ORIGIN || info->region2_start > MB_PAYLOAD_REGION2_END) {
        return MB_PAYLOAD_ERR_RANGE;
    }
    const uint64_t address = (uint64_t)info->region2_start + (uint64_t)index * MB_PAYLOAD_BLOCK_BYTES;
    if (address + MB_PAYLOAD_BLOCK_BYTES > info->region2_end) {
        return MB_PAYLOAD_ERR_RANGE;
    }

    const uint8_t *source = &window[(uint32_t)address - WINDOW_ORIGIN];
    memcpy(out, source, MB_PAYLOAD_BLOCK_BYTES);
    return MB_PAYLOAD_OK;
}
