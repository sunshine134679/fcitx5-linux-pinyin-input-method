#!/usr/bin/env python3
"""导出候选栏 / 客户端两套外观调色板，供 website 的色表比对。

数据源（不手抄色值，只解析源码）：
  候选栏 9 套：ui/fcitx5/src/candidate_bar_renderer.cpp
               —— RenderStyle::reference() / fromSettings()
  客户端 7 套：tools/settings/settings_widgets.cpp
               —— clientPalette() 的 ClientPalette 表
  主题键名    ：core/include/modernime/core/settings.h
               —— kCandidateThemes / kClientThemes

候选栏必须覆盖三条路径，只比 palette(0x...) 会漏 3 套：
  1. classic-blue : reference() 的浮点默认值
  2. light / dark : fromSettings() 里的 if-else 浮点数组
  3. 其余 6 套    : palette(0x...) 的十六进制参数

浮点转 hex 用四舍五入（半入）：0.10*255=25.5 -> 26。

用法：
  python3 tools/export_theme_palettes.py                # 打印 JSON
  python3 tools/export_theme_palettes.py --check 网站目录 # 打印并核对官网色表
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RENDERER = REPO / "ui/fcitx5/src/candidate_bar_renderer.cpp"
WIDGETS = REPO / "tools/settings/settings_widgets.cpp"
SETTINGS_H = REPO / "core/include/modernime/core/settings.h"

# 候选栏：渲染器字段 -> 输出键
CANDIDATE_FIELD_MAP = {
    "panel": "panel",
    "text": "ink",
    "selected": "accent",
    "selectedText": "selectedInk",
    "border": "border",
}
CANDIDATE_KEYS = ("panel", "ink", "accent", "selectedInk", "muted", "border")
MUTED_FIELDS = ("preedit", "annotation")

# 客户端：官网用到的键 -> ClientPalette 字段（这是约定，必须写成数据）
CLIENT_MAPPING = {
    "canvas": "sidebar",
    "ink": "muted",
    "border": "controlBorder",
    "accent": "accent",
}
CLIENT_KEYS = ("canvas", "ink", "border", "accent")

# 与主映射不同、且是有意为之的两套，必须带理由
CLIENT_OVERRIDES = {
    "night": {
        "border": ("border", "深海夜是暗底，点阵与截图描边取更暗的 border，不用更亮的 controlBorder"),
    },
    "contrast": {
        "ink": ("ink", "高对比主题的注释文字取 ink 而非 muted，保持最高对比"),
    },
}

ASSIGN_RE = re.compile(r"((?:style\.\w+\s*=\s*)+)\{([^{}]*)\}")
CASE_RE = re.compile(r"case\s+core::CandidateTheme::(\w+)\s*:\s*palette\(([^;]*?)\)\s*;")
CLIENT_ROW_RE = re.compile(
    r'\{\s*"([\w-]+)",\s*((?:"#[0-9a-fA-F]{6}",?\s*)+)(true|false)\s*\}'
)


def to_hex(floats: list[float]) -> str:
    if len(floats) < 3:
        raise ValueError(f"颜色分量不足: {floats}")
    parts = [max(0, min(255, int(v * 255.0 + 0.5))) for v in floats[:3]]
    return "#%02x%02x%02x" % tuple(parts)


def hex_literal(token: str) -> str:
    token = token.strip()
    if not token.startswith("0x"):
        raise ValueError(f"不是十六进制颜色: {token}")
    return "#%06x" % (int(token, 16) & 0xFFFFFF)


def slice_fn(text: str, header: str) -> str:
    start = text.index(header)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise ValueError(f"未闭合的函数体: {header}")


def slice_branch(text: str, marker: str) -> str:
    start = text.index(marker)
    depth = 0
    opened = False
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
            opened = True
        elif text[i] == "}":
            depth -= 1
            if opened and depth == 0:
                return text[start:i + 1]
    raise ValueError(f"未闭合的分支: {marker}")


def parse_block(body: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for names, values in ASSIGN_RE.findall(body):
        fields = re.findall(r"style\.(\w+)", names)
        try:
            numbers = [float(v) for v in values.split(",")]
        except ValueError:
            continue  # 字体之类的非纯数值初始化
        rendered = to_hex(numbers)
        for field in fields:
            out[field] = rendered
    return out


def slice_array(text: str, name: str) -> str:
    """切出 constexpr 数组的初始化体（含花括号）。"""
    start = text.index(name)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise ValueError(f"未闭合的数组: {name}")


def theme_keys() -> tuple[dict[str, str], list[str]]:
    """从 settings.h 取候选栏 enum->key 映射与客户端主题键序。"""
    text = SETTINGS_H.read_text(encoding="utf-8")
    # 只有首条重复写了类型名，其余是裸 {key, label} 初始化，必须先切数组体
    candidate_body = slice_array(text, "kCandidateThemes")
    client_body = slice_array(text, "kClientThemes")
    enum_to_key = dict(re.findall(
        r'CandidateTheme::(\w+),\s*"([\w-]+)"', candidate_body))
    client_keys = re.findall(r'\{"([\w-]+)",\s*"[^"]*"\}', client_body)
    if not enum_to_key or not client_keys:
        raise ValueError("settings.h 里没找到主题键表")
    return enum_to_key, client_keys


def build_candidate_themes(enum_to_key: dict[str, str]) -> dict[str, dict[str, str]]:
    text = RENDERER.read_text(encoding="utf-8")
    reference = parse_block(slice_fn(text, "RenderStyle RenderStyle::reference()"))
    from_settings = slice_fn(text, "RenderStyle RenderStyle::fromSettings(")
    dark = parse_block(slice_branch(from_settings, "::Dark)"))
    light = parse_block(slice_branch(from_settings, "::Light)"))

    def assemble(fields: dict[str, str]) -> dict[str, str]:
        merged = dict(reference)
        merged.update(fields)
        palette = {out: merged[field] for field, out in CANDIDATE_FIELD_MAP.items()}
        muted = next((merged[f] for f in MUTED_FIELDS if f in merged), None)
        if muted is None:
            raise KeyError("渲染器里找不到 muted 色（preedit/annotation）")
        palette["muted"] = muted
        return {k: palette[k] for k in CANDIDATE_KEYS}

    themes = {
        "classic-blue": assemble({}),   # 路径 1
        "light": assemble(light),       # 路径 2
        "dark": assemble(dark),
    }
    for enum_name, args in CASE_RE.findall(from_settings):  # 路径 3
        key = enum_to_key.get(enum_name)
        if key is None:
            raise ValueError(f"settings.h 里没有 {enum_name} 的键名")
        parts = [hex_literal(p) for p in args.split(",") if p.strip()]
        if len(parts) != 6:
            raise ValueError(f"{enum_name} 的 palette 参数不是 6 个: {parts}")
        themes[key] = dict(zip(CANDIDATE_KEYS, parts))
    return dict(sorted(themes.items()))


def client_field_order() -> list[str]:
    text = WIDGETS.read_text(encoding="utf-8")
    struct = slice_fn(text, "struct ClientPalette")
    order = re.findall(r"\*\s*(\w+)", struct)
    if len(order) < 11:
        raise ValueError(f"ClientPalette 字段解析失败: {order}")
    return order


def build_client_themes(client_keys: list[str]) -> dict[str, dict[str, str]]:
    text = WIDGETS.read_text(encoding="utf-8")
    order = client_field_order()
    rows = {}
    for key, values, dark in CLIENT_ROW_RE.findall(text):
        colors = re.findall(r"#[0-9a-fA-F]{6}", values)
        if len(colors) != len(order):
            raise ValueError(f"{key} 颜色数与字段数不符: {len(colors)}/{len(order)}")
        rows[key] = dict(zip(order, (c.lower() for c in colors)))

    themes = {}
    for key in client_keys:
        if key not in rows:
            raise ValueError(f"ClientPalette 里没有主题 {key}")
        row = rows[key]
        overrides = CLIENT_OVERRIDES.get(key, {})
        palette = {}
        for out in CLIENT_KEYS:
            field = overrides.get(out, (CLIENT_MAPPING[out], None))[0]
            palette[out] = row[field]
        themes[key] = palette
    return dict(sorted(themes.items()))


def commit_id() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def check_tables(website: Path, candidate: dict, client: dict) -> int:
    drift = 0

    studio = website / "studio.js"
    if studio.exists():
        text = studio.read_text(encoding="utf-8")
        block = text[text.index("const palettes = {"):text.index("};", text.index("const palettes = {"))]
        entry = re.compile(r"^\s*(?:'([\w-]+)'|([\w-]+))\s*:\s*\{(.*)\}\s*,?\s*$", re.M)
        site = {}
        for key_a, key_b, body in entry.findall(block):
            site[key_a or key_b] = {k: v.lower() for k, v in
                                    re.findall(r"(\w+)\s*:\s*'(#[0-9a-fA-F]{6})'", body)}
        for key, palette in candidate.items():
            if key not in site:
                print(f"MISSING studio.js {key}", file=sys.stderr); drift += 1; continue
            for field in CANDIDATE_KEYS:
                if site[key].get(field, "").lower() != palette[field]:
                    print(f"DRIFT studio.js {key}.{field}: 渲染器 {palette[field]} / 官网 {site[key].get(field)}",
                          file=sys.stderr)
                    drift += 1
        for key in sorted(set(site) - set(candidate)):
            print(f"EXTRA studio.js {key}: 渲染器里没有", file=sys.stderr); drift += 1

    app = website / "app.js"
    if app.exists():
        text = app.read_text(encoding="utf-8")
        block = text[text.index("const clientThemes = {"):text.index("};", text.index("const clientThemes = {"))]
        entry = re.compile(r"^\s*(?:'([\w-]+)'|([\w-]+))\s*:\s*\{(.*)\}\s*,?\s*$", re.M)
        site = {}
        for key_a, key_b, body in entry.findall(block):
            site[key_a or key_b] = {k: v.lower() for k, v in
                                    re.findall(r"(\w+)\s*:\s*'(#[0-9a-fA-F]{6})'", body)}
        for key, palette in client.items():
            if key not in site:
                print(f"MISSING app.js {key}", file=sys.stderr); drift += 1; continue
            for field in CLIENT_KEYS:
                if site[key].get(field, "").lower() != palette[field]:
                    print(f"DRIFT app.js {key}.{field}: ClientPalette {palette[field]} / 官网 {site[key].get(field)}",
                          file=sys.stderr)
                    drift += 1
        for key in sorted(set(site) - set(client)):
            print(f"EXTRA app.js {key}: ClientPalette 里没有", file=sys.stderr); drift += 1

    if drift == 0:
        print(f"OK: 候选栏 {len(candidate)} 套 + 客户端 {len(client)} 套与官网色表一致",
              file=sys.stderr)
    return drift


def main() -> int:
    parser = argparse.ArgumentParser(description="导出候选栏与客户端调色板")
    parser.add_argument("--check", metavar="WEBSITE_DIR", type=Path,
                        help="核对 website/studio.js 与 website/app.js 的色表")
    args = parser.parse_args()

    enum_to_key, client_keys = theme_keys()
    candidate = build_candidate_themes(enum_to_key)
    client = build_client_themes(client_keys)

    payload = {
        "generated_by": "tools/export_theme_palettes.py",
        "source": {
            "candidate": "ui/fcitx5/src/candidate_bar_renderer.cpp:98 RenderStyle::fromSettings",
            "client": "tools/settings/settings_widgets.cpp:23 clientPalette",
            "keys": "core/include/modernime/core/settings.h kCandidateThemes / kClientThemes",
        },
        "commit": commit_id(),
        "rounding": "round_half_up(v * 255)",
        "mapping": {
            "client": dict(CLIENT_MAPPING),
            "overrides": {k: {f: {"field": v[0], "reason": v[1]} for f, v in o.items()}
                          for k, o in CLIENT_OVERRIDES.items()},
        },
        "candidateKeys": list(CANDIDATE_KEYS),
        "clientKeys": list(CLIENT_KEYS),
        "candidateThemes": candidate,
        "clientThemes": client,
    }
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))

    if args.check:
        return 1 if check_tables(args.check, candidate, client) else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
