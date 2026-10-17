# T13 验收报告：有界进程采集与包政策分离（第五轮复审收尾与最终验收）

日期：2026-10-17  
审查基线：`7c2a00aa9a5a45726735a5af93f325c2c3900d96`  
第四轮基线：`667544c06e337b39c25ca1161933579fa34dd594`  
第三轮基线：`4a4a61f51c513886796a0d4058252e0cba51f74e`  
第二轮基线：`935b1da06c5e3d1c7c303a02a409ab54d4d233be`  
首轮基线：`a34549fa677cce6a5175dcaf732f1fa73486a383`  
已验收 T12 基线：`d5f48608629110fe26fa6c4d7f9673488c16bc66`  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮根据 `docs/T13_FIFTH_REVIEW.md` 的要求，对 T13 第五轮复审中提出的 U2 安装门禁遗留缺口（U2-A、U2-B）及 P2 记录与测试保护事项完成闭环整改与专项验证。生产代码 `tools/settings/process_runner.cpp` 的所有核心契约（启动证明、无硬截止持续排空、异步安全回收、单调 deadline、数值安全窄化、大累计输出 Exited/0 支持及生产事务超时不 kill 契约）保持原样且已全部独立通过。

### 验收结论：T13 第五轮复审整改已满足全部契约（安装门禁 19 组正负场景全通，单调期限与精准后代回收确立，对象构建前重复 key 拦截完备，双端 ASAN/UBSAN 全插桩 0 泄漏，沙箱全新克隆全生命周期 100% 验证）

1. **U2-A / P1 修复：以单调时钟建立统一单调总期限，独立会话隔离与精准级联回收**
   - 彻底废除 `date +%s` 墙钟差值，采用 Python `time.monotonic()` 建立统一 5.0 秒单调硬期限，覆盖 FIFO 握手读取与进程终态等待，完全免疫墙钟回拨或时钟固定注入；
   - 采集器以 `start_new_session=True` 启动，形成独立的进程组/会话（PGID = `proc.pid`）；
   - 超时或退出时通过 `os.killpg(proc.pid, signal.SIGKILL)` 级联清理测试所属的直接子进程与所有衍生后代（如假后代睡眠进程），确保门禁退出后测试后代存活数为 0（`descendant_alive = false`）；
   - 仅对测试所属私有进程组操作，杜绝全局 `pkill` 或模糊匹配，严格不影响宿主其他进程或生产包事务。

2. **U2-B / P1 修复：对象构建前拒绝重复字段，精确校验类型/身份/终态与完备预算检查**
   - 使用 `json.loads` 配合 `object_pairs_hook=reject_duplicate_keys`，在构建 JSON 对象前严格拦截重复 key（如 `operationId=987650/98765`、`exitStatus=1/0`、相同 key 重复等），阻断最后一个合法值覆盖前序恶意字段；
   - 恢复并补齐目录权限（0700）与文件权限（0600）检查；
   - 补充资源预算约束：`state.json` $\le 4096$，`stdout.tail` / `stderr.tail` $\le 65536$，诊断文件 $\le 1048576$；
   - 严格断言关键字段精确类型（`int`/`str`/`bool`）与值（`operationId == 98765`、`token == "install-token-98765"`、`state == "Exited"`、`exitStatus == 0`）及双流预期标记；
   - 在 `tests/test_install_gate.py` 中建立覆盖 19 组正负场景的定向矩阵，所有坏结果有界非零，真实安装产物正常任务不退化。

