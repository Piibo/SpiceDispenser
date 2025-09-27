#!/usr/bin/env python3
"""
Spice Extractor v6
- Wikipedia-DE/EN Plausibilitätscheck
- Strikter Prompt mit Whitelist
- Mengen in GRAMM je Gewürz (skalierbar nach Portionen + Intensität)
- Harte Untergrenze, softes Maximum mit Warnungen
- Normalisierung + Fuzzy-Whitelist
- Sprachmodus (--voice) mit Whisper-ASR (speech_input.py)
"""

from __future__ import annotations
import os, sys, json, re, subprocess, urllib.parse, requests, unicodedata, time
from functools import lru_cache
from typing import List, Tuple, Optional, Dict

# ================== Konfiguration ==================
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "mistral")
OLLAMA_BIN = os.environ.get("OLLAMA_BIN", "ollama")
OLLAMA_TIMEOUT = int(os.environ.get("OLLAMA_TIMEOUT", "25"))
WIKI_USER_AGENT = os.environ.get("WIKI_USER_AGENT", "SpiceDispenser/6.0 (+https://example.local)")
DEBUG = bool(int(os.environ.get("SPICE_DEBUG", "0")))

# Softes Max: Faktor über typischem Maximum, bevor hart gedeckelt wird
SOFT_MAX_FACTOR = 2.0

# ---------- Synonyme/Normalisierung (harte Map) ----------
SPICE_SYNONYMS: Dict[str, str] = {
    "kreuzkuemmel": "kreuzkümmel", "kumin": "kreuzkümmel", "cumin": "kreuzkümmel", "jeera": "kreuzkümmel",
    "paprikapulver": "paprika", "geraeuchertes paprikapulver": "rauchpaprika",
    "geräuchertes paprikapulver": "rauchpaprika", "smoked paprika": "rauchpaprika",
    "sweet paprika": "paprika", "hot paprika": "paprika",
    "chilipulver": "chili", "chiliepulver": "chili", "chili flakes": "chiliflocken",
    "chili flake": "chiliflocken", "red pepper flakes": "chiliflocken",
    "cayenne": "cayennepfeffer", "cayenne pepper": "cayennepfeffer",
    "curcuma": "kurkuma", "curcumapulver": "kurkuma", "turmeric": "kurkuma",
    "muskatnuss": "muskat", "schwarzer pfeffer": "pfeffer", "black pepper": "pfeffer",
    "white pepper": "weißpfeffer", "grüner pfeffer": "pfeffer", "green pepper": "pfeffer",
    "pink pepper": "rosa pfeffer",
    "knoblauchgranulat": "knoblauchpulver", "garlic powder": "knoblauchpulver",
    "zwiebelgranulat": "zwiebelpulver", "onion powder": "zwiebelpulver",
    "ground ginger": "ingwer", "ginger powder": "ingwer", "getrockneter ingwer": "ingwer",
    "cinnamon powder": "zimt", "ground cinnamon": "zimt",
    "clove": "nelken", "cloves": "nelken",
    "cardamom": "kardamom", "green cardamom": "kardamom",
    "coriander": "koriander", "coriander seeds": "koriandersamen",
    "mustard seeds": "senfsamen", "gelbe senfsamen": "senfsamen",
    "fennel seeds": "fenchelsamen",
    "fenugreek": "bockshornklee", "methi": "bockshornklee",
    "star anise": "sternanis", "anise": "anis",

    # häufige Fehler/Schreibweisen
    "schwarzpepper": "pfeffer", "schwarz pfeffer": "pfeffer", "pepper": "pfeffer"
}

# ---------- Erlaubte Gewürze (Whitelist) ----------
SPICE_WHITELIST = {
    "salz", "pfeffer", "weißpfeffer", "rosa pfeffer", "szechuanpfeffer",
    "paprika", "rauchpaprika", "chili", "chiliflocken", "cayennepfeffer",
    "kreuzkümmel", "koriander", "koriandersamen", "kurkuma", "ingwer",
    "zimt", "muskat", "nelken", "kardamom", "fenchel", "fenchelsamen",
    "senf", "senfsamen", "bockshornklee", "anis", "sternanis",
    "piment", "lorbeer", "kümmel", "knoblauchpulver", "zwiebelpulver",
    "currypulver", "garam masala", "tandoori masala"
}

