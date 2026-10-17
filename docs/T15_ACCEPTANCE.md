# T15 验收报告：设置数据异步与轻量统计

日期：2026-10-17  
工作树：`/home/wsl/ModernIME/.worktrees/parallel-t15-20261017`  
分支：`codex/architecture-t15-20261017`  
基线提交 (B0)：`7f2f05db34073b526a623d31039b4af06c9198e8`  
状态：`LOCAL_VERIFIED / WIRING_PENDING`  
接口版本：`C15`  

---

## 1. 任务概述与实现总结

本任务（ModernIME T15「设置数据异步与轻量统计」）严格遵循《2026-10-17 并行整改与集成规约》和《AGENTS.md》的架构约束。在独立 worktree 中完成，不干扰并行进行的 T14 或其他任务，不改动 `core/settings`、adapter/UI 候选排序及路径协议。

本轮实现的核心成果如下：

1. **LearningStore / PersonalEngine 轻量计数 (`entryCount`)**
   - 在 `core/include/modernime/core/learning_store.h` 和 `core/src/learning_store.cpp` 中新增 `std::optional<std::size_t> entryCount() const`：
     - **SQLite WAL 存储**：直接执行 `SELECT COUNT(*) FROM learning_entries;`，避免构造沉重的 `LearningSnapshot`，杜绝数万条全量事件反序列化和哈希表分配；
     - **LMDB 存储**：复用既有 `PersonalEngine::EntryCount()` 中的 `mdb_stat(txn, words, &stats)` 逻辑，直接通过元数据结构体返回精确词条数 `stats.ms_entries`；
     - **严谨失败语义**：存储未打开、损坏或执行失败时返回 `std::nullopt`，决不将故障伪装为 `0`；严格遵循读写互斥锁与只读事务规范。
   - `DataController::learningEntryCount` 对接 `store.entryCount()`，实现毫秒级统计。

2. **DataController 词典搜索纯函数 (`filterDictionary`)**
   - 提取出纯函数：
     `std::vector<std::size_t> filterDictionary(std::span<const pinyin::UserDictionaryEntry> entries, std::string_view query)`
   - 将磁盘 I/O（`loadDictionary`）与内存检索完全解耦。词条搜索在输入框输入时只对内存中已有词条列表进行索引过滤，决不重新读取磁盘；
   - 支持空查询全量返回、ASCII 大小写不敏感匹配、拼音分词音节与词条汉字子串匹配；
   - 经实测，50,000 条词典匹配耗时 < 30ms，200,000 条词典匹配耗时 < 100ms（基准门禁分别设为 100ms / 300ms）。

3. **异步 GTask 与代次安全拒收 (`loadGeneration`, `DiagnosticsLifetime`)**
   - `DictionaryPage` 与 `LearningPage` 的数据加载、备份、清空等操作全面迁移到 worker 线程（利用现有 `g_task_run_in_thread` 与 `GTask`）；
   - GTK 主线程通过 `lifetime->withOwner` 安全接收结果，窗口关闭/销毁时在析构函数中调用 `lifetime->deactivate()`，使进行中的异步回调安全失效，彻底消除 Use-After-Free (UAF)；
   - 引入代次标识 `loadGeneration` 与 `batchGeneration`：过期的加载或渲染结果被新请求自动废弃，旧结果决不覆盖新请求。

4. **大列表闲置分片渲染 (Idle Slicing)**
   - `DictionaryPage` 引入闲置分片机制：通过 `g_idle_add_full` 将过滤后的索引逐批追加到 `GtkListStore`；
   - 每片上限限制为：**最多 200 行或 4ms**（`count >= 200 || elapsed >= 4ms`），先到者立即让出主事件循环；
   - 所有词条最终全部可访问，没有截断伪提速；
   - 在 50,000 条大规模加载期间，心跳监控（10ms 定时器）最大延迟仅 **16ms**（远低于 100ms 门禁）。

5. **操作互斥与数据破坏防护**
   - 清空与刷新操作设置敏感状态（禁用相应操作按钮），防止重复点击和重入执行；
   - 备份失败时，`DataController::backupAndClearLearning` 立即终止，原学习记录库完好保留（实测验证原记录未被删除）；
   - 窗口在后台 I/O 进行中被关闭时，不会损坏底层存储事务。

