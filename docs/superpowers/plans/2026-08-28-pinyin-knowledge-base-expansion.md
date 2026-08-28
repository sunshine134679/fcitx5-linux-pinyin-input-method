# ModernIME 拼音知识库扩容实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不改候选排序和 UI 的前提下，生成并接入可离线安装的成语、专业词和专名扩展拼音词库。

**Architecture:** 保留 LibIME 系统词典和用户词典的现有索引，新增一个只读的 ModernIME 扩展词典索引。数据维护脚本把固定来源转换成规范化 TSV 和 LibIME raw 文本；CMake 在构建时调用 `libime_pinyndict` 生成二进制，provider 只在文件存在且可加载时挂载索引 2。首字母缩写保存为元数据但本计划不改变匹配器，避免把数据扩容和简拼算法混成一个不可回退的改动。

**Tech Stack:** C++20、CMake、LibIME Core/Pinyin、CTest、Python 3 标准库（仅维护数据生成脚本）、已核验的 MIT 数据源。

**Spec:** `docs/superpowers/specs/2026-08-28-pinyin-knowledge-base-design.md`

## Global Constraints

- 不修改候选栏尺寸、位置、字体、圆角、间距和渲染参数。
- 不修改候选排序、`coversPinyinInput()` 简拼判定和英文候选优先级。
- 不引入运行时联网、云候选、远程模型或 Python 运行时依赖。
- 只提交规范化数据、来源清单、许可证说明、维护脚本和必要的 C++/CMake/测试代码。
- 每个独立功能完成“失败测试 → 最小实现 → 全量测试 → 中文 Git commit”；完成后执行构建、测试和安装。
- 本阶段使用隔离分支 `codex/pinyin-knowledge-base`，最后经验证后再合并 `main`。

### Task 1: 记录数据工程规格并建立可审计的数据目录

**Files:**
- Create: `docs/superpowers/specs/2026-08-28-pinyin-knowledge-base-design.md`
- Create: `data/pinyin/README.md`
- Create: `data/pinyin/SOURCES.json`
- Create: `data/pinyin/THIRD_PARTY_NOTICES.md`
- Create: `tools/generate_pinyin_knowledge.py`
- Test: `tests/pinyin_knowledge_data_test.sh`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- `tools/generate_pinyin_knowledge.py --idiom-csv PATH --thuocl-dir PATH --output PATH --raw-output PATH` 读取固定来源，输出稳定的规范化 TSV 和 LibIME raw TSV。
- `tests/pinyin_knowledge_data_test.sh DATA_FILE` 检查数据列数、总量、必需类别、代表词、拼音格式和排序稳定性。

- [ ] **Step 1: 写失败的数据质量测试。**

  测试在数据文件不存在时返回失败；文件存在后解析每一行，要求至少 100,000 条、至少 5 个类别，并检查 `一心一意`、`自然语言处理`、`量子计算`、`阿司匹林` 的完整拼音和首字母缩写。

- [ ] **Step 2: 运行测试确认它因缺少数据失败。**

  Run: `bash tests/pinyin_knowledge_data_test.sh data/pinyin/modernime-knowledge.tsv`

  Expected: FAIL，明确报告数据文件不存在，而不是脚本语法错误。

- [ ] **Step 3: 写数据生成器和来源说明。**

  生成器使用标准库解析成语 CSV 和 THUOCL 的 `词语<TAB>DF` 文件；领域词通过维护环境注入 `pypinyin.lazy_pinyin` 生成完整拼音。过滤条件固定为 UTF-8、连续中文、长度 2–24、DF 至少 5；成语不使用 DF 过滤。对重复键 `词语 + 规范化完整拼音` 保留基础频率较高的一条，类别并列时按固定类别顺序选择；输出按 `(full_pinyin, phrase, source, -frequency)` 排序，raw 输出为 `phrase<TAB>full_pinyin<TAB>0`。

- [ ] **Step 4: 获取固定来源并生成实际数据。**

  维护者使用已经核验的来源提交号生成数据，不把临时源仓库复制进项目：

  ```text
  China-idiom  78606b0294a22e798633c4469a4009b78ad60f26
  THUOCL       a30ce79d895d01ab5132a5c74c29703ff7efb4cc
  python-pinyin 8595294b1a97845e30f11ecfdb3caa4e61ac398
  ```

  生成后使用 `wc -l`、`sha256sum` 和数据测试记录数量与摘要。

- [ ] **Step 5: 运行数据测试确认通过。**

  Run: `bash tests/pinyin_knowledge_data_test.sh data/pinyin/modernime-knowledge.tsv`

  Expected: PASS，并输出总条数、类别数、raw 条数和代表词检查结果。

- [ ] **Step 6: 提交数据包功能。**

  ```bash
  git add data/pinyin tools/generate_pinyin_knowledge.py tests/pinyin_knowledge_data_test.sh tests/CMakeLists.txt docs/superpowers/specs/2026-08-28-pinyin-knowledge-base-design.md
  git commit -m "扩充离线拼音知识库数据"
  ```

### Task 2: 将扩展词典作为独立 LibIME 层构建和加载

