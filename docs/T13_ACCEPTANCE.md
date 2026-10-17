# T13 验收报告：有界进程采集与包政策分离（第二轮复审收尾与最终验收）

日期：2026-10-17  
审查基线：`935b1da06c5e3d1c7c303a02a409ab54d4d233be`  
前轮基线：`a34549fa677cce6a5175dcaf732f1fa73486a383`  
已验收 T12 基线：`d5f48608629110fe26fa6c4d7f9673488c16bc66`  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮根据 `docs/T13_SECOND_REVIEW.md` 第 2 至 7 节的全部复审要求，对 T13 第二轮复审中发现的 1 项 P0 缺陷（S1）、3 项 P1 缺陷（S2–S4）以及验收规范修正（S5，涵盖 T13-S01 至 T13-S07）完成闭环整改。全面保留此前轮次已生效的状态预检、token 精确引用、Query 提前 EOF 保护、环境继承与后代管道排空。

### 验收结论：T13 第二轮复审整改已满足契约（29 项场景全通，门禁与沙箱 100% 验证）

1. **S1 / P0 修复：严格零启动证明与 Uncertain 槽位保护**
   - 采集器在父进程侧预先组装全部启动参数（`cArgs`/`cArgv`），消除多线程 GUI fork 后在子进程中复杂分配的安全隐患；
   - 增加独立的 `execPipe[2]`（设置 `O_CLOEXEC`），仅在采集器 binary 真正 exec 失败时向父进程回传 `errno`。若证明目标绝对未启动，才返回 `CommandState::NotStarted`；
   - 目标可能已启动（如 fork 后、状态发布后、向握手管道写 `R` 确认字节前 collector 崩溃、异常退出或管道遭遇 EOF/超时），一律返回 `CommandState::Unknown`；
   - 控制器严格保持同 ID `Uncertain` 状态，拒绝任何后续新槽请求（`next_slot_accepted=0`），不杀包命令、不删现场目录（经隔离探针与测试 `T13-S01` 验证）。

2. **S2 / P1 修复：严格有界 Token 状态语法解析与 Collector 身份核验**
   - 实现结构化无回溯词法分析器 `JsonLexer` 与完整字段校验器 `parseStateFile`，彻底废除按子串查找 key 的宽松逻辑；
   - 严格拒绝语法错误（如缺少逗号 `not_json_with_braces`）、非法数值后缀（如 `0garbage`）、非法布尔后缀（如 `falsejunk`）、重复 key、缺失必选字段（`stdoutBytes`、`stderrBytes`、`filePeak` 等）或非预期的 state 枚举值；
   - 严格校验文件内 `collectorPid` 与注册的 `OperationRef` 身份一致。伪造或 PID 不匹配的状态文件直接拒绝，返回 `Continuing` 或 `Unknown`，禁止错误授权完成或误清目录（经隔离探针与测试 `T13-S02` 验证）。

3. **S3 / P1 修复：移除未经授权的采集硬截止与多线程僵尸主任务处理**
   - 移除 `runInternalCollector` 中未经契约授权的 `collectorDeadline` 硬截止，使采集器持续有界排空管道直到所有继承写端关闭，彻底消除子进程晚写双流触发的 `EPIPE` / `SIGPIPE`（errno 32）；
   - 移除在父侧等待期限到期后依据主线程 `/proc` 'Z'（僵尸）状态直接判定 `Exited` 的分支；到期时严格按期限返回 `Continuing`，后台继续由采集器观察完整进程组退出，最终获得准确退出码 0（经隔离探针与测试 `T13-S03` 验证）。

4. **S4 / P1 修复：异步终态发布后的后台回收责任与零僵尸保障**
   - 针对 collector 在发布最终 `state.json` 后 teardown 暂停（终态发布与进程退出合法时差）导致 `waitForOperation` 一次 `WNOHANG` 遗留僵尸的问题，引入守护式后台回收器（`enqueuePendingReap`）；
   - 区分拥有本进程直接回收责任的子进程（`isOwnedChild`）与跨进程只读 observer；
   - 业务结果可在读到有效终态后立即返回交付（`collector_alive_at_return=1`），未退出的 collector PID 登记入后台待回收队列异步轮询回收，测试结束后全局僵尸数为 0（`unreaped_collectors=0`，经隔离探针与测试 `T13-S04` 验证）。

