# T14 验收报告：配置应用版本与路径契约（第二轮复审整改收尾）

日期：2026-10-17  
当前分支：`main`  
基线提交：`a5dbf742fd204e93f1ff68dfb644d8167411376f`  
状态：`LOCAL_VERIFIED / WIRING_PENDING`  

---

## 1. 任务概述与复审整改总结

本轮针对 `docs/T14_SECOND_REVIEW.md` 提出的 S1（首次启动事实边界）与 S2（真实消费者验收矩阵）进行了定向最小收尾，严格遵守 AGENTS.md 与原架构整改计划约束，不进入 T15（设置数据异步与轻量统计），不修改 T17 已接管的安装/启动脚本，不并发修改共享 CMake：

1. **S1 / P1：初始化与热载统一消费单次读取事实（消灭构造独立 Hash/stat，支持首次 IoError 恢复）**
   - 根因：原 `ModernIMEInputMethod` 构造函数调用 `loadSettings()`（丢弃了 `SettingsLoadResult` 的状态、Hash 和 stamp），随后重新 `stat` 并独立调用 `core::computeFileContentHash()` 打开文件计算 Hash；若在两次打开之间发生文件替换，会将旧设置标记为新 Hash；同时若首次 `loadSettings()` 遭遇 `IoError`（如 `EACCES`），`settingsLoadFailed_` 未被置为 true，首次失败后的默认配置被伪造为磁盘文件 Hash，且后续同 stamp 轮询无法触发重试。
   - 修复：
     - 在 `ModernIMEInputMethod` 中增加委托私有构造函数 `ModernIMEInputMethod(manager, const core::SettingsLoadResult &initResult)`，公开构造函数直接委托 `core::SettingsStore::load(settingsFile)`，**彻底消除构造函数中独立的第二次 `fopen`（即 `computeFileContentHash`）与独立 `stat`**；
     - `initResult.isSuccess()` 时应用配置、Hash 和 stamp，写入包含真实 Hash 的 `runtime.state`；
     - `initResult.isIoError()` 时保留默认启用配置但 Hash 记录为空，标记 `settingsLoadFailed_ = true`，写入空 Hash 的 `runtime.state`，并记录警告；后续 `pollFileChanges()` 在故障恢复后以受控频率重试，成功后收敛至磁盘配置并推进代次；
     - `initResult.isMissing()` 时遵循初始缺失文件默认契约，标记 `settingsLoadFailed_ = false`，写入空 Hash 的 `runtime.state`；
     - 在 `pollFileChanges()` 中，重载成功并推进代次时，若存在当前输入上下文，立即同步更新其设置；
   - 验证：真实 Fcitx 适配器探针及定向测试证实：构造函数仅产生 1 次打开，杜绝原子替换时新 Hash 与旧设置错配；首次 `EACCES` 恢复后自动重读磁盘配置并收敛至 `after_active=0`。

2. **S2 / 验收补齐：恢复旧学习断言、真实消费者验收与 save 签名校正**
   - 修复：
     - 恢复 `tests/adapter/engine_state_test.cpp` 中此前被误删的 3 条 `retained-prefix` 正向学习断言（`commitEvents` 非空、text 为“你”、fullPinyin 为“ni”）；
     - 修正验收文档中 `SettingsStore::save` 签名说明为 `SettingsStore::save(path, settings, error, expectedContentHash, rawLinesToPreserve)`，不虚构 API；
     - 扩充 `tests/settings/runtime_controller_test.cpp`，覆盖 `RuntimeController::probe` 对未知未来字段、多余诊断键、缺失代次/Hash、格式破损状态文件的健壮性，验证服务存活判断与状态文件解析解耦；
     - 强化 `tests/configuration_contract_test.sh`：严格指定 `--build-dir`，A01~A07 全部由真实编译对象执行验证：
       - A01：驱动真实 `modernime_settings_tests`，断言同 mtime 替换下 `SettingsStore::load` 捕获 stamp 变化并更新 Hash 与 settings；
       - A02：驱动真实 `modernime_settings_tests`，断言删除时分类为 Missing，重建后恢复 Success 并收敛；
       - A03：直接调用 `modernime_settings_tests`，验证真实 `SettingsStore::save` 冲突检测及注释/未知键保留；
       - A04：驱动 `modernime_adapter_tests`，验证真实敏感上下文中保留组合串与光标，且学习事件严格为 0；
       - A05：驱动 `modernime_ui_tests`，验证字号/候选数/布局代次失配时拒绝点击，重度量恢复后点击生效且语义不变；
       - A06：验证带空格路径下真实 SQLite 与 LMDB 数据库在沙箱中隔离，`modernime-data-guard` 备份校验正常，宿主库未被修改；
       - A07：直接执行 `modernime_runtime_controller_tests` 检验未知字段/旧格式兼容性，不使用 Python 重复实现。

---

## 2. C14 接口与路径契约说明

