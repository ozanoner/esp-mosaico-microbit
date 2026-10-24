#!/usr/bin/env bash
# Runs once when the DevContainer is created: reports what the toolchain is.
set -euo pipefail

cd "$(dirname "$0")/.."

echo "== ESP-IDF =="
idf.py --version 2>/dev/null | tail -1 || true
git -C "${IDF_PATH:-/opt/esp/idf}" log -1 --format='revision: %h (%ad) %s' --date=short || true

echo
echo "== Mosaico host doctor =="
cd firmware
python3 mosaico.py doctor || true

cat <<'EOF'

Notes
-----
* `esp-iris` may report unavailable on the very first run: the pinned ESP-Iris
  host runtime is bootstrapped into the named volume `esp-iris-state` on first
  use and needs network access. Re-run `python3 mosaico.py doctor` after it.
* The ESP-IDF image is pinned by digest in compose.yaml. Upstream documents
  revision 7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe; this image carries 6.2.0
  master (ab95e70748f2). The doctor requires >= 6.2 plus the esp32s31 target and
  accepts either — see firmware/README.md before assuming upstream build parity.
* Flashing and serial capture happen on cirunner. This container has no USB
  device mappings by design.
* The build box (`build service`) is a separate service: `docker compose up -d build-service`.
EOF
