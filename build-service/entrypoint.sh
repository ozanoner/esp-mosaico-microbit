#!/usr/bin/env bash
# Container entrypoint. Syncs the mounted project against the committed lock
# (offline: the wheels are in the image's uv cache) and then dispatches.
set -euo pipefail

cd /workspace/build-service
uv sync --frozen --extra dev --offline --quiet
export PATH="/opt/venv/bin:${PATH}"

case "${1:-serve}" in
    serve)
        exec python -m mb_build_service.app
        ;;
    verify)
        exec bash /workspace/scripts/verify.sh
        ;;
    build)
        shift
        exec python -m mb_build_service.builder "$@"
        ;;
    inspect)
        shift
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
