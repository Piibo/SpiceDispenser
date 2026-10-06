# SpiceDispenser: Host AI Service

Local Python backend used by the ESP32 firmware.  
Provides HTTP endpoints to create spice plans from a dish name and (optionally) via local speech recognition.

## Files

- `ai_host.py`: HTTP API server (expects requests from the ESP32)
- `speech_input.py`: helper to capture and transcribe one utterance via microphone (used by the server for `/voiceplan` if configured)
- `requirements.txt`: Python dependencies

## Install

```bash
python -m venv .venv
# Windows:
.venv\Scripts\activate
# macOS/Linux:
# source .venv/bin/activate

pip install -r requirements.txt
```

## Run the server

```bash
# Simple (builtin uvicorn)
uvicorn ai_host:app --host 0.0.0.0 --port 8000 --reload
```

The ESP32 should point to:

```
AI_URL    = http://<HOST-IP>:8000/spiceplan
AI_HEALTH = http://<HOST-IP>:8000/health
```

> Replace `<HOST-IP>` with your computer’s LAN IP.

---

## Endpoints

### `GET /health`

Health check.

**Response (200):**

```json
{ "status": "ok" }
```

---

### `POST /spiceplan`

Create a spice plan from a dish name.

**Request JSON:**

```json
{
  "dish": "chili con carne",
  "servings": 2,
  "intensity": "medium"
}
```

**Response JSON (example):**

```json
{
  "title": "chili con carne",
  "spices": [
    { "name": "chili", "grams": 2.0 },
    { "name": "paprika", "grams": 1.5 },
    { "name": "pfeffer", "grams": 0.5 },
    { "name": "kreuzkümmel", "grams": 1.0 }
  ]
}
```

---

### `POST /voiceplan`

Capture speech locally, transcribe, and return a spice plan.

- Relies on `speech_input.py` (uses `sounddevice`, `webrtcvad`, `faster-whisper`).
- Can accept optional parameters like `lang`, `servings`, `intensity`.

**Request JSON (example):**

```json
{
  "lang": "de",
  "servings": 2,
  "intensity": "medium"
}
```

**Response JSON:** same schema as `/spiceplan`, plus optional diagnostic fields like `"transcript"` depending on your implementation.

---

## CLI examples

### cURL

```bash
# health
curl http://localhost:8000/health

# spice plan
curl -X POST http://localhost:8000/spiceplan   -H "Content-Type: application/json"   -d '{"dish": "ratatouille", "servings": 2, "intensity": "medium"}'
```

### Python (requests)

```python
import requests

base = "http://localhost:8000"

print(requests.get(f"{base}/health").json())

payload = {"dish": "chili con carne", "servings": 2, "intensity": "medium"}
print(requests.post(f"{base}/spiceplan", json=payload).json())
```

---

## Voice helper (optional)

To test the microphone transcription alone:

```python
from speech_input import transcribe_once
print(transcribe_once(lang_hint="de"))
```

`speech_input.py` defaults:

- 16 kHz mono, 20 ms VAD frames
- Stops after 0.8 s silence or 12 s max
- Model: faster-whisper `"small"` (int8 compute)

---

## Notes

- Keep the whitelist of allowed spices in the firmware (`include/lexicon.h`) if you want stricter matching on-device.
- All private data (Wi-Fi SSID, passwords, IPs) belong in the firmware’s `config.h` and should be placeholders in public repos.
