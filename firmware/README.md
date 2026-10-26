# ESP-Mosaico firmware

This directory **is** a `esp-mosaico-vibe` workspace: `mosaico.py` here is the
upstream launcher, verbatim (18 lines, from `esp-mosaico-vibe/mosaico.py`), and
`submodule/` holds the pinned tooling the launcher needs. Keeping the workspace
shape is what makes the sanctioned device path work:

```sh
python3 mosaico.py recover
python3 mosaico.py iris system-update --project projects/microbit_studio
python3 mosaico.py iris app-update   --project projects/microbit_studio
```

Device operations go through `mosaico.py` only: no `idf.py flash`, no `esptool`
offsets. The application lives in `main_app` (`app/ota_0`); the retained Vibe Mode
firmware is untouched, so `recover` keeps working.

## Build environment: DevContainer only

ESP-IDF is never installed on a host. The repository provides it as a container
service, pinned by image digest:

```sh
docker compose up -d mosaico-idf
docker compose exec mosaico-idf bash -lc 'cd firmware && python3 mosaico.py doctor'
make firmware            # same thing, drops you into a shell
```

`IDF_IMAGE=<repo>@<digest>` overrides the pin if a build has to move.

### Measured facts for the current image

`espressif/idf@sha256:6ad927cf…`, the digest `compose.yaml` pins, carries IDF master
at revision `ab95e70748f2` (2026-10-12). Measured from `python3 mosaico.py doctor`
inside that container:

| Check | Result |
|---|---|
| `idf-version` | **6.2.0** (required ≥ 6.2), pass |
| `idf-target` | `esp32s31` available, pass |
| `idf-revision` | `ab95e70748f2`, pass (not enforced) |
| `esp-iris` | fails until the ESP-Iris host runtime is bootstrapped into the named volume |

⚠️ Upstream documents ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`
("`v6.2-dev-2221-g7b9cc1ac79f`") as the tested revision. The image carries a newer
6.2 master and the doctor accepts it, but "doctor passes" is not "upstream tested
this revision". If a build or device behaviour looks unexplained, pin the image to
a tag at or below that revision before blaming our code.

## Application

`projects/microbit_studio/`, created with the upstream template (names are
letters, digits and underscores only; hyphens are rejected):

```sh
git submodule update --init submodule/esp-mosaico-utils
python3 mosaico.py project init microbit_studio
```

Components to add on top of the template:

| Component | Purpose |
|---|---|
| `mb_voice` | push-to-talk capture, `POST /v1/speak` |
| `mb_build_client` | `POST /v1/build?region2=true`; TLS buffers and the image in PSRAM |
| `mb_payload` | Intel HEX → region-2 64-byte blocks (C port of `tools/hexkit`) |
| `mb_flash` | the partial-flash packet codec (`REGION_INFO`, `WRITE_DATA`, `END_OF_TX`) |
| `mb_ble_central` | NimBLE central: scan, connect, CCCD, notify-paced writes, verify |
| `app_display` | BSP panel and screen mirroring |
| `app_studio` | state machine + GSP scene |
| `mb_event` | the event type the pipeline emits |

`project init` initialises an independent git repository inside
`projects/<name>`; this monorepo tracks the application as ordinary files, so that
nested `.git` directory is removed after creation.