5. **S5 验收修正与覆盖增强**
   - **R01**：使用动态解析的绝对构建产物路径，并在 Debug 与 Release 的 CTest 工作目录下实际触发初始化故障；
   - **R04**：采用已登记的有效 `OperationRef` 进入 parser，针对 S2 的三个错误夹具进行确定性拒收断言；
   - **A04/R09/S06**：在 100MiB 输出与持续大输出期间，实测捕获缓冲区裁剪前真实峰值（`res.bufferPeak`）、诊断文件写入峰值（`res.filePeak`），断言严格目录权限 `0700` 与文件权限 `0600`；
   - **安装产物无显示模式验证**：在 `tests/verify_fresh_clone.sh` 中增加对沙箱安装后 `$MODERNIME_PREFIX/bin/modernime-settings` 剥离 `DISPLAY` 与 `WAYLAND_DISPLAY` 后的 `--version` 与内部采集器调用执行验证，确保非空、非崩溃且执行返回预期。

---

## 2. 独立复审探针与行为测试关卡矩阵

### 2.1 第二轮复审探针实测结果（`build/t13-second-review-evidence/run_probes.py`）
| 探针项目 | 第二轮审查基线表现 (935b1da) | 本轮整改后实测结果 | 结论 |
|---|---|---|---|
| `actual_collector_crash_before_ack` | 返回 NotStarted，状态回到 Idle，next_slot_accepted=1 | state=Unknown, active=1, phase=Uncertain, next_slot_accepted=0, unreaped_collectors=0 | **PASS** |
| `valid_json_wrong_collector` | 接受错误 collectorPid 为 Exited/0 | state=2 (Continuing), exit=-1, 拒绝完成 | **PASS** |
| `not_json_with_braces` | 外有花括号无逗号被接受为 Exited/0 | state=2 (Continuing), exit=-1, 拒绝完成 | **PASS** |
| `invalid_numeric_and_boolean` | `0garbage`/`falsejunk` 被接受为 Exited/0 | state=2 (Continuing), exit=-1, 拒绝完成 | **PASS** |
| `collector_deadline_stops_drain` | 10分钟后停止排空，写端遭遇 EPIPE (errno 32) | state=1 (Exited), exit=0, target_write_result=OK, 0 EPIPE | **PASS** |
| `async_final_before_collector_exit` | collector 发布后暂停，waitForOperation 返回后遗留僵尸 (unreaped=1) | initial=Continuing, final=Exited, collector_alive_at_return=1, unreaped_collectors=0 | **PASS** |

### 2.2 自动化测试矩阵（`tests/settings/process_runner_test.cpp` 全部 29 项）
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
| 28 | T13-S06 | `test_T13_S06_DualStream100MiBPeakBudgetAndFilePermissions` | 双流 100MiB 持续输出裁剪前真实缓冲峰值与文件峰值断言 | **PASS** |
| 29 | T13-S07 | `test_T13_S07_FreshEvidenceAndRegistrationSelfCheck` | 全量 29 项场景执行与注册自检校验 | **PASS** |

---

## 3. 门禁验证汇总与证据记录

