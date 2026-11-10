#!/usr/bin/env python3
"""回放明确列出的质量目标，保存原始候选与六类诊断证据；不是总体准确率评估。"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rank_change(before, after):
    if before == after:
        return "未变化"
    if after is not None and (before is None or after < before):
        return "提升"
    return "下降"


def diagnose(result):
    baseline = result["baseline"]
    final = result["after_learning"] or baseline
    reading = result["expected_pinyin"]
    expected = result["expected"]
    evidence = [item for item in final["ranked_evidence"] if item["text"] == expected]
    exact_input = result["scheme"] == "full" and result["input"].replace("'", "") == reading.replace("'", "")
    return {
        "拼音解析": {
            "实际切分": final["segmented_input"], "期望读音": reading,
            "状态": "已召回完整候选" if final["expected_rank"] else "首批未见完整候选，需检查拼写或召回预算",
        },
        "词典": {
            "完整词条存在": result["dictionary_contains_expected"],
            "说明": "整句可以由多个词组成；完整词条不存在不单独证明词典缺失。",
        },
        "分词": {
            "状态": ("输入切分与期望不同" if final["segmented_input"] != reading else "切分一致")
            if exact_input else "简拼、双拼或纠错输入需结合读音检查",
            "说明": "切分不同是歧义线索，不是自动判定的根因。",
        },
        "语言模型评分": {
            "状态": "可比较排序分项" if evidence else "未获得期望候选的排序证据",
            "期望候选证据": evidence,
            "说明": "原始解码分越高越好，final_cost越低越好；匹配组和后续混排也影响最终位置。",
        },
        "上下文": {
            "无上下文名次": result["without_context_rank"], "有上下文名次": baseline["expected_rank"],
            "变化": rank_change(result["without_context_rank"], baseline["expected_rank"]),
        },
        "用户词频": {
            "实际重复选择次数": result["learning_selections"],
            "学习前名次": baseline["expected_rank"], "学习后名次": final["expected_rank"],
            "变化": rank_change(baseline["expected_rank"], final["expected_rank"])
            if result["learning_selections"] else "未执行学习",
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--corpus", type=Path,
                        default=Path(__file__).resolve().parents[1] / "data/pinyin/候选质量-v1.tsv")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    # Explicit per-process temporary user paths are also set inside the binary.
    for key in list(environment):
        if key.startswith(("MODERNIME_", "FCITX_")):
            del environment[key]
    with args.corpus.open(encoding="utf-8", newline="") as source:
        cases = list(csv.DictReader(source, delimiter="\t"))
    if not cases or len({case["id"] for case in cases}) != len(cases):
        raise ValueError("语料不能为空，案例ID必须唯一")
    records = []
    resources = {}
    failures = []
    for case in cases:
        command = [str(binary)]
        for key in ("input", "expected", "pinyin", "before", "scheme", "learn"):
            command += ["--" + key, case[key]]
        completed = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=60)
        if completed.returncode:
            raise RuntimeError(f"{case['id']} 执行失败：{completed.stderr.strip()}")
        result = json.loads(completed.stdout)
        final = result["after_learning"] or result["baseline"]
        target = int(case["max_rank"])
        position = final["expected_rank"]
        met_target = position is not None and position <= target
        record = {"case": case, "met_target": met_target, "diagnosis": diagnose(result), "raw": result}
        records.append(record)
        if not met_target:
            failures.append({"id": case["id"], "input": case["input"], "expected": case["expected"],
                             "rank": position, "target_rank": target,
                             "actual_first": final["candidates"][0]["text"] if final["candidates"] else ""})
        for path in result["resource_paths"].values():
            if path not in resources:
                file = Path(path).resolve(strict=True)
                resources[path] = {"path": str(file), "bytes": file.stat().st_size, "sha256": sha256(file)}
        print(f"{case['id']}: {'达标' if met_target else '未达标'}，期望 {case['expected']}，名次 {position}")
    linked = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    libraries = {}
    for line in linked.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "=>" and parts[0].startswith(("libIME", "libFcitx5")):
            libraries[parts[0]] = {"path": str(Path(parts[2]).resolve()), "sha256": sha256(parts[2])}
    metadata = {
        "schema": 1, "corpus": str(args.corpus.resolve()), "corpus_sha256": sha256(args.corpus),
        "binary": str(binary), "binary_sha256": sha256(binary), "platform": platform.platform(),
        "machine": platform.machine(), "resources": resources, "libraries": libraries,
        "clock": "记录每次请求的实际墙钟；全新临时学习库，不保证固定时钟逐位重现",
        "measurement": "单次首批候选生成，包含按需诊断采集；不包括进程/模型加载，不是按键到像素延迟",
    }
    summary = {"cases": len(records), "met_targets": len(records) - len(failures), "unmet_targets": failures,
               "scope": "版本化功能目标样本，不是整体准确率；首批未召回不等于完整词库不存在"}
    for name, value in (("环境.json", metadata), ("汇总.json", summary)):
        (args.output / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with (args.output / "候选诊断.jsonl").open("w", encoding="utf-8") as output:
        for record in records:
            output.write(json.dumps(record, ensure_ascii=False) + "\n")
    print(f"完成 {len(records)} 项回放，{len(failures)} 项质量目标未达标。原始证据：{args.output}")
    # The command reports measurements, not a release gate. Unmet targets stay explicit in the report.


if __name__ == "__main__":
    main()
