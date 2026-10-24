# mb_ble_central

NimBLE **central** for the micro:bit v1 partial-flash service — the firmware side
of gates G2 and G3.

## What it does, in order

1. Scans with `filter_duplicates` and connects to the first advertiser whose name
   starts with `BBC micro:bit`; MTU stays at the default 23, which is exactly the
   20-byte packet size the protocol uses.
2. Discovers the service `E97DD91D-251D-470A-A062-FA1922DFA9A8`, its
   characteristic `E97D3B10-…`, and the CCCD under it, then subscribes.
3. **Until a transfer has completed:** reads region 1 (DAL) and region 2
   (program) hashes, refuses to write if the DAL differs from our image, streams
   region 2 in 64-byte blocks, sends `END_OF_TX`, and disconnects. The device
   resets itself, which drops the link.
4. **Afterwards:** writes `0xEE` and logs the STATUS answer — that is the gate G2
   probe, kept as the steady state so the log stays readable after a flash.

Each round ends in a disconnect and a fresh scan, so "twice without a reboot" is
the normal behaviour rather than a test case.

## Pacing

There is no task, semaphore or timer: the device notifies once per 64-byte block,
and the next block is sent from the notification callback. The acknowledgement is
the pacing. A block that is never acknowledged stalls the transfer in place,
which is visible in the log as a missing `flashing N/64` line rather than as
corrupted flash.

## Refusing to write

`REGION_INFO` for region 1 returns the DAL hash. If it differs from our image's
`template_hash`, the firmware logs `DAL hash differs from our image: refusing to
write` and disconnects. Streaming a mismatched DAL produces an image the device
would refuse anyway, but only after writing 64 blocks — refusing first keeps the
failure at "we did not write".

## Resync

`01 AA` means the device saw an out-of-order packet counter. The transfer skips to
the next block and resumes at the boundary the device advanced to; more than four
resyncs abandons the transfer instead of thrashing. With a well-behaved
`write_no_rsp` path this should never fire, so a resync in the log is worth
reading rather than ignoring.

## Memory

NimBLE costs about 20 KB of internal RAM here: `[MEM ble-up] free=170135
largest=90112 low=154123`. The largest contiguous block matters more than the
total, because the panel's QSPI path takes a temporary internal DMA bounce on
flush; if that block ever falls below ~77 KB the screen stops updating while
everything else keeps working. `mb_mem_probe` exists to catch exactly that.

## Threading

Everything runs on the NimBLE host task: the GAP event callback and the
notification callback are the only writers of the transfer state, so there are no
locks. `mb_ble_central_start()` creates one task that initialises NimBLE and
returns; nothing else touches the radio.
