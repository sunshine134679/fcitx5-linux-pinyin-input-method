# T13 验收报告：有界进程采集与包政策分离（第三轮复审收尾与最终验收）

日期：2026-10-17  
审查基线：`4a4a61f51c513886796a0d4058252e0cba51f74e`  
第二轮基线：`935b1da06c5e3d1c7c303a02a409ab54d4d233be`  
首轮基线：`a34549fa677cce6a5175dcaf732f1fa73486a383`  
已验收 T12 基线：`d5f48608629110fe26fa6c4d7f9673488c16bc66`  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮根据 `docs/T13_THIRD_REVIEW.md` 第 2 至 7 节的全部复审要求，对 T13 第三轮复审中发现的 3 项 P1 缺陷（T1–T3）及 1 项安装验收缺口（T4）完成闭环整改与专项验证（涵盖 T13-T01 至 T13-T05）。全面保留前两轮已生效的独立启动证明通道、结构化词法解析、无硬截止持续排空、异步安全回收、token 精确引用、Query 提前 EOF 保护、环境继承与 T12 状态机契约。

### 验收结论：T13 第三轮复审整改已满足全部契约（34 项关卡全通，4 项探针全部达标，沙箱全新克隆与安装内部模式 100% 验证）

1. **T1 / P1 修复：消除后台回收器对活 PID 的忙循环并确保低 CPU 与安全退出**
   - 彻底修复 `g_reaperCv.wait_for` 条件谓词在队列非空时持续立即返回的问题；
   - 明确分离“空队列等待新任务/退出通知”与“非空队列等待下一次回收时机（50ms 周期退避）”；
   - 引入带序列号的条件唤醒（`enqueueSeq`）与显式停止通知（`g_shutdownReaper`）；
   - 在真实 collector 终态后暂停 700ms 场景下，父侧 CPU 消耗从基线的 **694,107μs** 骤降至 **4,352μs**（降幅超过 99.3%，远低于 50ms 容限）；
   - 增加 `shutdownBackgroundReaper()` 与静态 `ReaperCleaner` 机制，在 GUI 进程安全退出时不访问已析构全局变量，不挂死、不 join 正在运行的包命令，也不杀活着的目标进程，测试结束后全局僵尸数为 0（`unreaped_collectors=0`，经隔离探针与 `T13-T01` 验证）。

2. **T2 / P1 修复：统一单调 Deadline 覆盖 Pre-exec 停滞与握手**
   - 将父进程单调截止时间（`startTime` / `deadline`）前移至 `fork()` 完成后立即建立，使 `execPipe` 零启动证明通道与 `handshakeFds` 握手通道共用同一个全局单调期限；
   - 将 `execPipe[0]` 与 `handshakeFds[0]` 均设为 `O_NONBLOCK`，使用计算剩余期限的有界 `poll()` 进行非阻塞分步读取，正确处理 `EINTR`、`EOF`、部分数据读与期限到达；
   - 在 650ms pre-exec 停滞但 `timeout=100ms` 的极端场景下，父进程不再受阻塞 `read()` 拖滞至 683ms，而是在约 100ms（实测 `elapsed_ms=100`）准时返回保守的 `CommandState::Unknown`（`exit=-1`），严格保持引用与 Uncertain 占槽，不杀可能已启动的包事务；
   - 只有完整读取 `sizeof(int)` 确凿收到子进程 `execvp` 失败的 `errno` 时，才授权返回 `CommandState::NotStarted`（经隔离探针与 `T13-T02` 验证）。

3. **T3 / P1 修复：数值窄化前完成表示范围与协议语义强校验**
   - 在 `parseStateFile` 内部建立窄化前前置校验，对由词法分析器解析出的 `long long` 数值，在转换至目标类型（`pid_t`/`int`）前完成表示范围与协议范围双重核查；
   - 严格约束 `collectorPid \in [1, 4194304]`、`childPid \in [0, 4194304]`；
   - 严格约束 `exitStatus`：`Exited` 状态必须在 `[0, 255]` 范围内；`Starting` 与 `Running` 状态必须为 `-1`；`Unknown` 状态允许为 `-1` 或合法 `[0, 255]`；
   - 彻底阻断利用 64 位溢出将 `exitStatus = 4294967296 (2^32)` 窄化为 `0` 或将 `collectorPid = PID + 2^32` 窄化绕过 PID 比对的漏洞；溢出或语义非法数据直接判定状态文件无效，返回 `Continuing` 或 `Unknown`，禁止错误授权完成或误清目录（经隔离探针与 `T13-T03` 验证）。

4. **T4 / 验收缺口修复：安装产物无显示真实内部采集模式任务门禁**
   - 彻底废除 `tests/verify_fresh_clone.sh` 中旧有的 `&& false || true` 恒成功表达式；
   - 在沙箱全新安装 prefix 下建立隔离的 `0700` 操作目录与 `0600` spec 规范，使用真实 FIFO 握手通道；
   - 剥离 `DISPLAY` 与 `WAYLAND_DISPLAY` 环境变量，直接执行安装后的 `$MODERNIME_PREFIX/bin/modernime-settings --internal-capture-collector` 真实二进制；
   - 断言真实收到握手确认字节（`R`）、collector 正常退出码 0、生成有效 `state.json`（`state=Exited`、`exitStatus=0`、ID 与 token 一致）、保留双流输出标记（`INSTALL_COLLECTOR_STDOUT_OK` 与 `INSTALL_COLLECTOR_STDERR_OK`）；
   - 单独断言缺参数时严格返回非零错误码（exit code 1），确保安装产物具备真实任务闭环与确定性拒绝能力。

