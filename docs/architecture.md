# Architecture

Four components, one of which is a box on the LAN. That is not a preference: the
`.hex` can only be produced by MakeCode's compiler (`mkc` runs the released
editor's compiler in node), so *something* with node and a network connection has
to sit between the kid's request and the micro:bit.

```
   kid's voice                LAN box                       ESP-Mosaico (esp32s31)              micro:bit v1
 ───────────────►  mic ──HTTPS──►  POST /v1/speak            ──►  parse hex, slice region 2   ──BLE──►  partial-flash
                   AI button      ASR → LLM → mkc build         pack 64-byte blocks                   service
                                  → hex + region hashes         NimBLE central, notify-paced
                                  (toolchain pinned)            GSP UI, progress, verdict
```

The same picture, rendered and source-cited: [solution view](architecture/solution.png)
([interactive](architecture/solution.html)) and [component view](architecture/components.png)
([interactive](architecture/components.html)). Boxes below refer to the components in that
second diagram; how to regenerate both is in [architecture/README.md](architecture/README.md).

## 1. Build service (`build-service/`)

One HTTPS endpoint on the LAN box rather than three device-side calls. Rationale:
the compile already has to live there, one TLS context keeps internal RAM free on
the Mosaico, and API keys for ASR/LLM never leave the box.

| Endpoint | Body | Returns |
|---|---|---|
| `GET /v1/health` | — | `toolchain_id`, editor version, DAL hash pinned |
| `POST /v1/build` | `{ "source": "<main.ts>" }` | build object (below) |
| `POST /v1/speak` | `audio/wav` (16 kHz mono, ≤15 s) | `transcript` + build object |

Build object:

```json
{
  "hex_b64": "…",
  "sha256": "…",
  "template_hash": "8 hex bytes",
  "program_hash": "8 hex bytes",
  "region2": { "start": 196608, "end": 244735 },
  "toolchain_id": "mkc-<editor build>",
  "log": "compiler output tail"
}
```

The service returns the hashes because it parses the produced hex server-side: the
device can then refuse a mismatched image before opening a BLE connection.

**Toolchain pinning is the load-bearing rule.** Partial flashing only accepts an
image whose DAL hash matches the running firmware. `mkc` refreshes its editor
parts on its own; the box must freeze one install, never pass `--update`, and
expose the pinned id through `/v1/health`.

## 2. ESP-Mosaico application (`firmware/`)

A normal Mosaico application installed through Vibe Mode, following the retained
Recovery contract (partition prefix preserved; OTA writer stays in Vibe Mode).
Device operations go through `mosaico.py` only.

| Component | Responsibility |
|---|---|
| `mb_payload` | Intel HEX → linear image, magic/hash extraction, region-2 64-byte blocks |
| `mb_flash` | NimBLE central: scan, connect, CCCD, five opcodes, notify-paced blocking, resync |
| `build_service_client` | HTTPS client; TLS allocations in PSRAM |
| `app_studio` | State machine + GSP scene on the 480×480 panel |

State machine — every state is a screen, not a log line:

```
IDLE → LISTENING → BUILDING → CONNECTING → FLASHING → VERIFYING → DONE | FAILED
```

What the screens show, and why the chain is this short:

- **No transcript confirm, no separate GENERATING screen.** `/v1/speak` runs ASR →
  LLM → `mkc` inside one request, so the device never holds a transcript to
  confirm and cannot tell the generation phase from the compile. Both would be
  guessed, so neither is shown.
- **`CONNECTING` is the `AWAIT_MICROBIT` state.** Gate G2 measured `EE 01 01` →
  `mode=1 (application)`, so a board running our template advertises by itself
  and the ritual is a fallback: if no micro:bit answers within 15 s the detail
  line becomes "hold A+B, then tap reset" and the scan keeps running.
- **FLASHING is five segments of 20 %.** The segment being written pulses at
  500 ms; finished segments stay solid. No block counts reach the screen.
- **DONE shows the trophy clip only** — no success text.

### RAM budget

The board has ~45 KB internal SRAM free and a ~20 KB largest internal block after
display bring-up, and each QSPI flush can transiently take ~77 KB of DMA bounce.
Therefore: no on-device ASR (push-to-talk + cloud ASR), TLS buffers in PSRAM
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, and a
`largestSRAM` sampler from the first build rather than snapshots.

## 3. micro:bit payload template (`microbit/`)

No micro:bit firmware development: the partial-flash service ships in the DAL and
is enabled for MakeCode programs. The repository only owns a pinned project
template (Bluetooth extension, "No pairing required", `bluetooth.startUartService()`
in `on start`) plus example payloads.

## 4. Reference tooling (`tools/hexkit/`)

The Python implementation of the hex geometry is the oracle for the C code in
`mb_payload`: same magic constants, same region map, same block packing, with unit
tests. Divergence between the two is the most likely source of a bricked-looking
flash, so both get tested against the same fixtures.

## Verification

| Question | How |
|---|---|
| Did the image land? | Reconnect, read `REGION_INFO` region 2, compare with `program_hash` |
| Does it run? | The template renders a 2-character revision glyph at boot |
| Is the toolchain still pinned? | `/v1/health` `toolchain_id` unchanged |

A failed flash never leaves a half-written program: nothing is written until the
build is valid, and an aborted transfer leaves the previous image in place.
