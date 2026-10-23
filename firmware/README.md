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

Device operations go through `mosaico.py` only — no `idf.py flash`, no `esptool`
offsets. The application lives in `main_app` (`app/ota_0`); the retained Vibe Mode
firmware is untouched, so `recover` keeps working.

## Build environment: DevContainer only

ESP-IDF is never installed on a host. Use the repository DevContainer, which
reuses the ESP-IDF 6.2 image already present on the machine:

```sh
docker compose -f .devcontainer/docker-compose.yml up -d
docker compose -f .devcontainer/docker-compose.yml exec esp-idf bash
cd firmware && python3 mosaico.py doctor
```

`IDF_IMAGE=<repo>:<tag> docker compose …` overrides the image if a build has to be
pinned.

### Measured facts for the current image

`espressif/idf:latest` (IDF master, revision `ab95e70748f2`, 2026-10-12), from
`python3 mosaico.py doctor` inside the container:

| Check | Result |
|---|---|
| `idf-version` | **6.2.0** (required ≥ 6.2) — pass |
| `idf-target` | `esp32s31` available — pass |
| `idf-revision` | `ab95e70748f2` — pass (not enforced) |
| `esp-iris` | fails until the ESP-Iris host runtime is bootstrapped into the named volume |

⚠️ Upstream documents ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`
("`v6.2-dev-2221-g7b9cc1ac79f`") as the tested revision. The image carries a newer
6.2 master and the doctor accepts it, but "doctor passes" is not "upstream tested
this revision". If a build or device behaviour looks unexplained, pin the image to
a tag at or below that revision before blaming our code.

## Application

`projects/microbit_studio/` — created with the upstream template (names are
letters, digits and underscores only; hyphens are rejected):

```sh
git submodule update --init submodule/esp-mosaico-utils
python3 mosaico.py project init microbit_studio
```

Components to add on top of the template:

| Component | Purpose |
|---|---|
| `mb_payload` | Intel HEX → region-2 64-byte blocks (C port of `tools/hexkit`) |
| `mb_flash` | NimBLE central + partial-flash client |
| `brain_client` | HTTPS to the brain service, TLS buffers in PSRAM |
| `app_studio` | state machine + GSP scene |

`project init` initialises an independent git repository inside
`projects/<name>`; this monorepo tracks the application as ordinary files, so that
nested `.git` directory is removed after creation.
