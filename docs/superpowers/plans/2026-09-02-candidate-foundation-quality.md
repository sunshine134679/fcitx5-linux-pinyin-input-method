# Candidate Foundation Quality Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 ModernIME 在固定宽度候选栏中完整分页，并提高长句混排、常见纠错和短简拼的首屏质量。

**Architecture:** 将候选混排提取为可独立测试的纯逻辑单元，以最佳整句派生可信前缀并结合真实解码候选；纠错继续依托 LibIME 模糊标志并由现有排序器保证精确匹配优先。将可变分页边界建模为纯逻辑值，由 Fcitx 候选列表和自定义 UI 共同消费，所有导航和选择共享同一全局索引。

**Tech Stack:** C++20、LibIME Pinyin、Fcitx5 CandidateList、GTK3/Cairo、CMake/CTest

**Spec:** `docs/superpowers/specs/2026-09-02-candidate-foundation-quality-design.md`

## Global Constraints

- 运行时默认完全离线，不新增网络请求、云候选或遥测。
- 候选栏外观、固定宽度、字体、颜色和圆角保持不变。
- 首候选仍由可信的完整句转换占据；部分候选只提供不同的选词路径，不得用低质量同音词凑满首屏。
- 选择部分候选后只提交已选择文字，剩余拼音继续保留。
- 所有行为修改严格执行 RED→GREEN，使用真实 LibIME、真实候选列表和手工推导的期望值。
- 完整测试套件必须零失败；无图形后端时 GTK 测试允许按既有规则跳过。

---

### Task 1: 质量驱动的多粒度候选混排

**Files:**
- Create: `algorithm/pinyin/include/modernime/pinyin/candidate_mixer.h`
- Create: `algorithm/pinyin/src/candidate_mixer.cpp`
- Modify: `algorithm/pinyin/CMakeLists.txt`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Test: `tests/core/pinyin_provider_test.cpp`
- Test: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: 已排序的完整候选、部分候选、最佳完整句、音节边界和原始拼音。
- Produces: `mixCandidateItems(...) -> std::vector<core::CandidateItem>`；结果保留全局 `sourceIndex`、`fullPinyin` 和 `consumedInputBytes`。

- [ ] **Step 1: 写失败的真实候选测试**

在 `tests/core/pinyin_provider_test.cpp` 增加独立用例，手工断言：

```cpp
assertTrue(provider.append("qingbangwodakaiwenjian"), "long request is accepted");
assertTrue(provider.page().items.front().text == "请帮我打开文件", "best sentence stays first");
assertTrue(indexOf(provider.page(), "请帮我") < 5, "trusted three-character prefix is visible");
assertTrue(indexOf(provider.page(), "请帮") < 5, "trusted two-character prefix is visible");

assertTrue(provider.append("nengbunengbangwokanxia"), "ambiguous sentence is accepted");
assertTrue(indexOf(provider.page(), "能不能帮我看下") < 3, "semantic sentence reaches top three");
```

同时断言 `qingbang...` 前五项中“青帮、清帮、青棒”最多出现一项。生产变更若恢复固定取三个同音短词，此测试必须失败。

- [ ] **Step 2: 运行 RED**

Run: `cmake --build --preset fcitx5-debug -j2 --target modernime_pinyin_provider_tests && ctest --test-dir build/fcitx5-debug -R '^modernime_pinyin_provider$' --output-on-failure`

Expected: FAIL，缺少“请帮我”或“请帮”，或者正确“看下”未进入前三。

- [ ] **Step 3: 实现最小混排器**

实现 UTF-8 字符边界切分，从最佳完整句按音节数派生三音节和二音节前缀；派生项设置对应 `consumedInputBytes`，不调用无效 `sourceIndex` 的完整选择路径。按“最佳整句、可信较长前缀、可信两字前缀、一个真实同音备选、其余完整句”去重混排。删除 provider 内固定 `candidateLimit == 3` 和首字替换凑数逻辑。

- [ ] **Step 4: 运行 GREEN 和相关集成测试**

Run: `cmake --build --preset fcitx5-debug -j2 --target modernime_pinyin_provider_tests modernime_pinyin_engine_tests && ctest --test-dir build/fcitx5-debug -R 'modernime_(pinyin_provider|pinyin_engine)$' --output-on-failure`

