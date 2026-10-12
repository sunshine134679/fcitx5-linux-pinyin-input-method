# 第三方数据与工具归属

ModernIME 的离线拼音与词典数据由以下固定版本的开源词库和工具经离线清洗、多音字交叉校准与分批筛选生成。各上游来源保留其各自的许可证与版权归属，**并未将不同许可证的数据合并声称为单一 MIT 许可的新词库**，亦不改变 ModernIME 项目自身代码的许可证。

仓库仅提交筛选后的词语、无声调完整拼音、有界归一化频率、类别/来源标记及必要的许可证文件，不将上游完整仓库、原始压缩包、诗词全集、繁体大表或释义/例句塞入发布包。

## 数据来源与许可证总览

| 来源标识 | 上游项目 | 固定提交 / 快照版本 | 上游声明许可证 | 随包保留许可证文本 | 用途说明 |
|---|---|---|---|---|---|
| `wanxiang-*` | [amzxyz/RIME-LMDG (万象词库)](https://github.com/amzxyz/RIME-LMDG) | `d378e94aa12300b47e7a6303016de5a2b9ce95ee` | `CC-BY-4.0` | `data/pinyin/licenses/CC-BY-4.0.txt` | 第一批主干通用复合词（`jichu`）、多音字词组校准（`duoyin`）及第二批医学/药品/化学/物种/地名/人名分级扩充 |
| `rime-ice-*` | [iDvel/rime-ice (雾凇拼音)](https://github.com/iDvel/rime-ice) | `da1fbe602e38f26db846fa10120ee64c2b0324c0` | `GPL-3.0-only` | `data/pinyin/licenses/GPL-3.0.txt` | 第一批基础词与扩展词交叉验证（`base`、`ext`），第三批无注音词表（`tencent`）经整词读音校验后分流导入 |
| `jieba` | [fxsjy/jieba](https://github.com/fxsjy/jieba) | `67fa2e36e72f69d9134b8a1037b83fbb070b9775` | `MIT` | 本文档内嵌 MIT 许可正文 | 第二批词频分级与词性（POS）交叉验证（`dict.txt`、`dict.txt.big`） |
| `cc-cedict` | [CC-CEDICT / MDBG](https://www.mdbg.net/chinese/dictionary?page=cc-cedict) | `2026-10-11T08:29:27Z` (`cf0bf5c8ff94...`) | `CC-BY-SA-4.0` | `data/pinyin/licenses/CC-BY-SA-4.0.txt` | 第一批双语词典整词读音交叉验证（仅提取词面与拼音，不复制英文释义） |
| `thuocl-*` | [thunlp/THUOCL](https://github.com/thunlp/THUOCL) | `a30ce79d895d01ab5132a5c74c29703ff7efb4cc` | `MIT` | 本文档内嵌 MIT 许可正文 | 基础分类专业词条（IT、财经、地名、历史人物、诗词、医学、饮食、法律、汽车、动物）及 DF 频率 |
| `china-idiom` | [sfyc23/China-idiom](https://github.com/sfyc23/China-idiom) | `78606b0294a22e798633c4469a4009b78ad60f26` | `MIT` | 本文档内嵌 MIT 许可正文 | 四字及多字成语词条来源 |
| `python-pinyin` | [mozillazg/python-pinyin](https://github.com/mozillazg/python-pinyin) | `8595294b1a97845e30f11ecfdb3caa4e61ac398` | `MIT` | 本文档内嵌 MIT 许可正文 | 仅在维护期从原始无注音词表生成基础拼音时使用，非运行时依赖 |
| `ecdict` | [skywind3000/ECDICT](https://github.com/skywind3000/ECDICT) | 固定快照 | `MIT` | 本文档内嵌 MIT 许可正文 | 英文候选词简明中文释义词典数据源（`modernime-english-dict.bin`） |
| `rime-essay`（未并入主表） | [rime/rime-essay (八股文)](https://github.com/rime/rime-essay) | `054920de4f54c9e5994276a96a4fc2a35cb51aa3` | `LGPL-3.0` | `data/pinyin/licenses/LGPL-3.0.txt` | 繁体无注音对照参考，未直接并入简体全拼主词典 |

## 规范化与修改说明（CC BY 4.0 / CC BY-SA 4.0 / GPL-3.0 合规声明）

1. **字段提取与格式转换**：仅提取连续中文词面、无声调完整拼音（`ü` 统一转为 `v`，音节以单引号 `'` 分隔）、有界归一化词频（`[10, 1000]`）、类别与首字母缩写；未复制 CC-CEDICT 的英文释义。
2. **多音字纠错与分流**：利用 `wanxiang/duoyin`、`wanxiang/jichu`、`rime-ice/base`、`cc-cedict` 对早期 `THUOCL` 单字默认注音产生的多音字误注进行了整词级校准；对无注音词表（如 `rime-ice/tencent`）中含强多音字且缺少多源一致读音的词条，隔离至 `data/pinyin/POLYPHONIC_REVIEW.tsv`，不盲目入主词典。
3. **排除范围**：未将 `rime-essay`（繁体）、`wanxiang/shici`（诗词大表）、`wanxiang/fangyan`（方言）、`abbrev`/`t9_abbrev`/`mixed`/`cuoyin` 直接并入简体标准全拼主词典。

## 各上游项目详细归属

### 1. 万象词库 / RIME-LMDG (`wanxiang-*`)
- 项目：<https://github.com/amzxyz/RIME-LMDG>
- 固定提交：`d378e94aa12300b47e7a6303016de5a2b9ce95ee`
- 作者与归属：AMZ / amzxyz 及上游贡献者
- 许可证：Creative Commons Attribution 4.0 International (`CC-BY-4.0`)，完整文本见 `data/pinyin/licenses/CC-BY-4.0.txt`。

### 2. 雾凇拼音 / rime-ice (`rime-ice-*`)
- 项目：<https://github.com/iDvel/rime-ice>
- 固定提交：`da1fbe602e38f26db846fa10120ee64c2b0324c0`
- 作者与归属：iDvel 及上游贡献者（字词表头部另含上游复合来源说明）
- 许可证：GNU General Public License v3.0 only (`GPL-3.0-only`)，完整文本见 `data/pinyin/licenses/GPL-3.0.txt`。

### 3. CC-CEDICT (`cc-cedict`)
- 项目：<https://www.mdbg.net/chinese/dictionary?page=cc-cedict>
- 快照版本：`2026-10-11T08:29:27Z` (`sha256: cf0bf5c8ff94316799bc82755dbe0627658d7167244060d8e4ce8d5928a6b9f5`)
- 作者与归属：CC-CEDICT 社群、MDBG；原 CEDICT 项目由 Paul Denisowski 发起
- 许可证：Creative Commons Attribution-ShareAlike 4.0 International (`CC-BY-SA-4.0`)，完整文本见 `data/pinyin/licenses/CC-BY-SA-4.0.txt`。

### 4. jieba (`jieba`)
- 项目：<https://github.com/fxsjy/jieba>
- 固定提交：`67fa2e36e72f69d9134b8a1037b83fbb070b9775`
- 作者与归属：Sun Junyi (fxsjy) 及上游贡献者
- 许可证：MIT License。

### 5. THUOCL (`thuocl-*`)
- 项目：<https://github.com/thunlp/THUOCL>
- 固定提交：`a30ce79d895d01ab5132a5c74c29703ff7efb4cc`
- 作者与归属：清华大学自然语言处理与社会人文计算实验室（THUNLP）及原项目贡献者
- 许可证：MIT License。

### 6. China-idiom (`china-idiom`)
- 项目：<https://github.com/sfyc23/China-idiom>
- 固定提交：`78606b0294a22e798633c4469a4009b78ad60f26`
- 作者与归属：Thunder Bouble (sfyc23)
- 许可证：MIT License。

### 7. python-pinyin (`python-pinyin`)
- 项目：<https://github.com/mozillazg/python-pinyin>
- 固定提交：`8595294b1a97845e30f11ecfdb3caa4e61ac398`
- 作者与归属：mozillazg, 闲耘 `<hotoo.cn@gmail.com>`
- 许可证：MIT License。

### 8. ECDICT (`ecdict`)
- 项目：<https://github.com/skywind3000/ECDICT>
- 作者与归属：Lin Wei (Skywind)
- 许可证：MIT License。

### 9. Rime 八股文 / rime-essay（仅对照，未并入简体全拼主表）
- 项目：<https://github.com/rime/rime-essay>
- 固定提交：`054920de4f54c9e5994276a96a4fc2a35cb51aa3`
- 作者与归属：佛振 (Lotem) 及 `AUTHORS` 所列贡献者
- 许可证：GNU Lesser General Public License v3.0 (`LGPL-3.0`)，完整文本见 `data/pinyin/licenses/LGPL-3.0.txt` 与 `data/pinyin/licenses/GPL-3.0.txt`。

## MIT License 文本（jieba / THUOCL / China-idiom / python-pinyin / ECDICT）

```text
MIT License

Copyright (c) 2012-2020 Sun Junyi (jieba)
Copyright (c) 2018 THUNLP (THUOCL)
Copyright (c) 2019 Thunder Bouble (China-idiom)
Copyright (c) 2016 mozillazg, 闲耘 <hotoo.cn@gmail.com> (python-pinyin)
Copyright (c) Lin Wei / Skywind (ECDICT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

具体数据源版本、SHA-256 校验值、过滤策略与分批统计以 `SOURCES.json` 为准；更新数据源时必须同步更新本说明。
