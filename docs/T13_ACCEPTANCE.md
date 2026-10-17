# T13 验收报告：有界进程采集与包政策分离（复审整改与最终验收）

日期：2026-10-17  
审查基线：`a34549fa677cce6a5175dcaf732f1fa73486a383`  
已验收 T12 基线：`d5f48608629110fe26fa6c4d7f9673488c16bc66`  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮根据 `docs/T13_REVIEW_AND_HANDOFF.md` 第 2 至 4 节的全部复审要求，对 T13 初始实现中发现的 1 项 P0 缺陷与 5 项 P1 缺陷完成闭环整改，补齐确定性行为测试用例（T13-R01 至 T13-R10），纠正 A04/A06/A08/A12 的断言口径与覆盖范围，并通过了全部定向探针、全量 Debug/Release CTest、8 项性能门限拒绝自检、GTK 界面生命周期测试与隔离沙箱全新克隆验证。

### 验收结论：T13 复审整改全部通过

1. **R1 / P0 修复：状态发布失败与终态协议整改**
   - 采集器进程 `runInternalCollector` 在 fork 目标命令前预先验证写入 `state.json`。若写初始状态失败，向父进程写回握手字节 `'N'`（NotStarted）并退出，确保真实命令 0 次 spawn（测试 T13-R01 验证）；
   - 若目标命令可能已启动但发布失败，向父进程发送 `'U'`（Unknown），父进程保持当前 ID 占槽并转为 `CommandState::Unknown`，绝不盲目当成退出而释放槽位；
   - `Exited` 状态严格来源于有效的退出事实（采集器正常写入的终态 JSON 或经核实的僵尸主任务），禁止仅因缺少 `childPid` 或 `kill(pid, 0)` 失败推导为退出；
   - `startBackgroundPackageTracking` 修复了仅等一次 300 秒丢失多轮 Continuing 的缺陷，在后台持续循环等待直至获得终态。

2. **R2 / P1 修复：操作身份与不可混淆的精确引用**
   - 引入 `OperationRef` 结构体（包含 `operationId`、`token`、`opDir`、`collectorPid`），操作目录带随机 token（`modernime-op-<id>-<token>`）；
   - 彻底废除扫描 `/tmp` 并按 ID 前缀或 mtime 挑选目录的不安全行为。无已登记引用的查询直接返回 `CommandState::Unknown`；
   - 状态文件严格限制在 4096 字节以内，且必须为完整封闭的 JSON 对象（包含花括号及所有必要字段），伪造或损坏的状态一律拒绝（测试 T13-R04 验证）。

3. **R3 / P1 修复：后代输出继承与持续采集**
   - 采集器在目标 leader 进程退出后，不立即关闭管道，而是继续轮询排空管道，直至所有继承写端描述符的存活后代完全关闭管道（测试 T13-R05 验证）；
   - 晚到的后代输出完整保留在尾部缓冲区中，绝不触发 SIGPIPE/EPIPE，消除写入 errno 32。

4. **R4 / P1 修复：资源生命周期与有界留存**
   - 明确释放责任：同步命令执行完成后，立即对 `collectorPid` 执行 `waitpid` 回收，并删除临时操作目录，从活跃映射中移除引用；
   - 异步后台命令在结果被消费后清理资源，消除 collector 僵尸进程与未被清理的遗留目录；
   - 故障及 Unknown 路径保留必要排查证据，但常规成功/失败反复操作后僵尸进程数与留存目录数严格为 0（测试 T13-R06 验证）。

5. **R5 / P1 修复：单调 deadline 期限与提前 EOF 保护**
   - `runQueryCommand` 采用单调时钟 `deadline` 统一驱动输出读取与进程等待；管道提前 EOF 后继续等待 leader 退出至 deadline，不再由硬编码 500ms 提前杀死合法进程；
   - 包事务握手读取以 poll 挂钩 deadline，防止采集器启动卡死造成无限挂起（测试 T13-R07 验证）。

