# SpAice — KI-gesteuerter Gewürzautomat

**Gericht nennen (per Sprache oder Text) → ein lokales LLM bestimmt die typischen Gewürze samt Grammmengen → die Maschine dosiert sie automatisch.**

▶️ **[Demo-Video auf YouTube](https://youtu.be/Efl0KOGhpKA)** · 📁 Mehr Kontext auf meiner [Portfolio-Seite](https://github.com/Piibo/portfolio/tree/main/projekte/spice-dispenser)

## Wie es funktioniert

```
 Sprache/Text ──▶ host/ai_host.py (Python, PC)          src/ + include/ (C++, ESP32-C6)
                  ├─ faster-whisper ASR + VAD            ├─ OLED-Menü + Drehknopf
                  ├─ LLM via Ollama (Mistral, lokal)     ├─ holt den Gewürzplan (JSON, WLAN)
                  ├─ Wikipedia-Plausibilitätscheck       ├─ Schrittmotor: Linearachse
                  ├─ Whitelist + Synonym-Normalisierung  │   zur Gewürzposition
                  └─ Mengen in Gramm, skalierbar    ──▶  └─ Servo: Kopplung zum Dosieren
```

1. **Eingabe:** Gericht nennen, getippt oder gesprochen. Die Spracherkennung läuft komplett lokal (faster-whisper + webrtcvad, kein Cloud-Dienst).
2. **Gewürz-Bestimmung** (`host/ai_host.py`): Ein lokales LLM (Ollama, Default Mistral) liefert eine JSON-Liste typischer Gewürze mit Grammmengen, skalierbar nach Portionen und Schärfe. Gegen Halluzinationen abgesichert: Wikipedia-Check (DE/EN), ob das Gericht existiert; Fuzzy-Abgleich gegen eine Gewürz-Whitelist; Synonym-Normalisierung; Fallback bei invalidem JSON; Mengen-Grenzen mit Warnungen.
3. **Bedienung am Gerät:** OLED-Display mit Drehknopf. Im AI-Modus spricht man das Gericht ein und bestätigt die vorgeschlagene Liste. In der **Einzel-Auswahl** stellt man die Menge jedes Gewürzes selbst ein (0,5-g-Schritte). Behälter lassen sich per Sprache umbenennen; der Name wird gegen ein festes Gewürz-Lexikon geprüft.
4. **Dosierung** (`src/mech.cpp`): Ein Schrittmotor fährt die Gewürzbehälter auf einer Linearachse zur richtigen Position, ein Servo koppelt die Dosiermechanik, und die Menge wird über kalibrierte Umdrehungen (g/Umdrehung pro Position) ausgegeben.

## Hardware

- ESP32-C6 DevKit
- Schrittmotor mit A4988-Treiber auf einer Linearachse mit fünf Gewürzbehältern (Firmware ausgelegt für bis zu 32 Positionen)
- Servo für die Kopplung zwischen Fahren und Dosieren, dazu ein Taster
- OLED-Display 128×64 (SSD1309, SPI)
- Drehknopf (Drehgeber mit Taster)
- Mikrofon am PC (die Spracheingabe läuft auf dem Host)

Die Pinbelegung steht in `include/pins.h`.

## Setup

**Host (PC):**

```bash
pip install -r host/requirements.txt   # + Ollama installieren, Modell laden: ollama pull mistral
python host/ai_host.py --serve         # HTTP-Server für den ESP32 auf Port 8000
```

Zum Testen ohne Gerät: `python host/ai_host.py "Chili con carne"` oder `python host/ai_host.py --voice`. Details zu den Endpunkten in `host/README.md`.

**Firmware (PlatformIO):** `include/secrets.h.example` nach `include/secrets.h` kopieren und WLAN-Zugangsdaten + Host-IP eintragen (die Datei ist gitignored, landet also nie im Repo), dann auf das ESP32-C6-Board flashen:

```bash
pio run -t upload
```

## Projektstruktur

```
src/        ESP32-Firmware (C++): Ablauf (main), Mechanik (mech), Display-Menü (ui), WLAN/AI-Anbindung (ai)
include/    Konfiguration, Pinbelegung, Gewürz-Lexikon, Display-Icons, Header
host/       Python-Host: LLM-Gewürzextraktion + HTTP-Server (ai_host.py), Spracheingabe (speech_input.py)
platformio.ini
```
