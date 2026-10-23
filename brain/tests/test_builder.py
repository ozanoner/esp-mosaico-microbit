"""Builder tests using a stand-in ``mkc`` — no network, no toolchain.

The fake writes a MakeCode-shaped hex into ``built/`` so the parsing, hash
extraction and region slicing are exercised exactly as they will be in
production, without downloading an editor.
"""

from __future__ import annotations

import os
import stat
from pathlib import Path

import pytest

from esp_mb_brain.builder import BuildError, build
from esp_mb_brain.config import Settings

TEMPLATE_HASH = bytes.fromhex("0662709FA0315567")
PROGRAM_HASH = bytes.fromhex("EDE9690A1093AE71")
DAL_MAGIC = bytes.fromhex("708E3B92C615A841C49866C975EE5197")
HEADER = DAL_MAGIC + TEMPLATE_HASH + PROGRAM_HASH + b"\x0b\x00" + b"\x00" * 10
BODY = b"\xAA" * (2 * 64)


def _record(address: int, data: bytes, record_type: int = 0) -> str:
    body = bytes([len(data), (address >> 8) & 0xFF, address & 0xFF, record_type]) + data
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def _canned_hex() -> str:
    body = HEADER + BODY
    lines = [_record(0, (0x30000 >> 16).to_bytes(2, "big"), 0x04)]
    for offset in range(0, len(body), 32):
        lines.append(_record(offset, body[offset : offset + 32]))
    return "\n".join(lines + [_record(0, b"", 0x01)]) + "\n"


def _fake_mkc(tmp_path: Path, *, exit_code: int = 0) -> Path:
    """Stand-in compiler: writes a canned hex into ``built/``."""
    script = tmp_path / "fake-mkc"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import pathlib, sys\n"
        "out = pathlib.Path('built')\n"
        "out.mkdir(exist_ok=True)\n"
        f"(out / 'microbit-test.hex').write_text({_canned_hex()!r})\n"
        f"sys.exit({exit_code})\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    return script


@pytest.fixture()
def settings(tmp_path: Path) -> Settings:
    template = tmp_path / "template"
    template.mkdir()
    (template / "pxt.json").write_text('{"name": "test", "dependencies": {}}')
    return Settings(
        template_dir=template,
        work_dir=tmp_path / "work",
        mkc_bin=str(_fake_mkc(tmp_path)),
        toolchain_id="test-toolchain",
    )


def test_build_extracts_hashes_and_geometry(settings: Settings) -> None:
    result = build("basic.showString('A1')", settings=settings)
    data_bytes = len(HEADER) + len(BODY)
    assert result.template_hash == TEMPLATE_HASH.hex()
    assert result.program_hash == PROGRAM_HASH.hex()
    assert result.region2_start == 0x30000
    assert result.region2_end == 0x30000 + data_bytes
    assert result.block_count == -(-data_bytes // 64)
    assert result.toolchain_id == "test-toolchain"
    assert len(result.sha256) == 64


def test_build_json_carries_base64_hex(settings: Settings) -> None:
    payload = build("basic.showString('A1')", settings=settings).to_json()
    assert payload["region2"] == {"start": 0x30000, "end": 0x30000 + len(HEADER) + len(BODY)}
    assert set(payload) >= {"hex_b64", "sha256", "template_hash", "program_hash", "block_count"}
    assert "hex_bytes" not in payload


def test_compiler_failure_is_reported_with_log(tmp_path: Path) -> None:
    template = tmp_path / "template"
    template.mkdir()
    (template / "pxt.json").write_text("{}")
    cfg = Settings(
        template_dir=template,
        work_dir=tmp_path / "work",
        mkc_bin=str(_fake_mkc(tmp_path, exit_code=1)),
        toolchain_id="test",
    )
    with pytest.raises(BuildError) as excinfo:
        build("nonsense", settings=cfg)
    assert "mkc build failed" in str(excinfo.value)


def test_missing_toolchain_is_a_clear_error(tmp_path: Path) -> None:
    template = tmp_path / "template"
    template.mkdir()
    (template / "pxt.json").write_text("{}")
    cfg = Settings(
        template_dir=template,
        work_dir=tmp_path / "work",
        mkc_bin="definitely-not-mkc",
        toolchain_id="test",
    )
    with pytest.raises(BuildError, match="npm install -g makecode"):
        build("basic.showString('A1')", settings=cfg)
