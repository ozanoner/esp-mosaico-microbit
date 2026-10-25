#!/usr/bin/env python3
"""Fail the build if the firmware was configured without bench networking.

An empty SSID and a 127.0.0.1 service URL compile happily and then make the panel
claim "cannot reach the build box" while the service is healthy — which cost a
whole evening once. Secrets are only ever reported as set/unset and a length.
"""

import pathlib
import re
import sys

BASE = pathlib.Path(__file__).resolve().parents[2]
SDKCONFIG_H = BASE / "build/config/sdkconfig.h"

REQUIRED_STRINGS = ("CONFIG_MB_WIFI_SSID", "CONFIG_MB_WIFI_PASS")
REQUIRED_URLS = ("CONFIG_MB_BUILD_SERVICE_URL", "CONFIG_MB_VOICE_SERVICE_URL")
LOOPBACK = ("127.0.0.1", "localhost", "0.0.0.0")


def value(text: str, key: str):
    m = re.search(key + r' "([^"]*)"', text)
    return m.group(1) if m else None


def main() -> int:
    if not SDKCONFIG_H.exists():
        print(f"check_config: {SDKCONFIG_H} missing — build once first", file=sys.stderr)
        return 1
    text = SDKCONFIG_H.read_text()
    problems = []

    for key in REQUIRED_STRINGS:
        v = value(text, key)
        if not v:
            problems.append(f"{key} is empty (device will not join WiFi)")
        else:
            print(f"{key}: set (len {len(v)})")

    for key in REQUIRED_URLS:
        v = value(text, key)
        if not v:
            problems.append(f"{key} is not set")
            continue
        if any(h in v for h in LOOPBACK):
            problems.append(f"{key} points at the device itself: {v}")
        else:
            print(f"{key}: {v}")

    for p in problems:
        print(f"BAD CONFIG: {p}")
    print(f"check_config: {'PASS' if not problems else f'FAIL ({len(problems)})'}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