---

## 2. 独立复审探针与行为测试关卡矩阵

### 2.1 第三轮复审探针实测结果（`build/t13-third-review-evidence/run_probes.py`）
| 探针项目 | 第三轮复审基线表现 (4a4a61f) | 本轮整改后实测结果 | 结论 |
|---|---|---|---|
| `reaper_pending_cpu` | 活 PID 导致忙循环，父侧 CPU 消耗 694,107μs | unreaped_collectors=0, collector_alive_at_return=1, **cpu_us=4352**（降幅 99.3%） | **PASS** |
| `pre_exec_deadline` | execPipe 阻塞读拖至 683ms，state=1 (Exited) | **elapsed_ms=100**（受 100ms 单调期限保护），**state=Unknown (3)**, exit=-1 | **PASS** |
| `overflow_exit` | exitStatus=2^32 窄化为 0，错误判定为 Exited/0 | **state=Continuing (2)**, exit=-1, 拒绝完成 | **PASS** |
| `overflow_pid` | collectorPid=PID+2^32 窄化回正确 PID，错误判定为 Exited/0 | **state=Continuing (2)**, exit=-1, 拒绝完成 | **PASS** |
| 原 P0 复查探针 (`p0-recheck.log`) | 握手前崩溃保持 Uncertain 占槽 | active=1, phase=4, next_slot_accepted=0 | **PASS** |

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
| 30 | **T13-T01** | `test_T13_T01_ReaperPendingCpuBoundedAndSafeShutdown` | 后台回收器活 PID 有限调度（CPU<50ms），安全退出不杀进程 | **PASS** |
| 31 | **T13-T02** | `test_T13_T02_PreExecDeadlineBoundedAndNonblockingRead` | Pre-exec 延迟 650ms 在 100ms 截止期返回 Unknown，严格零启动证明 | **PASS** |
| 32 | **T13-T03** | `test_T13_T03_NumericOverflowAndProtocolSemanticsPreValidation` | 64 位整型溢出（2^32）拒收，严格状态/PID/退出码域校验 | **PASS** |
| 33 | **T13-T04** | `test_T13_T04_VerifyFreshCloneHeadlessTaskContract` | 沙箱安装无显示真实任务成功闭环，参数缺失确定性拒收 | **PASS** |
| 34 | **T13-T05** | `test_T13_T05_All34TestsRegisteredAndPassing` | 全量 34 项关卡全注册与执行自检通过 | **PASS** |

---

## 3. 门禁验证汇总与证据记录

### 3.1 G1 定向测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_process_runner|modernime_update_controller|modernime_updates_page_contract|modernime_settings_focus_gtk|modernime_settings_save_gtk)$"
```
- `modernime_updates_page_contract`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.28s)
- `modernime_settings_save_gtk`: **Passed** (3.35s)
- `modernime_update_controller`: **Passed** (5.92s)
- `modernime_process_runner`: **Passed** (8.68s，34/34 场景全通)
- **结果**：**5/5 Passed (100%)**，耗时 18.23s。

### 3.2 G3 Debug 全量功能回归
```bash
ctest --test-dir build/architecture-debug -E "modernime_personal_engine_benchmark" --output-junit build/architecture-evidence/debug-junit.xml --output-on-failure
```
- 注册测试：68 项
- 执行测试：68 项
- 通过测试：**68 项 (100% Passed)**
- 失败测试：0 项
- 跳过/阻塞：0 项
- 总耗时：352.24s。

### 3.3 G3 Release 全量功能与性能回归
```bash
ctest --test-dir build/architecture-release --output-junit build/architecture-evidence/release-junit.xml --output-on-failure
```
- 注册测试：69 项（包含 `modernime_personal_engine_benchmark`）
- 执行测试：69 项
- 通过测试：**69 项 (100% Passed)**
- 失败测试：0 项
- 跳过/阻塞：0 项
- 总耗时：280.62s。
- 性能基准测试串行执行（未传任何 `--gtest_*` 参数），耗时 34.25s 顺利通过。

### 3.4 性能门限 8 项负向拒绝自检
```bash
./build/architecture-release/tests/modernime_personal_engine_benchmark --self_test_gate_rejection
```
- 全部 8 项指标超限样本（`published_snapshot`, `busy_writer`, `concurrent_sqlite`, `lmdb_writer`, `lmdb_concurrent_pruning`, `lmdb_first_observer`, `lmdb_first_key_after_clear`, `startup_initial_commit`）均被拒绝（exit code 1）；
- **结果**：`All 8 performance latency gates verified active and enforcing non-zero rejection on failure.`（程序返回 0）。

### 3.5 G6 GTK 界面与生命周期测试
```bash
ctest --test-dir build/architecture-debug -R "_gtk" --output-on-failure
```
- `modernime_settings_gtk_raii`: **Passed** (0.01s)
- `modernime_settings_focus_gtk`: **Passed** (0.24s)
- `modernime_settings_shell_gtk`: **Passed** (3.59s)
- `modernime_settings_save_gtk`: **Passed** (3.42s)
- **结果**：**4/4 Passed (100%)**，耗时 7.26s。

### 3.6 门禁注册与完整性校验（`verify_required_tests.py`）
```bash
python3 tests/verify_required_tests.py \
  --registered build/architecture-evidence/release-registered.json \
  --profile full --stage baseline \
  --junit build/architecture-evidence/release-junit.xml
