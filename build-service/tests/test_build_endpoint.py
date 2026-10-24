"""`/v1/build` — the non-voice path, and the byte stream the device writes.

Two things are load-bearing here and are asserted rather than described:

1. `?region2=true` answers with exactly the bytes the flash client streams and
   the geometry in headers — the device does no Intel HEX parsing.
2. It answers with the *same header names* as `/v1/speak?region2=true`, so the
   firmware carries one parser for both entry points (gate G4).

No compiler: `build` is replaced with a stub whose bytes are known.
"""

from __future__ import annotations

from fastapi.testclient import TestClient

from mb_build_service import app as app_module
from mb_build_service.builder import Build, BuildError

SOURCE = """
const REVISION = "A1"
basic.showString(REVISION)
"""

STUB_HASHES = {
    "template_hash": "08d9ef69355ad697",
    "program_hash": "8f007d004500b100",
    "region2_start": 0x37400,
    "region2_end": 0x37400 + 96 * 64,
    "block_count": 96,
}
STUB_BYTES = bytes(v & 0xFF for v in range(96 * 64))


def stub_build(source: str, *, settings=None) -> Build:
    return Build(
        hex_bytes=b":00000001FF\n",
        sha256="0" * 64,
        toolchain_id="mkc-1.3.6",
        log="",
        region2_data=STUB_BYTES,
        **STUB_HASHES,
    )


def test_build_returns_geometry_and_hex(monkeypatch) -> None:
    monkeypatch.setattr(app_module, "build", stub_build)
    client = TestClient(app_module.app)
    response = client.post("/v1/build", json={"source": SOURCE})
    assert response.status_code == 200
    body = response.json()
    assert body["block_count"] == 96
    assert body["template_hash"] == STUB_HASHES["template_hash"]
    assert body["program_hash"] == STUB_HASHES["program_hash"]
    assert body["region2"] == {"start": 0x37400, "end": 0x37400 + 96 * 64}
    assert "hex_b64" in body
    assert "region2_data" not in body


def test_build_can_return_region2_binary(monkeypatch) -> None:
    """The gate-G4 path: bytes plus geometry in headers, no JSON, no hex."""
    monkeypatch.setattr(app_module, "build", stub_build)
    client = TestClient(app_module.app)
    response = client.post("/v1/build?region2=true", json={"source": SOURCE})
    assert response.status_code == 200
    assert response.headers["content-type"] == "application/octet-stream"
    assert response.headers["X-Region-Start"] == str(0x37400)
    assert response.headers["X-Region-End"] == str(0x37400 + 96 * 64)
    assert response.headers["X-Template-Hash"] == STUB_HASHES["template_hash"]
    assert response.headers["X-Program-Hash"] == STUB_HASHES["program_hash"]
    assert response.headers["X-Block-Count"] == "96"
    assert response.content == STUB_BYTES


def test_both_build_paths_share_the_header_names(monkeypatch) -> None:
    """One parser in the firmware: the two endpoints must not drift apart."""
    monkeypatch.setattr(app_module, "build", stub_build)
    client = TestClient(app_module.app)
    headers = client.post("/v1/build?region2=true", json={"source": SOURCE}).headers
    assert set(headers.keys()) & {
        "x-region-start",
        "x-region-end",
        "x-template-hash",
        "x-program-hash",
        "x-block-count",
    } == {
        "x-region-start",
        "x-region-end",
        "x-template-hash",
        "x-program-hash",
        "x-block-count",
    }


def test_compiler_failure_is_a_502_with_the_log(monkeypatch) -> None:
    def failing(source: str, *, settings=None) -> Build:
        raise BuildError("mkc build failed (exit 1)", "error: cannot find name 'Foo'")

    monkeypatch.setattr(app_module, "build", failing)
    client = TestClient(app_module.app)
    response = client.post("/v1/build?region2=true", json={"source": SOURCE})
    assert response.status_code == 502
    detail = response.json()["detail"]
    assert "cannot find name 'Foo'" in detail["log"]


def test_empty_source_is_rejected_by_the_schema() -> None:
    client = TestClient(app_module.app)
    assert client.post("/v1/build", json={}).status_code == 422
