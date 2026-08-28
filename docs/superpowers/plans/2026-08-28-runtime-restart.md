# 设置客户端真实重启实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将设置客户端的 ModernIME 重载改为真正替换 Fcitx5 进程，确保安装后的新插件被加载并在确认激活后才报告成功。

**Architecture:** `RuntimeController` 继续负责外部进程通信，但将运行中分支从 `fcitx5-remote -r` 改为带 `--replace` 的 Fcitx5 启动流程。测试替身模拟替换、服务恢复和各类失败阶段；GTK 页面沿用现有异步任务，只更新阶段提示和最终状态。

**Tech Stack:** C++20、GLib/GIO `GSubprocess`、GTK3、Fcitx5、CTest。

**Spec:** `docs/superpowers/specs/2026-08-28-runtime-restart-design.md`

## Global Constraints

- 不修改候选栏尺寸、位置、字体、圆角和间距。
- 不使用管理员权限启动 Fcitx5；运行时使用当前用户环境和用户安装前缀。
- 外部命令使用参数数组，不拼接 shell 字符串。
- 每个功能变更先写失败测试，再实现；完整构建、CTest、安装验证后使用中文 Git 提交。
- 不清理或删除 `/usr/local` 历史文件；通过明确的 `FCITX_ADDON_DIRS` 优先级选择当前安装。

### Task 1: 锁定真实进程替换契约

**Files:**
- Modify: `tests/settings/fake_fcitx5.cpp`
- Modify: `tests/settings/fake_fcitx5_remote.cpp`
- Modify: `tests/settings/runtime_controller_test.cpp`

**Interfaces:**
- Consumes: 现有 `RuntimeController::reload()` 和测试环境变量。
- Produces: 可验证 `fcitx5 --replace` 参数、调用顺序、环境变量和失败阶段的测试替身行为。

- [ ] **Step 1: 写失败测试。**

将“运行中重载”的期望从远程 `remote:-r` 改为 Fcitx5 替换命令 `args -d --replace -u modernime-ui`，并增加断言：运行中不出现 `remote:-r`，替换命令收到当前 `FCITX_ADDON_DIRS`。增加替换失败和服务恢复超时的测试环境变量及断言。

- [ ] **Step 2: 构建并运行聚焦测试确认失败。**

运行：

```bash
cmake --build build/fcitx5-debug --target modernime_runtime_controller_tests -j2
ctest --test-dir build/fcitx5-debug -R '^modernime_runtime_controller$' --output-on-failure
```

预期：测试因当前实现仍记录 `remote:-r` 或缺少 `--replace` 而失败，不接受编译错误作为红灯结果。

- [ ] **Step 3: 扩展测试替身。**

让假的 Fcitx5 在收到 `--replace` 时记录调用并创建新的服务状态；让假的 remote 能按环境变量模拟状态失败、服务未恢复和 ModernIME 未激活，但保留已有启动错误覆盖。替身只为测试进程状态，不改变生产接口。

- [ ] **Step 4: 再次运行聚焦测试确认契约仍然为红。**

运行同一条 CTest 命令，确认失败原因来自生产代码仍调用错误的重载路径。

### Task 2: 实现真正的 Fcitx5 替换流程

**Files:**
- Modify: `tools/settings/runtime_controller.cpp`
- Modify: `tools/settings/include/modernime/settings/runtime_controller.h`（仅在需要表达阶段结果时修改）

**Interfaces:**
- Consumes: Task 1 的替身参数和既有 `RuntimeController::probe()`。
- Produces: `RuntimeController::reload()` 在运行中通过 `fcitx5 -d --replace -u modernime-ui`，在未运行时通过 `fcitx5 -d -u modernime-ui`，并在最终状态确认前不返回成功。

- [ ] **Step 1: 实现最小替换分支。**

保留现有路径解析和环境传递；运行中调用 `startCommand(fcitxExecutable, {"-d", "--replace", "-u", "modernime-ui"}, environment)`，未运行分支保持 `{"-d", "-u", "modernime-ui"}`。删除运行中对 `fcitx5-remote -r` 的调用。

- [ ] **Step 2: 用聚焦测试验证绿色。**

运行：

```bash
cmake --build build/fcitx5-debug --target modernime_runtime_controller_tests -j2
ctest --test-dir build/fcitx5-debug -R '^modernime_runtime_controller$' --output-on-failure
```

预期：运行中替换、未运行启动、启动失败和错误捕获用例全部通过。

- [ ] **Step 3: 补齐有界等待和失败信息。**

将替换/启动后的轮询明确区分为服务可用、ModernIME 选中、输入法激活三个条件；沿用现有上限，确保超时恢复按钮并返回包含阶段名称的错误。不得因 `fcitx5-remote -n` 能读到旧服务就直接报告成功。

- [ ] **Step 4: 运行聚焦测试确认失败路径。**

运行运行时控制器测试，确认替换失败、服务未恢复和 ModernIME 未激活都能失败并携带错误信息。

### Task 3: 完善状态页提示并验证用户入口

**Files:**
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tests/settings/settings_ui_contract_test.cpp`（如状态文案契约需要更新）

**Interfaces:**
- Consumes: Task 2 的 `RuntimeResult.success` 和中文错误消息。
- Produces: 重载期间、成功、失败状态清晰可见，失败后按钮可再次点击。

- [ ] **Step 1: 更新重载阶段文案。**

将重载中的状态提示改为“正在替换 Fcitx5 并加载 ModernIME…”，成功提示改为“ModernIME 已重启并激活”；保留异步线程、按钮禁用和失败后重新启用行为。

- [ ] **Step 2: 运行设置客户端相关测试。**

运行：

```bash
cmake --build build/fcitx5-debug --target modernime_runtime_controller_tests modernime_settings_ui_contract_tests modernime-settings -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_runtime_controller|modernime_settings_ui_contract' --output-on-failure
```

预期：运行时和界面契约测试全部通过。

### Task 4: 全量验证、部署和中文提交

**Files:**
- No source files beyond Tasks 1–3.

- [ ] **Step 1: 执行全量构建和 CTest。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
```

预期：构建成功，全部测试通过。

- [ ] **Step 2: 安装到用户前缀。**

```bash
cmake --install build/fcitx5-debug
```

检查 `/home/wsl/.local/lib/fcitx5/modernime_fcitx5.so`、`modernime_ui.so`、`modernime-settings` 和知识库文件存在且更新时间对应本次构建。

- [ ] **Step 3: 验证真实桌面会话。**

通过设置客户端触发重载后检查 `fcitx5-remote -n` 返回 `modernime`，检查当前 Fcitx5 进程映射到 `/home/wsl/.local/lib/fcitx5/modernime_fcitx5.so` 且不带 `(deleted)`；若失败，记录实际阶段和日志，不宣称完成。

- [ ] **Step 4: 提交本次功能。**

```bash
git diff --check
git add tools/settings/include/modernime/settings/runtime_controller.h \
        tools/settings/runtime_controller.cpp \
        tools/settings/settings_window.cpp \
        tests/settings/fake_fcitx5.cpp \
        tests/settings/fake_fcitx5_remote.cpp \
        tests/settings/runtime_controller_test.cpp \
        tests/settings/settings_ui_contract_test.cpp
git commit -m "根治设置客户端重启旧插件问题"
```

- [ ] **Step 5: 提交后再次构建、测试和部署。**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --install build/fcitx5-debug
git status --short --branch
```

预期：提交后工作区干净，构建、测试、安装均成功。
