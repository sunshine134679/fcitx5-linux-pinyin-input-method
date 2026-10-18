# T18 验收报告：统一页面解析与文档事实校准

- **任务代号**：T18（P2-05 统一页面解析与文档事实）
- **工作区**：`/home/wsl/ModernIME/.worktrees/parallel-t18-20261017`
- **当前分支**：`codex/architecture-t18-20261017`
- **基线提交**：`631d2584a2a5d0fc69fc9af0b07251210856f74a`
- **验收状态**：`LOCAL_VERIFIED`
- **接口契约**：`C18-PAGE-REGISTRY-V1.0`

---

## 1. 任务背景与核心完成项

本次 T18 任务严格遵循并行开发治理原则，聚焦于“收敛设置端各层页面解析”、“修复既有页面跳转缺陷”、“支持已有实例 DBus 目标与子定位”以及“六大核心文档事实核准”：

1. **唯一页面解析入口 (`parseSettingsPage`)**：
   - 在 `tools/settings/include/modernime/settings/page_registry.h` 与 `tools/settings/page_registry.cpp` 中定义并实现 `parseSettingsPage(std::string_view) -> std::optional<SettingsPageId>`。
   - 完整覆盖规范 8 大页面：`overview`、`input`、`candidates`、`dictionary`、`clipboard`、`learning`、`updates`、`diagnostics`。
   - 健壮性保障：支持首尾空白符自动修剪（trim）与大小写不敏感折叠（case-folding）。
   - 全面兼容既有 CLI 别名与中文常用别名：
     - `overview`: `home`, `概览`
     - `input`: `basic`, `输入与按键`, `输入`
     - `candidates`: `appearance`, `candidate`, `候选与外观`, `候选`, `外观`
     - `dictionary`: `dict`, `词库与词典`, `词库`, `个人词典`, `词典`
     - `clipboard`: `clip`, `剪贴板`
     - `learning`: `智能学习`, `自学习`, `学习`
     - `updates`: `update`, `upgrade`, `更新与维护`, `更新`, `检查更新`
     - `diagnostics`: `diagnostic`, `status`, `diag`, `system`, `系统与诊断`, `诊断`, `状态`
   - 对未知页面名称返回 `std::nullopt`，无隐式状态污染。

2. **通用入口与既有缺陷修复 (`SettingsWindow::show`)**：
   - 增加 `SettingsWindow::show(SettingsPageId page, std::string_view target = {})` 通用入口与 `window()` 访问器；所有旧版包装函数（`showOverviewPage()`、`showBasicPage()` 等）完全委托给通用 `show()`。
   - **历史 Bug 根除**：原 `SettingsWindow::showCandidatePage()` 硬编码调用 `shell_->show(SettingsPageId::Input)`，导致候选与外观页请求错误跳转至输入页，现已彻底修正为 `SettingsPageId::Candidates`。
   - 彻底移除 `settings_app.cpp` 中冗余的字符串 `if-else` 分支，统一委托 `parseSettingsPage`。
   - 命令行未知页面时输出明确 stderr 诊断错误（`Unknown settings page: <name>`）并返回退出码 1。

3. **GApplication 命令行信号与多实例隔离 DBus 转发**：
   - 在 `settings_app.cpp` 中为 `GtkApplication` 添加 `G_APPLICATION_HANDLES_COMMAND_LINE` 标志，并连接 `"command-line"` 信号。
   - 后续次级实例启动时，不再重复弹出互相冲突的多个设置窗口，而是通过会话 DBus 自动激活已有主实例窗口，并将目标页面与 `--target` 子定位锚点精准传递给已有实例展示。
   - 编写受版本控制的测试脚本 `tests/settings/settings_navigation_dbus_test.sh`，在 `dbus-run-session` 隔离沙箱中完整测试主实例启动、次级实例页面/子定位转发、别名跳转及未知页面负向诊断，并在无头环境下返回 77 明确跳过。

