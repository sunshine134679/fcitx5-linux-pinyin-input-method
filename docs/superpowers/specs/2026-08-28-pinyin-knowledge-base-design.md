# ModernIME 拼音知识库扩容设计规格

## 目标

在不改动候选栏尺寸、字体、间距、候选排序规则和英文判定规则的前提下，为 ModernIME 增加一套可离线安装、可复现构建的扩展拼音知识库。词库先解决“系统词库没有收录的成语、专业名词、地名和专有名词无法被解码”的数据问题，简拼、混合拼音和中英文意图判定在下一阶段单独实现。

## 调研依据

- LibIME 的 `PinyinDictionary` 支持多个独立词典层，以及文本/二进制词典格式；ModernIME 使用系统词典层 0、用户词典层 1，扩展知识库使用独立层 2。
- Rime 的 spelling algebra 将完整拼音与首字母缩写作为不同的派生拼写；本阶段保存首字母元数据，但不把缩写直接写进 LibIME 完整拼音词典，避免把未完成拼音误当成完整拼音。
- `China-idiom` 提供带完整拼音和拼音缩写的 MIT 数据集；THUOCL 提供带 DF 文档频率的 MIT 领域词表，涵盖 IT、财经、地名、历史人物、诗词、医学、饮食、法律、汽车和动物。
- `python-pinyin` 仅用于维护者离线标注没有自带拼音的领域词，不进入 ModernIME 运行时依赖。

## 范围

### 本阶段包含

1. 以固定来源版本生成约三万条成语和九万条筛选后的领域/专名词条。
2. 词条统一保存完整拼音、首字母缩写、类别、来源和基础频率。
3. 去掉 BOM、首尾空白、非中文噪声和不合法拼音；同一词语/完整拼音组合只保留频率最高的一条，同时保留多音词的不同读音。
4. 把规范化文本词库转换为 LibIME 二进制扩展词典，并作为独立字典层加载。
5. 缺少扩展词典、词典损坏或路径不可用时，输入法继续使用系统词库正常启动。
6. 增加数据质量测试、扩展词典构建测试和典型成语/专业词的 provider 回归测试。
7. 提供来源版本、许可证、过滤规则和重新生成命令，不把临时下载目录、压缩包、Python 环境、CMake 构建产物提交进仓库。

### 本阶段不包含

- 不修改候选排序、原始英文候选优先级或 `coversPinyinInput()` 的简拼判定。
- 不把单字母首字母缩写直接作为 LibIME 完整拼音候选。
- 不接入云端词库、联网候选、用户行为上传或远程模型。
- 不把第三方完整仓库、释义、例句和无关测试数据打包进安装目录。
- 不修改候选栏任何 UI 尺寸和渲染参数。

## 数据格式

提交到仓库的规范化数据文件为 `data/pinyin/modernime-knowledge.tsv`，每行六列：

```text
词语<TAB>完整拼音<TAB>基础频率<TAB>类别<TAB>首字母缩写<TAB>来源
```

示例：

```text
一心一意	yi'xin'yi'yi	100	idiom	yxyy	china-idiom
人工智能	ren'gong'zhi'neng	100	it	rgzn	thuocl-it
```

约束：

- 词语必须是非空 UTF-8，当前离线扩展数据只保留连续中文字符，ASCII 品牌和代码词交给英文候选/用户词典处理。
- 完整拼音统一小写，音节之间使用单撇号，不能以单撇号开头、结尾或连续出现。
- 基础频率为有限非负整数；没有可比频率的成语按稳定的默认值写入。
- 类别使用 `idiom`、`it`、`finance`、`place`、`person`、`poem`、`medical`、`food`、`law`、`car`、`animal`。
- 来源使用固定的短名称，详细 URL、提交号和许可证在 `data/pinyin/SOURCES.json` 与 `data/pinyin/THIRD_PARTY_NOTICES.md` 中维护。
- 文件按“完整拼音、词语、来源、频率”排序，生成过程必须稳定，避免同一输入在不同机器产生不同二进制词典。

LibIME 原始文本中使用 `词语<TAB>完整拼音<TAB>0`，所有扩展词条暂不把第三方 DF 直接当作 LibIME cost，避免第一阶段改变现有候选排序；基础频率和类别留给下一阶段的匹配/排序层使用。

## 数据来源与合规

- `China-idiom` 固定到 `78606b0294a22e798633c4469a4009b78ad60f26`，使用其 `china_idiom/idiom.csv` 的成语词语字段；完整拼音统一由带词组词典的标注工具生成，上游拼音仅作为未知词的回退。
- `THUOCL` 固定到 `a30ce79d895d01ab5132a5c74c29703ff7efb4cc`，使用十个明确类别的词表及其 DF 字段。
- `python-pinyin` 固定到 `8595294b1a97845e30f11ecfdb3caa4e61ac398`，只作为维护时的拼音标注工具。
- 这些来源的许可证和归属文字随项目保存；扩展数据是由来源词条筛选、规范化和拼音标注产生的派生数据，更新时必须同步更新来源提交号和生成日期。
- 来源数据不在 ModernIME 运行时联网下载，安装包只使用仓库中已生成的 TSV 和 LibIME 二进制文件。

## 加载架构

`PinyinDataPaths` 新增 `extensionDictionary`。provider 构造时按以下顺序加载：

1. 系统词典索引 0（现有路径）。
2. 用户词典索引 1（现有路径）。
3. 若扩展二进制文件存在，调用 `addEmptyDict()` 建立索引 2，再以 Binary 格式加载；缺失或加载失败只记录诊断并继续。

默认扩展路径依次搜索 `MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY`、当前用户数据目录、安装前缀下的 `share/modernime/pinyin/modernime-knowledge.dict` 和 `/usr/share/modernime/pinyin/modernime-knowledge.dict`。测试可显式传入临时路径。

## 构建与安装

- 在配置阶段检查 `libime_pinyndict`；它是已安装的 LibIME 开发/工具包的一部分。
- CMake 将提交的 raw 文本词库转换为 build 目录中的二进制词典，并安装到 `${CMAKE_INSTALL_DATADIR}/modernime/pinyin/modernime-knowledge.dict`。
- 生成的二进制只在 build/install 目录存在，不提交 Git；安装清单和卸载脚本必须包含该文件。
- 默认安装脚本继续使用现有用户前缀，不要求 `sudo cmake --install`。

## 验收标准

1. 数据测试确认词条总量不少于 100,000 条，且 `idiom`、`it`、`medical`、`law`、`place` 五类都存在。
2. 数据测试确认 `一心一意/yxyy`、`自然语言处理/zryycl`、`量子计算/lzjs`、`阿司匹林/aspl` 等代表性词条存在并有合法完整拼音。
3. 全量 CTest 在没有安装扩展词典时仍通过，provider 不因扩展数据缺失启动失败。
4. 安装后扩展二进制存在且大小非零；显式加载后输入连续完整拼音 `yixinyiyi` 能看到“一心一意”。首字母缩写输入由下一阶段的匹配器负责，本阶段不提前改变它。
5. 现有候选排序、英文 `who` 行为和所有 UI 尺寸测试保持不变。
