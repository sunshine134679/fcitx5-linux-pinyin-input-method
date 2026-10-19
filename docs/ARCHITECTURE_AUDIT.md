# ModernIME 全仓库架构与技术债审查

> 审查日期：2026-10-16。最终核对基线：HEAD `8526bf5182f168f9d1b5af30245fa88372430313`。仅新增本报告及整改清单，没有修改生产代码、测试、配置、算法或用户数据。

## 0. 范围、方法与证据限制

完整阅读了 `AGENTS.md`、`ALGORITHM_ARCHITECTURE.md`、`ALGORITHM_CHANGE_GUIDE.md`、`ALGORITHM_EVOLUTION.md`、`CANDIDATE_PIPELINE_ARCHITECTURE.md`、`STABILITY_AUDIT.md`、`HANDOVER.md`。扫描了 core、algorithm、adapter、ui、tools/settings、scripts、tests 的源码、接口及所有模块 CMake，并交叉核对安装/卸载、打包、数据保护及 CI。

审查按调用、数据与状态所有权展开。重点深入了用户指定的八个文件、设置各 Page/Controller，以及 LearningWriter/Store、候选核心模块与 Fcitx 桥接头文件。一级模块扫描不代表每个词表、每条测试和每个发行平台均已逐行或动态验收。

审查开始于HEAD `3af67dc54827c9b8075c031a7f6caed9be99a6fb`，设置窗口三文件已有未提交修改；过程中 `tools/settings/pages/updates_page.cpp` 也发生变化，并由外部工作形成提交 `8526bf5`。最终已复核两提交间四文件差异，它们均保留。报告采用可核对的稳定职责与调用证据，不把编辑中的瞬时编译不一致登记为项目缺陷。行号对应审查时源码，后续编辑应以函数名重新定位。

本轮执行了只读 `ctest --test-dir build/fcitx5-debug -N`，列出 **66 个测试**，包括 GTest、Benchmark 和三个指定安装测试。没有重新构建、执行完整 CTest/GTest/Benchmark、运行安装卸载或操作真实学习库。因此下文区分：

- **已确认机制**：源码能直接证明的同步缺口、锁顺序、重复政策或信息丢弃。
- **运行风险**：由机制推导的后果，尚未用当前源码夹具或真实桌面复现。
- **待迁移设计**：文档明确尚未实现的目标，不当成既有能力回退。

整改编号 `ARC-*` 仅用于本次审查与 [REFACTOR_BACKLOG.md](REFACTOR_BACKLOG.md) 互相引用，不替代已有 `ALG-*`、`CAND-*` 契约台账。

## 1. 当前架构概览

### 1.1 一级模块与真实依赖

| 模块 | 当前职责 | 主要依赖与边界判断 |
|---|---|---|
| core | 候选/提交数据模型，匹配与排序政策，学习快照/异步写入/双后端，标点、配置、剪贴板、造词 | 链接 SQLite、LMDB；未发现依赖 algorithm、adapter、GTK 或 Fcitx5 的反向 include。内部业务政策与存储职责仍有互相渗透 |
| algorithm/pinyin | LibIME 会话与资源、原生/纠错/学习召回、统一特征、前缀证据、中文混排、中英及宏政策、提交学习协调 | 依赖 core、LibIME；Provider 是主要职责汇聚点 |
| adapter/fcitx5 | 宿主事件与能力、周边文本有效性、编辑/提交状态机、Fcitx 候选列表、资源预热及文件轮询 | 依赖 core、可选 pinyin、Fcitx5；Controller 能通过 EngineHost 脱离真实宿主测试，这是有效边界 |
| ui/fcitx5 | 文字度量、宽度分页、几何、绘制、GTK 窗口/指示器与事件桥接 | layout 依赖 core/PangoCairo；addon 依赖 Fcitx/GTK，并 include adapter 的候选列表契约。该接缝过宽，但属于宿主集成，并非 core 反向依赖 UI |
| tools/settings | 配置草稿、页面/导航/预览、词典及学习数据操作、运行时诊断、软件源检查与升级 | 依赖 core、pinyin、ui_layout、GTK；候选预览复用布局/绘制，无第二套排序器 |
| scripts / 根安装脚本 | 安装清单和托管文件、会话环境、备份迁移、二进制/原生包/仓库发布 | 有共享 install_state 与 data-guard；仍有一次性清理旁路和安装脚本文本生成耦合 |
| tests | 纯政策、Provider/Controller、Fcitx/布局、GTK、存储并发、性能、安装与发行闭环 | 已有大量行为回归；部分 Page contract 只验证注册表/类型，不验证真实交互；必需测试仍有条件注册 |
| data / config / cmake / packaging | 离线数据、宿主/桌面元数据、构建及发行契约 | 数据离线构建，依赖显式探测；多个渠道仍手工维护相同包/环境知识 |

实际链接方向可简写为：

```text
settings ──→ pinyin ──→ core ──→ SQLite / LMDB
    └──────→ ui_layout ──→ core / PangoCairo
adapter ──→ pinyin / core / Fcitx5
ui_addon ──→ ui_layout / Fcitx5 / GTK
    └──────→ adapter 候选桥接头文件
```

没有证据支持“全仓分层已经崩溃”。问题主要发生在层内职责、跨层状态协议和政策接缝，而非大量下层链接上层。

### 1.2 核心输入与候选链路