6. **R6 / P1 修复：环境继承与显式覆盖兼容**
   - 实现 `prepareEnvironment`：完整快照父进程 `environ`，保留 `HOME`、`XAUTHORITY`、`DBUS_SESSION_BUS_ADDRESS`、代理变量等必要运行环境；
   - 应用显式传入的覆盖项，合并 `PATH`，设置 `LC_ALL=C` 和 `LANG=C`；
   - 在父侧 fork 前完成环境与参数内存预分配，避免在多线程 fork 后子进程中发生 C++ 堆分配竞争（测试 T13-R08 验证）。

7. **纠正 A04/A06/A08/A12 事实与断言**
   - **A04**：实测 100MiB 输出期间与持续输出下的实际峰值，断言私有目录权限 `0700`、文件权限 `0600`，有效状态文件 $\le 4096$ 字节；
   - **A06**：在同一测试中组合覆盖 GUI 父退出、leader 退出、继承 FD 的后代晚写双流，验证跨进程导出引用与晚到输出完整性；
   - **A08**：覆盖采集器异常终止、损坏状态拒绝、旧会话同 ID 目录拒收等复合异常场景；
   - **A12**：真实调用 `modernime-settings --internal-capture-collector` 并在无 DISPLAY/WAYLAND 环境下成功执行并产出预期结果。

---

## 2. 独立复审探针与新增行为测试关卡矩阵

### 2.1 审查探针验证结果（`build/t13-review-evidence/run_probes.py`）
| 探针项目 | 审查复现基线表现 | 本轮整改后实测结果 | 结论 |
|---|---|---|---|
| `query_closed_streams` | 约 514ms 提前杀，exit=-1, timedOut=0 | state=1, exit=0, timedOut=0, elapsed_ms=1012 | **PASS** |
| `query_inherited_environment` | 环境变量丢失，exit=1 | state=1, exit=0, stdout='synthetic-review-value' | **PASS** |
| `package_inherited_environment` | 环境变量丢失，exit=1 | state=1, exit=0, stdout='synthetic-review-value' | **PASS** |
| `stale_id_directory_accepted` | 错误接受历史同 ID 目录作为 Exited | state=3 (Unknown), 拒绝未登记目录 | **PASS** |
| `malformed_state_accepted` | 伪造非 JSON 状态被接受为 Exited/0 | state=3 (Unknown), 严格校验 JSON 格式 | **PASS** |
| `package_descendant_output` | 目标后代写端遭遇 EPIPE (errno 32) | state=1, exit=0, descendant_result=OK, 0 EPIPE | **PASS** |
| `state_publication_failure` | 目标未结束即释放槽位，非零 spawn | state=0 (NotStarted), 0 次 spawn, 槽位正确保护 | **PASS** |
| `handshake_deadline` | 握手卡死无限阻塞 | state=3 (Unknown), elapsed_ms=100 精确超时 | **PASS** |
| `completed_operation_retention` | 10 次同步操作遗留 10 僵尸/10 目录 | unreaped_collectors=0, retained_directories=0 | **PASS** |

