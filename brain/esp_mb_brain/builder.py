"""Compile a MakeCode source file to a micro:bit v1 hex.

The pinned template directory supplies ``pxt.json``; only ``main.ts`` changes.
That is deliberate: the DAL version — and therefore the template hash the
partial-flash service compares against — must not move between builds.

``mkc`` is never invoked with ``--update``. It downloads parts of a released
MakeCode editor on first use and refreshes them on its own schedule; freezing
that install is the whole reason this service exists.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, asdict
from pathlib import Path

import hexkit

from .config import Settings, settings as default_settings


class BuildError(RuntimeError):
    """Raised when the toolchain is missing or the compiler rejects the source."""

    def __init__(self, message: str, log: str = "") -> None:
        super().__init__(message)
        self.log = log


@dataclass(frozen=True)
class Build:
    hex_bytes: bytes
    sha256: str
    template_hash: str
    program_hash: str
    region2_start: int
    region2_end: int
    block_count: int
    toolchain_id: str
    log: str

    def to_json(self, *, include_hex: bool = True) -> dict:
        payload = asdict(self)
        payload["region2"] = {"start": self.region2_start, "end": self.region2_end}
        for key in ("region2_start", "region2_end", "hex_bytes"):
            payload.pop(key, None)
        if include_hex:
            payload["hex_b64"] = base64.b64encode(self.hex_bytes).decode("ascii")
        return payload


def check_toolchain(mkc_bin: str) -> None:
    if shutil.which(mkc_bin) is None:
        raise BuildError(
            f"{mkc_bin!r} not found. Install the pinned MakeCode compiler: "
            "npm install -g makecode"
        )


def build(
    source: str,
    *,
    settings: Settings | None = None,
    keep_workdir: bool = False,
) -> Build:
    """Compile ``source`` (TypeScript) and return the resulting image geometry."""
    cfg = settings or default_settings
    check_toolchain(cfg.mkc_bin)

    template = Path(cfg.template_dir)
    pxt_json = template / "pxt.json"
    if not pxt_json.is_file():
        raise BuildError(f"template is missing pxt.json: {pxt_json}")

    cfg.ensure_dirs()
    work = Path(tempfile.mkdtemp(prefix="build-", dir=cfg.work_dir))
    try:
        shutil.copy2(pxt_json, work / "pxt.json")
        (work / "main.ts").write_text(source, encoding="utf-8")

        completed = subprocess.run(
            [cfg.mkc_bin, "build"],
            cwd=work,
            capture_output=True,
            text=True,
            timeout=cfg.build_timeout_s,
            check=False,
        )
        log = (completed.stdout + completed.stderr).strip()
        if completed.returncode != 0:
            raise BuildError(f"mkc build failed (exit {completed.returncode})", log)

        produced = sorted((work / "built").glob("*.hex"), key=lambda p: p.stat().st_mtime)
        if not produced:
            raise BuildError("mkc build produced no .hex output", log)

        hex_bytes = produced[-1].read_bytes()
        image = hexkit.parse_hex(hex_bytes.decode("utf-8", errors="replace"))
        template_hash, program_hash = image.hashes()
        start, end = image.region2()
        blocks = image.blocks()

        return Build(
            hex_bytes=hex_bytes,
            sha256=hashlib.sha256(hex_bytes).hexdigest(),
            template_hash=template_hash.hex(),
            program_hash=program_hash.hex(),
            region2_start=start,
            region2_end=end,
            block_count=len(blocks),
            toolchain_id=cfg.toolchain_id,
            log=log[-4000:],
        )
    finally:
        if not keep_workdir:
            shutil.rmtree(work, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m esp_mb_brain.builder",
        description="Build a MakeCode source file into a micro:bit v1 hex",
    )
    parser.add_argument("source", type=Path, help="TypeScript source (main.ts)")
    parser.add_argument("-o", "--out", type=Path, required=True, help="output .hex path")
    parser.add_argument("--keep-workdir", action="store_true")
    args = parser.parse_args(argv)

    try:
        result = build(args.source.read_text(encoding="utf-8"), keep_workdir=args.keep_workdir)
    except (BuildError, hexkit.HexError, OSError) as exc:
        print(f"build: {exc}", file=sys.stderr)
        log = getattr(exc, "log", "")
        if log:
            print(log, file=sys.stderr)
        return 2

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(result.hex_bytes)
    info = result.to_json(include_hex=False)
    print(f"hex            : {args.out} ({len(result.hex_bytes)} B)")
    print(f"sha256         : {info['sha256']}")
    print(f"template hash  : {info['template_hash']}")
    print(f"program hash   : {info['program_hash']}")
    print(f"region 2       : 0x{result.region2_start:08X}–0x{result.region2_end - 1:08X}")
    print(f"blocks         : {result.block_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
