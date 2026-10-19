# ModernIME 架构整改解决方案与验收执行计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 本方案交给另一位AI执行；当前编写会话不实施代码。未获用户明确安排时，不自行创建其他聊天或部署、发布。

**Goal:** 消除审查中的核心并发/数据/选择授权缺口，按职责逐步收敛15项技术债，并以可复核证据证明成熟打字体验、数据兼容和安装生命周期未退化。

**Architecture:** 保留 core→algorithm→adapter/UI 的现有构建方向和不可变学习视图。先修状态/资源所有权，再统一提交身份和学习接受语义，最后提取候选组合、更新进程、设置数据与安装阶段；一次只处理一个可独立验收的责任。

**Tech Stack:** C++20、CMake≥3.24、LibIME、Fcitx5、GTK3/PangoCairo、SQLite/LMDB、Python≥3.10、Bash、CTest/GTest/Benchmark。默认不引入新系统依赖。

**Spec:** [全仓审查](../../ARCHITECTURE_AUDIT.md)、[整改清单](../../REFACTOR_BACKLOG.md)，以及仓库根 `AGENTS.md` 和已有算法/候选架构、变更指南、演化、稳定性、交接文档。

**基线与状态：** 审查基线为2026-10-16的`8526bf5182f168f9d1b5af30245fa88372430313`；方案编写期间已复核后续界面提交至`5389c3b45ae0033ee778718764935df646dadb98`（更新页隐藏原始源地址、学习页简化技术信息，底层同步数据工作仍存在）。执行以T00采集的当前HEAD/工作树为准。此文件全部为拟实施设计；其中的新类型、文件、测试、CMake选项和验收脚本尚不存在。此前只盘点过66个已配置CTest，不能当成当前代码已通过。本计划不要求固定测试总数。

## 1. 执行边界与不可退化约束

- 默认最大学习容量 **200,000**；独立PersonalEngine超限剪枝目标 **160,000**；Store的LMDB门面及其他后端保留现行策略差异。
- PersonalEngine查询 **7天**、Snapshot学习增益 **30天**、Ranker相对休眠 **12小时**，用途/单位独立。512分片、不可变旧快照、全局前缀扫描256/最多16结果、原生128/纠错16评分预算保留。
- 故障待写队列最多100事件，不把它当词库容量。不能以延迟为由削减容量、缩小成熟召回范围或丢合法消费。
- 高频中文短码保护、一次偶选防夺位、正常重复选择晋升、技术英文补全、全/简/混拼、纠错、分段造词和取字/留字保留。
- 敏感/禁学在Tracker和共享统计修改前拦截；持久屏蔽、用户清空、上下文有效性和消费范围是公共边界，新来源不能例外。
- schema版本、resetRevision、snapshotVersion、候选展示版本、布局版本不能互换；不修改SQLite/LMDB/14B/扩展二进制格式，不静默迁移、清理或覆盖真实用户库。
- 保留中文管线现有评分/归一化/屏蔽的操作顺序；提前过滤、调整公式或新增插位属于另一个行为变更，禁止混入提取。
- 新依赖必须同步README、根/模块CMake和包依赖。安装默认离线，HTTPS克隆入口及已有隔离环境参数有效，安装/卸载清单对称，用户改过的托管文件受保护。
- 保留后续界面提交的产品表达：不把数据库后端、reset_revision或原始软件源地址重新塞回设置主流程；诊断证据进入受控测试/诊断输出。
- 不整体拆LearningSnapshot、拼音匹配主体、Controller退格状态机或SettingsShell。新文件必须承担明确权限/生命周期边界；不建设通用Manager/Utils/规则引擎。
- 只在隔离库、假包管理器及测试宿主中验证；不要替用户重装日常输入法、运行真实apt/dnf升级、恢复旧个人库、推送、合并或发布。实现分支与本地小提交可按用户交付授权和环境能力执行，暂存精确路径，不用`git add -A`卷入无关修改。

**已知有意修正，仅限对应任务：** 未同步读写/死锁、陈旧选择与分页、危险脚本默认可执行、可信恢复截频、非传递比较器、非等价身份被去重、重复包操作与无界捕获。其余差异先调查，不得批量更新旧断言。异步清空的过渡状态见T11，不能把它当免费行为变化。

## 2. 方案选择与最终责任分配

采用“最小协议修复＋静态职责提取”。仅搬文件无法降低写权限与状态数量；整体重写候选/存储或引入动态插件，会同时改变行为、数据和性能，不采用。

| 责任 | 最终唯一负责者 | 不能越界做的事 |
|---|---|---|
| 宿主能力/上下文事实、提交授权 | inputmethod采集，Controller确认 | 不在core读取Fcitx/GTK；鼠标不能绕过同一确认入口 |
| 组合/LibIME资源 | Provider会话；SharedResources生命周期 | 资源重载必须返回版本/失效信息，不偷偷替换展示授权页 |
| 中文召回/特征/组内排序 | Pipeline / Features / Ranker | 不直接占最终高位或写学习 |
| 全来源最终组合 | candidate_composition静态边界 | 只消费已声明证据/既有模式政策，不读盘、造词或分页 |
| 语义身份与选择结果 | core值类型，Provider确认结果 | UI不猜角色/拼音，Controller不从旧页取另一份提交文本 |
| 物理分页/视图 | CandidatePagination / Layout / Renderer | 不改语义顺序/消费；边界回传绑定展示和布局版本 |
| 学习接受规则 | learning_policy纯函数 | 不自行访问快照成员、磁盘或共享Tracker |
| 快照/队列/世代协调 | LearningWriter | 后台Store不回调UI；每次接受/发布有明确版本 |
| 持久事务/环境生命周期 | Store后端；LMDB环境句柄 | 内存暂存不注册/关闭MDB_env；恢复不走外部导入清洗 |
| 包操作与进程 | 包操作状态持有者；独立process_runner | UI busy不是防重锁；包政策不管理FD/线程 |
| 设置数据 | DataController的纯数据操作与异步调用 | Page不在GTK回调整库读盘/备份/索引 |
| 配置与路径 | SettingsStore/SettingsPaths及明确跨进程协议 | 草稿、已保存、已应用、UI度量仍分别存在 |
| 安装 | 共同生命周期脚本、渠道产物提供者 | 脚本不按源码文本切片、不回滚活跃用户数据 |

## 3. 文件与接口地图

路径均相对仓库根。下列新文件是上限内的明确拆分位置，不是要求立即创建空壳；在所属任务实施时才创建。若已有等价接口，先在执行记录写映射并复用，不能增加第二权威。

