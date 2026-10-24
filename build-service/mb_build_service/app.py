"""HTTP surface of the build service.

`/v1/build` compiles source that already exists. `/v1/idea` is the voice entry
point: audio (or text) in, a compiled image out, with one compiler-feedback
retry. Both return the same geometry, because both feed the same flash path.
"""

from __future__ import annotations

import uvicorn
from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel, Field

import hexkit

from .builder import BuildError, build
from .config import settings
from .idea import IdeaError, generate_program
from .providers import (
    AsrConfig,
    LlmConfig,
    OpenAiCompatibleGenerator,
    OpenAiCompatibleTranscriber,
    ProviderError,
)

app = FastAPI(title="esp-mosaico-microbit build service", version="0.1.0")


class BuildRequest(BaseModel):
    source: str = Field(..., description="MakeCode TypeScript (contents of main.ts)")


@app.get("/v1/health")
def health() -> dict:
    return {
        "status": "ok",
        "toolchain_id": settings.toolchain_id,
        "template_dir": str(settings.template_dir),
        "mkc": settings.mkc_bin,
    }


@app.post("/v1/build")
def build_endpoint(request: BuildRequest) -> dict:
    try:
        result = build(request.source, settings=settings)
    except BuildError as exc:
        raise HTTPException(status_code=502, detail={"error": str(exc), "log": exc.log})
    except hexkit.HexError as exc:
        raise HTTPException(status_code=502, detail={"error": f"hex geometry: {exc}"})
    return result.to_json()


@app.post("/v1/idea")
async def idea_endpoint(request: Request) -> dict:
    """Audio (or text) in, compiled image out.

    Accepts either `application/json` with {"text": ...} — used by tests and by
    the device when it sends a transcript for confirmation — or a raw WAV body
    with any other content type, which goes through ASR first.
    """
    content_type = request.headers.get("content-type", "")
    try:
        if content_type.startswith("application/json"):
            payload = await request.json()
            transcript = (payload.get("text") or "").strip()
            if not transcript:
                raise HTTPException(status_code=422, detail="text is empty")
        else:
            audio = await request.body()
            transcript = OpenAiCompatibleTranscriber(AsrConfig()).transcribe(audio)

        generator = OpenAiCompatibleGenerator(LlmConfig())
        result = generate_program(transcript, generator=generator, settings=settings)
    except ProviderError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except IdeaError as exc:
        raise HTTPException(
            status_code=502,
            detail={
                "error": str(exc),
                "transcript": exc.transcript,
                "attempts": exc.attempts,
                "compiler_message": exc.compiler_message,
                "source": exc.source,
            },
        ) from exc
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    return result.to_json()


def run() -> None:
    uvicorn.run(app, host="0.0.0.0", port=settings.port)


if __name__ == "__main__":
    run()