| 阶段 | 实际负责人/入口 | 输入、输出与改变顺序的权限 |
|---|---|---|
| 宿主事实 | `ModernIMEInputMethod::keyEvent`，inputmethod.cpp:785、826 | 读取能力与合法周边文本，翻译 KeyEvent；不负责评分 |
| 组合编辑 | `ModernIMEController::handle/replaceComposition`，engine.cpp:361、425 | 拥有编辑字符串/光标，调用 Provider；部分选择后接纳剩余组合 |
| 拼音解析 | Provider 的 LibIME PinyinContext；core 的 PinyinMatchPolicy | LibIME 解码切分；core 提供规范化、匹配、简拼等政策。两者职责不同，不能强行换成单一解析器 |
| 中文召回合并 | `buildCandidatePipeline`，candidate_pipeline.cpp:17 | 原生最多128、纠错最多16及学习召回；按文本/读音合并，调用 Features/Ranker |
| 特征/组内排序 | candidate_features.cpp:95；CandidateRanker | 同一请求快照/时间计算自身证据，按匹配资格和代价排序；不写盘 |
| 后置资格及分组 | Provider::refresh，provider.cpp:1975 | 屏蔽、人工词数量约束、完整/部分消费分组 |
| 模糊插位 | `interleaveHigherNativeRankFuzzyCandidate`，provider.cpp:906、2025 | 可把模糊项插到第二位，属于评分后政策 |
| 前缀/中文混排 | buildPrefixEvidence；mixCandidateItems | 前缀读取自身学习；Mixer 用证据编排多粒度席位，不访问存储 |
| 模式与语言最终组合 | Provider::refresh，provider.cpp:2036、2060 | 宏、英文、原文、命令继续插位；2287最终屏蔽，2315文本去重 |
| 候选发布 | Controller::publishProviderPage，engine.cpp:955；FcitxEngineHost::publishPage，inputmethod.cpp:283 | Provider页复制到Controller，再生成带全局下标的Fcitx词表 |
| 分页/绘制 | CandidatePagination；ui_addon.cpp:511、530；Layout/Renderer | UI按宽度分连续页面并回传；不重排语义。Controller有无UI时的数量分页回退 |
| 选择 | 数字/空格→Controller；鼠标→FcitxCandidateWord→Controller | 以全局下标调用Provider，Provider产生学习/消费，Controller上屏。代次和身份校验缺口见ARC-03 |

**目前没有“所有候选都经过同一个统一 pipeline”的事实。** 原生、纠错、中文学习候选走统一中文核心；宏、英文补全、原文与命令仍在 Provider 后置生成。它们有最终屏蔽保护，不能说当前全部绕过公共边界；但新增来源若照搬后置 `insert`，容易绕过统一证据与组合验收。

中文 Ranker、模糊插位、中文 Mixer、宏插位、中英/原文插位均可改变顺序。它们有阶段上的合理分工，但**最终全来源顺序尚无单一政策负责人**。实际最后写入者是 Provider，中文 Mixer 不是全来源 Composer。文档第0节已承认这一状态。

### 1.3 核心学习与持久化链路

```text
已确认选择/原文/取字/删字证据
  → Provider构造CommitEvent并检查学习资格
  → PhraseFormationTracker / 共享PhraseFormationStats（合格输入才修改）
  → LearningWriter::enqueueCommit
  → 乐观不可变快照发布 + 待写事件（带resetRevision）
  → worker → LearningStore::recordBatch
  → SQLite事务 / PersonalEngine::RecordLearningBatch（LMDB事务）
  → 一致读取状态，校验snapshotVersion与resetRevision后重新发布
```

正常异步学习写入集中在 Writer；Store/PersonalEngine 的直接API和设置清空仍是合法的独立管理入口，不能仅因存在就认定为错误双写。真正的问题是资格/差量学习政策被复制到多个入口、并发访问未完全遵守发布协议，以及维护脚本绕过管理协议。

Controller 负责退格窗口和留字确认这类宿主证据；Provider负责选择角色、消费、造词与事件；Writer负责学习接受后的发布/队列；Store负责事务。不要把这些全部塞进一个“PersonalizationManager”。

### 1.4 核心配置链路

`SettingsStore::load/validate/save` → `SettingsWindowModel` 保存基线与草稿 → settings.conf 原子替换 → adapter轮询mtime加载、递增settingsGeneration并写runtime.state → 下次keyEvent应用到会话；UI addon另以1500ms节流检查mtime加载字体和容量。

配置草稿、磁盘配置、运行时已应用状态、UI度量快照是不同生命周期的状态，不能简单合并。缺口是它们没有共同的应用版本/分页版本协议；Provider setter还可单独刷新候选而不发布Controller页面。

路径构造主要集中在 `SettingsPaths::fromEnvironment`（core/src/settings.cpp:167），运行程序读取XDG/HOME；安装与data-guard另支持MODERNIME环境变量。必须明确安装覆盖参数如何传递成运行时XDG环境，不能假设所有进程直接理解同一组变量。

## 2. 当前健康度

| 维度 | 评价 | 证据与边界 |
|---|---|---|
| 模块边界 | 可接受 | 链接方向清楚，core无GTK/Fcitx依赖；Provider内部及UI/adapter接缝需收敛 |
| 内聚性 | 有风险 | Provider混合资源/语言政策/选择/学习；PersonalEngine混合检索、迁移、事务和文件政策 |
| 耦合度 | 有风险 | 同一特性常需同时调整Provider、Controller、事件、Writer、Store与测试；隐式调用顺序承担协议 |
| 可测试性 | 可接受 | 已有EngineHost、假进程、隔离库、真实LibIME和布局回归；缺版本竞争、锁交错和三路事件一致性套件 |
| 可维护性 | 有风险 | 最新保护规则继续散入旧类；一次性修复脚本仍处于常规scripts目录 |
| 可扩展性 | 有风险 | 新来源可模仿后置插位，新学习政策可复制到两个recordCommit入口 |
| 状态管理 | 高风险 | 候选页多份副本无版本授权；Writer新增判断绕过快照同步；更新active只覆盖部分阶段 |
| 生命周期管理 | 高风险 | LMDB句柄注册与事务暂存对象形成锁反序；并非所有对象都可安全按普通引擎生命周期处理 |
| 性能风险 | 有风险 | LMDB代次观察可等待后台写锁；清空后输入同步整库读；设置搜索/统计做同步全量工作。未测当前桌面延迟 |

