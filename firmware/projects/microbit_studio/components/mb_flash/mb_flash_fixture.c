// SPDX-License-Identifier: Apache-2.0
//
// Access to the embedded region-2 fixture. Kept out of mb_flash.c so that file
// stays free of build-system symbols and can be compiled by the host test.

#include "mb_flash.h"

/* Provided by EMBED_FILES in CMakeLists.txt. */
extern const uint8_t _binary_mb_fixture_bin_start[];
extern const uint8_t _binary_mb_fixture_bin_end[];

const uint8_t *mb_flash_fixture_data(void)
{
    return _binary_mb_fixture_bin_start;
}

uint32_t mb_flash_fixture_len(void)
{
    return (uint32_t)(_binary_mb_fixture_bin_end - _binary_mb_fixture_bin_start);
}
