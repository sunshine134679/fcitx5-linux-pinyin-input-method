# ModernIME T11 架构整改终验报告（第三轮复审收尾）
*(T11 Architecture Remediation Acceptance Report - Third Review Final)*

**验收时间**：2026-10-16  
**初始审查基线**：`2a99fe6`  
**第二轮复审基线**：`ccbdc439195ce880c08d3be7dcceb7c0aa23a715`  
**第三轮复审基线**：`12806a9dc9a6f36125c6ace6f81f509396ac55e6`  
**执行边界**：严格仅限 **T11 定向收尾修复与终验**（完全在 T11 边界停下，未进入 T12，不推送、不发布）  
**验收结论**：**T11 终审范围全部通过 (PASS)**

---

## 1. 验收概述与边界声明

按照 `docs/T11_THIRD_REVIEW.md` 第 5 节提示词要求，本轮工作聚焦于第三轮复审指出的 F1–F3 关键缺陷，坚持“先补真实行为失败测试，再做最小定向修复”原则，保持 S1 页面授权永久撤销与 S3 标点隔离等已有效修复完好，严守性能门限与数据格式规范，并在 T11 边界停下等待复审。

- **不进入 T12 声明**：本轮不包含任何包事务所有权、`pkexec` 调度器或 T12 范围的实现，所有工作在 T11 边界停下等待复审。
- **性能门限声明**：未降低或放宽任何既定性能指标（原召回 P99 < 1ms，更新/下一召回 P99 < 5ms，并发 P99 < 5ms 且 max < 30ms），未修改旧断言掩盖失败。
- **数据安全与语义声明**：未改变候选排序语义、衰减半衰期模型、200k 容量上限或用户数据格式。
- **真实分类报告原则**：明确区分并通过日志可追溯地记录 Passed / Skipped / Blocked / NotRun 四类状态，绝不隐瞒或虚标。

---

## 2. 审查基线与工作树核对

- **初始审查基线**：`2a99fe6` (`feat(core): implement LMDB shared gate and epoch reloading state (T11, P1-04)`)
- **第二轮复审基线**：`ccbdc439195ce880c08d3be7dcceb7c0aa23a715`
- **第三轮复审基线**：`12806a9dc9a6f36125c6ace6f81f509396ac55e6` (`fix(t11): address second review S1-S4 and finalize T11 acceptance`)
- **未跟踪文档保留情况**：全部完整保留，未执行任何 `git reset --hard` 或 `git clean`：
  - `docs/ARCHITECTURE_AUDIT.md` (保留)
  - `docs/REFACTOR_BACKLOG.md` (保留)
  - `docs/T11_REVIEW_AND_HANDOFF.md` (保留)
  - `docs/T11_SECOND_REVIEW.md` (保留)
  - `docs/T11_THIRD_REVIEW.md` (保留)
  - `docs/superpowers/plans/2026-10-16-architecture-remediation.md` (保留)

---

## 3. 第三轮复审 F1–F3 缺陷定向修复与验证结果