“高风险”不等于已复现用户数据损失或桌面崩溃；它表示当前源码已存在足以破坏核心契约的机制缺口，应先固定失败证据，再小范围修复。

## 3. Hotspot：按职责判断，不按文件长度判断

| 文件/类 | 当前职责、复杂度来源、耦合对象 | 继续增长的风险 | 是否应该拆、推荐边界 |
|---|---|---|---|
| core/src/personal_engine.cpp / PersonalEngine | 内存变体与索引、独立查询/衰减、LMDB环境共享/扩容/事务、SQLite迁移、旧文件读写导入、上下文与容量；耦合LearningEvent、Store、全局注册表 | 存储生命周期进入暂存对象；外部导入政策影响可信恢复；新增后端规则易复制 | **应按责任渐进提取**环境句柄所有权、纯事件变换、格式解码/导入政策。先修锁与恢复意图，不重写查询/事务，不另造总Manager |
| algorithm/pinyin/src/pinyin_candidate_provider.cpp | LibIME会话、资源装配/热重载、召回协调、语言/宏/模糊政策、最终去重、选择角色/造词/学习；耦合几乎全部候选模块 | 新特性习惯性增加if、fallback和插位；选择/刷新副作用难看见 | **应拆职责**：先命名并提取既有模式/语言组合政策，再明确选择→事件边界，最后分离共享资源生命周期；保留会话协调 |
| core/src/learning_snapshot.cpp | 不可变分片、索引、别名/屏蔽、召回/增益、局部更新与淘汰 | 后端事件语义复制；更多查询政策容易侵入分片实现 | **暂不整体拆**。512分片/COW/cache是同一不可变视图责任；先提取确实共享的事件/身份政策，保持物理索引独立 |
| core/src/pinyin_match.cpp / PinyinMatchPolicy | 拼音切分、规范化、匹配/简拼资格；现还含isCoreChineseWord学习保护词表（:139） | 学习白名单继续进入匹配工具，变成业务杂物入口 | **解析主体不拆**。只把核心词学习资格归到学习政策；可信简拼属于匹配准入，不能为追求纯粹盲目搬走 |
| core/src/learning_writer.cpp | 乐观发布、队列、重试、清空观察、后台重放与提交过滤 | 新政策轻易绕过锁；输入线程承担同步观察/重建 | **优先收敛协议**，不是加Manager。接受判断必须在明确的版本/同步边界执行；队列和发布仍同属Writer |
| adapter/fcitx5/src/engine.cpp / ModernIMEController | 按键/编辑/模式/分页/提交，周边有效性、退格留字证据、剪贴板模式；耦合EngineHost、Provider、CandidatePage | bool与清理顺序继续增多，页面授权与业务决策混在按键分支 | **不整体拆**。先固定版本化选择边界；退格窗口/待确认/失效是一个状态机，可整体封装但不拆碎每个分支 |
| adapter/fcitx5/src/inputmethod.cpp | Fcitx事件、能力、资源预热、配置/词典轮询、剪贴板与runtime.state | 每个新设置都要手动接多个setter，失效/发布容易漏 | **可局部提取**资源装配和设置应用批次；宿主采集留在此处。不能下推Fcitx事实到core |
| tools/settings/update_controller.cpp | 进程执行/超时/捕获、后台事务全局状态、APT/DNF解析、锁/渠道/版本/命令政策 | 新发行版或超时修复同时触及进程生命周期与产品政策 | **应拆职责**：独立进程执行边界、包操作所有权、纯APT/DNF解析；先处理槽位和输出预算，不一口气建插件系统 |
| tools/settings/settings_shell.cpp | GTK组装、页面路由、搜索焦点、保存与异步状态协调 | 路由表重复、全页立即构造放大数据工作；继续添加业务操作会失控 | **暂不整体拆**。壳层组装本就集中；移出数据工作，统一注册表解析即可。保留保存代次和失活令牌 |
| 设置Pages / DataController | Input管理快捷键；Candidates复用布局预览；Dictionary做I/O+过滤+GTK列表；Learning做统计/备份清空；Clipboard做历史操作；Overview聚合；Diagnostics/Updates做任务展示 | Dictionary/Learning在GTK回调中继续累积存储工作；页面busy被当成后端并发保证 | **只拆有问题的边界**：磁盘刷新与内存过滤分开、轻量统计、异步数据操作。其余页面不机械再分Controller |

## 4. 架构违规与已确认的协议缺口

### ARC-01：提交判断绕过快照发布同步（P0）

**文件/函数：** [learning_writer.cpp](../core/src/learning_writer.cpp) `enqueueCommit():63–94`，`run():389–397`，`snapshot():155`。

**依赖关系：** 输入提交线程读 `snapshot_`；后台worker替换同一个shared_ptr对象。`:88`直接解引用，未使用同类既有mutex协议；不可变的被指向对象不代表shared_ptr成员本身可并发读写。

**为何违反：** 违背ALG-SNAPSHOT的一份已发布视图/受控发布边界。新增差量政策先于refreshBeforeEnqueue，决策还可能取清空前世代。

**风险：** 数据竞争与未定义行为机制已确认；崩溃/错误学习未动态复现。先修同步和决策世代，不调整学习公式。

