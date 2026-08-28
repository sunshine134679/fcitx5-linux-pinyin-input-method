# ModernIME 设置客户端优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在保持候选栏设计不变的前提下，完善 ModernIME GTK3 设置客户端的设置生命周期、运行时重载、数据管理和界面显示。

**Architecture:** 保留现有 GTK3 应用和共享数据格式，把设置编辑状态、统一校验、运行时重载和数据操作分别放到明确的控制层；设置窗口通过少量可复用的页面/卡片构造辅助函数统一视觉和交互。每个任务都能独立构建、测试和安装，并产生一个中文 Git 提交。

**Tech Stack:** C++20、GTK3、GLib/GIO、CMake、CTest、SQLite（现有 `LearningStore`）、现有 `SettingsStore`/`UserDictionary`/`ClipboardHistory`。

**Spec:** `docs/superpowers/specs/2026-08-28-settings-client-optimization-design.md`

## 当前执行状态

- Task 1–5 已完成，并分别使用中文 Git 提交；候选栏代码和几何参数没有纳入这些改动。
- Task 6 正在进行：补充状态页明细、安装说明和最终全量验收。
- 当前验证基线为完整构建、31 项 CTest、用户前缀安装和 `modernime-settings --version`。

## Global Constraints

- 保留 GTK3，不迁移 GTK4/libadwaita。
- 候选栏现有尺寸、位置、字体、圆角和间距保持不变，设置客户端不得写入候选栏几何参数。
- 所有用户数据继续写入 `SettingsPaths` 指定的用户目录，不使用 `sudo`。
- 外部命令通过参数数组执行，不拼接用户输入到 shell 命令。
- 保存失败不得覆盖旧文件或丢失界面编辑内容。
- 每个独立功能完成后都必须单独使用中文 Git 提交信息。
- 每个任务的提交前都必须执行构建、CTest、安装和 `modernime-settings --version` 验证。

---

### Task 1: 统一设置校验与模型诊断

**Files:**
- Modify: `core/include/modernime/core/settings.h`
- Modify: `core/src/settings.cpp`
- Test: `tests/core/settings_test.cpp`
- Modify: `tools/settings/include/modernime/settings/settings_model.h`
- Modify: `tools/settings/settings_model.cpp`
- Test: `tests/settings/settings_window_model_test.cpp`

**Interfaces:**
- Produces `modernime::core::SettingsValidationResult validateSettings(const ModernIMESettings &settings)`，返回 `bool valid` 和按字段排列的中文/英文诊断字符串。
- Produces `SettingsWindowModel::loadDiagnostics()`，返回构造时读取配置产生的诊断列表。
- `SettingsWindowModel::save()` 在写文件前调用统一校验；失败时保留 `edited_` 和 `dirty()` 状态。
- 设置窗口后续通过模型的校验结果和加载诊断更新敏感状态及底部状态栏。

- [ ] **Step 1: 添加会失败的校验测试。**

在 `tests/core/settings_test.cpp` 增加以下断言，确保目前保存层缺失的 `input.toggle_key` 校验被固定下来：

```cpp
auto settings = modernime::core::defaultSettings();
settings.toggleKey = "Ctrl Space";
const auto result = modernime::core::validateSettings(settings);
assertTrue(!result.valid, "invalid toggle key is rejected");
assertTrue(!result.errors.empty(), "toggle key error is explained");
```

在 `tests/settings/settings_window_model_test.cpp` 增加损坏配置读取测试：写入 `input.toggle_key=Ctrl Space` 后构造模型，断言模型仍使用默认快捷键且 `loadDiagnostics()` 非空。

- [ ] **Step 2: 运行聚焦测试并确认失败。**

运行：

```bash
cmake --build build/fcitx5-debug --target modernime_settings_tests modernime_settings_model_tests -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_settings|modernime_settings_model' --output-on-failure
```

预期：编译或断言失败，原因是统一校验接口和加载诊断接口尚未存在。

