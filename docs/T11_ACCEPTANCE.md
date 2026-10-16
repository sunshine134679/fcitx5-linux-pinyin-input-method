# ModernIME T11 架构整改终验报告（性能门禁恢复与最终复审）
*(T11 Architecture Remediation Acceptance Report - Performance Gate Restoration & Final Review)*

**验收时间**：2026-10-16  
**初始审查基线**：`2a99fe6`  
**第二轮复审基线**：`ccbdc439195ce880c08d3be7dcceb7c0aa23a715`  
**第三轮复审基线**：`12806a9dc9a6f36125c6ace6f81f509396ac55e6`  
**第四轮复审基线**：`b4de9460217c9c5695f912528f67812ad02e1d26`  
**性能门禁复审基线**：`4b1d857dcde8a585abb8949712fc604e33d115f4`  
**执行边界**：严格仅限 **T11 性能门禁恢复、负向超限拒绝验证与三轮串行复测归档**（完全在 T11 边界停下，未进入 T12，不推送、不发布）  
**验收结论**：**自动验收通过，桌面待验 (AUTOMATED ACCEPTANCE PASSED, DESKTOP PENDING)**

---

## 1. 验收概述与边界声明

按照 `docs/T11_PERFORMANCE_GATE_REVIEW.md` 第 4 节提示词要求，本轮工作聚焦于恢复实际 Release 编译目标中的性能延迟门禁，提供确定性超限负向拒绝证据，保留已有失败日志并在静默环境下串行复测三轮完整 Release 性能基准，坚持原门限与不重做生产架构原则，在 T11 边界停下等待最终复审。

- **不进入 T12 声明**：本轮不包含任何包事务所有权、`pkexec` 调度器或 T12 范围的实现，所有工作在 T11 边界停下等待复审。
- **V1–V3 状态声明**：第四轮复审指出的 V1（容量契约恢复）、V2（不可变 Writer 容量消除数据竞争）、V3（可选拼音依赖解耦与 core-only 构建）均维持**关闭**状态，本轮不重新展开生产架构重构。
- **性能门禁与宏解耦声明**：确认并纠正此前因 `tests/CMakeLists.txt` 的 `-UNDEBUG` 与代码中 `#ifdef NDEBUG` 冲突导致多项延迟检查在 Release 目标中被编译器预处理剔除的问题。引入独立宏 `MODERNIME_PERFORMANCE_GATE_ACTIVE=1`（`MODERNIME_ENFORCE_LATENCY_GATES`），实现门禁与 `NDEBUG` 彻底解耦，同时保留测试断言。
- **历史超限与原门限坚守声明**：客观承认旧日志 `performance-run-3.log:24` 中 LMDB 并发剪枝 max=34.61595ms（>30ms）及清空首键 6.29ms（>5ms）超限事实，将该日志独立保留为 `performance-run-3-historical-failure.log`；未放宽任何既定性能指标（原召回 P99 < 1ms，更新/下一召回 P99 < 5ms，并发 P99 < 5ms 且 max < 30ms），未删除或修改任何测试样本。
- **真实分类报告原则**：明确区分并通过真实日志可追溯地记录 Passed / Skipped / Blocked / NotRun 四类状态，绝不隐瞒或虚标；以实际指标和退出码作为判定依据；无物理显示服务环境下写明“自动验收通过，桌面待验”。

---

## 2. 审查基线与工作树核对

- **初始审查基线**：`2a99fe6` (`feat(core): implement LMDB shared gate and epoch reloading state (T11, P1-04)`)
- **第二轮复审基线**：`ccbdc439195ce880c08d3be7dcceb7c0aa23a715`
- **第三轮复审基线**：`12806a9dc9a6f36125c6ace6f81f509396ac55e6`
- **第四轮复审基线**：`b4de9460217c9c5695f912528f67812ad02e1d26` (`fix(t11): address third review F1-F3 and finalize T11 acceptance`)
- **性能门禁复审基线**：`4b1d857dcde8a585abb8949712fc604e33d115f4` (`docs: add review on performance gate omission and latency spike`)
- **未跟踪文档保留情况**：全部完整保留，未执行任何 `git reset --hard` 或 `git clean`：
  - `docs/ARCHITECTURE_AUDIT.md` (保留)
  - `docs/REFACTOR_BACKLOG.md` (保留)
  - `docs/T11_REVIEW_AND_HANDOFF.md` (保留)
  - `docs/T11_SECOND_REVIEW.md` (保留)
  - `docs/T11_THIRD_REVIEW.md` (保留)
  - `docs/T11_FOURTH_REVIEW.md` (保留)
  - `docs/T11_PERFORMANCE_GATE_REVIEW.md` (保留)
  - `docs/superpowers/plans/2026-10-16-architecture-remediation.md` (保留)

