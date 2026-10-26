# Changelog

Generated from conventional commits by `tools/release/version.py`. Newest first.
A section heading is the release tag without the `v`, which is also the version
the firmware reports and the value ESP-Iris records as `release` in a system
update manifest.

## [0.1.0] - 2026-10-26

_First tagged release, so this covers the whole history._

### Features
- **firmware:** studio UI — eight-state panel and a working speech-to-flash pipeline (issue #7) (#17)
- **build-service:** /v1/speak — speech in, compiled program out (issue #6) (#13)
- **firmware:** partial-flash a micro:bit over BLE (gate G3) (#12)
- **firmware:** BLE central that speaks the micro:bit partial-flash protocol (gate G2) (#11)
- **build-service:** move the service to port 8477
- **microbit:** build payloads with an open BLE link
- **build-service:** pin mkc 1.3.6, tolerate PXT hex records, derive region-2 from the image

### Fixes
- **firmware:** leave image acceptance to iris_ota_support, as hello_world does (#20)

### Refactoring
- name the LAN service build-service

### Tests
- **poc:** generated image runs on the micro:bit end to end (gate G4) (#14)

### CI
- **release:** reproducibility gate, native versioning and one docs style (issue #9) (#21)

### Chores
- drop the duplicate bootstrap entry point
- load credentials from a gitignored .env
- per-checkout compose project and container-safe clean
- make the system reproducible from a clone
- scaffold monorepo with brain, hexkit, firmware workspace and DevContainer