---

## 2. C15 接口与契约说明

| 契约项 | 接口 / 协议细节 | 行为语义 |
|---|---|---|
| **轻量学习词条计数** | `std::optional<std::size_t> LearningStore::entryCount() const` | O(1) SQL `COUNT(*)` 或 LMDB `mdb_stat`；未打开或失败返回 `nullopt`，不返回伪造 0；不构造 `LearningSnapshot`。 |
| **词典检索纯函数** | `std::vector<std::size_t> filterDictionary(std::span<const pinyin::UserDictionaryEntry> entries, std::string_view query)` | 纯内存计算，大小写折叠，返回满足匹配的词条原始索引；不触发任何磁盘 I/O。 |
| **异步加载与分片** | `DictionaryPage::refresh()` / `applyFilter()` / `onBatchIdle` | 磁盘异步读取；批次渲染每片 `<= 200` 行或 `<= 4ms`；代次 `loadGeneration` / `batchGeneration` 拦截作废批次。 |
| **生命周期安全失活** | `detail::DiagnosticsLifetime<Impl>` 与 `~Impl()` | 页面析构时调用 `deactivate()` 并移除 active idle source；异步回调通过 `withOwner` 守卫，杜绝 UAF。 |
| **清空与失败保留** | `DataController::backupAndClearLearning(storePath, backupPath, &error)` | 清空前先验证并生成独立备份，备份生成失败时立即中止，绝不执行清空操作。 |

---

## 3. 契约关卡逐项实测表现 (T15-A01 ~ A06)

| 关卡代号 | 契约要求 | 验证执行方式 | 实测表现 | 结论 |
|---|---|---|---|---|
| **T15-A01** | **轻量计数复杂度与失败语义** | `modernime_learning_store_tests` & `settings_data_async_gtk_test` | 未打开库返回 `nullopt`；50,000 条 SQLite `COUNT(*)` 耗时 0ms；LMDB stat 准确返回 0 和 1000。 | **PASS** |
| **T15-A02** | **词典过滤纯函数与性能基准** | `modernime_data_controller_tests` | 50k 过滤耗时 < 30ms (< 100ms)；200k 过滤耗时 < 100ms (< 300ms)；大小写折叠、拼音/词条子串均正确。 | **PASS** |
| **T15-A03** | **50k 大列表分片与心跳** | `settings_data_async_gtk_test` | 50,000 条记录完整加载至 `GtkListStore`；每批 <=200 行/4ms；心跳最大延迟 16ms (< 100ms)。 | **PASS** |
| **T15-A04** | **连续搜索打断与旧批次作废** | `settings_data_async_gtk_test` | 连续快速输入 6 次搜索关键词；旧批次被 `batchGeneration` 立即拦截作废；心跳 33ms (< 100ms)；最终结果精准对齐。 | **PASS** |
| **T15-A05** | **慢 I/O 响应与关闭无 UAF** | `settings_data_async_gtk_test` | 模拟 2s 耗时后台任务，GTK 心跳延迟仅 2ms (< 100ms)；在刷新/清理中途直接销毁窗口与 Page，0 崩溃 0 UAF。 | **PASS** |
| **T15-A06** | **备份失败拒绝清空** | `settings_data_async_gtk_test` & `data_controller_test` | 目标路径非法或无法创建备份时，返回错误且原学习数据库词条数量保持为 1，无任何数据丢失。 | **PASS** |

---

## 4. 验证汇总与测试清单

本地私有构建目录 (`build/parallel-debug`) 下全部执行通过：

1. **`modernime_learning_store_tests`**: PASS (0 失败)
2. **`modernime_data_controller_tests`**: PASS (0 失败，包含 50k/200k 过滤基准测试)
3. **`modernime_settings_data_async_gtk`**: PASS (全部 7 组综合测试通过，包含 50k 分片、连续搜索、2s 慢 I/O 心跳、无头检测 77 机制等)
4. **既有页面与模型测试回归**：
   - `modernime_learning_page_contract_tests`: PASS
   - `modernime_dictionary_page_contract_tests`: PASS
   - `modernime_settings_tests`: PASS
   - `modernime_settings_shell_gtk_tests`: PASS
   - `modernime_overview_model_tests`: PASS
   - `modernime_clipboard_history_model_tests`: PASS

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
