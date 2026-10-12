#!/usr/bin/env python3
"""ModernIME offline pinyin knowledge expansion and validation tool.

Validates pinyin syllables, polyphonic readings, intra/cross-dictionary duplicates,
and merges curated entries into modernime-knowledge.tsv and modernime-knowledge.raw.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path

CHINESE_RE = re.compile(r"^[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]+$")
PINYIN_RE = re.compile(r"^[a-z]+(?:'[a-z]+)*$")
ABBREVIATION_RE = re.compile(r"^[a-z]+$")

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
    "daily": 11,
}

# The 400 canonical Mandarin Pinyin syllables recognized by LibIME / ModernIME
VALID_SYLLABLES = frozenset(
    [
        "a", "ai", "an", "ang", "ao",
        "ba", "bai", "ban", "bang", "bao", "bei", "ben", "beng", "bi", "bian",
        "biao", "bie", "bin", "bing", "bo", "bu",
        "ca", "cai", "can", "cang", "cao", "ce", "cen", "ceng", "cha", "chai",
        "chan", "chang", "chao", "che", "chen", "cheng", "chi", "chong", "chou",
        "chu", "chua", "chuai", "chuan", "chuang", "chui", "chun", "chuo", "ci",
        "cong", "cou", "cu", "cuan", "cui", "cun", "cuo",
        "da", "dai", "dan", "dang", "dao", "de", "dei", "den", "deng", "di",
        "dia", "dian", "diao", "die", "ding", "diu", "dong", "dou", "du",
        "duan", "dui", "dun", "duo",
        "e", "ei", "en", "eng", "er",
        "fa", "fan", "fang", "fei", "fen", "feng", "fo", "fou", "fu",
        "ga", "gai", "gan", "gang", "gao", "ge", "gei", "gen", "geng", "gong",
        "gou", "gu", "gua", "guai", "guan", "guang", "gui", "gun", "guo",
        "ha", "hai", "han", "hang", "hao", "he", "hei", "hen", "heng", "hong",
        "hou", "hu", "hua", "huai", "huan", "huang", "hui", "hun", "huo",
        "ji", "jia", "jian", "jiang", "jiao", "jie", "jin", "jing", "jiong",
        "jiu", "ju", "juan", "jue", "jun",
        "ka", "kai", "kan", "kang", "kao", "ke", "kei", "ken", "keng", "kong",
        "kou", "ku", "kua", "kuai", "kuan", "kuang", "kui", "kun", "kuo",
        "la", "lai", "lan", "lang", "lao", "le", "lei", "leng", "li", "lia",
        "lian", "liang", "liao", "lie", "lin", "ling", "liu", "lo", "long",
        "lou", "lu", "luan", "lun", "luo", "lv", "lve",
        "ma", "mai", "man", "mang", "mao", "me", "mei", "men", "meng", "mi",
        "mian", "miao", "mie", "min", "ming", "miu", "mo", "mou", "mu",
        "na", "nai", "nan", "nang", "nao", "ne", "nei", "nen", "neng", "ni",
        "nian", "niang", "niao", "nie", "nin", "ning", "niu", "nong", "nou",
        "nu", "nuan", "nun", "nuo", "nv", "nve",
        "o", "ou",
        "pa", "pai", "pan", "pang", "pao", "pei", "pen", "peng", "pi", "pian",
        "piao", "pie", "pin", "ping", "po", "pou", "pu",
        "qi", "qia", "qian", "qiang", "qiao", "qie", "qin", "qing", "qiong",
        "qiu", "qu", "quan", "que", "qun",
        "ran", "rang", "rao", "re", "ren", "reng", "ri", "rong", "rou", "ru",
        "rua", "ruan", "rui", "run", "ruo",
        "sa", "sai", "san", "sang", "sao", "se", "sen", "seng", "sha", "shai",
        "shan", "shang", "shao", "she", "shei", "shen", "sheng", "shi", "shou",
        "shu", "shua", "shuai", "shuan", "shuang", "shui", "shun", "shuo", "si",
        "song", "sou", "su", "suan", "sui", "sun", "suo",
        "ta", "tai", "tan", "tang", "tao", "te", "teng", "ti", "tian", "tiao",
        "tie", "ting", "tong", "tou", "tu", "tuan", "tui", "tun", "tuo",
        "wa", "wai", "wan", "wang", "wei", "wen", "weng", "wo", "wu",
        "xi", "xia", "xian", "xiang", "xiao", "xie", "xin", "xing", "xiong",
        "xiu", "xu", "xuan", "xue", "xun",
        "ya", "yan", "yang", "yao", "ye", "yi", "yin", "ying", "yo", "yong",
        "you", "yu", "yuan", "yue", "yun",
        "za", "zai", "zan", "zang", "zao", "ze", "zei", "zen", "zeng", "zha",
        "zhai", "zhan", "zhang", "zhao", "zhe", "zhei", "zhen", "zheng", "zhi",
        "zhong", "zhou", "zhu", "zhua", "zhuai", "zhuan", "zhuang", "zhui",
        "zhun", "zhuo", "zi", "zong", "zou", "zu", "zuan", "zui", "zun", "zuo"
    ]
)

# Critical polyphonic words and phrases that have specific required pronunciations
POLYPHONIC_CONSTRAINTS = {
    "脑卒中": "nao'cu'zhong",       # 卒中读 cùzhòng
    "尉迟": "yu'chi",              # 尉迟姓读 yùchí
    "尉迟敬德": "yu'chi'jing'de",
    "尉迟恭": "yu'chi'gong",
    "令狐": "ling'hu",            # 令狐姓读 línghú
    "令狐冲": "ling'hu'chong",
    "万俟": "mo'qi",              # 万俟复姓读 mòqí
    "澹台": "tan'tai",            # 澹台复姓读 tántái
    "单于": "chan'yu",            # 单于读 chányú
    "区寄": "ou'ji",              # 区姓读 ōu
    "仇英": "qiu'ying",           # 仇姓读 qiú
    "查良镛": "zha'liang'yong",    # 查姓读 zhā
    "商贾": "shang'gu",           # 贾作商人读 gǔ
    "卷积": "juan'ji",            # 卷积神经网络
    "卷积神经网络": "juan'ji'shen'jing'wang'luo",
    "参差": "cen'ci",
    "便秘": "bian'bi",            # 秘读 bì
    "大夫": "dai'fu",             # 医生读 dàifu
}


@dataclass(frozen=True)
class Entry:
    phrase: str
    pinyin: str
    frequency: int
    category: str
    abbreviation: str
    source: str


def normalize_phrase(value: str) -> str:
    phrase = unicodedata.normalize("NFKC", value.strip().lstrip("\ufeff"))
    if not (2 <= len(phrase) <= 24):
        raise ValueError(f"词长必须在 2 到 24 之间：'{value}'")
    if CHINESE_RE.fullmatch(phrase) is None:
        raise ValueError(f"词语包含非汉字字符：'{value}'")
    return phrase


def normalize_pinyin(phrase: str, pinyin: str) -> str:
    pinyin = pinyin.lower().strip()
    syllables = pinyin.split("'")
    if len(syllables) != len(phrase):
        raise ValueError(
            f"音节数与字数不一致：词 '{phrase}' (长度 {len(phrase)}) vs 拼音 '{pinyin}' (音节数 {len(syllables)})"
        )
    for s in syllables:
        if s not in VALID_SYLLABLES:
            raise ValueError(f"非法拼音音节 '{s}' (词 '{phrase}', 拼音 '{pinyin}')")
    result = "'".join(syllables)
    if not PINYIN_RE.fullmatch(result):
        raise ValueError(f"拼音格式不合规：'{result}'")
    return result


def abbreviation_for(pinyin: str) -> str:
    return "".join(syllable[0] for syllable in pinyin.split("'"))


def validate_entry(
    phrase: str,
    pinyin: str,
    frequency: int,
    category: str,
    source: str,
    hotwords_set: set[tuple[str, str]],
) -> Entry:
    norm_phrase = normalize_phrase(phrase)
    norm_pinyin = normalize_pinyin(norm_phrase, pinyin)

    if category not in CATEGORY_ORDER:
        raise ValueError(f"未知的词条类别 '{category}'，可用类别：{list(CATEGORY_ORDER.keys())}")

    if frequency <= 0:
        raise ValueError(f"词条频次必须为正整数：{frequency}")

    if not source:
        raise ValueError("来源标识不可为空")

    # Polyphonic constraint validation
    if norm_phrase in POLYPHONIC_CONSTRAINTS:
        expected = POLYPHONIC_CONSTRAINTS[norm_phrase]
        if norm_pinyin != expected:
            raise ValueError(
                f"多音字校验失败：'{norm_phrase}' 预期读音为 '{expected}'，实际提供 '{norm_pinyin}'"
            )

    # Hotwords collision validation
    if (norm_phrase, norm_pinyin) in hotwords_set:
        raise ValueError(
            f"词条与 modernime-hotwords.raw 冲突：'({norm_phrase}, {norm_pinyin})'，跨库重复会破坏分层！"
        )

    abbrev = abbreviation_for(norm_pinyin)
    return Entry(
        phrase=norm_phrase,
        pinyin=norm_pinyin,
        frequency=frequency,
        category=category,
        abbreviation=abbrev,
        source=source,
    )


def load_hotwords(path: Path) -> set[tuple[str, str]]:
    hotwords = set()
    if not path.is_file():
        return hotwords
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split("\t")
            if len(parts) >= 2:
                hotwords.add((parts[0], parts[1]))
    return hotwords


def load_knowledge_tsv(path: Path) -> dict[tuple[str, str], Entry]:
    entries: dict[tuple[str, str], Entry] = {}
    if not path.is_file():
        return entries
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.strip().split("\t")
            if len(parts) != 6:
                continue
            phrase, pinyin, freq_str, cat, abbrev, src = parts
            entry = Entry(
                phrase=phrase,
                pinyin=pinyin,
                frequency=int(freq_str),
                category=cat,
                abbreviation=abbrev,
                source=src,
            )
            entries[(phrase, pinyin)] = entry
    return entries


def sorted_entries(entries: dict[tuple[str, str], Entry]) -> list[Entry]:
    return sorted(
        entries.values(),
        key=lambda entry: (entry.pinyin, entry.phrase, entry.source),
    )


def write_outputs(entries: list[Entry], output_tsv: Path, output_raw: Path) -> None:
    output_tsv.parent.mkdir(parents=True, exist_ok=True)
    output_raw.parent.mkdir(parents=True, exist_ok=True)
    with output_tsv.open("w", encoding="utf-8", newline="\n") as normalized:
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
    with output_raw.open("w", encoding="utf-8", newline="\n") as raw:
        for entry in entries:
            raw.write(f"{entry.phrase}\t{entry.pinyin}\t0\n")


def sha256_of_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--knowledge-tsv",
        type=Path,
        default=Path("data/pinyin/modernime-knowledge.tsv"),
        help="Path to modernime-knowledge.tsv",
    )
    parser.add_argument(
        "--knowledge-raw",
        type=Path,
        default=Path("data/pinyin/modernime-knowledge.raw"),
        help="Path to modernime-knowledge.raw",
    )
    parser.add_argument(
        "--hotwords-raw",
        type=Path,
        default=Path("data/pinyin/modernime-hotwords.raw"),
        help="Path to modernime-hotwords.raw",
    )
    parser.add_argument(
        "--input-tsv",
        type=Path,
        action="append",
        help="Additional TSV files to validate and merge (can specify multiple)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate only, do not write files",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    print("Loading hotwords...", file=sys.stderr)
    hotwords = load_hotwords(args.hotwords_raw)
    print(f"Loaded {len(hotwords)} hotwords from {args.hotwords_raw}", file=sys.stderr)

    print("Loading existing knowledge...", file=sys.stderr)
    entries = load_knowledge_tsv(args.knowledge_tsv)
    print(f"Loaded {len(entries)} entries from {args.knowledge_tsv}", file=sys.stderr)

    if args.input_tsv:
        added_count = 0
        updated_count = 0
        skipped_count = 0
        for input_path in args.input_tsv:
            print(f"Processing input file: {input_path}...", file=sys.stderr)
            with input_path.open("r", encoding="utf-8") as f:
                for line_idx, line in enumerate(f, 1):
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split("\t")
                    if len(parts) < 5:
                        raise ValueError(f"[{input_path}:{line_idx}] 至少需要 5 列 (词语, 拼音, 频次, 类别, 来源): '{line}'")
                    phrase, pinyin, freq_str, cat, src = parts[0], parts[1], parts[2], parts[3], parts[4]
                    try:
                        freq = int(freq_str)
                    except ValueError:
                        raise ValueError(f"[{input_path}:{line_idx}] 非法频次 '{freq_str}'")

                    entry = validate_entry(phrase, pinyin, freq, cat, src, hotwords)
                    key = (entry.phrase, entry.pinyin)
                    if key in entries:
                        existing = entries[key]
                        if entry.frequency > existing.frequency:
                            entries[key] = entry
                            updated_count += 1
                        else:
                            skipped_count += 1
                    else:
                        entries[key] = entry
                        added_count += 1

        print(
            f"Merge complete: added {added_count}, updated {updated_count}, skipped {skipped_count}",
            file=sys.stderr,
        )

    sorted_list = sorted_entries(entries)
    print(f"Total entries: {len(sorted_list)}", file=sys.stderr)

    if not args.dry_run:
        print(f"Writing outputs to {args.knowledge_tsv} and {args.knowledge_raw}...", file=sys.stderr)
        write_outputs(sorted_list, args.knowledge_tsv, args.knowledge_raw)
        tsv_hash = sha256_of_file(args.knowledge_tsv)
        raw_hash = sha256_of_file(args.knowledge_raw)
        print(f"modernime-knowledge.tsv SHA256: {tsv_hash}", file=sys.stderr)
        print(f"modernime-knowledge.raw SHA256: {raw_hash}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())

