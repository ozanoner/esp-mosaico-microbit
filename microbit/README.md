# micro:bit side

No firmware development happens here. The partial-flash service ships inside the
DAL and is enabled for MakeCode programs ("partial flashing is only supported by
programs built using MakeCode" — micro:bit partial-flash docs), so all we own is:

- `template/` — the pinned MakeCode project the brain service compiles. Only
  `main.ts` varies between builds; `pxt.json` is fixed so the DAL — and therefore
  the template hash compared during flashing — never moves.
- `payloads/` — example programs, used by the PoC and by tests.

## Template rules

1. `bluetooth.startUartService()` runs in `on start`. It keeps the device
   advertising, so the *next* flash does not need the A+B+reset ritual. Gate G2
   proves whether the partial-flash service answers in that mode.
2. A two-character revision glyph is shown at boot: human-visible proof of which
   build is running, since the Mosaico cannot see the LED matrix.
3. "No pairing required: anyone can connect via Bluetooth" in Project Settings.
   **Unverified:** the exact `pxt.json` key that encodes it. The current
   `"microbit": {"pairing_mode": "none"}` is a placeholder to be confirmed against
   the editor's project settings implementation before the first real build.

## Bootstrap (once, over USB)

A micro:bit v1 running MicroPython has no BLE stack at all ("it only has 16k of
RAM… the BLE stack alone takes 12k"), so it cannot be reached over BLE until a
MakeCode image is on it. One drag-and-drop of a hex built by the **same pinned
toolchain** is the bootstrap step; everything after that is BLE.

## Payload limits

Region 2 is `0x30000–0x3BBFF` = 47.6 KB on a 256 KB part. Pad the last 64-byte
block, do not exceed the region, and remember that the transfer stops at the page
containing the embedded-source magic.