- [ ] **Step 3: 实现共享校验和模型诊断。**

在 `core/include/modernime/core/settings.h` 增加：

```cpp
struct SettingsValidationResult final {
    bool valid = true;
    std::vector<std::string> errors;
};

SettingsValidationResult validateSettings(const ModernIMESettings &settings);
```

把当前 `settings.cpp` 中的快捷键校验整理为共享函数；`SettingsStore::save()` 同时校验 `toggleKey` 和 `clipboardTrigger`。`SettingsWindowModel` 保存 `SettingsLoadResult::diagnostics`，并暴露只读访问器。

- [ ] **Step 4: 让设置模型暴露可消费的校验结果。**

在 `SettingsWindowModel` 增加只读方法：

```cpp
core::SettingsValidationResult validation() const;
```

该方法直接校验当前 `edited_`，供 GTK 界面在后续任务中即时决定按钮状态；保存失败仍保留编辑值和 `dirty()` 状态。

- [ ] **Step 5: 运行测试和本地安装验证。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
modernime-settings --version
```

- [ ] **Step 6: 提交本任务。**

```bash
git add core/include/modernime/core/settings.h core/src/settings.cpp \
        tests/core/settings_test.cpp \
        tools/settings/include/modernime/settings/settings_model.h \
        tools/settings/settings_model.cpp \
        tests/settings/settings_window_model_test.cpp
git commit -m "完善设置保存校验"
```

---

### Task 2: 修复 Fcitx5 状态检测与重载闭环

**Files:**
- Modify: `tools/settings/include/modernime/settings/runtime_controller.h`
- Modify: `tools/settings/runtime_controller.cpp`
- Modify: `tests/settings/runtime_controller_test.cpp`
- Modify: `tests/settings/fake_fcitx5_remote.cpp`
- Modify: `tests/settings/fake_fcitx5.cpp`
- Modify: `tools/settings/settings_window.cpp`

**Interfaces:**
- `RuntimeController::probe()` 保持现有返回结构，并区分远程命令不可用、Fcitx5 未运行、Fcitx5 已运行但 ModernIME 未激活、ModernIME 已激活。
- `RuntimeController::reload()` 先执行 `fcitx5-remote -r`；只有探测到服务未运行时才执行 `fcitx5 -d -u modernime-ui`。
- 运行失败结果必须携带捕获到的标准错误或明确阶段名称。

- [ ] **Step 1: 扩展假的 Fcitx5 命令和失败测试。**

在 `runtime_controller_test.cpp` 增加断言：服务已运行时日志包含 `remote:-r` 且不包含重复启动命令；服务未运行时才包含 `args -d -u modernime-ui`；启动命令输出错误时结果包含错误文本。

示例断言：

```cpp
assertTrue(remoteLogContents.str().find("remote:-r") != std::string::npos,
           "running Fcitx5 is reloaded through fcitx5-remote");
assertTrue(fcitxLogContents.str().find("args -d -u modernime-ui") ==
               std::string::npos,
           "running Fcitx5 is not started a second time");
```

- [ ] **Step 2: 运行运行时聚焦测试并确认失败。**

```bash
cmake --build build/fcitx5-debug --target modernime_runtime_controller_tests -j2
ctest --test-dir build/fcitx5-debug -R modernime_runtime_controller --output-on-failure
```

- [ ] **Step 3: 实现分支重载和错误捕获。**

调整 `RuntimeController::reload()`：先调用 `probe()`；如果 `running` 为真，执行参数数组 `{"-r"}`；如果不在运行，执行 `{"-d", "-u", "modernime-ui"}`，然后轮询 `fcitx5-remote -s modernime`、`-o` 和 `probe()`。启动子进程不再静默标准错误，超时和非零返回码转换为包含阶段名称的结果。

- [ ] **Step 4: 更新状态页的异步状态显示。**

在窗口中增加“检测中/重载中/已运行/未运行/失败”的样式类和重试入口。保存成功后，如果配置影响运行中的 ModernIME，状态区显示“设置已保存，需要重新加载”，并能直接触发重载；重载失败不清除未保存编辑。

- [ ] **Step 5: 运行完整验证。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
modernime-settings --version
```