---

## 3. 性能门禁解耦与负向超限拒绝验证 (Performance Gate Remediation)

### 3.1 宏冲突定位与解耦整改方案

- **根因确认**：`tests/CMakeLists.txt:6` 为保留所有测试目标的断言，全局添加了 `-UNDEBUG`。Release 构建标志实际为 `-O3 -DNDEBUG -std=c++20 -UNDEBUG`，导致后置的 `-UNDEBUG` 取消了 `NDEBUG` 宏。而 `tests/core/personal_engine_benchmark.cpp` 中除 Hot-path 检查外，其余 8 项延迟关卡均被包裹在 `#ifdef NDEBUG` 内，导致这些检查在编译器预处理阶段被全部剔除。
- **解耦方案**：
  1. 在 `tests/CMakeLists.txt` 中引入 `MODERNIME_ENABLE_PERFORMANCE_GATE`（Release 与 RelWithDebInfo 模式下默认为 `ON`），为 `modernime_personal_engine_benchmark` 添加独立编译定义 `MODERNIME_PERFORMANCE_GATE_ACTIVE=1`（置于 `CXX_DEFINES` 中，完全不受 `-UNDEBUG` 标志影响）；
  2. 在 `tests/core/personal_engine_benchmark.cpp` 中定义 `MODERNIME_ENFORCE_LATENCY_GATES` 宏绑定到 `MODERNIME_PERFORMANCE_GATE_ACTIVE`，将 8 处 `#ifdef NDEBUG` 替换为 `#if MODERNIME_ENFORCE_LATENCY_GATES`；
  3. 通过对编译命令实际预处理输出（`/usr/bin/c++ -E ...`）进行全文检查，确认所有 9 项延迟错误分支（含热路径及此前缺失的 8 项关卡）已全部恢复为 **`PRESENT`**（见 `build/t11-fourth-review-evidence/release-gate-check.log`）。

### 3.2 确定性负向超限拒绝验证 (Negative Rejection Proof)

为证明实际 Release 目标在样本超限时坚决非 0 退出，而非仅仅存在判断语句：
- 在 `personal_engine_benchmark.cpp` 的实际同一阈值检查路径中，引入确定性超限注入机制（`--inject_gate_failure=<gate>` 与 `--self_test_gate_rejection`）；
- 使用 `fork()`/`waitpid()` 子进程隔离依次触发全部 8 项延迟门禁注入超限样本，验证子进程退出码严格为 `1`（`EXIT_FAILURE`）：

| 门禁标识 | 验证场景与门限 | 注入超限行为 | 实测退出状态 | 验证结果 |
|:---|:---|:---|:---:|:---:|
| `published_snapshot` | 快照召回 (<1ms) / 发布 (<5ms) | 注入 99.99ms | 退出码 1 | **PASSED** |
| `busy_writer` | 繁忙 SQLite Writer 输入端 P99 (<5ms) | 注入 99.99ms | 退出码 1 | **PASSED** |
| `concurrent_sqlite` | SQLite 并发 Writer P99 (<5ms) / max (<30ms) | 注入 max 99.99ms | 退出码 1 | **PASSED** |
| `lmdb_writer` | LMDB Writer P99 (<5ms) / max (<30ms) | 注入 max 99.99ms | 退出码 1 | **PASSED** |
| `lmdb_concurrent_pruning` | LMDB 并发写入与剪枝 P99 (<5ms) / max (<30ms) | 注入 max 99.99ms | 退出码 1 | **PASSED** |
| `lmdb_first_observer` | LMDB 首观察者只读打开与 revision (<5ms) | 注入 99.99ms | 退出码 1 | **PASSED** |
| `lmdb_first_key_after_clear` | LMDB 清空后首键提交+快照召回 (<5ms) | 注入 99.99ms | 退出码 1 | **PASSED** |
| `startup_initial_commit` | 异步启动首提交 / 下一键打字 (<5ms) | 注入 99.99ms | 退出码 1 | **PASSED** |