# ---------- Mengen-Logik (g pro Portion) ----------
GRAM_RULES_PER_SERV: Dict[str, Tuple[float, float]] = {
    "salz": (0.2, 1.5),
    "pfeffer": (0.1, 0.8),
    "weißpfeffer": (0.1, 0.8),
    "rosa pfeffer": (0.05, 0.3),
    "szechuanpfeffer": (0.05, 0.3),
    "paprika": (0.3, 1.5),
    "rauchpaprika": (0.2, 1.0),
    "chili": (0.05, 0.5),
    "chiliflocken": (0.05, 0.4),
    "cayennepfeffer": (0.03, 0.3),
    "kreuzkümmel": (0.1, 0.6),
    "koriander": (0.05, 0.5),
    "koriandersamen": (0.05, 0.6),
    "kurkuma": (0.05, 0.4),
    "ingwer": (0.05, 0.6),
    "zimt": (0.05, 0.4),
    "muskat": (0.02, 0.15),
    "nelken": (0.01, 0.1),
    "kardamom": (0.02, 0.2),
    "fenchel": (0.05, 0.6),
    "fenchelsamen": (0.05, 0.6),
    "senf": (0.05, 0.5),
    "senfsamen": (0.05, 0.6),
    "bockshornklee": (0.02, 0.2),
    "anis": (0.02, 0.2),
    "sternanis": (0.01, 0.1),
    "piment": (0.02, 0.2),
    "lorbeer": (0.01, 0.05),
    "kümmel": (0.05, 0.6),
    "knoblauchpulver": (0.05, 0.6),
    "zwiebelpulver": (0.1, 0.8),
    "currypulver": (0.2, 1.0),
    "garam masala": (0.1, 0.8),
    "tandoori masala": (0.1, 0.8),
}
DEFAULT_RULE = (0.05, 0.5)
INTENSITY_SCALE = {"mild": 0.85, "medium": 1.0, "bold": 1.25}

# ---------- Qualifier ----------
QUALIFIER_PATTERNS = [
    r"\bgemahlen\b", r"\bpulver\b", r"\bgetrocknet\b", r"\bfrisch\b",
    r"\bsamen\b", r"\bschoten\b", r"\bkörner\b", r"\bkoerner\b",
]

# ---------- Wikipedia Hints ----------
WIKI_DISH_HINTS_DE = ["gericht", "speise", "suppe", "eintopf", "sauce", "marinade", "paste",
    "auflauf", "salat", "dessert", "kuchen", "teig", "stew", "curry",
    "nationalgericht", "küche", "kueche"]
WIKI_DISH_HINTS_EN = ["dish", "soup", "stew", "sauce", "curry", "marinade", "paste",
    "salad", "dessert", "pie", "stew", "staple food", "national dish"]

# ================== Hilfsfunktionen ==================
def _log(*a):
    if DEBUG:
        print("[DEBUG]", *a, file=sys.stderr)

def _strip_code_fences(text: str) -> str:
    text = re.sub(r"^\s*```(?:json|JSON)?\s*", "", text.strip())
    text = re.sub(r"\s*```\s*$", "", text)
    return text.strip()

def _to_nfkc(s: str) -> str:
    return unicodedata.normalize("NFKC", s)