### ARC-02：环境注册与事务暂存引擎生命周期形成锁循环（P0）

**文件/函数：** [personal_engine.cpp](../core/src/personal_engine.cpp) `OpenLmdb():1172–1178`，`RecordLearningBatch():1456,1522`，`~PersonalEngine():257`，`CloseLmdb():1369`。

**依赖关系：** Open持registry锁等待handle锁；批次持handle锁构造staged PersonalEngine，staged退出析构经CloseLmdb再等待registry锁。staged只是内存事务暂存，却继承真实环境注销生命周期。

**为何违反：** 存储所有权混入纯暂存状态，没有统一锁层级；违反ALG-STORE可恢复事务及生命周期边界。

**风险：** 同路径新打开与批次成功/失败退出可构成ABBA。锁环已由调用关系确认；未进行死锁动态复现，不宣称当前桌面已经死锁。

### ARC-03：显示、学习、消费使用多份候选页，选择无版本授权（P0，按本次“极易回归”标准）

**文件/函数：** [fcitx_engine.h](../adapter/fcitx5/include/modernime/fcitx5/fcitx_engine.h) FcitxCandidateWord:54、setPageBoundaries:84；[engine.cpp](../adapter/fcitx5/src/engine.cpp) select:680；[inputmethod.cpp](../adapter/fcitx5/src/inputmethod.cpp) applySettings:363、keyEvent:823；Provider setter:1856。

**依赖关系：** FcitxWord只带Controller指针/下标；Controller先取自己page的文本，再让Provider以相同下标学习/消费。两页generation没有比较，分页回传也不带版本。

**已确认入口：** 文件轮询递增设置代次，下一keyEvent调用applySettings；Provider外观/剪贴板setter会refresh，Controller常规setOptions不发布新页。后续数字/空格可直接选择旧页。`v`页面关闭剪贴板后，旧Command分支仍忽略Provider选择结果并调用openClipboard（engine.cpp:696、1008）。

**为何违反：** CAND-SELECT要求当前展示版本确认；一个下标不是跨版本身份。错误提交、错误学习和剩余输入不一致有共同根因。

**风险边界：** 列表失同步和旧命令可达机制已确认；具体中文错字/错学组合与真实GTK事件竞争尚需夹具验证。鼠标直接调用FcitxWord还缺统一宿主事实确认，不据此认定已发生敏感数据泄漏。

### ARC-04：一次性清理脚本拥有不受控的数据写权限（P0，手动运行时）

**文件/函数：** [cleanse_learning_store.py](../scripts/cleanse_learning_store.py) `main():7–147`。

**依赖关系：** 硬编码HOME路径、个人备份名/时间戳及词黑名单→copy2在线SQLite主文件→重建表并固定reset_revision=1→替换主库/删除WAL和SHM→移动LMDB/删除lock。检查必要备份之前已移动个人bin文件。

**为何违反：** 绕过已有data-guard在线一致备份与Store清空代次协议；一次事故的数据判断被保留为通用维护入口。既不要求停止Writer，也不使用安装隔离参数。

**风险：** 活跃连接/后台旧快照可继续写旧inode或回放旧数据，WAL备份不完整，真实用户数据可被按硬编码政策删除。源码操作已确认；本轮未执行。未发现安装链调用该脚本，不能描述为默认安装自动破坏数据。

### ARC-05：学习政策复制，存储门面反向参与业务决策（P1）

**文件/函数：** Writer `enqueueCommit():63` 与 [learning_store.cpp](../core/src/learning_store.cpp) `recordCommit():378`复制Raw拼音过滤、默认核心词差量策略；`PinyinMatchPolicy::isCoreChineseWord():139`承载学习白名单。

**依赖关系：** Provider产生事实→Writer/Store分别解释；Writer读乐观视图，Store为一次判断构造持久快照；PersonalEngine又include learning_store.h获取事件/helper，Store依赖PersonalEngine。

**为何违反：** 一项学习接受规则没有唯一语义定义，事务执行者解释提交业务意图。正常Writer转已决策Selection后用recordBatch并不重复判定，这是应保留的方向。

**风险：** 增加开关、偏差政策或身份兼容时漏改入口；同一事件在不同快照下被不同接受。公共单事件SQLite写入口还弱于批次事务，未来调用者容易误用。

### ARC-06：统一中文核心之后仍分散插位并按文本丢弃语义身份（P1）

**文件/函数：** Provider `refresh():2025–2324`；[candidate_mixer.cpp](../algorithm/pinyin/src/candidate_mixer.cpp) append:28–35；[candidate_prefixes.cpp](../algorithm/pinyin/src/candidate_prefixes.cpp) 身份合并:83–101。

**依赖关系：** Pipeline文本/读音合并→Prefix文本/读音/消费合并→Mixer和Provider退回仅按text去重；中文Mixer之后还有宏/中英政策。

**为何违反：** CAND-IDENTITY目标要求读音、角色、消费等价才可语义合并；CAND-COMPOSE的全来源协调尚未完成。文档已承认未迁移，不把所有既有插位本身都判成Bug。

**风险：** 新来源复制固定高位，或同文本不同消费被先到项覆盖。身份信息丢弃机制已确认；具体真实拼音反例未建立。UI没有参与语义排序。

### ARC-07：文件解码把外部导入政策传播到可信恢复（P1）

**文件/函数：** PersonalEngine `SaveToFile():2315`、`LoadFromFile():2351`、`ImportFromFile():2480–2486`。

**依赖关系：** Load直接委托Import(false)，扩展/14B解码一律截频至单字3、短语20。

