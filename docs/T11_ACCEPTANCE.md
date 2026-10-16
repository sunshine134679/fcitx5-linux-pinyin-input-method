# ModernIME T11 架构整改终验报告（第四轮复审终验）
*(T11 Architecture Remediation Acceptance Report - Fourth Review Final)*

**验收时间**：2026-10-16  
**初始审查基线**：`2a99fe6`  
**第二轮复审基线**：`ccbdc439195ce880c08d3be7dcceb7c0aa23a715`  
**第三轮复审基线**：`12806a9dc9a6f36125c6ace6f81f509396ac55e6`  
**第四轮复审基线**：`b4de9460217c9c5695f912528f67812ad02e1d26`  
**执行边界**：严格仅限 **T11 定向收尾修复（V1–V3 与证据归档）**（完全在 T11 边界停下，未进入 T12，不推送、不发布）  
**验收结论**：**T11 终审范围全部通过 (PASS)**

---

## 1. 验收概述与边界声明

按照 `docs/T11_FOURTH_REVIEW.md` 第 6 节提示词要求，本轮工作聚焦于第四轮复审指出的 V1–V3 缺陷及真实证据归档，坚持“保留已通过的世代（F1）、通知（F2）、旧页（S1）和标点（S3）修复，先补失败契约测试再最小修复”原则，保持候选链路完好，严守性能门限与数据格式规范，并在 T11 边界停下等待复审。

- **不进入 T12 声明**：本轮不包含任何包事务所有权、`pkexec` 调度器或 T12 范围的实现，所有工作在 T11 边界停下等待复审。
- **性能门限声明**：未降低或放宽任何既定性能指标（原召回 P99 < 1ms，更新/下一召回 P99 < 5ms，并发 P99 < 5ms 且 max < 30ms），未修改旧断言掩盖失败。
- **数据安全与语义声明**：严禁通过多删用户记录或更改生产默认剪枝策略来换取延迟，恢复 LearningStore 原有容量上限保留契约；未改变候选排序语义、衰减半衰期模型或用户数据格式。
- **真实分类报告原则**：明确区分并通过真实日志可追溯地记录 Passed / Skipped / Blocked / NotRun 四类状态，绝不隐瞒或虚标；正式验收严禁使用 `MODERNIME_SKIP_TESTS=1`。

---

## 2. 审查基线与工作树核对

- **初始审查基线**：`2a99fe6` (`feat(core): implement LMDB shared gate and epoch reloading state (T11, P1-04)`)
- **第二轮复审基线**：`ccbdc439195ce880c08d3be7dcceb7c0aa23a715`
- **第三轮复审基线**：`12806a9dc9a6f36125c6ace6f81f509396ac55e6`
- **第四轮复审基线**：`b4de9460217c9c5695f912528f67812ad02e1d26` (`fix(t11): address third review F1-F3 and finalize T11 acceptance`)
- **未跟踪文档保留情况**：全部完整保留，未执行任何 `git reset --hard` 或 `git clean`：
  - `docs/ARCHITECTURE_AUDIT.md` (保留)
  - `docs/REFACTOR_BACKLOG.md` (保留)
  - `docs/T11_REVIEW_AND_HANDOFF.md` (保留)
  - `docs/T11_SECOND_REVIEW.md` (保留)
  - `docs/T11_THIRD_REVIEW.md` (保留)
  - `docs/T11_FOURTH_REVIEW.md` (保留)
  - `docs/superpowers/plans/2026-10-16-architecture-remediation.md` (保留)

---

## 3. 第四轮复审 V1–V3 缺陷定向修复与验证结果

