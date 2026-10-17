# T13 验收报告：有界进程采集与包政策分离（第四轮复审收尾与最终验收）

日期：2026-10-17  
审查基线：`667544c06e337b39c25ca1161933579fa34dd594`  
第三轮基线：`4a4a61f51c513886796a0d4058252e0cba51f74e`  
第二轮基线：`935b1da06c5e3d1c7c303a02a409ab54d4d233be`  
首轮基线：`a34549fa677cce6a5175dcaf732f1fa73486a383`  
已验收 T12 基线：`d5f48608629110fe26fa6c4d7f9673488c16bc66`  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮根据 `docs/T13_FOURTH_REVIEW.md` 第 2 至 5 节的全部复审要求，对 T13 第四轮复审中提出的缺陷与验收缺口（U1–U3）完成闭环整改与专项验证。全面保留已生效的启动证明通道、结构化词法解析、持续排空、异步安全回收、token 精确引用、Query 提前 EOF 保护、环境继承与 T12 状态机契约。

### 验收结论：T13 第四轮复审整改已满足全部契约（34 项关卡全通，6 项核心探针全部达标，安装门禁总期限与精确 JSON 验证完备，ASAN/UBSAN 父子双端全插桩 0 泄漏，沙箱全新克隆 100% 验证）

1. **U1 / P1 修复：取消未经授权的累计输出 1GiB 业务上限，保持窄化转换与域强校验**
   - 在 `tools/settings/process_runner.cpp` 的 `parseStateFile()` 中，移除了在 `stdoutBytes`、`stderrBytes`、`bufferPeak`、`filePeak` 上未经授权的 `> 1073741824LL`（1GiB）上限；
   - 明确累计排空字节数（`stdoutBytes/stderrBytes`）为无硬截止累计传输量，不是驻留缓冲大小或落盘诊断文件限制；
   - 保持目标类型（`long long` / `int`）的合法性检查（`num < 0` 严格拒收）；
   - 严格保留 PID（`[1, 4194304]`）与 `exitStatus`（Exited 必须在 `[0, 255]`，Starting/Running 为 `-1`）的强域核查；
   - 测试覆盖 1GiB 边界、1GiB+1、5GiB 合法累计量（均能正常完成并报告 Exited/0），同时保留负值、溢出值与非法退出码的确定性拒收（经探针 `overflow_bytes_limit`、`overflow_bytes_over` 与 `test_T13_T03` 验证）。

2. **U2 / 安装门禁修复：安装检查建立全局总期限，采用受约束结构化 JSON 精确验证**
   - 修复 `tests/verify_fresh_clone.sh` 中 FIFO 读取后 `wait "$t4_pid"` 无截止可能挂死的问题，建立 5 秒全局单调硬期限，超时则向测试所属子进程及后代发送 SIGKILL 退出并返回退出码 1；
   - 缺参数调用（missing arguments）严格断言退出码等于 1（而非任意非零）；
   - 废除原有的文本 grep 检查，改用 Python 结构化读取限制在 4096 字节以内的 `state.json`；
   - 严格验证 JSON 语法完整性、字段类型及精确匹配：`operationId == 98765`（阻断 `987650` 前缀伪造）、`token == "install-token-98765"`、`state == "Exited"`、`exitStatus == 0`，以及断言双流保留标记 `INSTALL_COLLECTOR_STDOUT_OK` 与 `INSTALL_COLLECTOR_STDERR_OK`；
   - 编写 `verify_install_gate.py` 进行 3 项负向/正向场景验证：真实产物退出 0（耗时 105ms）、握手后挂起 5s 超时退出 1（耗时 4992ms）、伪造/损坏 JSON 立即拒绝退出 1（耗时 100ms）；
   - 将上述契约同步纳入单元回归测试 `test_T13_T04`。

