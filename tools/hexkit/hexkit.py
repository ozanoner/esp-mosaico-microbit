"""Intel HEX geometry for micro:bit v1 partial flashing.

This module is the reference implementation for the firmware's `mb_payload`
component: identical magic constants, identical region map, identical block
packing. Both are tested against the same fixtures because divergence between
them is the most likely way to produce an image that silently fails to boot.

Facts and their sources:

* region map, hash placement:
  ``lancaster-university/microbit-dal/source/drivers/MicroBitMemoryMap.cpp``
  (``pushRegion`` / ``findHashes``)
* 64-byte blocks, address split across the first two packets of a block:
  ``…/source/bluetooth/MicroBitPartialFlashingService.cpp``
* transfer range: from the DAL magic up to the embedded-source magic
  (micro:bit partial-flashing documentation)
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

#: 16-byte DAL magic that precedes the region hashes in a MakeCode image.
DAL_MAGIC = bytes.fromhex("708E3B92C615A841C49866C975EE5197")
#: Magic that marks the start of the embedded source; the transfer ends before it.
EMBEDDED_SOURCE_MAGIC = bytes.fromhex("41140E2FB82FA2BB")

#: micro:bit v1 (nRF51822, 256 KB flash) region 2 boundaries.
REGION2_START = 0x30000
REGION2_END = 0x3BBFF

#: One WRITE_DATA block is four 16-byte packets; the device writes 64 B per block.
BLOCK_BYTES = 64
PACKET_BYTES = 16

#: Region ids accepted by the REGION_INFO command.
REGION_SOFTDEVICE = 0
REGION_DAL = 1
REGION_PXT = 2


class HexError(ValueError):
    """Raised when a hex file cannot be interpreted."""


@dataclass
class Segment:
    start: int
    data: bytearray

    @property
    def end(self) -> int:
        """Exclusive end address."""
        return self.start + len(self.data)


@dataclass
class Image:
    """A sparse flash image built from Intel HEX records."""

    segments: list[Segment] = field(default_factory=list)
    #: Non-Intel record types seen in the file, kept for inspection only.
    #: MakeCode writes 0x0A/0x0C/0x0D records that are not flash data.
    extra_records: dict[int, list[tuple[int, bytes]]] = field(default_factory=dict)

    # -- construction ----------------------------------------------------

    def add(self, address: int, data: bytes) -> None:
        if not data:
            return
        for segment in self.segments:
            if segment.start <= address and address + len(data) <= segment.end:
                offset = address - segment.start
                segment.data[offset : offset + len(data)] = data
                return
        self.segments.append(Segment(address, bytearray(data)))
        self.segments.sort(key=lambda s: s.start)
        self._coalesce()

    def _coalesce(self) -> None:
        merged: list[Segment] = []
        for segment in self.segments:
            if merged and segment.start <= merged[-1].end:
                last = merged[-1]
                overlap = last.end - segment.start
                if overlap < len(segment.data):
                    last.data.extend(segment.data[overlap:])
            else:
                merged.append(Segment(segment.start, bytearray(segment.data)))
        self.segments = merged

    # -- queries ---------------------------------------------------------

    def read(self, address: int, length: int) -> bytes:
        """Return ``length`` bytes at ``address`` or raise if not contiguous."""
        for segment in self.segments:
            if segment.start <= address and address + length <= segment.end:
                offset = address - segment.start
                return bytes(segment.data[offset : offset + length])
        raise HexError(f"no contiguous data for 0x{address:08X}+{length}")

    def find(self, pattern: bytes) -> int | None:
        """Return the first address where ``pattern`` appears, or None."""
        for segment in self.segments:
            index = segment.data.find(pattern)
            if index >= 0:
                return segment.start + index
        return None

    def covered(self, start: int, end: int) -> bool:
        """True when every byte in ``[start, end)`` is present."""
        cursor = start
        for segment in self.segments:
            if segment.start <= cursor < segment.end:
                cursor = min(end, segment.end)
                if cursor == end:
                    return True
        return False

    def read_or_ff(self, address: int, length: int) -> bytes:
        """Read ``length`` bytes, filling anything absent with 0xFF."""
        out = bytearray(b"\xff" * length)
        for segment in self.segments:
            lo = max(address, segment.start)
            hi = min(address + length, segment.end)
            if lo < hi:
                out[lo - address : hi - address] = segment.data[lo - segment.start : hi - segment.start]
        return bytes(out)

    def covered_end(self, start: int, ceiling: int) -> int:
        """Highest covered address in ``[start, ceiling)``."""
        end = start
        for segment in self.segments:
            if segment.start >= ceiling:
                continue  # e.g. the bootloader living above region 2
            end = max(end, min(segment.end, ceiling))
        return end

    # -- micro:bit specifics --------------------------------------------

    def dal_magic_address(self) -> int:
        address = self.find(DAL_MAGIC)
        if address is None:
            raise HexError(
                "DAL magic 708E3B92C615A841C49866C975EE5197 not found — "
                "this is not a MakeCode image (no partial-flash metadata)"
            )
        return address

    def hashes(self) -> tuple[bytes, bytes]:
        """Return ``(template_hash, program_hash)`` as 8-byte values."""
        magic = self.dal_magic_address()
        return self.read(magic + 16, 8), self.read(magic + 24, 8)

    def region2(self) -> tuple[int, int]:
        """Return the half-open ``(start, end)`` range to transfer.

        The start is the DAL magic. The end is the page containing the
        embedded-source magic when present — "the client transfers the hex data
        from the 'magic number' up until the page that contains the embedded
        source" — and the highest byte actually present in region 2 otherwise
        (useful for synthetic images in tests).
        """
        start = self.dal_magic_address()
        if start < REGION2_START or start > REGION2_END:
            raise HexError(
                f"DAL magic at 0x{start:08X} is outside region 2 "
                f"(0x{REGION2_START:08X}–0x{REGION2_END:08X})"
            )
        ceiling = REGION2_END + 1
        embedded = self.find(EMBEDDED_SOURCE_MAGIC)
        if embedded is not None and start <= embedded < ceiling:
            return start, min((embedded // 1024) * 1024 + 1024, ceiling)
        end = self.covered_end(start, ceiling)
        # Whole pages: the device erases a page before writing into it, so a
        # partial final page would drop the rest of that page's contents.
        end = min(((end + 1023) // 1024) * 1024, ceiling)
        return start, end

    def blocks(self) -> list[tuple[int, bytes]]:
        """Return ``[(address, 64 bytes)]`` ready for WRITE_DATA."""
        start, end = self.region2()
        last = self.covered_end(start, REGION2_END + 1)
        if not self.covered(start, last):
            raise HexError(
                f"region 2 payload 0x{start:08X}–0x{last - 1:08X} is not contiguous in the hex"
            )
        out: list[tuple[int, bytes]] = []
        for address in range(start, end, BLOCK_BYTES):
            out.append((address, self.read_or_ff(address, BLOCK_BYTES)))
        return out


# -- Intel HEX parsing ---------------------------------------------------


def parse_hex(text: str) -> Image:
    """Parse Intel HEX text into a sparse :class:`Image`."""
    image = Image()
    base = 0
    for number, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise HexError(f"line {number}: record does not start with ':'")
        try:
            raw = bytes.fromhex(line[1:])
        except ValueError as exc:
            raise HexError(f"line {number}: not valid hex ({exc})") from exc
        if len(raw) < 5:
            raise HexError(f"line {number}: record too short")
        count, address, record_type = raw[0], (raw[1] << 8) | raw[2], raw[3]
        payload = raw[4 : 4 + count]
        checksum = raw[4 + count] if len(raw) > 4 + count else 0
        if (sum(raw[: 4 + count]) + checksum) & 0xFF:
            raise HexError(f"line {number}: bad checksum")
        if record_type == 0x00:
            image.add(base + address, payload)
        elif record_type == 0x01:
            break
        elif record_type == 0x02:
            base = int.from_bytes(payload, "big") << 4
        elif record_type == 0x04:
            base = int.from_bytes(payload, "big") << 16
        elif record_type in (0x03, 0x05):
            continue  # start addresses are irrelevant for flashing
        elif record_type in (0x0A, 0x0C, 0x0D):
            # MakeCode (PXT) extensions. 0x0A carries a 0xC0DE marker block,
            # 0x0C filler, 0x0D records that are addressed from 0 rather than
            # from the linear base. None of them is flash data for our purpose:
            # ignoring them keeps the reconstructed image correct, and the fact
            # that they were seen is preserved for diagnostics.
            image.extra_records.setdefault(record_type, []).append((base + address, payload))
        else:
            raise HexError(f"line {number}: unsupported record type 0x{record_type:02X}")
    return image


def load_hex(path: str | Path) -> Image:
    return parse_hex(Path(path).read_text())


# -- CLI -----------------------------------------------------------------


def _cmd_inspect(path: Path) -> int:
    image = load_hex(path)
    magic = image.dal_magic_address()
    template_hash, program_hash = image.hashes()
    start, end = image.region2()
    blocks = image.blocks()
    print(f"file            : {path}")
    print(f"dal magic       : 0x{magic:08X}")
    print(f"template hash   : {template_hash.hex()}")
    print(f"program hash    : {program_hash.hex()}")
    print(f"region 2        : 0x{start:08X}–0x{end - 1:08X} ({(end - start)} B)")
    print(f"blocks          : {len(blocks)} × {BLOCK_BYTES} B")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="hexkit", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser("inspect", help="print region map and hashes")
    inspect.add_argument("hex", type=Path)
    args = parser.parse_args(argv)
    try:
        return _cmd_inspect(args.hex)
    except (HexError, OSError) as exc:
        print(f"hexkit: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