所有 8 项门禁的确定性负向超限日志完整记录在 `build/t11-fourth-review-evidence/negative-gates-rejection.log`，证明门禁真实有效生效。

---

## 4. 历史超限承认与静默串行复测结果

### 4.1 历史超限客观承认

- **历史超限记录**：前轮旧日志 `performance-run-3.log:24` 中出现 `LMDB concurrent write/pruning: enqueue+next recall P99=3946.21 us, maximum=34615.95 us`，max 超过 30ms 门限；同时第 27 行首键延迟为 6291.89us 超过 5ms 门限。因当时门禁宏被预处理剔除，导致当时测试未发生非 0 报错。
- **归档保留**：该失败日志已原样保存为 `build/t11-fourth-review-evidence/performance-run-3-historical-failure.log`，并在报告中明确承认，绝不掩盖或删除。
- **根因分析**：在 WSL2 虚拟化环境下，此前测试执行期间后台伴随全量编译和磁盘重 I/O 调度竞争，虚拟 CPU 线程发生短暂抢占调度延迟。

### 4.2 静默串行三轮完整实测

在停止一切并行编译与测试、系统处于完全空闲的静默环境下，使用同一 Release 可执行文件（`./build/architecture-release/tests/modernime_personal_engine_benchmark --benchmark_filter="^$"`）严格串行执行三轮完整基准，保持原门限、原样本与原容量不变：

