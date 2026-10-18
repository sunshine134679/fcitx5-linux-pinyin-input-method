# T18 验收报告：统一页面解析与文档事实校准（第二轮复核 S1–S3 闭环）

- **任务代号**：T18（P2-05 统一页面解析与文档事实）
- **工作区**：`/home/wsl/ModernIME/.worktrees/parallel-t18-20261017`
- **当前分支**：`codex/architecture-t18-20261017`
- **基线提交 (Base SHA)**：`631d2584a2a5d0fc69fc9af0b07251210856f74a`
- **首轮复审提交**：`a1c80495d0445ef2d7ef31436f36c793bb86a741`
- **次轮复核提交**：`d2e03d436ee720c6a7a32250879b46680ee571fb`
- **验收状态**：`LOCAL_VERIFIED`（停在 T18 等待协调者复审，严禁自行宣布 T19 完成）
- **接口契约**：`C18-PAGE-REGISTRY-V1.0`

---

## 1. 任务背景与核心交付

T18 任务严格遵循并行开发治理原则，聚焦于“收敛设置端各层页面解析”、“修复既有页面跳转缺陷”、“支持已有实例 DBus 目标与子定位”以及“核心文档事实核准”：

1. **唯一页面解析入口 (`parseSettingsPage`)**：
   - 在 `tools/settings/include/modernime/settings/page_registry.h` 与 `tools/settings/page_registry.cpp` 中实现 `parseSettingsPage(std::string_view) -> std::optional<SettingsPageId>`。
   - 规范 8 大页面英文名称：`overview`、`input`、`candidates`、`dictionary`、`clipboard`、`learning`、`updates`、`diagnostics`。
   - 规范英文别名与 8 个规范中文页面标题：`概览`、`输入与按键`、`候选与外观`、`个人词典`、`剪贴板`、`智能学习`、`更新与安装`、`系统与诊断`。
   - 首尾空白符修剪（trim）与 ASCII 大小写折叠（case-folding），未知页面明确返回 `std::nullopt`。

2. **通用入口与既有缺陷修复 (`SettingsWindow::show`)**：
   - 增加 `SettingsWindow::show(SettingsPageId page, std::string_view target = {})` 通用入口与 `window()` 访问器；旧版包装函数完全委托通用 `show()`。
   - **历史 Bug 根除**：原 `SettingsWindow::showCandidatePage()` 硬编码调用 `shell_->show(SettingsPageId::Input)`，彻底修正为 `SettingsPageId::Candidates`。
   - 移除 `settings_app.cpp` 中冗余的字符串分支，统一委托 `parseSettingsPage`。

3. **GApplication 命令行信号与多实例隔离 DBus 转发**：
   - 为 `GtkApplication` 添加 `G_APPLICATION_HANDLES_COMMAND_LINE` 标志，接入 `"command-line"` 信号。
   - 次级实例启动时，通过会话 DBus 自动唤起已有主实例窗口，并将目标页面与 `--target` 子定位锚点精准传递给已有实例展示。

---

## 2. 第二轮复核 S1–S3 闭环事实

根据协调者 `/home/wsl/ModernIME/docs/T18_SECOND_REVIEW.md` 意见，已完成以下三项定向缺陷修复与实测闭环：

### 2.1 S1 / P1：状态新鲜度序号绑定、真实 UI 采样与拒绝旧快照

