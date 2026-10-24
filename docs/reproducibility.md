# Reproducibility

Goal: `git clone` → `make verify` → same bytes, on any machine with Docker.

## What is pinned, and where

| Thing | Pinned by | File |
|---|---|---|
| MakeCode compiler (`mkc`) | exact npm version `1.3.6`, lockfile, `npm ci` | `build-service/package.json`, `build-service/package-lock.json` |
| MakeCode editor worker | baked into the image at build time (`/root/.pxt`) | `build-service/Dockerfile` |
| Python dependencies | frozen lock, `uv sync --frozen` | `build-service/uv.lock` |
| Node base image | digest | `build-service/Dockerfile` |
| ESP-IDF toolchain | image digest | `compose.yaml` |
| esp-mosaico tooling + BSP | git submodule commits | `.gitmodules` (gitlinks in `main`) |
| Expected build output | golden hashes | `microbit/golden/expected.json` |

`scripts/verify.sh` is the contract: unit tests, rebuild `payloads/hello.ts`, then
compare the template hash, program hash, region-2 range, block count and the
sha256 of the region-2 bytes against the golden file. Any drift fails the check
with the exact field that moved.

## Housekeeping

Containers run as root, so the artefacts they write into the checkout
(`build/`, `__pycache__/`, `*.egg-info/`) are root-owned. They are all
gitignored, and `make clean` removes them from inside a container:

```sh
make clean
```

`docker compose down` stops the services; named volumes (`idf-ccache`,
`esp-iris-state`) survive on purpose.

## What is deliberately *not* pinned

- **Network at image build time.** `npm ci`, `uv sync` and the mkc warm-up build
  all reach the internet once, when the image is built. That is the moment
  versions are resolved; the runtime container builds payloads offline.
- **The ESP-Iris host runtime** for `mosaico.py`. It is bootstrapped into the
  named volume `esp-iris-state` on first use and will fetch what it needs.
- **Where the BLE radio is.** Flashing runs on cirunner against a physical
  micro:bit, which is hardware, not something a container can pin.

## Refreshing the pin (deliberate upgrade)

1. Change the version in `build-service/package.json`, run `npm install --package-lock-only`.
2. `make verify` — it will fail and print the new hashes.
3. Update `microbit/golden/expected.json` **and** `build-service/TOOLCHAIN.md` in the same
   commit, and say in the message that the toolchain moved.
4. Re-bootstrap the micro:bit over USB: a new DAL means partial flashing will
   (correctly) refuse the old device state.

## Why the golden file compares bytes, not just the hex file

The `.hex` carries PXT extension records (`0A`/`0C`/`0D`) whose ordering is not
part of the payload. The check that matters for the flasher is the region-2 byte
stream, so that is what is hashed.
