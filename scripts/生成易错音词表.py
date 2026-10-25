#!/usr/bin/env python3
"""生成 core/include/modernime/core/reading_hints.inc（易错音内联读音提示词表）。

触发规则（2026-10-25 调研结论，与微信输入法"正确读音秒知道"、搜狗"出错字后
标注正确读音"一致）：仅当用户输入的无声调节节与该字在本词中的规范读音不一致
（如把 卡住 记成 kazhu，规范读音 qiǎ）时，才在该字后内联标注正确读音。

本脚本职责：
1. 依据《现代汉语词典》第七版整理的高频易错音词表（下方 CURATED）；
2. 逐字对 Unicode Unihan 数据库（kHanyuPinyin + kMandarin）校验：规范读音
   必须出现在该字的 Unihan 读音集合中，任何一字校验失败立即报错退出，
   绝不生成未经校验的读音；
3. 由"无声调拼音 + 声调"自动生成带声调展示串（含 iu/ui 标调特判），
   并做 NFC 规范化；
4. 输出按字节序排序的 constexpr 词表供 reading_hints.h 二分查找。

再生成方式（需要网络下载 Unihan 数据，或提前放置缓存文件）：
    python3 scripts/生成易错音词表.py /tmp/unihan/Unihan_Readings.txt
产物已提交入库，日常构建离线可用，不依赖本脚本运行。
"""
import sys
import unicodedata
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "core/include/modernime/core/reading_hints.inc"
CACHE = Path("/tmp/unihan/Unihan_Readings.txt")
UNIHAN_URL = "https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip"

# 词语 -> (标注字位, 该字规范无声调拼音, 声调)。读音依据《现代汉语词典》
# 第七版；收录门槛：该字存在两个及以上"无声调即不同"的常用读音（可被输入
# 行为检测到记错），且本词为高频词。无声调相同的读音对（如 处 chǔ/chù、
# 重要 zhòng 的 zhong）无法从按键检测误读，一律不收录。
CURATED = {
    "卡住": (0, "qia", 3),
    "卡在": (0, "qia", 3),
    "卡顿": (0, "qia", 3),
    "卡壳": (0, "qia", 3),
    "关卡": (1, "qia", 3),
    "发卡": (1, "qia", 3),
    "卡脖子": (0, "qia", 3),
    "角色": (0, "jue", 2),
    "觉得": (0, "jue", 2),
    "睡觉": (1, "jiao", 4),
    "音乐": (1, "yue", 4),
    "乐器": (0, "yue", 4),
    "穿着": (1, "zhuo", 2),
    "着装": (0, "zhuo", 2),
    "血液": (0, "xue", 4),
    "血管": (0, "xue", 4),
    "说服": (0, "shuo", 1),
    "地壳": (1, "qiao", 4),
    "银行": (1, "hang", 2),
    "重新": (0, "chong", 2),
    "重复": (0, "chong", 2),
    "长大": (0, "zhang", 3),
    "曾经": (0, "ceng", 2),
    "剥削": (0, "bo", 1),
    "给予": (0, "ji", 3),
    "供给": (1, "ji", 3),
    "粗犷": (1, "guang", 3),
    "酵母": (0, "jiao", 4),
    "徘徊": (1, "huai", 2),
    "落枕": (0, "lao", 4),
    "落下": (0, "la", 4),
    "一叶扁舟": (2, "pian", 1),
}

TONE_MARKS = {
    1: dict(zip("aeiouvü", "āēīōūǖ")),
    2: dict(zip("aeiouvü", "áéíóúǘ")),
    3: dict(zip("aeiouvü", "ǎěǐǒǔǚ")),
    4: dict(zip("aeiouü", "àèìòùǜ")),
}


def tone_marked(syllable: str, tone: int) -> str:
    """按汉语拼音正词法标调：iu→u、ui→i 标尾，其余按 a>o>e>(i/u/ü) 优先。"""
    if tone == 5:
        return syllable
    marks = TONE_MARKS[tone]
    if syllable.endswith("iu"):
        return syllable[:-1] + marks["u"]
    if syllable.endswith("ui"):
        return syllable[:-1] + marks["i"]
    for vowel in ("a", "o", "e"):
        pos = syllable.find(vowel)
        if pos >= 0:
            return syllable[:pos] + marks[vowel] + syllable[pos + 1:]
    for i in range(len(syllable) - 1, -1, -1):
        if syllable[i] in marks:
            return syllable[:i] + marks[syllable[i]] + syllable[i + 1:]
    raise ValueError(f"no vowel to mark: {syllable}")


