# SpiceDispenser

AI-assisted **spice dispenser** built with an **ESP32-C6** (firmware in this root via PlatformIO) and a **Python AI backend** in `/host`.

- Firmware (ESP32-C6): `src/`, `include/`, `platformio.ini`
- AI backend (Python/Ollama): `host/`

---

## ✦ What it does

- Positions spice containers and dispenses precise amounts
- Servo-based coupling: one stepper motor handles both translation and dispensing
- OLED UI with rotary encoder and button navigation
- Voice input for dish recognition (AI-powered)
- Manual “single spice” dispensing mode
- Integration with a local AI service (via HTTP over Wi-Fi)

---

## 🧭 Repository layout

```
SpiceDispenser/
├── host/                 # Python AI backend (runs on your PC)
│   ├── ai_host.py
│   ├── speech_input.py
│   ├── requirements.txt
│   └── README.md
├── include/              # Firmware headers
│   ├── pins.h
│   ├── config.h
│   ├── ai.h
│   ├── mech.h
│   ├── ui.h
│   ├── icons.h
│   └── lexicon.h
├── src/                  # Firmware sources
│   ├── main.cpp
│   ├── ai.cpp
│   ├── mech.cpp
│   └── ui.cpp
├── platformio.ini        # PlatformIO config (ESP32-C6 + Arduino)
└── README.md             # (this file)
```

---

## ⚙️ Hardware overview

| Component                      | Function                               | Pin                                                       |
| ------------------------------ | -------------------------------------- | --------------------------------------------------------- |
| Stepper driver (A4988/DRV8825) | Moves and dispenses                    | `STEP_PIN`, `DIR_PIN`, `EN_PIN`                           |
| Servo                          | Couples/decouples dispensing mechanism | `SERVO_PIN`                                               |
| Rotary encoder                 | UI navigation                          | `ROT_CLK`, `ROT_DT`                                       |
| Button 1                       | Confirm / Start / Voice                | `BTN_SEL`                                                 |
| Button 2                       | Manual servo toggle                    | `BTN_SERVO`                                               |
| OLED Display (SSD1309 128×64)  | UI output                              | `OLED_CLK`, `OLED_MOSI`, `OLED_CS`, `OLED_DC`, `OLED_RST` |

> Adjust your wiring and pin mapping in `include/pins.h`.

---

## 🧠 AI Backend (runs on your PC)

The ESP32 connects via Wi-Fi to a local backend located in `/host`.

Endpoints expected by the firmware:

| Endpoint     | Method | Description                                 |
| ------------ | ------ | ------------------------------------------- |
| `/health`    | GET    | Readiness check                             |
| `/spiceplan` | POST   | Request spice plan for a given dish         |
| `/voiceplan` | POST   | Voice-based dish recognition and spice plan |

### Configuration (`include/config.h`)

```cpp
inline constexpr const char* WIFI_SSID = "<YOUR_WIFI_SSID>";
inline constexpr const char* WIFI_PW   = "<YOUR_WIFI_PASSWORD>";
inline constexpr const char* AI_URL    = "http://<HOST-IP>:8000/spiceplan";
inline constexpr const char* AI_HEALTH = "http://<HOST-IP>:8000/health";
```

> Replace `<HOST-IP>` with your PC’s local network IP running `ai_host.py`.

---

## 🔧 Building and flashing

This project uses **PlatformIO** with the **Arduino framework**.

```bash
# From the project root:
pio run              # build firmware
pio run --target upload
pio device monitor
```

Target board: `esp32-c6-devkitm-1`  
Libraries auto-install:

- [ESP32Servo](https://github.com/madhephaestus/ESP32Servo)
- [ArduinoJson](https://arduinojson.org/)
- [U8g2](https://github.com/olikraus/u8g2)

---

## 🗣️ How to use

1. Power on → device connects to Wi-Fi → OLED shows **"Lass uns kochen!"**
2. Rotate the knob to select:
   - **AI-Gericht** → short press: voice dish input (AI recognition)
   - **AI-Gericht** → long press: fixed example dish (“chili con carne”)
   - **Einzel-Gewürz** → short press: manual dispensing mode
   - **Einzel-Gewürz** → long press: toggle servo coupling
3. Confirm to start dispensing.

### Serial command (debug mode)

You can trigger a recipe manually via serial:

```bash
dish: spaghetti carbonara
```

The firmware requests the spice plan from the AI backend and executes it automatically.

---

## 📏 Calibration

Adjust per-slot calibration in `include/config.h`:

```cpp
inline float GRAMS_PER_ROTATION[MAX_POS] = {
  2.0f, 1.5f, 1.2f, 1.8f,  // Slots 1–4
  // ...
};
```

Also set your number of containers:

```cpp
inline constexpr int POS_COUNT_DEFAULT = 5;
```

---

## 🧩 Communication flow

```
User input (voice or button)
        ↓
ESP32 sends dish → /spiceplan
        ↓
AI backend (Ollama model) returns spice list
        ↓
UI shows recipe + amounts
        ↓
Stepper + servo dispense exact grams
```

---

## ✅ Troubleshooting

| Issue                   | Possible cause                                |
| ----------------------- | --------------------------------------------- |
| `[WIFI] FAILED`         | Wrong SSID/password or 5 GHz network          |
| `[AI][plan] http_error` | Backend not running or wrong IP               |
| OLED stays blank        | Check display pins and reset wiring           |
| Servo not moving        | Verify `SERVO_PIN` and power source           |
| Stepper stalls          | Adjust driver current or delays in `config.h` |

---

## 🗃️ Python backend setup (summary)

From `/host`:

```bash
python -m venv .venv
source .venv/bin/activate        # Windows: .venv\Scripts\activate
pip install -r requirements.txt
python ai_host.py
```

Backend uses **Ollama** (default model: `mistral`) for local inference.

See `/host/README.md` for detailed usage instructions.

---

## 📄 License & Credits

Developed as part of the **Sketching with Hardware (2025)** course project.  
Combines embedded motion control, local AI inference, and interactive design.

✦ Hardware: ESP32-C6 DevKitM-1  
✦ Software: Arduino (PlatformIO) + Python + Ollama  
✦ Authors: Peter Trenkle, Nina Binder

---

**Enjoy precise, AI-powered seasoning! 🍲**
