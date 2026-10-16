# ModernIME T11 架构整改验收报告
*(T11 Architecture Remediation Acceptance Report)*

**验收时间**：2026-10-16  
**审查基线**：`2a99fe6`  
**执行边界**：严格仅限 **T11 收尾修复与验收**（完全在 T11 边界停下，未进入 T12）  
**验收结论**：**全部通过 (PASS)**

---

## 1. 验收概述与边界声明

按照 `docs/T11_REVIEW_AND_HANDOFF.md` 第 6 节要求，本轮工作聚焦于解决 T11 遗留的 R1–R9 架构缺陷，补齐确定性失败测试，修复核心竞争与生命周期问题，并逐项对照关卡 T11-A01 至 T11-A15 进行严格验收。

- **不进入 T12 声明**：本轮不包含任何包事务所有权、`pkexec` 调度或 T12 范围的实现，所有工作在 T11 边界停下等待复审。
- **性能门限声明**：未降低或放宽任何既定性能指标（原召回 P99 < 1ms，更新/下一召回 P99 < 5ms，并发 P99 < 5ms 且 max < 30ms），未修改既有测试断言以掩盖失败。
- **数据安全与语义声明**：未改变候选排序语义、衰减半衰期模型或用户数据格式。

---

## 2. 审查基线与工作树核对

- **审查基准 Commit**：`2a99fe6` (`feat(core): implement LMDB shared gate and epoch reloading state (T11, P1-04)`)
- **未跟踪文档保留情况**：全部完整保留，未执行任何 `git reset --hard` 或 `git clean`：
  - `docs/ARCHITECTURE_AUDIT.md` (保留)
  - `docs/REFACTOR_BACKLOG.md` (保留)
  - `docs/T11_REVIEW_AND_HANDOFF.md` (保留)
  - `docs/superpowers/plans/2026-10-16-architecture-remediation.md` (保留)

---

## 3. R1–R9 缺陷逐项处理与验证结果

