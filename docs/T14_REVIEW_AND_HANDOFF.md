# T14 复审与定向整改交接

日期：2026-10-17  
审查提交：`7f2f05db34073b526a623d31039b4af06c9198e8`  
前序测试维护：`12e125d7c16b08b6f3db0db257f44779dee0cb6d`  
T13 已验收基线：`8d10597c8d74de4f428143cf2366c4ecd262ded0`

**结论：T14 暂不通过。已独立复现三项 P1 行为/契约问题，并确认配置验收矩阵存在实质覆盖缺口。保留已有效的物理 stamp 和布局字号检查，按 R1–R4 定向整改，不重做 T13，也不进入 T15。**

本轮未修改生产代码、现有测试或执行方验收报告，只新增本文与忽略目录中的复审材料。未提交、推送、发布，历史未跟踪文档保留。

## 1. 已确认的有效部分与验证范围

- T13 遗留 P2 已独立提交：假产物权限、有效假产物对照、错误类别断言、binary 参数及 CTest 接线均有实际改动。`modernime_install_gate_matrix` 使用当前构建的 settings binary，避免原来的 Debug 固定路径依赖。
- `SettingsFileStamp` 能分辨同 mtime 的 inode 替换、缺失与重建，现有核心单测确实调用了该实现。
- `SettingsApplicationVersion` 类型存在，adapter/UI 已从单 mtime 比较迁移到 stamp。问题在读取与应用边界，不能以字段已存在推定整个版本契约闭合。
- 纯布局层新增字号不匹配拒绝命中的断言有效；真实 UI 消费版本与敏感热载的完整证明仍不足，见 R4。
- 独立设置/配置/模型/runtime 回归 4/4 Passed，2.29s；重建后的真实 Fcitx input-panel 与布局回归 2/2 Passed，2.09s。既有测试通过没有覆盖下述失败交错。
- 执行方全量 XML 确为新归档：Debug 71 项、0 failures/0 skipped、17:13:45、time=563s；Release 71 项、0 failures/0 skipped、17:23:20、time=320s。复审读取了这些归档，没有重复运行全部套件或完整 fresh clone。

独立证据目录：`build/t14-review-evidence/`，主要包括 `g1.xml`、`adapter-ui.xml`、`config_probe.cpp`、`config_hooks.c`、`config-probe-results.json`、`adapter_probe.cpp`、`adapter-probe-results.json`、`path_probe.py`、`path-probe-results.json`。

## 2. R1 / P1：解析内容与 hash 不是同一快照，版本去重会让旧配置持续生效

**位置：** `core/src/settings.cpp:209–212,306`；`adapter/fcitx5/src/inputmethod.cpp:775–787`；`ui/fcitx5/src/ui_addon.cpp:99–106`。

`SettingsStore::load()` 先 stat 路径、第一次打开文件逐行解析，最后调用 `computeFileContentHash(path)` 第二次打开路径计算 hash。外部原子替换可以发生在两次打开之间。因此返回值可能是：A 文件的配置和 stamp，B 文件的 hash。

T14 新增“hash 等于已应用 hash 就跳过”的分支，使问题从一次不一致扩大成持续错误：第一轮将 A 配置标成 B hash；下一轮读到 B 的真实配置时，hash 又等于已应用值，于是跳过应用。

### 独立确定性复现

故障钩子只在本探针私有 settings 路径的第二次 fopen 前，将预先准备的 B 原子替换为当前文件，不动宿主文件或时钟。

真实 SettingsStore 输出：

```text
loaded_enabled=0 loaded_font=14 hash=163fea997ea4b910
disk_enabled=1   disk_font=28 hash=163fea997ea4b910
same_hash=1 same_settings=0
```

进一步使用**当前真实 ModernIMEInputMethod、真实 Fcitx EventLoop 和公开输入上下文行为**，初始为禁用，加载期间替换成启用配置；两轮轮询窗口后发送真实 n 键：

```text
before_active=0 generation=0
after_active=0 generation=1 accepted=0 preedit=
runtime: applied_content_hash=f009f7c137e228c6, settings_generation=1
disk_enabled=1 disk_font=28 disk_hash=f009f7c137e228c6
```

诊断 hash 与磁盘一致，但真正生效的输入设置仍旧。此证据不是重写 Python 配置应用算法所得，而是链接当前生产 adapter 对象执行的行为。

### 根因与目标

配置内容、rawLines、hash、stamp 应属于同一次成功读取的配置事实。不能分别从可变化的路径采样后拼接成一个已应用版本。

