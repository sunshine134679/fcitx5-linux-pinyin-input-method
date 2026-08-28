# ModernIME 离线拼音知识库

这里保存 ModernIME 使用的、可离线安装的扩展词库。当前数据包包含成语、IT、财经、地名、历史人物、诗词、医学、饮食、法律、汽车和动物等类别。

## 文件

- `modernime-knowledge.tsv`：带类别、来源、基础频率和首字母缩写元数据的规范化词库。
- `modernime-knowledge.raw`：供 `libime_pinyndict` 构建二进制词典的派生文本，格式是“词语、完整拼音、cost”。
- `SOURCES.json`：源项目、固定提交号、下载地址和过滤规则。
- `THIRD_PARTY_NOTICES.md`：随派生数据发布的许可证和归属说明。

这两个数据文件是有意提交的离线运行时输入，不是临时下载物。CMake 构建生成的 `modernime-knowledge.dict` 位于 build 目录，属于构建产物，不提交 Git。

## 重新生成

生成脚本是维护工具，不是运行时依赖。需要 Python 3 和固定版本的 `python-pinyin` 源码；ModernIME 正常构建、安装和运行不需要 Python。

```bash
PYTHONPATH=/path/to/python-pinyin \
python3 tools/generate_pinyin_knowledge.py \
  --idiom-csv /path/to/China-idiom/china_idiom/idiom.csv \
  --thuocl-dir /path/to/thuocl-data \
  --output data/pinyin/modernime-knowledge.tsv \
  --raw-output data/pinyin/modernime-knowledge.raw
```

生成后运行：

```bash
bash tests/pinyin_knowledge_data_test.sh \
  data/pinyin/modernime-knowledge.tsv \
  data/pinyin/modernime-knowledge.raw
```

不要把上游完整仓库、压缩包、释义/例句、Python 虚拟环境或 build 目录复制到本项目。
