"""Tests for hexkit using synthetic MakeCode-shaped images.

The fixture mirrors the real layout: region 2 starts at 0x30000 with the 16-byte
magic, followed by the template hash and the program hash, then the program body.

Intel HEX addresses are 16-bit, so anything at or above 0x10000 needs an
extended-linear-address (type 04) record first — the helper below does that.
"""

from __future__ import annotations

import pytest

from hexkit import (
    BLOCK_BYTES,
    DAL_MAGIC,
    EMBEDDED_SOURCE_MAGIC,
    REGION2_END,
    REGION2_START,
    HexError,
    Image,
    parse_hex,
)

TEMPLATE_HASH = bytes.fromhex("0662709FA0315567")
PROGRAM_HASH = bytes.fromhex("EDE9690A1093AE71")
HEADER = DAL_MAGIC + TEMPLATE_HASH + PROGRAM_HASH + b"\x0b\x00" + b"\x00" * 10


def _record(address: int, data: bytes, record_type: int = 0) -> str:
    body = bytes([len(data), (address >> 8) & 0xFF, address & 0xFF, record_type]) + data
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def hex_records(address: int, data: bytes, chunk: int = 32) -> list[str]:
    """Emit records for ``data`` at ``address`` (chunked, handles > 0xFFFF)."""
    lines: list[str] = []
    if address >= 0x10000:
        lines.append(_record(0, (address >> 16).to_bytes(2, "big"), 0x04))
    for offset in range(0, len(data), chunk):
        lines.append(_record((address + offset) & 0xFFFF, data[offset : offset + chunk]))
    return lines


def make_hex(payload: bytes, magic_at: int = REGION2_START) -> str:
    lines = hex_records(magic_at, HEADER + payload)
    return "\n".join(lines + [_record(0, b"", 0x01)]) + "\n"


def test_parses_and_finds_magic() -> None:
    image = parse_hex(make_hex(b"\xAA" * 64))
    assert image.dal_magic_address() == REGION2_START


def test_hashes_are_read_from_the_magic_header() -> None:
    image = parse_hex(make_hex(b"\xAA" * 64))
    assert image.hashes() == (TEMPLATE_HASH, PROGRAM_HASH)


def test_region2_start_is_the_magic_and_end_covers_the_data() -> None:
    image = parse_hex(make_hex(b"\xAA" * 64))
    start, end = image.region2()
    assert start == REGION2_START
    # the range is rounded up to whole 1 KB pages
    assert end == REGION2_START + 1024


def test_data_above_region2_does_not_extend_the_range() -> None:
    image = Image()
    image.add(REGION2_START, HEADER + b"\xAA" * 64)
    image.add(0x3C000, b"\xBB" * 512)  # bootloader, above region 2
    start, end = image.region2()
    assert start == REGION2_START
    assert end == REGION2_START + 1024


def test_pxt_extension_records_are_ignored_but_recorded() -> None:
    data = hex_records(REGION2_START, HEADER + b"\xAA" * 64)
    extra = [_record(0, bytes.fromhex("9900c0de"), 0x0A), _record(0, b"\x42" * 16, 0x0C)]
    image = parse_hex("\n".join(data + extra + [_record(0, b"", 0x01)]) + "\n")
    assert image.dal_magic_address() == REGION2_START
    assert sorted(image.extra_records) == [0x0A, 0x0C]


def test_region2_stops_at_the_page_containing_embedded_source() -> None:
    payload = b"\xAA" * 1024 + EMBEDDED_SOURCE_MAGIC + b"\xBB" * 32
    image = parse_hex(make_hex(payload))
    _, end = image.region2()
    # embedded magic lands in the second 1 KB page, so the transfer ends after it
    assert end == REGION2_START + 2048


def test_region2_is_capped_at_the_end_of_flash() -> None:
    image = Image()
    image.add(REGION2_START, HEADER + b"\xAA" * (REGION2_END - REGION2_START))
    _, end = image.region2()
    assert end == REGION2_END + 1


def test_blocks_are_64_bytes_and_4_byte_aligned() -> None:
    payload = b"\xAA" * (3 * BLOCK_BYTES)
    image = parse_hex(make_hex(payload))
    start, end = image.region2()
    blocks = image.blocks()
    assert len(blocks) == (end - start) // BLOCK_BYTES
    assert all(len(data) == BLOCK_BYTES for _, data in blocks)
    assert all(address % 4 == 0 for address, _ in blocks)
    assert [a for a, _ in blocks] == [start + i * BLOCK_BYTES for i in range(len(blocks))]


def test_tail_beyond_the_program_is_filled_with_ff() -> None:
    payload = b"\xAA" * 20
    image = parse_hex(make_hex(payload))
    start, end = image.region2()
    blocks = image.blocks()
    used = len(HEADER) + len(payload)
    flat = b"".join(data for _, data in blocks)
    assert flat[:used] == HEADER + payload
    # everything from the end of the program to the page boundary is erased flash
    assert flat[used:] == b"\xff" * (end - start - used)


def test_missing_magic_is_a_clear_error() -> None:
    line = hex_records(REGION2_START, b"\x00" * 16)
    image = parse_hex("\n".join(line + [_record(0, b"", 0x01)]) + "\n")
    with pytest.raises(HexError, match="not a MakeCode image"):
        image.dal_magic_address()


def test_magic_outside_region2_is_rejected() -> None:
    image = parse_hex(make_hex(b"\xAA" * 64, magic_at=0x20000))
    with pytest.raises(HexError, match="outside region 2"):
        image.region2()


def test_bad_checksum_is_rejected() -> None:
    line = _record(0, b"\x00" * 16)
    with pytest.raises(HexError, match="bad checksum"):
        parse_hex(line[:-2] + "00\n")


def test_gap_in_region2_is_rejected() -> None:
    image = Image()
    image.add(REGION2_START, HEADER)
    image.add(REGION2_START + 0x200, b"\xAA" * 16)  # hole after the header
    with pytest.raises(HexError, match="not contiguous"):
        image.blocks()
