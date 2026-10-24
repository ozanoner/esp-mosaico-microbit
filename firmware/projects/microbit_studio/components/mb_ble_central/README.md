# mb_ble_central

NimBLE **central** for the micro:bit v1 partial-flash service. This component is
the gate-G2 probe (issue #3): it proves the radio, the GATT surface and the
STATUS handshake before any flash logic exists.

The wire facts — service `E97DD91D-…`, characteristic `E97D3B10-…`,
`WRITE_WITHOUT_RESPONSE | NOTIFY`, no security mode, 20-byte write payload at the
default ATT MTU — are all documented with sources in
`docs/partial-flash-protocol.md`. Nothing here is derived from memory.

## Flow

```
scan (name prefix "BBC micro:bit") → connect → discover characteristics
  → find E97D3B10-… → discover its descriptors → subscribe (CCCD 0x0001)
  → write 0xEE → log notify → disconnect → scan again
```

The service UUID is **not** in the 31-byte advertising payload, so the name is
the only handle: matching is a prefix compare on `BBC micro:bit`, which tolerates
the per-board `[abcde]` suffix.

The loop is deliberate. The acceptance criterion is `0xEE` answered *twice in a
row without a reboot*, so a completed round tears the connection down and rescans
instead of holding it.

## Threading and ownership

`mb_ble_central_start()` creates one short-lived task that initialises the host
and deletes itself; the NimBLE host then runs on its own FreeRTOS task. The
caller keeps its own task free — on this board that caller is the GSP/UI owner,
and blocking it stops the panel.

The call returns as soon as the task exists. Every outcome (peer found,
connected, subscribed, status answered, error) arrives on the UART log, never as
a return value. A second call returns `ESP_ERR_INVALID_STATE`; there is no stop
path in the PoC, because the probe runs until reboot.

## Testability

`include/mb_status_parse.h` and `mb_status_parse.c` are free of ESP-IDF headers:
the same object is compiled into the firmware and into the host test under
`host/` (`cmake -S host -B host/build && cmake --build host/build && ctest
--test-dir host/build`). That test is what pins the `EE <pfs> <mode>` shape; the
BLE call sequence itself can only be proven on hardware.

## Memory

NimBLE is the first component to claim internal RAM on a board that is already
tight: the CO5300 panel bounces ~77 KB of internal DMA buffers while rendering
(`esp-mosaico-devkit` → internal RAM notes). `mb_mem_probe_stage("ble-up")` is
called once the stack is running so the log records free/largest internal RAM
after BLE comes up; `largest`, not total, is the number that decides whether the
next flush fails.