3. **U3 / 必需验收补齐：真实 Pre-exec 停滞与活 Collector 退出回归保护、双端全 ASAN/UBSAN 覆盖**
   - **Pre-exec 延迟回归保护**：在 `test_T13_T02` 中动态编译带构造函数的 `execvp` 延迟 hook 动态库，并在父侧检测 `/proc/self/maps` 自动前置 ASAN 运行库，确凿验证在子进程调用 `execvp` 之前挂起 650ms 场景下，父进程在 100ms 截止期准时返回 `CommandState::Unknown`，严格保留零启动证明；
   - **Reaper 安全停机与进程隔离**：在 `test_T13_T01` 中将包含活 collector 的 `shutdownBackgroundReaper()` 测试隔离在 `fork()` 子进程中执行，并在 `shutdownBackgroundReaper()` 中加入 `g_reaperStop.store(false)` 重置，确保不污染父测试套件的共享 reaper 线程；
   - **双端 ASAN / UBSAN 真实编译与执行**：配置并完整编译 `build/architecture-asan` profile，将 `-fsanitize=address,undefined` 同时注入父侧 `modernime_process_runner_tests` 与子侧采集器 `modernime-settings`（SHA256 校验见 `binary-identity.txt`）；全部 34 项关卡全通（0 泄漏、0 越界、0 未定义行为，退出码 0，详见 `build/t13-fourth-remediation-evidence/asan-tests.log`）；
   - **证据归档与精确耗时**：建立独立证据目录 `build/t13-fourth-remediation-evidence/`，完整记录测试时 HEAD、工作树 patch、配置、命令、binary 身份、新 JUnit XML（Debug 352.68s / Release 278.63s）及 584.44s 完整全新克隆日志。

---

## 2. 独立复审探针与行为测试关卡矩阵

### 2.1 第四轮复审与整改探针实测结果（`build/t13-fourth-remediation-evidence/probe-results.json` 与 `install-gate-results.json`）
| 探针项目 | 基线与审查表现 (667544c) | 本轮整改后实测结果 | 结论 |
|---|---|---|---|
| `reaper_pending_cpu` | 活 PID 忙循环已修复，CPU 消耗 5313μs | unreaped_collectors=0, collector_alive_at_return=1, **cpu_us=4659**（wall 1009ms） | **PASS** |
| `pre_exec_deadline` | 650ms pre-exec 停滞受 100ms 截止期约束 | **elapsed_ms=100**, **state=Unknown (3)**, exit=-1（wall 668ms） | **PASS** |
| `overflow_exit` | exitStatus=2^32 拒绝完成 | **case=exit, state=Continuing (2)**, exit=-1 | **PASS** |
| `overflow_pid` | collectorPid=PID+2^32 拒绝完成 | **case=pid, state=Continuing (2)**, exit=-1 | **PASS** |
| **`overflow_bytes_limit` (U1)** | 1GiB 边界正常放行 | **case=bytes_limit, state=Exited (1)**, exit=0 | **PASS** |
| **`overflow_bytes_over` (U1)** | 1GiB+1 边界正常放行（原先被拒收为 Continuing） | **case=bytes_over, state=Exited (1)**, exit=0 | **PASS** |
| **`install_gate: real_binary_copy` (U2)** | 真实安装产物执行合成任务 | **exit=0**, elapsed_ms=105, 双流标记与 state.json 结构验证通过 | **PASS** |
| **`install_gate: stalled_after_handshake` (U2)** | 握手后挂起假产物 | **exit=1**, elapsed_ms=4992, 5s 期限超时杀死并报错 | **PASS** |
| **`install_gate: malformed_wrong_id` (U2)** | 非 JSON / 错误 ID 假产物 | **exit=1**, elapsed_ms=100, 严格 JSON 解析拒绝 | **PASS** |

