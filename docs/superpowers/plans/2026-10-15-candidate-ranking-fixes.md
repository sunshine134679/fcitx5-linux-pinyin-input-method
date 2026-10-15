# Candidate Ranking Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复实测的近期偏好切换、部分候选晋升、派生前缀学习丢失及静态先验不可达问题。

**Architecture:** 在既有 Pipeline、Ranker、Mixer 边界内落实已确认设计：有界单调基础代价、独立的有界会话选择意图、前缀自身学习合并与 Composer 个人前缀席位。Provider 只协调生命周期，不改变 Writer/Store 或 UI 分页协议。

**Tech Stack:** C++20、LibIME、现有 SQLite/LMDB、GTest、CTest、Benchmark。无新增依赖。

**Spec:** `docs/CANDIDATE_PIPELINE_ARCHITECTURE.md`、`docs/ALGORITHM_USAGE_SIMULATION_2026-10-15.md`。

## Global Constraints

- 保留 200,000 容量、独立 PersonalEngine 160,000 剪枝、7 天/30 天/12 小时时间策略。
- 保留短码中文、防单次夺位、敏感与禁学、屏蔽、分段消费和冷启动前五完整候选配额。
- 不读取个人数据目录；测试使用隔离数据库；保留其他未提交改动。
- 召回 P99 <1 ms，更新加召回 P99 <5 ms；完成全量 CTest、GTest、Benchmark、fresh clone 安装闭环。

## 变更影响与状态权限

- 涉及稳定契约：`ALG-RANK/PINYIN/ZHEN/DECAY/GATE/SUPPRESS/IDENTITY/FORM/CAPACITY/SNAPSHOT/STORE`，`CAND-RECALL/ADAPT/IDENTITY/COMPOSE/ELIGIBILITY/PAGES/SELECT`。新增并登记 `CAND-RECENT`；后续停用保留此 ID 和历史记录，不清除持久词频。
- 状态权限：Pipeline/Features/PrefixEvidence 只读本次输入与允许的不可变快照；Ranker/Mixer 只变更本次候选顺序。RecentChoices 仅由当前 Provider 的已允许明确选择及会话清理修改；不写 SharedResources、周边文本或数据库。Writer/Store 继续拥有全部持久事件与清空代次。禁学只读屏蔽使用 SharedResources 预载只读 Store，不写选择也不创建词库。
- 依赖：近期意图依赖 GATE/IDENTITY/DECAY/SUPPRESS；个人前缀席位依赖自身学习、身份和消费范围；512 分片依赖全局召回预算、同词上下文归片、容量淘汰和不可变版本。
- 预期差异：允许新库尾部候选、强竞争目标及已学部分前缀晋升；派生项显示自身证据；负值保留；禁学不绕过屏蔽。无意图变化的短码中文、中英、可信简拼、三次纠正、存储/清空、快捷键、分页映射继续保留旧断言。
- 回退：这些模块在原构建目标中，无 schema 或新依赖；回退排序/近期政策不删除用户数据。分片是内存实现细节，持久记录格式保持。
- 验证证据：现有 GTest46+新增18=64，最终 CTest63通过/3GUI跳过；Benchmark未降低门限；真实控制器反复选择、物理页面、数字键/命中选词、剩余输入与持久次数均验证；fresh-clone完整闭环完成后登记。

## Review Focus

1. 反向历史强竞争：是/试、见/键均需在十次普通连续选择内切换；单次不切换。
2. 失焦/重置、敏感、禁学及共享 Provider 不得串用近期意图。
3. 父整句学习不能冒充前缀学习；前缀去重仍保持自身消费范围。
4. 原生索引不得受学习召回遍历顺序改写；可信简拼原保护保持。
5. 部分词提权仍保留首整句、完整候选配额；屏蔽先于席位选择。

### Task 1: 前缀自身证据与个人前缀席位

**Files:** `candidate_mixer.cpp`、`pinyin_candidate_provider.cpp`；新增 `tests/core/candidate_ranking_regression_test.cpp` 并注册到既有 GTest 目标。

**Interfaces:** Mixer 保持现有 `mixCandidateItems` 调用协议；Provider 在混排前补全派生项自身 `learningBoost`；Mixer 按文本、读音、消费范围合并同语义项。

