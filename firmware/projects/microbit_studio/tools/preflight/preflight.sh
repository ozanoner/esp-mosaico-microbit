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

step "1/5 state machine (host, no board)"
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

step "2/5 text fits its column"
"$PY" tools/preflight/check_text_fit.py; report $?

step "3/5 scene budget"
"$PY" tools/preflight/check_bundle.py; report $?

step "4/5 bench configuration"
"$PY" tools/preflight/check_config.py; report $?

step "5/5 build-service contract (audio fixture -> program -> image)"
base=$("$PY" - <<'EOF'
import pathlib, re
h = pathlib.Path("build/config/sdkconfig.h")
url = re.search(r'CONFIG_MB_VOICE_SERVICE_URL "([^"]*)"', h.read_text()).group(1) if h.exists() else ""
print(url.rsplit("/v1/", 1)[0] if url else "")
EOF
)
if [ -z "$base" ]; then
    echo "  SKIP: no service URL in sdkconfig yet"
elif ! curl -s -m 3 -o /dev/null "$base/v1/health"; then
    echo "  SKIP: $base unreachable from here (the panel would fail too — check that)"
else
    "$PY" tools/preflight/test_service_audio.py; report $?
fi

if [ "${WITH_BUILD:-0}" = "1" ]; then
    step "6/6 firmware build"
    idf.py build >/tmp/preflight_build.log 2>&1
    status=$?
    tail -3 /tmp/preflight_build.log
    report $status
fi

printf '\n%s\n' "$([ "$fail" -eq 0 ] && echo 'PREFLIGHT PASS — safe to install' || echo 'PREFLIGHT FAIL — do not install')"
exit "$fail"
