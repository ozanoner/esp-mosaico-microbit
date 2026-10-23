#!/usr/bin/env bash
# Runs once when the container is created.
set -euo pipefail

cd /workspaces/esp-mosaico-microbit

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
  host runtime is bootstrapped into a named volume on first use and needs
  network access. Re-run `python3 mosaico.py doctor` after that bootstrap.
* The workspace documents ESP-IDF revision 7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe;
  the image ships a newer 6.2 master. The doctor requires >= 6.2 plus the
  esp32s31 target and accepts either — see firmware/README.md before assuming
  build parity with upstream.
* Flashing and serial capture happen on cirunner. This container has no USB
  device mappings by design.
EOF
