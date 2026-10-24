"""Fetch the pinned Whisper model at image-build time and verify it.

The model is the one thing in this image that could drift silently: the hub
serves whatever the branch points at today. Downloading by commit revision and
checking the weight file's sha256 makes a rebuild produce the same bytes, and
keeps the container off the network at run time.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import urllib.request

FILES = ("config.json", "model.bin", "tokenizer.json", "vocabulary.txt")


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--revision", required=True, help="commit sha, not a branch")
    parser.add_argument("--sha256", required=True, help="expected sha256 of model.bin")
    parser.add_argument("--dest", required=True)
    args = parser.parse_args()

    dest = pathlib.Path(args.dest)
    dest.mkdir(parents=True, exist_ok=True)

    for name in FILES:
        url = f"https://huggingface.co/{args.repo}/resolve/{args.revision}/{name}"
        target = dest / name
        print(f"fetch {url}", flush=True)
        urllib.request.urlretrieve(url, target)

    actual = sha256_of(dest / "model.bin")
    if actual != args.sha256:
        print(f"model.bin sha256 mismatch\n  expected {args.sha256}\n  actual   {actual}", file=sys.stderr)
        return 1

    print(f"model verified: {actual} ({(dest / 'model.bin').stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
