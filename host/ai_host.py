#!/usr/bin/env python3
"""
Spice Extractor v2
- Wikipedia-DE/EN Plausibilitätscheck (robuster, sucht mehrere Treffer, ignoriert Disambiguation)
- Hartes JSON aus LLM erzwingen (strict prompt + Fallback + Codefence-Strip + erste JSON-Array-Extraktion)
- Umlaut- & Unicode-Normalisierung, Synonymmapping in DE-Schreibweise
- Fehlerhandhabung (Timeouts), optionale Debug-Ausgabe
- CLI mit reiner JSON-Ausgabe (--json-only)

Abhängigkeiten: requests
LLM: lokales Ollama (Standard: mistral)
"""

from __future__ import annotations
import os, sys, json, re, subprocess, urllib.parse, requests, unicodedata, time
from functools import lru_cache
from typing import List, Tuple, Optional

# ---- Konfiguration ----
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "mistral")
OLLAMA_BIN = os.environ.get("OLLAMA_BIN", "ollama")
OLLAMA_TIMEOUT = int(os.environ.get("OLLAMA_TIMEOUT", "25"))  # Sekunden für Subprozess
WIKI_USER_AGENT = os.environ.get("WIKI_USER_AGENT", "SpiceDispenser/2.0 (+https://example.local)")
DEBUG = bool(int(os.environ.get("SPICE_DEBUG", "0")))

# Synonyme/Normalisierung (keine Filterung!)
SPICE_SYNONYMS = {
    # Kreuzkümmel
    "kreuzkuemmel": "kreuzkümmel", "kumin": "kreuzkümmel", "cumin": "kreuzkümmel",
    # Paprika
    "paprikapulver": "paprika", "geraeuchertes paprikapulver": "rauchpaprika",
    "geräuchertes paprikapulver": "rauchpaprika", "smoked paprika": "rauchpaprika",
    # Chili
    "chilipulver": "chili", "chiliepulver": "chili", "chili flakes": "chiliflocken",
    # Kurkuma
    "curcuma": "kurkuma", "curcumapulver": "kurkuma",
    # Pfeffer/Muskat u.a.
    "muskatnuss": "muskat", "schwarzer pfeffer": "pfeffer",
    "knoblauchgranulat": "knoblauchpulver", "zwiebelgranulat": "zwiebelpulver",
}

# Zusätzliche Reduktionsregeln (nur kosmetisch)
QUALIFIER_PATTERNS = [
    r"\bgemahlen\b", r"\bpulver\b", r"\bgetrocknet\b", r"\bfrisch\b",
    r"\bsamen\b", r"\bschoten\b", r"\bkörner\b", r"\bkoerner\b",
]

# Keywords, die in der Wikipedia-Zusammenfassung vorkommen sollten
WIKI_DISH_HINTS_DE = [
    "gericht", "speise", "suppe", "eintopf", "sauce", "marinade", "paste",
    "auflauf", "salat", "dessert", "kuchen", "teig", "stew", "curry",
    "nationalgericht", "küche", "kueche",
]
WIKI_DISH_HINTS_EN = [
    "dish", "soup", "stew", "sauce", "curry", "marinade", "paste",
    "salad", "dessert", "pie", "stew", "staple food", "national dish",
]

# -------------- Hilfsfunktionen --------------

def _log(*a):
    if DEBUG:
        print("[DEBUG]", *a, file=sys.stderr)


def _strip_code_fences(text: str) -> str:
    # Entfernt ```json ... ``` oder ``` ... ``` Zäune
    text = re.sub(r"^\s*```(?:json|JSON)?\s*", "", text.strip())
    text = re.sub(r"\s*```\s*$", "", text)
    return text.strip()


def _to_nfkc(s: str) -> str:
    return unicodedata.normalize("NFKC", s)