| 契约项 | 接口 / 协议细节 | 行为语义 |
|---|---|---|
| **读取状态类型** | `enum class SettingsLoadStatus { Success, Missing, IoError };` | 区分成功、缺失与读取故障；允许保留未知键/注释等诊断而标为 Success。 |
| **单快照同源结果** | `SettingsLoadResult` (`settings`, `rawLines`, `contentHash`, `stamp`, `diagnostics`, `status`) | 单句柄打开一次读取全部内容并校验 inode，Hash 直接从缓冲区提取，保证同源一致。 |
| **保存冲突与保留** | `SettingsStore::save(path, settings, error, expectedContentHash, rawLinesToPreserve)` 返回 `bool` | 期望 Hash 与当前磁盘 Hash 不符时拒绝覆盖；成功写入时保留未修改的注释与未知配置项。 |
| **启动与热载同源消费** | `ModernIMEInputMethod` 构造函数委托 `SettingsStore::load()` 结果 | 首次启动与轮询热载消费同一成功读取事实，不另开路径二次计算 Hash，杜绝版本冒充。 |
| **应用代次与交界** | `SettingsApplicationVersion` (`contentHash`, `generation`) | adapter/UI 仅在 `loadResult.isSuccess()` 时更新版本并推进 `generation`，同步写入 `runtime.state`。 |
| **UI 布局失效** | `hitTestCandidate(layout, metrics, x, y)` | 当 `fontSize`、`maxCandidates`、`layoutGeneration` 任一不匹配时返回 `-1`，拒绝旧布局点击。 |
| **统一路径环境** | `SettingsPaths::fromEnvironment(configRoot, dataRoot, homeRoot)` | XDG 优先、降级 HOME；启动脚本将 `MODERNIME_*` 映射为有效 `XDG_*`，与 `modernime-data-guard` 保持同一保护路径。 |

---

## 3. 契约关卡逐项实测表现 (T14-A01 ~ A07)

| 关卡代号 | 契约要求 | 验证执行方式 | 实测表现 | 结论 |
|---|---|---|---|---|
| **T14-A01** | **同 mtime 原子替换** | 原子 rename 且保持 mtime 不变时，因 inode 变化被捕获；驱动真实消费端，同源读取新配置与 Hash，更新代次。 | `tests/configuration_contract_test.sh` A01 及 `modernime_settings_tests`：mtime 一致但 inode 改变时触发重新解析，Hash 生效。 | **PASS** |
| **T14-A02** | **删除 / 重建** | 删除文件感知 missing；原路径重建新配置时捕获新 inode 并重新计算 Hash，不虚构代次。 | `tests/configuration_contract_test.sh` A02 及 `modernime_settings_tests`：感知删除与重建，正确分类缺失与恢复。 | **PASS** |
| **T14-A03** | **外部保存冲突与保留** | 磁盘被外部修改时依据 `expectedContentHash` 拒绝写入；保存成功时保留注释与未知配置项。 | `tests/configuration_contract_test.sh` A03 及 `modernime_settings_tests`：冲突拦截生效，注释与 `custom.unrecognized.key` 保持完好。 | **PASS** |
| **T14-A04** | **敏感时热载安全** | 敏感模式下热重载配置，原有 preedit 组合串完好保留，不触发任何提交学习事件。 | `tests/adapter/engine_state_test.cpp` 及 `tests/configuration_contract_test.sh` A04：热载后 preedit="nihao" 保持，commitEvents 为空。 | **PASS** |
| **T14-A05** | **字号/候选数/代次失效** | 字号、候选数或 `layoutGeneration` 不匹配时，`hitTestCandidate` 返回 `-1`；重新度量后恢复。 | `tests/ui/candidate_bar_layout_test.cpp` 及 `tests/configuration_contract_test.sh` A05：旧坐标拒绝，新度量恢复，候选词不变。 | **PASS** |
| **T14-A06** | **路径矩阵与数据库隔离** | 支持空格路径；session-init 正确生成环境；沙箱 SQLite 与 LMDB 正常读写备份，宿主数据库不受影响。 | `tests/configuration_contract_test.sh` A06、`modernime-data-guard` 及 `path_probe.py`：带空格路径读写正常，宿主库未修改。 | **PASS** |
| **T14-A07** | **runtime.state 兼容** | `runtime.state` 注入未来未知字段时无损解析与共存，不影响核心字段提取与进程探针。 | `tests/configuration_contract_test.sh` A07 及 `modernime_runtime_controller_tests`：真实 probe 测试未知/老旧/破损字段，解耦存活事实。 | **PASS** |

---

## 4. 验证汇总与证据记录

### 4.1 定向与必要 ASAN/UBSAN 验证
- **配置契约套件 (Debug)**：`modernime_settings`, `modernime_configuration_contract`, `modernime_settings_model`, `modernime_runtime_controller`
- **Adapter-UI 交互套件 (Debug)**：`modernime_fcitx5_input_panel`, `modernime_candidate_bar_layout`, `modernime_ui_tests`
- **契约总控套件**：`tests/configuration_contract_test.sh --build-dir build/architecture-debug` -> **7/7 Passed**
- **复审真实探针重验**：`build/t14-second-remediation-evidence/`
  - `swap_after_read`: PASS
  - `read_failure`: PASS
  - `startup_swap`: PASS（无构造阶段二次 open，杜绝旧配置打新 Hash）
  - `startup_read_failure`: PASS（首次 EACCES 恢复后收敛到磁盘配置）

### 4.2 交付状态说明
- **当前任务状态**：`LOCAL_VERIFIED / WIRING_PENDING`
- 依据 `docs/superpowers/plans/2026-10-17-parallel-remediation.md` 规范，本分支完成定向验证，交由协调者在集成快照中统一执行全量门禁。
- **物理桌面人工交互**：`Pending (待验)`（WSL 虚拟环境无物理显示器）。

---

## 5. 边界与交接说明

- 本轮工作严格限定在 T14 第二轮复审 S1、S2 定向收尾；
- 所有历史未跟踪审查文档完整保留，未执行任何 `git reset` 或 `git clean`；
- 交付报告写入 `build/parallel-handoff/T14.json`；
- 严格停留在 T14 边界，不进入 T15（设置数据异步与轻量统计），不向远程仓库推送，不发布。
