# Candidate foundation / quality 最终修复报告

## 范围

- 普通候选页在 GTK 尚未测量或使用其他 Fcitx UI 时，由 controller 先生成连续、完整覆盖且每页最多 9 项的保守边界。
- GTK 后续提供的有效实测边界仍通过现有 `setPageBoundaries(...)` 整体替换 fallback；键盘、Space、数字和 Fcitx 标准导航继续读取同一个 controller 边界。
- `modernime_pinyin_quality` 显式使用测试私有、确认不存在的用户词典路径；learning 与 context learning 继续关闭，生产默认路径未改。

## RED

### 无 UI 测量边界的第 10 项

先在真实 Fcitx `InputPanel` integration test 中发布 fresh 10-item provider 页，不手工写入边界，并断言 controller 已有 fallback、首屏至多 9 项、PageDown 后数字 `1` 与 Space 都能提交第 10 项。

```bash
cmake --build build/fcitx5-debug --target modernime_fcitx5_input_panel_tests -j2
./build/fcitx5-debug/tests/modernime_fcitx5_input_panel_tests
```

构建成功，测试按预期失败于：

```text
fcitx5 input panel test failed: controller supplies conservative boundaries before UI measurement
```

### 质量基线账户依赖

测试先在独占临时 `XDG_DATA_HOME` 的默认用户词典位置写入哨兵词条，再从质量 provider 输入对应拼音，断言该词条不得以 `CandidateSource::UserDictionary` 出现。此时 `makeProvider()` 仍传空路径。

```bash
cmake --build build/fcitx5-debug --target modernime_pinyin_quality_tests -j2
./build/fcitx5-debug/tests/modernime_pinyin_quality_tests
```

构建成功，测试按预期失败于：

```text
pinyin quality test failed: quality provider ignores account dictionary data
```

这证明空 `paths.userDictionary` 会读取当前账户默认路径，而不是独立基线。修复仅把质量测试 factory 指向同一独占临时目录中的 `absent-user-dictionary.txt`，并在构造 provider 前确认该文件不存在。

## 实现

- 新增 `kFallbackCandidatePageSize = 9` 与 `ModernIMEController::ensurePageBoundaries()`；只在普通候选刷新且页面未自带边界时，生成 `{0,9},{9,18},...` 形式的连续 fallback。
- 已存在的 provider/UI 边界不被覆盖；GTK 实测边界的现有替换路径及有效性校验保持不变。
- 质量测试用 `mkdtemp` 创建独占目录，在其中布置受控账户哨兵并选择另一个不存在的路径作为显式用户词典；析构时恢复 `XDG_DATA_HOME`，只删除自己创建的目录。
- 生产 `PinyinDataPaths` 的空路径默认规则没有修改；quality factory 中 learning/context learning 仍均为 `false`。

## GREEN 与验证

聚焦验证：

```bash
cmake --build build/fcitx5-debug --target modernime_fcitx5_input_panel_tests modernime_pinyin_quality_tests -j2
ctest --test-dir build/fcitx5-debug --output-on-failure -R 'modernime_(fcitx5_input_panel|pinyin_quality|pinyin_engine|engine_state)'
```

结果：4/4 PASS，0 failed；临时质量目录在正常退出后无残留。

完整验证：

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
git diff --check
```

结果：完整 Debug 构建成功；46/46 测试中 0 failed，2 个既有 GTK 测试因无显示环境按规则 SKIP；`git diff --check` 无错误。

## Concerns

- 无已知功能阻塞或回归。
- 当前环境未安装 `clang-format`；修改已按现有样式人工检查，并由完整编译和 `git diff --check` 验证。

## Final fix round 2：provider fresh page 统一发布

### 根因

首轮 fallback 只在 `refreshPage()` 中补齐。partial selection 和 candidate removal 都会直接执行 `page_ = provider_->page()` 后发布；provider 返回的新页没有 GTK 测量边界时，这两条路径再次暴露空边界，10 项以上的剩余候选会被 Fcitx 列表退化为单页。

### RED

新增 deterministic provider fake，但所有操作都通过真实 `ModernIMEController` 公共路径完成：

- 选择消费部分输入的候选后，provider 返回 11 项 fresh remainder；
- 删除候选后，provider 返回 11 项 fresh remainder；
- 两者均要求 controller 生成字面边界 `{0,9},{9,11}`，再用 PageDown + 数字 `2` 提交全局第 11 项。

```bash
cmake --build build/fcitx5-debug --target modernime_adapter_tests -j2
./build/fcitx5-debug/tests/modernime_adapter_tests
```

生产代码未改时构建成功，测试稳定失败于：

```text
engine state test failed: partial selection applies fallback to the fresh provider page
```

### 实现与 GREEN

- 新增唯一的 `publishProviderPage(...)`：复制 provider page；partial remainder 场景先更新 composition；随后统一更新 preedit cursor、补齐 fallback、发布。
- partial selection、removal、reset/clear 和常规 refresh 的 provider page 接收都走该 helper；源码中只剩 helper 内一处 `page_ = provider_->page()`。
- 完整候选选择不发布中间 provider page，仍只发布 reset 后空页，避免重复刷新。
- `ensurePageBoundaries()` 对已有非空边界早退，因此 GTK 实测边界仍可替换并持续保留。

聚焦验证：

```bash
cmake --build build/fcitx5-debug --target modernime_adapter_tests modernime_fcitx5_input_panel_tests modernime_pinyin_engine_tests modernime_fcitx5_candidate_list_tests -j2
ctest --test-dir build/fcitx5-debug --output-on-failure -R 'modernime_(engine_state|fcitx5_input_panel|pinyin_engine|fcitx5_candidate_list)$'
```

结果：4/4 PASS，0 failed。

完整验证：

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
git diff --check
```

结果：完整 Debug build 成功；46 项 CTest 中 0 failed，2 项既有 GTK 测试因无显示环境 SKIP；diff check 无错误。

### Round 2 concerns

- 无已知功能阻塞或回归。
- deterministic fake 仅构造 fresh provider page；分页、选择、删除、PageDown 和数字提交全部执行真实 controller 代码。