| 缺陷编号 | 缺陷描述 | 修复方案与代码位置 | 确定性失败测试与验证状态 |
|:---|:---|:---|:---|
| **R1** | `flush()` 死锁与重载失败仍伪就绪 | 修复 `core/src/learning_writer.cpp`：`flush()` 等待条件补充 `(viewState_ == Ready \|\| retryExhausted_)`，彻底打破死锁；重载读取失败时保持 `Reloading` 状态，不伪造 `Ready`；停止循环在重试耗尽时立即退出。 | `PermanentReadFailureFlushesCleanlyWithoutDeadlockOrFakeReady` **PASSED** |
| **R2** | 重试失败后 `PendingCommit` 丢失与二次 clear 脏覆盖 | `core/src/learning_writer.cpp`：重载失败保留 `PendingCommit` 事件供后续恢复；Worker 重载准备完成后校验 `freshState->resetRevision < resetRevision_`，丢弃过时 rebase 结果并触发再次唤醒。 | `ReloadingReadFailureRetainsPendingCommitsForSubsequentRecovery`、`SecondClearDuringWorkerRebaseRejectsStaleResult` **PASSED** |
| **R3** | 候选占位、跨代选词与不可达分支 | `algorithm/pinyin/src/pinyin_candidate_provider.cpp` & `adapter/fcitx5/src/engine.cpp`：`Reloading` 时候选列表展示 `CandidateSource::None`（"正在更新候选"）；`select`、`extractCharacter`、`removeCurrent`、`commitCurrent` 全面拒绝 `None` 占位符与跨代请求；标点/回车提交占位符时回退为提交原始 preedit，杜绝文字污染。 | `T11_A05_StalePageRejectsSelectionExtractionRemoval`、`T11_A06_NoneSourceFullActionRejection` **PASSED** |
| **R4** | LMDB 升级与生命周期门存在双初始化/共享泄露 | `core/src/lmdb_environment.cpp` & `core/src/personal_engine.cpp`：在 `LmdbRegistry::getOrCreate` 中使用 `std::unique_lock` 对 `handle->lifecycle_gate` 加锁并原子判定 `need_init` 与 `need_upgrade`；`OpenLmdb` 使用 `std::shared_lock` 复制环境句柄，移除所有锁外裸指针判断。 | `ConcurrentRoAndRwInitialization`、`MapFullTriggersRealMapResize` **PASSED (Debug & TSAN)** |
| **R5** | `Reloading` 队列在加载阶段无界 | `core/src/learning_writer.cpp`：`enqueueForPersistence` 在 `!durablyAvailable \|\| viewState_ == Reloading` 时强制执行队列上限（$\le 100$），优先淘汰早先事件，保留最新用户事实。 | `QueueCapacityEnforcedDuringReloadingAndStorageFailure` **PASSED** |
| **R6** | `Ready` 只由 page 懒观察，真实 UI 不会自动恢复 | `algorithm/pinyin/src/pinyin_shared_resources.cpp` & `adapter/fcitx5/src/inputmethod.cpp`：`PinyinSharedResources` 增加 `setOnReadyCallback`；`ModernIMEInputMethod` 挂载 `EventDispatcher`，Worker 线程进入 `Ready` 后主动向 Fcitx5 事件循环调度主线程刷新，调用 `controller_->refreshPage()` 恢复真实候选；通过 `alive_` 共享生命周期标记防止析构/失焦悬空崩溃。 | `T11_A07_ReadyNotificationRefreshesController`、`T11_A08_ReadyNotificationSafeAgainstDestroyBlurAndClear` **PASSED** |
| **R7** | 异步启学后的首提交仍 join 全库构造 | `algorithm/pinyin/src/pinyin_shared_resources.cpp` & `pinyin_candidate_provider.cpp`：`ensureLearningWriter(bool synchronousInitialLoad = true)`；打字主线程中触发的 `learningWriter()` 采用异步参数（`synchronousInitialLoad=false`），不阻塞主线程打字与上屏；世代与版本采用原子变量（`std::atomic`）保护。 | `T11_A10_AsyncStartupInitialCommitDoesNotBlockInput` **PASSED (<5ms)** |
| **R8** | `Reloading` 额外忽略全部 English/Raw | `core/src/learning_writer.cpp`：`enqueueCommit` 在 `Reloading` 期间与正常状态统一：仅跳过 `Command`，对合法拼音裸串（`Raw`）及英文词暂存为 `PendingCommit`，在重载完成后统一决策。 | `CommitRoleMatrixParityBetweenNormalAndReloading` **PASSED** |
| **R9** | 分支重构引发的车库翻译回归 (Test #37) | `algorithm/pinyin/src/candidate_composition.cpp` & `pinyin_candidate_provider.cpp`：恢复车库候选（garage）释义动态查询逻辑，解决静态分词对特殊英文单词的释义截断。 | CTest #37 (`modernime_pinyin_provider`) **PASSED (100%)** |

---

## 4. T11-A01–A15 验收关卡矩阵与证据

| 关卡编号 | 验收场景 | 关卡要求 | 验证证据与结果 |
|:---|:---|:---|:---|
| **T11-A01** | 读取后、发布前二次 clear | revision 单调递增、旧结果拒收、旧词零复活、新代事实保留 1 次 | `ArchitectureWriterRegression.SecondClearDuringWorkerRebaseRejectsStaleResult`：`epoch2 > epoch1`，旧词“北京”零复活，新词“广州”仅 1 条且在 epoch2，**PASSED** |
| **T11-A02** | Reloading 读失败→同 revision 恢复 | 未知视图零授权、有界事实保留、恢复成功、次数准确 | `ArchitectureWriterRegression.ReloadingReadFailureRetainsPendingCommitsForSubsequentRecovery`：读失败时 `PendingCommit` 保留，故障排除后 flush 恢复，“上海”频次准确为 1，**PASSED** |
| **T11-A03** | 永久读失败/销毁 | flush 有界失败、无忙循环/死锁、资源安全退出 | `ArchitectureWriterRegression.PermanentReadFailureFlushesCleanlyWithoutDeadlockOrFakeReady`：flush 返回 false 且不超时，`epochViewState` 保持 Reloading，析构平稳退出，**PASSED** |
| **T11-A04** | 慢加载/故障期间持续 150+ 次提交 | 每次入队后队列 $\le 100$，溢出政策可解释，恢复可验证 | `ArchitectureWriterRegression.QueueCapacityEnforcedDuringReloadingAndStorageFailure`：暂停 Worker 并提交 150 次，队列精确稳定在 100，恢复后只保留最新 100 条，**PASSED** |
| **T11-A05** | 未先 page 刷新的旧选择/取字/删除 | 零提交、零消费、零学习；不得选择新页同索引 | `ArchitectureSelectionRegression.T11_A05_StalePageRejectsSelectionExtractionRemoval`：跨代选择返回 `nullopt`，取字/删除返回 `false`，候选世代失效，**PASSED** |
| **T11-A06** | None 的选择/取字/删除/反馈/命令 | 零副作用、零占位文字持久化；Enter 原文契约保留 | `ArchitectureSelectionRegression.T11_A06_NoneSourceFullActionRejection`：对 `CandidateSource::None` 选词/取字/删除全拒，回车提交 raw preedit 且不持久化占位符，**PASSED** |
| **T11-A07** | 后台 Ready 后停止输入 | 事件循环自动更新 Controller/UI；组合/光标保持 | `ArchitectureSelectionRegression.T11_A07_ReadyNotificationRefreshesController`：后台释放就绪栅栏后，模拟 Fcitx5 事件循环分发，自动刷新并恢复真实候选，**PASSED** |
| **T11-A08** | 上述通知后销毁/失焦/敏感/再 clear | 旧通知拒收，无越权、悬空或跨会话污染 | `ArchitectureSelectionRegression.T11_A08_ReadyNotificationSafeAgainstDestroyBlurAndClear`：销毁、失焦、敏感模式及二次 clear 场景下旧通知安全作废，无非法回调，**PASSED** |
| **T11-A09** | 正常/Reloading 提交角色对照 | 英文/Raw 英文正确，Raw 拼音/命令/敏感遵守旧政策 | `ArchitectureWriterRegression.CommitRoleMatrixParityBetweenNormalAndReloading`：Chinese/English/Raw 拼音对照矩阵 100% 吻合，**PASSED** |
| **T11-A10** | 启学初始化尚未完成立即提交 | 输入不 join；合法事实最终一次；禁学切回无副作用 | `ArchitectureResourceCoordinationTest.T11_A10_AsyncStartupInitialCommitDoesNotBlockInput`：首提交耗时 < 5ms，合法事实准确入库，**PASSED** |
| **T11-A11** | 首次 RO/RW 同时初始化两个顺序 | RW 最终可写，共享单环境，无错误关闭 | `ArchitectureLmdbRegression.ConcurrentRoAndRwInitialization`：RO/RW 并发开启全部成功，单环境句柄共享且成功支持写入，**PASSED** |
| **T11-A12** | 真正 MAP_FULL + 读写/关闭交错 | 断言地图增长；无持共享门升级；无死锁/悬空 | `ArchitectureLmdbRegression.MapFullTriggersRealMapResize`：小地图触发真实 MDB_MAP_FULL，扩容后 map_size 翻倍，读写平稳交错，**PASSED** |
| **T11-A13** | revision 发布与读取并发 | 统一同步、TSAN 可运行时无数据竞争 | `ArchitectureWriterRegression.RevisionAtomicPublicationWithWorkerRebase`：TSAN 运行时全量 125 测试运行通过，0 data race，**PASSED** |
| **T11-A14** | Release 完整性能矩阵 | 原样门限，三轮均过；真实裁剪/扩容且保存发生证据 | `./build/architecture-release/tests/modernime_personal_engine_benchmark`：召回 P99 0.154ms (50k)/0.280ms (200k)，更新召回 P99 0.647ms (50k)/1.380ms (200k)，并发 P99 2.426ms，最大延迟 18.25ms，全部达标，**PASSED** |
| **T11-A15** | 完整 CTest/GTest/安装/文档 | 必需项实际注册执行，失败/跳过/未跑分别记录 | Debug CTest 67/67 通过；`verify_fresh_clone.sh` 100% 通过；`ALGORITHM_EVOLUTION.md` 特性 14 与不变量 29 已同步，**PASSED** |

---

## 5. 性能基准实测数据 (Release 模式)

在 `build/architecture-release` 下执行 `./tests/modernime_personal_engine_benchmark`：

```text
================================================================================
  ModernIME PersonalEngine 50,000 Entries Latency Percentiles & Memory Report
================================================================================
  Total Entries         : 50000
  Process VmRSS Total   : 65.27 MiB (50k Fixture Delta: ~58.16 MiB)
--------------------------------------------------------------------------------
  Operation (50,000 entries)             |   P50 (us) |   P95 (us) |   P99 (us)
--------------------------------------------------------------------------------
  Exact Query (In-Memory Indexed)        |       1.11 |       1.20 |       2.22
  Exact Query (LMDB mmap Zero-Copy)      |       1.66 |       1.87 |       2.67
  Prefix Query [Before: Unindexed Scan]  |     231.03 |     310.32 |     527.06
  Prefix Query [After: Ordered Index]    |       2.45 |       4.13 |      14.78
  Prefix Query [After: LMDB Range Cursor]|       2.60 |       3.41 |      10.43
  Context Sanitize + Query + Full Rerank |       2.20 |       2.30 |       3.38
================================================================================

  Published snapshot 50000 entries:
    - recall P99: 154.29 us (< 1000 us 门限，达标)
    - selection+next recall P99: 646.71 us (< 5000 us 门限，达标)
    - negative+recall P99: 539.37 us
    - suppression+recall P99: 431.16 us
    - new word+recall P99: 550.71 us

  Published snapshot 200000 entries:
    - recall P99: 279.93 us (< 1000 us 门限，达标)
    - selection+next recall P99: 1380.16 us (< 5000 us 门限，达标)
    - negative+recall P99: 1325.67 us
    - suppression+recall P99: 1273.93 us
    - new word+recall P99: 2371.88 us (< 5000 us 门限，达标)

  LMDB writer 50000 entries:
    - enqueue+commit policy+publish+recall P99: 905.76 us (< 5000 us)
    - maximum: 1863.28 us (< 30000 us)

  LMDB writer 200000 entries:
    - enqueue+commit policy+publish+recall P99: 3294.84 us (< 5000 us)
    - maximum: 18251.83 us (< 30000 us)

  LMDB concurrent write/pruning:
    - enqueue+next recall P99: 2426.23 us (< 5000 us)
    - maximum: 4926.37 us (< 30000 us)
```

---

## 6. 测试执行与分类汇总

### 6.1 通过项 (Passed)
1. **CTest 全量功能测试** (`build/architecture-debug`)：**67/67 100% 通过**
   - 覆盖设置客户端、词典管理、会话初始、输入逻辑、候选渲染、安装事务及仓库门禁等全部 67 项测试。
2. **PersonalEngine GTest 单元测试套件** (`modernime_personal_engine_gtest`)：**125/125 100% 通过**
   - 包含 `ArchitectureWriterRegression` (11/11)
   - `ArchitectureLmdbRegression` (6/6)
   - `ArchitectureSelectionRegression` (10/10)
   - `ArchitectureResourceCoordinationTest` (6/6)
   - `ArchitectureCompositionRegression` (8/8)
   - `ArchitectureCandidateIdentityRegression` (3/3)
   - `ArchitectureLearningOrderRegression` (7/7)
   - 及既有全部回归用例。
3. **ThreadSanitizer 并发与竞态测试** (`build/architecture-tsan` via `setarch x86_64 -R`)：**125/125 100% 通过**，无任何 data race 报告。
4. **沙箱全生命周期安装/卸载验证** (`./tests/verify_fresh_clone.sh`)：**100% 通过**，构建、打包、安装、服务运行及卸载清理全部闭环。

### 6.2 跳过项 (Skipped)
- `modernime_settings_focus_gtk`、`modernime_settings_shell_gtk`、`modernime_settings_save_gtk`：在无真实 X11/Wayland 桌面显示器环境变量（headless CI/WSL 环境）下按用例设计自动跳过，不影响核心算法与架构正确性。

### 6.3 失败项 (Failed)
- **0 项**（无任何失败）。

### 6.4 环境阻断与未执行项 (Blocked / Unrun)
- **T12 范围项**：根据指令要求，T12（包管理与事务所有权控制）在本轮严格未执行，保留在后续阶段处理。

---

## 7. 算法演化与规范同步

1. **`docs/ALGORITHM_EVOLUTION.md`**：已如实追加 **特性 14**（LMDB 共享门、EpochViewState 异步加载、有界队列与确定性学习排序），并在第三节不可退化矩阵中追加 **第 29 项** 架构状态机不变量保护。
2. **`docs/T11_ACCEPTANCE.md`**：已创建并记录完整证据链。

---

## 8. 回退策略与交付状态

- **回退方式**：本轮改动聚焦在核心组件同步边界，若需回退至审查基线，可执行 `git checkout 2a99fe6 -- adapter/ algorithm/ core/ tests/`，不会触碰未跟踪架构设计文档。
- **交付状态**：T11 缺陷修复与验收全部完成，在 T11 边界正式停下，等待复审。