- [x] 写测试：父句有学习但前缀无学习时不能继承；前缀有学习时最终候选保留自身权重；连续选五次“你”后 `nihaoshijie` 全局名次 ≤2，首项仍完整消费，选择后只剩 `haoshijie`。
- [x] 运行新增测试并记录预期失败。
- [x] 修复派生证据合并并将最强已学习部分候选放入第二席，保留冷启动组合规则及两个完整候选的前五配额。
- [x] 验证新增测试和既有 Provider/日常/仲裁测试。

### Task 2: 有界基础先验与确定性索引

**Files:** `core/include/modernime/core/candidate_ranker.h`、`core/src/candidate_ranker.cpp`、`algorithm/pinyin/src/candidate_pipeline.cpp`。

**Interfaces:** CandidateScore 保留原生 source_index；非个人基础代价单调映射到有界区间；可信简拼晋升资格保留现有独立政策。

- [x] 写测试：学习饱和可突破未学习静态先验；原生索引不被学习召回改写；枚举顺序不影响语义平分结果；深尾普通词十次进入前五。
- [x] 运行新增测试并记录预期失败。
- [x] 修复基础代价与召回位置耦合，不调低容量、不改时间衰减、不增加词级伪频率。
- [x] 验证 Ranker、Provider、质量、日常和简拼旧断言。

### Task 3: 有界近期选择意图

**Files:** 新增 `algorithm/pinyin/{include/modernime/pinyin,src}/recent_choice.{h,cpp}`；连接 Pipeline、Provider 和 CMake。

**Interfaces:** RecentChoices 只持有当前 Provider 会话的最多 64 个输入/覆盖组，每组最近八次已确认中文选择；至少三次且当前最后选择占严格多数或连续三次才影响排序；普通清组合保留，明确会话边界清除。

- [x] 写测试：四组强竞争十次夺首、一次不夺首、共享与重置不泄露、禁学/敏感不累计、容量淘汰。
- [x] 运行新增测试并记录预期失败。
- [x] 实现独立近期特征，资格和匹配级别优先；原学习与存储保持。
- [x] 验证专项、完整模拟和重新打开数据库的持久行为。

### Task 4: 交付验证与文档

- [x] 重新运行人工逐键模拟并与修复前报告逐项对比。
- [x] 完整构建、全量 CTest（核对 GTest/Benchmark 实际执行及 GUI 跳过）。
- [x] 执行 `./tests/verify_fresh_clone.sh` 完成隔离安装卸载闭环。
- [x] 独立审查本次改动；修复实质问题并回归。
- [x] 追加 `ALGORITHM_EVOLUTION.md`，同步 README、架构和模拟报告，明确已实现范围及未覆盖桌面条件。

## Execution ledger

- 已有设计与用户“修复”指令构成本次执行授权，直接在当前 `codex/fix-personalization-regressions` 工作分支推进；不再请求重复设计确认。
- 当前工作区含上一轮授权修复及其他任务改动；本次保留未提交状态，不提交/推送混合工作区。
- Task 1/2/3 共享 CandidateScore 与 CandidateItem 特征，先通过存储快照补齐前缀，再在组内评分加入近期意图；不把排序政策塞进 UI。

- Task 3: Ruling: 严格多数单独作为资格会破坏已有三次连续纠正断言；改为当前最后选择拥有严格多数或连续三次，旧选择立即失去近期加分。代价：短会话中连续三次选择足以成为当前意图；仍保留学习阈值和单次防夺位。
- Task 2: Ruling: 将三处重复的词典/字符/学习特征统一抽取到 `candidate_features`，Pipeline 只采集与合并召回证据；同一刷新共用一个快照和时间。原因：本轮重点是结构优化，避免未来新增特征只覆盖某一来源。风险由所有来源、简拼与旧纠正测试回归控制。
- Task 1/2 RED: `/tmp/modernime-ranking-red.log`、`/tmp/modernime-ranking-prior-red.log`；前缀四项与先验八项后续通过。Task 3 四组历史强竞争及两个近期意图单测通过；额外连续纠正断言曾失败，修复后 2/2 近期模块通过。

