#!/usr/bin/env python3
"""Fail the build if the scene outgrows what boots reliably.

A 34-frame animation in the startup set decoded 170 KiB at boot and tripped the
interrupt watchdog, so the panel rebooted once after every install. The budget
below is the one the panel has actually survived.
"""

import glob
import pathlib
import re
import subprocess
import sys
import tempfile

BASE = pathlib.Path(__file__).resolve().parents[2]
SCENE = BASE / "components/app_studio/ui/main.json"

ANIMATION_PEAK_LIMIT = 96 * 1024      # decoded animation RAM at boot
BUNDLE_LIMIT = 512 * 1024             # packed bundle in flash


def find_gspc():
    """gspc lives in whichever cache the workspace that fetched it used."""
    patterns = [
        str(pathlib.Path.home() / ".cache/esp-mosaico/gspc/*/gspc"),
        "/root/.cache/esp-mosaico/gspc/*/gspc",
        "/home/*/.hermes/profiles/*/home/.cache/esp-mosaico/gspc/*/gspc",
    ]
    candidates = []
    for pat in patterns:
        candidates += glob.glob(pat)
    return sorted(candidates)[-1] if candidates else None


def main() -> int:
    gspc = find_gspc()
    if gspc is None:
        print("check_bundle: gspc not found (run any sim/build once to fetch it)",
              file=sys.stderr)
        return 1
    with tempfile.NamedTemporaryFile(suffix=".gspb") as out:
        proc = subprocess.run(
            [gspc, "pack", str(SCENE), "--pixel-format", "rgb565", "--deployable",
             "--image-cache-bytes", str(4 * 1024 * 1024), "-o", out.name],
            capture_output=True, text=True, cwd=str(SCENE.parent))
    log = proc.stdout + proc.stderr
    if proc.returncode != 0:
        print("check_bundle: scene does not compile", file=sys.stderr)
        print("\n".join(log.splitlines()[-12:]), file=sys.stderr)
        return 1

    def field(name):
        m = re.search(name + r"=(\d+)", log)
        return int(m.group(1)) if m else None

    anim = field("animation_frame_peak_bytes")
    bundle = field("bundle_bytes")
    problems = []
    if anim is None:
        problems.append("could not read the animation RAM figure")
    elif anim > ANIMATION_PEAK_LIMIT:
        problems.append(f"animation peak {anim // 1024} KiB > {ANIMATION_PEAK_LIMIT // 1024} KiB budget")
    if bundle is None:
        problems.append("could not read the bundle size")
    elif bundle > BUNDLE_LIMIT:
        problems.append(f"bundle {bundle // 1024} KiB > {BUNDLE_LIMIT // 1024} KiB budget")

    if anim is not None:
        print(f"animation peak : {anim // 1024} KiB (limit {ANIMATION_PEAK_LIMIT // 1024})")
    if bundle is not None:
        print(f"bundle flash   : {bundle // 1024} KiB (limit {BUNDLE_LIMIT // 1024})")
    for p in problems:
        print(f"BAD BUDGET: {p}")
    print(f"check_bundle: {'PASS' if not problems else f'FAIL ({len(problems)})'}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
