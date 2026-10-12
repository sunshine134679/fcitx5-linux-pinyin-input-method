#!/usr/bin/env python3
"""Generate ModernIME's checked-in offline pinyin knowledge files.

The generated files deliberately contain only phrase, pinyin, frequency and
provenance metadata.  Definitions and examples from the upstream sources are
not copied into the input method data package.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path

try:
    from pypinyin import Style, lazy_pinyin
except ImportError:  # pragma: no cover - imported lazily when annotating raw texts
    Style = None  # type: ignore[assignment]
    lazy_pinyin = None  # type: ignore[assignment]


CHINESE_RE = re.compile(r"^[\u3007\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]+$")
PINYIN_RE = re.compile(r"^[a-z]+(?:'[a-z]+)*$")
ABBREVIATION_RE = re.compile(r"^[a-z]+$")
MIN_DOMAIN_FREQUENCY = 5
MIN_PHRASE_LENGTH = 2
MAX_PHRASE_LENGTH = 24

THUOCL_SOURCES = (
    ("THUOCL_IT.txt", "it", "thuocl-it"),
    ("THUOCL_caijing.txt", "finance", "thuocl-finance"),
    ("THUOCL_diming.txt", "place", "thuocl-place"),
    ("THUOCL_lishimingren.txt", "person", "thuocl-person"),
    ("THUOCL_poem.txt", "poem", "thuocl-poem"),
    ("THUOCL_medical.txt", "medical", "thuocl-medical"),
    ("THUOCL_food.txt", "food", "thuocl-food"),
    ("THUOCL_law.txt", "law", "thuocl-law"),
    ("THUOCL_car.txt", "car", "thuocl-car"),
    ("THUOCL_animal.txt", "animal", "thuocl-animal"),
)

CATEGORY_ORDER = {
    "daily": 0,
    "idiom": 1,
    "it": 2,
    "finance": 3,
    "place": 4,
    "person": 5,
    "poem": 6,
    "medical": 7,
    "chemistry": 8,
    "biology": 9,
    "food": 10,
    "law": 11,
    "car": 12,
    "animal": 13,
}


@dataclass(frozen=True)
class Entry:
    phrase: str
    pinyin: str
    frequency: int
    category: str
    abbreviation: str
    source: str


def normalize_phrase(value: str) -> str | None:
    phrase = unicodedata.normalize("NFKC", value.strip().lstrip("\ufeff"))
    if not (MIN_PHRASE_LENGTH <= len(phrase) <= MAX_PHRASE_LENGTH):
        return None
    if CHINESE_RE.fullmatch(phrase) is None:
        return None
    return phrase


def normalize_syllable(value: str) -> str:
    value = value.lower().strip().replace("u:", "ü")
    # Convert precomposed or NFD-decomposed ü (including ǖ, ǘ, ǚ, ǜ) to 'v'
    # BEFORE stripping combining diacritical marks so lǜ/nǚ/lüè become lv/nv/lve.
    value = unicodedata.normalize("NFD", value)
    value = value.replace("u\u0308", "v").replace("ü", "v")
    value = "".join(
        char for char in value if not unicodedata.combining(char) and not char.isdigit()
    )
    if value.startswith(("j", "q", "x", "y")) and "v" in value:
        value = value.replace("v", "u")
    return value


def pinyin_from_source(value: str) -> str | None:
    syllables = [normalize_syllable(item) for item in re.split(r"[\s']+", value.strip())]
    syllables = [item for item in syllables if item]
    if not syllables or any(re.fullmatch(r"[a-z]+", item) is None for item in syllables):
        return None
    result = "'".join(syllables)
    return result if PINYIN_RE.fullmatch(result) else None


def pinyin_for_phrase(phrase: str) -> str | None:
    if lazy_pinyin is None or Style is None:
        raise SystemExit(
            "缺少 pypinyin。仅生成数据时请先把 python-pinyin 源码加入 PYTHONPATH。"
        )
    syllables = [
        normalize_syllable(item)
        for item in lazy_pinyin(phrase, style=Style.NORMAL, errors="default")
    ]
    if len(syllables) != len(phrase) or any(
        re.fullmatch(r"[a-z]+", item) is None for item in syllables
    ):
        return None
    result = "'".join(syllables)
    return result if PINYIN_RE.fullmatch(result) else None


def abbreviation_for(pinyin: str) -> str:
    return "".join(syllable[0] for syllable in pinyin.split("'") if syllable)


def choose_entry(entries: dict[tuple[str, str], Entry], entry: Entry) -> None:
    key = (entry.phrase, entry.pinyin)
    previous = entries.get(key)
    if previous is None:
        entries[key] = entry
        return
    current_key = (
        entry.frequency,
        -CATEGORY_ORDER.get(entry.category, 999),
        entry.source,
    )
    previous_key = (
        previous.frequency,
        -CATEGORY_ORDER.get(previous.category, 999),
        previous.source,
    )
    if current_key > previous_key:
        entries[key] = entry


def read_idioms(path: Path, entries: dict[tuple[str, str], Entry]) -> int:
    added = 0
    with path.open("r", encoding="utf-8-sig", newline="") as input_file:
        for row in csv.DictReader(input_file):
            phrase = normalize_phrase(row.get("word", ""))
            if phrase is None:
                continue
            # The upstream CSV contains a small number of malformed or
            # truncated pinyin fields.  Prefer the phrase-aware annotator so
            # those rows cannot poison the LibIME dictionary; retain the
            # upstream field only as a fallback for an unknown phrase.
            pinyin = pinyin_for_phrase(phrase)
            if pinyin is None:
                pinyin = pinyin_from_source(row.get("pinyin", ""))
            if pinyin is None:
                continue
            entry = Entry(
                phrase=phrase,
                pinyin=pinyin,
                frequency=100,
                category="idiom",
                abbreviation=abbreviation_for(pinyin),
                source="china-idiom",
            )
            before = len(entries)
            choose_entry(entries, entry)
            added += int(len(entries) != before or entries[(phrase, pinyin)] == entry)
    return added


def read_thuocl(directory: Path, entries: dict[tuple[str, str], Entry]) -> int:
    added = 0
    for filename, category, source in THUOCL_SOURCES:
        path = directory / filename
        if not path.is_file():
            raise FileNotFoundError(f"找不到 THUOCL 词表：{path}")
        with path.open("r", encoding="utf-8-sig") as input_file:
            for line in input_file:
                fields = line.strip().split()
                if len(fields) != 2:
                    continue
                phrase = normalize_phrase(fields[0])
                if phrase is None:
                    continue
                try:
                    frequency = int(fields[1])
                except ValueError:
                    continue
                if frequency < MIN_DOMAIN_FREQUENCY:
                    continue
                pinyin = pinyin_for_phrase(phrase)
                if pinyin is None:
                    continue
                entry = Entry(
                    phrase=phrase,
                    pinyin=pinyin,
                    frequency=frequency,
                    category=category,
                    abbreviation=abbreviation_for(pinyin),
                    source=source,
                )
                before = len(entries)
                choose_entry(entries, entry)
                added += int(len(entries) != before or entries[(phrase, pinyin)] == entry)
    return added


def sorted_entries(entries: dict[tuple[str, str], Entry]) -> list[Entry]:
    return sorted(
        entries.values(),
        key=lambda entry: (entry.pinyin, entry.phrase, entry.source),
    )


def write_outputs(entries: list[Entry], output: Path, raw_output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as normalized:
        normalized.write(
            "# ModernIME pinyin knowledge v1\n"
            "# phrase<TAB>full_pinyin<TAB>frequency<TAB>category<TAB>abbreviation<TAB>source\n"
        )
        for entry in entries:
            normalized.write(
                "\t".join(
                    (
                        entry.phrase,
                        entry.pinyin,
                        str(entry.frequency),
                        entry.category,
                        entry.abbreviation,
                        entry.source,
                    )
                )
                + "\n"
            )
    with raw_output.open("w", encoding="utf-8", newline="\n") as raw:
        for entry in entries:
            raw.write(f"{entry.phrase}\t{entry.pinyin}\t0\n")


def run_self_tests() -> int:
    cases = [
        ("lü", "lv"),
        ("lǜ", "lv"),
        ("nǚ", "nv"),
        ("lüè", "lve"),
        ("nüè", "nve"),
        ("lu:4", "lv"),
        ("nu:3", "nv"),
        ("jü", "ju"),
        ("qü", "qu"),
        ("xü", "xu"),
        ("yü", "yu"),
    ]
    for raw, expected in cases:
        actual = normalize_syllable(raw)
        if actual != expected:
            raise AssertionError(f"normalize_syllable({raw!r}) = {actual!r}, expected {expected!r}")

    phrase_cases = [
        ("绿色", "lǜ sè", "lv'se", "ls"),
        ("女儿", "nǚ ér", "nv'er", "ne"),
        ("略微", "lüè wēi", "lve'wei", "lw"),
        ("重庆", "chóng qìng", "chong'qing", "cq"),
        ("重阳", "chóng yáng", "chong'yang", "cy"),
        ("银行", "yín háng", "yin'hang", "yh"),
        ("音乐", "yīn yuè", "yin'yue", "yy"),
        ("快乐", "kuài lè", "kuai'le", "kl"),
    ]
    for phrase, toned_py, expected_py, expected_abbr in phrase_cases:
        norm_phrase = normalize_phrase(phrase)
        norm_py = pinyin_from_source(toned_py)
        assert norm_phrase == phrase, f"normalize_phrase({phrase!r}) failed"
        assert norm_py == expected_py, f"pinyin_from_source({toned_py!r}) = {norm_py!r}, expected {expected_py!r}"
        assert abbreviation_for(norm_py) == expected_abbr
    print("generate_pinyin_knowledge self-test passed", file=sys.stderr)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="Run normalization self-tests")
    parser.add_argument("--idiom-csv", type=Path)
    parser.add_argument("--thuocl-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--raw-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        return run_self_tests()
    if not (args.idiom_csv and args.thuocl_dir and args.output and args.raw_output):
        raise SystemExit(
            "生成词库需同时提供 --idiom-csv、--thuocl-dir、--output 和 --raw-output（或使用 --self-test）。"
        )
    entries: dict[tuple[str, str], Entry] = {}
    idiom_count = read_idioms(args.idiom_csv, entries)
    thuocl_count = read_thuocl(args.thuocl_dir, entries)
    result = sorted_entries(entries)
    write_outputs(result, args.output, args.raw_output)
    print(
        f"已生成 {len(result)} 条词条（成语来源 {idiom_count}，THUOCL 来源 {thuocl_count}）",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