Expected: 2/2 PASS。

- [ ] **Step 5: 提交**

```bash
git add algorithm/pinyin tests/core/pinyin_provider_test.cpp tests/CMakeLists.txt
git commit -m "优化长句多粒度候选混排"
```

### Task 2: 常见模糊音与短简拼排序

**Files:**
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `core/src/pinyin_match.cpp`
- Test: `tests/core/pinyin_provider_test.cpp`
- Test: `tests/core/pinyin_match_test.cpp`

**Interfaces:**
- Consumes: LibIME `PinyinFuzzyFlags` 与 `PinyinMatchPolicy::priority`。
- Produces: 精确匹配继续高于模糊匹配；可信三字母简拼中文候选高于 Raw，但普通英文 `who` 仍由 Raw 占首位。

- [ ] **Step 1: 写失败测试**

增加真实 provider 断言：

```cpp
assertTrue(provider.append("zongguo"), "fuzzy input is accepted");
assertTrue(indexOf(provider.page(), "中国") < 3, "z/zh fuzzy match is recoverable");
provider.reset();
assertTrue(provider.append("wsm"), "short abbreviation is accepted");
assertTrue(provider.page().items.front().text == "为什么", "common short abbreviation wins");
assertTrue(indexOf(provider.page(), "wsm") < provider.page().items.size(), "raw abbreviation remains available");
provider.reset();
assertTrue(provider.append("who"), "English word is accepted");
assertTrue(provider.page().items.front().text == "who", "ordinary English stays raw first");
```

- [ ] **Step 2: 运行 RED**

Run: `cmake --build --preset fcitx5-debug -j2 --target modernime_pinyin_provider_tests modernime_pinyin_match_tests && ctest --test-dir build/fcitx5-debug -R 'modernime_(pinyin_provider|pinyin_match)$' --output-on-failure`

Expected: FAIL，`zongguo` 不含“中国”或 `wsm` Raw 仍在第一。

- [ ] **Step 3: 实现保守纠错和简拼置信度**

在创建共享 `PinyinIME` 时组合 `CommonTypo | Z_ZH | C_CH | S_SH | L_N | EN_ENG | IN_ING`。保留 `priority` 的精确匹配最高级；把 Raw 前置判断从固定长度改为“是否存在可信简拼候选”，可信条件仅包含真实 LibIME 中文候选、完整简拼键相等且命中常见短语，普通英文仍由 Raw 优先。若需要词表，限定为小型内置高频短简拼集合并使用行为测试保护。

- [ ] **Step 4: 运行 GREEN 与排序回归**

Run: `cmake --build --preset fcitx5-debug -j2 --target modernime_pinyin_provider_tests modernime_pinyin_match_tests modernime_settings_tests && ctest --test-dir build/fcitx5-debug -R 'modernime_(pinyin_provider|pinyin_match|settings)$' --output-on-failure`

Expected: 3/3 PASS。

- [ ] **Step 5: 提交**

```bash
git add algorithm/pinyin/src/pinyin_candidate_provider.cpp core/src/pinyin_match.cpp tests/core
git commit -m "增强拼音纠错与短简拼排序"
```

### Task 3: 可变候选分页边界

**Files:**
- Create: `ui/fcitx5/include/modernime/ui/candidate_pagination.h`
- Create: `ui/fcitx5/src/candidate_pagination.cpp`
- Modify: `ui/fcitx5/CMakeLists.txt`
- Modify: `ui/fcitx5/src/candidate_bar_layout.cpp`
- Modify: `ui/fcitx5/src/ui_addon.cpp`
- Modify: `adapter/fcitx5/include/modernime/fcitx5/fcitx_engine.h`
- Modify: `adapter/fcitx5/src/inputmethod.cpp`
- Modify: `adapter/fcitx5/include/modernime/fcitx5/engine.h`
- Modify: `adapter/fcitx5/src/engine.cpp`
- Test: `tests/ui/candidate_bar_layout_test.cpp`
- Test: `tests/adapter/fcitx5_input_panel_test.cpp`
- Test: `tests/adapter/engine_state_test.cpp`

**Interfaces:**
- Produces: `CandidatePagination::partition(items, metrics, textWidth) -> std::vector<PageBoundary>`，其中每个 `PageBoundary{begin,end}` 连续、非空、覆盖全部候选且互不重叠。
- Candidate list/navigation consumes the same boundary vector for local labels, digits, cursor movement and page movement.

