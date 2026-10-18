# T18 验收报告：统一页面解析与文档事实校准（R1–R3 整改闭环）

- **任务代号**：T18（P2-05 统一页面解析与文档事实）
- **工作区**：`/home/wsl/ModernIME/.worktrees/parallel-t18-20261017`
- **当前分支**：`codex/architecture-t18-20261017`
- **基线提交**：`631d2584a2a5d0fc69fc9af0b07251210856f74a`
- **首轮复审提交**：`a1c80495d0445ef2d7ef31436f36c793bb86a741`
- **整改状态**：`LOCAL_VERIFIED`（停在 T18 等待协调者复审，严禁自行宣布 T19 完成）
- **接口契约**：`C18-PAGE-REGISTRY-V1.0`

---

## 1. 任务背景与核心交付

T18 任务严格遵循并行开发治理原则，聚焦于“收敛设置端各层页面解析”、“修复既有页面跳转缺陷”、“支持已有实例 DBus 目标与子定位”以及“核心文档事实核准”：

1. **唯一页面解析入口 (`parseSettingsPage`)**：
   - 在 `tools/settings/include/modernime/settings/page_registry.h` 与 `tools/settings/page_registry.cpp` 中实现 `parseSettingsPage(std::string_view) -> std::optional<SettingsPageId>`。
   - 规范 8 大页面英文名称：`overview`、`input`、`candidates`、`dictionary`、`clipboard`、`learning`、`updates`、`diagnostics`。
   - 英文规范别名：`basic` -> `input`; `appearance`, `candidate` -> `candidates`; `dict` -> `dictionary`; `clip` -> `clipboard`; `update` -> `updates`; `status`, `diagnostic` -> `diagnostics`。
   - 健壮性保障：支持首尾空白符自动修剪（trim）与 ASCII 大小写折叠（case-folding）。
   - **R3 校准**：支持全部 8 个规范中文页面标题：`概览`、`输入与按键`、`候选与外观`、`个人词典`、`剪贴板`、`智能学习`、`更新与安装`、`系统与诊断`。
   - 未知页面名称返回 `std::nullopt`，无隐式状态污染。

2. **通用入口与既有缺陷修复 (`SettingsWindow::show`)**：
   - 增加 `SettingsWindow::show(SettingsPageId page, std::string_view target = {})` 通用入口与 `window()` 访问器；旧版包装函数（`showOverviewPage()`、`showBasicPage()` 等）完全委托给通用 `show()`。
   - **历史 Bug 根除**：原 `SettingsWindow::showCandidatePage()` 硬编码调用 `shell_->show(SettingsPageId::Input)`，导致候选与外观页请求错误跳转至输入页，现已彻底修正为 `SettingsPageId::Candidates`。
   - 彻底移除 `settings_app.cpp` 中冗余的字符串 `if-else` 分支，统一委托 `parseSettingsPage`。

3. **GApplication 命令行信号与多实例隔离 DBus 转发**：
   - 为 `GtkApplication` 添加 `G_APPLICATION_HANDLES_COMMAND_LINE` 标志，接入 `"command-line"` 信号。
   - 次级实例启动时，通过会话 DBus 自动唤起已有主实例窗口，并将目标页面与 `--target` 子定位锚点精准传递给已有实例展示。

---

## 2. R1–R3 必须整改项闭环事实

根据协调者 `/home/wsl/ModernIME/docs/T18_REVIEW_AND_HANDOFF.md` 首轮复审意见，已完成以下三项定向缺陷修复与实测闭环：

### 2.1 R1 / P1：首次/已有实例非法页确定性退出契约

- **根因分析**：旧实现中 `commandLine()` 收到信号后立即无条件构造 `SettingsWindow`，向 GTK 注册了顶层窗口并映射展示，随后才在 `applyNavigationRequest()` 中解析页面；即使返回 `EXIT_FAILURE`，GtkApplication 依然持有一个打开的活动窗口，因此进入 GTK 事件循环挂起等待窗口关闭（复审探针 4 秒超时挂起）。
- **整改实现**：
  1. `tools/settings/settings_app.cpp` 重构 `commandLine()`：严格实行“先验证请求，再创建/展示窗口”。
  2. 若 `!req.pageName.empty()` 且 `parseSettingsPage(req.pageName)` 为 `std::nullopt`：
     - 调用 `g_application_command_line_printerr` 输出 `Unknown settings page: <name>\n`；
     - 调用 `g_application_command_line_set_exit_status(cmdline, EXIT_FAILURE)`；
     - 立即返回 `EXIT_FAILURE`。
  3. **首实例无活动窗口**：此时 `state->window` 为 `nullptr`，没有任何窗口注册到 GtkApplication，GtkApplication 检测到既无窗口也无 hold 引用，立即随命令行回调结束而退出进程（实测耗时 < 0.05 秒，退出码 1）。
  4. **次级实例调用失败保护**：若主实例已在运行，次级命令发送非法页面时，同样在回调中直接返回 `EXIT_FAILURE`；主实例现存的 `SettingsWindow` **不被关闭、不改变当前页面、不被篡改**，次级命令接收非零退出码（1）并退出。