### 2.2 自动化测试矩阵（`tests/settings/process_runner_test.cpp` 全部 34 项）
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
| 12 | T13-A12 | `test_T13_A12_HeadlessInternalModeRunsWithoutDisplay` | 无显示运行内部采集模式参数防御 | **PASS** |
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
| 23 | T13-S01 | `test_T13_S01_TargetForkBeforeAckCrashPreservesStateAndExecFailureZeroSpawn` | 目标启动后握手前崩溃保持 Uncertain，exec 失败 0 spawn | **PASS** |
| 24 | T13-S02 | `test_T13_S02_StrictStateProtocolRejectionCoverage` | 严格协议拒绝缺字段/重复 key/错误后缀/伪造 PID | **PASS** |
| 25 | T13-S03 | `test_T13_S03_CollectorContinuousDrainAndZombieLeader` | 采集器无硬截止排空；多线程僵尸主任务按期限 Continuing | **PASS** |
| 26 | T13-S04 | `test_T13_S04_AsyncFinalPublishedBeforeCollectorExitReapedByBackground` | 异步终态先行发布由后台安全回收，零僵尸进程 | **PASS** |
| 27 | T13-S05 | `test_T13_S05_R01AbsolutePathAndR04ValidRefVerification` | R01 绝对路径解析与 R04 有效引用进入 parser 校验 | **PASS** |
| 28 | T13-S06 | `test_T13_S06_DualStream100MiBPeakBudgetAndFilePermissions` | 双流 10MiB 输出裁剪前真实缓冲峰值与文件峰值断言 | **PASS** |
| 29 | T13-S07 | `test_T13_S07_FreshEvidenceAndRegistrationSelfCheck` | 历史场景执行与注册自检校验 | **PASS** |
| 30 | **T13-T01** | `test_T13_T01_ReaperPendingCpuBoundedAndSafeShutdown` | 后台回收器活 PID 有限调度（CPU<50ms），**隔离子进程测试活 collector 退出重置** | **PASS** |
| 31 | **T13-T02** | `test_T13_T02_PreExecDeadlineBoundedAndNonblockingRead` | **动态 execvp 注入 pre-exec 停滞 650ms**，在 100ms 截止期准时返回 Unknown | **PASS** |
| 32 | **T13-T03** | `test_T13_T03_NumericOverflowAndProtocolSemanticsPreValidation` | **U1 修复：1GiB、1GiB+1、5GiB 累计输出正常完成**；整型溢出/负数拒收 | **PASS** |
| 33 | **T13-T04** | `test_T13_T04_VerifyFreshCloneHeadlessTaskContract` | **U2 门禁回归：缺参数严格断言 exit code 1**，结构化状态验证与假产物拒绝 | **PASS** |
| 34 | **T13-T05** | `test_T13_T05_All34TestsRegisteredAndPassing` | 全量 34 项关卡全注册与执行自检通过 | **PASS** |

---

## 3. 门禁验证汇总与证据记录

### 3.1 G1 定向测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_process_runner|modernime_update_controller|modernime_updates_page_contract|modernime_settings_focus_gtk|modernime_settings_save_gtk)$" \
  --output-junit build/t13-fourth-remediation-evidence/g1-debug.xml
```
- `modernime_updates_page_contract`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.26s)
- `modernime_settings_save_gtk`: **Passed** (3.33s)
- `modernime_update_controller`: **Passed** (5.97s)
- `modernime_process_runner`: **Passed** (9.27s，34/34 场景全通)
- **结果**：**5/5 Passed (100%)**，耗时 **18.84s**（日志见 `build/t13-fourth-remediation-evidence/g1-debug.log`）。

### 3.2 G3 Debug 全量功能回归
```bash
ctest --test-dir build/architecture-debug -E "modernime_personal_engine_benchmark" \
  --output-junit build/t13-fourth-remediation-evidence/debug-junit.xml --output-on-failure
```
- 注册测试：68 项
- 执行测试：68 项
- 通过测试：**68 项 (100% Passed)**
- 失败测试：0 项
- 跳过/阻塞：0 项
- 总耗时：**352.68s**（JUnit `timestamp="2026-10-17T15:18:05"`, `time="352"`, `process_runner=9.27438s`，日志见 `build/t13-fourth-remediation-evidence/debug-ctest.log`）。

### 3.3 G3 Release 全量功能与性能回归
```bash
ctest --test-dir build/architecture-release \
  --output-junit build/t13-fourth-remediation-evidence/release-junit.xml --output-on-failure
