#!/usr/bin/env bash
# Prints the facts that decide whether a build is trustworthy. Cheap; runs on
# every container start.
set -euo pipefail

printf 'idf      : %s\n' "$(idf.py --version 2>/dev/null | tail -1 || echo unknown)"
printf 'revision : %s\n' "$(git -C "${IDF_PATH:-/opt/esp/idf}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
printf 'python   : %s\n' "$(python3 --version 2>&1)"
printf 'image    : %s\n' "${IDF_IMAGE:-espressif/idf:latest}"
