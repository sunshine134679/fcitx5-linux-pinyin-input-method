# T15 验收报告：设置数据异步与轻量统计（第二轮收尾后）

日期：2026-10-17  
工作树：`/home/wsl/ModernIME/.worktrees/parallel-t15-20261017`  
分支：`codex/architecture-t15-20261017`  
基线提交 (B0)：`7f2f05db34073b526a623d31039b4af06c9198e8`  
状态：`LOCAL_VERIFIED / WIRING_PENDING`  
接口版本：`C15`  

---

## 1. 任务概述与实现总结

本任务（ModernIME T15「设置数据异步与轻量统计」）严格遵循《2026-10-17 并行整改与集成规约》和《AGENTS.md》的架构约束。在独立 worktree 中完成，不干扰并行进行的 T14 或其他任务，不改动 `core/settings`、adapter/UI 候选排序及路径协议。

本轮在第一轮生产修复（R1–R3）基础上，针对第二轮复审反馈（S1/S2）完成了测试设施与验收探针的定向收网：

1. **LearningStore / PersonalEngine 轻量计数 (`entryCount`)**
   - 在 `core/include/modernime/core/learning_store.h` 和 `core/src/learning_store.cpp` 中新增 `std::optional<std::size_t> entryCount() const`：
     - **SQLite WAL 存储**：直接执行 `SELECT COUNT(*) FROM learning_entries;`（轻量遍历 B-tree 叶子节点，避免构造沉重的 `LearningSnapshot`，常用量级毫秒级返回）；
     - **LMDB 存储**：复用既有 `PersonalEngine::EntryCount()` 中的 `mdb_stat(txn, words, &stats)` 逻辑，直接通过元数据结构体返回精确词条数 `stats.ms_entries`（O(1) 复杂度）；
     - **严谨失败语义**：存储未打开、损坏或执行失败时返回 `std::nullopt`，决不将故障伪装为 `0`；严格遵循读写互斥锁与只读事务规范。
   - `DataController::learningEntryCount` 对接 `store.entryCount()`，实现毫秒级统计。

2. **DataController 词典搜索纯函数 (`filterDictionary`)**
   - 提取纯函数：
     `std::vector<std::size_t> filterDictionary(std::span<const pinyin::UserDictionaryEntry> entries, std::string_view query)`
   - 将磁盘 I/O（`loadDictionary`）与内存检索完全解耦。词条搜索在输入框输入时只对内存中已有词条列表进行索引过滤，决不重新读取磁盘；
   - 支持空查询全量返回、ASCII 大小写不敏感匹配、拼音分词音节与词条汉字子串匹配；
   - 经实测，50,000 条词典匹配耗时 < 30ms，200,000 条词典匹配耗时 < 100ms（基准门禁分别设为 100ms / 300ms）。

3. **异步 GTask 与代次安全拒收 (`loadGeneration`, `DiagnosticsLifetime`)**
   - `DictionaryPage` 与 `LearningPage` 的数据加载、保存、导入预览、导入合并/替换与导出全面迁移到 worker 线程（利用现有 `g_task_run_in_thread` 与 `GTask`）；
   - GTK 主线程通过 `lifetime->withOwner` 安全接收结果，窗口关闭/销毁时在析构函数中调用 `lifetime->deactivate()`，使进行中的异步回调安全失效，彻底消除 Use-After-Free (UAF)；
   - 引入代次标识 `loadGeneration`、`batchGeneration`、`operationGeneration` 与 `refreshGeneration_`：过期的加载、统计或渲染结果被新请求自动废弃，旧结果决不覆盖新请求。

4. **大列表闲置分片渲染 (Idle Slicing)**
   - `DictionaryPage` 引入闲置分片机制：通过 `g_idle_add_full` 将过滤后的索引逐批追加到 `GtkListStore`；
   - 每片上限限制为：**最多 200 行或 4ms**（`count >= 200 || elapsed >= 4ms`），先到者立即让出主事件循环；
   - 所有词条最终全部可访问，没有截断伪提速；
   - 在 50,000 条大规模加载期间，心跳监控（10ms 定时器）最大延迟仅 **40ms**（低于 100ms 门禁）。

5. **S1 测试设施安全：IoHook 互斥同步与闭包局部稳定副本**
   - 修复了 `tools/settings/data_controller.cpp` 中全局 `IoHook` 无同步的缺陷：
     - 使用 `std::mutex s_hookMutex` 保护 `s_ioHook` 的设置与读取；
     - `notifyIoHook` 在临界区内提取 `localHook = s_ioHook` 稳定副本，在**锁外**调用该副本。当主线程并发调用 `setIoHookForTesting(nullptr)` 时，全局对象被重置，但当前 worker 的局部副本依然持有闭包生命周期，从根源上消除了 heap-use-after-free；
     - 新增独立回归用例 `testIoHookLifetimeAndConcurrentReset`，并在 ASAN/UBSan 下验证通过（0 UAF、0 未定义行为；测试卸钩后通过 `worker.join()` 确定性观察线程完整退出）。