| 指标项目 | 门限要求 | 第 1 轮实测 | 第 2 轮实测 | 第 3 轮实测 | 历史失败对照 | 判定状态 |
|:---|:---|:---:|:---:|:---:|:---:|:---:|
| **50k 内存索引精确查询 P99** | $< 1000\text{ }\mu\text{s}$ | 1.72 $\mu\text{s}$ | 1.59 $\mu\text{s}$ | 1.10 $\mu\text{s}$ | 1.79 $\mu\text{s}$ | **PASS** |
| **50k LMDB mmap 精确查询 P99** | $< 1000\text{ }\mu\text{s}$ | 2.20 $\mu\text{s}$ | 2.12 $\mu\text{s}$ | 2.49 $\mu\text{s}$ | 2.73 $\mu\text{s}$ | **PASS** |
| **50k LMDB 前缀范围查询 P99** | $< 1000\text{ }\mu\text{s}$ | 5.71 $\mu\text{s}$ | 4.03 $\mu\text{s}$ | 4.04 $\mu\text{s}$ | 4.92 $\mu\text{s}$ | **PASS** |
| **50k 上下文清洗+查询+重排 P99** | $< 1000\text{ }\mu\text{s}$ | 4.15 $\mu\text{s}$ | 3.49 $\mu\text{s}$ | 3.14 $\mu\text{s}$ | 3.48 $\mu\text{s}$ | **PASS** |
| **50k 候选召回 P99 (发布快照)** | $< 1000\text{ }\mu\text{s}$ | 181.21 $\mu\text{s}$ | 174.22 $\mu\text{s}$ | 159.79 $\mu\text{s}$ | 152.90 $\mu\text{s}$ | **PASS** |
| **200k 候选召回 P99 (发布快照)** | $< 1000\text{ }\mu\text{s}$ | 228.04 $\mu\text{s}$ | 197.49 $\mu\text{s}$ | 183.40 $\mu\text{s}$ | 186.90 $\mu\text{s}$ | **PASS** |
| **50k 更新+下一召回 P99** | $< 5000\text{ }\mu\text{s}$ | 592.78 $\mu\text{s}$ | 598.19 $\mu\text{s}$ | 474.31 $\mu\text{s}$ | 609.96 $\mu\text{s}$ | **PASS** |
| **200k 更新+下一召回 P99** | $< 5000\text{ }\mu\text{s}$ | 1468.84 $\mu\text{s}$ | 1319.32 $\mu\text{s}$ | 1476.91 $\mu\text{s}$ | 1543.55 $\mu\text{s}$ | **PASS** |
| **50k 繁忙 SQLite Writer P99** | $< 5000\text{ }\mu\text{s}$ | 854.36 $\mu\text{s}$ | 798.38 $\mu\text{s}$ | 625.91 $\mu\text{s}$ | 694.96 $\mu\text{s}$ | **PASS** |
| **50k 并发 SQLite Writer P99** | $< 5000\text{ }\mu\text{s}$ | 1097.49 $\mu\text{s}$ | 1034.05 $\mu\text{s}$ | 929.19 $\mu\text{s}$ | 818.89 $\mu\text{s}$ | **PASS** |
| **50k 并发 SQLite Writer 最大延迟 (max)** | $< 30,000\text{ }\mu\text{s}$ | **28,937.69 $\mu\text{s}$** | **21,839.12 $\mu\text{s}$** | **20,988.67 $\mu\text{s}$** | 25,515.64 $\mu\text{s}$ | **PASS** |
| **50k LMDB Writer P99 / max** | P99<5ms, max<30ms | 1031.94 / 2615.46 $\mu\text{s}$ | 996.26 / 1540.53 $\mu\text{s}$ | 816.80 / 1130.23 $\mu\text{s}$ | 1191.02 / 3757.98 $\mu\text{s}$ | **PASS** |
| **200k LMDB Writer P99 / max** | P99<5ms, max<30ms | 4787.73 / 23988.16 $\mu\text{s}$ | 2958.39 / 4699.12 $\mu\text{s}$ | 2926.08 / 7168.78 $\mu\text{s}$ | 3147.62 / 9537.83 $\mu\text{s}$ | **PASS** |
| **LMDB 并发写与剪枝 P99** | $< 5000\text{ }\mu\text{s}$ | 2233.92 $\mu\text{s}$ | 2771.59 $\mu\text{s}$ | 2651.48 $\mu\text{s}$ | 3946.21 $\mu\text{s}$ | **PASS** |
| **LMDB 并发写与剪枝 最大延迟 (max)** | $< 30,000\text{ }\mu\text{s}$ | **4,563.90 $\mu\text{s}$** | **8,507.22 $\mu\text{s}$** | **15,900.46 $\mu\text{s}$** | **34,615.95 $\mu\text{s}$ (FAIL)** | **PASS** |
| **LMDB 首观察者延迟** | $< 5000\text{ }\mu\text{s}$ | 1619.23 $\mu\text{s}$ | 1769.00 $\mu\text{s}$ | 1787.86 $\mu\text{s}$ | 1752.24 $\mu\text{s}$ | **PASS** |
| **LMDB 清空后首键提交延迟** | $< 5000\text{ }\mu\text{s}$ | 4656.25 $\mu\text{s}$ | 3312.05 $\mu\text{s}$ | 3098.63 $\mu\text{s}$ | **6291.89 $\mu\text{s}$ (FAIL)** | **PASS** |
| **启学首提交延迟 (50k / 200k)** | $< 5000\text{ }\mu\text{s}$ | 11.60 / 10.82 $\mu\text{s}$ | 12.39 / 14.11 $\mu\text{s}$ | 14.28 / 12.81 $\mu\text{s}$ | 9.21 / 9.92 $\mu\text{s}$ | **PASS** |
| **启学下一键延迟 (50k / 200k)** | $< 5000\text{ }\mu\text{s}$ | 1513.86 / 688.46 $\mu\text{s}$ | 1794.59 / 675.41 $\mu\text{s}$ | 1420.84 / 647.24 $\mu\text{s}$ | 1578.71 / 1227.09 $\mu\text{s}$ | **PASS** |
| **200k 独立后台加载 (10 轮循环)** | 监控记录 | avg 381.19 ms | avg 384.92 ms | avg 385.98 ms | avg 402.31 ms | **INFO** |
| **真实容量剪枝断言** | 50k $\to$ 40k (10,001 条) | 剪枝前 50k，后 40k | 剪枝前 50k，后 40k | 剪枝前 50k，后 40k | 剪枝前 50k，后 40k | **PASS** |
| **真实 LMDB 扩容断言** | 初始 65,536 $\to$ 扩容 | 扩容至 634,880 字节 | 扩容至 634,880 字节 | 扩容至 634,880 字节 | 扩容至 634,880 字节 | **PASS** |
| **基准运行退出码 (EXIT_CODE)** | `0` | **`EXIT_CODE=0`** | **`EXIT_CODE=0`** | **`EXIT_CODE=0`** | (门禁缺失，伪 0) | **PASS** |

三轮实测原始日志已归档至 `build/t11-fourth-review-evidence/performance-run-[1,2,3].log`，全部门禁完全激活且退出码均为 0。

---

## 5. T11-A01–A15 验收关卡矩阵完整状态

