# ESP-Mosaico microbit_studio

Created with `python mosaico.py project init my_app`.
The same GSP 1.5.1 scene and portable C UI run on PC and ESP-Mosaico.

From the workspace root:

```sh
python mosaico.py project sim --project projects/microbit_studio --interactive
python mosaico.py recover
python mosaico.py iris system-update --project projects/microbit_studio
```

Run `recover` before the first install on a blank or unverified device.
Use `system-update` for new apps or changed resources/layouts. Use `app-update`
only for code changes with an identical full partition table and resources.

Application components:

- `components/app_studio`: eight-state controller and the studio scene. Two
  columns: messages on the left, the bear on the right; five-segment flash
  progress; trophy clip on DONE. Its README carries the state table.
- `components/mb_event`: header-only `mb_event_t` contract that `mb_voice`,
  `mb_build_client` and `mb_ble_central` report through, so no pipeline
  component depends on the UI.
- `components/app_display`: BSP-to-GSP display and optional touch policy.
- `main`: device startup and dependency composition only.
- `components/app_studio/host`: desktop adapter and its separate CMake build,
  reusing the same controller and public header; excluded from the ESP32 build.
  `STUDIO_SIM_STATE=<IDLE|LISTENING|BUILDING|AWAIT_MICROBIT|FLASHING|VERIFYING|DONE|FAILED>`
  renders one exact screen headlessly.
- `components/app_studio/ui/main.json`: authored scene; generated bindings and
  bundles stay in the build. `tmp/build_scene.py` regenerates it from the hello
  chrome in git, with explicit parent remapping.

`project sim` discovers a unique scene under `components/*/ui/main.json` (or the
older root `ui/main.json`). With multiple scenes, select one using `--scene PATH`,
relative to the project. This previews only that scene; omit `--scene` to run the
PC backend and its compiled application behavior.

Dependency direction is `main -> app_hello -> ESP-GSP` and
`main -> app_display -> BSP/ESP-GSP`. Neither application component depends on
`main` or on its sibling. Public headers live in `include/`; private state stays
in implementation files. Each component README includes its contract and usage.

When adding behavior, keep `main` as a composition root. Add cohesive `app_*`
components only for independent responsibilities, declare public/private CMake
dependencies explicitly, and keep the graph acyclic. Avoid cross-module globals;
pass handles, context and value snapshots. Keep each public API comment within
three lines, describing its purpose and key preconditions/results. Put detailed
ownership, lifecycle, threading and timeout contracts in the component README;
document units and ranges beside fields.
Third-party dependencies retain upstream names. Pins and workspace overrides live
in `main/idf_component.yml` and the root CMake configuration. Apply local patches
in a dedicated dependency checkout and select it with `override_path`; never edit
`managed_components/` in place.

Git, editor and hook entry files stay at the root; format rules, project policy
and setup instructions live in `tools/quality/`. See its README to enable hooks.
The `tools/quality/policy.json` prefix rule checks local application component names.
Architecture and API semantics also require code review; formatter/static-analysis
success alone does not establish those contracts.

## Bench configuration is not in the repo

`CONFIG_MB_WIFI_SSID`, `CONFIG_MB_WIFI_PASS` and `CONFIG_MB_BUILD_SERVICE_URL`
live in the **generated** `build/sdkconfig`, never in the tracked defaults: the
first two are secrets, the last is bench-dependent. A build that skips this step
compiles with an empty SSID and a `127.0.0.1` service URL, so the panel reports
"cannot reach the build box" while the service is perfectly healthy. Inject them
before building, then rebuild so the values are compiled in.

## Testing before hardware

`tools/preflight/preflight.sh` is the gate: nothing gets installed on a panel
until it passes. It needs no board, no microphone and no button.

| Step | Catches |
|---|---|
| state machine (host, 15 cases) | wrong transitions, silent wiring, stale events repainting a finished screen, unmapped failure sentences |
| text fit | any string that would clip its column, measured with the real TTF |
| scene budget | a scene whose startup decode could trip the watchdog (animation peak, bundle size) |
| bench configuration | an empty SSID or a `127.0.0.1` service URL, the build that says "cannot reach the build box" |
| build service (audio) | ASR → LLM → `mkc` → region-2 bytes, replayed with fixtures instead of a person speaking |
| application version (`WITH_BUILD=1`) | a build that reports a version other than the `project(… VERSION …)` line, or a `CONFIG_APP_PROJECT_VER` override that outranks it. This is the number ESP-Iris reads as `app_version` |

Audio fixtures are generated once with `piper` and committed under
`tools/preflight/fixtures/` (`command.wav`, `short.wav`, `silence.wav`).

For a run with nobody present, configure `CONFIG_MB_VOICE_BENCH_WAV=y`: the firmware
then posts `components/mb_voice/bench/command.wav` through the real pipeline about
5 s after boot, so `LISTENING → … → DONE` can be captured from `iris screenshot`
without a button press. Never enable it in a shipped build.