- [ ] **Step 1: 写失败的纯逻辑和集成测试**

用确定性宽度函数构造 10 个长短不同候选，手工断言边界形如 `{0,4},{4,7},{7,10}`，并逐项收集所有页面内容，断言原始全局索引 `0..9` 各出现一次。Fcitx 输入面板测试断言第 5、9、10 项均能通过翻页和本地数字键选中，生产变更若以 placeholder 隐藏候选或固定 `9` 跳页则失败。

- [ ] **Step 2: 运行 RED**

Run: `cmake --build --preset fcitx5-debug -j2 --target modernime_candidate_bar_layout_tests modernime_fcitx5_input_panel_tests modernime_engine_state_tests && ctest --test-dir build/fcitx5-debug -R 'modernime_(candidate_bar_layout|fcitx5_input_panel|engine_state)$' --output-on-failure`

Expected: FAIL，现有实现没有可变边界且第 5～9 项会被隐藏或跳过。

- [ ] **Step 3: 实现统一分页模型**

分页器复用候选布局的固定宽度、padding、gap 和实际文字测量，依次打包所有候选。候选列表保留全局索引并公开当前页局部候选；所有选择回调最终调用 `ModernIMEController::select(globalIndex)`。移除 `markAsNotDisplayed`/placeholder 绕过路径，PageUp/PageDown 按边界移动，Space 和数字键使用同一全局光标。

- [ ] **Step 4: 运行 GREEN 和全部候选交互测试**

Run: `cmake --build --preset fcitx5-debug -j2 && ctest --test-dir build/fcitx5-debug -R 'modernime_(candidate_bar_layout|candidate_bar_renderer|fcitx5_candidate_list|fcitx5_input_panel|engine_state|key_translation)$' --output-on-failure`

Expected: 相关测试全部 PASS。

- [ ] **Step 5: 提交**

```bash
git add ui/fcitx5 adapter/fcitx5 tests/ui tests/adapter
git commit -m "按候选内容完整划分固定宽度分页"
```

### Task 4: 候选质量回归基线与最终验收

**Files:**
- Create: `tests/core/pinyin_quality_test.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`

**Interfaces:**
- Consumes: Tasks 1–3 的用户可感知行为。
- Produces: `modernime_pinyin_quality` 离线回归目标；README 说明模糊音、短简拼和固定宽度动态分页。

- [ ] **Step 1: 写独立质量回归测试**

使用真实系统/扩展词典覆盖以下字面期望：`jintiantianqihenhao→今天天气很好@1`、`qingbangwodakaiwenjian→请帮我打开文件@1 + 请帮我@Top5`、`nengbunengbangwokanxia→能不能帮我看下@Top3`、`zongguo→中国@Top3`、`wsm→为什么@1`、`who→who@1`、`xign→xing候选存在`，并验证选择“请帮我”后剩余 `dakaiwenjian`。

- [ ] **Step 2: 运行测试并确认能捕获任一行为回退**

Run: `cmake --build --preset fcitx5-debug -j2 --target modernime_pinyin_quality_tests && ctest --test-dir build/fcitx5-debug -R '^modernime_pinyin_quality$' --output-on-failure`

Expected: PASS；临时在本地反转任一断言对应排序可观察 FAIL，随后恢复，不提交临时变更。

- [ ] **Step 3: 更新 README**

在“常用功能”中说明：固定宽度候选栏按文字长度决定本页数量且不会跳过候选；常见模糊音和三字母高频简拼可恢复；所有能力默认离线。

- [ ] **Step 4: 完整构建和测试**

Run: `cmake --build --preset fcitx5-debug -j2 && ctest --test-dir build/fcitx5-debug --output-on-failure`

Expected: 46 个测试目标零失败；无图形后端时既有两项 GTK 测试允许显示 `Skipped`。

- [ ] **Step 5: 安装验收并提交**

Run: `./install.sh`

Expected: 安装脚本构建、测试和用户前缀安装成功；没有图形会话时只跳过 Fcitx 自动重载。

```bash
git add tests/core/pinyin_quality_test.cpp tests/CMakeLists.txt README.md
git commit -m "建立候选质量离线回归基线"
```