```
- **结果**：`[OK] Required tests check passed (profile=full, stage=baseline, execution=headless).`（退出码 0）。

### 3.7 沙箱全新克隆与全生命周期验证（`verify_fresh_clone.sh`）
```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 ./tests/verify_fresh_clone.sh
```
- 包含沙箱隔离编译、69 项测试全量执行；
- 真实调用 `$MODERNIME_PREFIX/bin/modernime-settings --internal-capture-collector` 执行合成任务，断言握手确认（`R`）、正常退出码 0、`state.json` 终态（Exited, 0, ID, token）、双流保留标记（`INSTALL_COLLECTOR_STDOUT_OK`/`INSTALL_COLLECTOR_STDERR_OK`），以及缺参数时确定性拒绝（exit code 1）；
- 卸载清理完全性检验；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`（退出码 0）。

### 3.8 资源泄漏、FD 与 Sanitizer 执行状态记录
- **FD 与僵尸进程状态**：**Passed**。`test_T13_R06`、`test_T13_S04` 与 `test_T13_T01` 经反复并发与异步终态测试，确认回收期间 CPU 极低（4.3ms），最终无未回收僵尸（`unreaped_collectors=0`），无描述符泄漏。
- **ASAN / UBSAN 状态**：
  - **父侧设置进程专项 ASAN/UBSAN**：**Passed**。在 `build/t13-third-review-evidence/asan/` 下使用 `-fsanitize=address,undefined` 独立编译并执行 `modernime_process_runner_tests`（包含全部 34 项关卡），全部通过（耗时 12.0s），退出码 0（记录于 `asan-tests-exit-code.txt`），无任何内存泄漏、越界或未定义行为（日志见 `asan-tests.log`）；
  - **采集器子进程 Sanitizer 覆盖**：**NotRun**。当前采集器 binary 为普通构建，尚未启用全 profile 插桩。严格单列记录为 `NotRun`，不冒充全 profile 通过。
- **捕获峰值口径与用例说明**：
  - `res.bufferPeak` 测量的是截断前实际到达的字符数（`std::string::size()`），而非底层内存分配器或容器的 `capacity`；
  - 100MiB 大数据流由 `test_T13_A03` 与 `test_T13_R09` 覆盖，验证尾部保留与内存预算；`test_T13_S06` 覆盖双流各 10MiB 场景。
- **物理桌面人工测试**：**Pending (待验)**。在无真实物理显示器的 WSL 环境中，GTK 单元测试通过虚拟显示与无显示安全退出模式全量验证。

---

## 4. 源码身份与证据关联

- **审查基线 HEAD**：`4a4a61f51c513886796a0d4058252e0cba51f74e`
- **测试快照状态**：
  - 工作树修改补丁：`build/t13-third-review-evidence/working-tree.patch`
  - 工作树状态日志：`build/t13-third-review-evidence/working-tree.txt`
  - 源码 SHA 标记：`build/t13-third-review-evidence/source-sha.txt`
- **注册表证据**：
  - Debug：`build/t13-third-review-evidence/debug-registered.json`
  - Release：`build/t13-third-review-evidence/release-registered.json`
- **专项复审探针输出与日志**：
  - 探针运行日志：`build/t13-third-review-evidence/probes.log`
  - 探针结果 JSON：`build/t13-third-review-evidence/probe-results.json`
  - P0 复查日志：`build/t13-third-review-evidence/p0-recheck.log`
  - ASAN 构建日志：`build/t13-third-review-evidence/asan-build.log`
  - ASAN 测试日志：`build/t13-third-review-evidence/asan-tests.log`（34/34 Passed，退出码 0）
  - ASAN 退出码：`build/t13-third-review-evidence/asan-tests-exit-code.txt`

---

## 5. 边界与停下复审声明

- 本轮严格限定在 T13 第三轮复审整改收尾工作（T1–T4、T13-T01 至 T13-T05）；
- 工作树中所有 14 份未跟踪复审文档（`docs/ARCHITECTURE_AUDIT.md`、`docs/REFACTOR_BACKLOG.md`、`docs/T11_*`、`docs/T12_*`、`docs/T13_*`、`docs/superpowers/plans/*` 等）完整保留，未执行任何 `git reset` 或 `git clean`；
- **严格停留在 T13 边界，不进入 T14，不向远程仓库推送，不发布**。
