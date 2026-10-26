# mb_flash

The micro:bit v1 partial-flash protocol, in two halves: pure packet building and
notification parsing (`mb_flash.c`, host-tested), and the embedded image the
device streams.

## What the device actually sends

Only **region 2**, the user program. The SoftDevice and the DAL are already on
the micro:bit and are never transmitted, so the fixture is a few kilobytes
instead of a 1.3 MB hex. Region 2 for the dice build is 4,096 bytes: 64 blocks.

`mb_fixture.bin` is committed, not generated at build time, so the firmware build
needs neither the toolchain nor the network. `scripts/extract_region2.py`
reproduces it from a mkc build, and `--check` fails if the bytes move:

```sh
docker compose run --rm build-service build microbit/payloads/dice.ts -o build/dice.hex
python3 scripts/extract_region2.py build/dice.hex --check
```

The generated header pins the geometry and both hashes, so a toolchain change is
a build-time mismatch rather than a mysterious refusal on the device.

## Wire format

One 20-byte packet per ATT write, exactly the default MTU payload, so no MTU
exchange is needed:

```
| 1 byte  | 2 bytes | 1 byte  | 16 bytes |
| COMMAND | OFFSET  | PACKET# | DATA     |
```

Four packets make one 64-byte block. The device notifies once per block
(`01 FF`), or `01 AA` when the packet counter is out of order, and then expects
the next block to start at the boundary it advanced to.

Three details here come from `MicroBitPartialFlashingService.cpp` directly, and
two of them contradict earlier notes in this repo:

1. **The address is split across packets 0 and 1, low half first.** Packet 0
   carries bits 0-15, packet 1 bits 16-31 (`case 0` / `case 1`). Region 2 at
   `0x37400` therefore goes out as `74 00` then `00 03`.
2. **`PACKET#` is transfer-wide.** The device compares it against a counter that
   increments for every accepted packet and never restarts at a block boundary
   (`packetCount++`), so the caller stamps a running value. Restarting at 0 per
   block makes the device answer `0xAA` from the second block on.
3. **`END_OF_TX` writes once more.** The device burns 16 bytes at its current
   offset with whatever is left in its block buffer, strips the embedded-source
   magic, then resets, which is what drops the BLE connection.

## Testing

`host/test_flash.c` asserts the packet layout byte by byte, including
re-assembling `0x37400` through the DAL's own expression, and covers `01 FF`,
`01 AA`, REGION_INFO, STATUS, and short/unknown payloads.

```sh
cd host && cc -std=c11 -Wall -Wextra -Werror -I../include test_flash.c ../mb_flash.c -o t && ./t
```

`mb_flash.c` has no ESP-IDF dependency on purpose; `mb_flash_fixture.c` is the
only file that touches the embedded symbols.
