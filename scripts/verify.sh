#!/usr/bin/env bash
# Reproducibility check: this must pass on any clone, on any machine.
#
#   1. unit tests
#   2. rebuild a payload with the pinned toolchain
#   3. compare hash + geometry with microbit/golden/expected.json
set -euo pipefail

# Works inside the brain container (/workspace) and on a checkout with a local venv.
ROOT="${MB_ROOT:-/workspace}"
if [ ! -d "$ROOT" ]; then
    ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
cd "$ROOT"
if [ -x /opt/venv/bin/python ]; then
    export PATH="/opt/venv/bin:${PATH}"
fi

echo "== toolchain =="
mkc --version
python --version
echo "toolchain id: ${MB_TOOLCHAIN_ID:-unset}"

echo
echo "== unit tests =="
python -m pytest tools/hexkit/tests brain/tests -q

echo
echo "== rebuild payload =="
rm -f build/hello.hex
MB_TOOLCHAIN_ID="${MB_TOOLCHAIN_ID:-mkc-1.3.6}" \
    python -m esp_mb_brain.builder microbit/payloads/hello.ts -o build/hello.hex

echo
echo "== compare against golden =="
python - <<'PY'
import json
import sys

import hexkit

golden = json.load(open("microbit/golden/expected.json", encoding="utf-8"))
expected = golden["payloads"]["hello"]

image = hexkit.load_hex("build/hello.hex")
template_hash, program_hash = image.hashes()
start, end = image.region2()
blocks = image.blocks()
region_bytes = b"".join(data for _, data in blocks)

actual = {
    "toolchain_id": golden["toolchain_id"],
    "hex_sha256": __import__("hashlib").sha256(open("build/hello.hex", "rb").read()).hexdigest(),
    "template_hash": template_hash.hex(),
    "program_hash": program_hash.hex(),
    "region2_start": start,
    "region2_end": end,
    "block_count": len(blocks),
    "region2_sha256": __import__("hashlib").sha256(region_bytes).hexdigest(),
}

failures = []
for key, want in expected.items():
    got = actual[key]
    if got != want:
        failures.append(f"  {key}: expected {want!r}, got {got!r}")

if failures:
    print("REPRODUCIBILITY FAILED")
    print("\n".join(failures))
    print(
        "\nIf the toolchain was upgraded on purpose, refresh "
        "microbit/golden/expected.json in the same commit and say so in the message."
    )
    sys.exit(1)

print(f"OK  template={actual['template_hash']}  program={actual['program_hash']}")
print(f"    region 2 0x{start:05X}-0x{end - 1:05X}  blocks={len(blocks)}")
print(f"    region sha256={actual['region2_sha256']}")
PY