### 2.2 R2 / P1：强化隔离 DBus 测试与真实 UI/Target 观察门禁

- **根因分析**：原 DBus 测试仅检查主进程存活及次级命令返回 0，未实际观察主实例内部控件状态，导致无 GTK、无 DBus、无路由的 `fake-settings` 能虚假通过。
- **整改实现**：
  1. **生产端真实 UI 状态导出（窄测试接缝）**：
     - 当环境变量 `MODERNIME_SETTINGS_STATE_FILE` 存在时，在导航完成并处理完 GTK 事件循环后，直接读取真实 GTK 控件树：
       - `findWidgetByType(win, GTK_TYPE_STACK)` -> `gtk_stack_get_visible_child_name` 获取实际展示的页面名；
       - `gtk_window_get_focus(win)` 及祖先/子容器遍历 -> 获取实际拥有焦点的 `modernime-settings-target` 属性；
       - `gtk_application_get_windows` -> 获取实际关联的应用窗口总数；
       - `getpid()` -> 主实例真实进程 PID；
     - 原子写入 JSON 状态文件，绝不回显 argv，真实反映 GTK UI 渲染与聚焦状态。
  2. **测试脚本重构 (`tests/settings/settings_navigation_dbus_test.sh`)**：
     - **补齐边界**：显式参数为空或指定二进制不存在时直接以 exit 1 失败（不跨工作树寻找，不记为 exit 77）；仅无任何物理/虚拟显示环境（`DISPLAY` 与 `WAYLAND_DISPLAY` 均空）时按契约返回 77 跳过。
     - **正向全路径验证**：
       - Step 1: 首次非法页命令 4s 内以 exit 1 确定性退出，无残余窗口或进程；
       - Step 2: 启动主实例 `--page overview`，有界轮询握手（5s 上限），断言 `ready==true`、`pid==PRIMARY_PID`、`page==overview`、`window_count==1`；
       - Step 3: 次级实例发送 `--page candidates --target candidate-font-size`，断言主实例更新为 `visible_page==candidates`、`focused_target==candidate-font-size`、`window_count==1`、PID 不变；
       - Step 4: 次级实例发送别名 `--clipboard`，断言更新为 `visible_page==clipboard`、`window_count==1`；
       - Step 5: 次级实例发送 `--page invalid_dummy_page`，断言次级命令 exit 1 并输出诊断，主实例依然存活，且主实例页面仍为 `clipboard`、窗口数仍为 1。
     - **对照门禁验证**：脚本内置 5 大对照验证，证明门禁能有效拒绝所有缺陷假实现：
       - 对照 0：复审证据中的真实假脚本 `build/t18-review-evidence/fake-settings`（100% 拒绝并记录 PASS）；
       - 对照 1：非法页 exit 0 变体（100% 拒绝并记录 PASS）；
       - 对照 2：不导航变体（100% 拒绝并记录 PASS）；
       - 对照 3：丢 target 变体（100% 拒绝并记录 PASS）；
       - 对照 4：另开窗口（`window_count==2`）变体（100% 拒绝并记录 PASS）。
     - **安全隔离与清理**：在隔离 `dbus-run-session` 中执行，使用独立私有沙箱 `SANDBOX_DIR`，退出时安全排空并卸载 FUSE 挂载点，仅清理本次测试所属资源。

### 2.3 R3 / P2：文档承诺与命令口径校准

- **中文别名校准**：
  - `page_registry.cpp` 严格实现对 8 个规范中文标题（`概览`、`输入与按键`、`候选与外观`、`个人词典`、`剪贴板`、`智能学习`、`更新与安装`、`系统与诊断`）的支持，并经由 `page_registry_test.cpp` 100% 断言覆盖。
  - `docs/T18_ACCEPTANCE.md` 与 `README.md` 如实标明支持的 8 个规范中文标题与英文别名，清除此前未经实现的过度承诺列表。