| 关卡编号 | 验收场景 | 关卡要求 | 验证证据与结果 |
|:---|:---|:---|:---|
| **T11-A01** | 读取后、发布前二次 clear | revision 单调递增、旧结果拒收、旧词零复活、新代事实保留 1 次 | `ArchitectureWriterRegression.SecondClearDuringWorkerRebaseRejectsStaleResult` 与 `ExternalClearAfterWorkerReadRejectsStalePublication`：`epoch2 > epoch1`，旧词零复活，新词仅 1 条且归属新代，**PASSED** |
| **T11-A02** | Reloading 读失败→同 revision 恢复 | 未知视图零授权、有界事实保留、恢复成功、次数准确为 1 | `ArchitectureWriterRegression.ReloadingReadFailureRetainsPendingCommitsForSubsequentRecovery`：读失败时 `PendingCommit` 保留，故障排除后 flush 恢复，独立查库验证频次精准为 1，**PASSED** |
| **T11-A03** | 永久读失败/销毁 | flush 有界失败、无忙循环/死锁、资源安全退出 | `ArchitectureWriterRegression.PermanentReadFailureFlushesCleanlyWithoutDeadlockOrFakeReady`：坏库 flush < 1000ms 返回 false，保持 Reloading，析构平稳退出，**PASSED** |
| **T11-A04** | 慢加载/故障期间持续 150+ 次提交 | 每次入队后队列 $\le 100$，溢出政策可解释，恢复可验证 | `ArchitectureWriterRegression.QueueCapacityEnforcedDuringReloadingAndStorageFailure`：暂停 Worker 并提交 150 次，队列严格限制在 100，独立查库验证仅保留最新事实（频次均为 1），前 50 条优雅丢弃，**PASSED** |
| **T11-A05** | 未先 page 刷新的旧选择/取字/删除 | 零提交、零消费、零学习；不得选择新页同索引；永久撤销 | `ArchitectureSelectionRegression.T11_A05_StalePageRejectsSelectionExtractionRemoval` 与 `ArchitectureResourceCoordinationTest.OldPageAuthorizationRevokedPermanentlyAcrossClearAndReady`：跨代与清空后选择返回 `nullopt`，取字/删除返回 `false`，重复尝试永久拒绝，**PASSED** |
| **T11-A06** | None 的选择/取字/删除/反馈/命令/标点 | 零副作用、零占位文字持久化；Enter 原文契约保留；标点不隐式上屏 | `ArchitectureSelectionRegression.T11_A06_NoneSourceFullActionRejection` 与 `T11_A06_PunctuationOptionsMatrixUnderNonePreservesComposition`：对 `CandidateSource::None` 选词/取字/删除/标点全拒，回车提交 raw preedit 且不持久化占位符，**PASSED** |
| **T11-A07** | 后台 Ready 后停止输入 | 事件循环自动更新 Controller/UI；组合/光标保持 | `ArchitectureSelectionRegression.T11_A07_ReadyNotificationRefreshesController`：使用真实 `fcitx::EventDispatcher` 与 `EventLoop`，后台就绪后调度主线程任务，无新输入自动刷新并恢复真实候选，**PASSED** |
| **T11-A08** | 上述通知后销毁/失焦/敏感/再 clear | 旧通知拒收，在途调度排空，无越权、悬空或跨会话污染 | `ArchitectureSelectionRegression.T11_A08_ReadyNotificationSafeAgainstDestroyBlurAndClear`：Barrier 1（调度前）与 Barrier 2（执行前）排空取消，二次 clear 代次作废，失焦与敏感模式过滤全部生效，无崩溃或野指针，**PASSED** |
| **T11-A09** | 正常/Reloading 提交角色对照 | 英文/Raw 英文正确，Raw 拼音/命令/敏感遵守旧政策 | `ArchitectureWriterRegression.CommitRoleMatrixParityBetweenNormalAndReloading`：偏离词、默认词、英文、裸英文、裸拼音（"jianpan"）、命令 6 项角色在正常与 Reloading 下对照 100% 吻合，经独立数据库查询确认，**PASSED** |
| **T11-A10** | 启学初始化尚未完成立即提交 | 真实异步加载；输入不 join；延迟 < 5ms；合法事实最终一次 | `ArchitectureResourceCoordinationTest.T11_A10_AsyncStartupInitialCommitDoesNotBlockInput`：非零 revision 库启动，`prepareLearningWriterAsync()` 异步加载，首提交 ~12 $\mu\text{s}$，下一键 ~1.5 $\text{ms}$，合法事实精准入库恰好 1 次，**PASSED** |
| **T11-A11** | 首次 RO/RW 同时初始化两个顺序 | RW 最终可写，共享单环境，无错误关闭 | `ArchitectureLmdbRegression.ConcurrentRoAndRwInitialization`：RO/RW 并发开启全部成功，单环境句柄共享且成功支持写入，**PASSED** |
| **T11-A12** | 真正 MAP_FULL + 读写/关闭交错 | 断言地图增长；无持共享门升级；无死锁/悬空 | `ArchitectureLmdbRegression.MapFullTriggersRealMapResize`：小地图触发真实 MDB_MAP_FULL，扩容后 map_size 从 64 KiB 翻倍至 $\ge 128$ KiB，并发读取与临时打开平稳交错，**PASSED** |
| **T11-A13** | revision 发布与读取并发 | 统一同步、TSAN 运行时无数据竞争 | `ArchitectureWriterRegression.RevisionAtomicPublicationWithWorkerRebase` 与 `ImmutableCapacityConfiguredAtConstructionIsRaceFreeUnderConcurrency`：读取线程与 Worker 并发 rebase，单调递增无死锁，TSAN 核心并发套件 0 data race，**PASSED** |
| **T11-A14** | Release 完整性能矩阵 | 门禁解耦并恢复；负向超限 8/8 证明非0退出；三轮串行复测全部达标；max < 30ms 严格满足 | `./build/architecture-release/tests/modernime_personal_engine_benchmark`：门禁完全激活；8 项负向超限自检全过；三轮完整基准 exit 0，SQLite 并发 max 20.9~28.9ms，LMDB 并发剪枝 max 4.5~15.9ms，**PASSED** |
| **T11-A15** | 完整 CTest/GTest/安装/文档 | 必需项实际注册执行，失败/跳过/未跑分别记录；真实桌面待验 | Debug CTest 68 项中 65 Passed/3 GUI Skipped；GTest 132/132 Passed；`verify_fresh_clone.sh` 100% Passed；文档与不变量矩阵已同步，**PASSED** |

