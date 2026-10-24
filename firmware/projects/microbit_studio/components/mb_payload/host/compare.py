#!/usr/bin/env python3
"""Compare the C port (mb_payload) against the reference implementation (hexkit).

    python3 compare.py <image.hex> [path/to/test_payload]

Builds nothing: it runs the compiled C binary and recomputes every value with
tools/hexkit, then fails on the first difference. This is gate-G3's AC "C block
packing matches hexkit blocks() byte-for-byte on the same fixture" — the digest at
the end covers all 96 blocks, so a single shifted byte changes it.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
sys.path.insert(0, str(REPO / "tools" / "hexkit"))

import hexkit  # noqa: E402  (path set above)


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def expected(hex_path: Path) -> dict[str, str]:
    image = hexkit.load_hex(hex_path)
    template, program = image.hashes()
    start, end = image.region2()
    blocks = image.blocks()
    return {
        "magic": f"0x{image.dal_magic_address():05X}",
        "template": template.hex(),
        "program": program.hex(),
        "region2": f"0x{start:05X}-0x{end - 1:05X}",
        "blocks": str(len(blocks)),
        "fnv1a64": f"{fnv1a64(b''.join(data for _, data in blocks)):016x}",
    }


def actual(binary: Path, hex_path: Path) -> dict[str, str]:
    result = subprocess.run([str(binary), str(hex_path)], capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        raise SystemExit(f"C test failed ({result.returncode}): {result.stdout}{result.stderr}")
    return dict(line.split("=", 1) for line in result.stdout.strip().splitlines())


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    hex_path = Path(argv[1])
    binary = Path(argv[2]) if len(argv) > 2 else Path("/tmp/mb_payload_test")

    want = expected(hex_path)
    got = actual(binary, hex_path)

    failures = [(key, want[key], got.get(key)) for key in want if got.get(key) != want[key]]
    for key, want_value, got_value in failures:
        print(f"FAIL {key}: hexkit={want_value} mb_payload={got_value}")
    if failures:
        return 1

    print(f"mb_payload matches hexkit on {hex_path.name}:")
    for key in ("magic", "template", "program", "region2", "blocks", "fnv1a64"):
        print(f"  {key:<9} {want[key]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
