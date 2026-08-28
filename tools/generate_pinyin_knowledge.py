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
except ImportError as exc:  # pragma: no cover - maintenance tool diagnostic
    raise SystemExit(
        "缺少 pypinyin。仅生成数据时请先把 python-pinyin 源码加入 PYTHONPATH。"
    ) from exc


CHINESE_RE = re.compile(r"^[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]+$")
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
    "idiom": 0,
    "it": 1,
    "finance": 2,
    "place": 3,
    "person": 4,
    "poem": 5,
    "medical": 6,
    "food": 7,
    "law": 8,
    "car": 9,
    "animal": 10,
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
    value = unicodedata.normalize("NFKD", value.lower().strip())
    value = "".join(char for char in value if not unicodedata.combining(char))
    # LibIME's ASCII spelling uses v for ü, just like its standard pinyin
    # input profiles do.
    return value.replace("ü", "v")


def pinyin_from_source(value: str) -> str | None:
    syllables = [normalize_syllable(item) for item in value.split()]
    syllables = [item for item in syllables if item]
    if not syllables or any(re.fullmatch(r"[a-z]+", item) is None for item in syllables):
        return None
    result = "'".join(syllables)
    return result if PINYIN_RE.fullmatch(result) else None


def pinyin_for_phrase(phrase: str) -> str | None:
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
    return "".join(syllable[0] for syllable in pinyin.split("'"))


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
            pinyin = pinyin_from_source(row.get("pinyin", ""))
            if pinyin is None:
                pinyin = pinyin_for_phrase(phrase)
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idiom-csv", type=Path, required=True)
    parser.add_argument("--thuocl-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--raw-output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
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