```
- 注册测试：69 项（包含 `modernime_personal_engine_benchmark`）
- 执行测试：69 项
- 通过测试：**69 项 (100% Passed)**
- 失败测试：0 项
- 跳过/阻塞：0 项
- 总耗时：**278.63s**（JUnit `timestamp="2026-10-17T15:24:10"`, `time="278"`, `process_runner=9.23669s`，日志见 `build/t13-fourth-remediation-evidence/release-ctest.log`）。
- 性能基准测试串行执行（未传任何 `--gtest_*` 参数），耗时 **34.09s** 顺利通过。

### 3.4 性能门限 8 项负向拒绝自检
```bash
./build/architecture-release/tests/modernime_personal_engine_benchmark --self_test_gate_rejection
```
- 全部 8 项指标超限样本（`published_snapshot`, `busy_writer`, `concurrent_sqlite`, `lmdb_writer`, `lmdb_concurrent_pruning`, `lmdb_first_observer`, `lmdb_first_key_after_clear`, `startup_initial_commit`）均被确定性拒绝（exit code 1）；
- **结果**：`All 8 performance latency gates verified active and enforcing non-zero rejection on failure.`（程序退出码 0，日志见 `build/t13-fourth-remediation-evidence/gate-rejection.log`）。

### 3.5 G6 GTK 界面与生命周期测试
```bash
ctest --test-dir build/architecture-debug -R "_gtk" \
  --output-junit build/t13-fourth-remediation-evidence/g6-debug.xml --output-on-failure
```
- `modernime_settings_gtk_raii`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.22s)
- `modernime_settings_shell_gtk`: **Passed** (3.24s)
- `modernime_settings_save_gtk`: **Passed** (3.34s)
- **结果**：**4/4 Passed (100%)**，耗时 **6.81s**（日志见 `build/t13-fourth-remediation-evidence/g6-debug.log`）。

### 3.6 门禁注册与完整性校验（`verify_required_tests.py`）
```bash
python3 tests/verify_required_tests.py \
  --registered build/t13-fourth-remediation-evidence/release-registered.json \
  --profile full --stage baseline \
  --junit build/t13-fourth-remediation-evidence/release-junit.xml
```
- **结果**：`[OK] Required tests check passed (profile=full, stage=baseline, execution=headless).`（退出码 0，日志见 `build/t13-fourth-remediation-evidence/required-tests-check.log`）。

### 3.7 沙箱全新克隆与全生命周期验证（`verify_fresh_clone.sh`）
```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 ./tests/verify_fresh_clone.sh
```
- 全新克隆隔离构建与全量 69 项测试串行执行；
- 执行沙箱内无显示真实安装产物内部采集任务门禁（带 5 秒硬期限与结构化 JSON 精确检查），断言握手确认（`R`）、正常退出码 0、`state.json` 终态（Exited, 0, ID 98765, token install-token-98765）、双流保留标记，以及缺参数时确定性退出码 1；
- 卸载清理完全性验证通过；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`（总耗时 **584.44s**，退出码 0，日志见 `build/t13-fourth-remediation-evidence/fresh-clone.log`）。

### 3.8 资源泄漏、FD 与 Sanitizer 执行状态记录
- **FD 与僵尸进程状态**：**Passed**。`test_T13_R06`、`test_T13_S04` 与 `test_T13_T01` 经并发、异步终态及活 collector 退出测试，回收期间父侧 CPU 消耗极低（4.6ms），最终无未回收僵尸（`unreaped_collectors=0`），无描述符泄漏。
- **ASAN / UBSAN 状态**：
  - **父子双端全 Profile ASAN/UBSAN 覆盖**：**Passed**。
  - 在 `build/architecture-asan` 下使用 `-fsanitize=address,undefined` 同时编译 `tools/settings/modernime-settings`（采集器）与 `tests/modernime_process_runner_tests`（父侧测试二进制）；
  - SHA256 身份见 `build/t13-fourth-remediation-evidence/binary-identity.txt`：
    - `a5249ad89b3c4aca78d6583d9e01928e6ae511cadc2fa3a07bc31a066cc1692f  modernime-settings`
    - `ccdb95ed6920281d8132f88e6eed3ee1881e6fd69698baba3175570a1c823540  modernime_process_runner_tests`
  - 运行全量 34 项测试：**34/34 Passed**，耗时 10.9s，0 内存泄漏、0 越界访问、0 未定义行为，退出码 0（日志见 `build/t13-fourth-remediation-evidence/asan-tests.log`，退出码见 `asan-tests-exit-code.txt`）。