- Final review: 两项 Important，无 Critical/Minor。负权重被默认零截断、禁学绕过持久屏蔽均以真实 Provider 用例确认失败（`/tmp/modernime-negative-red.log`、`/tmp/modernime-suppression-red.log`）。统一 Features 直接读取带符号快照；可见性快照读取与学习资格分离，禁学不启用个人评分或提交写入。
- Final: Ruling: 网站改动及私人数据不属于算法审查范围；真实桌面事件顺序留待可用桌面验证；构建、性能、安装和文档由主执行者完成。风险：本轮模拟不能证明真实字体/DPI/Wayland 行为。

- Final: fixed 负反馈符号截断 — `NativeCandidatePreservesNegativeLearningEvidence` RED→GREEN，并断言同先验负反馈候选落后中性候选。
- Final: fixed 禁学绕过屏蔽 — `DisablingLearningDoesNotRestorePersistentlySuppressedCandidates` RED→GREEN；随后旧 Provider 断言发现直接初始化 Writer 会建库，改为 SharedResources 启动时读取已有只读 Store，热路径只读内存。旧 `modernime_pinyin_provider` 失败后通过，64/64 GTest 通过。最终完整门禁仍需重跑。

- Task 1–3 complete: 新增 18 项候选回归与旧 46 项共 64/64；最终 CTest 63 通过、3 GUI 跳过、0 失败（183.44s）。`modernime_pinyin_provider` 9.68s 通过，原有禁学不建库、可信简拼、三次纠正不弱化。
- Task 4 evidence: `build/personalization-review/candidate-ranking-fixes-2026-10-15/{ctest.log,ctest-detail.log,gtest.log,usage.log}`。复测 300 次提交、36 组布局、24868 次命中、216 次选择通过；会话/重开差异如实记录。Benchmark 50k/200k 召回 P99 47.81/86.92us，更新+召回 509.13/1762.32us，200k 新词 4376.12us；写锁 128 事件恰好一次。fresh-clone 运行中。

- Task 4: fresh-clone 初次 RED：200k 满容量新增 P99=5361.64us 超过 5000us（原日志另存 `fresh-clone-first-failure.log`），其余 63 测试含 GUI 跳过无失败。Benchmark 已是 RUN_SERIAL，不能归咎并行测试竞争或放宽门限。
- Task 4: Ruling: 256 分片在 200k 下单片约 800 项，新增/淘汰需重建两片索引，延迟余量不足。将内存分片粒度细化至 512，不降容量、不改衰减或存储 schema；召回仍全局合并，旧快照与淘汰优先级回归保留。代价：快照指针表和各片固定开销增加，召回需要遍历更多片，须重新通过召回门禁。

- 512 分片完整回归完成：63 通过、3 GUI 跳过、0 失败，213.03s；内含 GTest64/64。最终 Benchmark 50k/200k 召回 P99=236.69/246.75us，更新+召回=506.49/1450.01us，200k新增=2722.22us。原有门限均保留。
- 上次文档收尾命令因自动审批审查额度不足未执行；用户要求继续后已恢复调用，并补写实际状态。没有绕过审批。最后安装闭环仍需按512结构验证。

- 最终512结构手工回放通过：300次训练全部走物理页数字键/点击（各150），270组学习后页位置检查十次后均第一页；另36组全页布局24868命中、216真实选择通过。保持300次准确词频和部分消费，未把物理页通过与真实字体验收混淆。

- Task 4 complete: 最终512分片 fresh-clone 退出0；全新构建、63测试通过/3GUI跳过、GTest/Benchmark实际执行、安装发现与配置、卸载清理全部通过。证据 `fresh-clone.log`；初次性能失败另存，不以重跑掩盖结构修正。
- Final review completion: 两项 Important 均 RED→GREEN 并经最终完整套件与安装闭环验证；无未处理 Critical/Important 或 deferred minor。真实桌面/字体与统一仲裁/追踪未实施边界仍明确记录。
- 交付：保留 `codex/fix-personalization-regressions` 当前混合未提交工作区；本轮无提交、推送、PR 或宿主安装/重启。不清理他人分支、工作区或私人数据。
