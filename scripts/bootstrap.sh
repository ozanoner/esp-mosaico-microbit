#!/usr/bin/env bash
# Kept for muscle memory: the supported path is `make verify`.
# Nothing is installed on the host — everything runs in containers.
set -euo pipefail

cd "$(dirname "$0")/.."

command -v docker >/dev/null 2>&1 || {
    echo "docker is required; nothing else is installed on the host" >&2
    exit 1
}
docker compose version >/dev/null 2>&1 || {
    echo "docker compose v2 is required" >&2
    exit 1
}

echo "building the build service image and running the reproducibility check…"
exec docker compose run --rm build-service verify