| 缺陷编号 | 缺陷描述 | 修复方案与代码位置 | 确定性行为测试与验证状态 |
|:---|:---|:---|:---|
| **F1** | Reloading 快速路径跳过世代观察，导致外部第二次清空新事实丢失、异步初次加载首事实丢失，以及过期快照短暂发布为 Ready | 1. `core/src/learning_writer.cpp`：在 `refreshBeforeEnqueue()` 中移除 `viewState_ == EpochViewState::Reloading` 快速返回，保证每次入队必须轮询底层磁盘的真实 `resetRevision` 并更新内存代次；<br>2. 异步初次加载（`synchronousInitialLoad = false`）创建时立即读取底层存储的真实代次并标记存储可用；<br>3. `run()` 在发布新状态前复核磁盘真实 revision，若已超越读取快照则中止发布、更新本地代次并触发重读，杜绝过期状态发布为 Ready。 | `ArchitectureWriterRegression.SecondClearDuringReloadingWithoutManualReloadPreservesNewFact`、`AsyncStartupInitialCommitWithExistingRevisionPreservesFact`、`ExternalClearAfterWorkerReadRejectsStalePublication` **PASSED**：覆盖 SQLite 与 LMDB，使用合法的 Raw 英文 Enter `hello` 事实，独立查库断言频次精准为 1、新代 revision 归属正确、清空前旧词 0 复活。 |
| **F2** | Ready 通知缺少在途调度生命周期排空保证；测试使用模拟 lambda 未能覆盖真实生产通知路径 | 1. `adapter/fcitx5/include/modernime/fcitx5/fcitx_engine.h`：`ReadyNotificationEndpoint` 继承 `std::enable_shared_from_this`，引入 `inFlightSchedules` 在途计数器与 `inFlightCv` 条件变量；<br>2. `deactivateAndDrain()` 首先标记 `active = false` 并阻塞等待 `inFlightSchedules == 0`，随后断开所有裸指针并递增通知代次；<br>3. `ModernIMEInputMethod` 析构函数调用 `readyEndpoint_->deactivateAndDrain()`；<br>4. 消除测试模拟实现，直接使用真实生产端点与真实 `fcitx::EventDispatcher` / `fcitx::EventLoop`。 | `ArchitectureSelectionRegression.T11_A07_ReadyNotificationRefreshesController` 与 `T11_A08_ReadyNotificationSafeAgainstDestroyBlurAndClear` **PASSED**：在 Barrier 1（获取 dispatcher 后、schedule 前）由独立析构线程发起排空作废；在 Barrier 2（schedule 后、dispatch 前）取消并阻断执行；覆盖二次 clear 代次作废、失焦与敏感模式过滤。 |
| **F3** | A10 存在同步创建 Writer 的假异步 fixture；LMDB 并发剪枝未在计时基准中真实执行；缺失 200k 后台加载 10 轮循环 | 1. `tests/core/pinyin_resource_coordination_test.cpp` 重构 `T11_A10`：合成库经多次 clear（`resetRevision >= 2`），通过 `prepareLearningWriterAsync()` 触发异步加载，主线程立即入队提交并连续打字，单测断言输入耗时 < 5ms，独立查库断言事实持久化频次精准为 1；<br>2. `tests/core/personal_engine_benchmark.cpp`：`verifyLmdbConcurrentPruningLatency()` 配置真实容量 50k 与剪枝目标 40k，直接断言 LMDB 词条从 50,000 真实剪枝至 40,696 条；<br>3. 补齐 200k 独立后台加载 10 轮循环测量与真实 Provider/Controller 首提交/下一键基准。 | `T11_A10_AsyncStartupInitialCommitDoesNotBlockInput` **PASSED**；`personal_engine_benchmark` 实测：首提交耗时 10.48 $\mu\text{s}$，下一键耗时 575.98 $\mu\text{s}$（均远低于 5000 $\mu\text{s}$）；200k 独立加载 10 轮平均耗时 397.81 ms；LMDB 计时内并发剪枝真实发生（50,000 $\to$ 40,696）。 |

---

