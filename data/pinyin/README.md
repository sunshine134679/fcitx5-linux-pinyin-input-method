# ModernIME 离线拼音知识库

这里保存 ModernIME 使用的、可离线安装的扩展词库。当前数据包包含 **283,781 条 `(词语, 完整拼音)` 词条**（覆盖 **283,558 个独立词面** 及 **223 条多音词扩展读音**），与系统词典 `sc.dict` 及网络热词合并后共覆盖 **460,766 个独立中文词面**（**468,979 条 `(词语, 完整拼音)` 组合**），涵盖日常通用复合词、成语、IT/系统/嵌入式开发、财经、地名、人名、诗词、临床医学、药品、化学、物种生物学、饮食、法律、汽车和动物等类别。

## 文件

- `modernime-knowledge.tsv`：带类别、来源、有界归一化频率（`[20, 1000]`）和首字母缩写元数据的 6 列规范化词库。
- `modernime-knowledge.raw`：供 `libime_pinyindict` 构建二进制词典的派生文本，格式是“词语、完整拼音、cost（固定为 0）”。
- `POLYPHONIC_REVIEW.tsv`：第三批无注音词表（如 `rime-ice/tencent`、`jieba`）离线分流时隔离出的强多音字冲突或待整词注音复核清单（10 列）。
- `modernime-hotwords.raw`：网络热词表（词语、完整拼音、cost=5），构建为独立的 `modernime-hotwords.dict` 并作为 libime 第三层词典加载；正 cost 让简拼（如 yyds）命中的热词排在系统词典组合之前。新增热词直接在此文件追加一行（保持 cost 为 5），并避免与 `modernime-knowledge.raw` 或系统词典 `sc.dict` 收录的词条重复。
- `SOURCES.json`：源项目、固定提交号、SHA-256 校验值、跨源词频映射与分批过滤规则。
- `THIRD_PARTY_NOTICES.md` 与 `licenses/`：随派生数据发布的各来源独立许可证（`CC-BY-4.0`、`GPL-3.0-only`、`CC-BY-SA-4.0`、`MIT`、`LGPL-3.0`）和归属说明。

这些数据文件是有意提交的离线运行时输入，不是临时下载物。CMake 构建生成的 `modernime-knowledge.dict` 位于 build 目录，属于构建产物，不提交 Git。

## 重新生成与离线分批导入

生成与分批导入脚本是维护工具，不是运行时依赖；ModernIME 正常构建、安装和运行不需要 Python 或网络连接。

1. 运行 `ü -> v` 拼音规范化自检：
```bash
python3 tools/generate_pinyin_knowledge.py --self-test
```

2. 从离线词库调研包执行三批次交叉验证导入与多音字复核分流：
```bash
python3 tools/import_lexicon_bundle.py \
  --bundle-dir /path/to/modernime-lexicon-2026-10-12 \
  --output data/pinyin/modernime-knowledge.tsv \
  --raw-output data/pinyin/modernime-knowledge.raw \
  --polyphonic-review-output data/pinyin/POLYPHONIC_REVIEW.tsv
```

3. 生成后运行数据契约校验：
```bash
bash tests/pinyin_knowledge_data_test.sh \
  data/pinyin/modernime-knowledge.tsv \
  data/pinyin/modernime-knowledge.raw \
  data/pinyin/modernime-hotwords.raw
```

不要把上游完整仓库、压缩包、诗词全集、繁体大表、释义/例句、Python 虚拟环境或 build 目录复制到本项目。
