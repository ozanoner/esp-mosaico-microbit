#!/usr/bin/env bash
# PoC bootstrap: check the host tools, install the Python packages editable,
# and install the pinned MakeCode compiler if it is missing.
#
# Nothing here touches the ESP-Mosaico; firmware work happens in the project's
# DevContainer (see firmware/README.md).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing: $1 ($2)" >&2
        exit 1
    }
}

need python3 "3.10+"
need node   "20+ for mkc"
need npm    "comes with node"

python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' || {
    echo "python3 >= 3.10 required" >&2
    exit 1
}

if [ ! -d .venv ]; then
    python3 -m venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate

pip install --quiet --upgrade pip
pip install --quiet -e tools/hexkit -e "brain[dev]"

if ! command -v mkc >/dev/null 2>&1; then
    echo "installing the MakeCode compiler (makecode) globally…"
    npm install -g makecode
fi

echo
echo "toolchain: mkc $(mkc --version 2>/dev/null || echo unknown)"
echo "pin it now: record the version in brain/TOOLCHAIN.md and never run 'mkc --update'."
echo
echo "next:"
echo "  source .venv/bin/activate"
echo "  python -m pytest tools/hexkit/tests brain/tests -q"
echo "  python -m esp_mb_brain.builder microbit/payloads/hello.ts -o build/hello.hex"