- 最小修复在既有 SettingsStore 中形成一致读取结果：解析和现有内容 hash 使用同一份实际读取字节，stamp 对应该读取对象；若需要校验变化，采用有界重试或明确“待重试”结果，不能在输入线程无限循环。
- 保留现有 hash 算法与保存冲突契约，不用另一种语义 hash 静默替换文件 hash，不另建配置 Manager/总线。
- adapter/UI 使用成功读取结果的版本事实，不把轮询开始时的路径 stat 无条件当作已完成应用的 stamp。

**前置测试与完成标准：** 先让当前实现稳定失败；覆盖解析前替换、解析与 hash 之间替换、连续两次替换、相同内容替换。任何成功结果都必须配置/hash/stamp 自洽；最终最新合法配置会被应用，旧内容不能因错误 hash 被永久去重；外部保存冲突不被绕过。

## 3. R2 / P1：读取失败被应用为默认启用配置，且失败 stamp 已被消费

**位置：** `SettingsStore::load()` 的失败返回，以及 adapter `inputmethod.cpp:778–786`、UI `ui_addon.cpp:101–105`。

读取失败时，SettingsLoadResult 返回默认配置、空 hash 和读取诊断。adapter 没有检查读取是否成功：空 hash 与旧 hash 不同，就应用默认值、推进 generation、写成功样式的 runtime.state。UI 有相同消费顺序。

两边又都在读取前更新已观察 stamp；故障清除但物理 stamp 未变化时，后续不会重试。

### 真实 adapter 复现

在私有文件的一次 fopen 注入 EACCES，磁盘配置仍是 `input.enabled=false`。真实 Fcitx 事件循环轮询后发送 n 键：

```text
before_active=0 generation=0
after_active=1 generation=1 accepted=1 preedit=n
runtime: applied_content_hash=, settings_generation=1
disk_enabled=0 disk_font=14
```

即读取失败把明确禁用的输入打开了。默认配置还包含学习等默认选项；本次没有把该现象夸大成已验证的敏感上下文越权或用户数据写入。

### 最小目标与完成标准

- 明确区分成功读取、缺失、I/O 失败和允许保留的解析诊断；不能把“有任何 diagnostics”统一当作失败，否则会破坏未知键/注释兼容性，也不能以空 hash 冒充成功。
- 热载 I/O 失败保留最后有效配置，不推进成功应用版本/诊断；在同 stamp 下具备有界的后续重试机会。UI 不应在暂时读失败时回退默认度量并误标成功。
- 保留明确的首次缺文件默认行为与既有容错政策；删除/重建另测，不用这个修复静默重定义全部配置错误策略。
- 测一次失败后恢复但不再修改文件：最终正确应用；永久失败不忙循环、不推进版本、不启用禁用选项；敏感组合/光标和学习/剪贴板权限保持。
- R1 与 R2 可共享一个清晰的读取结果契约；不要用不断追加 if/flag 修复各调用点，也不要创建第二套配置状态负责人。

## 4. R3 / P1：启动边界未统一，运行目录与管理保护目录不同

**位置：** `scripts/modernime-session-init:16,92,164–165`；`install.sh` 的环境/desktop/autostart 生成；`tools/settings/include/modernime/settings/runtime_environment.h` 与 settings 启动入口。

T14 没有修改这些启动/管理边界。`SettingsPaths::fromEnvironment` 维持原有 XDG→HOME 规则本身正确，但并不等于 MODERNIME_* 已被映射到运行时 XDG。

### 独立实测

探针使用含空格的私有 prefix、HOME，调用真实 session-init；fcitx5/fcitx5-remote 都是假命令，绝不接触真实桌面或守护进程：

```text
MODERNIME_CONFIG_HOME = .../managed config
MODERNIME_DATA_HOME   = .../managed data
继承 XDG_CONFIG_HOME = .../old config
继承 XDG_DATA_HOME   = .../old data

实际假 Fcitx 子进程：
XDG_CONFIG_HOME = .../managed config
XDG_DATA_HOME   = .../old data

真实 data-guard 的只读 resolve_user_dirs：
.../managed data/modernime
.../managed config/modernime
```

配置覆盖被转换，数据覆盖没有。运行时可能访问 old data，管理保护却针对 managed data；存在误开旧库/保护错目录风险。本次只测路径事实，没有执行真实备份、clear、升级或删除。

### 最小目标与完成标准

