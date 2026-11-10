#!/usr/bin/env python3
"""从随仓库固定的拆字、拼音数据离线生成查询索引，仅使用 Python 标准库。"""

import argparse
import hashlib
import itertools
from pathlib import Path
import unicodedata


ROOT = Path(__file__).resolve().parent
SOURCE_SHA256 = {
    "chaizi-jt.txt": "c341a99fc6a4a4f3169279c451ca55057a22603a5d7a520094378f65e817ad26",
    "chaizi.LICENSE": "243ce65e3a5a24669b08b5b6d25e82e961dfc747f841f04f199126a218968100",
    "chaizi.README.md": "9b52172aaf8f6170e475288706ce095ab7b5f8d5b1815e75ebf72ffa0b8fcfee",
    "pinyin.txt": "2342a6f98c85dbc3c8d26aef31744876a36326c6466e51e9a0f16898c36c2011",
    "pinyin.LICENSE": "9c048697be2502a16e8bcb282d5d465a07295b2def0ffb05a269c5d39dbe1586",
    "pinyin.README.md": "864b2e4f9e4dbdb01bfa32892b78c25bd982eb45fa6faf1d6c701be3d3f707d3",
}


def without_tone(reading):
    # Keep the keyboard distinction between ü and u; support v and u aliases.
    reading = unicodedata.normalize("NFD", reading.lower())
    reading = reading.replace("u\u0308", "v")
    reading = "".join(c for c in reading if not unicodedata.combining(c))
    return reading if reading.isascii() and reading.isalpha() else None


def generate(output):
    for name, expected in SOURCE_SHA256.items():
        actual = hashlib.sha256((ROOT / "upstream" / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"上游文件 SHA256 不匹配：{name}")

    pronunciations = {}
    for line in (ROOT / "upstream/pinyin.txt").read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        point, readings = line.split(":", 1)
        keys = set()
        for reading in readings.strip().split(","):
            key = without_tone(reading)
            if key:
                keys.add(key)
                keys.add(key.replace("v", "u"))
        pronunciations[chr(int(point[2:], 16))] = sorted(keys)

    entries = {}
    utf8_characters, pinyin_characters = set(), set()
    missing = set()
    for line in (ROOT / "upstream/chaizi-jt.txt").read_text(encoding="utf-8").splitlines():
        character, *variants = line.split("\t")
        # The upstream simplified conversion uses □ where it has no target.
        if character == "□" or len(character) != 1:
            continue
        for variant in variants:
            parts = variant.split()
            if len(parts) < 2 or any("□" in part for part in parts):
                continue
            components = " ".join(parts)
            entries.setdefault(("".join(parts), character), components)
            utf8_characters.add(character)
            choices = [pronunciations.get(part, []) for part in parts]
            if not all(choices):
                missing.update(part for part, keys in zip(parts, choices) if not keys)
                continue
            # All readings are materialized here, never during input handling.
            for reading in itertools.product(*choices):
                entries.setdefault(("".join(reading), character), components)
            pinyin_characters.add(character)

    lines = [
        "# ModernIME component lookup v1",
        "# Derived from kfcd/chaizi (CC BY 3.0) and mozillazg/pinyin-data (MIT).",
        "# See 数据来源与许可.md and licenses in this directory.",
        "# query\tcharacter\tcomponents",
    ]
    lines.extend(f"{query}\t{character}\t{components}"
                 for (query, character), components in sorted(entries.items()))
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"已生成 {len(entries)} 条索引；部件查询覆盖 {len(utf8_characters)} 字，"
          f"拼音查询覆盖 {len(pinyin_characters)} 字；无读音部件：{''.join(sorted(missing))}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "component_lookup.tsv")
    generate(parser.parse_args().output)