def _norm_spaces(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()

def _de_umlaut_variants(s: str) -> str:
    return s.replace("ae", "ä").replace("oe", "ö").replace("ue", "ü")

def _get_minmax_for(spice: str) -> Tuple[float, float]:
    return GRAM_RULES_PER_SERV.get(spice, DEFAULT_RULE)

def _round_gram(x: float) -> float:
    if x < 0.2:
        return round(x, 2)
    return round(x, 1)

# ================== Wikipedia ==================
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
    dish_q = _norm_spaces(_to_nfkc(dish))
    for lang in ("de", "en"):
        js = _wiki_get(f"https://{lang}.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(dish_q)}", {"redirect": "true"})
        if js and js.get("title") and js.get("type") != "disambiguation":
            desc = f"{js.get('description') or ''} {js.get('extract') or ''}".strip()
            if desc:
                return lang, js.get("title"), desc
        s = _wiki_get(f"https://{lang}.wikipedia.org/w/rest.php/v1/search/title", {"q": dish_q, "limit": 3})
        for p in (s or {}).get("pages") or []:
            t = p.get("title")
            if not t:
                continue
            js2 = _wiki_get(f"https://{lang}.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(t)}", {"redirect": "true"})
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

# ================== LLM & Parsing ==================
def run_ollama(prompt: str) -> str:
    try:
        p = subprocess.run([OLLAMA_BIN, "run", OLLAMA_MODEL],
                           input=prompt.encode("utf-8"),
                           capture_output=True,
                           timeout=OLLAMA_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise RuntimeError("ollama timeout")
    if p.returncode != 0:
        raise RuntimeError(p.stderr.decode(errors="ignore"))
    return p.stdout.decode("utf-8", errors="ignore").strip()

def parse_json_list(text: str) -> Optional[List[str]]:
    if not text:
        return None
    t = _strip_code_fences(text)
    try:
        v = json.loads(t)
        if isinstance(v, list):
            return [x for x in v if isinstance(x, str)]
    except Exception:
        pass
    m = re.search(r"\[.*?\]", t, flags=re.S)
    if m:
        try:
            v = json.loads(m.group(0))
            if isinstance(v, list):
                return [x for x in v if isinstance(x, str)]
        except Exception:
            pass
    return None

def parse_json_spice_grams(text: str) -> Optional[List[dict]]:
    if not text:
        return None
    t = _strip_code_fences(text)
    try:
        v = json.loads(t)
        if isinstance(v, list) and all(isinstance(it, dict) for it in v):
            return [{"name": it.get("name"), "grams": float(it.get("grams", 0.0))} for it in v if isinstance(it.get("name"), str)]
    except Exception:
        pass
    m = re.search(r"\[.*?\]", t, flags=re.S)
    if m:
        try:
            v = json.loads(m.group(0))
            if isinstance(v, list) and all(isinstance(it, dict) for it in v):
                return [{"name": it.get("name"), "grams": float(it.get("grams", 0.0))} for it in v if isinstance(it.get("name"), str)]
        except Exception:
            pass
    return None

# ================== Normalisierung ==================
def normalize_spices(items: List[str]) -> List[str]:
    def _pre(s: str) -> str:
        s = _norm_spaces(_to_nfkc(s)).lower()
        s = re.sub(r"[(){}\[\],;:·•\-_/]+", " ", s)
        for pat in QUALIFIER_PATTERNS:
            s = re.sub(pat, "", s)
        return _de_umlaut_variants(_norm_spaces(s))
    out = []
    for it in items:
        s = _pre(it)
        s = SPICE_SYNONYMS.get(s, s)
        out.append(s)
    seen, uniq = set(), []
    for s in out:
        if s and s not in seen:
            seen.add(s)
            uniq.append(s)
    return uniq

def _fuzzy_match_to_whitelist(token: str) -> Optional[str]:
    t = token.strip().lower()
    if not t:
        return None
    if t in SPICE_WHITELIST:
        return t
    t2 = _norm_spaces(_de_umlaut_variants(re.sub(r"[^\wäöüß ]+", " ", t)))
    if t2 in SPICE_WHITELIST:
        return t2
    def _char_overlap(a, b): return len(set(a) & set(b)) / max(1, len(set(a) | set(b)))
    best, best_score = None, 0.0
    for w in SPICE_WHITELIST:
        score = _char_overlap(t2, w)
        if score > best_score:
            best, best_score = w, score
    return best if best_score >= 0.6 else None

# ================== Prompt ==================
def build_llm_prompt(dish: str, servings: int, intensity: str) -> str:
    allowed = ", ".join(f'"{w}"' for w in sorted(SPICE_WHITELIST))
    return (
        "Du bekommst NUR den Namen eines real existierenden Gerichts.\n"
        "Gib eine REINE JSON-LISTE zurück mit Objekten:\n"
        '[{"name":"<gewürz>","grams":<zahl>}, ...]\n\n'
        f"- 'name' NUR aus [{allowed}]\n"
        "- 'grams' NUR in GRAMM (Zahl, kein String, kein 'g').\n"
        f"- Für {servings} Portion(en), Intensität: {intensity}.\n"
        "- Keine anderen Zutaten.\n"
        "- Wenn keine Gewürze: [].\n\n"
        f'Gericht: "{dish.strip()}"\nAntwort:'
    )


def build_names_prompt(dish: str) -> str:
    allowed = ", ".join(f'"{w}"' for w in sorted(SPICE_WHITELIST))
    return (
        "Du bekommst NUR den Namen eines Gerichts.\n"
        "Antworte mit einer JSON-LISTE (Array von Strings) typischer Gewürze (nur Namen, klein).\n"
        f"Erlaubt: [{allowed}]\n"
        "Keine anderen Zutaten. Wenn keine: [].\n\n"
        f'Gericht: "{dish.strip()}"\nAntwort:'
    )

# ================== Mengenprüfung ==================
def enforce_gram_rules(spice_objs: List[dict], servings: int, intensity: str) -> Tuple[List[dict], List[str]]:
    warnings: List[str] = []
    scale = INTENSITY_SCALE.get(intensity, 1.0)
    names = [x["name"] for x in spice_objs]
    norm_names = normalize_spices(names)
    norm_objs = []
    for i, obj in enumerate(spice_objs):
        nm = _fuzzy_match_to_whitelist(norm_names[i])
        if nm:
            try: grams_val = float(obj.get("grams", 0.0))
            except Exception: grams_val = 0.0
            norm_objs.append({"name": nm, "grams": grams_val})
    agg: Dict[str, float] = {}
    for it in norm_objs:
        agg[it["name"]] = agg.get(it["name"], 0.0) + it["grams"]
    out: List[dict] = []
    for name, g in agg.items():
        mn, mx = _get_minmax_for(name)
        mn_tot = mn * servings * scale
        mx_tot = mx * servings * scale
        soft_ceiling = mx_tot * SOFT_MAX_FACTOR
        g0 = g if g > 0 else (mn_tot + mx_tot) / 2.0
        if g0 < mn_tot:
            if g0 > 0:
                warnings.append(f"{name}: {g0:.3g} g unter Minimum ({mn_tot:.3g} g) → Minimum gesetzt.")
            g_final = mn_tot
        elif g0 <= mx_tot:
            g_final = g0
        elif g0 <= soft_ceiling:
            warnings.append(f"{name}: {g0:.3g} g über Maximum ({mx_tot:.3g} g) – erlaubt (soft).")
            g_final = g0
        else:
            warnings.append(f"{name}: {g0:.3g} g deutlich über Maximum; gedeckelt auf {soft_ceiling:.3g} g.")
            g_final = soft_ceiling
        out.append({"name": name, "grams": _round_gram(g_final)})
    order = {w: i for i, w in enumerate(sorted(SPICE_WHITELIST))}
    out.sort(key=lambda x: order.get(x["name"], 9999))
    return out, warnings

# ================== Extraktion ==================
def extract_spices_for_dish_list(dish: str) -> Tuple[List[str], str]:
    if not is_known_dish(dish):
        return [], "(not a known dish)"
    out = run_ollama(build_names_prompt(dish))
    lst = parse_json_list(out)
    if not lst: return [], out
    spices_norm = normalize_spices(lst)
    spices_final = [s for s in spices_norm if _fuzzy_match_to_whitelist(s)]
    return spices_final, out

def extract_spices_with_grams(dish: str, servings: int, intensity: str) -> Tuple[List[dict], str, List[str]]:
    if not is_known_dish(dish):
        return [], "(not a known dish)", []
    out = run_ollama(build_llm_prompt(dish, servings, intensity))
    objs = parse_json_spice_grams(out)
    if not objs:
        names, _ = extract_spices_for_dish_list(dish)
        objs = [{"name": n, "grams": 0.0} for n in names]
    final, warns = enforce_gram_rules(objs, servings, intensity)
    return final, out, warns

# ================== CLI ==================
def main(argv: List[str]):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("dish", nargs="*")
    ap.add_argument("--json-only", action="store_true")
    ap.add_argument("--voice", action="store_true")
    ap.add_argument("--lang", default="de")
    ap.add_argument("--servings", type=int, default=2)
    ap.add_argument("--intensity", choices=list(INTENSITY_SCALE.keys()), default="medium")
    ap.add_argument("--with-grams", action="store_true", default=True)
    ap.add_argument("--names-only", action="store_true")
    args = ap.parse_args(argv)

    if args.voice:
        from speech_input import transcribe_once
        utterance = transcribe_once(lang_hint=args.lang)
        if not utterance:
            print("Kein Sprachinput erkannt."); return
        print(f"\n== Sprache erkannt ==\n{utterance}")
        dish = utterance
    else:
        dish = " ".join(args.dish) if args.dish else input("Gericht: ").strip()

    start = time.time()
    if args.names_only and not args.with_grams:
        spices, raw = extract_spices_for_dish_list(dish)
        result, warnings_list = spices, []
    else:
        spice_objs, raw, warnings_list = extract_spices_with_grams(dish, args.servings, args.intensity)
        result = spice_objs
    dur = time.time() - start

    if args.json_only:
        print(json.dumps(result, ensure_ascii=False)); return

    print("\n== Gericht =="); print(dish)
    if isinstance(result, list) and result and isinstance(result[0], dict):
        print("== Gewürze (mit Gramm) ==")
        for it in result: print(f"- {it['name']}: {it['grams']} g")
        if warnings_list:
            print("\nHinweise:")
            for w in warnings_list: print("•", w)
        print("\nJSON:", json.dumps(result, ensure_ascii=False))
    else:
        print("== Gewürze ==")
        print(", ".join(result) if result else "(kein bekanntes Gericht)")
        print("\nJSON:", json.dumps(result, ensure_ascii=False))
    if DEBUG: print(f"[debug] duration={dur:.2f}s\n-- RAW --\n", raw)

if __name__ == "__main__":
    main(sys.argv[1:])
