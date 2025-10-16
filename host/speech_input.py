# speech_input.py
"""
Voice capture + single-utterance transcription using:
- sounddevice (mic input @ 16 kHz, mono)
- webrtcvad (VAD on 20 ms frames)
- faster-whisper (transcription)

Behavior:
- Listens until speech is detected.
- Stops after 'silence_stop_s' of silence or when 'max_utterance_s' is reached.
- Returns a plain string (may be empty if nothing was captured).
"""

from __future__ import annotations

import queue
import sys
import time
import wave
from pathlib import Path
from typing import Optional, Tuple, Iterable

import numpy as np
import sounddevice as sd
import webrtcvad
from faster_whisper import WhisperModel

# ---------------------------------------------------------------------------
# Audio / VAD config
# ---------------------------------------------------------------------------
SAMPLE_RATE: int = 16_000       # Whisper standard
CHANNELS: int = 1
FRAME_MS: int = 20              # 10, 20, or 30 ms for webrtcvad
FRAME_SAMPLES: int = int(SAMPLE_RATE * FRAME_MS / 1000)  # e.g. 320 at 20 ms

# ---------------------------------------------------------------------------
# Whisper model (loaded lazily once)
# ---------------------------------------------------------------------------
WHISPER_MODEL_NAME: str = "small"    # 'base', 'medium', 'large-v3' also possible
WHISPER_COMPUTE_TYPE: str = "int8"   # 'int8' fast; alternatively 'float16'/'float32'
_model: Optional[WhisperModel] = None


def _get_model() -> WhisperModel:
    """Return a global, lazily-initialized faster-whisper model."""
    global _model
    if _model is None:
        _model = WhisperModel(WHISPER_MODEL_NAME, compute_type=WHISPER_COMPUTE_TYPE)
    return _model


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------
def _pcm16le_from_float32(data_f32: np.ndarray) -> bytes:
    """Convert float32 [-1..1] mono to int16 PCM bytes (little-endian)."""
    data = np.clip(data_f32, -1.0, 1.0)
    data_i16 = (data * 32767.0).astype(np.int16)
    return data_i16.tobytes()


def _write_wav(path: Path, pcm_bytes: bytes) -> None:
    """(Debug) Write raw PCM16 mono @ 16 kHz to a WAV file."""
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm_bytes)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
def transcribe_once(
    lang_hint: str = "de",
    max_utterance_s: float = 12.0,
    silence_stop_s: float = 0.8,
    device: Optional[int | str] = None,
) -> str:
    """
    Record a single utterance from the default (or given) input device and transcribe it.

    Flow:
      - Start capturing audio (16 kHz mono).
      - Use webrtcvad on 20 ms frames to detect speech.
      - Once speech starts: keep collecting frames until 'silence_stop_s' of silence,
        or until 'max_utterance_s' is reached.
      - Transcribe collected audio with faster-whisper and return the text.

    Args:
        lang_hint: "de" | "en" | "auto"
        max_utterance_s: Hard cap on capture time in seconds.
        silence_stop_s: Stop after this much silence (seconds) following speech.
        device: sounddevice input device (index or name). None = default.

    Returns:
        The transcribed text (empty string if no speech was captured).
    """
    vad = webrtcvad.Vad(2)  # 0..3 (aggressiveness). 2 is a good compromise.
    q: "queue.Queue[np.ndarray]" = queue.Queue()

    def cb(indata: np.ndarray, frames: int, time_info, status) -> None:
        if status:
            # Print non-fatal stream warnings (xruns, etc.) to stderr.
            print(status, file=sys.stderr)
        # Copy to decouple from sounddevice's buffer.
        q.put(indata.copy())

    # Open input stream
    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype="float32",
        callback=cb,
        blocksize=FRAME_SAMPLES,  # one VAD frame per callback (e.g., 320 @ 20 ms)
        device=device,
    ):
        print("🎙️ Sprich jetzt … (Pause zum Beenden)")

        voiced = False
        last_voice_t: Optional[float] = None
        start_t = time.time()

        collected_pcm = bytearray()  # collected speech frames (PCM16)
        buffered = bytearray()       # staging buffer to slice exact 20 ms frames

        BYTES_PER_FRAME = FRAME_SAMPLES * 2  # 16-bit mono → 2 bytes/sample

        while True:
            # Try to grab the next chunk from the callback
            try:
                chunk = q.get(timeout=0.5)  # float32 ndarray shape: (N, CHANNELS)
            except queue.Empty:
                chunk = None

            now = time.time()

            # Stop if silence reached after speech OR hard time cap reached
            if chunk is None:
                if voiced and last_voice_t and (now - last_voice_t) >= silence_stop_s:
                    break
                if (now - start_t) > max_utterance_s:
                    break
                continue

            # Convert to PCM16 and append to staging buffer
            pcm_bytes = _pcm16le_from_float32(chunk.reshape(-1))
            buffered.extend(pcm_bytes)

            # Process exact 20 ms frames for VAD
            while len(buffered) >= BYTES_PER_FRAME:
                frame = bytes(buffered[:BYTES_PER_FRAME])
                del buffered[:BYTES_PER_FRAME]

                is_speech = vad.is_speech(frame, SAMPLE_RATE)
                if is_speech:
                    voiced = True
                    last_voice_t = now
                    collected_pcm.extend(frame)
                # else: ignore leading/trailing silence; stopping is handled by timers

            # Stop checks again (covers the case where we appended speech this loop)
            if voiced and last_voice_t and (now - last_voice_t) >= silence_stop_s:
                break
            if (now - start_t) > max_utterance_s:
                break

    if not collected_pcm:
        return ""

    # Optional WAV dump for debugging:
    # _write_wav(Path("last_utterance.wav"), bytes(collected_pcm))

    # Transcribe: raw PCM16 → float32 [-1..1] NumPy array
    audio_np = np.frombuffer(bytes(collected_pcm), dtype=np.int16).astype(np.float32) / 32768.0

    model = _get_model()
    segments, info = model.transcribe(
        audio=audio_np,                              # NumPy array (not bytes)
        language=None if lang_hint == "auto" else lang_hint,
        vad_filter=False,                            # VAD already performed
        beam_size=5,
        best_of=5,
    )
    text = "".join(seg.text for seg in segments).strip()
    return text
