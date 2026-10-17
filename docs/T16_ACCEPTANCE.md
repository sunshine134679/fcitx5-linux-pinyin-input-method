# T16 验收报告：构建目标与测试接线

日期：2026-10-17  
工作树：`/home/wsl/ModernIME/.worktrees/parallel-t16-20261017`  
分支：`codex/architecture-t16-20261017`  
基线提交 (Provisional)：`d65ef8a`（包含 C14 `a5dbf74`、C17 `612c32c` 及 C15 `2a0460a`）  
阶段状态：`WIRING_COMPLETE`（前置提交为 provisional，最终通过判定留待集成验收）  
接口版本：`C16-BUILD-TEST-WIRING`  

---

## 1. 任务概述与职责界定

本任务（ModernIME T16「构建目标与测试接线」）获得了协调者明确移交的全仓 CMake/CI/必需测试接线的单一写权限。根据《2026-10-17 并行整改与集成规约》、架构文档及 T16 任务指引，本任务严格限定在接线、配置与规约校验范畴，不替 T14/T15/T17/T18 实现业务功能。

核心职责已全部交付：
1. **测试与脚本接线**：完整覆盖 T14、T15、T17 留下的全部测试与脚本接线要求；
2. **设置组件目标收敛**：构建统一的 `modernime_settings_components` INTERFACE 目标，彻底解耦设置二进制入口与测试套件，建立严格的编译宏隔离；
3. **测试端到端注册与 Profile 扩展**：在 `tests/verify_required_tests.py` 中新增 `core-only` 显式 profile 校验，并在严格模式全量 CI 中增加完整门禁；保持 Release 编译下断言强制开启 (`-UNDEBUG`)；
4. **按需挂载与选项解耦**：新增 `MODERNIME_BUILD_UI_LAYOUT_TESTS` 选项（默认 `ON`），按需挂载 UI 目录，在全量严格模式下强制校验开启；
5. **机器级排他锁执行**：所有构建与测试过程均在 `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock` 保护下执行。

---

## 2. 目标定义与组件收敛 (C16)

### 2.1 modernime_settings_components INTERFACE 目标
在 `tools/settings/CMakeLists.txt` 中创建了 `modernime_settings_components` INTERFACE 库目标，统一收敛全部 22 个组件源文件：
- `data_controller.cpp`
- `dictionary_pronunciation.cpp`
- `settings_model.cpp`
- `runtime_controller.cpp`
- `package_operation_state.cpp`
- `process_runner.cpp`
- `package_policy.cpp`
- `update_controller.cpp`
- `clipboard_history_model.cpp`
- `overview_model.cpp`
- `page_registry.cpp`
- `settings_widgets.cpp`
- `settings_shell.cpp`
- `settings_shell_state.cpp`
- `pages/candidates_page.cpp`
- `pages/clipboard_page.cpp`
- `pages/diagnostics_page.cpp`
- `pages/dictionary_page.cpp`
- `pages/input_page.cpp`
- `pages/learning_page.cpp`
- `pages/overview_page.cpp`
- `pages/updates_page.cpp`
- `settings_window.cpp`（包含）

明确排除二进制入口专属文件 `settings_app.cpp`。

### 2.2 宏隔离与独立编译规范
- **可执行程序 `modernime-settings`**：
  单独编译 `settings_app.cpp` 并私有链接 `modernime_settings_components`，注入生产宏：
  ```cmake
  target_compile_definitions(modernime-settings PRIVATE
      MODERNIME_VERSION="${PROJECT_VERSION}"
      MODERNIME_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
      MODERNIME_SYSTEM_FCITX5_ADDON_DIR="${modernime_system_fcitx5_addon_dir}"
  )
  ```
- **GTK 测试套件**：
  `modernime_settings_shell_gtk_tests`、`modernime_settings_save_gtk_tests` 与 `modernime_settings_data_async_gtk_tests` 均独立编译自身测试源文件并私有链接 `modernime_settings_components`，分别持有各自的隔离宏：
  ```cmake
  target_compile_definitions(modernime_settings_data_async_gtk_tests PRIVATE
      MODERNIME_VERSION="${PROJECT_VERSION}"
      MODERNIME_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
      MODERNIME_SYSTEM_FCITX5_ADDON_DIR=""
  )
  ```
  彻底避免了二进制专属入口宏定义泄漏或头文件重复注入。

---

## 3. 测试接线落地清单

| 任务来源 | 测试可执行目标 / 脚本 | CTest 注册测试名 | 附加属性 / 参数配置 | 验证状态 |
|---|---|---|---|---|
| **T14** | `tests/configuration_contract_test.sh` | `modernime_configuration_contract` | 追加 `--build-dir "${CMAKE_BINARY_DIR}"`；受 `if(MODERNIME_BUILD_UI_LAYOUT_TESTS)` 保护 | 已接线 |
| **T15** | `tests/settings/settings_data_async_gtk_test.cpp` | `modernime_settings_data_async_gtk` | `RESOURCE_LOCK modernime_gtk_display`<br>`SKIP_RETURN_CODE 77`<br>`TIMEOUT 20` | 已接线 |
| **T17** | `tests/install_rollback_concurrent_writes_test.sh` | `modernime_install_rollback_concurrent_writes` | `RESOURCE_LOCK modernime_install_transaction` | 已接线并通过测试 |
| **T17** | `tests/install_lifecycle_prep_test.sh` | `modernime_install_lifecycle_prep` | `RESOURCE_LOCK modernime_install_transaction` | 已接线并通过测试 |
| **T17** | `tests/install_transaction_test.sh` | `modernime_install_transaction` | `RESOURCE_LOCK modernime_install_transaction` | 已接线并通过测试 |
| **T17** | `scripts/install_lifecycle.sh` | (Install Rule) | 安装至 `share/modernime` | 已配置 |

