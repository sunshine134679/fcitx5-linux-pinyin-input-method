# AI 改动前后检查清单

适用：新增功能、修复、重构、参数/数据/依赖/构建变更、打包及在线更新维护。与 [项目基础与 AI 修改护栏](项目基础与AI修改护栏.md) 配合使用；不是第二份算法台账或另一套测试门限。

## 1. 开始修改前

- [ ] 阅读 `AGENTS.md`、适用子目录规范及护栏；算法相关完整阅读 [算法演化记录](算法演化记录.md) 的必读清单/不变量，并核对 [算法架构](算法架构.md)、[算法变更指南](算法变更指南.md) 和相关 [候选契约](候选链路架构.md)。
- [ ] 记录当前提交、分支及已有改动；核对实际运行/安装身份。不要凭报告中的绝对路径或固定版本认定自己正在修改正确代码。**引擎/插件改动部署后，必须以 `/proc/<fcitx5-pid>/maps` 核对进程实际加载的 .so inode 与构建产物一致；`fcitx5-remote -r` 只重载配置、不重载插件，不构成部署完成（2026-10-24 zongwen 实机事故教训，映射含 `(deleted)` 即为旧引擎）。**
- [ ] 明确类型：新增、行为调整、无意图行为变化的重构、停用或删除。仅文档工作不假称需要产品功能审批，也不随手改生产代码。
- [ ] 列出涉及的契约、输入/输出、状态所有者、线程及生命周期；新增状态写明唯一写者和失效条件。重构说明调用顺序是否保持。
- [ ] 找到真实消费者及验证入口；有缺陷则先建立能在旧行为上失败的用例，有既有契约则保留原断言。不把只测新 helper 当作端到端集成完成。
- [ ] 明确数据兼容、隐私、容量、性能和回退影响；不存在相关影响时给出简短依据，不为所有微小改动套一遍全部测试。

## 2. 按影响选择验证

下表列出当前真实入口；将来目标改名应同时更新接线、必需项契约和本表。CTest 目标与 GTest 套件不是同一种计数。

| 改动范围 | 应覆盖的现有入口和实际行为 |
|---|---|
| 匹配、排序、中英、前缀、混排或学习策略 | CTest 的 `modernime_pinyin_match`、`modernime_dictionary_prior`、`modernime_pinyin_quality`、`modernime_pinyin_provider`、`modernime_english_arbitration`、`modernime_pinyin_daily_simulation`、`modernime_pinyin_engine`；GTest 的候选身份/学习顺序回归；按现有算法规范执行完整 CTest/GTest/Benchmark |
| 选择、分页、上下文、取字或反馈 | `modernime_engine_state`、`modernime_fcitx5_input_panel`、`modernime_fcitx5_candidate_list`、`modernime_candidate_bar_layout`、`modernime_candidate_bar_renderer`，以及 GTest `ArchitectureSelectionRegression`；覆盖真实键鼠/消费范围/旧页失效 |
| Writer、快照、清空及异步启学 | `modernime_personal_engine_gtest` 的 `ArchitectureWriterRegression`、`ArchitectureResourceCoordinationTest`、`PersonalizationSnapshotRegression`；真实二次 clear、失败恢复、首提交、首键、快照释放与相应 Release 基准 |
| LMDB 事务、锁、扩容及环境所有权 | GTest `ArchitectureLmdbRegression`、`modernime_learning_store`，包含换库、RO/RW、close/reopen、真实 MAP_FULL、最后持有者；相关 TSAN/ASAN，不能只测单线程成功路径 |
| 外部导入、本地恢复及容量 | `ArchitecturePersonalFileRegression`、`modernime_learning_store` 及现有导入/学习用例；两后端实际查询、频次/屏蔽/世代、容量边界与重开 |
| 子进程、包更新及采集器 | `modernime_process_runner`、`modernime_update_controller`、`modernime_updates_page_contract`；fork 反例、输出预算、启动不明、等待超时、队列、旧 ID、页面关闭及安装后内部模式 |
| 设置文件、路径、运行时诊断 | `modernime_settings`、`modernime_settings_model`、`modernime_runtime_controller`、`modernime_runtime_controller_concurrency`、`modernime_configuration_contract`、`modernime_ui_addon`、`modernime_adapter_recovery`；同 stamp 故障解除、原子替换与真实消费者 |
| GTK 数据页与导航 | `modernime_data_controller`、`modernime_settings_data_async_gtk`、`modernime_settings_focus_gtk`、`modernime_settings_shell_gtk`、`modernime_settings_save_gtk`、`modernime_page_registry`、`modernime_settings_navigation_dbus`；真实在途打断/读后挂起、主循环心跳及实际控件状态 |
| 依赖或 CMake/CI | full 与 core-only 配置，`modernime_required_tests_contract`、`tests/required_tests_contract_test.py`；注册和执行双检查，失败/缺报告/非法 skip 负向保护 |
| 安装、卸载、迁移或数据备份 | `modernime_install_transaction`、`modernime_install_runtime`、`modernime_install_environment`、`modernime_install_channel_collision`、`modernime_upgrade_data_protection`、`modernime_install_rollback_concurrent_writes` 等；按 AGENTS 执行 `tests/verify_fresh_clone.sh` 与相关真实包门禁 |
| 正式发布或更新协议 | 软件源/安装门禁、网站 Python/Node 测试、生产签名、不可变 tag→源码→资产身份；真实旧客户端/旧渠道升级，不能只跑 `tests/previous_release_upgrade_test.sh` 就宣称 APT/DNF 旧 GUI 直升 |
| 仅正文、注释或链接 | 检查链接/符号链接/示例命令/目标名称、`git diff --check` 和改动范围；无产品改动不为此构建整仓或重启用户输入法 |