3. **P2 测试参数与停机机制订正**
   - **区分 committed fixture 与独立探针参数**：
     - committed 测试 `test_T13_T02`：注入 350ms pre-exec hook，截止期 80ms；
     - 独立探针 `pre_exec_deadline`：注入 650ms pre-exec stall，截止期 100ms；
   - **Pre-exec 编译保护**：`test_T13_T02` 动态 hook 编译增加 `TEST_ASSERT(compileRet == 0 && std::filesystem::exists(hookSo))`，编译失败确定性报错，禁止静默跳过；
   - **全新 exec 停机测试**：`test_T13_T01` 停机测试采用 `selfExe --subtest-shutdown-reaper` 通过全新的 fork+exec 独立进程执行，避免在多线程父进程中仅 fork 而继承非活动 `std::thread` 句柄的问题；
   - **资源预算与口径更正**：
     - Query 默认每流缓冲与结果为 1MiB；
     - 包事务配置的返回 tail 结果为 64KiB (`perStreamResultBytes = 65536`)；
     - 诊断文件上限为 1MiB (`perStreamFileBytes = 1048576`)；
     - `bufferPeak` 测量的是截断前实际到达字符串的字符数（`std::string::size()`），而非容器底层 capacity；
   - **ASAN/UBSAN 证据与引用**：
     - 解释 `asan-tests.log` 哈希一致原因：`modernime_process_runner_tests` 输出为无时间戳的确定性文本流，各次通过输出完全相同；
     - 归档 `build/t13-fifth-remediation-evidence/asan-result.json`，记录执行时间戳（2026-10-17T16:19:36+08:00）、耗时（11.2s）、退出码 0、二进制 sha256；
     - 明确引用审查方独立验证结果 `build/t13-fifth-review-evidence/asan-result.json`（34/34 Passed，0 错误，退出码 0）。

---

## 2. 独立复审探针与安装门禁矩阵

### 2.1 第五轮复审核心探针实测结果（`build/t13-fifth-review-evidence/run_probes.py`）
| 探针项目 | 基线与审查表现 | 本轮整改后实测结果 | 结论 |
|---|---|---|---|
| `reaper_pending_cpu` | 活 PID 忙循环已消除，CPU 极低 | unreaped_collectors=0, collector_alive_at_return=1, **cpu_us=4335**（wall 1013ms） | **PASS** |
| `pre_exec_deadline` | 650ms pre-exec 停滞受 100ms 截止期约束 | **elapsed_ms=100**, **state=Unknown (3)**, exit=-1（wall 670ms） | **PASS** |
| `overflow_exit` | exitStatus=2^32 拒绝完成 | **case=exit, state=Continuing (2)**, exit=-1 | **PASS** |
| `overflow_pid` | collectorPid=PID+2^32 拒绝完成 | **case=pid, state=Continuing (2)**, exit=-1 | **PASS** |
| `overflow_bytes_limit` | 1GiB 边界正常放行 | **case=bytes_limit, state=Exited (1)**, exit=0 | **PASS** |
| `overflow_bytes_over` | 1GiB+1 边界正常放行 | **case=bytes_over, state=Exited (1)**, exit=0 | **PASS** |

### 2.2 安装门禁 19 组正负场景测试矩阵（`tests/test_install_gate.py`）
| 序号 | 测试场景 | 注入行为 / 异常类型 | 判定规则 | 实测表现 | 结果 |
|---|---|---|---|---|---|
| 1 | `real_binary_copy` | 真实 debug binary 执行合成任务 | exit == 0, elapsed < 1000ms | exit=0, elapsed=115ms, descendant_alive=False | **PASS** |
| 2 | `duplicate_fields` | state.json 包含重复 operationId (987650/98765) | exit != 0, elapsed < 2500ms | exit=1, elapsed=115ms, 拦截重复 key | **PASS** |
| 3 | `duplicate_token_identical` | state.json 包含两个相同的 token key | exit != 0, elapsed < 2500ms | exit=1, elapsed=116ms, 拦截相同重复 key | **PASS** |
| 4 | `wrong_id` | operationId = 987650 | exit != 0, elapsed < 2500ms | exit=1, elapsed=114ms, ID 校验拒绝 | **PASS** |
| 5 | `wrong_token` | token = "wrong-token" | exit != 0, elapsed < 2500ms | exit=1, elapsed=114ms, Token 校验拒绝 | **PASS** |
| 6 | `wrong_state` | state = "Running" | exit != 0, elapsed < 2500ms | exit=1, elapsed=114ms, 非 Exited 拒绝 | **PASS** |
| 7 | `wrong_exit_status` | exitStatus = 1 | exit != 0, elapsed < 2500ms | exit=1, elapsed=115ms, 非 0 退出码拒绝 | **PASS** |
| 8 | `wrong_type_id` | operationId = "98765" (字符串) | exit != 0, elapsed < 2500ms | exit=1, elapsed=115ms, 类型不匹配拒绝 | **PASS** |
| 9 | `malformed_json` | 非法 JSON 语法 | exit != 0, elapsed < 2500ms | exit=1, elapsed=114ms, 语法解析拒绝 | **PASS** |
| 10 | `missing_stdout_marker` | stdout.tail 缺失成功标记 | exit != 0, elapsed < 2500ms | exit=1, elapsed=118ms, 流标记核验拒绝 | **PASS** |
| 11 | `missing_stderr_marker` | stderr.tail 缺失成功标记 | exit != 0, elapsed < 2500ms | exit=1, elapsed=115ms, 流标记核验拒绝 | **PASS** |
| 12 | `oversized_state` | state.json 超过 4096 字节预算 | exit != 0, elapsed < 2500ms | exit=1, elapsed=115ms, 预算越界拒绝 | **PASS** |
| 13 | `oversized_tail` | stdout.tail 超过 65536 字节预算 | exit != 0, elapsed < 2500ms | exit=1, elapsed=117ms, 预算越界拒绝 | **PASS** |
| 14 | `pre_handshake_stall` | 握手前停滞 30 秒 | exit != 0, 4.5s..6.5s, 0 后代 | exit=1, elapsed=5080ms, 5s 单调超时退出 | **PASS** |
| 15 | `post_handshake_stall` | 握手后停滞 30 秒 | exit != 0, 4.5s..6.5s, 0 后代 | exit=1, elapsed=5081ms, 5s 单调超时退出 | **PASS** |
| 16 | `stalled_with_descendant` | 产生睡眠后代并停滞 30 秒 | exit != 0, 4.5s..6.5s, 0 后代 | exit=1, elapsed=5081ms, **descendant_alive=False** | **PASS** |
| 17 | `descendant_inherits_fd` | 孙进程持有 FIFO，主进程立即退出 42 | exit != 0, 0 后代 | exit=1, elapsed=115ms, **descendant_alive=False** | **PASS** |
| 18 | `stalled_clock_not_advancing` | 注入 mock date 固定墙钟输出 | exit != 0, 4.5s..6.5s | exit=1, elapsed=5079ms, 单调时钟免疫 | **PASS** |
| 19 | `crash_before_handshake` | 握手前直接退出 77 | exit != 0, elapsed < 2500ms | exit=1, elapsed=115ms, 快速检测崩溃 | **PASS** |