- **捕获峰值口径与用例说明**：
  - `res.bufferPeak` 测量的是截断前实际到达的字符数（`std::string::size()`），而非底层内存分配器或容器的 `capacity`；
  - 100MiB 大数据流由 `test_T13_A03` 与 `test_T13_R09` 覆盖，验证尾部保留（$\le 1\text{MiB}$）与内存缓冲预算（$\le 64\text{KiB}$）；`test_T13_S06` 覆盖双流各 10MiB 场景。
- **物理桌面人工测试**：**Pending (待验)**。在无物理显示器的 WSL 环境中，GTK 单元测试通过虚拟显示与无显示安全退出模式全量验证。

---

## 4. 源码身份与证据关联

- **审查基线 HEAD**：`667544c06e337b39c25ca1161933579fa34dd594`
- **测试快照状态**：
  - 工作树修改补丁：`build/t13-fourth-remediation-evidence/working-tree.patch`
  - 工作树状态日志：`build/t13-fourth-remediation-evidence/working-tree.txt`
  - 源码 SHA 标记：`build/t13-fourth-remediation-evidence/source-sha.txt`
  - 二进制 SHA 校验：`build/t13-fourth-remediation-evidence/binary-identity.txt`
- **注册表证据**：
  - Debug：`build/t13-fourth-remediation-evidence/debug-registered.json`
  - Release：`build/t13-fourth-remediation-evidence/release-registered.json`
- **门禁与测试日志**：
  - G1 Debug 日志与 JUnit：`g1-debug.log`, `g1-debug.xml`
  - G6 Debug 日志与 JUnit：`g6-debug.log`, `g6-debug.xml`
  - G3 Debug 日志与 JUnit：`debug-ctest.log`, `debug-junit.xml` (352.68s, 68/68 Passed)
  - G3 Release 日志与 JUnit：`release-ctest.log`, `release-junit.xml` (278.63s, 69/69 Passed)
  - 性能门限自检日志：`gate-rejection.log`
  - 门禁注册检查日志：`required-tests-check.log`
  - 沙箱全新克隆完整日志：`fresh-clone.log` (584.44s)
- **专项复审探针输出与日志**：
  - 核心探针结果 JSON：`build/t13-fourth-remediation-evidence/probe-results.json`
  - 安装门禁探针 JSON：`build/t13-fourth-remediation-evidence/install-gate-results.json`
  - ASAN 全量 34 项测试日志：`build/t13-fourth-remediation-evidence/asan-tests.log`（34/34 Passed，退出码 0）
  - ASAN 退出码文件：`build/t13-fourth-remediation-evidence/asan-tests-exit-code.txt`
- **提交关系**：
  - 待本轮提交后将新 commit SHA 追加至 `build/t13-fourth-remediation-evidence/commit-relationship.txt`。

---

## 5. 边界与停下复审声明

- 本轮严格限定在 T13 第四轮复审整改收尾工作（U1–U3、T13-T01 至 T13-T05）；
- 工作树中所有 14 份未跟踪复审文档（`docs/ARCHITECTURE_AUDIT.md`、`docs/REFACTOR_BACKLOG.md`、`docs/T11_*`、`docs/T12_*`、`docs/T13_*`、`docs/superpowers/plans/*` 等）完整保留，未执行任何 `git reset` 或 `git clean`；
- **严格停留在 T13 边界，不进入 T14，不向远程仓库推送，不发布**。
