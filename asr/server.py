"""OpenAI-compatible `/v1/audio/transcriptions` over faster-whisper.

The build service already speaks this shape (`providers.py`), so pointing
`MB_ASR_BASE_URL` at this container needs no code change on the other side.

CPU only, one inference at a time: faster-whisper holds a single model instance
and its transcribe path is not safe to run concurrently.
"""

from __future__ import annotations

import io
import os
import threading
import wave

import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from faster_whisper import WhisperModel

MODEL_DIR = os.environ.get("MB_ASR_MODEL_DIR", "/models")
COMPUTE_TYPE = os.environ.get("MB_ASR_COMPUTE", "int8")
DEFAULT_LANGUAGE = os.environ.get("MB_ASR_LANGUAGE", "en") or None
BEAM_SIZE = int(os.environ.get("MB_ASR_BEAM", "1"))

app = FastAPI(title="faster-whisper ASR sidecar", version="0.1.0")

_lock = threading.Lock()
_model: WhisperModel | None = None


def model() -> WhisperModel:
    global _model
    if _model is None:
        _model = WhisperModel(MODEL_DIR, device="cpu", compute_type=COMPUTE_TYPE)
    return _model


def pcm_to_float32(audio: bytes) -> np.ndarray:
    """Decode a PCM WAV to float32 mono in [-1, 1].

    faster-whisper would otherwise hand the file to PyAV, whose `av.open`
    signature moved between releases — the resulting TypeError has nothing to do
    with the audio. Our input is PCM16 mono that we produce ourselves, so the
    standard library is both sufficient and version-proof.
    """
    with wave.open(io.BytesIO(audio), "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        frames = handle.readframes(handle.getnframes())

    if width == 2:
        samples = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 1:
        samples = (np.frombuffer(frames, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif width == 4:
        samples = np.frombuffer(frames, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise HTTPException(status_code=422, detail=f"unsupported sample width: {width}")

    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1)
    return samples


@app.get("/v1/health")
def health() -> dict:
    return {"status": "ok", "model_dir": MODEL_DIR, "compute_type": COMPUTE_TYPE}


@app.post("/v1/audio/transcriptions")
async def transcriptions(
    file: UploadFile = File(...),
    model_name: str = Form("base", alias="model"),
    language: str = Form(""),
) -> dict:
    audio = await file.read()
    if not audio:
        raise HTTPException(status_code=422, detail="empty audio payload")

    samples = pcm_to_float32(audio)
    if samples.size == 0:
        raise HTTPException(status_code=422, detail="no audio frames")

    with _lock:
        segments, info = model().transcribe(
            samples,
            language=language or DEFAULT_LANGUAGE,
            beam_size=BEAM_SIZE,
            vad_filter=False,
        )
        text = " ".join(segment.text.strip() for segment in segments).strip()

    return {
        "text": text,
        "language": info.language,
        "duration": info.duration,
        "model": model_name,
    }