---

## 6. 测试分类汇总 (Passed / Skipped / Failed / Blocked / NotRun)

### 6.1 通过项 (Passed)
1. **Release 性能基准全量门禁与串行三轮实测** (`build/architecture-release` via `modernime_personal_engine_benchmark`)：
   - 宏解耦验证：预处理文本确认全部 9 项延迟错误分支（含原热路径及恢复的 8 项关卡）100% 存在；
   - 确定性负向超限自检：8 项延迟关卡在注入超时样本时 100% 触发 `exit code 1` 拒绝退出（`build/t11-fourth-review-evidence/negative-gates-rejection.log`）；
   - 静默串行三轮完整实测：三轮基准连续执行，退出码均为 `0`，P99 与 max 延迟严格满足原门限（`build/t11-fourth-review-evidence/performance-run-[1,2,3].log`）。
2. **PersonalEngine GTest 完整单元测试** (`modernime_personal_engine_gtest`)：**132/132 100% Passed**（耗时 43.6 秒，0 失败）。
3. **LearningStore 门面独立单元测试** (`modernime_learning_store`)：**100% Passed**（耗时 8.0 秒，包含小容量边界与独立 PersonalEngine 剪枝目标对比测试）。
4. **CTest 全量功能测试套件** (`build/architecture-debug`)：**68 项测试中 65 项 Passed，3 项合法 Skipped，0 项 Failed**。
5. **ThreadSanitizer 并发与生命周期竞态检查** (`build/architecture-tsan` via `setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1`)：核心并发套件（30 项）**100% Passed**，0 数据竞争。
6. **Core-only profile 条件编译与基准验证** (`build/t11-fourth-review-core-only`)：关闭拼音模块下独立构建与基准执行 100% 通过。
7. **全新克隆全生命周期安装/卸载验证** (`./tests/verify_fresh_clone.sh`)：**100% Passed**。

