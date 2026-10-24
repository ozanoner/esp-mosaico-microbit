# Local patches to the pinned vendor tools

Applied to the `esp-mosaico-utils` submodule at the pinned revision
(`be151ae047014e79a9c7ae0092dd203844499003`), never committed there. Apply with:

```sh
make patch-tools          # idempotent; also a prerequisite of make firmware[-hw]
```

`git -C firmware/submodule/esp-mosaico-utils diff` shows exactly what is applied;
`git -C firmware/submodule/esp-mosaico-utils apply -R patches/…` removes it.

## 0001 — ROM Download Mode is never discovered

**Symptom.** On a board that is not running Vibe Mode, `mosaico.py recover` fails
with:

```
No ESP-Mosaico ROM Download Mode interface was detected. Power off the device,
hold the Boot button ... and retry recover.
```

even though the board *is* in ROM Download Mode and the host sees it
(`/dev/serial/by-id/usb-Espressif_ESP32-S31_<mac>-if00`, `303A:0020`). No button
sequence helps: the interface is filtered out of the list the command inspects.

**Cause — two independent breaks in the vendor revision.**

1. `recover` builds its candidates from `esp_iris.py doctor --json`
   (`mosaico-tools/tools/mosaico_cli/recovery.py:333-339`) and keeps entries whose
   `transport` is one of `usb_serial_jtag`, `serial_jtag`, `rom` (`:397`). But
   `doctor` enumerates with `discover_iris_usb_devices(include_usb_serial_jtag=True)`
   — `include_rom` is never passed (`ESP-Iris/.../iris_gateway/cli.py:896`).
2. The ROM branch of the discovery is gated on that flag
   (`iris_gateway/discovery.py:65`: `is_rom = include_rom and vid == 0x303A and pid == 0x0020`),
   and even when it matches, the record is labelled `transport="usb"`
   (`discovery.py:79-81`), which the filter above rejects.

Net effect: a `303A:0020` device is dropped by every branch — not `rom`, not
`usb_serial_jtag` (`0x1001`), not `uart` (that branch excludes `0x303A`), not
`is_iris_cdc` (`0x4002`).

**Fix.** Pass `include_rom=True` when `doctor` enumerates, and give the ROM
branch its own `transport` label. Two lines, no behavioural change for any other
device class.

**Verified.** With the patch, `discover_iris_usb_devices(include_rom=True)`
returns the board as `303A:0020`, `serial_number` = the eFuse MAC,
`transport='rom'` — i.e. it lands in `recovery.py`'s candidate set.

**Remove this patch when** the pinned submodule reaches a revision that fixes
both sites; check `git -C firmware/submodule/esp-mosaico-utils log -1` after any
bump, and re-run `make patch-tools` — it reports "already applied" instead of
failing if upstream has fixed it in a compatible way.
