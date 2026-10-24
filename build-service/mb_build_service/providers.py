"""ASR and LLM access, kept behind two narrow interfaces.

Both providers are OpenAI-compatible over plain HTTP using only the standard
library: adding `requests`/`httpx` to the runtime would mean re-pinning the
lockfile for two POSTs.

Credentials come from the environment and nowhere else — no default, no file in
the repo. `Settings` refuses to build a live provider without a key, so a
misconfigured box fails at request time with a clear message instead of sending
audio to an unauthenticated endpoint.
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass
from typing import Protocol


class ProviderError(RuntimeError):
    """Raised when a remote provider is unreachable, rejects us, or answers oddly."""


@dataclass(frozen=True)
class AsrConfig:
    base_url: str = os.environ.get("MB_ASR_BASE_URL", "https://api.openai.com/v1")
    api_key: str = os.environ.get("MB_ASR_API_KEY", "")
    model: str = os.environ.get("MB_ASR_MODEL", "whisper-1")
    timeout_s: int = int(os.environ.get("MB_ASR_TIMEOUT_S", "60"))


@dataclass(frozen=True)
class LlmConfig:
    base_url: str = os.environ.get("MB_LLM_BASE_URL", "https://api.openai.com/v1")
    api_key: str = os.environ.get("MB_LLM_API_KEY", "")
    model: str = os.environ.get("MB_LLM_MODEL", "gpt-4o-mini")
    timeout_s: int = int(os.environ.get("MB_LLM_TIMEOUT_S", "90"))
    max_tokens: int = int(os.environ.get("MB_LLM_MAX_TOKENS", "900"))


class Transcriber(Protocol):
    def transcribe(self, wav: bytes) -> str: ...


class Generator(Protocol):
    def complete(self, system: str, user: str) -> str: ...


def _post_json(url: str, payload: dict, api_key: str, timeout_s: int) -> dict:
    body = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:400]
        raise ProviderError(f"{url} returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise ProviderError(f"{url} unreachable: {exc.reason}") from exc


class OpenAiCompatibleTranscriber:
    """`POST {base}/audio/transcriptions`, multipart built by hand."""

    def __init__(self, config: AsrConfig) -> None:
        if not config.api_key:
            raise ProviderError("MB_ASR_API_KEY is not set; ASR is unavailable")
        self._config = config

    def transcribe(self, wav: bytes) -> str:
        if not wav:
            raise ProviderError("empty audio payload")

        boundary = f"----mb{uuid.uuid4().hex}"
        parts: list[bytes] = []
        parts.append(
            f'--{boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\n'
            f"{self._config.model}\r\n".encode()
        )
        parts.append(
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
            f'filename="clip.wav"\r\nContent-Type: audio/wav\r\n\r\n'.encode()
        )
        parts.append(wav)
        parts.append(f"\r\n--{boundary}--\r\n".encode())
        body = b"".join(parts)

        url = f"{self._config.base_url.rstrip('/')}/audio/transcriptions"
        request = urllib.request.Request(
            url,
            data=body,
            headers={
                "Content-Type": f"multipart/form-data; boundary={boundary}",
                "Authorization": f"Bearer {self._config.api_key}",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self._config.timeout_s) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")[:400]
            raise ProviderError(f"ASR returned HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise ProviderError(f"ASR unreachable: {exc.reason}") from exc

        text = (payload.get("text") or "").strip()
        if not text:
            raise ProviderError("ASR returned an empty transcript")
        return text


class OpenAiCompatibleGenerator:
    """`POST {base}/chat/completions`."""

    def __init__(self, config: LlmConfig) -> None:
        if not config.api_key:
            raise ProviderError("MB_LLM_API_KEY is not set; program generation is unavailable")
        self._config = config

    def complete(self, system: str, user: str) -> str:
        payload = {
            "model": self._config.model,
            "temperature": 0,
            "max_tokens": self._config.max_tokens,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
        }
        url = f"{self._config.base_url.rstrip('/')}/chat/completions"
        answer = _post_json(url, payload, self._config.api_key, self._config.timeout_s)
        try:
            return answer["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise ProviderError(f"LLM response had no message content: {answer}") from exc


class FakeTranscriber:
    def __init__(self, text: str) -> None:
        self.text = text
        self.calls: list[bytes] = []

    def transcribe(self, wav: bytes) -> str:
        self.calls.append(wav)
        return self.text


class FakeGenerator:
    """Returns queued answers in order, so a retry loop can be exercised."""

    def __init__(self, *answers: str) -> None:
        self._answers = list(answers)
        self.prompts: list[str] = []

    def complete(self, system: str, user: str) -> str:
        self.prompts.append(user)
        if not self._answers:
            raise ProviderError("FakeGenerator ran out of answers")
        return self._answers.pop(0)