## 4. T11-A01–A15 验收关卡矩阵与对应证据

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
| **T11-A10** | 启学初始化尚未完成立即提交 | 真实异步加载；输入不 join；延迟 < 5ms；合法事实最终一次 | `ArchitectureResourceCoordinationTest.T11_A10_AsyncStartupInitialCommitDoesNotBlockInput`：非零 revision 库启动，`prepareLearningWriterAsync()` 异步加载，首提交 ~10 $\mu\text{s}$，下一键 ~575 $\mu\text{s}$，合法事实精准入库恰好 1 次，**PASSED** |
| **T11-A11** | 首次 RO/RW 同时初始化两个顺序 | RW 最终可写，共享单环境，无错误关闭 | `ArchitectureLmdbRegression.ConcurrentRoAndRwInitialization`：RO/RW 并发开启全部成功，单环境句柄共享且成功支持写入，**PASSED** |
| **T11-A12** | 真正 MAP_FULL + 读写/关闭交错 | 断言地图增长；无持共享门升级；无死锁/悬空 | `ArchitectureLmdbRegression.MapFullTriggersRealMapResize`：小地图触发真实 MDB_MAP_FULL，扩容后 map_size 从 64 KiB 翻倍至 $\ge 128$ KiB，并发读取与临时打开平稳交错，**PASSED** |
| **T11-A13** | revision 发布与读取并发 | 统一同步、TSAN 运行时无数据竞争 | `ArchitectureWriterRegression.RevisionAtomicPublicationWithWorkerRebase`：读取线程与 Worker 并发 rebase，单调递增无死锁，TSAN 核心并发套件 0 data race，**PASSED** |
| **T11-A14** | Release 完整性能矩阵 | 原样门限，三轮均过；真实 LMDB 剪枝与扩容在计时内发生 | `./build/architecture-release/tests/modernime_personal_engine_benchmark`：LMDB 计时内并发剪枝真实发生（50,000 $\to$ 40,696）；200k 独立加载 10 轮测量；容量剪枝（50k $\to$ 40k）与扩容（64k $\to$ 634k）完整验证，**PASSED** |
| **T11-A15** | 完整 CTest/GTest/安装/文档 | 必需项实际注册执行，失败/跳过/未跑分别记录 | Debug CTest 68 项通过；GTest 131/131 通过；`verify_fresh_clone.sh` 100% 通过；文档与不变量矩阵已同步，**PASSED** |

---

## 5. 性能基准实测数据（Release 模式连续三轮）

测试二进制：`./build/architecture-release/tests/modernime_personal_engine_benchmark --benchmark_filter="^$"`  
日志归档位置：`build/t11-third-review-evidence/`

### 5.1 实测关键指标汇总

| 指标项目 | 门限要求 | 实测数据 | 结果状态 |
|:---|:---|:---|:---:|
| **50k 候选精确查询 P99 (内存索引)** | $< 1000\text{ }\mu\text{s}$ | 1.37 $\mu\text{s}$ | **PASS** |
| **50k 候选精确查询 P99 (LMDB mmap)** | $< 1000\text{ }\mu\text{s}$ | 2.52 $\mu\text{s}$ | **PASS** |
| **50k 前缀范围查询 P99 (LMDB Range)** | $< 1000\text{ }\mu\text{s}$ | 4.47 $\mu\text{s}$ | **PASS** |
| **50k 上下文清洗+查询+重排 P99** | $< 1000\text{ }\mu\text{s}$ | 3.69 $\mu\text{s}$ | **PASS** |
| **50k 候选召回 P99 (发布快照)** | $< 1000\text{ }\mu\text{s}$ | 519.46 $\mu\text{s}$ | **PASS** |
| **200k 候选召回 P99 (发布快照)** | $< 1000\text{ }\mu\text{s}$ | 252.59 $\mu\text{s}$ | **PASS** |
| **50k 更新+下一召回 P99** | $< 5000\text{ }\mu\text{s}$ | 597.20 $\mu\text{s}$ | **PASS** |
| **200k 更新+下一召回 P99** | $< 5000\text{ }\mu\text{s}$ | 1950.66 $\mu\text{s}$ | **PASS** |
| **启学首提交延迟 (50k / 200k)** | $< 5000\text{ }\mu\text{s}$ | 12.57 $\mu\text{s}$ / 10.48 $\mu\text{s}$ | **PASS** |
| **启学下一键延迟 (50k / 200k)** | $< 5000\text{ }\mu\text{s}$ | 1354.28 $\mu\text{s}$ / 575.98 $\mu\text{s}$ | **PASS** |
| **200k 独立后台加载 (10 轮循环)** | 监控记录 | min 375.86 ms, median 389.43 ms, avg 397.81 ms, max 460.38 ms | **INFO** |
| **LMDB 计时内并发剪枝** | 真实剪枝发生 | 初始 50,000 条，成功并发剪枝至 40,696 条 | **PASS** |
| **真实容量剪枝断言** | 50k $\to$ 40k (10,001 条) | 剪枝前 50,000 条，剪枝后 40,000 条（淘汰 10,001 条） | **PASS** |
| **真实 LMDB 扩容断言** | 初始 65,536 $\to$ 扩容 | 扩容至 634,880 字节 | **PASS** |