4. **六大文档事实严格核准**：
   - `docs/HANDOVER.md`：校准 C++20 与 CMake ≥ 3.24，替换旧 `src/engine/` 为真实 6 模块架构；更新测试描述为动态 Profile（Full 74+，Core-only 25+）；彻底清除直接 `git push origin main` 与 `git add -A` 指引，替换为安全分支流程。
   - `README.md`：声明 C++20，全面补充设置客户端 CLI 选项、子项锚定（`--target`）、DBus 单实例唤醒机制与未知参数诊断说明。
   - `docs/ALGORITHM_ARCHITECTURE.md`：更新 M0–M4 迁移状态（区分已落实的核心模块 vs 待持续推进的跨版本 CI 回放）；详述差量自学习（Deviation-Only Learning）及 5 大核心词正向频次记录例外。
   - `docs/ALGORITHM_EVOLUTION.md`：详述 5 大正向频次记录例外；明确区分“不可信外部词典导入（截断至 3/20）”与“可信本地备份恢复（原样保留）”；如实记录 T11 过渡状态与无物理显示环境下的客观边界；完整保留 256→512 分片历史与历史反例。
   - `docs/CANDIDATE_PIPELINE_ARCHITECTURE.md`：明确设置端候选外观页（`SettingsPageId::Candidates`）的职责边界（仅调整栏宽与预览，不修改学习或排序）。
   - `docs/STABILITY_AUDIT.md`：追加 T18 设置客户端候选页路由修复与页面统一解析条目。

---

## 2. 验证执行与关卡矩阵

### 2.1 专项测试执行

1. **`modernime_page_registry_tests`**：
   - 覆盖全部 8 组规范页面标识；
   - 覆盖所有历史别名与中文别名映射；
   - 覆盖大小写不敏感（`CaNdIdAtEs`）、首尾空格（`  clip  `）；
   - 覆盖未知页面返回 `nullopt`。
   - **结果**：PASS (exit code 0)。

2. **`modernime_settings_shell_gtk_tests`**：
   - 验证 `SettingsWindow::show` 通用跳转；
   - 验证 `showCandidatePage()` 正确指向 `SettingsPageId::Candidates`；
   - 验证旧版包装器委托；
   - 验证带 `--target` 的子项锚定。
   - **结果**：PASS (exit code 0)。

3. **`tests/settings/settings_navigation_dbus_test.sh`**：
   - 验证 `dbus-run-session` 隔离环境下主实例拉起；
   - 验证次级实例向主实例转发 `--page candidates --target candidate-font-size`；
   - 验证次级实例向主实例转发 `--clipboard`；
   - 验证未知页面 `--page invalid_dummy_page` 输出明确诊断并非零退出；
   - 验证沙箱 FUSE 挂载点在 cleanup trap 中安全卸载并退出。
   - **结果**：PASS (exit code 0)。

### 2.2 全量构建与 CTest 套件（`build/parallel-t18-debug`）

在全局互斥锁 `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock` 保护下执行全量测试：
```bash
ctest --test-dir build/parallel-t18-debug --output-on-failure
```
- **注册测试总数**：77
- **通过测试数**：77
- **失败测试数**：0
- **通过率**：100%
- **总耗时**：994.47 秒（包含完整的 Release 性能基准、1,000 轮日常打字仿真、打包与安装回滚测试）

### 2.3 门禁准入工具核验 (`tests/verify_required_tests.py`)

```bash
python3 tests/verify_required_tests.py --registered build/parallel-t18-debug/registered_tests.json --profile full --stage final
```
- **核验结果**：`[OK] Required tests check passed (profile=full, stage=final, execution=headless).`
- 36 项必需测试（29 项 baseline + 7 项 final）全部注册并具备合规覆盖。

---

## 3. 受控变更清单 (Changed Files)

以下文件为 T18 精确修改与新增受控文件：

### 生产源码与头文件
- `tools/settings/include/modernime/settings/page_registry.h`
- `tools/settings/page_registry.cpp`
- `tools/settings/include/modernime/settings/settings_window.h`
- `tools/settings/settings_window.cpp`
- `tools/settings/settings_app.cpp`

### 测试代码与构建配置
- `tests/settings/page_registry_test.cpp`
- `tests/settings/settings_shell_gtk_test.cpp`
- `tests/settings/settings_navigation_dbus_test.sh`
- `tests/CMakeLists.txt`

### 事实校准文档
- `docs/HANDOVER.md`
- `README.md`
- `docs/ALGORITHM_ARCHITECTURE.md`
- `docs/ALGORITHM_EVOLUTION.md`
- `docs/CANDIDATE_PIPELINE_ARCHITECTURE.md`
- `docs/STABILITY_AUDIT.md`
- `docs/T18_ACCEPTANCE.md`（本报告）

*(注：临时参考文档未纳入 git 暂存，避免污染代码库)*

---

## 4. 遗留事项与客观边界

1. **真实桌面物理环境确认**：本轮自动化验证在 WSL / X11 虚拟显示与 DBus 会话中完成并通过；设置窗口的真实物理高分屏缩放与桌面视觉微调需待最终桌面验收。
2. **分支移交**：本分支（`codex/architecture-t18-20261017`）不直接推送到远程，待协调者审查核准后统一合并。
