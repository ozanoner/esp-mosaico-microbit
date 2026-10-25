#!/usr/bin/env python3
"""Preflight: exercise the build service the way the device does, from the host.

Two calls, in the order the firmware makes them:
  1. POST a WAV to /v1/speak        -> ASR + LLM + mkc, answers JSON
  2. POST {"source": ...} to /v1/build?region2=true -> the region-2 image bytes

This catches the class of failure where the panel is blamed for a service or
config problem: it runs in seconds, with no board, no microphone and no battery.

  tools/preflight/test_service_audio.py            # uses the URL in sdkconfig
  tools/preflight/test_service_audio.py --url ...  # or an explicit one
"""

import argparse
import json
import pathlib
import re
import sys
import urllib.error
import urllib.request

BASE = pathlib.Path(__file__).resolve().parents[2]


def find_repo() -> pathlib.Path:
    """The workspace holding tmp/audio and microbit/payloads, wherever we run from."""
    for parent in BASE.parents:
        if (parent / "microbit/payloads/hello.ts").exists():
            return parent
    raise SystemExit("could not locate the repo root (microbit/payloads/hello.ts)")


REPO = find_repo()
AUDIO = BASE / "tools/preflight/fixtures"
PAYLOAD = REPO / "microbit/payloads/hello.ts"
SDKCONFIG_H = BASE / "build/config/sdkconfig.h"


def service_base(cli_url):
    if cli_url:
        return cli_url.rstrip("/")
    if SDKCONFIG_H.exists():
        m = re.search(r'CONFIG_MB_VOICE_SERVICE_URL "([^"]*)"', SDKCONFIG_H.read_text())
        if m:
            return m.group(1).rsplit("/v1/", 1)[0]
    return "http://192.168.68.62:8477"


def post(url, data, content_type, timeout=180):
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": content_type})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url")
    ap.add_argument("--clip", default="command")
    args = ap.parse_args()
    base = service_base(args.url)
    failures = 0
    print(f"service: {base}")

    # 1. speech -> program
    clip = AUDIO / f"{args.clip}.wav"
    if not clip.exists():
        print(f"missing fixture {clip}", file=sys.stderr)
        return 1
    status, headers, body = post(f"{base}/v1/speak?include_hex=false", clip.read_bytes(),
                                 "audio/wav")
    print(f"/v1/speak  {args.clip}: http={status} type={headers.get('Content-Type')} bytes={len(body)}")
    if status != 200:
        print(f"  FAIL: expected 200, body: {body[:200]!r}")
        failures += 1
    else:
        reply = json.loads(body)
        transcript = (reply.get("transcript") or "").strip()
        print(f"  transcript: {transcript!r}")
        print(f"  block_count: {reply.get('block_count')} sha256: {str(reply.get('sha256'))[:16]}…")
        if not transcript:
            print("  FAIL: empty transcript — ASR did not hear the fixture")
            failures += 1
        if not isinstance(reply.get("block_count"), int) or reply.get("block_count", 0) <= 0:
            print("  FAIL: no program was built")
            failures += 1

    # 2. source -> region-2 image bytes (what the flasher streams)
    source = PAYLOAD.read_text()
    status, headers, body = post(f"{base}/v1/build?region2=true",
                                 json.dumps({"source": source}).encode(), "application/json")
    start, end = headers.get("x-region-start"), headers.get("x-region-end")
    print(f"/v1/build  region2: http={status} type={headers.get('Content-Type')} bytes={len(body)} "
          f"range={start}..{end} blocks={headers.get(chr(120)+chr(45)+chr(98)+chr(108)+chr(111)+chr(99)+chr(107)+chr(45)+chr(99)+chr(111)+chr(117)+chr(110)+chr(116))}")
    if status != 200:
        print(f"  FAIL: expected 200, body: {body[:200]!r}")
        failures += 1
    elif not start or not end:
        print(f"  FAIL: geometry headers missing (saw {dict(headers)!r})")
        failures += 1
    else:
        expected = int(end) - int(start)
        if len(body) != expected:
            print(f"  FAIL: body {len(body)} B != announced region-2 size {expected} B")
            failures += 1

    print(f"test_service_audio: {'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