6. **S2a 真实读后挂起与失败对照验证**
   - 在 `DataController::learningEntryCount` 读完 `store.entryCount()` 后增加 `"learningEntryCount:afterCount"` 挂钩点；
   - Scenario A 探针在旧请求**真实读完 count=1 后**暂停，清空 DB 后发起新请求读到 0 并应用到界面，最后释放旧请求；
   - **对照验证**：
     - 当前生产修复通过测试，旧统计被代次检查丢弃，按钮维持禁用；
     - 使用 `2a0460a` 旧版 `LearningPage` 运行相同测试，旧统计 `count=1` 覆盖新状态并重新启用按钮，触发 assert 崩溃（退出码 134），反证测试具有严格的缺陷拦截能力。

7. **S2b 连续搜索在途分片真打断与过滤前断言**
   - 重构 `pumpEventsUntil` 为逐事件单步调度，避免内部循环直接将所有 idle 分片排空；
   - 在两次目标切换（"xue" 与 "zhong"）设置文本后立即发出真实控件信号 `g_signal_emit_by_name(searchEntry, "search-changed")`；
   - 通过 `g_signal_add_emission_hook` 在生产过滤处理函数开始前直接断言：
     `0 < xueOldRowsBeforeFilter < 4000` (实测 200 行)
     `0 < zhongOldRowsBeforeFilter < 4000` (实测 200 行)
     确证旧 idle 分片在尚未完成时被新过滤取消并替换；
   - **对照验证**：若未立即发出信号，在控件 150ms 防抖期间旧批次全部完成（4000 行），断言确定性失败（exit -6 / SIGABRT）；
   - 最终呈现的 4000 行拼音、词条、`sourceIndex` 逐行与纯函数 `filterDictionary` 比对无误，移走磁盘文件后内存搜索验证通过。

---

## 2. C15 接口与契约说明

| 契约项 | 接口 / 协议细节 | 行为语义 |
|---|---|---|
| **轻量学习词条计数** | `std::optional<std::size_t> LearningStore::entryCount() const` | 轻量 SQL `COUNT(*)`（遍历 B-tree 叶子节点，避免构造 `LearningSnapshot`，常用量级毫秒级返回）或 LMDB `mdb_stat`（O(1)）；未打开或失败返回 `nullopt`，不返回伪造 0。 |
| **词典检索纯函数** | `std::vector<std::size_t> filterDictionary(std::span<const pinyin::UserDictionaryEntry> entries, std::string_view query)` | 纯内存计算，大小写折叠，返回满足匹配的词条原始索引；不触发任何磁盘 I/O。 |
| **异步加载与分片** | `DictionaryPage::refresh()` / `applyFilter()` / `onBatchIdle` | 磁盘异步读取；批次渲染每片 `<= 200` 行或 `<= 4ms`；代次 `loadGeneration` / `batchGeneration` 拦截作废批次。 |
| **生命周期安全失活** | `detail::DiagnosticsLifetime<Impl>` 与 `~Impl()` | 页面析构时调用 `deactivate()` 并移除 active idle source；异步回调通过 `withOwner` 守卫，杜绝 UAF。 |
| **清空与失败保留** | `DataController::backupAndClearLearning(storePath, backupPath, &error)` | 清空前先验证并生成独立备份，备份生成失败时立即中止，绝不执行清空操作。 |
| **测试钩子安全同步** | `DataController::setIoHookForTesting(IoHook hook)` | 内部采用短锁与稳定局部副本机制，支持并发重置与在途回调执行，杜绝跨线程 UAF。 |

---

## 3. 契约关卡逐项实测表现 (T15-A01 ~ A07)

