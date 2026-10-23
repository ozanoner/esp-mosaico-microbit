# Brain service

Turns MakeCode source into a micro:bit v1 `.hex` plus the two region hashes the
flasher needs. PoC scope: `/v1/health` and `/v1/build`. Speech and LLM endpoints
arrive once a generated image is proven to boot (gate G4).

Runs as a container; the host needs nothing installed.

```sh
docker compose run --rm brain verify                    # reproducibility check
docker compose run --rm brain build microbit/payloads/hello.ts -o build/hello.hex
docker compose run --rm brain inspect build/hello.hex
docker compose up -d brain                              # http://localhost:8080
docker compose run --rm brain shell                     # poke around
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
