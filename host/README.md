# Spice Extractor v2

Ein Python-Tool, das aus einem Gerichtsnamen die typischen **Gewürze** extrahiert – robust, sprachsensibel und mit Wikipedia-Plausibilitätscheck.

## Features

- **Wikipedia-Check (DE/EN):** prüft, ob es sich um ein echtes Gericht handelt, ignoriert Disambiguation.
- **LLM-Anbindung (Ollama, z. B. Mistral):** erzeugt eine reine JSON-Liste mit typischen Gewürzen.
- **Synonym-Normalisierung:** wandelt Schreibweisen & Synonyme in deutsche Standardformen um.
- **Robust gegen Halluzinationen:** Fallback-Modus, wenn LLM kein valides JSON liefert.
- **CLI mit Optionen:** Standardausgabe oder reine JSON-Liste (`--json-only`).

## Installation

```bash
pip install requests
```

## Nutzung

```bash
# Standard
python3 spice_extractor_v2.py "chili con carne"

# Nur JSON-Liste
python3 spice_extractor_v2.py --json-only "pho bo"

# Mit Debug-Ausgabe
SPICE_DEBUG=1 python3 spice_extractor_v2.py "ratatouille"
```

## Voraussetzungen

- Python 3.8+
- [Ollama](https://ollama.ai/) installiert und Modell lokal verfügbar (Default: `mistral`).

## Beispielausgabe

```bash
== Gericht ==
chili con carne
== Gewürze ==
chili, paprika, pfeffer, kreuzkümmel

JSON: ["chili", "paprika", "pfeffer", "kreuzkümmel"]
```

## Erweiterungsmöglichkeiten

- **Synonym-Liste auslagern:** externe JSON-/YAML-Datei zur leichteren Pflege.
- **Multi-Language-Support:** zusätzliche Wikipedia-Sprachen (FR, ES …).
- **Testsuite:** Unit-Tests für Parser, Normalisierung und Wikipedia-Check.
- **Spice-Lexikon:** optionales Whitelisting für eine kontrollierte Gewürzliste.

---

✦ Entwickelt für Projekte, die kulinarische Daten weiterverarbeiten wollen (z. B. Rezeptanalyse, Küchen-Chatbots, Food-NLP).