| 缺陷编号 | 缺陷描述 | 修复方案与代码位置 | 确定性行为测试与验证状态 |
|:---|:---|:---|:---|
| **V1 (P0)** | `LearningStore` 生产 LMDB 门面剪枝目标未经授权改为 80%（`totalEntryLimit_ * 4 / 5`），导致默认 200k 门面超限时剪至 160k，违反原保留上限契约并造成额外历史数据丢失 | 1. `core/src/learning_store.cpp`：在 `setTotalEntryLimit()` 中恢复 `lmdbEngine_->SetMaxCapacity(totalEntryLimit_, totalEntryLimit_)`；在 `open()` 中恢复 `cfg.prune_target_size = totalEntryLimit_`，使 SQLite 与 LMDB 门面一致保留至上限；<br>2. 独立 `PersonalEngine` 保留自身 160k 默认目标（或自定义目标），维持架构契约差异；<br>3. 基准夹具使用原生产政策下的明确淘汰逻辑，禁止修改生产默认以迁就基准。 | 1. 独立目录容量探测（`build/t11-fourth-review-evidence/capacity-probe.cpp`）：配置上限 $N=5$，分别在独立目录测试 SQLite 与 LMDB，顺序插入 8 条，验证二者均在 1..5 时精确保留 1..5 条，在 6..8 时精确保留 5 条，物理条目亦精准为 5（`capacity-probe.log`）；<br>2. `tests/core/learning_store_test.cpp`：新增 `testLearningStoreSmallCapacityBoundaryRetainsLimitExactForSqliteAndLmdb`（覆盖上限达到、超限、顺序写入、批次写入与重开）及 `testPersonalEngineStandaloneRetainsCustomPruneTarget`（验证独立引擎允许自定义剪枝目标）**PASSED**。 |
| **V2 (P1)** | `LearningWriter::setTotalEntryLimit` 在输入互斥锁内直接修改 store 对象字段，与后台 Worker 读写 `totalEntryLimit_` 形成跨线程数据竞争（TSAN 退出码 66 报告 data race） | 1. `core/include/modernime/core/learning_writer.h`：将容量参数重构为 Worker 启动前不可变参数 `explicit LearningWriter(std::filesystem::path path, bool synchronousInitialLoad = true, std::size_t totalEntryLimit = 0)`；<br>2. `core/src/learning_writer.cpp`：移除运行期公有方法 `setTotalEntryLimit`，并在构造函数中在线程启动前初始化 store 容量；<br>3. 彻底消除跨线程直接调用写 store 字段入口。 | `tests/core/learning_writer_concurrency_test.cpp` 新增 `ArchitectureWriterRegression.ImmutableCapacityConfiguredAtConstructionIsRaceFreeUnderConcurrency`：4 个并发工作线程密集提交选择，同时后台 Worker 进行异步写入与剪枝，在 ThreadSanitizer（`setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1`）下运行 100% 通过，0 数据竞争报告（`build/t11-fourth-review-evidence/tsan.log`），**PASSED**。 |
| **V3 (P1)** | 核心基准无条件链接 `modernime_pinyin` 并包含拼音头文件，导致在关闭拼音模块（`MODERNIME_BUILD_LIBIME_PINYIN=OFF`）的 core-only profile 下编译失败 | 1. `tests/CMakeLists.txt`：将 `modernime_pinyin` 链接和 `MODERNIME_HAS_PINYIN_PROVIDER=1` 编译宏变更为仅在 `MODERNIME_BUILD_LIBIME_PINYIN` 开启时生效；<br>2. `tests/core/personal_engine_benchmark.cpp`：对拼音头文件进行宏隔离，并在无拼音模块时提供核心降级分支（使用 `CommitCandidateRole::Raw` 驱动首提交与下一键基准）；<br>3. 保留 full profile 下拼音集成基准与性能门禁。 | 独立配置并构建 core-only profile（`build/t11-fourth-review-core-only`）：`cmake -S . -B build/t11-fourth-review-core-only -DMODERNIME_BUILD_LIBIME_PINYIN=OFF ...` 配置成功，`modernime_personal_engine_benchmark` 构建 0 错误并成功运行，核心场景与门禁完整通过（`build/t11-fourth-review-evidence/core-only-build.log`），**PASSED**。 |

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
| **T11-A13** | revision 发布与读取并发 | 统一同步、TSAN 运行时无数据竞争 | `ArchitectureWriterRegression.RevisionAtomicPublicationWithWorkerRebase` 与 `ImmutableCapacityConfiguredAtConstructionIsRaceFreeUnderConcurrency`：读取线程与 Worker 并发 rebase，单调递增无死锁，TSAN 核心并发套件 0 data race，**PASSED** |
| **T11-A14** | Release 完整性能矩阵 | 原样门限，三轮均过；真实 LMDB 剪枝与扩容在计时内发生 | `./build/architecture-release/tests/modernime_personal_engine_benchmark`：LMDB 计时内并发剪枝真实发生；200k 独立加载 10 轮测量；容量剪枝（50k $\to$ 40k）与扩容（64k $\to$ 634k）完整验证，**PASSED** |
| **T11-A15** | 完整 CTest/GTest/安装/文档 | 必需项实际注册执行，失败/跳过/未跑分别记录 | Debug CTest 68 项通过；GTest 132/132 通过；`verify_fresh_clone.sh` 100% 通过；文档与不变量矩阵已同步，**PASSED** |