**为何违反：** 文件格式解码、外部输入清洗、合法自有数据恢复是三个不同责任；同一merge布尔参数不能表达信任/恢复意图。

**风险：** 自有高频数据Save→Load不能保持频次。未找到生产路径调用LoadFromFile，影响范围是公共兼容API；不声称运行主库已丢失。外部导入限频必须保留。

### ARC-08：后台存储仍可反向阻塞输入（P1）

**文件/函数：** Writer `enqueueSelection():52`→`refreshBeforeEnqueue():213–237`→LMDB `ResetRevision():2016–2022`；`RecordLearningBatch():1456`。

每次提交代次观察需取后台整批持有的handle锁；观察清空后还同步readState/索引重建/rebase。ALG-SNAPSHOT要求输入不承担后台整库重放，异步队列并未完全隔离这些工作。机制已确认，当前延迟未测。现有Writer忙锁/并发基准主要用SQLite，不能证明LMDB同样达标。

### ARC-09：比较器契约只在主Ranker得到保护（P1）

**文件/函数：** [learning_snapshot.cpp](../core/src/learning_snapshot.cpp) recallCandidates:1305、recallEnglishWords:1324；PersonalEngine四种Query:836/903/1022/1135及Prune:2592。

多个检索/剪枝各自复制“差值大于epsilon比分，否则比较身份/时间”的近似比较。分数`0、0.75ε、1.5ε`与递增身份可形成a<b、b<c、c<a。不是“分数公式应该统一”，而是严格弱序契约没覆盖所有std::sort入口。当前词库是否触发未验证；主Ranker传递性测试不能替代这些比较器测试。

### ARC-10：更新操作的所有权和资源预算不覆盖完整生命周期（P1）

**文件/函数：** [update_controller.cpp](../tools/settings/update_controller.cpp) refreshPackageCache:1070、runSystemUpgrade:1139、startBackgroundPackageTracking:290；readAllFromFd:107、onWaitDone:265。

调用先check active再spawn，active仅超时转后台后设置。正常等待阶段由页面busy承担防重，公共API没有原子占槽。输出先写无链接临时文件、再全量读入字符串/全局tracker；600字符裁剪发生在最终显示，不能限制磁盘和内存。

这违反操作从认证到完成由一个所有者管理、资源预算在采集处生效的边界。并发调用保护窗口与无界捕获机制已确认；未复现系统事务损坏或OOM。保留包事务不强杀、退出无SIGPIPE的历史修复，不能简单改回普通管道。

### 其他值得登记的结构改进

| 编号 | 文件/函数与当前问题 | 风险/分类 |
|---|---|---|
| ARC-11 | settings_shell.cpp:490全页构造；DictionaryPage::refresh:346读盘/过滤/重建，onSearchChanged:410每键重做；LearningPage::refresh:136与清空回调:207同步统计/备份；DataController::learningEntryCount:265构造整库快照 | GTK主循环承担无关数据工作，数据规模放大成本；实际卡顿未测。P2，先测后分离 |
| ARC-12 | SettingsStore统一格式，但adapter与UI独立mtime轮询；runtime.state、settingsGeneration、UI度量版本分散；页面注册、CLI、窗口包装手工映射 | 合法副本缺一致应用协议，新增设置/页面影响面扩大。P2；优先ARC-03后再收敛 |
| ARC-13 | tests/CMakeLists.txt条件注册GTest/Benchmark；普通CI未统一检查必需测试存在，发行DEB job已检查；core/UI依赖较粗 | 缺测试可能被误记全绿，纯政策测试仍带存储依赖；P2。不能说所有CI均无门禁 |
| ARC-14 | generate_binary_installer.sh:11–44按install.sh精确文本锚点切出构建段；源码/二进制payload回滚各有实现；install_state.sh依赖调用者全局managed_state_dir | 已有锚点失败保护，但内部文本形状成为隐式API；回滚政策容易漂移。P2，先固定渠道行为矩阵 |
| ARC-15 | HANDOVER与现结构/标准/测试数不一致；演化绝对保证与部分当前语义不符；候选迁移状态描述粗粒度 | 后续AI容易按旧事实重复实现/削弱保护。P2，仅更新当前事实和状态，不删历史 |

## 5. 重复与分散逻辑

| 类别 | 实际位置/内容 | 应收敛什么；不要误合并什么 |
|---|---|---|
| 重复学习政策 | Writer/Store的recordCommit，匹配类核心词表 | 共享纯决策语义和输入状态契约；Writer仍拥有队列与乐观视图，后端仍拥有事务 |
| 重复事件变换 | Snapshot、SQLite applySelection、LMDB RecordLearningBatch | 三路同事件回放后提取共同规范化/别名/屏蔽语义；LMDB清洗12字符与SQLite/Snapshot原样存上下文的现行差异先明示 |
| 重复算法 | Snapshot/PersonalEngine各类epsilon比较器；多处UTF-8与拼音规范化 | 仅共享已证明相同的身份/顺序规则；不统一7天/30天/12小时衰减 |
| 重复候选规则 | Pipeline/Prefix/Mixer/Provider不同去重键；后置模糊/宏/中英插位 | 身份先统一，阶段顺序和政策仍保留；不把英文与中文放进一个无差别分数 |
| 重复状态 | Provider/Controller/Fcitx/UI的页与边界，Controller/Provider fallback上下文 | 页面需版本绑定；fallback分别服务宿主连续事实与standalone/部分刷新，不能未经联测删一份 |
| 重复配置读取 | adapter轮询、UI节流轮询、设置端草稿/文件hash | 同一应用版本与解析/路径契约；草稿≠已保存≠已应用，保留不同状态 |
| 重复持久化/回滚 | Store/PersonalEngine/data-guard/cleanse；源码与binary payload安装 | 建立管理写权限与格式兼容契约，关闭一次性旁路；不把所有事务物理实现合成通用存储类 |
| 重复进程/错误处理 | runtime_controller、update_controller、session-init的执行/超时/诊断 | 可先在设置端共用有界执行协议，查询和不可强杀长事务保持差别；bool返回成功/跳过/仅乐观接受的含义应明示 |
| 重复配置/发行知识 | README、CMake、包元数据、CI依赖表；页面注册与CLI/窗口映射 | 小型声明表/检查即可，不建设通用反射配置框架 |