- 保持 C++ 的 XDG→HOME 优先级，不通过新增 C++ MODERNIME_* 隐藏优先级掩盖问题。
- 在安装/管理启动边界统一生成有效 XDG 配置/数据环境，覆盖立即启动、autostart、settings desktop、runtime restart/probe 等真实入口；含空格值按 argv/env 原样传递。
- 对齐 data-guard 的管理覆盖与运行时访问事实；不得迁移/删除用户旧库，不改 SQLite/LMDB 格式或默认容量。
- 矩阵覆盖仅 HOME、仅 XDG、管理覆盖与 XDG 冲突、含空格非标准 prefix、两类活动数据库。使用假进程记录实际环境与真实库路径，不能只验证字符串拼接或用 Python 自建一个 SQLite。

## 5. R4 / 验收与架构契约缺口：测试必须观察真实消费者

### 5.1 配置脚本不是完整 A01–A07 行为证明

- A01/A02 Python 部分只验证 os.stat/rename/read_text，未驱动应用版本或 adapter/UI。
- A03/A05 是打印 PASS；已有 C++ 保存冲突/字号纯函数测试可作为部分证据，但不能扩写成新 UI 全流程已验。
- **A04 敏感热载只有注释和 print(PASS)，没有行为断言。**
- A06 只在临时目录创建一个 Python SQLite 数据库，未让应用/启动脚本选择路径，没有 LMDB 活动库覆盖。
- A07 用 Python 重写简单解析，没有调用 RuntimeController 的真实读取器，更没验证旧读者/未知字段与进程存活的区分。
- 配置脚本会回退到仓库旧 architecture-debug/release binary，当前构建缺产物时可能测试另一个版本。改为显式传当前 target 路径，缺少就明确失败/分类。

保留有价值的 SettingsFileStamp 与纯布局测试；补真正缺少的行为，不删除旧断言，也不把注释当执行证据。

### 5.2 已应用版本没有完整进入 UI/layout 契约

`layoutGeneration` 当前只声明和自增，没有被消费；`SettingsApplicationVersion` 只留在 adapter，本次没有把可比较应用版本带到 UI 的页面/布局交界。UI 保留独立读取时，需要明确它的内容版本与布局代次如何拒绝旧结果，不能仅靠给结构体加字段宣布已实现。

新增字号纯函数测试只证明“给旧 layout 传新 metrics 会拒绝”，尚未证明真实事件流确实把新版本传到点击入口、页面大小变化也有效、重建后新坐标可选、语义候选序列/消费范围不变。需要实际 adapter/UI 或同一生产入口的受控集成测试；不能增加另一套只供测试使用的应用逻辑。

### 5.3 文档订正与阶段纠偏

- `SettingsSaveResult::Conflict`、`SettingsStore::saveWithConflictDetection` 在当前代码中不存在；实际使用既有 `SettingsStore::save(...expectedContentHash...)` 和错误返回。应订正文档，不为迎合文档再添加 API。
- 多个报告测试函数名也与当前实际函数不一致，按代码和日志重新映射。
- “全部契约 100% 通过”不成立；缺失测试分别列 NotRun/未覆盖，不得借完整 CTest 绿色证明这些分支。
- **原 T15 是「设置数据异步与轻量统计（P2-02）」；不是报告写的「包管理沙箱与多包安装」。** 当前不进入任一种 T15，之后仍以原计划为准，不能从错误阶段描述生成新工作。

## 6. 渐进整改与完成标准

| 顺序 | 整改 | 必须看到的结果 |
|---|---|---|
| 1 | R1 一致配置读取 | 解析/hash/stamp 同源；替换交错后真实引擎最终应用最新合法内容 |
| 2 | R2 成功/失败与重试 | I/O 失败保持最后有效状态；同 stamp 恢复可成功；不虚报成功版本 |
| 3 | R3 启动环境协议 | 管理与运行时实际路径一致；含空格与活动 SQLite/LMDB 矩阵成立 |
| 4 | R4 集成验证/版本交界/记录 | 敏感热载、布局更新/旧点击、诊断兼容真实执行；必需测试注册与当前产物绑定 |

先固化本轮失败前测试，再最小修复。R1/R2 为同一配置事实边界，可以一并设计，但保留独立失败行为。不要引入通用 Manager/总线，不重开 T11/T12/T13，不修改候选排序、容量、数据格式或用户数据路径上的已有内容。

验证先运行定向的真实消费者/路径/故障矩阵与必要 ASAN，再完成原 T14 的 G1/G3/G5/G6、性能门限和隔离 fresh clone。G5 按原计划定义执行，不能用无关的存储 TSAN 成绩替代配置/算法行为证明；若确有并发改动再增加对应 TSAN。性能与编译/插桩测试分开，不为重复绿灯扩大无关测试成本。

