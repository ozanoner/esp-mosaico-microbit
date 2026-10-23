# micro:bit v1 BLE partial-flash protocol

Everything here was read from source or vendor documentation, not from memory.
Sources are cited per section; anything marked *unverified* has not been confirmed
on hardware yet.

## GATT surface

```
service  E97DD91D-251D-470A-A062-FA1922DFA9A8
char     E97D3B10-251D-470A-A062-FA1922DFA9A8
props    WRITE_WITHOUT_RESPONSE | NOTIFY
```

`codal-microbit-v2/source/bluetooth/MicroBitPartialFlashingService.cpp:46-47,69-72`
creates the characteristic with no security mode, so **no pairing or bonding is
required**. The 20-byte write payload (`1 + 2 + 1 + 16`) exactly fills the default
ATT MTU of 23, so no MTU exchange is needed and none helps.

The service exists only in **MakeCode-built** images: "partial flashing is only
supported by programs built using MakeCode… the service requires the user program
to be appended onto the end of the DAL" (v1 and v2 partial-flash docs).

## Opcodes

| Byte 0 | Command | Payload | Response |
|---|---|---|---|
| `0x00` | REGION_INFO | region id (0=SoftDevice, 1=DAL, 2=PXT) | notify `00 <id> <start:4> <end:4> <hash:8>` |
| `0x01` | WRITE_DATA | `offset hi:2` / `offset lo:2` / packet# / 16 data bytes | notify `01 FF` or `01 AA` |
| `0x02` | END_OF_TX | — | device strips the embedded-source magic and resets |
| `0xEE` | STATUS | — | notify `EE <pfs version> <mode>` (mode 0 = pairing, 1 = application) |
| `0xFF` | RESET | `0x00` pairing mode, `0x01` application | — |

### The address-split quirk (this one bites)

`source/bluetooth/MicroBitPartialFlashingService.cpp:208-218` assembles the 32-bit
target address from the **first two packets of each 4-packet block**, not from
every packet:

- packet 0 → `offset  = (data[1] << 8) | data[2]` (high 16 bits)
- packet 1 → `offset |= (data[1] << 24) | (data[2] << 16)` (low 16 bits)
- packets 0–3 each carry 16 bytes → **64 bytes written per block** at that address

So the host emits contiguous, 4-byte-aligned 64-byte blocks, with a byte-wide
packet counter cycling 0..255. On a mismatch the device notifies `01 AA` and sets
the expected counter to `blockStart + 4`; the host must jump to the next block
(`source/…/MicroBitPartialFlashingService.cpp:188-196`).

## v1 flash map and hashes

`lancaster-university/microbit-dal/source/drivers/MicroBitMemoryMap.cpp:48-56`:

| Region | Contents | Range |
|---|---|---|
| 0 | SoftDevice | `0x00000–0x18000` |
| 1 | DAL | `0x18000–0x30000` |
| 2 | PXT user program | `0x30000–0x3BBFF` (≤ 47.6 KB) |

`MicroBitMemoryMap::findHashes()` scans pages from `0x30000` for the 16-byte magic
`708E3B92C615A841C49866C975EE5197`, then:

```c
memcpy(…memoryMap[1].hash, magicAddress + 4, 8);   // DAL / hex-template hash  (bytes magic+16..23)
memcpy(…memoryMap[2].hash, magicAddress + 6, 8);   // program hash            (bytes magic+24..31)
…memoryMap[2].startAddress = (uint32_t)magicAddress;
```

Consequences for the host:

1. Find the magic in the new hex; the two hashes are the 8 bytes each at
   `magic+16` and `magic+24`.
2. Compare them with REGION_INFO regions 1 and 2. Region 1 mismatch ⇒ the DAL
   differs ⇒ **declare full-flash required**, do not attempt a partial flash.
3. Stream from `magic` up to the page containing the embedded-source magic
   `41140E2FB82FA2BB`, then send `0x02`.

Because region 2's reported hash is the one *declared in the image* rather than a
computed CRC, reading it back after the reboot proves the image (and its header)
landed — it does not prove the program runs.

## Preconditions on the device

- Firmware must be a MakeCode build (DAL present).
- The DAL must be advertising: in pairing mode (hold A+B, tap reset — or triple-tap
  reset), or because the running program started Bluetooth itself.
- If the MakeCode project was built with "No pairing required: anyone can connect
  via Bluetooth", no passkey/pattern step is needed. The exact `pxt.json` key that
  encodes that setting is **unverified** — resolve it when pinning the template.

## Fallback: full flash (out of PoC scope)

v1's DFU service hands the device to the Nordic FOTA bootloader, which speaks
Nordic **legacy DFU** (`0xFE59`). Implementations to port rather than invent:
`cra0/esp32-nordic-ota-dfu` (ESP32 BLE-central updater) and
`infsoft-locaware/nrfdfu` (C implementation).

## Unverified on hardware

- Does the partial-flash service answer while a *running app* owns BLE, rather than
  in pairing mode? (gate G2)
- Is a partial final block accepted when padded with `0xFF` to 64 bytes?
- Measured throughput: 64 B per notification round-trip ⇒ expected ~1.5–3 kB/s,
  i.e. ~10–25 s for a typical 10–30 KB region 2.