### 2.2 自动化测试矩阵（`tests/settings/process_runner_test.cpp` 全部 22 项）
| 序号 | 关卡代号 | 测试函数 | 验证行为 | 结果 |
|---|---|---|---|---|
| 1 | T13-A01 | `test_T13_A01_QueryDualStreamTruncation` | 查询双流截断与有界内存 | **PASS** |
| 2 | T13-A02 | `test_T13_A02_QueryDescendantInheritedFdDoesNotHang` | 查询孙进程继承 FD 不挂起 | **PASS** |
| 3 | T13-A03 | `test_T13_A03_PackageLargeOutput100MiBPreservesTail` | 100MiB 大输出尾部保留与环形缓冲 | **PASS** |
| 4 | T13-A04 | `test_T13_A04_ResourceBudgetMeasurement` | 真实缓冲/文件峰值测量与严格权限 (0700/0600) | **PASS** |
| 5 | T13-A05 | `test_T13_A05_PackageTimeoutTransitionsToContinuing` | 超时转移为 Continuing 并保持占槽 | **PASS** |
| 6 | T13-A06 | `test_T13_A06_ParentExitWithInheritedOutput` | GUI 父退出 + leader 退出 + 后代晚写双流 | **PASS** |
| 7 | T13-A07 | `test_T13_A07_CollectorStartFailureZeroSpawn` | 采集器启动失败保证真实命令严格 0 spawn | **PASS** |
| 8 | T13-A08 | `test_T13_A08_CollectorAbnormalTerminationTransitionsToUncertain` | 采集器异常终止转移为 Uncertain | **PASS** |
| 9 | T13-A09 | `test_T13_A09_CompletionQueueBackpressureAndBoundedRetention` | 完成队列 8 项上限与背压 | **PASS** |
| 10 | T13-A10 | `test_T13_A10_PurePackagePolicyUnit` | APT/DNF 与 Debian 版本纯政策单元测试 | **PASS** |
| 11 | T13-A11 | `test_T13_A11_T12RegressionSynchronousLockDoesNotPollute` | T12 同步锁拒绝不污染后台队列回归 | **PASS** |
| 12 | T13-A12 | `test_T13_A12_HeadlessInternalModeRunsWithoutDisplay` | 无显示实际运行内部采集模式 | **PASS** |
| 13 | T13-R01 | `test_T13_R01_StatePublicationFailureGuaranteesZeroSpawnOrUncertain` | 初始发布失败：零启动则 0 spawn，否则 Unknown | **PASS** |
| 14 | T13-R02 | `test_T13_R02_HandshakeLossOrCollectorCrashDoesNotReportNotStarted` | 启动后握手丢失不报 NotStarted、不误释放 | **PASS** |
| 15 | T13-R03 | `test_T13_R03_MultiRoundContinuingEventuallyCompletesUnderSameId` | 多轮 Continuing 后最终按原 ID 一次性完成 | **PASS** |
| 16 | T13-R04 | `test_T13_R04_OperationIdentityAndMalformedStateRejection` | 异会话同 ID/伪造 JSON 状态全部拒收 | **PASS** |
| 17 | T13-R05 | `test_T13_R05_LateWritingDescendantsReceiveZeroEpipe` | 后代晚写双流零 EPIPE，输出完整 | **PASS** |
| 18 | T13-R06 | `test_T13_R06_ResourceBoundedRetentionAndZeroZombies` | 反复操作 0 僵尸、0 遗留目录，FD 稳定 | **PASS** |
| 19 | T13-R07 | `test_T13_R07_QueryMonotonicDeadlineAndEarlyEof` | Query 提前 EOF 服从 deadline，真正超时标记 timedOut | **PASS** |
| 20 | T13-R08 | `test_T13_R08_EnvironmentInheritanceAndExplicitOverrides` | 环境快照继承与显式覆盖正确生效 | **PASS** |
| 21 | T13-R09 | `test_T13_R09_ResourcePeaksAndBudgetMeasurement` | 缓冲 $\le 64\text{KiB}$、文件 $\le 1\text{MiB}$、状态 $\le 4\text{KiB}$ 峰值测量 | **PASS** |
| 22 | T13-R10 | `test_T13_R10_HeadlessExecInternalCollectorWithoutDisplay` | 真实 exec 二进制产物无显示运行完成 | **PASS** |

---

## 3. 门禁验证汇总与证据记录