def toneless(reading: str) -> str:
    """去声调并小写：piān -> pian、lü -> lv。非拼音项返回空。"""
    stripped = "".join(
        c for c in unicodedata.normalize("NFD", reading)
        if not unicodedata.combining(c))
    stripped = stripped.lower().replace("ü", "v")
    return stripped if stripped.isalpha() else ""


def load_unihan(path: Path):
    """合并 5 个读音字段（简体字完整读音常挂在繁体形或现代词典字段上）。"""
    fields = ("kHanyuPinyin", "kHanyuPinlu", "kMandarin", "kTGHZ2013", "kXHC1983")
    readings = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("U+"):
            continue
        codepoint, field, value = line.split("\t", 2)
        if field not in fields:
            continue
        char = chr(int(codepoint[2:], 16))
        syllables = readings.setdefault(char, set())
        for token in value.replace(",", " ").split():
            # kHanyuPinyin/kTGHZ2013 项形如 "21280.160:yuè" 或 "yuè"，kHanyuPinlu
            # 形如 "lè(283)"：统一取":"后、"("前片段。
            segment = token.split(":", 1)[-1].split("(", 1)[0]
            syllable = toneless(segment)
            if syllable:
                syllables.add(syllable)
    return readings


def load_variants(path: Path):
    """繁简映射（双向）：kTraditionalVariant / kSimplifiedVariant。"""
    import re
    variants = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("U+"):
            continue
        codepoint, field, value = line.split("\t", 2)
        if field not in ("kTraditionalVariant", "kSimplifiedVariant"):
            continue
        char = chr(int(codepoint[2:], 16))
        others = variants.setdefault(char, set())
        for token in value.split():
            for piece in re.findall(r"U\+([0-9A-Fa-f]{4,6})", token):
                others.add(chr(int(piece, 16)))
            if not token.startswith("<"):
                for c in token:
                    if len(c.encode("utf-8")) >= 3:
                        others.add(c)
    return variants


def ensure_unihan(arg: str) -> Path:
    if arg:
        return Path(arg)
    if CACHE.exists():
        return CACHE
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    zip_path = CACHE.parent / "Unihan.zip"
    print(f"下载 {UNIHAN_URL} ...")
    urllib.request.urlretrieve(UNIHAN_URL, zip_path)
    with zipfile.ZipFile(zip_path) as zf:
        zf.extract("Unihan_Readings.txt", CACHE.parent)
    return CACHE


def main():
    unihan_path = ensure_unihan(sys.argv[1] if len(sys.argv) > 1 else "")
    readings = load_unihan(unihan_path)
    variants = load_variants(unihan_path.parent / "Unihan_Variants.txt")
    rows = []
    for word, (idx, canonical, tone) in sorted(CURATED.items()):
        char = word[idx]
        char_readings = set(readings.get(char, set()))
        for other in variants.get(char, set()):
            char_readings |= readings.get(other, set())
        if not char_readings:
            sys.exit(f"校验失败：{char!r} 不在 Unihan 读音库中（{word}）")
        if canonical not in char_readings:
            sys.exit(
                f"校验失败：{word} 中 {char} 的规范读音 {canonical} 未见于 "
                f"Unihan（{sorted(char_readings)}）")
        display = unicodedata.normalize("NFC", tone_marked(canonical, tone))
        rows.append((word, idx, canonical, display))
    rows.sort(key=lambda r: r[0].encode("utf-8"))

    lines = [
        "// 由 scripts/生成易错音词表.py 生成（Unihan kHanyuPinyin 逐字校验），请勿手改。",
        "// 读音依据《现代汉语词典》第七版；触发规则：用户输入的无声调节节与该字",
        "// 在本词中的规范读音不一致（如 卡住 记成 kazhu）才标注，简拼/错位拼/正确",
        "// 读音输入一律不标注。收录门槛见脚本 CURATED 注释。",
        "",
        "inline constexpr ReadingHintEntry kReadingHintEntries[] = {",
    ]
    for word, idx, canonical, display in rows:
        lines.append(f'    {{"{word}", {idx}, "{canonical}", "{display}"}},')
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"已生成 {OUT}（{len(rows)} 词，全部通过 Unihan 校验）")


if __name__ == "__main__":
    main()
