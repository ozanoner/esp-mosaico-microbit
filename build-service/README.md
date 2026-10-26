# Build service

Turns MakeCode source into a micro:bit v1 `.hex` plus the two region hashes the
flasher needs, and turns speech into that source. Endpoints: `/v1/health`,
`/v1/build` and `/v1/speak`.

Runs as a container; the host needs nothing installed.

```sh
docker compose run --rm build-service verify                    # reproducibility check
docker compose run --rm build-service build microbit/payloads/hello.ts -o build/hello.hex
docker compose run --rm build-service inspect build/hello.hex
docker compose up -d build-service                              # http://localhost:8477
docker compose run --rm build-service shell                     # poke around
```

## Image contents (all pinned)

| Layer | Pin |
|---|---|
| base | `node:22-bookworm-slim@sha256:de17…` |
| `mkc` | `makecode@1.3.6` via `npm ci` from `package-lock.json` |
| MakeCode editor parts | downloaded **once at image build**, frozen into `/root/.pxt` |
| Python | `uv sync --frozen` from `uv.lock` |

At runtime the entrypoint re-syncs the mounted project against the lock **offline**
from the image's uv cache, so editing the code needs no rebuild; only a dependency
change does.

## Configuration

| Variable | Meaning | Default |
|---|---|---|
| `MB_TEMPLATE_DIR` | pinned MakeCode template | `/workspace/microbit/template` |
| `MB_WORK_DIR` | scratch build area | `/tmp/mb-work` |
| `MB_MKC_BIN` | compiler binary | `mkc` |
| `MB_TOOLCHAIN_ID` | id reported by `/v1/health` | `mkc-1.3.6` |
| `MB_BUILD_TIMEOUT_S` | per-build timeout | `180` |
| `MB_PORT` | port uvicorn listens on | `8477` |

Host-side, `BUILD_SERVICE_PORT` changes which port compose publishes; the container
always listens on 8477. 8477 is chosen to stay clear of the usual 3000/5000/8000/8080
defaults so a second service on the same LAN box cannot collide with it.

## Response shape

```json
{
  "hex_b64": "…", "sha256": "…",
  "template_hash": "4cc31a5b0ac4adb3",
  "program_hash": "f200340042003f00",
  "region2": {"start": 226304, "end": 232448},
  "block_count": 96,
  "toolchain_id": "mkc-1.3.6",
  "log": "…"
}
```

Measured values for the two PoC payloads: `TOOLCHAIN.md`.

## `POST /v1/speak` — speech (or text) in, compiled image out

The voice entry point. Two content types:

- `audio/wav` (raw body) → ASR → LLM → `mkc` → build object
- `application/json` `{"text": "..."}` → skips ASR, useful for tests and for a
  device that has already shown the transcript to a human

Credentials are read from the environment only (`MB_ASR_*`, `MB_LLM_*` in
`.env`, gitignored and dockerignored). Both providers speak the OpenAI-compatible
HTTP API with `urllib`, so there is no new runtime dependency and swapping to a
local vLLM or a LAN gateway is a `.env` change. With no key set, `/v1/speak`
answers `503` with the missing variable's name; `/v1/health` and `/v1/build`
keep working.

One retry, with the compiler's own output fed back. If the second attempt also
fails, the response is `502` carrying `transcript`, `source`, `attempts` and
`compiler_message`, so the device can display the real error instead of "try
again". The prompt forbids imports, packages and `pxt.json` changes, and
requires a `REVISION` constant shown at boot — the DAL hash must not move, or
the partial-flash service will refuse the image.
