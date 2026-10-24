# faster-whisper ASR sidecar.
#
# Everything that could drift is pinned: the base image by digest, the Python
# packages by exact version, and the model by HuggingFace commit revision plus a
# sha256 check on the weight file. A rebuild on any machine produces the same
# bytes, and the container needs no network at run time.
FROM python:3.12-slim@sha256:a0f57064cbdcd6285fa45f51237ab0b35ea8ebaed06a1f5973e89e19dfb86704

ARG WHISPER_REPO=Systran/faster-whisper-base
ARG WHISPER_REV=ebe41f70d5b6dfa9166e2c581c45c9c0cfc57b66
ARG MODEL_SHA256=d01c3014881c9c6f3133c182f3d2887eb6ca1c789a7538c5c007196857a0a6a9

WORKDIR /opt/asr

COPY asr/requirements.txt /opt/asr/requirements.txt
RUN pip install --no-cache-dir -r /opt/asr/requirements.txt

COPY asr/fetch_model.py asr/server.py /opt/asr/
RUN python /opt/asr/fetch_model.py \
        --repo "${WHISPER_REPO}" \
        --revision "${WHISPER_REV}" \
        --sha256 "${MODEL_SHA256}" \
        --dest /models

# CPU inference only; no shell, no root.
RUN useradd --system --uid 10001 asr && chown -R asr:asr /models
USER asr

EXPOSE 8000
CMD ["uvicorn", "server:app", "--host", "0.0.0.0", "--port", "8000", "--log-level", "warning"]