- **命令行选项与 Target 校准**：
  - `README.md` 与 `docs/HANDOVER.md` 中：
    - `--candidates` 对应短选项为 `-k`（非 `-a`）；
    - `--diagnostics` 对应选项为 `--status`（非 `-s`）；
    - 个人词典导入子定位锚点校准为真实注册名称 `--target dictionary-import`（非 `--target import`）。
- **门禁工具命令校准**：
  - `docs/HANDOVER.md` 中修正两处错误的 `verify_required_tests.py --ctest-dir ...`，替换为标准的 `--registered build/fcitx5-debug/registered_tests.json`。
  - 明确区分“仅测试注册检查”与“基于 `--junit` 的实际执行证据检查”。
- **构建环境性质说明**：
  - 明确标注 `build/parallel-t18-debug` 为 Debug 构建（`-DCMAKE_BUILD_TYPE=Debug`），用于开发调试与自动化测试；明确说明该目录下的测试不能等同于“完整的 Release 性能基准”（生产 Release 性能基准须由独立 Release 构建生成）。

---

## 3. 验证执行结果与证据记录

所有重构建与测试均在全局互斥锁 `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock` 保护下执行：

### 3.1 定向测试执行

```bash
ctest --test-dir build/parallel-t18-debug -R "modernime_settings_shell_gtk|modernime_page_registry|modernime_settings_navigation_dbus" --output-on-failure
```
- `modernime_settings_shell_gtk`: **Passed (2.95 sec)**
- `modernime_page_registry`: **Passed (0.00 sec)**
- `modernime_settings_navigation_dbus`: **Passed (19.21 sec)**（包含正向真实 GTK 观察与全部 5 组对照变体验证）
- **测试通过率**：3/3 (100% Pass)

### 3.2 独立边界与对比验证

1. **首次非法页非零退出**：
   - 命令：`timeout 3s ./build/parallel-t18-debug/tools/settings/modernime-settings --page invalid_dummy_page`
   - 结果：立即返回，`EXIT_CODE=1`，输出 `Unknown settings page: invalid_dummy_page`，无超时。
2. **缺必需参数边界**：
   - 命令：`./tests/settings/settings_navigation_dbus_test.sh /nonexistent/binary`
   - 结果：`EXIT_CODE=1`，输出 `[ERROR] Required binary ... does not exist or is not executable`。
3. **无图形显示环境跳过**：
   - 命令：`env -u DISPLAY -u WAYLAND_DISPLAY ./tests/settings/settings_navigation_dbus_test.sh build/.../modernime-settings`
   - 结果：`EXIT_CODE=77`，输出 `[SKIP] No graphical display available for GTK DBus navigation test (exit 77)`。
4. **拒绝 fake-settings**：
   - 运行复审证据中的 `/home/wsl/ModernIME/build/t18-review-evidence/fake-settings`，门禁成功拒绝。

### 3.3 门禁准入工具核验 (`tests/verify_required_tests.py`)

```bash
ctest --test-dir build/parallel-t18-debug --show-only=json-v1 > build/parallel-t18-debug/registered_tests.json
python3 tests/verify_required_tests.py --registered build/parallel-t18-debug/registered_tests.json --profile full --stage final
```
- **核验结果**：`[OK] Required tests check passed (profile=full, stage=final, execution=headless).`
- 36 项必需测试全部合规注册。

---

## 4. 受控变更清单 (Changed Files)

本轮整改精确修改的文件如下：
- `tools/settings/page_registry.cpp`（支持 8 组规范中文页面标题）
- `tools/settings/settings_app.cpp`（请求先行校验、首实例与次级实例非法页保护、GTK 真实控件观察导出）
- `tests/settings/page_registry_test.cpp`（补充 8 组规范中文页面标题单测断言）
- `tests/settings/settings_navigation_dbus_test.sh`（真实 UI/Target 观察握手、边界补齐、5 组对照变体验证）
- `README.md`（校准 `-k`、`--status`、`--target dictionary-import`）
- `docs/HANDOVER.md`（校准 `--registered` 命令及 Debug 构建说明）
- `docs/T18_ACCEPTANCE.md`（本验收报告更新）

---

## 5. 移交与约束承诺

1. **唯一实现者承诺**：本工作树未启动第二实现者，未修改算法代码，未破坏既有 API 契约。
2. **清洁边界承诺**：未执行 `git reset`、`git clean` 或 `git stash`，未污染未跟踪文件，保留旧复审证据。
3. **停在 T18 等待复审**：本分支（`codex/architecture-t18-20261017`）不直接推送到远程，不合并到 main，不自行宣布 T19 完成，停在当前状态交由协调者复审。
