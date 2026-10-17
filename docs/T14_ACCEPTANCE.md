# T14 验收报告：配置应用版本与路径契约（首轮复审整改收尾）

日期：2026-10-17  
当前分支：`main`  
基线提交：`7f2f05db34073b526a623d31039b4af06c9198e8`  
状态：`LOCAL_VERIFIED / WIRING_PENDING`  

---

## 1. 任务概述与复审整改总结

本轮针对 `docs/T14_REVIEW_AND_HANDOFF.md` 提出的 R1–R4 复审问题进行了定向最小修复，严格遵守 AGENTS.md 与原架构整改计划约束，不进入 T15（设置数据异步与轻量统计）：

1. **R1 / P1：配置读取单快照同源性（解析内容与 Hash 一致）**
   - 根因：原 `SettingsStore::load()` 分两次打开文件（先解析、后计算 Hash），导致并发原子替换时返回文件 A 的内容与文件 B 的 Hash；版本去重使得后续真实的 B 无法被再次应用。
   - 修复：重构 `SettingsStore::load()` 为单一句柄单快照读取（单次 `fopen`、`fstat` 提取物理 `SettingsFileStamp`、一次性读入内存缓冲区、直接从缓冲区内存计算 FNV1a-64 Hash 并解析），并在关闭前二次校验 `stat` 与 `inode` 一致性；检测到读取中途被原子替换时进行有界重试（最多 3 次）。返回的 `settings`、`rawLines`、`contentHash`、`stamp` 保证 100% 同源自洽。
   - 验证：真实 Fcitx 探针 `run_adapter_probes.py (swap_on_hash)` 证实单次 `load` 仅调用 1 次 `fopen`（`observed_load_opens=1`），`runtime.state` 写入与当前生效配置完全同源的 Hash，杜绝跨版本拼接与错误去重。

2. **R2 / P1：区分读取状态、I/O 失败保留最后有效配置与有界重试**
   - 根因：原 `SettingsStore::load()` 失败时返回默认启用配置与空 Hash；adapter/UI 直接消费 stamp 并推进 generation，将明确禁用的输入法静默改设为启用。
   - 修复：
     - 在 `modernime::core::SettingsLoadResult` 中增加 `SettingsLoadStatus` 枚举（`Success`, `Missing`, `IoError`）及 `isSuccess()`, `isMissing()`, `isIoError()` 辅助方法；
     - `SettingsStore::load()` 严格区分文件不存在（`Missing`）与读取权限/I/O 错误（`IoError`），仅保留语法/未知键的警告为非致命诊断，不把常规 diagnostics 误当成 I/O 错误；
     - `InputMethod::pollFileChanges()` 与 `ModernIMEUserInterface::Impl::refreshSettingsIfNeeded()` 检查 `loadResult.isSuccess()`：发生 `IoError` 时保留现有有效配置，不推进应用版本（generation），不重写 `runtime.state`；在同 stamp 恢复下保留最多 5 次有界重试机会；只有 `Success` 才真正应用新配置并推进代次。
   - 验证：真实探针 `run_adapter_probes.py (read_failure)` 注入 EACCES，实测 `after_active=0 generation=0 accepted=0`，前序禁用状态得到严格保留，generation 未推进，错误读取不再误启用输入法。

3. **R3 / P1：统一启动边界环境，数据目录与 data-guard 完全对齐**
   - 根因：`scripts/modernime-session-init` 与 `install.sh` 未将 `MODERNIME_DATA_HOME` 映射到运行时 `XDG_DATA_HOME`，导致 Fcitx 运行时访问旧数据目录，而 `modernime-data-guard` 保护托管数据目录。
   - 修复：
     - 在 `scripts/modernime-session-init`、`install.sh` 及 `runtime_environment.h` 中，在所有会话与启动边界（`environment.d/90-modernime.conf`、`autostart` 快捷方式、`fcitx_environment` 环境变量字典）统一将 `MODERNIME_CONFIG_HOME` 导出为 `XDG_CONFIG_HOME`、`MODERNIME_DATA_HOME` 导出为 `XDG_DATA_HOME`；
     - C++ 内部严格遵循 `XDG_CONFIG_HOME`/`XDG_DATA_HOME` 优先并降级回 `HOME` 的原有规则，不增加隐藏全局优先级；
     - 确保运行时访问路径与 `modernime-data-guard` 保护路径完全一致，原生支持带空格的非标准 prefix 与路径。
   - 验证：`path_probe.py` 实测 `runtime_env.XDG_DATA_HOME` 与 `intended_data_root` 及 `guard_roots` 完全对齐，空格路径解析正确，活动 SQLite 与 LMDB 数据库在沙箱中隔离运行。

