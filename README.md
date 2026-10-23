# esp-mosaico-microbit

A kid asks an **ESP-Mosaico** for something she wants to see on her **micro:bit**.
The Mosaico turns the request into a MakeCode program, has it compiled on a LAN
build box, and flashes the micro:bit over **Bluetooth LE** — no cable, no editor.

```
   kid's voice          LAN box (x86 + node)             ESP-Mosaico (ESP32-S31)          micro:bit v1
 ──────────────►  mic ──HTTPS──►  ASR → LLM → mkc build ──►  hex → BLE partial flash  ──►  new program runs
                  (AI button)     → .hex + region hashes      progress UI + hash check
```

Status: **PoC** — this repository currently validates the hard part of the chain
(build a MakeCode hex, flash it over BLE, verify it landed). Voice and LLM come next.

## Components

| Path | What it is | Runs on |
|---|---|---|
| `brain/` | HTTP service: receives audio or source, returns a `.hex` + region hashes | LAN box (x86, node) |
| `firmware/` | ESP-Mosaico application (NimBLE central + partial-flash client + UI) | ESP32-S31 (`esp32s31`) |
| `microbit/` | Pinned MakeCode project template and example payload programs | LAN box (build input) |
| `tools/hexkit/` | Intel HEX parsing, DAL magic/hash extraction, region-2 block packing | anywhere (reference implementation) |
| `docs/` | Architecture, BLE protocol reference, bring-up gates | — |

## Why a build box

A micro:bit `.hex` can only be produced by MakeCode's compiler: `mkc`
"compiles MakeCode (PXT) projects by downloading parts of a released MakeCode web
app and running them in node.js". Nothing on an ESP32 can do that step, so the
boxes in the diagram are not a preference — they are the architecture.

The toolchain is **pinned**: BLE partial flashing refuses when the DAL hash of the
new image differs from what the device is running, so the box must never pick up a
new editor build on its own.

## Quickstart (PoC)

```sh
./scripts/bootstrap.sh                  # checks python/node, installs hexkit + brain

# 1. Build a canned program to a .hex and inspect it
python -m esp_mb_brain.builder microbit/payloads/hello.ts -o build/hello.hex
python -m hexkit inspect build/hello.hex

# 2. Serve the build API (LAN box)
python -m esp_mb_brain.app              # http://0.0.0.0:8080/v1/health
```

Firmware is built **only** inside the DevContainer (ESP-IDF image already on the
host, no host install):

```sh
docker compose -f .devcontainer/docker-compose.yml up -d
docker compose -f .devcontainer/docker-compose.yml exec esp-idf bash
cd firmware && python3 mosaico.py doctor
python3 mosaico.py project sim --project projects/microbit_studio --interactive
```

## Documentation

- [Architecture and contracts](docs/architecture.md)
- [micro:bit v1 BLE partial-flash protocol](docs/partial-flash-protocol.md)
- [Bring-up gates (do these before writing protocol code)](docs/bringup-gates.md)

## Scope

micro:bit **v1 (nRF51822)** only. The partial-flash service exists only in
MakeCode-built images; a board running MicroPython v1 has no BLE stack at all and
needs one USB flash to bootstrap.

## Licence

MIT — see [LICENSE](LICENSE).