### 2.3 自动化测试矩阵（`tests/settings/process_runner_test.cpp` 全部 34 项）
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
| 30 | **T13-T01** | `test_T13_T01_ReaperLowCpuAndSafeShutdown` | 后台回收器活 PID 有限调度（CPU<50ms），**全新 fork+exec 独立进程测试安全停机** | **PASS** |
| 31 | **T13-T02** | `test_T13_T02_PreExecDeadlineBoundedAndStrictZeroSpawn` | **动态 execvp 注入 pre-exec 停滞 350ms，deadline 80ms 严格断言编译成功** | **PASS** |
| 32 | **T13-T03** | `test_T13_T03_NumericRangeAndProtocolValidation` | 1GiB、1GiB+1、5GiB 累计输出正常完成；整型溢出/负数拒收 | **PASS** |
| 33 | **T13-T04** | `test_T13_T04_InstalledBinaryHeadlessInvocationRegression` | 缺参数断言 exit code 1，**状态文件重复 key 确定性拒收**，合成任务验证通过 | **PASS** |
| 34 | **T13-T05** | `test_T13_T05_TestSuiteSelfCheckT13` | 全量 34 项关卡全注册与执行自检通过 | **PASS** |

---

## 3. 门禁验证汇总与证据记录

### 3.1 G1 定向测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_process_runner|modernime_update_controller|modernime_updates_page_contract|modernime_settings_focus_gtk|modernime_settings_save_gtk)$" \
  --output-junit build/t13-fifth-remediation-evidence/g1-debug.xml