4. **R4 / 验收：消除伪测试，应用版本/布局代次进入真实交界**
   - 修复：
     - 为 `CandidateBarLayout` 与 `CandidateBarMetrics` 增加 `maxCandidates` 与 `layoutGeneration` 字段；`hitTestCandidate` 不仅校验字号匹配（误差 <= 1e-3），同时严格校验每页候选数 `maxCandidates` 与布局代次 `layoutGeneration` 匹配；任一失配立即返回 `-1` 拒绝旧坐标点击；重新度量后立即恢复有效命中，候选语义文本与排序保持完全不变；
     - 在 `tests/adapter/engine_state_test.cpp` 中新增敏感上下文（`isSensitive=true`）热重载测试，断言热重载期间保留原有未提交 preedit 组合串，且学习事件严格为 0；
     - 重写 `tests/configuration_contract_test.sh`：严格接受 `--build-dir` 参数，禁止回退到未指定的构建目录；真实调用 `modernime_settings_tests`、`modernime_ui_tests`、`modernime_adapter_tests` 以及 `scripts/modernime-data-guard`、`scripts/modernime-session-init`，逐项执行真实 T14-A01 ~ A07 契约断言，彻底消除 `print PASS` 伪造测试。
     - 订正文档：删去不存在的 `SettingsSaveResult::Conflict` 和 `saveWithConflictDetection`，准确说明既有 `SettingsStore::save(path, settings, expectedContentHash)` 冲突防护与注释/未知键保留机制。

---

## 2. C14 接口与路径契约说明

| 契约项 | 接口 / 协议细节 | 行为语义 |
|---|---|---|
| **读取状态类型** | `enum class SettingsLoadStatus { Success, Missing, IoError };` | 区分成功、缺失与读取故障；允许保留未知键/注释等诊断而标为 Success。 |
| **单快照同源结果** | `SettingsLoadResult` (`settings`, `rawLines`, `contentHash`, `stamp`, `diagnostics`, `status`) | 单句柄打开一次读取全部内容并校验 inode，Hash 直接从缓冲区提取，保证同源一致。 |
| **保存冲突与保留** | `SettingsStore::save(path, settings, expectedContentHash)` 返回 `bool` | 期望 Hash 与当前磁盘 Hash 不符时拒绝覆盖；成功写入时保留未修改的注释与未知配置项。 |
| **应用代次与交界** | `SettingsApplicationVersion` (`contentHash`, `generation`) | adapter/UI 仅在 `loadResult.isSuccess()` 时更新版本并推进 `generation`，同步写入 `runtime.state`。 |
| **UI 布局失效** | `hitTestCandidate(layout, metrics, x, y)` | 当 `fontSize`、`maxCandidates`、`layoutGeneration` 任一不匹配时返回 `-1`，拒绝旧布局点击。 |
| **统一路径环境** | `SettingsPaths::fromEnvironment(configRoot, dataRoot, homeRoot)` | XDG 优先、降级 HOME；启动脚本将 `MODERNIME_*` 映射为有效 `XDG_*`，与 `modernime-data-guard` 保持同一保护路径。 |

---

## 3. 契约关卡逐项实测表现 (T14-A01 ~ A07)

