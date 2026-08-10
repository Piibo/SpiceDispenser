# SpiceDispenser — KI-gesteuerter Gewürzautomat

**Gericht nennen (per Sprache oder Text) → ein lokales LLM bestimmt die typischen Gewürze samt Grammmengen → die Maschine dosiert sie automatisch.**

▶️ **[Demo-Video auf YouTube](https://youtu.be/Efl0KOGhpKA)** · 📁 Mehr Kontext auf meiner [Portfolio-Seite](https://github.com/Piibo/portfolio/tree/main/projekte/spice-dispenser)

## Wie es funktioniert

```
 Sprache/Text ──▶ host/ai_host.py (Python, PC)          src/ (C++, ESP32-C6)
                  ├─ faster-whisper ASR + VAD            ├─ empfängt Gewürzplan (JSON)
                  ├─ LLM via Ollama (Mistral, lokal)     ├─ Schrittmotor: Karussell
                  ├─ Wikipedia-Plausibilitätscheck       │   zur Gewürzposition
                  ├─ Whitelist + Synonym-Normalisierung  ├─ Servo: Dosiermechanik
                  └─ Mengen in Gramm, skalierbar    ──▶  └─ Taster + Statusanzeige
```

1. **Eingabe:** Gericht nennen — getippt oder gesprochen. Spracherkennung läuft komplett lokal (faster-whisper + webrtcvad, kein Cloud-Dienst).
2. **Gewürz-Bestimmung** (`host/ai_host.py`): Ein lokales LLM (Ollama, Default Mistral) liefert eine JSON-Liste typischer Gewürze mit Grammmengen, skalierbar nach Portionen und Schärfe. Gegen Halluzinationen abgesichert: Wikipedia-Check (DE/EN), ob das Gericht existiert; Fuzzy-Abgleich gegen eine Gewürz-Whitelist; Synonym-Normalisierung; Fallback bei invalidem JSON; Mengen-Grenzen mit Warnungen.
3. **Dosierung** (`src/`): Der ESP32-C6 empfängt den Plan, ein Schrittmotor dreht das Gewürzkarussell zur richtigen Position, ein Servo koppelt die Dosiermechanik und gibt die Menge über kalibrierte Umdrehungen (g/Umdrehung pro Position) aus.

## Hardware

ESP32-C6 DevKit · Schrittmotor (STEP/DIR-Treiber) · Servo für die Dosier-Kopplung · Taster · Gewürzkarussell mit bis zu 32 Positionen

## Setup

**Host (PC):**

```bash
pip install -r host/requirements.txt   # + Ollama installieren, Modell laden (z. B. mistral)
python host/ai_host.py --voice         # oder mit Gerichtsnamen als Argument
```

**Firmware (PlatformIO):** `src/secrets.h.example` nach `src/secrets.h` kopieren und WLAN-Zugangsdaten + Host-IP eintragen (die Datei ist gitignored, landet also nie im Repo), dann auf das ESP32-C6-Board flashen:

```bash
pio run -t upload
```

## Projektstruktur

```
src/        ESP32-Firmware (C++): main, Mechanik (mech), WLAN/AI-Anbindung (ai), Konfiguration
host/       Python-Host: LLM-Gewürzextraktion (ai_host.py), Spracheingabe (speech_input.py)
include/    Display-Icons
platformio.ini
```