跨模块行为要覆盖连接处，不是把表中若干局部绿灯拼成一次完整端到端通过。GUI 操作使用隔离测试数据；不为了验收清空日常词库。

## 3. 执行证据的最小标准

1. **身份清楚**：代码 SHA、未提交补丁、构建类型、功能开关、编译器/模型、测试命令及产物身份可追溯；原生与插桩、full 与 core-only 分开。
2. **原始记录保留**：完整日志、原生 XML/注册表及实际退出码，不以摘要替原件，不以复跑通过抹去第一次失败。
3. **计数来自当前注册**：`ctest --show-only=json-v1` 与执行报告配对，GTest 必需套件实际运行；不能仅报“测试数没减少”。
4. **状态如实**：Passed/Failed/Skipped/NotRun/Blocked 分开；headless 只允许白名单 skip，desktop 不能把 skip 算通过。自动 GUI 不等于用户肉眼确认。
5. **检测器范围明确**：启用 Fcitx5 才包含相关 Ready 端点；检测器退出非零不叫通过。记录 LSan 抑制范围；ASAN 无 UAF 不等于 TSAN 无竞争。
6. **预算测量完整**：原生 Release 门禁使用真实 50k/200k、写入/剪枝/扩容及快照释放；保持负向超限拒绝。不要用 Debug 默认关闭的门禁、忽略的参数或吞退出码制造通过。
7. **夹具隔离且真命中**：每进程唯一临时目录，安装安全测试满足真实 `/tmp` 前提；缺产物 fail，不跨 worktree 偷用旧二进制。确定性栅栏证明交错，负向变体确实被目标断言拒绝。
8. **复用有依据**：明确旧基线、相关源码/配置等价及未重跑范围；不得把若干次结果合成一次虚构全绿。JUnit 若从日志转换，应标来源和转换规则，不漏失败或伪造耗时。
9. **共享资源不竞争**：本机有全局 machine.lock 时重构建/压测持同一锁，不替换锁 inode、不让子脚本重复申请造成自锁；不要为性能门禁一味扩大超时。

### 当前 checker 用法示例

以下假设已经为本次变更配置好 Debug/Release 构建并按影响决定需要全量验证；不是要求每次文档修改执行全部命令。路径使用当前工作树，不能照搬历史已删除目录。

```bash
task_debug_dir="$PWD/build/fcitx5-debug"
task_release_dir="$PWD/build/maintenance-release"
task_proof_dir="$PWD/build/maintenance-evidence"
mkdir -p "$task_proof_dir/gtest"
ctest --test-dir "$task_debug_dir" --show-only=json-v1 > "$task_proof_dir/registered.json"
ctest --test-dir "$task_debug_dir" --output-on-failure --output-junit "$task_proof_dir/debug-junit.xml"
ctest --test-dir "$task_release_dir" --output-on-failure --output-junit "$task_proof_dir/release-junit.xml"
"$task_debug_dir/tests/modernime_personal_engine_gtest" --gtest_output="xml:$task_proof_dir/gtest/gtest.xml"
python3 tests/verify_required_tests.py \
  --registered "$task_proof_dir/registered.json" --profile full --stage final \
  --junit "$task_proof_dir/debug-junit.xml" --junit "$task_proof_dir/release-junit.xml" \
  --gtest-xml "$task_proof_dir/gtest" --execution headless
```

若构建目录不存在先按实际需要配置，Release 不存在现成 preset 时用显式 `cmake -S . -B ... -DCMAKE_BUILD_TYPE=Release` 及完整所需开关；不要编造 `fcitx5-release` preset。执行命令的调用者须保留每一步失败及退出码，脚本中不得无条件继续并仅检查最后一行。真实桌面执行时使用 `--execution desktop`，不为放行 skip 改成 headless。

## 4. 结束前自查

- [ ] 新功能没有增加反向依赖或第二个排序/学习/配置/事务事实来源。
- [ ] 没有因修复而降低容量、吞失败、削弱隐私/清空/消费边界，或以修改测试代替修复代码。
- [ ] 运行代码、测试、数据、文档的变化均有意图；无关文件和其他人的改动未进入提交。
- [ ] 算法实现通过验证后更新既有演化记录及相关契约；新增/停用功能保留稳定 ID 和退役历史。
- [ ] 依赖/设置/安装/版本/更新发生变化时同步对应 README、CMake、脚本、官网或发布说明，不只改当前机器。
- [ ] 最终报告区分已执行、复用、失败、未测和环境局限，说明用户可观察变化及数据回退；“当前未观察到”不写成永久安全保证。
- [ ] 按本次用户授权推进必要交付，不机械继承旧任务的等待/禁止状态；也不把一次发布授权扩大为任意数据清理或历史改写。

## 5. 最小变更说明模板

```text
类型与目标：
当前代码/运行基线：
涉及模块及 ALG/CAND 契约：
具体输入/操作下的前后行为：
唯一状态所有者、线程和失效条件：
必须保持的既有行为与理由：
数据/容量/隐私/性能/安装/升级影响及回退：
验证入口、真实命令、配置、退出码与证据：
复用、未跑、失败及环境范围：
应更新的长期文档：
```

不适用项说明理由即可，不要求另开审批、另造台账或长篇流程文档。目标是让下一位维护者能判断改动是否保住已有基础。