| 关卡代号 | 契约要求 | 验证执行方式 | 实测表现 | 结论 |
|---|---|---|---|---|
| **T14-A01** | **同 mtime 原子替换** | 原子 rename 且保持 mtime 不变时，因 inode 变化被捕获；同源读取新配置与 Hash，更新代次。 | `tests/configuration_contract_test.sh` A01 及 `modernime_settings_tests`：mtime 一致但 inode 改变时触发重新解析，Hash 生效。 | **PASS** |
| **T14-A02** | **删除 / 重建** | 删除文件感知 missing；原路径重建新配置时捕获新 inode 并重新计算 Hash，不虚构代次。 | `tests/configuration_contract_test.sh` A02 及 `modernime_settings_tests`：感知删除与重建，正确分类缺失与恢复。 | **PASS** |
| **T14-A03** | **外部保存冲突与保留** | 磁盘被外部修改时依据 `expectedContentHash` 拒绝写入；保存成功时保留注释与未知配置项。 | `tests/configuration_contract_test.sh` A03 及 `modernime_settings_tests`：冲突拦截生效，注释与 `custom.unrecognized.key` 保持完好。 | **PASS** |
| **T14-A04** | **敏感时热载安全** | 敏感模式下热重载配置，原有 preedit 组合串完好保留，不触发任何提交学习事件。 | `tests/adapter/engine_state_test.cpp` 及 `tests/configuration_contract_test.sh` A04：热载后 preedit="nihao" 保持，commitEvents 为空。 | **PASS** |
| **T14-A05** | **字号/候选数/代次失效** | 字号、候选数或 `layoutGeneration` 不匹配时，`hitTestCandidate` 返回 `-1`；重新度量后恢复。 | `tests/ui/candidate_bar_layout_test.cpp` 及 `tests/configuration_contract_test.sh` A05：旧坐标拒绝，新度量恢复，候选词不变。 | **PASS** |
| **T14-A06** | **路径矩阵与数据库隔离** | 支持空格路径；session-init 正确生成环境；沙箱 SQLite 正常读写备份，宿主数据库不受影响。 | `tests/configuration_contract_test.sh` A06、`modernime-data-guard` 及 `path_probe.py`：带空格路径读写正常，宿主库未修改。 | **PASS** |
| **T14-A07** | **runtime.state 兼容** | `runtime.state` 注入未来未知字段时无损解析与共存，不影响核心字段提取。 | `tests/configuration_contract_test.sh` A07 及 `modernime_runtime_controller_tests`：未知字段兼容解析，旧字段完好。 | **PASS** |

---

## 4. 验证汇总与证据记录

### 4.1 定向与必要 ASAN/UBSAN 验证
- **配置契约套件 (Debug)**：`modernime_settings`, `modernime_configuration_contract`, `modernime_settings_model`, `modernime_runtime_controller` -> **4/4 Passed** (2.47s)
- **Adapter-UI 交互套件 (Debug)**：`modernime_fcitx5_input_panel`, `modernime_candidate_bar_layout` -> **2/2 Passed** (2.34s)
- **Release 配置契约套件**：`tests/configuration_contract_test.sh --build-dir build/architecture-release` -> **7/7 Passed** (0.24s)
- **ASAN/UBSAN 插桩套件**：`tests/configuration_contract_test.sh --build-dir build/architecture-asan` -> **7/7 Passed** (0.35s)，0 内存越界，0 内存泄漏，0 未定义行为。
- **复审真实探针重验**：
  - `build/t14-remediation-evidence/adapter-probe-results.json`：`swap_on_hash` (opens=1, 同源自洽), `read_failure` (I/O 失败保留原有禁用，generation=0) 全部通过；
  - `build/t14-remediation-evidence/path-probe-results.json`：数据与配置根完全一致。

### 4.2 交付状态说明
- **当前任务状态**：`LOCAL_VERIFIED / WIRING_PENDING`
- 依据 `docs/superpowers/plans/2026-10-17-parallel-remediation.md` 规范，本分支完成定向验证与必要 ASAN 检查，相关接线已在本地测试通过，交由协调者在集成快照中统一执行全量构建门禁验证。
- **物理桌面人工交互**：`Pending (待验)`（WSL 虚拟环境无物理显示器）。

---

## 5. 边界与交接说明

- 本轮工作严格限定在 T14「配置应用版本与路径契约」复审 R1–R4 定向整改；
- 所有历史未跟踪审查文档完整保留，未执行任何 `git reset` 或 `git clean`；
- 交付报告写入 `build/parallel-handoff/T14.json`；
- 严格停留在 T14 边界，不进入 T15（设置数据异步与轻量统计），不向远程仓库推送，不发布。