**Files:**
- Modify: `algorithm/pinyin/include/modernime/pinyin/pinyin_candidate_provider.h`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `algorithm/pinyin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `data/pinyin/CMakeLists.txt`
- Modify: `install.sh`
- Modify: `uninstall.sh`
- Modify: `tests/core/pinyin_provider_test.cpp`
- Modify: `tests/settings/settings_install_test.sh`

**Interfaces:**
- `PinyinDataPaths::extensionDictionary` 显式覆盖扩展二进制路径；为空时由 provider 搜索用户数据目录和安装数据目录。
- `PinyinCandidateProvider` 在系统词典层 0、用户词典层 1 后，以 `addEmptyDict()` 创建并加载扩展层 2；扩展文件不可读时不抛异常、不阻断输入法。
- CMake target `modernime_pinyin_knowledge_dictionary` 将 `modernime-knowledge.raw` 转换为 build 目录中的 `modernime-knowledge.dict`，并安装到 `share/modernime/pinyin/`。

- [ ] **Step 1: 写扩展词典回归测试。**

  在 `pinyin_provider_test.cpp` 中显式设置 `paths.extensionDictionary` 为 CMake 生成的二进制路径，输入无撇号连续串 `yixinyiyi`，断言页面中能找到“一心一意”；另测 `paths.extensionDictionary` 指向不存在文件时 provider 仍能输入 `nihao`。测试通过公开编译定义 `MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY` 读取 `${CMAKE_CURRENT_BINARY_DIR}/data/pinyin/modernime-knowledge.dict`。

- [ ] **Step 2: 运行新增测试确认缺少接口/数据加载时失败。**

  Run: `cmake --build build/fcitx5-debug -j2 && ctest --test-dir build/fcitx5-debug -R modernime_pinyin_provider --output-on-failure`

  Expected: 新增扩展词条断言失败或扩展路径接口尚未编译，现有 `nihao` 测试仍能说明系统词典基线正常。

- [ ] **Step 3: 实现 CMake 二进制生成和安装。**

  在 `data/pinyin/CMakeLists.txt` 中用 `find_program(LIBIME_PINYINDICT_EXECUTABLE libime_pinyindict REQUIRED)`，用 `add_custom_command` 调用 `libime_pinyindict raw binary`，给生成文件建立 `modernime_pinyin_knowledge_dictionary` target；用 `install(FILES ...)` 安装到 `${CMAKE_INSTALL_DATADIR}/modernime/pinyin`。把 data 子目录接入顶层 CMake，并把 provider target 依赖到该 target。

- [ ] **Step 4: 实现默认路径解析和容错加载。**

  解析顺序为环境变量 `MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY`、`XDG_DATA_HOME/modernime/pinyin/modernime-knowledge.dict`、`HOME/.local/share/modernime/pinyin/modernime-knowledge.dict`、编译时安装数据目录和 `/usr/share/modernime/pinyin/modernime-knowledge.dict`。只对 `is_regular_file` 的候选调用 `addEmptyDict()` 和 `load(2, ..., Binary)`；任何文件系统错误都保留系统词典继续运行。

- [ ] **Step 5: 更新安装清单、卸载和安装测试。**

  `install.sh` 的 manifest 增加扩展二进制，`uninstall.sh` 只依据 manifest 删除它；设置客户端安装测试检查 manifest 包含扩展二进制路径，实际安装验证用 `libime_pinyndict -d` 检查文件内容。不要把 build 目录文件写入仓库。

- [ ] **Step 6: 运行 focused、full CTest 和安装。**

  ```bash
  cmake -S . -B build/fcitx5-debug -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DMODERNIME_BUILD_FCITX5=ON -DMODERNIME_BUILD_LIBIME_PINYIN=ON -DMODERNIME_BUILD_SETTINGS=ON -DMODERNIME_BUILD_TESTS=ON
  cmake --build build/fcitx5-debug -j2
  ctest --test-dir build/fcitx5-debug --output-on-failure
  cmake --install build/fcitx5-debug
  ```

- [ ] **Step 7: 提交接入功能。**

  ```bash
  git add CMakeLists.txt data/pinyin/CMakeLists.txt algorithm/pinyin/CMakeLists.txt algorithm/pinyin/include/modernime/pinyin/pinyin_candidate_provider.h algorithm/pinyin/src/pinyin_candidate_provider.cpp install.sh uninstall.sh tests/core/pinyin_provider_test.cpp tests/install_runtime_test.sh
  git commit -m "接入扩展拼音词典"
  ```

### Task 3: 阶段验收和合并准备

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/plans/2026-08-28-pinyin-knowledge-base-expansion.md`
- Test: `build/fcitx5-debug` generated artifacts only; do not commit them.

**Interfaces:**
- README documents the offline dataset, installed dictionary path, data regeneration command and the boundary that abbreviation matching is the next phase.

- [ ] **Step 1: 更新用户可见安装说明。**

  明确 `./install.sh` 会构建并安装扩展词典，数据保存在 `share/modernime/pinyin/`，构建不需要 Python；维护者更新数据时才需要执行生成器和 pypinyin。

- [ ] **Step 2: 做新鲜的完整验证。**

  Run: `git diff --check && cmake --build build/fcitx5-debug -j2 && ctest --test-dir build/fcitx5-debug --output-on-failure && cmake --install build/fcitx5-debug`

  Expected: build exit code 0、全部 CTest 通过、安装的扩展二进制存在且与 build 产物摘要一致。

- [ ] **Step 3: 提交文档更新。**

  ```bash
  git add README.md docs/superpowers/plans/2026-08-28-pinyin-knowledge-base-expansion.md
  git commit -m "完善拼音知识库安装说明"
  ```

- [ ] **Step 4: 汇总隔离分支提交并请求合并到 main。**

  在合并前确认 `git status --short` 只有干净输出，列出三个中文提交和构建/测试/安装证据；不自动推送远程仓库，除非用户另行要求。
