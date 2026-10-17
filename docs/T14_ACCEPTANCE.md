# T14 验收报告：配置应用版本与路径契约（第三轮复审整改收尾）

日期：2026-10-17  
当前分支：`main`  
基线提交：`74c19d869a12771d3feeb2300a480783c74734fa`  
状态：`LOCAL_VERIFIED / WIRING_PENDING`  

---

## 1. 任务概述与第三轮复审整改总结

本轮针对 `docs/T14_THIRD_REVIEW.md` 提出的 V1（重试耗尽后同 stamp 恢复）与 V2（原 S2 真实数据库/UI 验收与缺夹具假通过闭合）进行了定向最小收尾，严格遵守 AGENTS.md 与原架构整改计划约束，不进入 T15（设置数据异步与轻量统计），不修改 T17 已接管的安装/启动脚本，不并发修改共享 CMake：

1. **V1 / P1：连续失败耗尽重试后，同 stamp 故障解除持续有界恢复**
   - 根因：在 `adapter/fcitx5/src/inputmethod.cpp` 与 `ui/fcitx5/src/ui_addon.cpp` 中，首次 I/O 失败记录了该文件的 stamp，当 `settingsRetryCount_` 达到 5 之后，`(currentStamp != settingsStamp_)` 与 `(settingsRetryCount_ < 5)` 两侧条件均为 false，导致文件未被外部修改时永久停止重试，即使故障（如 `EACCES` 或锁争用）解除也无法收敛至真实磁盘配置。
   - 修复：
     - 实现有界且持续的退避恢复重试策略：若 stamp 变化立即重试；若处于失败待重试状态且 stamp 未变，前 5 次轮询（每 2s）积极重试，之后退避至每 2 次轮询（每 4s）重试一次，持续有界尝试，不忙循环，不泄漏无限异步任务，直到成功读取或文件变更；
     - 明确成功读取事实与待重试状态：I/O 失败期间不推进有效代次、不虚报 Hash，写入空 Hash 的 `runtime.state`；
     - 故障解除后，在相同 stamp 下自动成功读取，将 `settingsLoadFailed_` 清零、重置计数器，并收敛至磁盘配置，推进 `settingsGeneration_` 并更新运行时状态；
     - 在 `ModernIMEUserInterface` 中同步应用该有界退避恢复逻辑，并在 `handleButtonPress()` 入口调用 `refreshSettingsIfNeeded()`。
   - 验证：真实 Fcitx 探针全量场景通过（见 `build/t14-third-review-evidence/adapter-probe-results.json`），特别是 `startup_long_failure` 在连续注入 6 次失败后解除故障，无需修改文件即在第 7 次读取成功收敛至磁盘禁用配置（`after_active=0`，`generation=1`）。

2. **V2 / 验收补齐：真实数据库夹具、生产 UI Addon 集成与严谨二进制校验**
   - **A06 真实双活动数据库（SQLite + LMDB）矩阵验收**：
     - 新建 `tests/core/db_fixture_helper.cpp`，调用生产级 `modernime::core::LearningStore` 与 `PersonalEngine` 创建、写入真实提交记录（`CommitEvent`）、关闭并重新打开验证；
     - 彻底废除原假 18 字节 `history.lmdb/data.mdb` 文本文件；
     - 在 `tests/configuration_contract_test.sh` 中覆盖 4 类路径矩阵：`spaces`（含空格路径）、`home_default`（$HOME 回退路径）、`xdg_override`（$XDG 显式覆盖）、`managed_conflict`（$MODERNIME 托管环境变量优先于冲突的 $XDG 环境变量）；
     - 在每种场景下均经由 `modernime-data-guard --verify` 和 `--backup` 验证生产路径识别与双库有效性，检查备份目录真实包含 `learning.sqlite3` 与 `learning.lmdb`，断言宿主数据库未受任何污染，验证 `modernime-session-init` 生成的环境变量配置；
   - **A05 生产 UI Addon 集成测试（配置刷新、布局失效与候选语义保持）**：
     - 新建 `tests/ui/ui_addon_test.cpp`，使用真实 `fcitx::Instance` 实例化 `ModernIMEUserInterface`；
     - 初始加载 14 号字配置，完成候选栏初始度量并模拟点击首选，断言命中候选 0；
     - 磁盘更新配置字号为 28 号字，在调用 `update()` 前触发点击，验证 `handleButtonPress()` 检测到 stamp 变更触发重载，旧布局代次（generation N）与新指标代次（generation N+1）失配，`hitTestCandidate()` 坚决拒绝点击（返回 -1，未误触候选）；
     - 随后调用 `update()` 重新度量新布局，在新几何边界下点击候选，验证候选 0 正常选中，且候选文本（“你好”）与词条语义严格保持；
   - **A07 / A04 严格二进制存在性检查与缺产物负向回归**：
     - 在 `tests/configuration_contract_test.sh` 中将所有必需产物（`modernime_fake_fcitx5`, `modernime_fake_fcitx5_remote`, `modernime_fcitx5_input_panel_tests`, `modernime_ui_addon_tests`, `modernime_db_fixture_helper`）设为强依赖，任何一项缺失立即以退出码 1 报错失败，杜绝静默跳过假通过；
     - 增加自动化负向回归测试 `test_missing_fixture_negative_regression()`，验证在构建目录缺少 fake 程序时脚本严格返回非零退出码；
     - 通过复审方 `build/t14-third-review-evidence/gate_checks.py` 验证。