- [ ] **Step 6: 提交本任务。**

```bash
git add tools/settings/include/modernime/settings/runtime_controller.h \
        tools/settings/runtime_controller.cpp tests/settings/runtime_controller_test.cpp \
        tests/settings/fake_fcitx5_remote.cpp tests/settings/fake_fcitx5.cpp \
        tools/settings/settings_window.cpp
git commit -m "完善设置客户端运行时重载"
```

---

### Task 3: 建立统一 GTK3 页面视觉和交互系统

**Files:**
- Modify: `tools/settings/settings_window.cpp`
- Create: `tools/settings/include/modernime/settings/settings_ui_contract.h`
- Test: `tests/settings/settings_ui_contract_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- 页面构造复用以下 GTK3 辅助函数：

```cpp
GtkWidget *makePageShell(const char *title, const char *subtitle);
GtkWidget *makeSectionCard(const char *title, const char *description);
void setWidgetError(GtkWidget *widget, bool invalid, const char *message);
```

- 设置窗口统一维护页面标题、说明、状态区和底部操作栏。
- UI 合约测试在可用显示服务器下验证所有页面标题和主要控件存在；无显示服务器时保留模型/数据测试，不伪造视觉通过结果。
- 界面实现使用 `SettingsWindowModel::dirty()`、`validation()` 和 `loadDiagnostics()` 驱动按钮状态、未保存提示和启动警告。

- [ ] **Step 1: 写 UI 合约测试。**

新增 `tests/settings/settings_ui_contract_test.cpp`，先用无显示服务器也能运行的页面/操作契约固定窗口必须使用的六个页面名、页面标题和四个主要操作标签；设置窗口构造时直接消费同一份契约，避免导航和操作按钮文案漂移。

- [ ] **Step 2: 运行合约测试并确认当前实现不满足。**

```bash
cmake --build build/fcitx5-debug --target modernime_settings_ui_contract_tests -j2
ctest --test-dir build/fcitx5-debug -R modernime_settings_ui_contract --output-on-failure
```

预期：当前界面仍使用旧的动作按钮和重复的页面布局，合约断言失败或目标尚不存在。

- [ ] **Step 3: 实现页面壳、统一样式和编辑状态。**

在 `settings_window.cpp` 中用 GTK3 样式类创建统一页面壳：窗口默认尺寸调整为适合设置内容的大小，左侧导航保持固定宽度，右侧 stack 包裹在可滚动容器中；页面标题下增加用途说明；底部动作区固定显示修改状态和状态提示。将按钮区实现为 `应用`、`保存并关闭`、`恢复修改` 和 `恢复默认`：应用保存后留在窗口，保存并关闭成功后隐藏窗口，恢复修改只回退编辑值；有未保存修改时关闭窗口必须确认。仅修改设置客户端窗口，不修改任何输入法候选栏代码。

- [ ] **Step 4: 完善控件状态和键盘可用性。**

把剪贴板触发键、候选开关和词典操作按钮接入统一的敏感状态更新；主开关关闭时禁用从属控件；为输入框、列表和按钮设置 tooltip、可读标签和默认焦点；为 GTK3 控件增加最小宽度和合理的滚动策略，防止窗口缩小时文字和按钮被裁剪。

- [ ] **Step 5: 运行 UI 合约、完整测试和安装验证。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
modernime-settings --version
```

如果当前环境没有 X11/Wayland 测试显示服务器，记录为环境限制，不降低无显示环境下的编译、模型和数据测试要求。

- [ ] **Step 6: 提交本任务。**