### 6.2 跳过项 (Skipped)
- `modernime_settings_focus_gtk`
- `modernime_settings_shell_gtk`
- `modernime_settings_save_gtk`
- **跳过原因**：当前运行环境为无物理/虚拟 X11/Wayland 桌面显示服务器的 WSL/Linux 终端环境，上述 3 项测试在初始化 GTK 窗口失败时设计为自动跳过（`Skipped`），不影响核心算法与架构正确性。

### 6.3 失败项 (Failed)
- **0 项**（无任何失败；历史 `performance-run-3.log` 中由于调度抖动造成的 max=34.6ms 超限已归档保留为 `performance-run-3-historical-failure.log`）。

### 6.4 环境阻断项 (Blocked)
- **物理桌面端到端人工实测**：受限于 WSL/Linux 无物理桌面环境，无法直接开展人工弹窗交互测试，结论标明“**自动验收通过，桌面待验**”。核心逻辑已通过 Fcitx5 输入面板单测、引擎状态机及真实 `fcitx::EventLoop` 事件循环单测完成自动化闭环覆盖。

### 6.5 未执行项 (NotRun)
- **T12 范围项**：根据明确指示，T12（包管理所有权及 pkexec 调度）在本轮严格未执行，保留在后续阶段处理。

---

## 7. 算法演化与规范同步

1. **`docs/ALGORITHM_EVOLUTION.md`**：已同步更新 **特性 14** 与 **不变量矩阵第 29 项**，完整记录：
   - 门禁解耦：`tests/CMakeLists.txt` 引入 `MODERNIME_ENABLE_PERFORMANCE_GATE` 与 `MODERNIME_PERFORMANCE_GATE_ACTIVE=1`，解绑 `NDEBUG` 宏，恢复全部 8 项延迟检查分支；
   - 确定性负向拒绝验证：同一检查路径下 `--inject_gate_failure` 与 `--self_test_gate_rejection` 验证全部 8 关卡超限退出码 1；
   - 历史超限记录留存与静默三轮串行复测结果更新；
   - 保持 V1 容量契约、V2 不可变 Writer 容量、V3 core-only 解耦及此前全部世代/通知/旧页/标点修复。
2. **`docs/T11_ACCEPTANCE.md`**：本文件已更新为性能门禁恢复与最终复审报告。

---

## 8. 交付证据索引与当前状态

所有测试日志与原始结果均严格生成并归档于第四轮证据目录：
- 证据目录：`build/t11-fourth-review-evidence/`
  - `release-benchmark-flags.txt`：Release 基准真实编译标志（含 `MODERNIME_PERFORMANCE_GATE_ACTIVE=1`）
  - `release-gate-check.log`：实际预处理输出检查日志，确认 9/9 门禁错误分支全部 **PRESENT**
  - `negative-gates-rejection.log`：全部 8 项延迟门禁确定性注入超限样本后的负向拒绝测试日志（8/8 exit code 1）
  - `performance-run-3-historical-failure.log`：保留的历史第 3 轮调度抖动超限原始日志（max=34615.95 us）
  - `performance-run-1.log`：静默环境下 Release 性能基准第 1 轮完整日志（`EXIT_CODE=0`）
  - `performance-run-2.log`：静默环境下 Release 性能基准第 2 轮完整日志（`EXIT_CODE=0`）
  - `performance-run-3.log`：静默环境下 Release 性能基准第 3 轮完整日志（`EXIT_CODE=0`）
  - `gtest-debug.log` & `gtest-debug.xml`：132 项 GTest 运行全日志（132 Passed）与 JUnit XML
  - `ctest-debug.log`：全量 68 项 CTest 运行全日志（65 Passed, 3 Skipped, 0 Failed）
  - `tsan.log` & `config-race-tsan.log`：TSAN 核心并发套件及不可变容量并发运行日志（30 Passed，0 数据竞争）
  - `capacity-probe.cpp` & `capacity-probe.log`：SQLite 与 LMDB 独立目录容量保留上限一致性探测日志
  - `core-only-configure.log` & `core-only-build.log`：关闭拼音 profile 的独立配置、构建及基准运行日志
  - `fresh-clone.log`：沙箱隔离全新克隆全生命周期安装/卸载完整验证日志
  - `source-sha.txt`：当前基线与提交哈希记录
- **交付状态**：性能门禁完全恢复，负向超限有效性得证，静默串行三轮完整达标，V1–V3 维持关闭，全量回归与沙箱安装 100% 达标。在 T11 边界正式停下，等待最终复审；通过后下一阶段才进入原计划 T12。
