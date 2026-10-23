# Pinned toolchain

Whatever produces images must not move. These are the values this repository was
validated against; re-record them (and re-bootstrap the micro:bit) if they change.

| | |
|---|---|
| `mkc` (`makecode` npm package) | **1.3.6** |
| host | node 22.23.1, Linux x86_64 |
| target | `microbit` (v1, nRF51822) |
| template dir | `microbit/template` |

## Measured on the two PoC payloads

| payload | template hash | program hash | region 2 | blocks |
|---|---|---|---|---|
| `payloads/hello.ts` (rev `A1`) | `4cc31a5b0ac4adb3` | `f200340042003f00` | `0x37400–0x38BFF` (6144 B) | 96 |
| `payloads/dice.ts` (rev `B2`) | `4cc31a5b0ac4adb3` | `4b004a007d001800` | `0x37400–0x383FF` (4096 B) | 64 |

Two things this proves:

1. **The template hash is stable** across different programs — which is the value
   the partial-flash service compares before accepting an image.
2. **The program hash moves** with the program, so a successful flash is
   distinguishable from a stale one.

## Two facts that contradict the documentation and matter

- **Region 2 does not start at `0x30000`.** `MicroBitMemoryMap.cpp` documents
  region 1 (DAL) as `0x18000–0x30000` and region 2 as `0x30000–0x3BBFF`, but the
  DAL shipped by mkc 1.3.6 ends at **`0x37400`**, and that is where the magic
  lands. The magic address must be read from the image, never hardcoded — the
  DAL's own `findHashes()` computes `FLASH_PROGRAM_END` the same way.
- **The "program hash" is not a hash of the file.** It is an opaque 8-byte value
  stored after the magic; it changes when the program changes, and that is the
  only property the protocol relies on. Do not "verify" it by hashing bytes.

## Reproducing

```sh
source .venv/bin/activate
MB_TOOLCHAIN_ID=mkc-1.3.6 python -m esp_mb_brain.builder microbit/payloads/hello.ts -o build/hello.hex
python -m hexkit inspect build/hello.hex
```

If either hash differs from the table above without a deliberate `mkc` upgrade,
the build box has drifted. Never run `mkc --update`.
