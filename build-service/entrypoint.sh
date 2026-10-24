#!/usr/bin/env bash
# Container entrypoint. Syncs the mounted project against the committed lock
# (offline: the wheels are in the image's uv cache) and then dispatches.
set -euo pipefail

PROJECT_DIR=/workspace
SERVICE_DIR=${PROJECT_DIR}/build-service

cd "${SERVICE_DIR}"
uv sync --frozen --extra dev --offline --quiet
export PATH="/opt/venv/bin:${PATH}"

case "${1:-serve}" in
    serve)
        exec python -m mb_build_service.app
        ;;
    verify)
        exec bash "${PROJECT_DIR}/scripts/verify.sh"
        ;;
    build)
        shift
        # Paths on the command line are project-relative: `-o build/x.hex`,
        # `microbit/payloads/y.ts`. Stay in the service directory and they
        # resolve against build-service/ instead.
        cd "${PROJECT_DIR}"
        exec python -m mb_build_service.builder "$@"
        ;;
    inspect)
        shift
        cd "${PROJECT_DIR}"
        exec python -m hexkit inspect "$@"
        ;;
    test)
        shift
        exec python -m pytest /workspace/tools/hexkit/tests /workspace/build-service/tests "$@"
        ;;
    shell)
        exec bash
        ;;
    *)
        exec "$@"
        ;;
esac