```bash
git add tools/settings/settings_window.cpp tools/settings/settings_app.cpp \
        config/desktop/modernime-settings.desktop \
        tests/settings/settings_ui_contract_test.cpp tests/CMakeLists.txt
git commit -m "优化设置客户端界面与交互"
```

---

### Task 4: 完善剪贴板历史和智能学习管理

**Files:**
- Modify: `core/include/modernime/core/clipboard_history.h`
- Modify: `core/src/clipboard_history.cpp`
- Modify: `tools/settings/include/modernime/settings/clipboard_history_model.h`
- Modify: `tools/settings/clipboard_history_model.cpp`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tests/core/clipboard_history_test.cpp`
- Modify: `tests/settings/clipboard_history_model_test.cpp`
- Modify: `tools/settings/data_controller.cpp`
- Modify: `tools/settings/include/modernime/settings/data_controller.h`
- Modify: `tests/settings/data_controller_test.cpp`

**Interfaces:**
- `ClipboardHistory` 增加按索引删除能力：

```cpp
bool remove(std::size_t index);
```

- `ClipboardHistoryModel` 增加：

```cpp
bool remove(std::size_t index, std::string *error = nullptr);
bool clear(std::string *error = nullptr);
```

- `DataController` 增加学习数据只读统计接口，清空仍通过备份验证路径：

```cpp
std::size_t learningEntryCount(const std::filesystem::path &path,
                               std::string *error = nullptr);
```

- 剪贴板列表按最新到最旧显示，任何删除/清空都原子保存；复制到系统剪贴板只在 GTK 窗口回调中调用 `gtk_clipboard_set_text()`，模型层不依赖 GTK。

- [ ] **Step 1: 添加剪贴板删除/清空和学习统计测试。**

在核心历史测试中验证按索引删除、清空和保存后重新加载；在设置数据测试中写入学习记录并断言 `learningEntryCount()` 返回正确数量，数据库不存在时返回空统计和可读状态。

- [ ] **Step 2: 运行聚焦测试并确认失败。**

```bash
cmake --build build/fcitx5-debug --target modernime_clipboard_history_tests modernime_clipboard_history_model_tests modernime_data_controller_tests -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_(clipboard_history|clipboard_history_model|data_controller)' --output-on-failure
```

- [ ] **Step 3: 实现安全的数据操作。**

在 `ClipboardHistory` 中拒绝越界删除；模型删除或清空时先加载当前文件、修改内存副本、调用现有原子 `save()`，保存失败时保持模型和磁盘旧内容。窗口回调通过 `GtkClipboard` 只复制用户明确选中的条目。学习统计复用 `LearningStore::snapshot()`，不直接拼接 SQL。

- [ ] **Step 4: 完善剪贴板和学习页面。**

剪贴板页面进入时自动加载，显示“当前 N 条，最多 30 条”、空状态和错误状态；选中条目后启用复制/删除，清空全部需要确认。学习页面显示路径、记录数量、备份路径和上次操作结果；清空按钮继续在后台执行，避免冻结窗口。

- [ ] **Step 5: 运行完整验证、安装并检查历史持久化。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
modernime-settings --version
```

- [ ] **Step 6: 提交本任务。**

```bash
git add core/include/modernime/core/clipboard_history.h core/src/clipboard_history.cpp \
        tools/settings/include/modernime/settings/clipboard_history_model.h \
        tools/settings/clipboard_history_model.cpp tools/settings/settings_window.cpp \
        tools/settings/data_controller.cpp tools/settings/include/modernime/settings/data_controller.h \
        tests/core/clipboard_history_test.cpp tests/settings/clipboard_history_model_test.cpp \
        tests/settings/data_controller_test.cpp
git commit -m "完善剪贴板与学习数据管理"
```

---

### Task 5: 完善用户词典管理和导入反馈