| 位置 | 创建/修改 | 唯一用途 |
|---|---|---|
| core/include/modernime/core/learning_event.h | 创建 | 从learning_store.h移出LearningEvent，解除后端/事件头依赖循环 |
| core/include/modernime/core/learning_policy.h；core/src/learning_policy.cpp | 创建 | Raw保护、核心词差量判断、英文选择身份；无I/O |
| core/src/personal_engine_state.h | 创建 | 内部学习记录/索引暂存状态；无环境/线程/析构I/O |
| core/src/lmdb_environment.h；core/src/lmdb_environment.cpp | 创建 | 内部同路径环境槽、初始化/关闭/扩容及生命周期锁 |
| core/include/modernime/core/candidate_model.h；core/src/candidate_model.cpp | 修改 | 身份、选择请求/结果值类型、等价判断 |
| algorithm/pinyin/include/modernime/pinyin/candidate_composition.h；src/candidate_composition.cpp | 创建 | 既有模糊/中文席位/宏/中英/原文组合政策及可选有界移动原因 |
| algorithm/pinyin/src/pinyin_shared_resources.h；src/pinyin_shared_resources.cpp | 创建 | 不透明SharedResources定义与加载/重载；公共工厂仍在Provider接口 |
| tools/settings/include/modernime/settings/detail/process_runner.h；tools/settings/process_runner.cpp | 创建 | 查询/包进程执行、期限、独立采集子进程和输出预算 |
| tools/settings/include/modernime/settings/detail/package_operation_state.h；tools/settings/package_operation_state.cpp | 创建 | 从spawn前到完成的操作槽、ID、状态和一次结果消费 |
| tools/settings现有Model/Page/DataController/registry | 修改 | 复用已有GTask/失活令牌、纯数据接口和页面解析，默认不新建PageManager |
| scripts/install_lifecycle.sh | 创建 | 接收显式参数的共同部署/托管集成/回滚阶段 |
| tests/verify_required_tests.py | 创建 | 检查注册存在性与JUnit执行证据，不能替代行为测试 |
| tests/core、tests/settings、tests/*.sh；相关CMake/README/docs/CI | 按任务修改/新增 | 下文指定行为测试与构建接线；不删除原测试 |

### 3.1 候选协议（T03定义，T08/T09复用）

拟在`modernime::core`定义；这些是接口决定，不是现有实现：

```cpp
struct CandidateIdentity {
    std::string text;
    std::string fullPinyin;
    CommitCandidateRole role;
    std::size_t consumedInputBytes;
    bool operator==(const CandidateIdentity &) const = default;
};
struct CandidateSelectionRequest {
    std::uint64_t sessionId;
    std::uint64_t displayGeneration;
    std::size_t globalIndex;
    CandidateIdentity expected;
};
enum class SelectionAction { CommitText, OpenClipboard, NoCommit };
struct CandidateSelectionOutcome {
    CandidateIdentity selected;
    SelectionAction action;
    std::string remainingInput;
};
```

- `consumedInputBytes`在边界处将旧值0解析为当前rawInput.size()，之后不再含“0代表全部”的歧义。fullPinyin取当前候选声明的读音/完整英文键，不在点击时重新推断。
- `CandidateItem`末尾增加`std::optional<CommitCandidateRole> commitRole`，避免默认Chinese误解释旧Raw/Command。生产Provider发布前显式填写；兼容夹具缺省值由唯一`candidateIdentity(const CandidateItem &, const CandidatePage &)`按旧角色政策解析并测试。T08后生产链不再走缺省路径。
- Controller新增`bool select(const CandidateSelectionRequest &)`。旧`select(size_t)`只作为“从当前页立即选择”的本地包装，不能被FcitxWord或旧UI缓存直接调用。
- Provider新增`std::optional<CandidateSelectionOutcome> selectConfirmed(const CandidateSelectionRequest &, std::uint64_t providerGeneration)`；旧standalone select接口包装当前Provider页。Provider再次核对原生页代次和身份；拒绝时不得学习、消费或改Tracker。
- Controller的sessionId一生唯一；displayGeneration在语义/资格/设置变化时递增且reset不复用；providerGeneration作为发布记录绑定，**不是第二个独立候选页**。纯光标移动不改语义代次；布局另有单调layoutGeneration。
- FcitxWord/List保存请求及弱引用SelectionEndpoint；Endpoint由Controller拥有、销毁时失效，只在宿主事件线程使用，禁止旧列表持裸Controller指针。
- `bool setPageBoundaries(uint64_t displayGeneration, uint64_t layoutGeneration, vector<PageBoundary>)`只接受当前展示版本和不早于当前布局版本的连续全覆盖边界。无UI回退继续有效，旧宽度结果不能覆写新字体布局。
- 新增`PinyinCandidateProvider::applyOptions(const PinyinProviderOptions &)`批量应用，至多refresh一次；adapter经Controller发布新页。Controller上屏只使用Outcome.selected.text，命令只有Outcome确认成功才执行。

### 3.2 学习与文件接口（T05/T06定义）

```cpp
enum class CommitLearningDecision { IgnoreCommand, IgnoreRawPinyin,
                                    IgnoreCoreDefault, Record };
CommitLearningDecision decideCommitLearning(const CommitEvent &event,
                                            bool hasPositiveFrequency);
bool isProtectedCoreDefault(std::string_view text, std::string_view pinyin);
std::string canonicalEnglishSelectionWord(std::string_view phrase,
                                          std::string_view pinyin);
enum class PersonalFileIntent { RestoreTrusted, ImportExternal };
```

这三个学习函数放learning_policy；事实由调用者提供，函数不拿Writer/Store。`LearningEvent`保持现有字段与resetRevision含义，移头文件不改数据格式。旧LoadFromFile包装RestoreTrusted+replace；旧ImportFromFile包装ImportExternal+原merge。内部新增`bool readPersonalFile(const string &, PersonalFileIntent, bool merge)`，先完整解码到暂存状态，再应用政策/发布。

### 3.3 更新进程协议（T12/T13定义）

操作状态：`Idle → Starting → Running → Continuing → Completed`；进程身份/结果无法确认时进入`Uncertain`并继续占槽，只有验证该operationId对应进程已结束或受控人工处置后才可释放；不能因GUI退出直接释放。spawn失败允许Starting→Completed。超时后Continuing仍占槽，Completed的结果绑定operationId且只能消费一次；另一个任务成功占新槽不抹掉尚未消费的旧结果，完成结果队列最多8项并显式报告过载。

`tryBeginPackageOperation(kind) -> optional<uint64_t>`在spawn前原子占槽；`finishPackageOperation(id, result)`只结束同ID。现有UpdateController静态公开API作为兼容门面，不在页面再写并发规则。

输出预算是本计划**新拟定目标**：查询每流最多1MiB，截断则解析不完整、禁止误判“已最新”；包事务保留每流64KiB尾部，超出仍持续排空；采集进程每流驻留不超过1MiB、临时诊断文件每流不超过1MiB；控制状态文件≤4KiB。全局结果不得再保留无限stdout/stderr。

为同时满足有界捕获与窗口退出不SIGPIPE，选用**同一个modernime-settings可执行文件的内部采集子进程模式**，在GTK初始化前分派到process_runner。采集子进程拥有命令管道/回收职责，持续排空并丢弃超预算字节，独立存活到真实包命令结束；GUI只观察有界状态/结果。不得仅在GUI内改成管道，也不得用RLIMIT_FSIZE杀死真实包进程来限制文件。内部调用使用argv和受控环境，无shell拼接；控制文件0600，使用每操作唯一目录，不复用旧PID/文件。若采集器无法启动，命令不得启动；采集器异常后标记状态未知并提示人工检查，禁止自动重试升级或声称事务已结束。


拟在`modernime::settings::detail`定义以下进程接口，使用现有Environment/RuntimeResult：

```cpp
enum class CommandMode { Query, PackageTransaction };
enum class CommandState { NotStarted, Exited, Continuing, Unknown };
struct CaptureLimits {
    std::size_t perStreamBufferBytes;
    std::size_t perStreamResultBytes;
    std::size_t perStreamFileBytes;
};
struct CommandSpec {
    std::vector<std::string> argv;
    Environment environment;
    std::uint32_t timeoutMs;
    CommandMode mode;
    std::uint64_t operationId;
    CaptureLimits limits;
};
struct CommandResult {
    CommandState state;
    std::uint64_t operationId;
    int exitStatus;
    bool stdoutTruncated;
    bool stderrTruncated;
    std::string stdoutText;
    std::string stderrText;
};
CommandResult runCommand(const CommandSpec &spec);
int runCommandCaptureChild(int argc, char **argv);
enum class PackageOperationKind { RefreshCache, SystemUpgrade };
enum class PackageOperationPhase { Idle, Starting, Running,
                                   Continuing, Uncertain, Completed };
std::optional<std::uint64_t> tryBeginPackageOperation(PackageOperationKind kind);
bool advancePackageOperation(std::uint64_t id, PackageOperationPhase expected,
                             PackageOperationPhase next);
bool finishPackageOperation(std::uint64_t id, const RuntimeResult &result);
```

Query结果每流1MiB，PackageTransaction结果每流64KiB，Buffer/File均每流1MiB；状态文件上限4KiB独立于stdout/stderr。内部启动参数如何编码是局部实现选择，但必须使用受控argv/0600状态文件，不能把所有环境变量打印出来或通过shell eval反序列化。

## 4. 依赖和交付单元

| 任务 | 对应清单 | 依赖 | 可独立验收的交付 |
|---|---|---|---|
| T00 | P2-03的准入基础 | 无 | 隔离基线、测试存在性与结果检查工具 |
| T01 | P0-01 | T00 | Writer同步与接受世代修复 |
| T02 | P0-02 | T00 | LMDB无锁环、纯暂存与环境所有权 |
| T03 | P0-03 | T00 | 版本化键鼠选择/分页/设置发布 |
| T04 | P0-04 | T00 | 事故脚本默认拒绝，无活跃库写权限 |
| T05 | P1-01 | T01/T02 | 纯学习决策、三路回放与事务入口 |
| T06 | P1-03 | T02 | 自有文件恢复与外部导入分流 |
| T07 | P1-05 | T00 | 全部学习比较器严格弱序 |
| T08 | P1-02身份部分 | T03 | 全链路语义身份，不等价项不被覆盖 |
| T09 | P1-02组合部分 | T08/T07 | 一个全来源最终组合入口 |
| T10 | P1-02资源/选择协调部分 | T05/T09 | Provider只作会话协调，资源重载有版本 |
| T11 | P1-04 | T02/T05/T10 | LMDB输入预算与清空后台准备 |
| T12 | P1-06操作部分 | T00 | 完整包事务所有权 |
| T13 | P1-06进程部分 | T12 | 有界采集子进程及纯包政策 |
| T14 | P2-01 | T03/T10 | 配置/路径/应用版本协议 |
| T15 | P2-02 | T02/T05 | 设置数据异步、轻量计数/内存搜索 |
| T16 | P2-03构建部分 | T13/T15 | 共享设置source target及必要的目标细分 |
| T17 | P2-04 | T00/T13 | 显式安装阶段，渠道回归 |
| T18 | P2-05 | T14/T16/T17 | 页面统一解析、文档现状准确 |
| T19 | 全部 | T01–T18 | 全量验收及最终交接证据 |

并行执行仅限用户或适用执行技能授权，且不得同时修改同一core/Provider/CMake文件；依赖图表示逻辑可独立，不意味着可以共享同一工作树并发写。每个任务完成一次测试/复核/精确路径提交，失败不得标完成；本地不能提交时保留可审阅diff，不为提交而破坏用户修改。

## 5. Review Focus：五个跨任务漏测高风险条件

| 条件 | 必须得到的行为 | 归属测试 |
|---|---|---|
| 旧FcitxList仍存活，原Controller销毁或新列表长度相同 | 无UAF，旧操作零上屏/学习/消费 | T03 OldListAfterControllerDestruction / SameSizeReplacement |
| 清空完成后第一键到达，后台仍在读旧世代且磁盘随后不可用 | 不复活旧词，首个新事件归新代，故障队列有界 | T01/T11 FirstCommitAfterClearDuringRebase |
| 多实例同路径只读升级、扩容和关闭交错 | 单环境、无死锁、旧映射不悬空 | T02/T11 OpenResizeCloseInterleaving |
| GUI退出后包命令继续大量输出、后代继承FD | 命令可完成、采集有界、不因GUI退出SIGPIPE | T13 ParentExitWithInheritedOutput |
| 安装失败时个人库又有新提交且托管文件被用户改动 | 回滚程序，保留新写入和用户修改 | T17 RollbackPreservesConcurrentUserWrites |

## 6. 逐任务实施步骤

每项均执行：失败证据→最小实现→定向测试→所需完整关卡→复核→精确提交。下文给定的新测试名是要创建的契约，不能拿旧同名二进制代替。测试夹具沿用隔离路径、固定时钟、EngineHost、假进程。可使用窄的测试构建transition hook形成栅栏，禁止`#define private public`，禁止靠sleep概率触发当作并发证明；测试宏/类布局在所有链接目标中一致。

### T00：冻结基线和强制测试证据

**Files:** 修改根CMake、tests/CMake、CI；创建`tests/verify_required_tests.py`、`tests/required_tests_contract_test.py`。证据只存忽略的`build/architecture-evidence/`，最终汇总写文档。

**Interfaces:** `python3 tests/verify_required_tests.py --registered FILE --profile full|storage-tsan [--stage baseline|final] [--junit FILE ... --gtest-xml DIR --execution headless|desktop]`；stage默认baseline，junit可重复，完整参数由本任务一次实现（详见G7），不留到最后补工具。返回0仅表示指定检查通过，缺名/失败/非法跳过返回非0。full要求GTest、Benchmark、拼音/仲裁/Controller/Fcitx/布局/设置/安装相关入口；storage-tsan要求GTest和learning_store。新增`MODERNIME_REQUIRE_FULL_TESTS=ON`配置选项，只有完整验收配置可启用。

- [ ] 记录HEAD、工作树、编译器/LibIME/系统词库版本和校验值；若不等于基线，先重定位ARC入口，保留新的外部提交。
- [ ] 用第7节命令新建Debug/Release目录并构建当前源码，保存注册JSON及基线执行/跳过/失败清单。先不传尚未实现的strict选项。
- [ ] 写检查工具测试：缺GTest、缺Benchmark、任一指定安装测试、JUnit失败、未知skip、空结果均失败；仅注册但无执行结果不能称执行通过。
- [ ] 实现检查工具与strict配置；Python用标准库json/xml/unittest。把最小依赖模式与强制验收模式分开，普通开发不强制全部组件。
- [ ] 将required_tests_contract_test.py注册为modernime_required_tests_contract，直接运行`python3 tests/required_tests_contract_test.py`必须通过。负向运行基准注册JSON删除一个必需名，确认非0；在strict构建中模拟探测不到GTest/Benchmark，配置失败。不得卸载开发机依赖实现负向测试。
- [ ] 保存基线失败为既有缺口，不改断言消音；影响后续任务的环境/注册缺口先处理。通过工具契约测试后提交准入基础。

### T01：Writer同步与世代（P0-01）

**Files:** 修改learning_writer.h/.cpp；创建`tests/core/learning_writer_concurrency_test.cpp`并加入现有GTest目标；Benchmark按需增加对应场景。**Interfaces:** 保留公开enqueue/flush/snapshot；新增私有`enqueueSelectionLocked(...)`，调用前必须持mutex_，无磁盘I/O。

- [ ] 新增`ArchitectureWriterRegression.CommitRacesWithPublication`；并发大量enqueueCommit与worker发布，旧快照始终不变；TSAN旧代码应报告snapshot_竞争。另用受控hook覆盖清空交错，不以TSAN一次无报告证明安全。
- [ ] 新增`DecisionUsesRefreshedRevision`：默认核心词无历史跳过，有历史按原政策记录；外部clear后首事件决策不能使用旧快照，旧代排队事件不得复活。
- [ ] 将观察世代与锁内决策/快照修改/入队组织成明确入口，移除enqueueCommit的裸snapshot_读。不要锁内执行readState或调用会重复锁的公开enqueueSelection。
- [ ] 核查每条snapshot_访问和mutex/storeMutex/observerMutex锁序；保持现有“跳过成功”“仅乐观接受/存储失败”的兼容bool含义并注释。
- [ ] 运行G1、G2、G4对应Writer/存储测试；算法完整G3/G5；保存修复前后竞争报告。确认`EXPECT_EQ(oldSnapshotEntries, before)`、新世代频次正确后提交。

### T02：LMDB生命周期与纯暂存（P0-02）

**Files:** personal_engine.h/.cpp；新建internal state及lmdb_environment文件；core/CMake；创建`tests/core/lmdb_lifecycle_test.cpp`加入GTest。**Interfaces:** PersonalEngine持`shared_ptr<detail::LmdbEnvironment>`；内部state仅含记录/索引/计数/政策输入，析构无I/O。现有Open/Close/RecordLearningBatch签名不变。

- [ ] 新增`ArchitectureLmdbRegression.OpenWhileBatchExits`，在独立合成子进程用栅栏重现registry→handle和handle→registry；测试子进程3秒不退出判失败，CTest设30秒最终超时。覆盖成功、拒绝事件、map-full及容量full暂存退出。
- [ ] 先提取state与对state的既有事件操作；staged/full不再是完整PersonalEngine，不触发CloseLmdb。保持公式、清洗、剪枝及序列一致，T05再收敛共同政策。
- [ ] 同路径注册表保存可初始化的共享slot：锁内只lookup/插入slot，不做LMDB调用/等待handle；释放registry后在slot初始化。失败状态可重试同slot，不能并发创建第二MDB_env。
- [ ] 取消按引擎地址查全局handle的操作路径，环境由成员强引用持有；Close先detach成员，最后引用在registry锁外销毁。定义instance→environment的统一锁序；不得在environment锁内回取registry。
- [ ] 新增`OpenResizeCloseInterleaving`、`ReadOnlyUpgradeSharesEnvironment`、`LastOwnerClosesOnce`，核对失败后DBI/env引用不悬空，原整批回滚/容量测试不变。
- [ ] 运行G1/G2/G4/G5及完整G3，登记锁层级。先不在同提交优化查询锁或改变物理存储，提交生命周期修复。

### T03：版本化选择/分页和一致设置发布（P0-03）

**Files:** candidate_model/provider接口与实现；adapter engine/fcitx_engine/inputmethod；UI addon；Provider选词与options；创建`tests/adapter/candidate_selection_test.cpp`接入GTest，扩展真实input_panel测试。**Interfaces:** 采用3.1节类型和方法，所有生产入口迁移，现有本地包装仅用于当前页。

- [ ] 写`ArchitectureSelectionRegression.SameSizeReplacementRejectsOldWord`：列表A保存word，刷新为同长度B后调用旧word，`commits=0, learnedDelta=0, consumedDelta=0`。
- [ ] 写`OldListAfterControllerDestruction`、`StaleBoundaryAndOldFontRejected`及`ClipboardDisabledDuringComposition`；失败必须归因于旧版本/命令，不能仅因下标越界。
- [ ] 实现身份/结果、会话和展示代次、弱Endpoint；Provider先确认再执行，拒绝零副作用。Controller从Outcome提交，不再先复制旧文本后用Provider另一页学习。
- [ ] 键盘/鼠标统一经宿主事实确认和Controller入口。敏感切换不能通过点击绕过资格；不要把Fcitx采集下推core，EngineHost缺宿主支持时沿既有有效性契约降级。
- [ ] applyOptions批量更新至多一次refresh；adapter发布一次一致页，重载/清空失效明确。Command必须Provider确认成功且当前模式允许后才能打开剪贴板；NoCommit不冒充文本提交。
- [ ] 写当前页数字/鼠标/空格/取字/部分消费等价测试，`nihaoma→你好`剩余`ma`；多次reset代次不复用，纯光标移动仍可合法选择。
- [ ] 运行G1的候选/Controller/Fcitx/布局组与G2、G3/G5、G6；旧列表销毁测试以ASAN验证。记录所有接口调用点均已迁移后提交。

### T04：关闭事故脚本的常规写入口（P0-04）

**Files:** `scripts/cleanse_learning_store.py`；新建`tests/cleanse_guard_test.sh`并注册`modernime_cleanse_guard`；相关文档。**Interfaces:** 保留原路径为拒绝入口，打印历史用途并以非0退出；任何参数都不得写库。事故算法保存在git历史和带日期说明，不另留可默认执行的副本。

- [ ] 在合成home/data树准备db/WAL/SHM/LMDB/lock/bin与备份，记录完整目录内容hash；活跃Writer在栅栏等待且不提交事件，避免自身写入污染hash比较；测试无参数、缺备份、伪参数、活跃Writer时都零内容改变。
- [ ] 将入口改成明确停用并返回非0，在任何mkdir/move/open动作之前退出。不要在本次顺手产品化数据清理或迁移格式。
- [ ] 保留data-guard在线备份和Store.clear为受控路径；文档注明程序回滚保留活跃新数据。以后若要清洗需独立用户需求和合成夹具，不复用事故黑名单。
- [ ] G1运行cleanse_guard/upgrade_data_protection及安装组；G3/G5验证生命周期。确认断言`afterTreeHash == beforeTreeHash`，提交最小止血。

### T05：学习政策与三路事件一致性（P1-01）

**Files:** 新learning_event/policy；Writer/Store/Snapshot/PersonalEngine/pinyin_match；创建`tests/core/learning_semantics_test.cpp`加入GTest。**Interfaces:** 采用3.2；`LearningStore::recordBatch`执行已接受事件，单事件接口委托单元素批次，不能递归调用自己。

- [ ] 写固定时钟事件流回放，覆盖Raw jianpan、真实English、默认核心词有/无正历史、次选、上下文、部分选择、反馈/屏蔽/恢复、清空及失效事件；比较Snapshot/SQLite/LMDB共同字段。
- [ ] 对长上下文/控制字符/空前文fallback、拼音空白/分隔符、饱和频次和容量差异生成显式差异表；保留不同后端已声明语义，不直接统一为某后端。
- [ ] 提取纯decideCommitLearning和身份函数；Writer在T01受控视图下调用，Store兼容recordCommit提供持久事实；移走isCoreChineseWord学习词表，旧入口保留委托兼容直至调用者迁完。
- [ ] LearningEvent移出Store头；PersonalEngine不再依赖learning_store.h取事件/helper；内部state复用同语义函数，物理索引、LMDB事务、Snapshot分片保持独立。
- [ ] 单事件SQLite接口统一事务覆盖解除屏蔽、计数、上下文/容量裁剪；新增故障注入断言“返回失败则无半个事件”；批次重试不得重新执行提交接受判断。
- [ ] G1/G2/G3/G5；记录“SkipCoreDefault仅在无正频次时成立”等精确语义，追加演化证据后提交。新来源只供CommitEvent，不加后端专用学习分支。

### T06：可信文件恢复与外部导入（P1-03）

**Files:** PersonalEngine文件API和解码；创建`tests/core/personal_engine_file_test.cpp`加入GTest。**Interfaces:** 采用PersonalFileIntent/readPersonalFile，旧公开签名兼容。

- [ ] 写`ArchitecturePersonalFileRegression.TrustedRoundTripPreservesMetadata`：单字1000次、短语2000次，加上下文/反馈/屏蔽/时间；Save→Load导出共同字段完全相等。
- [ ] 写`ExternalImportRetainsFrequencyCaps`：每条导入记录单字≤3、短语≤20，覆盖14B/扩展格式、merge=true/false、重复导入；merge保留原累加/饱和语义，不把已有本地真实频次强制截到3/20。
- [ ] 解码只验证格式并产生完整暂存记录；RestoreTrusted不截频，ImportExternal保持原政策；非法/截断文件在任何替换前失败。
- [ ] 磁盘持久化/内存发布成功前不得clear原状态；旧bin自动导入仍为External，禁止从文件名猜可信来源。
- [ ] G1/G2/G3/G5；原格式字节夹具可读、失败零部分覆盖；记录无生产Load调用的真实影响范围后提交。

### T07：严格弱序与确定性淘汰（P1-05）

**Files:** Snapshot/PersonalEngine各sort/Prune；创建`tests/core/learning_order_test.cpp`加入GTest。**Interfaces:** 内部比较key只服务排序，不改任何分数/时间公式。

- [ ] `ArchitectureLearningOrderRegression.EpsilonTripleIsTransitive`覆盖`0、0.75ε、1.5ε`、全部6种排列、身份/频次/时间平分，以及等价传递性。
- [ ] 选用精确有限分数降序＋现有tie-break；非有限分数统一视为最低有效证据，后接稳定身份；Prune保留原保留优先级方向。明确`-0/+0`同分，禁止再以pairwise epsilon判断相等。
- [ ] 更新每个独立比较器，检查快照/英语召回、四种Personal查询和淘汰；不以修Ranker一处代替全路径。
- [ ] 用固定词库/时钟对照排序；只允许已声明的epsilon附近顺序/淘汰差异；主干高频、屏蔽、容量及选择曲线不变。
- [ ] G1/G2/G3/G5，记录允许差异及原因；此项作为正确性修正独立提交。

### T08：全链路候选身份（P1-02第一部分）

**Files:** 3.1类型；Pipeline/Prefix/Mixer/Provider；候选回归。**Interfaces:** `candidateIdentity`唯一等价定义；来源集合/评分证据与提交身份分开。

- [ ] 写`ArchitectureCandidateIdentityRegression.SameTextDifferentConsumptionSurvives`、`DifferentReadingAndRoleSurvive`、`EquivalentOriginsMergeOnce`，贯穿Pipeline→Prefix→Mixer→最终列表，验证自身学习与消费。
- [ ] 移除Mixer/Provider仅text丢弃非等价项；所有生成来源显式填commitRole，匹配规范读音与有效消费；等价项合并来源，不叠加重复学习/父词学习。
- [ ] 同文本非等价项仍可分别选择；只在歧义存在时由View展示读音/保留后续拼音提示，annotation不得改提交text。该可见差异单独留夹具，不能批量变更列表金样。
- [ ] G1候选全组、G2/G3/G5/G6；每个显示项映射唯一Outcome，重复选择持久词频只增加一次后提交。

### T09：全来源组合边界（P1-02第二部分）

**Files:** 新candidate_composition；Provider及已有Mixer/Prefix；创建`tests/core/pinyin_composition_trace_test.cpp`加入GTest。**Interfaces:** `composeCandidates(const CandidateCompositionInput &) -> CandidateCompositionResult`，Input为当前请求模式/原输入、已评分中文/前缀、英文/宏/原文/命令证据和资格事实；Result为items及可选moves。类型定义在对应头，禁止持Writer/Store/LibIME资源指针。

- [ ] 冻结现有阶段顺序与固定位置政策、冷启动配额、中英短码/技术词、v金额/剪贴板命令、个人前缀席位共存夹具。
- [ ] 按“模糊→中文多粒度→宏→中英/原文/命令→原位置最终屏蔽/去重”的现状逐项提取，不在提取时更换打分/席位算法；最终屏蔽仍有统一兜底。
- [ ] Provider只采集证据并调用一次最终组合；任何后置insert/remove政策迁入该边界。已有中文Mixer继续是内部中文政策，不访问存储。
- [ ] 可选moves最多256条，只记来源类别、阶段下标和原因枚举，默认关闭、敏感禁用、不落盘/不记录周边原文；不建设完整诊断平台。
- [ ] G1/G2/G3/G5/G6；学习曲线同时记录组合后全局位置和物理页，确认Provider不再另改最终语义列表后提交。

### T10：Provider会话、资源和选择协调分工（P1-02第三部分）

**Files:** Provider；新pinyin_shared_resources；inputmethod；学习/资源回归。**Interfaces:** 资源公开工厂签名保持，内部新增`ResourceChange { dictionaryGeneration, learningRevision, changed }`返回重载结果；Provider内一个选择→CommitEvent函数复用T03身份/T05政策。

- [ ] 资源共享/跨Provider、词典热重载、禁学只读visibility、不建新库、普通→敏感→普通、reset/失焦和RecentChoices隔离测试固定原行为。
- [ ] 移出SharedResources加载/热重载实现；只在后台初始化或宿主资源线程完成可变资源工作，声明LibIME访问线程，不凭shared_ptr宣称资源线程安全。
- [ ] 重载结果通知当前会话页失效/重建，沿T03发布；清空还清正常造词统计与近期意图的既有边界，不能跨会话保留敏感证据。
- [ ] 选择/取字/组词通过同一事件身份构造；Controller仍拥有退格事实，不搬到Ranker/Store；消除当前不可达native select分支前先用消费/角色回放证明等价。
- [ ] G1/G2/G3/G5；资源共享不共享会话意图；新增来源不需改资源析构/Controller按键逻辑，提交职责提取。

### T11：输入延迟与清空过渡（P1-04）

**Files:** Writer/Store/内部LMDB环境/Provider资源；Benchmark及writer回归。**Interfaces:** 现有readonly revision接口保留；Writer新增显式`EpochViewState { Ready, Reloading }`，不是多个互斥bool。后台准备结果携带resetRevision与capturedVersion。

- [ ] 先补LMDB Writer的50k/200k正常、并发写/容量剪枝、首次observer、clear后首键、禁学启学基准；时间包含enqueue/提交政策+快照发布+下一键召回，不能仅测cursor。保存旧实现超过或通过门限的真实结果。
- [ ] 环境生命周期改用共享读写门：正常read/write事务持共享生命周期锁，允许LMDB读与写并行；关闭/只读升级/map扩容持独占门。map-full必须abort并释放事务及共享门后再独占扩容，核对所有DBI/env访问，禁止持共享门升级锁。
- [ ] 每次提交只作轻量世代读取，不等待后台整批私有state/索引锁；新世代立刻使旧快照/页失效。整库读取/索引/重放在后台、版本匹配后发布，旧对象释放移出输入锁。
- [ ] **过渡决策固定如下：** Reloading时不以旧学习/旧屏蔽结果生成或授权候选；保留预编辑并显示短暂“正在更新候选”，旧数字/空格/鼠标选择拒绝且不能改成自动提交拼音原文。只有用户显式Enter才保留既有原文提交能力；合法命令仍必须当前授权。合格的新提交事实标记已观察的新世代并保留有界队列；暂存待决策CommitEvent，不提前转换成Selection，敏感/禁学事实禁止入队。依赖基线正频次的决策等后台新视图就绪按原顺序执行，不能把“未知”当0；所有入队事实沿同一worker先决策/乐观发布再执行已接受事件，同代新事实不被旧后台结果覆盖。
- [ ] 这项临时可见性变化必须在T11单独记录为明确降级，不称纯重构；后台就绪后原始组合自动重建，正确性/屏蔽优先，不为了保持瞬时列表使用旧世代。若无法满足首事件归属、零敏感副作用和有界队列，任务失败，保留原正确路径并报告未完成，不能跳过清空观察。
- [ ] 写`FirstCommitAfterClearDuringRebase`、`NewEpochSuppressionNotExposedWhileLoading`、`ClearThenStorageFailureBoundedQueue`、`EnableLearningDoesNotConstructWriterOnInputThread`；加载期间保留输入/光标，不悄悄自动上屏。
- [ ] G1/G2/G3/G4/G5/G6；正常/竞争门限见8节，另记录降级持续时间与失败可恢复性；不把故障永久Reloading记完成。提交性能边界，文档明确新过渡。

### T12：完整包操作所有权（P1-06第一部分）

**Files:** UpdateController及package_operation_state；update_controller_test。**Interfaces:** 采用3.3状态/ID及函数签名，现有静态API委托；busy仅用于展示。advance只允许合法转移，Unknown结果转Uncertain，不自动finish；busy包括Starting/Running/Continuing/Uncertain。

- [ ] 新增`NormalWaitRejectsConcurrentOperation`，假pkexec阻塞在spawn后栅栏，同时刷新/升级：`spawnCount == 1`；不是仅测超时后重复拒绝。
- [ ] spawn前tryBegin，占槽后才能清理同ID状态；认证取消/启动失败也经finish。超时转后台只改Running→Continuing，不重新占槽，不等待别的tracker再宣布自己active。
- [ ] Completed结果按ID一次消费，队列8项满时拒绝开始新操作并保留未读结果；页面打开/关闭不能释放仍在执行的操作槽。
- [ ] 覆盖`AuthenticationFailureReleasesSlot`、`CompletionConsumedOnce`、`OldCompletionCannotFinishNewOperation`、`UnconsumedResultsHaveBound`。
- [ ] G1更新/运行时/设置保存组，G3/G6；不运行真实包事务；提交状态所有权修复。

### T13：有界进程采集与包政策分离（P1-06第二部分）

**Files:** process_runner及settings_app内部模式分派；UpdateController；tests/settings/process_runner_test.cpp、fake子进程；相关CMake。**Interfaces:** 采用3.3节的CommandSpec/CommandResult/runCommand/runCommandCaptureChild；Spec包含argv、Environment、deadline、Query或PackageTransaction模式及3.3输出预算；Result区分started/exited/continuing/unknown/truncated和operationId，不能只剩bool。

- [ ] 将process_runner_test.cpp注册为modernime_process_runner。写大输出和截断测试，查询>1MiB不能报告成功解析；包每流100MiB假输出后保留≤64KiB尾部、正常结束；采集驻留/文件大小按预算断言，不用最终600字符消息当资源证据。
- [ ] 实现GTK前内部采集模式，命令与collector生命周期脱离GUI；采集器持续排空、控制/结果文件有上界且0600；直接argv执行，不创建额外安装依赖或把敏感环境打印到日志。
- [ ] 查询保留5s默认期限，可终止自身查询子进程；包保留300s等待后Continuing，collector继续等待，禁止超时强杀。子孙继承输出FD不让查询控制循环无限等待。
- [ ] `ParentExitWithInheritedOutput`在隔离假事务中退出GUI父进程，collector和假事务仍完成且无SIGPIPE；验证PID重用/无collector/异常collector返回Unknown，不误重试。
- [ ] 将APT/DNF解析/版本/渠道保留为纯函数政策，输出截断/解析失败/网络不可用与“无更新”分开。复用执行协议时不抹平runtime安全空输出启动与包事务差异。
- [ ] G1更新/进程/运行时、G3/G6、ASAN/FD检查；安装产物真实内部模式可运行（无GTK显示也可），提交资源边界。

### T14：配置应用版本与路径（P2-01）

**Files:** SettingsStore/Paths；adapter/UI；settings runtime_environment/model/shell；install/session-init/data-guard；配置/环境测试。**Interfaces:** 新`SettingsFileStamp`含device/inode/size/mtime/ctime；`SettingsApplicationVersion`含contentHash和会话generation，仅运行内存/诊断，用户settings格式不变。

- [ ] 写同mtime原子替换、删除/重建、外部保存冲突和敏感时热载；UI字号改变后旧布局拒绝，候选语义序列不变。
- [ ] 明确路径协议：运行程序仍以XDG_CONFIG_HOME/XDG_DATA_HOME→HOME默认解析；MODERNIME_*是安装/管理覆盖，启动边界必须转换成对应XDG，含空格原样传argv/env。不要新增C++对这些变量的隐藏优先级，避免读错旧库。
- [ ] 共享stamp/解析，adapter负责将业务配置应用并给页版本；UI独立读取作为兼容降级时携带同contentHash/布局代次，不能旧度量覆盖新边界。loaded/edited/保存/运行时仍分别存在。
- [ ] runtime.state只作可再生诊断，保留旧字段并兼容读取未知字段；写明版本与采样来源，不当作进程存活证据。格式扩展若旧读者不兼容则先只用已有hash/generation，禁止改用户数据格式。
- [ ] 新建`tests/configuration_contract_test.sh`注册modernime_configuration_contract，矩阵HOME/XDG/管理覆盖/非标准prefix/活动SQLite-LMDB；G1配置环境组、G3/G5/G6后提交。

### T15：设置数据异步与轻量统计（P2-02）

**Files:** DataController、Dictionary/Learning/Overview/Clipboard页面及Shell；Store/PersonalEngine计数；新`tests/settings/settings_data_async_gtk_test.cpp`。**Interfaces:** `optional<size_t> LearningStore::entryCount() const`，SQL COUNT(*)/LMDB stat，不构造Snapshot；`filterDictionary(span<const UserDictionaryEntry>, string_view) -> vector<size_t>`纯函数，放现有DataController。异步沿现有GTask＋失活令牌，无新增通用任务框架。

- [ ] 固定50k/200k、慢I/O两秒夹具；搜索频繁改变不读盘，旧结果不覆盖新结果，任务期间关窗口无UAF；备份失败不clear。
- [ ] Dictionary磁盘refresh与内存filter分开；存储加载/导入/备份/clear在worker执行，GTK只接收结果。统计调用entryCount；无需要时不在启动构造中同步加载全数据。
- [ ] 大列表GTK更新分批，每次idle最多200行或4ms，最终全部合法结果可访问，不能截掉结果假装加速；清空/导入禁重复操作，关闭窗口不取消已经进入Store事务的正确性流程。
- [ ] 新测试注册modernime_settings_data_async_gtk，RESOURCE_LOCK与现有GTK相同，SKIP_RETURN_CODE=77；headless登记跳过，desktop必须执行。事件循环heartbeat最大延迟拟定目标<100ms，非硬实时承诺。
- [ ] G1设置数据/模型组、G3/G6及ASAN；50k/200k响应证据、失败不损数据通过后提交。

### T16：构建目标与测试接线（P2-03剩余部分）

**Files:** 根/core/ui/settings/tests CMake及CI/README。**Interfaces:** 保留现有公开target名；共享`modernime_settings_components` INTERFACE source target，由应用及GTK集成测试消费同一源清单；各消费目标按各自版本/prefix/system-addon编译定义编译，不把settings_app main编入组件。这样先消除清单重复，避免OBJECT/STATIC共享不同宏导致测试误用真实安装路径。

- [ ] source清单统一一次，用target_sources(INTERFACE)提供；保留应用与shell/save测试各自的版本/prefix/system-addon宏及环境隔离，测试assert在Release仍启用。若未来统一为运行时注入，再独立评估OBJECT编译复用，本轮不强求。
- [ ] 将UI布局add_subdirectory挂到实际使用它的settings/Fcitx/tools/布局测试配置；增加MODERNIME_BUILD_UI_LAYOUT_TESTS选项，默认ON保留原构建；最小core验证显式OFF，不被Pango/GTK强制拖入；strict full必须ON。纯政策/存储target仅在依赖收益明确时细分，不重命名全部target。
- [ ] strict profile强制全模块/必需测试；普通core-only允许较少注册但必须显式标profile，不能冒充全量通过。
- [ ] 验证完整Debug、Release性能、core-only、缺可选依赖、headless、desktop；T00检查工具纳入CI，发行已有必需测试检查复用而非删掉。
- [ ] G1/G3/G5/G6，README依赖/预设同步；确认源文件无多处复制接线后提交。

### T17：显式安装阶段（P2-04）

**Files:** install/uninstall/generate_binary_installer/install_binary_payload/install_state/package_tarball、session-init；新install_lifecycle；安装事务/环境/升级数据保护测试。

**Interfaces:** 共同入口`modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR`、`modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR`；每个参数显式，返回非0保留恢复证据。source installer负责build/test/stage，binary负责payload校验，随后调用同生命周期；产物清单可用NUL文件列表，不用eval承载对象。

- [ ] 先冻结源码/二进制/原生包行为矩阵，`RollbackPreservesConcurrentUserWrites`验证失败阶段中新增学习与用户改配置不被恢复旧快照覆盖。
- [ ] 新共同阶段保持按文件备份、unlink后替换已映射so、失败受控恢复、manifest和托管快照；不复制整个prefix、不在安装回滚中恢复用户数据。
- [ ] binary生成改为明确薄入口＋版本/渠道数据，取消按install.sh行文本切片；新入口完成前保留旧锚点拒绝保护，不静默生成半包。
- [ ] install_state传显式MANAGED_STATE_DIR；同步CMake安装与源码/binary清单，将新生命周期脚本纳入卸载；native包生命周期通过现有data-guard/session-init，不能让script清理native归属文件。
- [ ] 按现有隔离测试运行prefix/空格/无显示/已有软链/用户改托管文件/各阶段失败/并发数据；G1安装组、G3、G5及既有tarball/release/package-repo隔离验证。不可在真实系统运行apt/dnf。
- [ ] 安装重复运行和卸载重复运行幂等；产物/环境/桌面/autostart/manifest闭环后提交。

### T18：统一页面解析与文档事实（P2-05）

**Files:** page_registry/app/window/shell及路由/GTK测试；HANDOVER、算法架构/变更/演化、候选、稳定性、README。**Interfaces:** `optional<SettingsPageId> parseSettingsPage(string_view)`在registry定义；`SettingsWindow::show(SettingsPageId, string_view target={})`作为通用入口，旧包装委托。

- [ ] 遍历全部8注册页测试CLI/别名/搜索/导航/包装一致，覆盖candidates与overview；未知页明确诊断，不默默跳错误页；已有实例接收目标页用隔离DBus集成验证。
- [ ] registry承担唯一名/别名解析，app/shell/window不再复制字符串switch；旧CLI别名保持。
- [ ] HANDOVER更新C++20/CMake3.24、现模块和测试动态列表；不写固定66为永久标准。演化追加勘误/已验证记录，保留256→512历史和修复前反例。
- [ ] 逐项更新M/C迁移状态，说明默认核心词已有正频次例外、可信恢复与导入差异、T11过渡行为及实际验证范围；不把trace/CI目录存在当行为已完成。
- [ ] G1路由/保存/配置、G6与文档链接检查后提交；所有T任务实施证据汇总到T19，不另建算法台账。

### T19：最终集成验收与交接

**Files:** 创建`docs/ARCHITECTURE_REMEDIATION_ACCEPTANCE.md`；审查/清单只追加状态链接，保留历史判断。**Interfaces:** 验收报告按8节矩阵标Pass/Fail/Skipped/NotRun，必须链接对应提交和证据路径。

- [ ] 检查T00–T18每项交付及15项清单覆盖，不能只交四个P0就称全仓整改完成；条件任务不达目标时写未完成及阻塞原因。
- [ ] 从最终HEAD及明确记录的工作树构建，执行G0–G7、包渠道矩阵；构建/测试前后复核代码、构建文件和词库输入摘要，期间发生外部修改则重跑受影响关卡并更新基线，不借旧证据；所有必要测试实际注册、执行并留输出。
- [ ] 人工检查层依赖/写入者/后置排序/锁序/资源边界，统计责任、状态权限和新增特性联动点变化；行数不作验收指标。
- [ ] 填写全部测试数、GUI跳过及真实桌面限制、性能P50/P95/P99/max、schema/格式兼容、安装卸载、回退演练和未覆盖项。
- [ ] 无失败/未注册必需项才标自动验收通过；desktop未完成只能“自动验收通过，桌面待验”，不能写全部完成。精确提交方案与验收文档，向用户交付分支/diff；不自行发布。

## 7. 验收命令与证据关卡

以下命令供**执行AI实施后**使用，本次编写没有运行这些构建/测试/安装命令。每个代码块均重新定义工作目录变量，适用于工具启动的新shell；命令从仓库根执行，Bash启用pipefail，证据存build忽略目录；每次使用最终当前源码构建，不能借旧build结果。严禁边编译边跑性能门禁。

### G0：身份、环境、注册存在性

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
mkdir -p "$proof_dir" "$proof_dir/gtest" "$proof_dir/tmp"
git rev-parse HEAD > "$proof_dir/source-sha.txt"
git status --short > "$proof_dir/working-tree.txt"
git diff --binary HEAD > "$proof_dir/working-tree.patch"
git ls-files --others --exclude-standard > "$proof_dir/untracked-paths.txt"
cmake --version > "$proof_dir/cmake-version.txt"
c++ --version > "$proof_dir/compiler-version.txt"

# T00首次基线配置时删除尚未实现的REQUIRE_FULL_TESTS这一行参数。
cmake -S . -B build/architecture-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="$repo_root/build/architecture-install-prefix" \
  -DMODERNIME_BUILD_FCITX5=ON \
  -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
  -DMODERNIME_BUILD_SETTINGS=ON \
  -DMODERNIME_BUILD_TESTS=ON \
  -DMODERNIME_REQUIRE_FULL_TESTS=ON
cmake --build build/architecture-debug --parallel 4 \
  2>&1 | tee "$proof_dir/debug-build.log"
ctest --test-dir build/architecture-debug --show-only=json-v1 \
  > "$proof_dir/debug-registered.json"
python3 tests/verify_required_tests.py \
  --registered "$proof_dir/debug-registered.json" --profile full
```

另保存LibIME版本、`PinyinDataPaths`实际选中的系统/扩展/热词数据路径与sha256，不能只记录发行版名称。只hash公开系统/仓库词库，不复制真实用户学习数据。

full profile至少要求：`modernime_personal_engine_gtest`、`modernime_personal_engine_benchmark`、`modernime_learning_store`、`modernime_pinyin_match`、`modernime_pinyin_provider`、`modernime_pinyin_quality`、`modernime_english_arbitration`、`modernime_pinyin_daily_simulation`、`modernime_pinyin_engine`、`modernime_engine_state`、`modernime_fcitx5_input_panel`、`modernime_fcitx5_candidate_list`、`modernime_candidate_bar_layout`、`modernime_candidate_bar_renderer`、`modernime_update_controller`、`modernime_runtime_controller`、`modernime_data_controller`、三个原有GTK测试、三个指定安装测试及现有安装/升级/数据保护入口。T19的final stage还要求新增modernime_required_tests_contract、modernime_cleanse_guard、modernime_configuration_contract、modernime_process_runner、modernime_settings_data_async_gtk入口。

### G1：任务定向回归

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
# 每次先构建当前源码。选择与任务对应的一组，不把局部通过称为全量。
cmake --build build/architecture-debug --parallel 4
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error -R '^modernime_personal_engine_gtest$'
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error -R '^modernime_(pinyin_match|pinyin_provider|pinyin_quality|english_arbitration|pinyin_daily_simulation|pinyin_engine|engine_state|fcitx5_input_panel|fcitx5_candidate_list|candidate_bar_layout|candidate_bar_renderer)$'
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error -R '^modernime_(update_controller|runtime_controller|data_controller|settings_model|settings_save_gtk|page_registry|process_runner|settings_data_async_gtk|configuration_contract)$'
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error -R '^modernime_(install_runtime|install_environment|settings_install|install_transaction|install_channel_collision|session_init|upgrade_data_protection|cleanse_guard)$'
```

新入口在所属任务前可以不运行该组中的它，但组内其他测试存在不能替代新增入口存在性；final stage逐名检查。GTest定向filter必须先list确认包含新测试；GTest零匹配返回0不能记通过。`Architecture*`套件加入现有GTest目标，T03涉及FcitxWord的源在Fcitx启用时条件编译并链接Fcitx::Core，storage-tsan配置不强制Fcitx。

### G2：存储/身份/候选行为验收

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
mkdir -p "$proof_dir/gtest"
GTEST_OUTPUT="xml:$proof_dir/gtest/" \
  build/architecture-debug/tests/modernime_personal_engine_gtest \
  2>&1 | tee "$proof_dir/gtest-full.log"
```

执行AI检查GTest XML：实际运行数>0，无失败，必需的本计划新套件无skip；记录内部testcase数，不只记录“一个CTest通过”。同时完成8节矩阵及固定时间/词库的基线与新实现回放。测试输入全部合成；未知模型按现有原生oracle/基线政策处理，禁止给指定测试词临时加分。

### G3：完整正确性与Release性能分别执行

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
# Debug全量正确性；性能在独立Release目录执行。
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error -E '^modernime_personal_engine_benchmark$' \
  --output-junit "$proof_dir/debug-junit.xml" \
  2>&1 | tee "$proof_dir/debug-ctest.log"

cmake -S . -B build/architecture-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$repo_root/build/architecture-install-prefix" \
  -DMODERNIME_BUILD_FCITX5=ON \
  -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
  -DMODERNIME_BUILD_SETTINGS=ON \
  -DMODERNIME_BUILD_TESTS=ON \
  -DMODERNIME_REQUIRE_FULL_TESTS=ON
cmake --build build/architecture-release --parallel 4 \
  2>&1 | tee "$proof_dir/release-build.log"
ctest --test-dir build/architecture-release --show-only=json-v1 \
  > "$proof_dir/release-registered.json"
python3 tests/verify_required_tests.py \
  --registered "$proof_dir/release-registered.json" --profile full

# 此命令前结束编译和其他重负载；私有TMPDIR避免同名基准文件冲突。
TMPDIR="$proof_dir/tmp" \
  ctest --test-dir build/architecture-release --output-on-failure \
  --no-tests=error -R '^modernime_personal_engine_benchmark$' \
  --output-junit "$proof_dir/performance-junit.xml" \
  2>&1 | tee "$proof_dir/release-performance.log"

# 最终还须在Release执行完整CTest，确认优化构建断言/行为均实际生效。
TMPDIR="$proof_dir/tmp" \
  ctest --test-dir build/architecture-release --output-on-failure \
  --no-tests=error --output-junit "$proof_dir/release-junit.xml" \
  2>&1 | tee "$proof_dir/release-ctest.log"
```

Release完整CTest中Benchmark保持RUN_SERIAL；不要并行运行另一套基准。GTest/Benchmark缺依赖、被排除、执行0用例或GUI跳过均单列，不能靠总数百分比掩盖。

### G4：并发与内存所有权

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
cmake -S . -B build/architecture-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=thread' \
  -DMODERNIME_BUILD_FCITX5=OFF \
  -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
  -DMODERNIME_BUILD_SETTINGS=OFF \
  -DMODERNIME_BUILD_TESTS=ON
cmake --build build/architecture-tsan \
  --target modernime_personal_engine_gtest --parallel 4
TSAN_OPTIONS='halt_on_error=1' \
  build/architecture-tsan/tests/modernime_personal_engine_gtest \
  --gtest_filter='ArchitectureWriterRegression.*:ArchitectureLmdbRegression.*:PersonalizationStorageRegression.*' \
  2>&1 | tee "$proof_dir/tsan.log"

cmake -S . -B build/architecture-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DMODERNIME_BUILD_FCITX5=ON \
  -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
  -DMODERNIME_BUILD_SETTINGS=ON \
  -DMODERNIME_BUILD_TESTS=ON
cmake --build build/architecture-asan --parallel 4
ASAN_OPTIONS='halt_on_error=1:detect_leaks=1' \
  UBSAN_OPTIONS='halt_on_error=1' \
  ctest --test-dir build/architecture-asan --output-on-failure \
  --no-tests=error \
  -R '^modernime_(personal_engine_gtest|fcitx5_input_panel|settings_shell_gtk|settings_save_gtk|settings_data_async_gtk|process_runner|update_controller|runtime_controller)$' \
  2>&1 | tee "$proof_dir/asan.log"
```

TSAN与ASAN分开。先核对filter确实包含目标套件；在TSAN中至少重放100个受控交错轮次并检查超时/失败退出。Sanitizer不支持当前宿主/第三方兼容失败时记录NotRun/Fail及日志，在支持环境补验；不能把退出66或忽略报告算通过。对第三方明确已知噪声的任何suppression都须最小范围记录理由，不屏蔽项目栈。

### G5：安装、升级、卸载及渠道

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
CMAKE_BUILD_PARALLEL_LEVEL=4 ./tests/verify_fresh_clone.sh \
  2>&1 | tee "$proof_dir/fresh-clone.log"

# G3完整CTest已执行这些入口；仅T17新增改动或失败需定向重跑。
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error \
  -R '^modernime_(install_runtime|install_environment|settings_install|session_init|install_transaction|install_channel_collision|verify_release_install_upgrade|upgrade_data_protection|verify_package_repo_gate)$' \
  2>&1 | tee "$proof_dir/install-targeted.log"
```

fresh-clone必须验证当前提交＋工作树补丁＋新增文件，不只旧HEAD。脚本自身隔离HOME/XDG/prefix；禁止替换为裸install/uninstall。包产物必须由当前源码重新生成，保存sha256/来源提交；不能以旧dist中存在包为证据。

Tarball额外命令（T17时需要，archive使用此次生成的精确路径）：

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
test "$(uname -m)" = x86_64
package_version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' CMakeLists.txt)
[[ "$package_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
CMAKE_BUILD_PARALLEL_LEVEL=4 ./scripts/package_tarball.sh \
  2>&1 | tee "$proof_dir/tarball-build.log"
tarball_path="$repo_root/dist/modernime-${package_version}-linux-x86_64.tar.gz"
test -s "$tarball_path"
sha256sum "$tarball_path" > "$proof_dir/tarball.sha256"
./tests/verify_tarball.sh "$tarball_path" \
  "$repo_root/build/architecture-debug/tests/modernime_addon_load_tests" \
  "$repo_root/build/architecture-debug/tests/modernime_pinyin_data_dirs_tests" \
  2>&1 | tee "$proof_dir/tarball-lifecycle.log"
```

该路径按当前package_tarball.sh的精确命名绑定本次重新生成的包；不得以此前同名包成功替代本次构建成功。非x86_64执行环境应在匹配平台补验，不能用错误标签包冒充通过。执行前确认两个probe程序实际存在。DEB/RPM的mock门禁与真实native包管理器验收分别记录；真实安装只在可丢弃容器/VM，签名发布使用现有CI的授权环境，本任务不在宿主安装或使用生产密钥。

### G6：GTK与真实桌面补验

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
# 在已获显示授权的测试桌面执行，不能通过unset DISPLAY伪造通过。
ctest --test-dir build/architecture-debug --output-on-failure \
  --no-tests=error \
  -R '^modernime_(settings_focus_gtk|settings_shell_gtk|settings_save_gtk|settings_data_async_gtk)$' \
  --output-junit "$proof_dir/desktop-junit.xml" \
  2>&1 | tee "$proof_dir/desktop-gtk.log"
```

无人值守环境可用隔离虚拟显示验证GTK生命周期，但Xvfb结果不冒充真实Wayland/X11应用事件。真实桌面至少在项目支持的X11/Wayland各一个测试会话检查：键鼠同词、长词/字号14/20/28与容量3/9、切应用/导航、密码/敏感、清空后继续输入、预编辑中热更新、后台更新时关窗口。只使用合成文本和测试账户，真实用户库不作素材。

### G7：最终证据检查（工具全部参数在T00实现）

`verify_required_tests.py`接受可重复`--junit`合并执行证据，以及`--gtest-xml DIR`、`--stage baseline|final`。baseline不要求本计划新增入口；final逐名要求。headless允许skip的**唯一白名单**：settings_focus_gtk、settings_shell_gtk、settings_save_gtk、settings_data_async_gtk；各项仍标Skipped。desktop模式这四项必须Pass；其他必需项不许skip。注册JSON与最终源码配置对应，JUnit来自相同构建或明确记录的Debug/Release配对，不能混旧版本。

```bash
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
proof_dir="$repo_root/build/architecture-evidence"
python3 tests/verify_required_tests.py \
  --registered "$proof_dir/debug-registered.json" --profile full --stage final \
  --junit "$proof_dir/debug-junit.xml" \
  --junit "$proof_dir/performance-junit.xml" \
  --gtest-xml "$proof_dir/gtest" --execution headless

# 桌面补验完成后运行；如果尚未完成，保留“桌面待验”而非删除该关卡。
python3 tests/verify_required_tests.py \
  --registered "$proof_dir/debug-registered.json" --profile full --stage final \
  --junit "$proof_dir/debug-junit.xml" \
  --junit "$proof_dir/performance-junit.xml" \
  --junit "$proof_dir/desktop-junit.xml" \
  --gtest-xml "$proof_dir/gtest" --execution desktop

git diff --check
```

## 8. 验收矩阵与明确通过标准

### 8.1 正确性、并发及数据

| 验收ID | 输入/交错 | 通过标准 | 所属任务/证据 |
|---|---|---|---|
| A01 | enqueueCommit与后台发布 | 无未同步成员访问；旧快照不变；TSAN无项目竞争 | T01，栅栏/TSAN |
| A02 | clear前旧事件、clear后首个新事件 | 旧事件不入新代，新事件恰在新代一次；revision单调 | T01/T05/T11，SQL/LMDB独立读 |
| A03 | 同路径Open、批次退出、升级/扩容/Close | 单一环境；受控交错正常退出；无死锁/悬空/重复关闭 | T02/T11，子进程/ASAN |
| A04 | 旧FcitxWord/List、同长度新页/已销毁会话 | 拒绝且零上屏/零学习/零消费，无UAF | T03，fakehost/Fcitx/ASAN |
| A05 | 旧字体/旧布局回传、预编辑热载 | 陈旧边界不覆盖；新页一次一致发布；新设置命令资格立即有效 | T03/T14，input panel/layout |
| A06 | 点击/数字/空格/取字/部分消费 | 同一身份与剩余输入；nihaoma选你好仅消费nihao | T03/T08，键鼠回放 |
| A07 | 同文本异读音/角色/消费与等价多来源 | 非等价项可区分并正确选；等价合并一次、不继承父学习 | T08/T09，受控身份夹具 |
| A08 | 事故脚本所有调用 | 非0、目录内容hash不变；无默认维护旁路 | T04，guard测试 |
| A09 | 三路提交/反馈/屏蔽/恢复/clear | 共同语义字段一致，允许差异逐项断言；失败无半批 | T05，三路事件回放 |
| A10 | 自有高频Save→Load、外部Import | 自有共同字段完整往返；外部每条导入记录3/20限频仍有效 | T06，14B/扩展夹具 |
| A11 | epsilon三元组/全部排列/非有限值 | 比较器所有严格弱序性质成立；稳定身份tie-break | T07，属性与排列测试 |
| A12 | 敏感/禁学两轮＋正常一轮，普通三轮 | 前者不凑造词次数，后者保留造词；不读敏感上下文 | T05/T10，Provider/Controller |
| A13 | 默认核心词有/无历史、Raw jianpan、英文gara | 保留差量例外；Raw不污染；完整英文键与实际消费分离 | T05/T09，仲裁/Store |
| A14 | 新代视图加载期间/后端故障/恢复 | 旧页无授权，未知屏蔽不被当允许；队列≤100；恢复后Ready | T11，加载屏障/故障回放 |
| A15 | 两个正常等待期包调用、认证失败/旧完成 | 至多一个spawn；失败释放；旧结果不能结束新任务 | T12，假pkexec栅栏 |
| A16 | 大输出、继承FD、GUI父退出 | 预算内采集与结果；假事务完成无SIGPIPE；查询期限有效 | T13，资源测量/进程测试 |
| A17 | slow I/O、连续搜索、关闭窗口、备份失败 | GTK响应；旧结果无覆盖/无UAF；失败不清库 | T15，GTK/ASAN |
| A18 | 八页面多入口/已有实例目标页 | CLI/搜索/导航/包装同页，兼容别名保留 | T18，registry/隔离DBus |
| A19 | 新clone、重复安装/卸载、中途失败 | 产物/环境/桌面/autostart/manifest完整对称，用户修改/新数据保留 | T17，fresh-clone/渠道矩阵 |
| A20 | 缺依赖/缺测试/测试0用例/skip | strict失败或明确未完成；不产生全绿误报 | T00/T16/T19，负向准入 |

### 8.2 候选学习曲线

固定时钟、词库/LibIME版本、初始数据、事件序列与上下文。每个目标记录选择次数`0/1/3/5/10/20/50`，包括接受/跳过原因、通用/上下文词频、resetRevision/快照版本、自身增益、组内及组合后全局位置、物理页/页内编号、消费与余串。不得只输出boost上升。

- `ni/泥`一次不夺`你`首位；`de/shi/qu`保留高频中文优先与英文竞争保护。
- `shi/是、试`、`jian/见、键`新库和竞争字已有200次学习分开测；连续正常选择10次切换目标首位的会话契约按现有回归保留；清会话/重启另测持久策略，不要求把RecentChoices写盘。
- `nihaoshijie/你`五次内全局≤2且仅消费ni；`nihaoma/你好`显示/评分来自自身证据，余ma保留；适用的前五两完整候选及可信前缀名额保留。
- 完整词初始前20、评分池尾部前128等已有目标按候选架构固定夹具验证；未实现或模型差异必须记录，禁止改目标迁就新公式。
- 1,000轮日常高频仿真保留原场景和零篡位门槛，外部导入防污染、技术英文、负反馈、持久屏蔽、重启与清空全链路联测。
- 字号14/20/28、容量3/9、注释开/关及窄宽面板均不得改语义序列；实际第一页项数由几何决定。X11/Wayland真实事件仍需G6，模拟布局不能冒充桌面通过。

### 8.3 性能、工作量与资源预算

| 指标 | 数据/条件 | 门限与证据性质 |
|---|---|---|
| 现有召回/检索 | 50k/200k的原有规定场景 | P99 **<1ms**，保留现有门禁 |
| 快照更新/反馈/屏蔽/满容量新词＋下一召回 | 原有50k/200k分布 | P99 **<5ms**，保留现有门禁 |
| 后台重放/竞争输入 | 原有SQLite并发50k | P99 **<5ms**且max **<30ms**，保留原门禁 |
| 新增LMDB Writer竞争 | 50k/200k，含容量/扩容/clear后首键 | 本计划拟定同样P99<5ms/max<30ms；先留基线再实施，不能以旧代码不达标降低门限 |
| 新代后台准备 | 200k，本地可读库且无注入慢I/O | 拟定就绪≤2s，期间保留原始组合；单独测过渡，不混进热路径计时冒充已Ready |
| 设置主循环 | 50k/200k，worker注入2s慢I/O | 拟定heartbeat max<100ms；idle批量≤200行或4ms；全部结果最终可访问 |
| 更新捕获 | 查询/长包事务大输出 | 3.3预算在采集处成立；不以最终显示长度代替内存/文件测量 |
| 成功完成后的生命周期 | 重复创建/关闭/运行100轮 | 无项目泄漏、FD/collector/线程回到基线；缓存与未消费结果符合上限 |

新门限是计划的验收目标，未在编写阶段实测。原有样本/断言保持；每个新增热路径场景预热200次后至少1000次有效样本，固定计时区间，独立执行3轮并保留全部结果，三轮均应过门；200k全量后台准备单独做至少10次独立加载，不将一次加载混成1000个假样本；不挑最快一轮。另记录CPU/构建/数据库分布/锁竞争、P50/P95/P99/max/RSS、样本与故障状态。Sanitizer/Debug结果不用于Release延迟判定；不能删除负反馈、满容量或首次未命中场景“优化”门禁。

受宿主调度显著干扰时，保留失败日志，改善隔离后完整复测；不能先放宽门限或悄悄删除max。新门限若确实需要不同工作负载，应单独提交测量依据与设计变更，任务仍标未通过，禁止伪称原门禁通过。

### 8.4 架构完成标准

- core不新增algorithm/adapter/GTK/Fcitx反向依赖；事件头不依赖Store，Store不解释页面业务规则。
- 正常学习一条写通道、一个接受政策；后端共同语义有回放，独立物理策略仍明确。
- 最终组合一个入口；所有生产候选显式身份/角色；所有键鼠选择和分页回传有当前版本授权。
- Registry锁不与环境/事务等待嵌套；内存暂存无环境析构；map扩容/Close协议有交错测试。
- Provider不再承担共享资源加载细节和末端散落排序；Controller宿主事实/退格状态机完整，UI不排序。
- 包操作从spawn前原子持有，进程预算采集处生效；新增APT/DNF政策无需修改FD生命周期。
- 设置Page不作同步整库工作；新页面只登记一次解析；应用/测试source接线共享。
- binary installer不按源码文本切片，新增安装文件能对称卸载；数据恢复/外部导入/程序回滚语义独立。
- 每个新增文件指出减少的责任/写权限/依赖/联动点；若只是把旧代码搬到Helper，不满足完成条件。

## 9. 停止、回退与异常处理

1. **基线不符：** 先重定位并更新证据映射，保留用户新提交；不能checkout旧版本覆盖用户工作。新代码已修某项时补证明并标“已有实现复核”，不重复造模块。
2. **行为差异超出允许范围：** 停止该提取，固定最小输入/事件/模型/时钟差异；依赖任务不继续。无需用户确认的常规实现选择由执行AI解决，新的排序政策、schema或数据迁移不得暗中加入。
3. **测试/性能失败：** 保留失败输出；不能删除旧断言、减少数据、跳过基准或用源码grep代替行为。未注册、0用例、skip、环境无法运行分别记录，不写Pass。
4. **并发死锁测试：** 仅可终止合成测试子进程防止CI永久等待；真实包管理事务不得使用此策略。新epoch故障恢复必须在存储恢复后通过下一输入/flush的有界重试回到Ready；永久存储故障的受控降级不冒称恢复成功。
5. **代码回退：** 回退当前任务独立提交/精确文件修改，不使用hard reset清用户工作；保留已通过且无依赖的前置安全修复，不机械回退整批。
6. **数据回退：** 本方案不改持久格式，因此代码回退继续读合法新学习。禁止恢复旧库抹去任务期间新输入；备份用于一致验证和用户明确选择的灾难恢复，不是自动撤销按钮。
7. **安装失败：** 仅恢复此次程序payload，保留新学习与用户改配置；恢复失败保留受控目录/清单并返回非0，禁止仍打印“回滚完成”。
8. **范围扩大：** 若必须引入新依赖、后台守护服务、格式升级或不同降级行为，先完成现有可独立交付项，提供具体差异/成本/测试方案。把该扩展标未实施，不能以“架构更漂亮”为由自行扩大。

## 10. 最终验收报告模板

执行AI创建`docs/ARCHITECTURE_REMEDIATION_ACCEPTANCE.md`，至少包含：

```markdown
# ModernIME 架构整改验收
- 基线SHA / 最终SHA / 工作树状态：
- 实际构建配置、系统/编译器/LibIME及词库hash：
- T00–T19和15项整改：完成 / 失败 / 未完成；对应提交与原因：
- 有意修正行为及固定输入前后差异：
- 未改变的容量、衰减、格式、隐私、消费、安装边界：

| 验收ID | 实际测试/命令 | 注册数/执行数 | Pass/Fail/Skipped/NotRun | 证据与限制 |
|---|---|---:|---|---|

- GTest内部testcase：实际执行/失败/跳过；新增必需套件存在性：
- CTest：Debug、Release、fresh-clone各自通过/失败/跳过：
- 性能：各场景三轮P50/P95/P99/max、RSS、计时区间与门限：
- TSAN/ASAN/UBSAN：实际执行、发现、修正、环境限制：
- 候选曲线：全局位置、物理页、键鼠、消费、频次、世代：
- 数据：SQLite/LMDB/14B/扩展往返、清空、失败恢复和未触碰真实用户库：
- 安装：源码/binary/DEB/RPM的真实或模拟范围、幂等与用户修改保护：
- 桌面：GTK、X11/Wayland实际验收与未覆盖应用：
- 架构：减少的责任、写入者、依赖、联动修改点；保留的合理复杂度：
- 回退演练、尚存风险、下一步：
- 结论：自动验收通过 / 自动验收失败 / 桌面待验 / 全部约定验收完成。
```

禁止只有“所有测试通过”“文件已拆小”“代码更优雅”三句话。证据目录若不会随分支交付，需把关键结果摘录及可复现命令写进报告；不提交包含真实输入/环境秘密的大日志。

## 11. 可直接复制给执行AI的任务指令

```text
请在ModernIME仓库执行 docs/superpowers/plans/2026-10-16-architecture-remediation.md。
先完整阅读AGENTS.md、计划引用的算法/候选/稳定性/交接文档，以及
ARCHITECTURE_AUDIT.md、REFACTOR_BACKLOG.md。核对当前HEAD和工作树，
保留用户已有修改；不要按旧行号盲改，不要重新做一轮无边界全仓审查。

按T00→P0四项→依赖图中的P1/P2→T19逐任务推进。
每项先固定失败/兼容证据，再作最小实现、定向回归和规定的完整关卡，
独立复核与精确提交。每个交付必须减少实际责任、写权限或联动点，
不能只搬代码、增加Manager/Helper或以文件长度为目标。

正常实现与修复按本计划完成，不在每项后只提出“下一步建议”就停止。
不得修改成熟排序政策、容量、衰减用途、schema或用户数据格式，
不得删除旧测试/批量改金样。计划允许的缺陷修正须单列前后差异。
所有用户数据/包操作使用隔离夹具；禁止动真实个人库、裸安装卸载、
真实apt/dnf升级、自动发布/合并/推送。

执行G0–G7与A01–A20，并填写完整学习曲线、性能、并发、GUI和安装证据。
缺必需测试、0用例、跳过、旧二进制或环境不可运行均不能记为通过。
有阻塞或方案与现代码不符，完成不受影响任务并明确记录；
不要跳过硬边界或扩大为数据迁移来制造“全部完成”。

最终提交代码及 docs/ARCHITECTURE_REMEDIATION_ACCEPTANCE.md，
报告T00–T19状态、15项整改覆盖、所有关卡结果、行为差异、未覆盖项，
以及具体减少了哪些职责、依赖、状态和修改影响范围。
```
