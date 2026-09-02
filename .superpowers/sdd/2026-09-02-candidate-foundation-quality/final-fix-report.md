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
