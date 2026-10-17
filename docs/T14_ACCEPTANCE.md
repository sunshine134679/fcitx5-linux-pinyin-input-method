# T14 验收报告：配置应用版本与路径契约

日期：2026-10-17  
当前分支：`main`  
T13 验收基线：`8d10597c8d74de4f428143cf2366c4ecd262ded0`  
前序测试维护提交：`12e125d7c16b08b6f3db0db257f44779dee0cb6d` (`test(install): harden install gate matrix fixtures and register ctest entry`)  
T14 实施提交基线：`12e125d7c16b08b6f3db0db257f44779dee0cb6d`  

---

## 1. 任务概述与验收结论

本轮根据 `docs/superpowers/plans/2026-10-16-architecture-remediation.md` 的 T14 规划及 `docs/T13_FINAL_REVIEW_AND_T14_HANDOFF.md` 的交接契约，完成两阶段工作：
1. **P2 安装门禁测试维护（前序已独立提交 `12e125d`）**：
   - 修正 `tests/test_install_gate.py` 中假产物权限为 0600，补齐 0700 目录与 0600 文件专门权限负向场景；
   - 增加 `valid_fake_control` 有效假产物正向基准对照用例；
   - 参数化 `--binary` 路径，消除硬编码 `architecture-debug` 路径依赖；
   - 负向用例不仅断言非零退出，而且严格匹配稳定的 stderr 预期错误原因与类别；
   - 在 `tests/CMakeLists.txt` 中将安装门禁矩阵注册为 CTest 目标 `modernime_install_gate_matrix`（覆盖 22 组正负场景）。
2. **T14「配置应用版本与路径契约」核心实现与验证**：
   - 引入物理文件状态戳 `SettingsFileStamp`（包含 `dev`、`ino`、`size`、`mtimeSec`、`mtimeNsec`、`ctimeSec`、`ctimeNsec`、`exists`），彻底解决传统仅依赖 `mtime` 在同时间戳原子重命名（`rename`）与删除重建场景下无法感知物理变化的缺陷；
   - 引入已应用配置版本 `SettingsApplicationVersion`（包含 `contentHash` 与会话自增 `generation`），仅存在于内存与兼容诊断，不污染用户持久化配置格式；
   - 明确配置生命周期各状态语义边界：`loaded`（磁盘最新）、`edited`（内存编辑）、`saved`（落盘成功）、`applied`（引擎与 UI 真正生效），保留 `SettingsStore::saveWithConflictDetection` 外部保存冲突防护；
   - 重构 `adapter/fcitx5/src/inputmethod.cpp` 与 `ui/fcitx5/src/ui_addon.cpp` 的配置轮询观察机制：由标量 `last_write_time` 升级为 `SettingsFileStamp`，物理变化触发读取，内容变化触发真正应用与代次推进，并向 `runtime.state` 写入应用诊断（`source=fcitx5-adapter`、`contentHash`、`generation`）；
   - 建立字号与布局边界防护：`CandidateBarLayout` 记录 `fontSize`，字号不匹配时 `hitTestCandidate` 返回 `-1` 拒绝旧坐标点击，`ui_addon` 在字号或每页数量变动时立即失效旧布局几何，语义候选序列、排序算法、分页与混排契约完全保持不变；
   - 确立统一启动路径边界：`SettingsPaths::fromEnvironment` 优先遵循 `XDG_CONFIG_HOME` / `XDG_DATA_HOME`，降级回 `HOME`，原生支持包含空格的非标准路径；保证活动 SQLite / LMDB 数据库在沙箱环境中隔离运行，无跨库泄漏；
   - 保证 `runtime.state` 诊断向前/向后兼容性：允许外部注入未知扩展字段，且明确其仅为再生诊断，不授予进程存活或越权；
   - 新增 `tests/configuration_contract_test.sh` 脚本并注册为 CTest 测试 `modernime_configuration_contract`，全面覆盖 T14-A01 到 A07 契约。

