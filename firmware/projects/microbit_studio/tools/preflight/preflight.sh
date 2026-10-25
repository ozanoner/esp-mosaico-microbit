#!/usr/bin/env bash
# The gate: nothing gets installed on the panel until this passes.
#
#   tools/preflight/preflight.sh            # checks that need no board
#   WITH_BUILD=1 tools/preflight/preflight.sh   # inside the idf container
#
# PYTHON=... to pick an interpreter that has Pillow (the host venv has it).
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
PY="${PYTHON:-python3}"
fail=0

step() { printf '\n=== %s\n' "$1"; }
report() { if [ "$1" -ne 0 ]; then fail=1; echo "  -> FAILED"; fi; }

step "1/4 state machine (host, no board)"
tmp_bin=$(mktemp -t studio_test.XXXXXX)
gcc -std=c11 -Wall -Wextra -O1 -o "$tmp_bin" \
    tools/preflight/harness/test_state_machine.c \
    tools/preflight/harness/stubs.c \
    components/app_studio/app_studio.c \
    -Itools/preflight/harness/include \
    -Icomponents/app_studio/include \
    -Icomponents/mb_event/include || fail=1
if [ "$fail" -eq 0 ]; then "$tmp_bin"; report $?; fi
rm -f "$tmp_bin"

step "2/4 text fits its column"
"$PY" tools/preflight/check_text_fit.py; report $?

step "3/4 scene budget"
"$PY" tools/preflight/check_bundle.py; report $?

step "4/4 bench configuration"
"$PY" tools/preflight/check_config.py; report $?

if [ "${WITH_BUILD:-0}" = "1" ]; then
    step "5/5 firmware build"
    idf.py build >/tmp/preflight_build.log 2>&1
    status=$?
    tail -3 /tmp/preflight_build.log
    report $status
fi

printf '\n%s\n' "$([ "$fail" -eq 0 ] && echo 'PREFLIGHT PASS — safe to install' || echo 'PREFLIGHT FAIL — do not install')"
exit "$fail"