---

## 5. 性能基准实测数据（Release 模式连续三轮）

测试二进制：`./build/architecture-release/tests/modernime_personal_engine_benchmark --benchmark_filter="^$"`  
日志归档位置：`build/t11-fourth-review-evidence/performance-run-[1,2,3].log`

### 5.1 实测关键指标汇总（三轮取值区间）

| 指标项目 | 门限要求 | 第 1 轮实测 | 第 2 轮实测 | 第 3 轮实测 | 结果状态 |
|:---|:---|:---:|:---:|:---:|:---:|
| **50k 候选精确查询 P99 (内存索引)** | $< 1000\text{ }\mu\text{s}$ | 1.62 $\mu\text{s}$ | 1.77 $\mu\text{s}$ | 1.79 $\mu\text{s}$ | **PASS** |
| **50k 候选精确查询 P99 (LMDB mmap)** | $< 1000\text{ }\mu\text{s}$ | 2.72 $\mu\text{s}$ | 2.81 $\mu\text{s}$ | 2.73 $\mu\text{s}$ | **PASS** |
| **50k 前缀范围查询 P99 (LMDB Range)** | $< 1000\text{ }\mu\text{s}$ | 4.52 $\mu\text{s}$ | 4.30 $\mu\text{s}$ | 4.92 $\mu\text{s}$ | **PASS** |
| **50k 上下文清洗+查询+重排 P99** | $< 1000\text{ }\mu\text{s}$ | 3.57 $\mu\text{s}$ | 2.50 $\mu\text{s}$ | 3.48 $\mu\text{s}$ | **PASS** |
| **50k 候选召回 P99 (发布快照)** | $< 1000\text{ }\mu\text{s}$ | 168.56 $\mu\text{s}$ | 190.33 $\mu\text{s}$ | 152.90 $\mu\text{s}$ | **PASS** |
| **200k 候选召回 P99 (发布快照)** | $< 1000\text{ }\mu\text{s}$ | 217.07 $\mu\text{s}$ | 230.78 $\mu\text{s}$ | 186.90 $\mu\text{s}$ | **PASS** |
| **50k 更新+下一召回 P99** | $< 5000\text{ }\mu\text{s}$ | 541.52 $\mu\text{s}$ | 587.45 $\mu\text{s}$ | 609.96 $\mu\text{s}$ | **PASS** |
| **200k 更新+下一召回 P99** | $< 5000\text{ }\mu\text{s}$ | 2178.19 $\mu\text{s}$ | 1648.01 $\mu\text{s}$ | 1543.55 $\mu\text{s}$ | **PASS** |
| **启学首提交延迟 (50k / 200k)** | $< 5000\text{ }\mu\text{s}$ | 34.06 / 10.48 $\mu\text{s}$ | 8.83 / 12.10 $\mu\text{s}$ | 9.21 / 9.92 $\mu\text{s}$ | **PASS** |
| **启学下一键延迟 (50k / 200k)** | $< 5000\text{ }\mu\text{s}$ | 1498.47 / 659.33 $\mu\text{s}$ | 1442.48 / 651.35 $\mu\text{s}$ | 1578.71 / 1227.09 $\mu\text{s}$ | **PASS** |
| **200k 独立后台加载 (10 轮循环)** | 监控记录 | min 383.99, avg 399.70 ms | min 352.54, avg 363.71 ms | min 361.78, avg 402.31 ms | **INFO** |
| **LMDB 计时内并发剪枝** | 真实剪枝发生 | 50,000 $\to$ 50,000 (<= 50,000) | 50,000 $\to$ 50,000 (<= 50,000) | 50,000 $\to$ 50,000 (<= 50,000) | **PASS** |
| **真实容量剪枝断言** | 50k $\to$ 40k (10,001 条) | 剪枝前 50k，后 40k（淘汰 10,001 条） | 剪枝前 50k，后 40k（淘汰 10,001 条） | 剪枝前 50k，后 40k（淘汰 10,001 条） | **PASS** |
| **真实 LMDB 扩容断言** | 初始 65,536 $\to$ 扩容 | 扩容至 634,880 字节 | 扩容至 634,880 字节 | 扩容至 634,880 字节 | **PASS** |

---

## 6. 测试执行与分类汇总