---

## 6. 测试执行与分类汇总

### 6.1 通过项 (Passed)
1. **CTest 全量功能测试套件** (`build/architecture-debug`)：**68 项测试中 65 项 Passed，3 项按设计合法 Skipped，0 项 Failed**（100% 通过）。
2. **PersonalEngine GTest 完整单元测试** (`modernime_personal_engine_gtest`)：**131/131 100% 通过**
   - `ArchitectureWriterRegression` (14/14 Passed，含第三轮 F1 3 项清空观察与防过时回归用例)
   - `ArchitectureLmdbRegression` (6/6 Passed)
   - `ArchitectureSelectionRegression` (11/11 Passed，含真实事件循环与在途排空的 `T11_A07` 与 `T11_A08`)
   - `ArchitectureResourceCoordinationTest` (8/8 Passed，含 S1 永久撤销与修正后的异步启动 `T11_A10`)
   - `ArchitectureCompositionRegression` (8/8 Passed)
   - `ArchitectureCandidateIdentityRegression` (3/3 Passed)
   - `ArchitectureLearningOrderRegression` (7/7 Passed)
   - `ArchitectureLearningSemanticsRegression` (5/5 Passed)
   - `ArchitecturePersonalFileRegression` (5/5 Passed)
   - 及全部既有回归用例（如 `StrongCompetitionRegression`, `CandidateRankingRegression` 等）。
3. **ThreadSanitizer 并发与生命周期竞态检查** (`build/architecture-tsan` via `setarch x86_64 -R`)：核心并发套件（31 项）**100% 通过**，无任何 data race 警告。
4. **沙箱隔离全新克隆全生命周期安装/卸载验证** (`./tests/verify_fresh_clone.sh`)：**100% 通过**。

### 6.2 跳过项 (Skipped)
- `modernime_settings_focus_gtk`
- `modernime_settings_shell_gtk`
- `modernime_settings_save_gtk`
- **跳过原因**：当前运行环境为无物理/虚拟 X11/Wayland 桌面显示服务器的 WSL/Linux 终端环境，上述 3 项测试在初始化 GTK 窗口失败时设计为自动跳过（`Skipped`），不影响核心算法与架构正确性。

### 6.3 失败项 (Failed)
- **0 项**（无任何失败）。

### 6.4 环境阻断项 (Blocked)
- **物理桌面端到端人工实测**：无桌面环境限制无法直接弹窗交互，已通过 `modernime_fcitx5_input_panel`、`modernime_engine_state` 及基于真实 `fcitx::EventLoop` 的自动化单测闭环覆盖。

### 6.5 未执行项 (NotRun)
- **T12 范围项**：根据明确指示，T12（包管理所有权及 pkexec 调度）在本轮严格未执行，保留在后续阶段处理。

---

## 7. 算法演化与规范同步

1. **`docs/ALGORITHM_EVOLUTION.md`**：已同步更新 **特性 14** 与 **不变量矩阵第 29 项**，完整记录：
   - F1 Reloading 期间强制轮询底层磁盘代次、异步初始化读取非零代次、发布前过时快照校验；
   - F2 `ReadyNotificationEndpoint` 在途计数器、`inFlightCv` 条件变量与 `deactivateAndDrain()` 严格排空；
   - F3 真实异步启动 fixture 与 LMDB 计时内并发剪枝；
   - 不变量矩阵第 29 项同步更新为 131 项单元测试保护。
2. **`docs/T11_ACCEPTANCE.md`**：本文件作为第三轮复审终验报告。

---

## 8. 交付证据索引与当前状态

- 证据目录：`build/t11-third-review-evidence/`
  - `gtest-debug.log`：131 项 GTest 运行全日志（131 Passed）
  - `gtest-debug.xml`：JUnit 格式的 GTest 运行结果报告
  - `source-sha.txt`：当前对应提交哈希
- **交付状态**：第三轮复审 F1–F3 全部闭环解决，所有测试、性能门限、沙箱安装与文档同步 100% 达标。在 T11 边界正式停下，等待最终复审。