| 关卡代号 | 契约要求 | 验证执行方式 | 实测表现 | 结论 |
|---|---|---|---|---|
| **T15-A01** | **轻量计数复杂度与失败语义** | `modernime_learning_store_tests` & `settings_data_async_gtk_test` | 未打开库返回 `nullopt`；50,000 条 SQLite `COUNT(*)` 耗时 0ms；LMDB stat 准确返回 0 和 1000。 | **PASS** |
| **T15-A02** | **词典过滤纯函数与性能基准** | `modernime_data_controller_tests` | 50k 过滤耗时 < 30ms (< 100ms)；200k 过滤耗时 < 100ms (< 300ms)；大小写折叠、拼音/词条子串均正确。 | **PASS** |
| **T15-A03** | **50k 大列表分片与心跳** | `settings_data_async_gtk_test` | 50,000 条记录完整加载至 `GtkListStore`；每批 <=200 行/4ms；心跳最大延迟 40ms (< 100ms)。 | **PASS** |
| **T15-A04** | **连续搜索在途打断与集合验证** | `settings_data_async_gtk_test` | 克服 150ms 控件防抖，即时信号触发过滤；通过 emission hook 在生产过滤执行前断言 0 < old_rows < 4000（实测两次切换前均为 200/4000，被打断取消）；最终 4000 行与原始索引精确对齐；心跳 37ms (< 100ms)；移走原文件后内存检索通过。 | **PASS** |
| **T15-A05** | **真实慢 I/O 心跳与安全销毁** | `settings_data_async_gtk_test` | 真实生产 I/O 挂钩 2s 耗时，GTK 心跳延迟仅 2ms (< 100ms)；在加载/保存途中直接销毁窗口与 Page，观察钩子阶段完成与 worker 回调失效，0 崩溃 0 UAF；独立卸钩用例中通过 worker.join 观察线程完整退出。 | **PASS** |
| **T15-A06** | **备份失败拒绝清空** | `settings_data_async_gtk_test` & `data_controller_test` | 目标路径非法或无法创建备份时，返回错误且原学习数据库词条数量保持为 1，无任何数据丢失。 | **PASS** |
| **T15-A07** | **乱序读作废与清空防重入** | `settings_data_async_gtk_test` | `learningEntryCount:afterCount` 挂起旧请求；新请求清空后返回 0；旧请求读到 1 恢复后被代次丢弃；清空期间旧回调无法重置按钮；防重入拦截生效。 | **PASS** |

---

## 4. 验证汇总与测试清单

本地私有构建目录 (`build/parallel-debug`) 与 ASAN 构建目录 (`build/parallel-asan`) 下全部执行通过：

1. **`modernime_settings_data_async_gtk_tests` (常规 Debug)**:
   - 全部 10 组测试 PASS (0 失败)：
     - `testLearningStoreEntryCount`
     - `testFilterDictionaryPureFunction`
     - `testDictionaryPageAsyncAndBatch` (50k 加载心跳 26ms < 100ms)
     - `testDictionaryPageAsyncIoAndSlowIoSafety` (2s 生产慢 I/O 心跳 2ms < 100ms)
     - `testSafeWindowClosingUnderSlowIo`
     - `testBackupFailureDoesNotClearStore`
     - `testContinuousSearch` (emission hook 断言过滤前在途行数 200/4000，心跳 37ms < 100ms)
     - `testLearningPageStaleRefreshAndClearReentry` (Scenario A 读后挂起 & Scenario B 备份重入拦截)
     - `testLearningPageAsyncAndSlowIo` (2s 生产慢 I/O 心跳 3ms < 100ms)
     - `testIoHookLifetimeAndConcurrentReset` (并发卸钩与在途闭包寿命，通过 worker.join 观察线程完整退出)
2. **`modernime_settings_data_async_gtk_tests_asan` (AddressSanitizer & UBSan)**:
   - 全部 10 组测试 PASS (exit 0)；
   - 验证范围：**内存安全检查（0 UAF / 0 堆溢出 / 0 栈溢出 / 0 未定义行为）**；
   - 数据竞争说明：**本轮未执行 TSAN，测试通过证明未报告地址及相关未定义行为错误，据实记录未执行数据竞争检测**；
   - 门限说明：插桩后因 ASAN 影子内存与事件循环开销，心跳门限设为 300ms（实测连续搜索心跳 88ms，50k 分片 41ms）；
   - 内存泄漏说明：因 GLib/GTK 框架存在既定的静态类型注册与全局缓存，测试运行使用了 `ASAN_OPTIONS=detect_leaks=0`，本测试报告不将此声称覆盖 GLib 级别的零泄漏。
3. **缺陷反证测试 (`stale_regression_mutant_test`)**:
   - 链接 `2a0460a` 旧版 `LearningPage` 执行 Scenario A，实测触发断言失败（退出码 134），确证测试具备拦截旧缺陷的有效性。
4. **既有回归测试套件**:
   - `modernime_learning_store_tests`: PASS
   - `modernime_data_controller_tests`: PASS
   - `modernime_settings_shell_gtk_tests`: PASS
   - `modernime_learning_page_contract_tests`: PASS
   - `modernime_dictionary_page_contract_tests`: PASS

---

## 5. 交付件与接线申请索引

- **代码变更文件清单**：
  - `core/include/modernime/core/learning_store.h`
  - `core/src/learning_store.cpp`
  - `core/include/modernime/core/personal_engine.h`
  - `core/src/personal_engine.cpp`
  - `tools/settings/include/modernime/settings/data_controller.h`
  - `tools/settings/data_controller.cpp`
  - `tools/settings/pages/dictionary_page.cpp`
  - `tools/settings/pages/learning_page.cpp`
  - `tests/core/learning_store_test.cpp`
  - `tests/settings/data_controller_test.cpp`
  - `tests/settings/settings_shell_gtk_test.cpp`
  - `tests/settings/settings_data_async_gtk_test.cpp`
- **接线申请文件**：`build/parallel-handoff/wiring-request.md`
- **状态登记文件**：`build/parallel-handoff/T15.json`