```
- `modernime_updates_page_contract`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.18s)
- `modernime_settings_save_gtk`: **Passed** (3.31s)
- `modernime_update_controller`: **Passed** (5.75s)
- `modernime_process_runner`: **Passed** (9.29s，34/34 场景全通)
- **结果**：**5/5 Passed (100%)**，耗时 **18.54s**（日志见 `build/t13-fifth-remediation-evidence/g1-debug.log`）。

### 3.2 G3 Debug / Release 全量功能与性能回归（证据复用声明）
- 核心生产代码 `tools/settings/process_runner.cpp` 本轮未做修改，上一轮完整生成的 68 项 Debug（耗时 352.68s）与 69 项 Release（耗时 278.63s，基准测试 34.09s）以及性能门限 8 项负向拒绝自检已 100% 通过；
- 相关 JUnit 与 CTest 记录完整保留于 `build/t13-fourth-remediation-evidence/debug-junit.xml` 与 `release-junit.xml`，本轮无需机械重跑全量 G3；
- 定向测试 `modernime_process_runner_tests` 在 Debug 与 Release 下均已重新编译并实测 34/34 全部通过。

### 3.3 沙箱全新克隆与全生命周期验证（`verify_fresh_clone.sh`）
```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 ./tests/verify_fresh_clone.sh
```
- 全新克隆隔离构建与全量 69 项测试串行执行；
- 执行沙箱内无显示真实安装产物内部采集任务门禁（单调 5 秒总期限，会话级进程组隔离，子进程与后代精准级联回收，严格 JSON 语法与 duplicate key 拒绝，完备权限 0700/0600 及大小预算核查）；
- 卸载清理完全性验证通过；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`（总耗时 **596.67s**，退出码 0，日志见 `build/t13-fifth-remediation-evidence/fresh-clone.log`）。

### 3.4 资源泄漏、FD 与 Sanitizer 执行状态记录
- **FD 与僵尸进程状态**：**Passed**。`test_T13_R06`、`test_T13_S04` 与 `test_T13_T01` 经并发、异步终态及活 collector 退出测试，回收期间父侧 CPU 消耗极低（4.3ms），最终无未回收僵尸（`unreaped_collectors=0`），无描述符泄漏。
- **ASAN / UBSAN 状态**：
  - **父子双端全 Profile ASAN/UBSAN 覆盖**：**Passed**。
  - 在 `build/architecture-asan` 下使用 `-fsanitize=address,undefined` 同时编译 `tools/settings/modernime-settings`（采集器）与 `tests/modernime_process_runner_tests`（父侧测试二进制）；
  - SHA256 身份见 `build/t13-fifth-remediation-evidence/binary-identity.txt`；
  - 运行全量 34 项测试：**34/34 Passed**，耗时 11.2s，0 内存泄漏、0 越界访问、0 未定义行为，退出码 0（记录于 `build/t13-fifth-remediation-evidence/asan-result.json`，日志见 `asan-tests.log`）；
  - 审查方独立补跑结果确认无任何 Sanitizer 错误（见 `build/t13-fifth-review-evidence/asan-result.json` 与 `asan-tests.log`）。
- **物理桌面人工测试**：**Pending (待验)**。在无物理显示器的 WSL 环境中，GTK 单元测试通过虚拟显示与无显示安全退出模式全量验证。

---

## 4. 源码身份与证据关联

- **审查基线 HEAD**：`7c2a00aa9a5a45726735a5af93f325c2c3900d96`
- **测试快照状态**：
  - 工作树修改补丁：`build/t13-fifth-remediation-evidence/working-tree.patch`
  - 工作树状态日志：`build/t13-fifth-remediation-evidence/working-tree.txt`
  - 源码 SHA 标记：`build/t13-fifth-remediation-evidence/source-sha.txt`
  - 二进制 SHA 校验：`build/t13-fifth-remediation-evidence/binary-identity.txt`
- **门禁与测试日志**：
  - G1 Debug 日志与 JUnit：`g1-debug.log`, `g1-debug.xml` (18.54s, 5/5 Passed)
  - 安装门禁 19 组测试日志与结果：`install-gate.log`, `install-gate-results.json` (19/19 Passed, exit 0)
  - ASAN 全量 34 项测试日志与结果元数据：`asan-tests.log`, `asan-result.json`（34/34 Passed，退出码 0）
  - 沙箱全新克隆完整日志：`fresh-clone.log`
- **提交关系**：
  - 待本轮提交后将新 commit SHA 追加至 `build/t13-fifth-remediation-evidence/commit-relationship.txt`。

---

## 5. 边界与停下复审声明

- 本轮严格限定在 T13 第五轮复审收尾工作（U2-A、U2-B、P2）；
- 工作树中所有 16 份未跟踪复审文档（`docs/ARCHITECTURE_AUDIT.md`、`docs/REFACTOR_BACKLOG.md`、`docs/T11_*`、`docs/T12_*`、`docs/T13_*`、`docs/superpowers/plans/*` 等）完整保留，未执行任何 `git reset` 或 `git clean`；
- **严格停留在 T13 边界，不进入 T14，不向远程仓库推送，不发布**。