---

## 2. C14 接口与路径契约说明

| 契约项 | 接口 / 协议细节 | 行为语义 |
|---|---|---|
| **读取状态类型** | `enum class SettingsLoadStatus { Success, Missing, IoError };` | 区分成功、缺失与读取故障；允许保留未知键/注释等诊断而标为 Success。 |
| **单快照同源结果** | `SettingsLoadResult` (`settings`, `rawLines`, `contentHash`, `stamp`, `diagnostics`, `status`) | 单句柄打开一次读取全部内容并校验 inode，Hash 直接从缓冲区提取，保证同源一致。 |
| **保存冲突与保留** | `SettingsStore::save(path, settings, error, expectedContentHash, rawLinesToPreserve)` 返回 `bool` | 期望 Hash 与当前磁盘 Hash 不符时拒绝覆盖；成功写入时保留未修改的注释与未知配置项。 |
| **启动与热载同源消费** | `ModernIMEInputMethod` 构造函数委托 `SettingsStore::load()` 结果 | 首次启动与轮询热载消费同一成功读取事实，不另开路径二次计算 Hash，杜绝版本冒充。 |
| **持续有界恢复** | 有界退避轮询（前 5 次每 2s，后续每 4s） | 耗尽重试后不永久停止，同 stamp 故障排除后自愈收敛，不忙循环。 |
| **UI 布局失效** | `hitTestCandidate(layout, metrics, x, y)` | 当 `fontSize`、`maxCandidates`、`layoutGeneration` 任一不匹配时返回 `-1`，拒绝旧布局点击。 |
| **统一路径环境** | `SettingsPaths::fromEnvironment(configRoot, dataRoot, homeRoot)` | XDG 优先、降级 HOME；启动脚本将 `MODERNIME_*` 映射为有效 `XDG_*`，与 `modernime-data-guard` 保持同一保护路径。 |

---

## 3. 契约关卡逐项实测表现 (T14-A01 ~ A07)