### 验收结论：T14 全部契约已 100% 通过验证，沙箱全新克隆全生命周期验证通过，物理桌面继续保持待验，严格停在 T14 边界，不进入 T15，不推送、不发布。

---

## 2. T14-A01 ~ T14-A08 关卡逐项验收表现

| 关卡代号 | 契约要求 | 验证实现与测试方法 | 实测表现 | 结论 |
|---|---|---|---|---|
| **T14-A01** | **同 mtime 替换** | 原子写入（`rename`）且保持 `mtime` 不变时，因 inode 改变仍能被 `SettingsFileStamp` 捕获；重新加载后计算 `contentHash`，检测到内容变化触发应用，更新 `SettingsApplicationVersion` 并同步到 `runtime.state`。 | `tests/core/settings_test.cpp` (`SettingsFileStampSameMtimeAtomicReplacement`) 及 `tests/configuration_contract_test.sh` A01 执行通过：mtime 一致但 inode 变化时 `stamp != oldStamp`，新哈希生效，`generation` 递增。 | **PASS** |
| **T14-A02** | **删除 / 重建** | 文件删除时 `SettingsFileStamp.exists == false`，系统感知缺失；在同一路径重新创建新配置时，新 inode/stamp 再次触发重载，计算新 contentHash 与 generation；错误读取不冒充新应用版本。 | `tests/core/settings_test.cpp` (`SettingsFileStampDeleteAndRecreate`) 及 `tests/configuration_contract_test.sh` A02 通过：删除后感知 `exists=false`，重建后捕获新 inode 并重新计算哈希。 | **PASS** |
| **T14-A03** | **外部保存冲突** | 磁盘被外部修改（contentHash 不一致）时，`SettingsStore::saveWithConflictDetection` 严格返回 `SettingsSaveResult::Conflict`，未保存的编辑不被静默覆盖；写入成功时保留注释与未知配置项。 | `tests/configuration_contract_test.sh` A03 通过：基于旧哈希保存被冲突防护拦截，未保存编辑得到保留。 | **PASS** |
| **T14-A04** | **敏感时热载** | 运行时配置热加载不破坏组合状态与输入法光标；已禁用的学习状态与剪贴板权限在热载后保持隔离，杜绝跨会话越权。 | `tests/configuration_contract_test.sh` A04 通过：热载期间组合状态与敏感隔离规则完好。 | **PASS** |
| **T14-A05** | **字号与旧布局** | 字号变更使得旧布局几何失效；`CandidateBarLayout` 记录 `fontSize`，在字号不匹配时 `hitTestCandidate` 返回 `-1` 拒绝旧坐标点击；不改变语义候选序列、排序或分页。 | `tests/ui/candidate_bar_layout_test.cpp` (`LayoutFontSizeMismatchRejectsHitTest`) 及 `tests/configuration_contract_test.sh` A05 通过：字号由 14 变更为 18 时，旧坐标点击返回 -1 被拒绝；UI 刷新后新坐标正常命中，候选文字顺序不变。 | **PASS** |
| **T14-A06** | **路径矩阵** | 运行路径解析遵循 `XDG_CONFIG_HOME`/`XDG_DATA_HOME` 优先，降级到 `HOME`；支持带空格的非标准 prefix 路径；SQLite/LMDB 在沙箱路径中活动，无旧库误开或泄露。 | `tests/core/settings_test.cpp` (`SettingsPathsSpaceAndCustomEnv`) 及 `tests/configuration_contract_test.sh` A06 通过：带空格路径 `/tmp/modern ime path test/app` 解析准确，SQLite/LMDB 在隔离数据目录正常读写。 | **PASS** |
| **T14-A07** | **runtime.state 兼容** | `runtime.state` 诊断文件包含 `source=fcitx5-adapter`、`contentHash`、`generation`；外部注入未知新字段或旧字段时无损解析与保留；明确诊断不作为进程存活凭据。 | `tests/configuration_contract_test.sh` A07 通过：注入未知字段 `future_extension_field=enabled` 兼容共存，旧字段无丢失。 | **PASS** |
| **T14-A08** | **接线与证据** | 新增测试注册入 CTest；`verify_required_tests.py` 全 profile 契约通过；Debug/Release/GTK/TSAN/Fresh-clone 完整产出日志与证据归档。 | CTest 目标 `modernime_configuration_contract` 与 `modernime_install_gate_matrix` 均全通过；Debug 71/71、Release 71/71、GTK 4/4、TSAN 2/2、Fresh-clone 100% 记录在案。 | **PASS** |

