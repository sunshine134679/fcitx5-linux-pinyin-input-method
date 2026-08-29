# ModernIME 质量修复设计规格

## 目标

针对 2026-08-29 全库评审发现的七个已核实问题，逐项修复并保持全量测试通过。每个独立修复单独一次 `git commit`，修复顺序按风险与收益排序。

## 核实结论

以下每一条都已在当前源码中逐一核实（引用行号为核实时的位置），基线为 `fcitx5-debug` 预设下 32/32 测试全部通过。

1. **编译警告旗标未生效**：`cmake/ModernIMEWarnings.cmake:3` 用 `target_compile_options(... INTERFACE ...)`，INTERFACE 属性只传播给消费者，`modernime_fcitx5`、`modernime_ui` 等无下游消费者的目标完全没吃到 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`。
2. **学习库主连接无 busy_timeout**：`core/src/learning_store.cpp:51-59` 打开连接后只设了 WAL；仅备份连接在 `:131` 设了 1 秒。设置客户端与插件并发读写同一 `learning.sqlite3` 时会直接 SQLITE_BUSY 失败。
3. **剪贴板轮询无激活门控**：`adapter/fcitx5/src/inputmethod.cpp:402-433` 每 50ms 无条件读取系统剪贴板并持久化，不检查 ModernIME 是否为当前活动输入法。密码管理器等敏感复制内容会被写入 `clipboard-history.bin`。
4. **用户词典导入整体替换且无确认**：`tools/settings/settings_window.cpp:1286-1306` 导入成功后直接 `saveDictionaryEntries(impl, imported)` 覆盖现有词典，无二次确认，存在数据丢失风险。
5. **学习快照共享语义脆弱**：`core/src/learning_writer.cpp:38-92` 中 `enqueue*` 在锁内原地改写 `snapshot_` 指向的对象，而 `snapshot()` 把同一对象的 `shared_ptr<const>` 交给外部长期持有。当前 fcitx5 单线程使用下无实际竞争，但 API 语义与约束不符，一旦引入异步读取即为数据竞争。
6. **学习记录无总量上限**：`core/src/learning_store.cpp` 只按上下文变体裁剪（每 (pinyin,phrase) 至多 8 个），无词条总数限制；`LearningSnapshot` 全表加载且 `entry()`/`boostAt()` 为线性扫描，长期使用后启动时间与每次刷新的扫描量随词条数线性增长。
7. **每个输入上下文一整套重量级资源**：`algorithm/pinyin/src/pinyin_candidate_provider.cpp:384-419` 中每个 Provider 各自加载系统词典、用户词典、扩展知识词典（12 万条）、构建 `PinyinIME` 与 `UserLanguageModel`，并各起一个 `LearningWriter` 后台线程。fcitx5 每窗口一个输入上下文，内存与线程数随窗口数线性膨胀。官方 fcitx5-chinese-addons 的做法是引擎级共享 `PinyinIME` 与词典，每个上下文只持有 `PinyinContext` 组词状态。

## 修复方案

### 阶段一：本次执行（每项单独提交）

#### 1. 修复警告旗标（cmake/ModernIMEWarnings.cmake）

- 把 `INTERFACE` 改为 `PUBLIC`：既作用于目标本身，又保持现有"链接 core 的测试与工具继承警告"的行为不变。
- 重新全量构建，修复新暴露的警告；不压制、不加 `#pragma`，按语义修复（窄化转换补显式转换、未用参数补名等）。
- 验证：`cmake --build --preset fcitx5-debug` 无新增警告，`ctest --preset fcitx5-debug` 全绿。

#### 2. 学习库 busy_timeout（core/src/learning_store.cpp）

- `open()` 成功后调用 `sqlite3_busy_timeout(db_, 1000)`，与备份连接的既有取值一致。
- 验证：`learning_store_test` 全绿；并发场景由设置客户端读取路径共享同一修复。

#### 3. 剪贴板轮询激活门控（adapter/fcitx5/src/inputmethod.cpp）

- `pollClipboard()` 在读取剪贴板前，用 `instance_->inputMethod(ic)` 检查最近输入上下文的当前输入法是否为 `modernime`，否则直接返回。选择这个零副作用查询而不是读 per-IC 状态的 `controller().active()`：`propertyFor` 只能在缺失时惰性构造整个状态对象，轮询里触发构造得不偿失；两者在"V 菜单可用性"上语义等价（ ModernIME 被选中时 `active()` 缺省值与设置一致）。
- 效果：ModernIME 未被选为当前输入法（包括切到键盘布局/其他输入法）时不读取、不落盘剪贴板；50ms 轮询的空转也随之消除。注意残留面：ModernIME 被选中但用户并未输入的窗口仍会采集（与 fcitx5 官方剪贴板模块一致），敏感应用排除列表留作后续功能。
- 验证：`fcitx5-debug` 构建全绿；行为通过 `engine_state_test` 既有激活语义回归保障。