- **问题根因**：首轮整改中，非法请求提前返回时未导出新状态，Step 5 读取的是 Step 4（clipboard）留下的旧 JSON。若构建副本包含“已有窗口收到非法页时误切到 Candidates 但正常返回 1”的 mutant，正式门禁因读取旧文件而误判通过（审查方已通过 `modernime-settings-mutant` 确凿反证）。
- **整改实现**：
  1. **观察序号与新鲜度绑定**：在 `ApplicationState` 中增加 `uint64_t sequenceNumber = 0`，每次状态导出时递增并在 JSON 中输出 `"sequence"`。
  2. **生产端次级非法请求即时观察**：在 `tools/settings/settings_app.cpp` 的 `commandLine()` 中，当非法页面请求到达且 `state->window != nullptr` 时，在设置 `EXIT_FAILURE` 后立即调用 `dumpSettingsStateIfConfigured(state)`，递增序号并导出当前未被篡改的真实 GTK 状态；首实例 `state->window == nullptr` 时依然不建窗、不导出、确定性非零退出。
  3. **测试端新鲜度保证与未导航采样**：在 `tests/settings/settings_navigation_dbus_test.sh` Step 5 中，记录前序序号 `PRE_ERR_SEQ`；次级非法命令返回非零后，若序号未增长（或被测二进制缺失序号字段），测试脚本触发一次无参数采样命令（激活已有实例但不改变页面），确保获取非法请求处理完成后的**新鲜实际控件快照**。
  4. **目标断言拦截**：严格断言新鲜快照中 `visible_page == "clipboard"`、`window_count == 1`、`pid == PRIMARY_PID`。审查方 mutant 切到 candidates 的错误行为在 `[REJECT: invalid_page_side_effect]` 目标断言上 100% 失败被拒。
  5. **自包含对照夹具与精确阶段记录**：将所有对照程序改写为自包含夹具（位于私有沙箱内，不依赖外部可选文件），保持基础协议正常（同一 PID、有界寿命），并使用 `verify_contrast_rejected` 严格核验具体拒绝阶段标签：
     - `fake-settings` (no GTK, exit 0) -> `[REJECT: first_invalid_exit_code]`
     - `contrast_exit0` (非法页 exit 0) -> `[REJECT: first_invalid_exit_code]`
     - `contrast_no_nav` (不导航) -> `[REJECT: no_navigation]`
     - `contrast_drop_target` (丢 target) -> `[REJECT: dropped_target]`
     - `contrast_multi_win` (另开窗口) -> `[REJECT: multiple_windows]`
     - `contrast_side_effect` (S1 真实副作用 mutant) -> `[REJECT: invalid_page_side_effect]`
     - 审查方 `modernime-settings-mutant` -> `[REJECT: invalid_page_side_effect]`

### 2.2 S2 / 验收边界：四格矩阵（先确认必需 binary，再按显示环境跳过）

- **问题根因**：旧版脚本先检查 `DISPLAY/WAYLAND_DISPLAY` 再检查传入 binary，导致“无显示 + 传入不存在的 binary”时返回了 77，掩盖了缺少产物的致命错误。
- **整改实现**：
  在 `tests/settings/settings_navigation_dbus_test.sh` 脚本最前置处调整顺序：
  1. 第一步：检验 `$SETTINGS_BIN` 是否提供且可执行，若缺失或不可执行直接 `exit 1`（无论是否有显示）。
  2. 第二步：在确认 binary 存在后，若 `DISPLAY` 与 `WAYLAND_DISPLAY` 均为空，返回契约允许的 `exit 77` 跳过。
- **四格矩阵实测结果**：
  - 缺参数/不存在 + 有显示：`exit 1`（PASS）
  - 缺参数/不存在 + 无显示：`exit 1`（PASS，修复此前 77 误判）
  - 有效二进制 + 无显示：`exit 77`（PASS）
  - 有效二进制 + 有显示：进入完整门禁（PASS）

### 2.3 S3 / P2：HANDOVER JUnit 绝对路径与 Debug/Release 配对口径

- **问题根因**：`docs/HANDOVER.md` 中两处 `ctest --output-junit build/fcitx5-debug/junit.xml` 使用了相对路径，会被 CTest 在 `--test-dir` 下再次嵌套为 `build/fcitx5-debug/build/fcitx5-debug/junit.xml`，导致下行 checker 找不到报告。
- **整改实现**：
  1. 将命令明确改为绝对路径：`ctest --test-dir build/fcitx5-debug --output-junit "$PWD/build/fcitx5-debug/junit.xml" --output-on-failure`。
  2. 沿用 Debug 功能测试与独立 Release benchmark（G3/G7）配对口径，明确标明 Debug 目录不得充当 Release 性能基准，Benchmark 性能门禁须由独立 Release 构建生成。

---

