"""HTTP surface of the brain service (PoC scope: build + health).

Speech and LLM endpoints are deliberately absent: the PoC validates the chain
that carries the risk (compile -> hashes -> BLE flash). Voice arrives once a
generated image has been proven to boot.
"""

from __future__ import annotations

import uvicorn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

import hexkit

from .builder import BuildError, build
from .config import settings

app = FastAPI(title="esp-mosaico-microbit brain", version="0.1.0")


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


def run() -> None:
    uvicorn.run(app, host="0.0.0.0", port=8080)


if __name__ == "__main__":
    run()