#### 4. 用户词典导入确认（tools/settings/settings_window.cpp）

- 在导入校验成功之后、覆盖保存之前，弹出与"删除词条"同样式的 `gtk_message_dialog` 确认：明确提示"导入将替换现有全部 N 条词条（导入文件含 M 条）"，取消则不做任何修改。
- 验证：`data_controller_test`、`settings_model_test` 全绿（对话框为纯 UI 层，不触碰数据层语义）。

#### 5. 学习快照写时复制（core/src/learning_writer.cpp/.h）

- `snapshot_` 类型改为 `std::shared_ptr<const LearningSnapshot>`；`enqueue*` 在锁内克隆现有快照、在克隆上应用变更、整体替换指针；`snapshot()` 语义不变。
- 克隆只发生在选词/删除学习词这类低频操作上，逐键 `refresh()` 仍然只读，无性能影响。
- 验证：`learning_store_test`、`pinyin_provider_test` 全绿。

#### 6. 学习记录总量上限（core/src/learning_store.cpp、learning_snapshot.cpp）

- 新增常量 `kMaxLearningEntries = 20000`。
- SQL 侧：`recordBatch` 提交后执行保留型裁剪——按 `suppressed ASC, last_selected_ms DESC, frequency DESC` 保留前 20000 行，删除其余。
- 内存侧：`LearningSnapshot::recordSelection` 应用同一规则，保证内存快照与会话内增长也有界；淘汰顺序与 SQL 侧一致（先淘汰被抑制项，再淘汰最久未选、频次最低者）。
- 验证：`learning_store_test` 新增总量裁剪用例（插入超限条目后断言行数与保留优先级）。

#### 7. 词典/学习资源引擎级共享（algorithm/pinyin、adapter/fcitx5）

- 新增 `PinyinCandidateProvider::SharedResources`（algorithm/pinyin，嵌套类，定义保留在源文件中继续隔离 libime）：持有 `PinyinIME`（连同其拥有的系统/用户/扩展词典与语言模型）、`LearningWriter`、`UserDictionary` 及词典路径；由静态工厂 `createSharedResources` 按 `PinyinDataPaths` 构建一次。
- `PinyinCandidateProvider` 新增接受 `std::shared_ptr<PinyinSharedResources>` 的构造方式；`Impl` 只保留每上下文私有状态（`PinyinContext`、`suppressedLearned_`、页面缓存、上下文文本）。原"独立构造全部资源"的构造函数保留，供测试与单进程工具使用。
- `adapter/fcitx5`：`ModernIMEInputMethod` 构造时创建一份共享资源，`FcitxInputContextState` 的每个 IC 从共享资源构造 Provider；provider 持 `shared_ptr`，保证插件析构与 IC 属性析构顺序无关紧要。
- 学习线程从每窗口一个降为全局一个；词典内存从每窗口一份降为全局一份。
- 验证：`pinyin_provider_test`、`pinyin_engine_test`、`addon_load`、`fcitx5_candidate_list` 全绿；手动冒烟 `fcitx5 -d --replace -u modernime-ui` 后多窗口打字、选词、删除学习词行为不变。

### 阶段二：暂缓项（本次不做，记录待办）

- 配置热重载：插件加 settings.conf mtime 轮询或 inotify，替代"重启 fcitx5 才能生效"。
- Wayland 适配：候选栏改用 layer-shell 或在 Wayland 会话回退 classic UI；HiDPI 分数缩放。
- UI 体验：中文标点转换、候选栏鼠标交互与翻页指示、主题配置化、GTK 事件循环 fd 集成替代 10ms 轮询。
- 代码去重：XDG 路径推导、`text\x1fpinyin` 键拼接、`setError` 助手的重复实现。
- `LearningSnapshot` 查询索引（消除 O(候选×词条) 扫描）、`settings_window.cpp` GTK 层 UI 测试、并发竞态测试。

## 提交计划

每完成一项并通过 `ctest --preset fcitx5-debug` 后立即提交，消息风格与仓库现有中文单主题提交一致：

1. `制定质量修复方案`
2. `修复编译警告旗标未生效`
3. `修复学习数据库并发写入失败`
4. `剪贴板轮询仅在输入法激活时采集`
5. `用户词典导入前增加替换确认`
6. `学习快照改为写时复制`
7. `限制学习记录总量`
8. `词典与学习资源提升到引擎级共享`