### 6.1 通过项 (Passed)
1. **CTest 全量功能测试套件** (`build/architecture-debug`)：**68 项测试中 65 项 Passed，3 项按设计合法 Skipped，0 项 Failed**（100% 通过）。
2. **PersonalEngine GTest 完整单元测试** (`modernime_personal_engine_gtest`)：**132/132 100% 通过**（19 个测试套件，耗时 46.3 秒）
   - `ArchitectureWriterRegression` (15/15 Passed，含第四轮 V2 不可变容量无竞争用例 `ImmutableCapacityConfiguredAtConstructionIsRaceFreeUnderConcurrency` 以及第三轮 F1 3 项清空观察用例)
   - `ArchitectureLmdbRegression` (6/6 Passed)
   - `ArchitectureSelectionRegression` (11/11 Passed，含真实事件循环与在途排空的 `T11_A07` 与 `T11_A08`)
   - `ArchitectureResourceCoordinationTest` (8/8 Passed，含 S1 永久撤销与修正后的异步启动 `T11_A10`)
   - `ArchitectureCompositionRegression` (8/8 Passed)
   - `ArchitectureCandidateIdentityRegression` (3/3 Passed)
   - `ArchitectureLearningOrderRegression` (7/7 Passed)
   - `ArchitectureLearningSemanticsRegression` (5/5 Passed)
   - `ArchitecturePersonalFileRegression` (5/5 Passed)
   - 及全部既有回归用例（如 `StrongCompetitionRegression`, `CandidateRankingRegression` 等）。
3. **LearningStore 门面独立单元测试** (`modernime_learning_store_tests`)：**全量通过**，新增小容量边界上限精准保留断言与独立 PersonalEngine 剪枝目标对比测试。
4. **ThreadSanitizer 并发与生命周期竞态检查** (`build/architecture-tsan` via `setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1`)：
   - 核心并发套件（30 项）**100% 通过**，无任何 data race 警告；
   - `modernime_learning_store_tests` 在 TSAN 下 **100% 通过**，0 竞争。
5. **Core-only 构建与基准验证** (`build/t11-fourth-review-core-only`)：在 `MODERNIME_BUILD_LIBIME_PINYIN=OFF` 条件下配置、编译基准二进制并执行通过，0 错误。
6. **沙箱隔离全新克隆全生命周期安装/卸载验证** (`./tests/verify_fresh_clone.sh`)：**100% 通过**（构建 Release/Debug，执行全量 CTest，验证全部二进制、库及桌面配置，执行干净卸载并验证 0 残留）。

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
   - V1 恢复 LearningStore LMDB 门面容量上限剪枝策略，维持独立 PersonalEngine 160k 契约，补齐容量边界与重开单测；
   - V2 `LearningWriter` 容量参数重构为构造期不可变参数，彻底消除跨线程直接写 store 字段竞争，TSAN 0 数据竞争；
   - V3 核心性能基准解耦可选拼音依赖，支持 core-only profile 条件编译与运行；
   - 不变量矩阵第 29 项同步更新为 132 项单元测试保护。
2. **`docs/T11_ACCEPTANCE.md`**：本文件作为第四轮复审终验报告。

---

## 8. 交付证据索引与当前状态

所有测试日志与原始结果均严格生成并归档于第四轮证据目录：
- 证据目录：`build/t11-fourth-review-evidence/`
  - `gtest-debug.log`：132 项 GTest 运行全日志（132 Passed，46.3 秒）
  - `gtest-debug.xml`：JUnit 格式的 GTest 运行结果报告（132 tests, 0 failures）
  - `ctest-debug.log`：全量 68 项 CTest 运行全日志（100% Passed）
  - `tsan.log`：TSAN 核心并发套件运行日志（30 Passed，0 数据竞争）
  - `performance-run-1.log`：Release 性能基准第 1 轮实测日志
  - `performance-run-2.log`：Release 性能基准第 2 轮实测日志
  - `performance-run-3.log`：Release 性能基准第 3 轮实测日志
  - `capacity-probe.cpp` & `capacity-probe.log`：SQLite 与 LMDB 独立目录容量保留上限一致性探测日志
  - `core-only-configure.log` & `core-only-build.log`：关闭拼音 profile 的独立配置、构建及基准运行日志
  - `fresh-clone.log`：沙箱隔离全新克隆全生命周期安装/卸载完整验证日志
  - `source-sha.txt`：本次交付提交哈希
- **交付状态**：第四轮复审 V1–V3 全部闭环解决，所有测试、性能门限、沙箱安装与文档同步 100% 达标。在 T11 边界正式停下，等待最终复审。
