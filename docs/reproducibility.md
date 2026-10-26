# Reproducibility

Goal: `git clone` → `make verify` → same bytes, on any machine with Docker.

## What is pinned, and where

| Thing | Pinned by | File |
|---|---|---|
| MakeCode compiler (`mkc`) | exact npm version `1.3.6`, lockfile, `npm ci` | `build-service/package.json`, `build-service/package-lock.json` |
| MakeCode editor worker | downloaded from the CDN at **image-build time** and frozen into the image (`/root/.pxt`) — not version-pinned, so it is only as fixed as the last image build | `build-service/Dockerfile` |
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
- **Where the BLE radio is.** Flashing goes through `mosaico.py` in the
  `mosaico-idf` container and out over the air to a physical micro:bit. That is
  hardware, not something a container can pin.

## Release checklist

The gate runs at the release, not on every push: PR feedback stays fast, and a cold
Docker build on each push buys little. The trigger is
`.github/workflows/verify-release.yml` — `release: types: [published]` and
`workflow_dispatch` only, deliberately no `push` and no `pull_request`.

Before a tag, in this order:

1. **Hardware test on a real micro:bit.** The gate is build-side only (G1); it
   cannot see the board. Confirm G2 (the partial-flash service answers `0xEE` in
   application mode), G3 (`REGION_INFO` regions 1/2 equal the hashes from our hex)
   and G4 (a generated image runs, and region 2 still matches after the reboot).
   The revision glyph on the LED matrix is a human check — nothing on the host can
   read it. See [bring-up gates](bringup-gates.md).
2. **Bump the version and tag it in one command:**
   `tools/release/version.py bump minor --tag --push`. It raises the
   `project(microbit_studio VERSION …)` line, writes the changelog section from the
   commits since the last tag, commits `chore(release): x.y.z` and creates the
   annotated tag `vx.y.z`. For a first release of a version that is already in the
   tree, `tools/release/version.py tag` does the same without the bump.
3. **Publish the release** from that tag: `gh release create vx.y.z
   --notes-from-tag` reuses the tag annotation, which is the changelog section.
   Publishing runs the job: cold build-service image, then `verify` (unit tests,
   rebuild `hello.ts`, compare against the golden file), then the same `verify`
   again inside `--network none` so an accidental runtime download fails it.
4. **Read the job output.** It uploads `build/hello.hex` and the verify log, and a
   failure names the field that moved — `template_hash`, `program_hash`, region
   geometry or the region-2 bytes — so the fix is either a real regression or a
   deliberate pin refresh (below), never a quietly updated golden file.

The version lives in one line: `project(microbit_studio VERSION …)` in
`firmware/projects/microbit_studio/CMakeLists.txt`, because that is the line ESP-Iris
reads. IDF copies it into the app description, which is the `app_version` a device
reports, and `system_update.cmake` passes the same value to the system update
manifest as `release`. The release tag is `v` plus that number, this gate fails a
published release whose tag is anything else, and
`tools/preflight/preflight.sh` with `WITH_BUILD=1` fails a build that reports a
different number, which is how a stale `build/sdkconfig` gets caught.

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