没有发现“helper/utils目录泛滥”本身是当前主因。更明显的是大类内局部函数和setter隐藏了政策/刷新/生命周期副作用。把这些简单搬到Helper，依赖、状态与修改范围都不会下降。

## 6. Single Source of Truth 审查

| 概念 | 当前权威/实际写入者 | 是否清晰；目标约束 |
|---|---|---|
| 中文特征 | candidate_features | 清晰；前缀自身证据由candidate_prefixes提供，不复制父学习 |
| 中文组内顺序 | CandidateRanker | 清晰；召回/英文检索有自己的有界排序，不能说全仓只有一个排序器 |
| 最终全来源顺序 | Provider最后写入；Mixer只覆盖中文 | **政策不唯一**；所有既有插位纳入一个可解释的组合协调边界 |
| 候选语义身份 | Pipeline文本/读音，Prefix加消费，末端仅文本 | **不统一**；区分语义合并与视觉去重，显式角色和消费 |
| 候选可见性/屏蔽 | Store持久状态→Writer或只读visibility快照；Provider即时mask/多阶段过滤 | 持久权威存在；副本/过滤口需受同一版本和清空边界约束，不能随意删最后屏蔽 |
| 用户学习 | Writer为正常异步写所有者；Provider/Controller供事实；Store/PersonalEngine执行 | 发布所有者清楚，**接受政策不唯一**；禁止新增来源专用写库通道 |
| 造词/近期选择 | 单Provider Tracker/RecentChoices，共享正常PhraseStats | 边界较清楚；近期窗口不共享/不落盘，禁学/敏感不积累共享统计 |
| 原始输入/编辑光标 | Controller编辑状态与LibIME context | 有同步接缝；提交结果应一次返回选中身份/剩余组合，避免各层猜测 |
| 选择授权 | Controller，但调用只带下标 | **不完整**；展示版本+身份+下标一起验证后学习/消费/上屏 |
| 几何分页 | CandidatePagination计算；Controller/Fcitx存边界；无UI固定回退 | 计算职责明确，回传无版本；不需消灭必要的降级路径 |
| 配置 | 磁盘SettingsStore；设置Model草稿；adapter已应用；UI度量 | 合法多版本，**缺统一应用协议**；不能合并成一个全局可变settings |
| runtime状态 | adapter写runtime.state，设置端RuntimeStatus/探测 | 声明状态与实际进程事实不同；诊断必须带采样/应用版本，不能互相覆盖当唯一真相 |
| 用户路径/活动后端 | SettingsPaths构造；PinyinDataPaths/安装脚本/Environment另作选择 | **跨进程协议不完整**；明确覆盖优先级及SQLite/LMDB实际后端，优先补契约测试 |
| 更新状态 | UpdateStatus、页面busy、全局packageTracker | **操作所有权不完整**；页面展示快照不能代替spawn前原子占槽 |
| 持久数据世代 | Store事务中的resetRevision | 正常路径有权威；cleanse固定重置破坏单调协议，必须隔离 |

## 7. 架构债务结论

**项目已出现局部“屎山化”趋势，但尚未发展成全仓无边界的单体。** 证据不是文件大，而是新增保护规则开始跨越原有职责：差量学习判断复制进Writer和Store并绕过同步；事务暂存复用完整PersonalEngine引入环境注销锁；候选setter可刷新Provider却不发布展示页；一次事故的清理规则仍能直接操作真实用户库；更新进程执行与事务状态持续叠加在同一个控制器。

反向证据同样重要：core没有UI反向依赖；候选核心已有真实模块；不可变快照、事务批次、清空代次、EngineHost、假进程与隔离安装验证都提供了可渐进演进的基础。退格留字、宽度分页、队列重试的许多分支是成熟行为和故障保护，不能统称临时补丁后删掉。

最危险的增长模式是：新需求先找“最容易插if的现有函数”，再依赖调用顺序修补同步，最后用局部评分/频次测试证明成功。应把验收转到最终身份、页面、提交、学习和数据世代，并让新增功能只能进入既定阶段。

### 7.1 文档与真实结构不一致

- HANDOVER仍写旧`src/engine`、`src/ui`目录、C++17、CMake≥3.20及固定48测试；根构建实际为C++20、CMake≥3.24，本次已配置测试列表66项。旧提交号/“最新”状态不能作当前基线。
- 演化第13项“核心词频次保持0”需注明例外：当前hasPositiveFrequency为真仍接受默认选择；它不是无条件永远归零。
- “无损二进制加载”需区分ARC-07的可信恢复与外部导入限频；“永不损坏”“任意导入永不污染”等超过已有验证。
- 总架构M0–M4和候选后续验收应逐项写已实现/部分/未实现。候选文档后段笼统称派生证据/曲线/配额待新增，前段及GTest已实现其中一部分。
- 特性9的256分片与特性10的512是历史演进；候选第2节是明确的修复前记录。它们不应被改写成当前退化证据。统一全来源Composer、完整身份/trace及契约CI尚未完成的标注应保留。

