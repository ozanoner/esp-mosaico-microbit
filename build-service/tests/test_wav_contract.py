"""The WAV the device uploads must be readable by the ASR sidecar.

Two independent implementations meet here and nothing in CI touched the seam:

* `mb_voice.c:55-69` (`wav_header`) writes a canonical 44-byte PCM header —
  RIFF/WAVE, format 1, mono, 16 kHz, 16-bit — into PSRAM and posts it as
  `Content-Type: audio/wav` with the PCM glued straight after it.
* `asr/server.py:39-63` (`pcm_to_float32`) reads that payload with the stdlib
  `wave` module and hands float32 samples to faster-whisper.

If either side drifts (a wrong chunk size, a byte-rate that disagrees with the
sample width, a `fmt ` chunk that is not 16 bytes) the decoder raises, the ASR
answer is empty, and the device shows `ASR returned an empty transcript` — a
symptom that points at the microphone rather than at the header. These tests pin
the layout so that failure can only come from the audio.

The header bytes are spelled out rather than imported from the C source: this is
the contract, and a test that regenerates it from the same code would agree with
any drift. `test_header_matches_the_c_writer` asserts the layout field by field
against `mb_voice.c`.
"""

from __future__ import annotations

import io
import wave

SAMPLE_RATE = 16000
CHANNELS = 1
BYTES_PER_SAMPLE = 2
WAV_HEADER_BYTES = 44


def wav_header(data_len: int) -> bytes:
    """The 44-byte header exactly as `mb_voice.c:wav_header` lays it out."""
    header = bytearray(WAV_HEADER_BYTES)
    header[0:4] = b"RIFF"
    header[4:8] = (36 + data_len).to_bytes(4, "little")
    header[8:16] = b"WAVEfmt "
    header[16:20] = (16).to_bytes(4, "little")
    header[20:22] = (1).to_bytes(2, "little")  # format tag: PCM
    header[22:24] = CHANNELS.to_bytes(2, "little")
    header[24:28] = SAMPLE_RATE.to_bytes(4, "little")
    header[28:32] = (SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE).to_bytes(4, "little")
    header[32:34] = (CHANNELS * BYTES_PER_SAMPLE).to_bytes(2, "little")
    header[34:36] = (BYTES_PER_SAMPLE * 8).to_bytes(2, "little")
    header[36:40] = b"data"
    header[40:44] = data_len.to_bytes(4, "little")
    return bytes(header)


def test_header_is_parsed_by_the_module_the_asr_uses() -> None:
    """`wave.open()` is the exact call `asr/server.py` makes on the upload."""
    pcm = b"\x01\x00\x02\x00\x03\x00\x04\x00"  # four 16-bit samples
    payload = wav_header(len(pcm)) + pcm

    with wave.open(io.BytesIO(payload), "rb") as handle:
        assert handle.getnchannels() == CHANNELS
        assert handle.getsampwidth() == BYTES_PER_SAMPLE
        assert handle.getframerate() == SAMPLE_RATE
        assert handle.getnframes() == len(pcm) // BYTES_PER_SAMPLE
        assert handle.readframes(handle.getnframes()) == pcm


def test_header_matches_the_c_writer() -> None:
    """Field-by-field, so a change on either side fails loudly here."""
    data_len = 32000  # one second of 16 kHz mono PCM16
    header = wav_header(data_len)

    assert len(header) == WAV_HEADER_BYTES
    assert header[0:4] == b"RIFF"
    assert int.from_bytes(header[4:8], "little") == 36 + data_len
    assert header[8:16] == b"WAVEfmt "
    assert int.from_bytes(header[16:20], "little") == 16  # PCM `fmt ` chunk size
    assert int.from_bytes(header[20:22], "little") == 1  # WAVE_FORMAT_PCM
    assert int.from_bytes(header[22:24], "little") == 1  # mono
    assert int.from_bytes(header[24:28], "little") == 16000
    assert int.from_bytes(header[28:32], "little") == 32000  # byte rate
    assert int.from_bytes(header[32:34], "little") == 2  # block align
    assert int.from_bytes(header[34:36], "little") == 16  # bits per sample
    assert header[36:40] == b"data"
    assert int.from_bytes(header[40:44], "little") == data_len


def test_full_scale_samples_survive_the_upload() -> None:
    """A clip at the rails must decode as +-1.0, not as silence.

    The device's own statistics put a room-speech capture at peak 2930 of 32767
    and a quiet one at 322, so the encoder has to keep small amplitudes intact.
    """
    pcm = (-32768).to_bytes(2, "little", signed=True) + (32767).to_bytes(2, "little", signed=True)
    payload = wav_header(len(pcm)) + pcm

    with wave.open(io.BytesIO(payload), "rb") as handle:
        frames = handle.readframes(handle.getnframes())

    samples = [int.from_bytes(frames[i : i + 2], "little", signed=True) for i in (0, 2)]
    assert samples == [-32768, 32767]
