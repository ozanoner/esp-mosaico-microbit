# esp-mosaico-microbit

A kid asks an **ESP-Mosaico** for something she wants to see on her **micro:bit**.
The Mosaico turns the request into a MakeCode program, has it compiled on a build
box, and flashes the micro:bit over **Bluetooth LE** — no cable, no editor.

```
   kid's voice          build box (container)            ESP-Mosaico (ESP32-S31)          micro:bit v1
 ──────────────►  mic ──HTTPS──►  ASR → LLM → mkc build ──►  hex → BLE partial flash  ──►  new program runs
                  (AI button)     → .hex + region hashes      progress UI + hash check
```

Status: **PoC** — this repository currently validates the hard part of the chain
(build a MakeCode hex with a pinned toolchain, extract the region geometry, flash
it over BLE, verify it landed). Voice and LLM come next.

## Requirements

**Docker only.** No ESP-IDF, no node, no Python, no venv on the host. Every tool
lives in a container; `docker compose` is the only entry point.

| | |
|---|---|
| Docker Engine + Compose v2 | required |
| micro:bit v1 (nRF51822) | required for the hardware path |
| ESP-Mosaico | required for the end-to-end path |

## Quickstart

```sh
git clone --recurse-submodules https://github.com/ozanoner/esp-mosaico-microbit
cd esp-mosaico-microbit

make verify        # tests + rebuild the PoC payload + compare against golden hashes
make up            # build service on http://localhost:8477/v1/health
make build         # build microbit/payloads/hello.ts -> build/hello.hex
make firmware      # shell with ESP-IDF + mosaico.py (firmware work)
make test          # unit tests only
make down          # stop everything
```

First `make verify` builds the build service image (node base + `npm ci` + `uv sync` +
a warm-up MakeCode compile) — a few minutes and a few hundred MB, once. After
that the container builds payloads with **no network access**.

Reproducibility is a checked property, not a promise: `make verify` fails if the
template hash, program hash, region-2 range or region-2 bytes differ from
`microbit/golden/expected.json`. See [docs/reproducibility.md](docs/reproducibility.md).

## Credentials

Nothing secret lives in this repository or in any image. A PAT is optional and
only needed to push from this checkout:

```sh
cp .env.example .env     # fill in GITHUB_TOKEN
set -a; . ./.env; set +a # exports GITHUB_TOKEN for git/gh
```

`.env` is gitignored **and** dockerignored, so it can never reach a commit or a
build context. No container reads it: the build-service image builds payloads with
no credentials of its own.

## Components

| Path | What it is | Runs on |
|---|---|---|
| `build-service/` | HTTP service: MakeCode source in, `.hex` + region hashes out | container (`build-service/Dockerfile`) |
| `firmware/` | ESP-Mosaico application (NimBLE central + partial-flash client + UI) | ESP32-S31 in the `esp-idf` container |
| `microbit/` | Pinned MakeCode template, PoC payloads, golden hashes | build input |
| `tools/hexkit/` | Intel HEX parsing, DAL magic/hash extraction, region-2 block packing | anywhere (reference for the C port) |
| `docs/` | Architecture, BLE protocol reference, bring-up gates, reproducibility | — |

## Why a build box

A micro:bit `.hex` can only be produced by MakeCode's compiler: `mkc`
"compiles MakeCode (PXT) projects by downloading parts of a released MakeCode web
app and running them in node.js". Nothing on an ESP32 can do that step, so the two
boxes in the diagram are the architecture, not a preference.

The toolchain is **pinned** because BLE partial flashing refuses an image whose DAL
hash differs from what the device runs. `mkc` refreshes its editor parts on its own
schedule, so the image bakes them in at build time and the runtime never fetches.

## Documentation

- [Architecture and contracts](docs/architecture.md)
- [micro:bit v1 BLE partial-flash protocol](docs/partial-flash-protocol.md)
- [Bring-up gates](docs/bringup-gates.md)
- [Reproducibility](docs/reproducibility.md)

## Scope

micro:bit **v1 (nRF51822)** only. The partial-flash service exists only in
MakeCode-built images; a board running MicroPython v1 has no BLE stack at all and
needs one USB flash to bootstrap.

## Licence

MIT — see [LICENSE](LICENSE).
