# Bring-up gates

Four checks that decide whether the architecture holds. No protocol code is
written until G1–G3 pass; G4 is the PoC.

## G1 — the build box can be trusted

**Question:** does a pinned `mkc` install produce a hex whose DAL hash matches a
reference hex built by the web editor at the same version?

**How**

```sh
node --version                                  # 20+
npm install -g makecode
python -m esp_mb_brain.builder microbit/payloads/hello.ts -o build/hello.hex
python -m hexkit inspect build/hello.hex        # prints magic address + hashes
```

**Pass:** magic found at `0x30000`, two 8-byte hashes printed, region 2 inside
`0x30000–0x3BBFF`, and the same hashes as a hex built from the same MakeCode
version in the browser. **Fail ⇒ stop:** without a stable DAL hash the flasher
refuses every image it is given.

## G2 — the service answers outside pairing mode

**Question:** with a *running* MakeCode app that started Bluetooth, does the
partial-flash service respond, or is pairing mode required every time?

**How:** flash a program containing `bluetooth.startUartService()`, connect, write
`0xEE`, expect `EE 01 01`. Then repeat against a board in pairing mode and compare.

**Pass:** `0xEE` answered in application mode ⇒ the kid never touches the board.
**Fail:** the flow keeps a physical step (hold A+B, tap reset), surfaced on the
Mosaico's screen — still shippable, but the UX changes and the spec must say so.

## G3 — hashes agree

**Question:** do REGION_INFO regions 1/2 from the device equal the two hashes
extracted from our hex?

**Pass:** equal ⇒ partial flash is legal. Not equal ⇒ the device needs a
full-flash/bootstrap to our pinned toolchain first (one USB flash).

## G4 — PoC: generated image runs

Flash `build/hello.hex` end to end, then reconnect and compare region 2 against
`program_hash`.

**Pass:** hash matches after reboot **and** the 2-character revision glyph from the
new program is visible on the LED matrix. Hardware acceptance is a human step: the
Mosaico cannot see the micro:bit's LEDs.

## Reference evidence to capture per run

- `GET /v1/health` (toolchain id)
- the build object (hashes, sha256)
- BLE log: blocks sent, any `01 AA`, bytes/sec
- post-flash REGION_INFO region 2