## 8. 最值得优先处理的五个问题

| 优先序 | 问题 | 先做什么 |
|---|---|---|
| 1 | 学习并发所有权不闭合（ARC-01/02） | 分别固定快照读写交错和LMDB锁环失败证据，再修同步/句柄生命周期；不要同时改算法 |
| 2 | 候选展示、选择、学习与消费无同版本身份（ARC-03） | 建版本化选择/分页回传，设置批次应用后发布一致页；验证显示=上屏=学习=消费 |
| 3 | 用户数据维护/恢复意图缺边界（ARC-04/07） | 先阻止事故脚本误用真实库；可信恢复与外部导入分流，保留导入限频和数据格式 |
| 4 | Provider最终组合与身份仍有旁路（ARC-05/06） | 先统一身份与学习决策契约，再逐项提取现有插位政策；保留默认行为与操作顺序 |
| 5 | 更新控制器没有完整操作所有权/资源预算（ARC-10） | spawn前占槽贯穿事务；捕获时限制资源，进程执行与APT/DNF政策分离 |

这里将同一根因域聚合成五项，不表示合并成五个大PR；具体小任务见整改清单。

### 8.1 现在就应该处理什么

先处理P0四项；其次固定学习接受/恢复、LMDB热路径和比较器的失败/性能证据。这些是边界修复和验收基础，不需要先完成架构大迁移。Provider和更新控制器按一种政策一次迁移。

### 8.2 暂时不要动什么

不要统一三种衰减、削减200,000容量、把独立引擎160,000剪枝目标改成另一后端政策；不要重写存储格式、引入动态算法插件、把英文与中文强制总排序；不要为了收敛上下文删掉宿主有效性/敏感/禁学清理；不要通过提前屏蔽改变解码分数归一化顺序。

### 8.3 哪些大文件不需要整体拆

LearningSnapshot的分片/索引/不可变视图主体，PinyinMatch的解析匹配主体，Controller的宿主退格确认状态机，SettingsShell的GTK组装路由主体，都没有“必须整体拆”的证据。优先剥离明确越界职责，而非制定文件行数上限。PersonalEngine、Provider、UpdateController才有多种生命周期/政策混杂的明确拆分依据。

### 8.4 哪些地方最容易被AI持续开发堆坏

Provider::refresh/select、Controller::handle/setContext/finalizeRecentCommitDeletion、Writer/Store的recordCommit、PersonalEngine的Open/Import/RecordBatch、UpdateController的runSubprocess与后台tracker，以及Dictionary/Learning页面回调。它们既容易插补丁，又能跨越排序、状态、数据或生命周期边界；局部测试容易掩盖全链路影响。

### 8.5 AI新增功能必须遵守的架构护栏

1. 先声明ALG/CAND契约、阶段、输入输出、可写状态、时间/身份/版本含义和消费者；没有明确边界不得新增Manager/Helper。
2. 新中文来源进入Pipeline；评分进入Features/Ranker；前缀读自身证据；席位进入组合政策；UI只做几何。未迁移的英文/宏模式只能扩展命名政策，不增加末端任意插位。
3. 选择以同一已确认身份和展示版本驱动上屏、消费、学习；重载返回失效/版本，不允许setter悄悄换列表。
4. 学习资格先于Tracker/共享统计修改；正常事件只沿既有Writer，后端只执行已决策事件；敏感、禁学、屏蔽、清空和消费范围不能被新特性豁免。
5. 共享指针发布、锁层级、句柄析构与线程归属要写明；内存暂存不能继承磁盘生命周期；bool设置项不等于需要合并的运行状态。
6. 管理脚本必须显式限定对象、备份/预览/恢复语义和世代；外部导入≠自有恢复；不得直接删除活跃WAL/lock或静默清用户数据。
7. 不在按键/GTK回调增加整库I/O；每个来源、队列、输出、缓存声明预算，性能分别测召回/更新/后台竞争/页面工作。
8. 无意图变化保留原断言；联测最终全局位置、物理页面、键鼠提交、消费、持久化/重开和禁学/敏感边界。未注册、跳过、旧二进制均不能作为当前通过证据。
9. 结构调整一次只缩减一种职责或状态权限；不同时重写公式、索引、schema和安装；较大改动遵守完整CTest/GTest/Benchmark及隔离fresh-clone闭环。
10. 交付时记录减少了哪些责任、依赖、状态和联动修改点；仅文件变小、类变多不算完成。文档严格区分历史、现状、目标和实测范围。

---

## 9. 架构整改最终验收状态与证据链接（2026-10-18 T19 组合基线验收更新）

本审查文档记录 2026-10-16 架构初审之历史事实。所有发现项均已纳入并行整改计划（T00–T18），并于 2026-10-18 由 T19 在独立工作树分别针对历史基线 `631d258` 与最新组合实现基线 `0cfb0bf` 执行了全量集成验收审计。

