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

## 批次二（2026-08-29 追加：习惯学习强化、标点、热重载、清理）

用户反馈：频繁选择排在后面的候选，后期没有被前移/置顶。核实结论：

- `pinyin_provider_test.cpp:297-319` 已有"选 5 次低位候选置顶"的回归测试且通过——因为测试从不设置上下文。
- 真实链路里 `engine.cpp:88-94` 把 surrounding text 传入 provider，`learning.enabled` 默认开启，于是每次选词都按 (pinyin, phrase, 上下文前缀, 上下文后缀) 落成**上下文变体行**；`boostAt` 只查"完全相同上下文"或"空上下文基础行"。同一词语在不同句子里各选一次，频率被拆成多行、每行都很低，全局频率信号永远聚不起来——这是主根因。
- 次因：排序层学习权重封顶 `LearningPriorCap = 8`（`candidate_ranker.h:15`），即便有 boost 也最多前移 8 个 source_index，够不到首页。

### 1. 学习频率跨上下文聚合（core）

- `LearningSnapshot::boostAt` 改为对 (phrase, pinyin) 的**全部未抑制行**聚合：频率求和（每次选词恰写入一行，求和即真实总次数）、最近时间取最大、负反馈取最大；签名去掉上下文参数（上下文细化由 `contextBoost` 单独承担）。`entry()` 保留原语义供测试与兼容。
- 新增 `hasPositiveFrequency(phrase, pinyin)`，provider 的 Learned 分类改用它，替代现在按精确/基础行查找的 `entry()` 调用。
- 存储层不变，无需迁移。新增回归测试：多上下文各选一次后，新上下文下 boost 仍显著大于 0；并新增 provider 级"带变化的上下文反复选词仍置顶"用例（复现用户场景，旧实现必失败）。

### 2. 提升常选候选的排序权重（core）

- `boostAt` 频率曲线改为 `min(3.0, 0.85×log1p(总频率))`，recency 不变，总 clamp 放宽到 [−2, 4.0]；`LearningPriorCap` 8 → 16。
- 效果梯度：1 次最近选择约前移 6 位，10 次约 12 位，30 次以上可达封顶 16 位；`match_priority` 仍是主键——无法消耗当前输入的候选（如前缀截断）不会被学习顶到前面，这是正确性边界。

### 3. 中文标点全角转换（core + adapter + 设置客户端）

- core 新增 `punctuation.{h,cpp}`：固定映射（`,`→`，`、`.`→`。`、`?`→`？`、`!`→`！`、`:`→`：`、`;`→`；`、`(`→`（`、`)`→`）`、`~`→`～`），配对引号 `"`/`'` 由控制器按开合状态输出 `“”`/`‘’`（每个输入上下文独立记忆，reset 复位）。
- 两条防误伤规则：上下文（或即将提交的 preedit）以 ASCII 字母/数字结尾时保持半角（覆盖 `3.14`、`1,000`、代码片段）；无映射的字符（`@#$/` 等）保持现状直通。
- 行为：中文模式（active）下，preedit 为空时标点键被输入法接管并上屏全角（旧实现是放行给应用）；组词中则先上屏候选/原始串再上屏全角标点。英文模式（非 active）完全不变。
- 设置项 `punctuation.enabled`（默认 true），`ModernIMESettings` 加字段；设置客户端"基本设置"页加开关。

### 4. 配置文件热重载（adapter）

- `ModernIMEInputMethod` 加 2 秒定时器检查 `settings.conf` mtime；变化则重载 `SettingsStore`，更新引擎级 `settings_`/`keyBindings_`，并经 `InputContextManager::foreach` 同步到**已存在**的每个 IC 状态（`applySettings`：控制器选项、剪贴板触发键、provider 学习开关）。
- 为避免 foreach 触发惰性构造重量级状态，工厂维护存活状态集合，引擎析构时 `stateFactory_.unregister()`（顺带补上此前缺失的属性注销）。
- provider 新增 `setLearningEnabled`/`setContextLearningEnabled`；学习写入器改为"启动时按初值创建，之后惰性创建"（保住"禁用学习不建库"的既有测试语义）。
- 用户词典文件与剪贴板历史文件同机制监听：词典变化时共享资源层 `clear(1)` + 重新 `addTo`，词典页改动免重启生效；剪贴板文件变化时插件先从磁盘重载再 observe，修复设置客户端删除/清空被插件旧内存覆盖回写的竞态。

### 5. 卫生清理

- pinyin provider 的 `defaultLearningPath`/`defaultUserDictionaryPath` 改用 `core::SettingsPaths::fromEnvironment`，消除双轨路径推导。
- 删除空壳 `foundation_test`；修掉 `runtime_controller_test.cpp` 中硬编码 `/home/wsl/...` 的断言。

### 提交计划

`更新修复方案批次二` → `学习频率跨上下文聚合` → `提升常选候选的排序权重` → `中文标点全角转换` → `实现配置文件热重载` → `实现用户词典修改免重启生效` → `修复剪贴板历史被插件旧内存覆盖` → `统一用户数据路径推导` → `清理测试与死代码`

