# ASR sidecar

`faster-whisper` behind an OpenAI-compatible `/v1/audio/transcriptions`, so the
build service can reach it with `MB_ASR_BASE_URL=http://asr:8000/v1` and **no
credential at all**. Nothing is published to the host: the service sees it on the
compose network only.

## Why a sidecar and not baked into the build service

The compiler image stays as it was — a clone that only needs `/v1/build` never
pulls 150 MB of model weights. It is also swappable: point `MB_ASR_BASE_URL` at
Groq, OpenAI, Voicebox (`http://<host>:17493`) or a `faster-whisper` server on
another machine and this container is simply not started.

## What is pinned, and why

| thing | how | why |
|---|---|---|
| base image | `python:3.12-slim@sha256:a0f57064…` | a floating tag rebuilds to different bytes |
| python packages | exact versions in `requirements.txt` | transcription must be a function of the repo |
| model weights | HF commit `ebe41f70…`, `model.bin` sha256 verified at build | the hub serves whatever the branch points at today |
| decode | stdlib `wave` + numpy, not PyAV | `faster-whisper`'s PyAV call broke on version skew (`av.open() got an unexpected keyword argument 'metadata_errors'`) and PyAV has no part in decoding a PCM WAV we produced ourselves |

`fetch_model.py` fails the image build if `model.bin` does not hash to the
expected value, so a silently changed upstream cannot ship.

## Measured cost (2 vCPU, no GPU)

- image: ~150 MB model + ~250 MB runtime
- RAM: ~1 GB resident once the model is loaded
- speed: 11 s clip in 2.7 s (RTF ≈ 0.25); first request adds ~1 s for model load

## Not reproducible, deliberately

ASR output is not bit-identical across hardware: int8 kernels can differ by a
word on ambiguous audio. Tests assert recognition of a known phrase rather than
an exact string, and no golden transcript is committed.