---

## 3. 门禁验证汇总与证据记录

### 3.1 G1 配置、环境与模型定向测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_settings|modernime_configuration_contract|modernime_update_controller|modernime_process_runner|modernime_install_gate_matrix|modernime_runtime_controller|modernime_data_controller|modernime_install_runtime|modernime_install_environment|modernime_session_init|modernime_settings_install)$"
```
- `modernime_settings`: **Passed** (0.01s)
- `modernime_configuration_contract`: **Passed** (0.05s)
- `modernime_update_controller`: **Passed** (6.27s)
- `modernime_process_runner`: **Passed** (9.33s, 34/34 场景)
- `modernime_install_gate_matrix`: **Passed** (22.50s, 22/22 场景)
- `modernime_runtime_controller`: **Passed** (2.28s)
- `modernime_data_controller`: **Passed** (0.04s)
- `modernime_install_runtime`: **Passed** (28.01s)
- `modernime_install_environment`: **Passed** (0.32s)
- `modernime_session_init`: **Passed** (1.57s)
- `modernime_settings_install`: **Passed** (5.01s)
- **结果**：**11/11 Passed (100%)**，耗时 **75.38s**（完整日志见 `build/t14-acceptance-evidence/g1.log`）。

### 3.2 必需测试基线契约检查（`verify_required_tests.py`）
```bash
ctest --test-dir build/architecture-debug -R modernime_required_tests_contract --output-on-failure
```
- 验证所有必需测试（`tests/required_tests_baseline.json`）在构建树中准确注册与匹配，无隐式缺失；
- **结果**：**1/1 Passed (100%)**，耗时 **0.07s**（日志见 `build/t14-acceptance-evidence/required-tests-contract.log`）。

### 3.3 G6 GTK 界面与配置保存测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_settings_gtk_raii|modernime_settings_focus_gtk|modernime_settings_shell_gtk|modernime_settings_save_gtk)$"
```
- `modernime_settings_gtk_raii`: **Passed** (0.01s)
- `modernime_settings_focus_gtk`: **Passed** (0.45s)
- `modernime_settings_shell_gtk`: **Passed** (2.94s)
- `modernime_settings_save_gtk`: **Passed** (3.34s)
- **结果**：**4/4 Passed (100%)**，耗时 **6.74s**（完整日志见 `build/t14-acceptance-evidence/gtk-tests.log`）。

### 3.4 G3 Debug 全量功能回归（CTest 71 项）
```bash
ctest --test-dir build/architecture-debug --output-on-failure --output-junit build/t14-acceptance-evidence/debug-junit.xml
```
- 覆盖词库、拼音切分、分词、候选排队、UI 布局、进程采集器（34 项）、安装门禁矩阵（22 项）、配置契约、GTK 设置界面、升级事务与包仓库门禁；
- **结果**：**71/71 Passed (100%)**，耗时 **563.10s**（日志见 `build/t14-acceptance-evidence/debug-ctest.log`，JUnit XML 见 `debug-junit.xml`）。

### 3.5 G3 Release 全量功能与基准性能回归（CTest 71 项）
```bash
ctest --test-dir build/architecture-release --output-on-failure --output-junit build/t14-acceptance-evidence/release-junit.xml
```
- 包含全量单元测试、集成测试及核心性能基准；
- **结果**：**71/71 Passed (100%)**，耗时 **320.91s**（日志见 `build/t14-acceptance-evidence/release-ctest.log`，JUnit XML 见 `release-junit.xml`）。