---

## 4. 构建选项与按需依赖

1. **`MODERNIME_BUILD_UI_LAYOUT_TESTS`**：
   - 默认开启 (`ON`)；
   - 在 `CMakeLists.txt` 中将 UI 布局相关测试（`modernime_ui_tests`、`modernime_window_anchor_tests`、`modernime_status_indicator_tests`、`modernime_candidate_bar_renderer_tests`、`modernime_cairo_surface_tests`）与 `modernime_configuration_contract` 统一纳入门禁保护；
   - 根级 `CMakeLists.txt` 中，仅在真正存在消费者（`MODERNIME_BUILD_FCITX5 OR MODERNIME_BUILD_SETTINGS OR MODERNIME_BUILD_TOOLS OR (MODERNIME_BUILD_TESTS AND MODERNIME_BUILD_UI_LAYOUT_TESTS)`）时才挂载 `ui/fcitx5`，在纯核心（core-only）构建下彻底避免引入 GTK/Pango 依赖；
   - 在严格全量模式（`MODERNIME_REQUIRE_FULL_TESTS=ON`）下，强制校验 `MODERNIME_BUILD_UI_LAYOUT_TESTS=ON`。

2. **严格模式负面测试验证**：
   - 当 `MODERNIME_REQUIRE_FULL_TESTS=ON` 且 `MODERNIME_BUILD_UI_LAYOUT_TESTS=OFF` 时：配置直接报 `FATAL_ERROR: MODERNIME_REQUIRE_FULL_TESTS=ON requires MODERNIME_BUILD_UI_LAYOUT_TESTS=ON`；
   - 当缺少 `SETTINGS` 或 `FCITX5` 时：同样准确触发 `FATAL_ERROR` 阻断。

---

## 5. Required Tests 验证矩阵与 Profile 支持

### 5.1 Profile 矩阵
在 `tests/verify_required_tests.py` 中规范了三种 Profile：
1. **`full` (严格全量)**：
   - Baseline (29 项)：覆盖 personal_engine_gtest、benchmark、learning_store、pinyin、fcitx5、gtk 设置及安装/升级测试；
   - Final (额外 5 项)：`modernime_required_tests_contract`、`modernime_cleanse_guard`、`modernime_configuration_contract`、`modernime_process_runner`、`modernime_settings_data_async_gtk`；
   - Headless Skip 白名单：严格限制为 `modernime_settings_focus_gtk`、`modernime_settings_shell_gtk`、`modernime_settings_save_gtk`、`modernime_settings_data_async_gtk`（仅在 headless 模式下允许返回 77 跳过，非白名单测试返回跳过直接判定失败）。
2. **`core-only` / `core` (核心库专享)**：
   - Baseline (12 项)：`modernime_settings`、`modernime_clipboard_history`、`modernime_persistent_clipboard_history`、`modernime_dictionary_prior`、`modernime_learning_store`、`modernime_input_state`、`modernime_english_dictionary`、`modernime_english_definition`、`modernime_pinyin_match`、`modernime_punctuation`、`modernime_engine_state`、`modernime_personal_engine_benchmark`；
   - Final (额外 1 项)：`modernime_required_tests_contract`；
   - 彻底避免纯核心构建时因未开启 UI/fcitx5 产生假阳性误报。
3. **`storage-tsan`**：
   - 维持 `modernime_personal_engine_gtest` 与 `modernime_learning_store`。

### 5.2 契约测试验证
在 `tests/required_tests_contract_test.py` 中新增 5 组针对 `core-only` 的单元测试，全量 16 项测试全部通过：
```
Ran 16 tests in 0.010s
OK
```

---

## 6. 本地执行与验证证据

### 6.1 机器锁保护
所有配置与执行命令均在排他锁保护下完成：
`flock -x /home/wsl/ModernIME/build/parallel-control-20261017/machine.lock ...`

### 6.2 Core-Only 测试全量通过 (100%)
在 `build/parallel-core-only` 下执行 CTest（排除涉及真实打包安装的验证脚本）：
```
100% tests passed, 0 tests failed out of 25
Total Test time (real) = 182.13 sec
```
关键通过测试包括：
- `modernime_required_tests_contract` (Passed)
- `modernime_learning_store` (Passed)
- `modernime_personal_engine_benchmark` (Passed 129.61s)
- `modernime_install_transaction` (Passed 3.20s, RESOURCE_LOCK 生效)
- `modernime_install_rollback_concurrent_writes` (Passed 0.41s, RESOURCE_LOCK 生效)
- `modernime_install_lifecycle_prep` (Passed 0.45s, RESOURCE_LOCK 生效)

### 6.3 规约验证工具检验
- `python3 tests/verify_required_tests.py --registered build/parallel-core-only/registered_tests.json --profile core-only --stage final` -> `[OK] Passed`
- `python3 tests/verify_required_tests.py --registered build/parallel-core-only/registered_tests.json --profile full --stage baseline` -> `[FAIL] Missing required tests` (准确拒绝，负面用例生效)

---

## 7. 交付状态与交接说明

- **当前状态**：`WIRING_COMPLETE`
- **交接提示**：基线提交 `d65ef8a` 为协调者指定的 provisional 提交（已集成 C14 `a5dbf74`、C17 `612c32c` 及 C15 `2a0460a`）。T16 的全仓 CMake / CI / 测试接线已全面就绪，T14、T15、T17 全部接线要求均已闭环。前置提交为 provisional，最终通过判定留待协调者在集成验收阶段统一运行全量测试。
- 写权限在提交后完整移交回协调者。