def _norm_spaces(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def _de_umlaut_variants(s: str) -> str:
    # ae->ä, oe->ö, ue->ü (nur wenn sinnvoll)
    s2 = (s
        .replace("ae", "ä")
        .replace("oe", "ö")
        .replace("ue", "ü")
    )
    return s2

# -------------- Wikipedia --------------

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": WIKI_USER_AGENT})


def _wiki_get(url: str, params=None) -> Optional[dict]:
    try:
        r = SESSION.get(url, params=params or {}, timeout=8)
        if r.status_code == 200:
            return r.json()
    except Exception as e:
        _log("wiki_get error:", e)
    return None


@lru_cache(maxsize=256)
def wiki_find_summary(dish: str) -> Optional[Tuple[str, str, str]]:
    """Versucht erst DE, dann EN. Liefert (lang, title, description/extract) oder None."""
    dish_q = _norm_spaces(_to_nfkc(dish))
    for lang in ("de", "en"):
        # 1) exakter Titel
        url = f"https://{lang}.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(dish_q)}"
        js = _wiki_get(url, {"redirect": "true"})
        if js and js.get("title") and js.get("type") != "disambiguation":
            desc = f"{js.get('description') or ''} {js.get('extract') or ''}".strip()
            if desc:
                return lang, js.get("title"), desc

        # 2) Suche -> bis zu 3 Ergebnisse prüfen
        search_url = f"https://{lang}.wikipedia.org/w/rest.php/v1/search/title"
        s = _wiki_get(search_url, {"q": dish_q, "limit": 3})
        pages = (s or {}).get("pages") or []
        for p in pages:
            t = p.get("title")
            if not t:
                continue
            url2 = f"https://{lang}.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(t)}"
            js2 = _wiki_get(url2, {"redirect": "true"})
            if js2 and js2.get("title") and js2.get("type") != "disambiguation":
                desc = f"{js2.get('description') or ''} {js2.get('extract') or ''}".strip()
                if desc:
                    return lang, js2.get("title"), desc
    return None


def is_known_dish(dish: str) -> bool:
    hit = wiki_find_summary(dish)
    if not hit:
        return False
    lang, title, desc = hit
    text = (f"{title} {desc}").lower()
    hints = WIKI_DISH_HINTS_DE if lang == "de" else WIKI_DISH_HINTS_EN
    return any(h in text for h in hints)

# -------------- LLM & Parsing --------------

def run_ollama(prompt: str) -> str:
    try:
        p = subprocess.run(
            [OLLAMA_BIN, "run", OLLAMA_MODEL],
            input=prompt.encode("utf-8"),
            capture_output=True,
            timeout=OLLAMA_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError("ollama timeout")
    if p.returncode != 0:
        raise RuntimeError(p.stderr.decode(errors="ignore"))
    return p.stdout.decode("utf-8", errors="ignore").strip()


def parse_json_list(text: str) -> Optional[List[str]]:
    if not text:
        return None
    t = _strip_code_fences(text)
    # 1) Direktes JSON?
    try:
        v = json.loads(t)
        if isinstance(v, list):
            return [x for x in v if isinstance(x, str)]
    except Exception:
        pass
    # 2) erste JSON-Liste aus Text extrahieren (nicht-gierige Klammerung)
    m = re.search(r"\[.*?\]", t, flags=re.S)
    if m:
        try:
            v = json.loads(m.group(0))
            if isinstance(v, list):
                return [x for x in v if isinstance(x, str)]
        except Exception:
            pass
    return None


def normalize_spices(items: List[str]) -> List[str]:
    out: List[str] = []
    for it in items:
        s = _norm_spaces(_to_nfkc(it)).lower()
        s = s.replace("(", "").replace(")", "")
        # Qualifier entfernen
        for pat in QUALIFIER_PATTERNS:
            s = re.sub(pat, "", s)
        s = _norm_spaces(s)
        # ae/oe/ue -> ä/ö/ü
        s = _de_umlaut_variants(s)
        # Synonyme anwenden
        s = SPICE_SYNONYMS.get(s, s)
        out.append(s)
    # Deduplizieren (stabile Reihenfolge)
    seen, uniq = set(), []
    for s in out:
        if s and s not in seen:
            seen.add(s)
            uniq.append(s)
    return uniq


def build_llm_prompt(dish: str) -> str:
    return (
        "Du bekommst NUR den Namen eines real existierenden Gerichts.\n"
        "Antworte **AUSSCHLIESSLICH** mit einer **reinen JSON-Liste** (Array von Strings) der in Deutschland typischen \n"
        "**Gewürze** für dieses Gericht. **Nur Gewürze** – keine frischen Kräuter (wenn als 'frisch' benannt),\n"
        "keine Öle, keine Mengen, keine Erklärungen. Alles in **Kleinbuchstaben**.\n\n"
        "Wenn du unsicher bist oder es keine typischen Gewürze gibt: gib **[]** zurück.\n\n"
        f"Gericht: \"{dish.strip()}\"\n"
    )


def extract_spices_for_dish(dish: str) -> Tuple[List[str], str]:
    # Wenn es KEIN echtes Gericht ist -> leere Liste
    if not is_known_dish(dish):
        return [], "(not a known dish)"

    prompt = build_llm_prompt(dish) + "Antwort:"
    out = run_ollama(prompt)
    lst = parse_json_list(out)

    if not lst:
        # Fallback: ein Gewürz pro Zeile anfordern und selbst in Liste packen
        prompt2 = (
            f"Liste die typischen **Gewürze** für \"{dish.strip()}\" auf – eine pro Zeile, "
            "Kleinbuchstaben, ohne Erklärungen. Wenn unsicher: keine Zeilen."
        )
        out2 = run_ollama(prompt2)
        cand = [w.strip("-•:\t ").lower() for w in out2.splitlines() if w.strip()]
        lst = [re.sub(r"[().]", "", c) for c in cand]

    return normalize_spices(lst or []), out

# -------------- CLI --------------

def main(argv: List[str]):
    import argparse
    ap = argparse.ArgumentParser(description="Extrahiert Gewürze aus einem Gerichts-Namen")
    ap.add_argument("dish", nargs="*", help="z. B. 'chili con carne'")
    ap.add_argument("--json-only", action="store_true", help="Nur die JSON-Liste ausgeben")
    args = ap.parse_args(argv)

    if args.dish:
        dish = " ".join(args.dish)
    else:
        dish = input("Gericht (z.B. 'chili con carne' / 'chicken marinade'): ").strip()

    start = time.time()
    spices, raw = extract_spices_for_dish(dish)
    dur = time.time() - start

    if args.json_only:
        print(json.dumps(spices, ensure_ascii=False))
        return

    print("\n== Gericht ==")
    print(dish)
    print("== Gewürze ==")
    if spices:
        print(", ".join(spices))
    else:
        print("(kein bekanntes Gericht → keine Gewürze)")
    print("\nJSON:", json.dumps(spices, ensure_ascii=False))
    if DEBUG:
        print(f"\n[debug] duration={dur:.2f}s")
        print("\n-- RAW --\n", raw)


if __name__ == "__main__":
    main(sys.argv[1:])