**Files:**
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/data_controller.cpp`
- Modify: `tools/settings/include/modernime/settings/data_controller.h`
- Modify: `tests/settings/data_controller_test.cpp`
- Modify: `tests/settings/settings_ui_contract_test.cpp`

**Interfaces:**
- `DataController::loadDictionary()` 保持现有返回兼容性，新增带诊断的读取接口：

```cpp
struct DictionaryLoadResult final {
    std::vector<pinyin::UserDictionaryEntry> entries;
    std::vector<std::string> diagnostics;
};

static DictionaryLoadResult loadDictionaryWithDiagnostics(
    const std::filesystem::path &path);
```

- 设置窗口新增词典筛选函数：

```cpp
void filterDictionaryPage(SettingsWindow::Impl *impl,
                          std::string_view query);
void updateDictionaryActionState(SettingsWindow::Impl *impl);
```

- 导入流程保留原文件直到所有输入行通过 `UserDictionary` 校验。

- [ ] **Step 1: 添加词典诊断和筛选测试。**

在数据控制器测试中写入一条有效行和多条非法行，断言有效词条被加载、诊断数量正确；增加空查询返回全部条目、精确拼音或词条查询只显示匹配条目的测试。

- [ ] **Step 2: 运行聚焦测试并确认缺口。**

```bash
cmake --build build/fcitx5-debug --target modernime_data_controller_tests -j2
ctest --test-dir build/fcitx5-debug -R modernime_data_controller --output-on-failure
```

- [ ] **Step 3: 实现诊断和界面筛选。**

让词典加载保留解析诊断；窗口显示有效条数、错误条数和空列表引导。增加搜索框，文本变化时只刷新可见行；未选中行时编辑/删除按钮禁用，选中行后恢复；添加/编辑对拼音、词条和权重即时提示错误。

- [ ] **Step 4: 完善导入、导出和错误显示。**

导入先读取到临时向量，显示“有效 N 条，忽略 M 条”确认信息；用户确认后才调用原子保存。导出失败显示文件路径和系统错误；保存后提示需要重载 ModernIME，不自动清除搜索条件或列表选择。

- [ ] **Step 5: 运行完整验证、安装并提交。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
modernime-settings --version
git add tools/settings/settings_window.cpp tools/settings/data_controller.cpp \
        tools/settings/include/modernime/settings/data_controller.h \
        tests/settings/data_controller_test.cpp tests/settings/settings_ui_contract_test.cpp
git commit -m "完善用户词典管理"
```

---

### Task 6: 汇总验收和部署文档

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-08-28-settings-client-optimization-design.md`
- Modify: `docs/superpowers/plans/2026-08-28-settings-client-optimization.md`
- Test: `tests/settings/settings_install_test.sh`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 添加安装和干净环境验收。**

在临时 `HOME`、`XDG_CONFIG_HOME` 和 `XDG_DATA_HOME` 下安装客户端，验证所有页面所需目录按需创建、桌面入口可用、无 `sudo` 命令、无关桌面文件不被删除。

- [ ] **Step 2: 运行完整验收。**

```bash
cmake -S . -B build/fcitx5-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/home/wsl/.local
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
modernime-settings --version
git status --short --branch
```

若桌面环境可用，再启动 `modernime-settings` 逐页检查；当前终端缺少可用截图/显示服务器时，记录限制并要求用户在 Ubuntu 桌面确认界面显示，不把不可见的 GUI 当作已验证。

- [ ] **Step 3: 更新 README 的用户操作说明。**

记录启动命令、桌面入口、设置保存/应用/重载的区别、剪贴板历史路径、学习数据备份位置和不需要 `sudo` 的安装方式。

- [ ] **Step 4: 提交汇总验收文档。**

```bash
git add README.md docs/superpowers/specs/2026-08-28-settings-client-optimization-design.md \
        docs/superpowers/plans/2026-08-28-settings-client-optimization.md \
        tests/settings/settings_install_test.sh tests/CMakeLists.txt
git commit -m "补齐设置客户端验收与部署说明"
```
