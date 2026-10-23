# Brain service

Turns MakeCode source into a micro:bit v1 `.hex` plus the two region hashes the
flasher needs. PoC scope: `/v1/health` and `/v1/build`. Speech and LLM endpoints
arrive once a generated image is proven to boot (gate G4).

```sh
python -m esp_mb_brain.builder microbit/payloads/hello.ts -o build/hello.hex
python -m esp_mb_brain.app          # 0.0.0.0:8080
```

## Toolchain pinning

`mkc` compiles by downloading parts of a released MakeCode editor and running them
in node. It refreshes those parts on its own schedule, and a refreshed editor means
a **new DAL hash** — which the partial-flash service rejects on the device. The box
must therefore run one frozen install:

1. Install once: `npm install -g makecode`.
2. Never pass `--update`.
3. Record the version and the resulting `template_hash` in `TOOLCHAIN.md`.
4. Expose the same id as `MB_TOOLCHAIN_ID` so `/v1/health` can prove it.

## Configuration

| Variable | Meaning | Default |
|---|---|---|
| `MB_TEMPLATE_DIR` | pinned MakeCode template | `microbit/template` |
| `MB_WORK_DIR` | scratch build area | `brain/.work` |
| `MB_MKC_BIN` | compiler binary | `mkc` |
| `MB_TOOLCHAIN_ID` | id reported by `/v1/health` | `unpinned` |
| `MB_BUILD_TIMEOUT_S` | per-build timeout | `180` |

## Response shape

```json
{
  "hex_b64": "…", "sha256": "…",
  "template_hash": "0662709fa0315567",
  "program_hash": "ede9690a1093ae71",
  "region2": {"start": 196608, "end": 244736},
  "block_count": 768,
  "toolchain_id": "mkc-2026-10-…",
  "log": "…"
}
```