## 7. 给下一位 AI 的可复制提示词

```text
在 /home/wsl/ModernIME 定向修复 T14 首轮复审 R1–R4。基线 7f2f05db34073b526a623d31039b4af06c9198e8；前序 P2 测试维护12e125d有效，T13生产架构已验收。核对HEAD/工作树，保留所有用户文件与历史未跟踪文档，不reset/clean，不进入T15，不推送、不发布。

完整阅读AGENTS.md、原七份架构/算法/稳定性/交接文档、原计划T14与G1/G3/G5/G6、docs/T13_FINAL_REVIEW_AND_T14_HANDOFF.md、docs/T14_REVIEW_AND_HANDOFF.md、docs/T14_ACCEPTANCE.md。

先将独立失败固化为确定性行为测试，再最小修复：
R1/P1：SettingsStore先解析A，再第二次打开路径hash到B，返回A内容/B hash；新hash去重导致下一轮B也不应用。真实Fcitx探针已复现disk_enabled=1但after_active=0，runtime却写B hash。建立一致读取结果，解析/rawLines/hash/stamp同源，保持既有文件hash算法和保存冲突契约。有界处理读取期间替换，adapter/UI只消费成功结果的事实。
R2/P1：读取失败返回默认启用配置/空hash，adapter/UI直接应用并先消费stamp；真实探针已复现磁盘禁用却after_active=1、preedit=n。区分成功/缺失/I/O失败/允许的解析诊断；热载失败保留最后有效配置，不推进成功版本，故障消失但stamp不变也可有界重试。不把全部diagnostics当失败，不改首次缺文件默认契约，不追加散乱fallback/flag。
R3/P1：MODERNIME_DATA_HOME没有转换到真实runtime XDG_DATA_HOME；真实session-init+假Fcitx看到old data，data-guard保护managed data。C++继续XDG→HOME；在安装/管理启动边界统一配置/数据env，覆盖立即启动/autostart/settings desktop/runtime启动入口。测HOME/XDG/管理覆盖冲突、空格prefix、真实SQLite/LMDB活动路径；不迁移/删旧库，不改格式。
R4/验收：configuration_contract_test.sh A04只有print PASS，A06只建Python SQLite，A07重写解析；补真实adapter/敏感热载/UI布局点击/RuntimeController/启动环境行为，当前target显式传参，禁止fallback旧build。让应用内容版本和布局代次进入真实UI交界，不能保留无消费的layoutGeneration就宣称防旧结果完成。旧字号/页面大小布局拒绝，新坐标恢复，语义候选和消费范围不变。

参考build/t14-review-evidence/的config_probe.cpp、config_hooks.c（实际fopen钩子）、adapter_probe.cpp、adapter-probe-results.json、path_probe.py和结果JSON。探针链接真实生产对象，所有I/O/假命令均私有隔离；退出0仅表示观察成功，不代表被测契约通过。不要把早期未命中hook的调试结果当验收。
保留有效stamp和字号纯函数断言，不删除测试、不新建通用Manager/配置总线、不机械拆文件。不修改候选排序/学习资格/容量/数据库格式/T11世代/T12-T13包操作协议。原T15是设置数据异步与轻量统计，不是包管理沙箱；本轮严格不实施。

验收包括：解析/hash间替换、连续替换、一次/永久I/O失败、同stamp恢复、缺失重建、外部保存冲突与注释/未知键、敏感组合/光标/禁学、真实旧布局拒绝/新布局选择、路径全矩阵与诊断真实读取兼容。新测试必须实际注册执行并绑定当前产物；脚本只聚合真实测试，不print伪造PASS。
重建当前源码，先做失败前后对照和定向G1/必要ASAN，再完成原G3/G5/G6、性能门限及隔离 ./tests/verify_fresh_clone.sh。无关TSAN不代替行为验证。记录新目录的测试时HEAD、patch/hash、binary、配置、命令、退出码、注册表、新JUnit与原始日志，不覆盖历史/审查证据。
重写docs/T14_ACCEPTANCE.md的实际契约/用例/证据映射；删去不存在的SettingsSaveResult::Conflict/saveWithConflictDetection表述，用真实API解释，不为文档新增API。区分Passed/Failed/Skipped/Blocked/NotRun，物理桌面继续待验。完成必要提交后停在T14复审，不进入T15、不推送、不发布。
```
