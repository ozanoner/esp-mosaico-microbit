"""Tests for the idea endpoint: prompt plumbing, the retry loop, honest failure.

No network and no compiler here — the point is the orchestration. The real
compile path is covered by test_builder.py, which runs the pinned mkc.
"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from mb_build_service import app as app_module
from mb_build_service.builder import Build, BuildError
from mb_build_service.speak import SpeakError, extract_source, generate_program
from mb_build_service.providers import (
    AsrConfig,
    FakeGenerator,
    FakeTranscriber,
    LlmConfig,
    OpenAiCompatibleGenerator,
    OpenAiCompatibleTranscriber,
    ProviderError,
)

GOOD = """
const REVISION = "A1"
basic.showString(REVISION)
input.onButtonPressed(Button.A, function () {
    basic.showIcon(IconNames.Heart)
})
"""


def stub_build(source: str, *, settings=None) -> Build:
    return Build(
        hex_bytes=b":00000001FF\n",
        sha256="0" * 64,
        template_hash="08d9ef69355ad697",
        program_hash="8f007d004500b100",
        region2_start=0x37400,
        region2_end=0x38400,
        block_count=64,
        toolchain_id="mkc-1.3.6",
        log="",
        region2_data=bytes(range(64)),
    )


def test_extract_source_strips_fences_and_prose() -> None:
    reply = "Here you go:\n```typescript\n" + GOOD + "\n```\nHope that helps."
    assert extract_source(reply).lstrip().startswith("const REVISION")
    assert "```" not in extract_source(reply)
    assert "Hope that helps" not in extract_source(reply)


def test_extract_source_rejects_empty_reply() -> None:
    with pytest.raises(ValueError):
        extract_source("   \n  ")


def test_success_on_first_attempt() -> None:
    idea = generate_program("make it show a heart", generator=FakeGenerator(GOOD), build_fn=stub_build)
    assert idea.attempts == 1
    assert idea.compiler_logs == []
    assert idea.build.block_count == 64


def test_retry_feeds_the_compiler_message_back() -> None:
    calls = {"n": 0}

    def flaky(source: str, *, settings=None) -> Build:
        calls["n"] += 1
        if calls["n"] == 1:
            raise BuildError("mkc build failed (exit 1)", "error: unknown symbol 'IconNamez'")
        return stub_build(source)

    generator = FakeGenerator(GOOD, GOOD)
    idea = generate_program("make it show a heart", generator=generator, build_fn=flaky)
    assert idea.attempts == 2
    assert len(idea.compiler_logs) == 1
    assert "IconNamez" in idea.compiler_logs[0]
    assert "IconNamez" in generator.prompts[1]


def test_second_failure_raises_with_the_compiler_message() -> None:
    def always_fails(source: str, *, settings=None) -> Build:
        raise BuildError("mkc build failed (exit 1)", "error: cannot find name 'Foo'")

    with pytest.raises(SpeakError) as caught:
        generate_program("nonsense", generator=FakeGenerator(GOOD, GOOD), build_fn=always_fails)
    err = caught.value
    assert err.attempts == 2
    assert "cannot find name 'Foo'" in err.compiler_message
    assert err.transcript == "nonsense"


def test_empty_transcript_is_rejected() -> None:
    with pytest.raises(ValueError):
        generate_program("   ", generator=FakeGenerator(GOOD), build_fn=stub_build)


def test_live_provider_requires_a_key() -> None:
    with pytest.raises(ProviderError):
        OpenAiCompatibleGenerator(LlmConfig(api_key=""))


def test_local_asr_needs_no_key() -> None:
    """The sidecar has no auth; an empty key must not block transcription."""
    transcriber = OpenAiCompatibleTranscriber(
        AsrConfig(base_url="http://asr:8000/v1", api_key="", model="base")
    )
    assert transcriber is not None


def test_endpoint_takes_text_and_returns_geometry(monkeypatch) -> None:
    monkeypatch.setattr(app_module, "OpenAiCompatibleGenerator", lambda cfg: FakeGenerator(GOOD))
    monkeypatch.setattr(
        app_module, "generate_from_speech", lambda transcript, **kw: generate_program(transcript, generator=FakeGenerator(GOOD), build_fn=stub_build)
    )
    client = TestClient(app_module.app)
    response = client.post("/v1/speak", json={"text": "show a heart"})
    assert response.status_code == 200
    body = response.json()
    assert body["transcript"] == "show a heart"
    assert body["block_count"] == 64
    assert body["template_hash"] == "08d9ef69355ad697"
    assert "hex_b64" in body


def test_endpoint_can_omit_hex(monkeypatch) -> None:
    """The device asks for geometry first; 1.8 MB of base64 would blow its heap."""
    monkeypatch.setattr(app_module, "OpenAiCompatibleGenerator", lambda cfg: FakeGenerator(GOOD))
    monkeypatch.setattr(
        app_module, "generate_from_speech", lambda transcript, **kw: generate_program(transcript, generator=FakeGenerator(GOOD), build_fn=stub_build)
    )
    client = TestClient(app_module.app)
    response = client.post("/v1/speak?include_hex=false", json={"text": "show a heart"})
    assert response.status_code == 200
    body = response.json()
    assert "hex_b64" not in body
    assert body["block_count"] == 64


def test_endpoint_can_return_region2_binary(monkeypatch) -> None:
    """The device path: bytes plus geometry in headers, no JSON, no hex."""
    monkeypatch.setattr(app_module, "OpenAiCompatibleGenerator", lambda cfg: FakeGenerator(GOOD))
    monkeypatch.setattr(
        app_module, "generate_from_speech", lambda transcript, **kw: generate_program(transcript, generator=FakeGenerator(GOOD), build_fn=stub_build)
    )
    client = TestClient(app_module.app)
    response = client.post("/v1/speak?region2=true", json={"text": "show a heart"})
    assert response.status_code == 200
    assert response.headers["content-type"] == "application/octet-stream"
    assert response.headers["X-Block-Count"] == "64"
    assert response.headers["X-Program-Hash"] == "8f007d004500b100"
    assert response.content == bytes(range(64))


def test_region2_data_stays_out_of_json(monkeypatch) -> None:
    """asdict() would put raw bytes in the JSON payload; it must be dropped."""
    body = stub_build("x").to_json()
    assert "region2_data" not in body
    assert "hex_b64" in body


def test_endpoint_returns_compiler_message_on_failure(monkeypatch) -> None:
    def boom(*args, **kwargs):
        raise SpeakError("show a heart", GOOD, "error: cannot find name 'Foo'", 2)

    monkeypatch.setattr(app_module, "OpenAiCompatibleGenerator", lambda cfg: FakeGenerator(GOOD))
    monkeypatch.setattr(app_module, "generate_from_speech", boom)
    client = TestClient(app_module.app)
    response = client.post("/v1/speak", json={"text": "show a heart"})
    assert response.status_code == 502
    detail = response.json()["detail"]
    assert "cannot find name 'Foo'" in detail["compiler_message"]
    assert detail["attempts"] == 2


def test_endpoint_uses_asr_for_audio(monkeypatch) -> None:
    transcriber = FakeTranscriber("play a tune")
    monkeypatch.setattr(app_module, "OpenAiCompatibleTranscriber", lambda cfg: transcriber)
    monkeypatch.setattr(app_module, "OpenAiCompatibleGenerator", lambda cfg: FakeGenerator(GOOD))
    client = TestClient(app_module.app)
    response = client.post("/v1/speak", content=b"RIFF....WAVE", headers={"content-type": "audio/wav"})
    assert response.status_code == 200
    assert response.json()["transcript"] == "play a tune"
    assert transcriber.calls == [b"RIFF....WAVE"]