### 3.6 G5 存储线程安全（TSAN，`setarch x86_64 -R` 模式）
```bash
setarch x86_64 -R ctest --test-dir build/architecture-tsan --output-on-failure \
  -R "^(modernime_learning_store|modernime_personal_engine_gtest)$"
```
- `modernime_learning_store`: **Passed** (95.04s)
- `modernime_personal_engine_gtest`: **Passed** (156.05s)
- **结果**：**2/2 Passed (100%)**，耗时 **251.09s**，0 数据竞争（Data Race），0 线程泄漏。

### 3.7 沙箱全新克隆与全生命周期验证（`verify_fresh_clone.sh`）
```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 ./tests/verify_fresh_clone.sh
```
- 完整流程：
  1. 隔离目录全量 CMake 配置与编译；
  2. 串行执行全量 71 项 CTest 回归（100% Passed）；
  3. 执行 `install.sh` 安装至临时沙箱目录（`/tmp/modernime-fresh-clone-test.XXXXXX/prefix`）；
  4. 触发真实安装产物门禁检查矩阵（22 项正负场景全部通过）；
  5. 执行 `uninstall.sh` 并彻底校验文件清理完全性；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`，耗时 **625.01s**，退出码 0（完整日志见 `build/t14-acceptance-evidence/fresh-clone.log`）。

### 3.8 物理桌面人工测试状态
- **状态**：**Pending (待验)**。在无物理显示器的 WSL 环境中，GTK 单元测试通过虚拟显示与无显示安全退出模式验证，真实物理桌面端人工交互与视觉效果仍标记为待验。

---

## 4. 源码身份与证据关联

- **审查与交接基线**：`8d10597c8d74de4f428143cf2366c4ecd262ded0`
- **前序测试维护提交**：`12e125d7c16b08b6f3db0db257f44779dee0cb6d`
- **证据文件存放位置**：`build/t14-acceptance-evidence/`
  - `g1.log`: G1 11 项测试通过日志（75.38s）
  - `gtk-tests.log`: G6 GTK 4 项测试通过日志（6.74s）
  - `required-tests-contract.log`: 必需测试基线匹配日志（0.07s）
  - `debug-ctest.log`: Debug 全量 71 项 CTest 日志（563.10s）
  - `debug-junit.xml`: Debug 71 项 JUnit XML 证据
  - `release-ctest.log`: Release 全量 71 项 CTest 日志（320.91s）
  - `release-junit.xml`: Release 71 项 JUnit XML 证据
  - `fresh-clone.log`: 沙箱全新克隆与全生命周期验证日志（625.01s）
  - `binary-identities.txt`: 编译二进制 SHA256 校验哈希
  - `commit-relationship.txt`: 提交关系链记录
  - `working-tree.patch`: 本阶段源码修改完整补丁
- **二进制 SHA256**：
  - `build/architecture-debug/tests/modernime_settings_tests`: `40b6398a658c705af98068cf236bd26b2f5340e51b5be643fbfa14b28ca21416`
  - `build/architecture-release/tests/modernime_settings_tests`: `5526214bd58c4ee46ff2696471e03569512621e796479b53d49ac7e00079cc57`
  - `build/architecture-debug/tools/settings/modernime-settings`: `9417edbe5573f7b9c9b9e5ce6b5520be5c6944f633b050f2f76bb25679843ddd`
  - `build/architecture-release/tools/settings/modernime-settings`: `bdf40cc6424658f3fcbbfd474bafc5a2185d7b3a88e6546edf8554d76243299d`

---

## 5. 边界与停下复审声明

- 本轮工作严格限定在 T14「配置应用版本与路径契约」范畴及前序 P2 安装门禁测试维护；
- 工作树中所有 17 份历史未跟踪审查与规划文档完整保留，未执行任何 `git reset` 或 `git clean`；
- **严格停留在 T14 边界，不进入 T15（包管理沙箱与多包安装），不修改候选重排/学习算法，不修改数据库格式，不向远程仓库推送，不发布**。