### 3.1 G1 定向测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_process_runner|modernime_update_controller|modernime_updates_page_contract|modernime_settings_focus_gtk|modernime_settings_save_gtk)$"
```
- `modernime_updates_page_contract`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.31s)
- `modernime_settings_save_gtk`: **Passed** (3.33s)
- `modernime_update_controller`: **Passed** (6.04s)
- `modernime_process_runner`: **Passed** (6.08s, 22/22 场景全通)
- **结果**：**5/5 Passed (100%)**，耗时 15.77s。

### 3.2 G3 Debug 全量功能回归
```bash
ctest --test-dir build/architecture-debug -E "modernime_personal_engine_benchmark" --output-on-failure
```
- 注册测试：68 项
- 执行测试：68 项
- 通过测试：**68 项 (100% Passed)**
- 失败测试：0 项
- 总耗时：375.86s。

### 3.3 G3 Release 全量功能与性能回归
```bash
ctest --test-dir build/architecture-release --output-on-failure
```
- 注册测试：69 项（包含 `modernime_personal_engine_benchmark`）
- 执行测试：69 项
- 通过测试：**69 项 (100% Passed)**
- 失败测试：0 项
- 总耗时：296.24s。
- 性能基准测试以标准 CTest 形式串行执行（未传任何 `--gtest_*` 参数），耗时 36.12s 顺利达标通过。

### 3.4 性能门限 8 项负向拒绝自检
```bash
./build/architecture-release/tests/modernime_personal_engine_benchmark --self_test_gate_rejection
```
- 全部 8 项指标超限样本（published_snapshot, busy_writer, concurrent_sqlite, lmdb_writer, lmdb_concurrent_pruning, lmdb_first_observer, lmdb_first_key_after_clear, startup_initial_commit）均被拒绝（exit code 1）；
- **结果**：`All 8 performance latency gates verified active and enforcing non-zero rejection on failure.`（程序返回 0）。

### 3.5 G6 GTK 界面与生命周期测试
```bash
ctest --test-dir build/architecture-debug -R "_gtk" --output-on-failure
```
- `modernime_settings_gtk_raii`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.39s)
- `modernime_settings_shell_gtk`: **Passed** (3.18s)
- `modernime_settings_save_gtk`: **Passed** (3.36s)
- **结果**：**4/4 Passed (100%)**，耗时 6.94s。

### 3.6 门禁注册与完整性校验（`verify_required_tests.py`）
```bash
python3 tests/verify_required_tests.py \
  --registered build/architecture-evidence/release-registered.json \
  --profile full --stage baseline \
  --junit build/architecture-evidence/release-junit.xml
```
- **结果**：`[OK] Required tests check passed (profile=full, stage=baseline, execution=headless).`

### 3.7 沙箱全新克隆与全生命周期验证（`verify_fresh_clone.sh`）
```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 ./tests/verify_fresh_clone.sh
```
- 沙箱环境隔离编译（Release profile，运行全部 69 项测试无跳过、无失败）；
- 产物部署到独立 prefix、桌面快捷方式与会话环境配置文件检验；
- Fcitx5 插件发现元数据测试；
- 卸载脚本完全清理验证；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`。

---

## 4. 源码身份与证据关联

- 测试时 HEAD：`a34549fa677cce6a5175dcaf732f1fa73486a383`
- 测试时工作树补丁：已持久化至 `build/architecture-evidence/working-tree.patch`
- 测试时源码哈希记录：`build/architecture-evidence/source-sha.txt`
- 注册表证据：`build/architecture-evidence/debug-registered.json`、`build/architecture-evidence/release-registered.json`
- 执行日志：`build/architecture-evidence/debug-junit.xml`、`build/architecture-evidence/release-junit.xml`

---

## 5. 边界与停下复审声明

- 本轮仅严格实施 T13 复审整改工作；
- 工作树中所有未跟踪复审文档（`docs/T11_*`、`docs/T12_*`、`docs/T13_*`、`docs/ARCHITECTURE_AUDIT.md`、`docs/REFACTOR_BACKLOG.md` 等）完整保留，未执行任何 `git reset` 或 `git clean`；
- **严格停留在 T13 边界，不进入 T14，不向远程推送，不发布**。