- **整改验收报告**：详见 [docs/ARCHITECTURE_REMEDIATION_ACCEPTANCE.md](ARCHITECTURE_REMEDIATION_ACCEPTANCE.md)。
- **测试证据与状态比对**：
  - **历史基线 `631d258`**：存在 4 项自动化测试失败（Debug 1 项、Release 3 项）及 1 项性能门限超标（并发 SQLite 最大延迟 35.25ms > 30ms），T18 未集成。
  - **基线 2 (`0cfb0bf`)**：
    - T18 统一页面解析与 DBus 导航已合入，`settings_navigation_dbus` 在 Debug 与 Release 均通过；
    - 旧基线中 `settings_data_async_gtk`（心跳与清空断言）及 `install_runtime`（临时目录测试）均已完全修复；
    - 独立空闲机运行 Release Benchmark 达到 20.82ms（< 30ms 门限，exit code 0）；
    - 但 Debug 全量 CTest 串行下存在 3 项失败（`settings_shell_gtk`、`update_controller`、`process_runner`；单测独立执行均全绿）；
    - Release 全量 CTest 串行下 `personal_engine_benchmark` 并发修剪延迟达 46.53ms，超出 30ms 门限；
    - `tests/verify_fresh_clone.sh` 全新克隆沙箱测试返回 exit code 8；
    - 正式契约检查工具 `verify_required_tests.py` 对 Debug 和 Release 证据均返回 exit code 1。
  - **前序实现基线 (`bc2d190`)**：
    - R1 读事务环境寿命下沉闭环，原生与 GNU wrap 探针全场景 exit 0；
    - 但该提交在 `pthread_atfork` 中清空继承队列引发分叉子进程二次释放（Double-Free），历史提交 `f87dab6` 中的最终签署因此被正式撤销作废。
  - **前序实现基线 (`15ae3fc` / `1fff2a1`) 缺陷消除与实测**：
    - **R2 分叉回收二次释放消除彻底闭环 (`15ae3fc`)**：彻底移除了 `tools/settings/process_runner.cpp` 中存在二次释放的 `pthread_atfork` 子回调，客户端退出改 fresh exec，确定性回归夹具退出 0，进程套件实测 35/35 全部通过；定向 Debug/Release/ASAN 均 3/3 通过；
    - **G4-L1 换库持锁析构旧环境潜在锁序环消除彻底闭环 (`1fff2a1`)**：在 `EnvironmentRegistry::SwitchEnvironment` 中将旧环境引用延迟到双锁作用域外释放（3 行生产代码变更）；开启 Fcitx5 的 TSAN 136 项全绿（退出码 0，无数据竞争与锁序反转）；Release Benchmark 清空首键 204.77 us、LMDB 并发 3.16 ms、SQLite 并发 20.65 ms、VmRSS 67.14 MiB，8 项负向拒绝自检拦截有效；
    - 旧 TSAN 退出 66 历史证据保留在 `build/final-acceptance-15ae3fc/before-lock-fix/`，不以新日志覆盖旧失败。
  - **最新冻结实现基线 (`0e74b80`) 最终自动化全量验收**：
    - 直接父实现 SHA：`1fff2a16c16ca583ffe3b866acdfbd55c3f316be`，前序关联文档提交 SHA：`0aebef57ca5c15cdc07f370adf9e1784830d4df2`；
    - 变更内容仅为用户授权的测试契约调整：在 `tests/core/personalization_snapshot_regression_test.cpp` 中将 ASAN（含 ASAN/UBSAN）纳入已有的 150ms 插桩预算分支（`EXPECT_LT(slowestMs, 150.0)`）；原生未插桩严格小于 30.0ms 门限（`EXPECT_LT(slowestMs, 30.0)`）、其他功能断言及 Release 性能门限完全未变，生产算法与核心代码 0 字节修改；
    - **本轮独立实测 (`build/final-acceptance-15ae3fc/`)**：
      - **原生 Debug 容量回归与 GTest 全量**：指定容量用例耗时 ~8.93ms（严格小于 30ms），原生 GTest 136/136 项全绿通过，退出码 0；
      - **ASAN/UBSAN 核心引擎**：136 项全部通过，退出码 0，0 内存破坏/UB/UAF/泄漏，容量回归在已授权 150.0ms 插桩预算内通过；
      - **G5 全生命周期全新克隆**：`tests/verify_fresh_clone.sh` Stage 1~6 闭环通过（内部 78 项测试 72 Pass、6 Skip 规范跳过、0 Fail；安装产物及内部采集器校验通过，卸载清理完全，退出码 0，`FRESH_CLONE_EXIT=0`；G5 日志转换 JUnit，Skip 耗时以原始日志为准）；
      - **G7 自动化测试强契约**：检查器独立读取注册表（78 项）、转换 JUnit 及原生 136 项 GTest XML，输出 `[OK]`，退出码 0。
    - **父基线 (`1fff2a1`) 一致性复用（严禁标成本轮重跑）**：
      - **TSAN 136 项全量**：复用父基线开启 Fcitx5 的实测结果（退出码 0，0 竞争 0 死锁，宏分支等价，生产代码 0 字节变化）；
      - **G3 Release 性能门禁**：复用父基线锁序修复后的 Release Benchmark 达标实测（清空首键 204.77 us < 5000 us，LMDB 并发 3.16 ms < 30 ms，SQLite 并发 20.65 ms < 30 ms，VmRSS 67.14 MiB）及 8 项负向拒绝自检通过（生产性能门限未变，测试预算调整不影响生产二进制）；
      - **ASAN 控制器与 GTK 套件**：复用已归档结果（控制器 4/4 退出 0；GTK 5 大套件在库级规则下未报告未抑制泄漏，不据此宣称业务代码零泄漏）；
      - **历史失败保留**：保留旧 TSAN 退出 66 与旧 ASAN 超限退出 1 历史记录。
- **最终验收裁定**：**【自动化验收通过，真实物理桌面待验】**。
  - 正式确认历史提交 `f87dab6` 中的过早交付签署作废；`488c390` 保留为历史阶段报告；
  - 全部 15 项技术债务及整改任务逻辑闭环，R1、R2、G4-L1 三大深层缺陷消除闭环；
  - 自动化门禁全量通过，G6 真实物理桌面人工点验依规保持 **物理待验 (Pending)**；
  - 自动化收尾循环结束，达到交付与文档归档标准，正式交真实物理桌面验证。