### 3.1 G1 定向测试套件（Debug）
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R "^(modernime_process_runner|modernime_update_controller|modernime_updates_page_contract|modernime_settings_focus_gtk|modernime_settings_save_gtk)$"
```
- `modernime_updates_page_contract`: **Passed** (0.00s)
- `modernime_settings_focus_gtk`: **Passed** (0.30s)
- `modernime_settings_save_gtk`: **Passed** (3.34s)
- `modernime_update_controller`: **Passed** (5.95s)
- `modernime_process_runner`: **Passed** (8.21s，29/29 场景全通)
- **结果**：**5/5 Passed (100%)**，耗时 17.81s。

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
ctest --test-dir build/architecture-release --output-junit /home/wsl/ModernIME/build/architecture-evidence/release-junit.xml --output-on-failure
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
- 全部 8 项指标超限样本（published_snapshot, busy_writer, concurrent_sqlite, lmdb_writer, lmdb_concurrent_pruning, lmdb_first_observer, lmdb_first_key_after_clear, startup_initial_commit）均被拒绝（exit code 1）；
- **结果**：`All 8 performance latency gates verified active and enforcing non-zero rejection on failure.`（程序返回 0）。

### 3.5 G6 GTK 界面与生命周期测试
```bash
ctest --test-dir build/architecture-debug -R "_gtk" --output-on-failure
```
- `modernime_settings_gtk_raii`: **Passed** (0.01s)
- `modernime_settings_focus_gtk`: **Passed** (0.23s)
- `modernime_settings_shell_gtk`: **Passed** (3.60s)
- `modernime_settings_save_gtk`: **Passed** (3.41s)
- **结果**：**4/4 Passed (100%)**，耗时 7.26s。

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
- 包含沙箱隔离编译、69 项测试全量执行；
- 安装产物完整性检验，且在无显示环境下实际调用 `$MODERNIME_PREFIX/bin/modernime-settings --version` 及 `--internal-capture-collector` 确认运行正常；
- 卸载清理完全性检验；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`。

### 3.8 资源泄漏、FD 与 Sanitizer 执行状态记录
- **FD 与僵尸进程状态**：**Passed**。`test_T13_R06` 与 `test_T13_S04` 经过 5 轮及多轮异步完成与回收，断言系统无新增未回收僵尸进程（`unreaped_collectors=0`），无描述符泄漏；
- **ASAN / UBSAN 状态**：**Blocked / NotRun**。当前 CMake 构建未对 `modernime-settings` 及相关 profile 启用编译期 Sanitizer 支持，且 WSL2 虚拟化环境对特定内存检测有硬件/内核限制。根据复审规范严格单列记录为 `NotRun`，不虚报为通过；
- **无头环境 GTK 状态**：在无真实物理显示器的 WSL 环境中，GTK 单元测试通过内部虚拟显示或无显示安全退出模式执行（在 G1/G3/G6 中已完成验证并通过）。

---

## 4. 源码身份与证据关联

- **审查基线 HEAD**：`935b1da06c5e3d1c7c303a02a409ab54d4d233be`
- **测试快照状态**：
  - 工作树修改补丁：`build/t13-second-review-evidence/working-tree.patch`（同步存档于 `build/architecture-evidence/working-tree.patch`）
  - 工作树状态日志：`build/t13-second-review-evidence/working-tree.txt`
  - 源码 SHA 标记：`build/t13-second-review-evidence/source-sha.txt`
- **注册表证据**：
  - Debug：`build/t13-second-review-evidence/debug-registered.json`
  - Release：`build/t13-second-review-evidence/release-registered.json`
- **新版 JUnit 执行日志**：
  - Debug JUnit：`build/t13-second-review-evidence/debug-junit.xml`（2026-10-17 13:39 生成，process_runner 耗时 8.08s）
  - Release JUnit：`build/t13-second-review-evidence/release-junit.xml`（2026-10-17 13:44 生成，process_runner 耗时 9.76s）
- **专项复审探针输出**：
  - 探针运行日志：`build/t13-second-review-evidence/probes.log`
  - 探针结果 JSON：`build/t13-second-review-evidence/probe-results.json`

---

## 5. 边界与停下复审声明

- 本轮仅严格实施 T13 第二轮复审整改收尾工作（S1–S5）；
- 工作树中所有 13 份未跟踪复审文档（`docs/ARCHITECTURE_AUDIT.md`、`docs/REFACTOR_BACKLOG.md`、`docs/T11_*`、`docs/T12_*`、`docs/T13_*`、`docs/superpowers/plans/*` 等）完整保留，未执行任何 `git reset` 或 `git clean`；
- **严格停留在 T13 边界，不进入 T14，不向远程仓库推送，不发布**。