## 3. 接线申请与协调事项 (Wiring Request)

本工作树遵守协调者文件所有权，未擅自修改共享 `CMakeLists.txt` 或 `tests/verify_required_tests.py`。特向协调者提交以下接线申请：

> **接线申请 (Wiring Request for Coordinator)**：
> 1. 请协调者在最终集成阶段，将设置端单实例 DBus 转发测试 `modernime_settings_navigation_dbus` 纳入最终必需测试清单（`tests/verify_required_tests.py` 中的 `FULL_FINAL_ADDITIONAL`）。
> 2. 请协调者在无显示白名单（`HEADLESS_SKIP_WHITELIST`）中登记 `modernime_settings_navigation_dbus`，确认在物理/虚拟显示缺失时允许返回 `SKIP_RETURN_CODE 77`。

---

## 4. 验证执行结果与证据记录

所有构建与测试均在互斥锁 `machine.lock` 保护下执行：

### 4.1 定向测试执行 (CTest 3/3 Pass)

```bash
ctest --test-dir build/parallel-t18-debug -R "modernime_settings_shell_gtk|modernime_page_registry|modernime_settings_navigation_dbus" --output-on-failure
```
- `modernime_settings_shell_gtk`: **Passed (2.95 sec)**
- `modernime_page_registry`: **Passed (0.00 sec)**
- `modernime_settings_navigation_dbus`: **Passed (19.80 sec)**
- **测试通过率**：3/3 (100% Pass)

### 4.2 对照门禁与边界实测

- **S2 四格边界实测**：
  - `Cell 1 (missing binary + display)`: `EXIT=1`
  - `Cell 2 (missing binary + headless)`: `EXIT=1`
  - `Cell 3 (valid binary + headless)`: `EXIT=77`
  - `Cell 4 (valid binary + display)`: 正常执行全量门禁
- **S1 假实现与 Mutant 拒绝实测**：
  - `fake-settings (no GTK, invalid exit 0)`: 正确拦截于 `[first_invalid_exit_code]`
  - `invalid page exit 0`: 正确拦截于 `[first_invalid_exit_code]`
  - `does not navigate`: 正确拦截于 `[no_navigation]`
  - `drops target focus`: 正确拦截于 `[dropped_target]`
  - `opens multiple windows`: 正确拦截于 `[multiple_windows]`
  - `S1 mutant (changes UI to candidates)`: 正确拦截于 `[invalid_page_side_effect]`
  - 审查方 `modernime-settings-mutant`: 正确拦截于 `[invalid_page_side_effect]`

### 4.3 门禁准入工具核验 (`tests/verify_required_tests.py`)

```bash
ctest --test-dir build/parallel-t18-debug --show-only=json-v1 > build/parallel-t18-debug/registered_tests.json
python3 tests/verify_required_tests.py --registered build/parallel-t18-debug/registered_tests.json --profile full --stage final
```
- **核验结果**：`[OK] Required tests check passed (profile=full, stage=final, execution=headless).`

---

## 5. 受控变更清单 (Changed Files)

本轮 S1–S3 收尾精确修改的文件如下：
- `tools/settings/settings_app.cpp`（状态新鲜度 sequence 计数、次级非法请求即时观察）
- `tests/settings/settings_navigation_dbus_test.sh`（四格矩阵先验 binary、S1 新鲜度判定、自包含对比夹具及阶段核验）
- `docs/HANDOVER.md`（修正 `--output-junit` 为绝对路径、明确 Debug/Release 配对口径）
- `docs/T18_ACCEPTANCE.md`（更新本报告及接线申请）

---

## 6. 移交与约束承诺

1. **唯一实现者承诺**：本工作树未启动第二实现者，未修改算法代码，未破坏既有 API 契约，未修改共享 glue/checker。
2. **清洁边界承诺**：未执行 `git reset`、`git clean` 或 `git stash`，未污染未跟踪文件，保留旧复审证据。
3. **停在 T18 等待复审**：本分支（`codex/architecture-t18-20261017`）不直接推送到远程，不合并到 main，不自行宣布 T19 完成，停在当前状态交由协调者复审。
