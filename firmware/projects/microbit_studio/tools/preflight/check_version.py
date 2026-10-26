#!/usr/bin/env python3
"""Fail when the build reports an application version that is not the source line.

The `project(microbit_studio VERSION x.y.z)` line in this project is what ESP-Iris
reads: IDF copies it into the app description, which is the `app_version` a device
answers with, and `system_update.cmake` passes the same value to the bundle manifest
as `release`. Two ways for that to go wrong, both silent until a device reports a
version nobody recognises:

  * a build directory configured before the version moved keeps the old number;
  * a reintroduced `CONFIG_APP_PROJECT_VER` overrides the CMake value in Kconfig,
    where IDF's own help text says other ways to set PROJECT_VER are ignored.

So this compares the built description against the line, and refuses to let an
explicit Kconfig override exist at all.
"""

import importlib.util
import json
import pathlib
import re
import sys

PROJECT_DIR = pathlib.Path(__file__).resolve().parents[2]
REPO = PROJECT_DIR.parents[2]
DESCRIPTION = PROJECT_DIR / "build/project_description.json"
SDKCONFIG_H = PROJECT_DIR / "build/config/sdkconfig.h"
OVERRIDE = "CONFIG_APP_PROJECT_VER_FROM_CONFIG"


def source_version() -> str:
    """The one source line, parsed by the release tool so there is one parser."""
    spec = importlib.util.spec_from_file_location("release_version", REPO / "tools/release/version.py")
    if spec is None or spec.loader is None:
        raise SystemExit(f"check_version: cannot load {REPO / 'tools/release/version.py'}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.read_version()


def main() -> int:
    if not DESCRIPTION.exists():
        print(f"check_version: {DESCRIPTION} missing, build once first", file=sys.stderr)
        return 1

    expected = source_version()
    built = json.loads(DESCRIPTION.read_text()).get("project_version", "")
    print(f"source line:   {expected}")
    print(f"build reports: {built}")

    problems = []
    if built != expected:
        problems.append(
            f"the build reports {built or '(nothing)'} but the source line says {expected}. "
            "The build directory was configured before the version moved: delete "
            "build/sdkconfig and rebuild."
        )

    if SDKCONFIG_H.exists():
        match = re.search(OVERRIDE + r" 1", SDKCONFIG_H.read_text())
        if match:
            problems.append(
                f"{OVERRIDE} is set in build/config/sdkconfig.h, so Kconfig wins over the "
                "CMake version and the app reports whatever CONFIG_APP_PROJECT_VER says. "
                "Remove it from sdkconfig.defaults, delete build/sdkconfig and rebuild."
            )
    elif built:
        problems.append(f"{SDKCONFIG_H} missing, so the override could not be checked; build once")

    for problem in problems:
        print(f"check_version: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
