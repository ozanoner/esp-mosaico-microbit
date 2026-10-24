// SPDX-License-Identifier: Apache-2.0
//
// Host test for mb_payload: parses a real mkc-built hex and prints the values
// the firmware will act on. tools/hexkit is the reference implementation, so the
// check is a byte-for-byte comparison of the two — the C side prints, the Python
// side computes, and any divergence is a failure (see host/compare.py).
//
// Built with -Wall -Wextra -Werror, so an unused path or a sloppy conversion is a
// build failure rather than a silent difference.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mb_payload.h"

/* 48 KB: the device does not need this buffer (it embeds the extracted region 2),
 * the host does, because the parse window must span all of region 2. */
static uint8_t s_window[MB_PAYLOAD_WINDOW_BYTES];

static uint64_t fnv1a64(uint64_t hash, const uint8_t *data, size_t length)
{
    for (size_t index = 0; index < length; index++) {
        hash ^= data[index];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image.hex>\n", argv[0]);
        return 2;
    }

    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("fopen");
        return 2;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 2;
    }
    const long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 2;
    }
    char *text = malloc((size_t)size);
    if (text == NULL) {
        fclose(file);
        return 2;
    }
    const size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(text);
        return 2;
    }

    mb_payload_info_t info;
    const mb_payload_err_t err =
        mb_payload_from_hex(text, read, s_window, sizeof(s_window), &info);
    free(text);
    if (err != MB_PAYLOAD_OK) {
        printf("parse_error=%d\n", (int)err);
        return 1;
    }

    const uint32_t count = mb_payload_block_count(&info);
    uint64_t digest = 0xcbf29ce484222325ULL;
    uint8_t block[MB_PAYLOAD_BLOCK_BYTES];
    for (uint32_t index = 0; index < count; index++) {
        if (mb_payload_block(&info, s_window, index, block) != MB_PAYLOAD_OK) {
            printf("block_error=%u\n", index);
            return 1;
        }
        digest = fnv1a64(digest, block, sizeof(block));
    }

    printf("magic=0x%05X\n", (unsigned)info.magic_address);
    printf("template=");
    for (size_t index = 0; index < MB_PAYLOAD_HASH_BYTES; index++) {
        printf("%02x", info.template_hash[index]);
    }
    printf("\nprogram=");
    for (size_t index = 0; index < MB_PAYLOAD_HASH_BYTES; index++) {
        printf("%02x", info.program_hash[index]);
    }
    printf("\nregion2=0x%05X-0x%05X\n", (unsigned)info.region2_start,
           (unsigned)(info.region2_end - 1u));
    printf("blocks=%u\n", (unsigned)count);
    printf("fnv1a64=%016llx\n", (unsigned long long)digest);
    return 0;
}