| 关卡代号 | 契约要求 | 验证执行方式 | 实测表现 | 结论 |
|---|---|---|---|---|
| **T14-A01** | **同 mtime 原子替换** | 原子 rename 且保持 mtime 不变时，因 inode 变化被捕获；驱动真实消费端，同源读取新配置与 Hash，更新代次。 | `tests/configuration_contract_test.sh` A01 及 `modernime_settings_tests`：mtime 一致但 inode 改变时触发重新解析，Hash 生效。 | **PASS** |
| **T14-A02** | **删除 / 重建** | 删除文件感知 missing；原路径重建新配置时捕获新 inode 并重新计算 Hash，不虚构代次。 | `tests/configuration_contract_test.sh` A02 及 `modernime_settings_tests`：感知删除与重建，正确分类缺失与恢复。 | **PASS** |
| **T14-A03** | **外部保存冲突与保留** | 磁盘被外部修改时依据 `expectedContentHash` 拒绝写入；保存成功时保留注释与未知配置项。 | `tests/configuration_contract_test.sh` A03 及 `modernime_settings_tests`：冲突拦截生效，注释与未知配置项保持完好。 | **PASS** |
| **T14-A04** | **敏感时热载安全** | 敏感模式下热重载配置，原有 preedit 组合串完好保留，不触发任何提交学习事件；强制要求 input panel 测试。 | `tests/adapter/engine_state_test.cpp` 与 `modernime_fcitx5_input_panel_tests`：热载后 preedit="nihao" 保持，commitEvents 为空。 | **PASS** |
| **T14-A05** | **字号/候选数/代次失效** | 运行纯布局函数测试与真实 UI addon 集成测试；字号改变旧布局拒绝点击，新度量恢复后点击生效且语义保持。 | `tests/ui/ui_addon_test.cpp` 及 `modernime_ui_tests`：配置刷新后旧布局点击返回 -1，update 重新度量后点击命中且文本完好。 | **PASS** |
| **T14-A06** | **路径矩阵与真实数据库隔离** | 4 类路径矩阵（spaces、home、xdg、managed_conflict）；真实 LearningStore 写入 SQLite 与 LMDB；data-guard 校验及双库备份完备，宿主库不受影响。 | `tests/core/db_fixture_helper.cpp`、`modernime-data-guard`：真实双库校验 0 错误，备份包含 sqlite 与 lmdb，宿主库未修改。 | **PASS** |
| **T14-A07** | **runtime.state 兼容与负向校验** | 强制执行真实 `modernime_runtime_controller_tests` 检验未知/损坏状态字段；缺失 fake 产物时严格非零失败。 | `tests/configuration_contract_test.sh` A07 及负向测试：驱动真实 probe，缺产物时 exit 1。 | **PASS** |

---

## 4. 验证汇总与证据记录

### 4.1 定向测试套件实测（`build/architecture-debug`）
- **Targeted CTest Suite (6/6 Passed, 15.13s)**:
  - `modernime_settings`: Passed (0.00s)
  - `modernime_configuration_contract`: Passed (10.64s)
  - `modernime_settings_model`: Passed (0.00s)
  - `modernime_runtime_controller`: Passed (2.26s)
  - `modernime_fcitx5_input_panel`: Passed (2.21s)
  - `modernime_candidate_bar_layout`: Passed (0.00s)

### 4.2 独立探针与 Gate Checks 实测（`build/t14-third-review-evidence/`）
- **Adapter Probes 5 项全通过**：
  - `swap_after_read`: PASS
  - `read_failure`: PASS
  - `startup_swap`: PASS（无构造阶段二次 open）
  - `startup_read_failure`: PASS（单次 EACCES 恢复）
  - `startup_long_failure`: **PASS**（注入 6 次失败耗尽旧计数后解除故障，无需修改文件即在第 7 次成功收敛）
- **Gate Checks (`gate_checks.py`)**：
  - `missing_fake_executables`: exit 1, `expected_nonzero`: true (PASS)
  - `history.lmdb` vs `learning.lmdb` dummy fixture 对照: learning.lmdb 严格拒绝损坏的 18 字节 payload (PASS)

---

## 5. CMake 接线请求与交接说明

### 接线请求（供协调者独占编辑 `tests/CMakeLists.txt`）
1. **添加目标 `modernime_ui_addon_tests`**：
   - 源文件：`tests/ui/ui_addon_test.cpp`
   - 头文件目录：`ui/fcitx5/include`, `adapter/fcitx5/include`, `core/include`, `algorithm/include`, 以及 Fcitx5 Core/Config/Utils
   - 链接库：`modernime_ui` (或 `modernime_ui_layout`), `modernime_core`, `Fcitx5::Core`, `Fcitx5::Config`, `Fcitx5::Utils`, `PkgConfig::GTK3`, `PkgConfig::PANGOCAIRO`, `SQLite3::SQLite3`, `LMDB::LMDB`
   - 测试注册：`add_test(NAME modernime_ui_addon COMMAND modernime_ui_addon_tests)`
2. **添加目标 `modernime_db_fixture_helper`**：
   - 源文件：`tests/core/db_fixture_helper.cpp`
   - 头文件目录：`core/include`, `algorithm/include`
   - 链接库：`modernime_core`, `SQLite3::SQLite3`, `LMDB::LMDB`

### 交接状态
- **交付状态**：`LOCAL_VERIFIED / WIRING_PENDING`
- 所有历史未跟踪审查文档完整保留，未执行任何 `git reset` 或 `git clean`；
- 交付元数据已更新至 `build/parallel-handoff/T14.json`；
- 严格停留在 T14 边界，不进入 T15，不推送远程，不发布。
