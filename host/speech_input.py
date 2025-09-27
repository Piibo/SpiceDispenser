# speech_input.py (fixed)
import queue, sys, time, wave
from pathlib import Path

import numpy as np
import sounddevice as sd
import webrtcvad
from faster_whisper import WhisperModel

SAMPLE_RATE = 16000      # Whisper-Standard
CHANNELS = 1
FRAME_MS = 20            # 10, 20 oder 30 ms Frames für webrtcvad
FRAME_SAMPLES = int(SAMPLE_RATE * FRAME_MS / 1000)  # z.B. 320 Samples bei 20 ms

# ASR-Modell einmal global laden (warm)
WHISPER_MODEL_NAME = "small"          # 'base', 'medium', 'large-v3' optional
WHISPER_COMPUTE_TYPE = "int8"         # int8 (schnell), sonst 'float16'/'float32'

_model = None
def _get_model():
    global _model
    if _model is None:
        _model = WhisperModel(WHISPER_MODEL_NAME, compute_type=WHISPER_COMPUTE_TYPE)
    return _model

def _pcm16le_from_float32(data_f32: np.ndarray) -> bytes:
    """float32 [-1..1] -> int16 PCM Bytes"""
    data = np.clip(data_f32, -1.0, 1.0)
    data_i16 = (data * 32767.0).astype(np.int16)
    return data_i16.tobytes()

def _write_wav(path: Path, pcm_bytes: bytes):
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm_bytes)

def transcribe_once(lang_hint: str = "de", max_utterance_s: float = 12.0,
                    silence_stop_s: float = 0.8, device=None) -> str:
    """
    Nimmt einmalig eine Äußerung vom Mikro auf (automatisch durch VAD) und transkribiert sie.
    - lang_hint: "de" | "en" | "auto"
    """
    vad = webrtcvad.Vad(2)  # 0-3 (aggressiv). 2 ist guter Kompromiss
    q = queue.Queue()

    def cb(indata, frames, time_info, status):
        if status:
            print(status, file=sys.stderr)
        q.put(indata.copy())

    # Audio stream starten
    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype="float32",
        callback=cb,
        blocksize=FRAME_SAMPLES,  # Frames pro Callback; 320 bei 20ms @16kHz
        device=device
    ):
        print("🎙️ Sprich jetzt … (Pause zum Beenden)")
        voiced = False
        last_voice_t = None
        start_t = time.time()

        collected_pcm = bytearray()   # gesammelte Speech-Frames (PCM16)
        buffered = bytearray()        # Frame-Puffer für VAD in 20ms-Schritten

        while True:
            try:
                chunk = q.get(timeout=0.5)  # chunk ist float32 ndarray shape (N, 1)
            except queue.Empty:
                chunk = None

            now = time.time()
            if chunk is None:
                # Abbruchkriterien prüfen
                if voiced and last_voice_t and (now - last_voice_t) >= silence_stop_s:
                    break
                if (now - start_t) > max_utterance_s:
                    break
                continue

            # in 20ms Frames teilen -> VAD
            pcm_bytes = _pcm16le_from_float32(chunk.reshape(-1))  # float32 -> int16 PCM
            buffered.extend(pcm_bytes)

            BYTES_PER_FRAME = FRAME_SAMPLES * 2  # 16-bit mono -> 2 Bytes/Sample
            while len(buffered) >= BYTES_PER_FRAME:
                frame = bytes(buffered[:BYTES_PER_FRAME])
                buffered = buffered[BYTES_PER_FRAME:]

                is_speech = vad.is_speech(frame, SAMPLE_RATE)
                if is_speech:
                    voiced = True
                    last_voice_t = now
                    collected_pcm.extend(frame)
                else:
                    # Stille vor Start ignorieren; nach Start beenden wir über silence_stop_s
                    pass

            # Abbruch wenn ausreichend Stille / max Länge
            if voiced and last_voice_t and (now - last_voice_t) >= silence_stop_s:
                break
            if (now - start_t) > max_utterance_s:
                break

    if not collected_pcm:
        return ""

    # Optional: WAV sichern (Debug)
    # _write_wav(Path("last_utterance.wav"), bytes(collected_pcm))

    # ---- Transkription: rohes PCM -> float32 NumPy-Array [-1..1] ----
    audio_np = np.frombuffer(bytes(collected_pcm), dtype=np.int16).astype(np.float32) / 32768.0

    model = _get_model()
    segments, info = model.transcribe(
        audio=audio_np,                              # <<< WICHTIG: NumPy-Array, KEIN bytes()
        language=None if lang_hint == "auto" else lang_hint,
        vad_filter=False,                            # VAD schon erfolgt
        beam_size=5,
        best_of=5
    )
    text = "".join(seg.text for seg in segments).strip()
    return text
