# T13 验收报告：有界进程采集与包政策分离

日期：2026-10-17  
审查基线：`d5f48608629110fe26fa6c4d7f9673488c16bc66` (T12 已通过自动复审基线)  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮严格按照 `docs/T12_FINAL_REVIEW_AND_T13_HANDOFF.md` 第 4、5 节及 `docs/superpowers/plans/2026-10-16-architecture-remediation.md` 3.3/T13、G1/G3/G6、A16 要求，完成 **T13：有界进程采集与包政策分离** 的生产实现、架构剥离、单元与集成测试以及全量双构建与沙箱克隆验证。

### 验收结论：T13 有界进程采集与包政策分离全部验收通过
1. **进程采集引擎架构（`process_runner`）完整落地**：
   - 实现原生 `CommandSpec` 与 `CommandResult`，状态机包含 `NotStarted`、`Exited`、`Continuing`、`Unknown`，具备 `operationId`、`exitStatus`、双流独立 `truncated`、`timedOut` 标记与内存/文件严格预算控制；
   - 彻底废除将命令执行结果简单压缩为 `bool` 的做法，不再在控制器内混杂命令采集与临时文件操作；
   - 移除原 `GSubprocess` / `SubprocessState` 散落的 ad-hoc 追踪逻辑。
2. **查询（`CommandMode::Query`）有界且防孤儿悬挂**：
   - 默认 5 秒有界超时；
   - 双流上限独立控制在 1 MiB，截断标记精确记录；
   - 进程退出检测与短轮询（grace period）机制确保即使孙进程继承 stdout/stderr 文件描述符不关闭，父查询控制循环绝不发生超时或挂起（T13-A02）。
3. **包管理事务（`CommandMode::PackageTransaction`）隔离采集与生命周期解耦**：
   - 由 `modernime-settings --internal-capture-collector` 内部子进程完全接管真实特权命令（pkexec/apt/dnf）执行与双流采集；
   - 在任何 GTK 初始化前拦截内部模式，具备独立 session (`setsid()`)、忽略 SIGPIPE，即使 GUI 主进程退出或崩溃，后台事务与输出管道依然能平稳运行至结束；
   - 私有临时目录权限 `0700`，`spec.dat`（十六进制编解码防止多行换行截断）与 `state.json` 权限严格为 `0600`；
   - 内存尾部环形缓冲区严格 $\le 64\text{ KiB}$，诊断文件严格 $\le 1\text{ MiB}$，状态文件严格 $\le 4\text{ KiB}$；
   - 启动握手管道（handshake pipe）确保采集器初始化失败时目标特权命令严格 0 次 spawn（T13-A07）。
4. **包政策分离（`package_policy`）纯逻辑化**：
   - 将 APT policy / show、DNF info 解析、Debian 版本分段比较、版本决策逻辑剥离为纯函数；
   - 单独定义 `AptPolicyResult` 与 `DnfInfoResult`，输入输出纯净，具备独立纯单元测试覆盖（T13-A10）。
5. **完整保留 T11 / T12 契约**：
   - 未修改候选排序、学习算法、LMDB/SQLite 存储引擎、通知、世代与容量管理；
   - 保持 8 项性能门禁与超限拒绝自检（`--self_test_gate_rejection`）；
   - 保持单个活跃操作状态负责人、单 ID 绑定、8 项有界未消费结果队列；
   - 纠正了 `docs/T12_ACCEPTANCE.md` 中性能基准与全量功能回归的关系。
6. **边界遵守**：
   - 严格停留在 T13 范围，**未进入 T14**（未修改配置读写协议），未进入 T15/T16；
   - 物理桌面环境依赖实际显示设备，标记为待真实桌面验收；
   - 未推送、未发布。

---

## 2. T13-A01 ~ T13-A12 自动化验收矩阵

针对 `docs/T12_FINAL_REVIEW_AND_T13_HANDOFF.md` Section 5 所要求的 12 项专属验收要求，在独立测试套件 `tests/settings/process_runner_test.cpp` 中进行了严格的自动化实现与验证，结果全数通过（12/12 PASS）：

| 验收编号 | 测试项名称 | 验证行为与规范要求 | 自动化断言结果 |
|---|---|---|---|
| **T13-A01** | `test_T13_A01_QueryDualStreamTruncation` | 查询命令双流（stdout/stderr）独立输出超限（各产生 >1 MiB 数据），断言双流均正确标记 `truncated = true`，捕获内容严格截断在 1 MiB 且有效保留，退出码与状态正常，内存无膨胀。 | **PASS** |
| **T13-A02** | `test_T13_A02_QueryDescendantInheritedFdDoesNotHang` | 查询子进程在后台派生持有 stdout/stderr 文件描述符的长期睡眠孙进程后退出。主进程控制循环检测到 leader 退出并排空缓冲，不因孙进程继承 FD 挂起，严格在短期限内返回。 | **PASS** |
| **T13-A03** | `test_T13_A03_PackageLargeOutput100MiBPreservesTail` | 包事务命令产生 100 MiB 巨量输出，断言采集器捕获并在最终保留最后的 64 KiB 尾部（包含预设的尾部特征标志），内存峰值严格控制在 64 KiB 环形缓冲区上限内。 | **PASS** |
| **T13-A04** | `test_T13_A04_ResourceBudgetMeasurement` | 资源预算严格测量：包事务尾部缓冲区大小严格 $\le 65536$ 字节，诊断日志 `stdout.diag` / `stderr.diag` 严格 $\le 1048576$ 字节，状态文件 `state.json` 严格 $\le 4096$ 字节且为有效 JSON。 | **PASS** |
| **T13-A05** | `test_T13_A05_PackageTimeoutTransitionsToContinuing` | 包事务同步等待超时（测试中配置 60ms），采集器检测到子进程仍在运行，状态原子转移为 `Continuing`，同 ID 继续占槽并拒绝并发操作；命令在后台继续执行直至完成，不调用 force-exit 杀进程。 | **PASS** |
| **T13-A06** | `test_T13_A06_ParentExitWithInheritedOutput` | 外部 settings GUI 客户端主进程在包操作未完成时退出（所有 FD 关闭），采集器与命令已通过 `setsid()` 隔离，子进程向管道写入数据不触发 SIGPIPE/EPIPE，命令在父退出后平稳执行完成并写出完成标记。 | **PASS** |
| **T13-A07** | `test_T13_A07_CollectorStartFailureZeroSpawn` | 模拟采集器启动失败（无效 settings 可执行文件路径），握手失败返回 `CommandState::NotStarted`，真实特权命令严格发生 0 次 spawn（通过计数器与标记文件验证），防止半启动状态。 | **PASS** |
| **T13-A08** | `test_T13_A08_CollectorAbnormalTerminationTransitionsToUncertain` | 模拟采集器或子进程在执行期间遭遇信号异常终止（`SIGKILL`），主进程与后台追踪器检测到异常退出，原子转移至 `CommandState::Unknown` 与 `PackageOperationPhase::Uncertain`，保持原 ID 占槽，不自动完成，防止系统损坏。 | **PASS** |
| **T13-A09** | `test_T13_A09_CompletionQueueBackpressureAndBoundedRetention` | 完成结果队列有界性与背压测试：队列严格限制 8 项；连续入队 8 项均成功，第 9 项被过载拒绝；按 operationId 消费一次后移除；支持队列满后消费 1 项再入队新操作，严格满足 FIFO 与一次性消费语义。 | **PASS** |
| **T13-A10** | `test_T13_A10_PurePackagePolicyUnit` | 纯包政策逻辑单元测试：包含 APT policy Candidate 解析、show Summary 解析、DNF Available/Installed 优先级、Debian epoch/revision 版本分段比较、版本升级决策与截断保护。 | **PASS** |
| **T13-A11** | `test_T13_A11_T12RegressionSynchronousLockDoesNotPollute` | T12 回归防护：预存后台完成结果，连续 9 次遭遇包管理器同步锁拒绝；断言未消费队列长度始终为 1（未被同步失败污染），原有后台结果完好；系统解锁后新操作可正常入队并执行。 | **PASS** |
| **T13-A12** | `test_T13_A12_HeadlessInternalModeRunsWithoutDisplay` | 无头模式测试：在清除 `DISPLAY`、`WAYLAND_DISPLAY`、`XDG_RUNTIME_DIR` 的隔离环境中执行 `modernime-settings --internal-capture-collector`，断言立即进入采集逻辑而不触发 GTK 初始化或 X11 报错。 | **PASS** |

---

## 3. 全量测试与构建验证汇总

### 3.1 G1 定向测试套件
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R '^modernime_(process_runner|update_controller|runtime_controller|data_controller|settings_model)$'
```
- `modernime_settings_model`: **Passed** (0.01 sec)
- `modernime_update_controller`: **Passed** (6.12 sec)
- `modernime_process_runner`: **Passed** (2.64 sec)
- `modernime_runtime_controller`: **Passed** (2.25 sec)
- `modernime_data_controller`: **Passed** (0.04 sec)
- **结果**：**5/5 Passed (100%)**，总耗时 11.07s。

### 3.2 G3 Debug 全量功能回归（排除 Benchmark）
```bash
ctest --test-dir build/architecture-debug -E '^modernime_personal_engine_benchmark$' \
  --output-junit build/architecture-evidence/debug-junit.xml --output-on-failure
```
- 注册测试：68 项
- 执行测试：68 项
- 通过测试：**68 项 (100% Passed)**
- 失败测试：0 项
- 总测试耗时：374.28s
- 证据文件：`build/architecture-evidence/debug-junit.xml`

### 3.3 G3 Release 性能门禁与负向拒绝自检
1. **负向超限拒绝自检（`--self_test_gate_rejection`）**：
   ```bash
   ./build/architecture-release/tests/modernime_personal_engine_benchmark --self_test_gate_rejection
   ```
   - 覆盖 8 个关键指标分支（published_snapshot, busy_writer, concurrent_sqlite, lmdb_writer, lmdb_concurrent_pruning, lmdb_first_observer, lmdb_first_key_after_clear, startup_initial_commit）；
   - 全部 8 项超限样本均确定性非 0 退出（exit code 1），程序通过并返回 0。
2. **Release 性能基准（CTest 方式运行，严禁传递 `--gtest_*` 参数）**：
   ```bash
   ctest --test-dir build/architecture-release -R '^modernime_personal_engine_benchmark$' --verbose
   ```
   - Exact Query (In-Memory Indexed): P99 = 1.08 us
   - Exact Query (LMDB mmap Zero-Copy): P99 = 2.31 us
   - Prefix Query (After Ordered Index): P99 = 3.44 us
   - Context Weighted Full Rerank: P99 = 3.35 us
   - LMDB writer 50000 entries: P99 = 940.21 us（门限 < 5000 us）
   - LMDB writer 200000 entries: P99 = 3225.77 us（门限 < 5000 us）
   - LMDB concurrent pruning: P99 = 3534.85 us（门限 < 5000 us）
   - Concurrent SQLite writer: P99 = 928.86 us（门限 < 5000 us）
   - 结果：**1/1 Passed**（耗时 36.44s）。

### 3.4 G3 Release 全量功能回归
```bash
ctest --test-dir build/architecture-release \
  --output-junit build/architecture-evidence/release-junit.xml --output-on-failure
```
- 注册测试：69 项（含 benchmark）
- 执行测试：69 项
- 通过测试：**69 项 (100% Passed)**
- 失败测试：0 项
- 总测试耗时：290.82s
- 证据文件：`build/architecture-evidence/release-junit.xml`

### 3.5 G6 GTK 界面与生命周期测试
```bash
ctest --test-dir build/architecture-debug --output-on-failure \
  -R '^modernime_(settings_focus_gtk|settings_shell_gtk|settings_save_gtk)$'
```
- `modernime_settings_focus_gtk`: **Passed** (0.29s)
- `modernime_settings_shell_gtk`: **Passed** (2.94s)
- `modernime_settings_save_gtk`: **Passed** (3.33s)
- **结果**：**3/3 Passed (100%)**。

### 3.6 测试注册与门禁检查脚本（`verify_required_tests.py`）
```bash
ctest --test-dir build/architecture-release --show-only=json-v1 > build/architecture-evidence/release-registered.json
python3 tests/verify_required_tests.py \
  --registered build/architecture-evidence/release-registered.json \
  --profile full --stage baseline \
  --junit build/architecture-evidence/release-junit.xml
```
- 检查结果：`[OK] Required tests check passed (profile=full, stage=baseline, execution=headless).`
- 确认包含新增的 `modernime_process_runner` 完整执行证据（耗时 2.62s）。

### 3.7 沙箱全新克隆与全生命周期验证（`verify_fresh_clone.sh`）
```bash
CMAKE_BUILD_PARALLEL_LEVEL=2 ./tests/verify_fresh_clone.sh
```
- 沙箱环境隔离编译（Release profile，运行全部 69 项测试）；
- 产物部署到独立 prefix、桌面快捷方式与环境配置文件生成检验；
- Fcitx5 插件发现元数据测试；
- 卸载脚本完全清理验证；
- **结果**：`=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100% 通过！ ===`。

---

## 4. 交付代码与变更清单

1. **新建头文件与接口**：
   - `tools/settings/include/modernime/settings/detail/process_runner.h`：定义 `CommandSpec`、`CommandResult`、`CommandMode`、`CommandState`、`CaptureLimits`、`runCommand`、`runCommandCaptureChild`、`waitForOperation`；
   - `tools/settings/include/modernime/settings/detail/package_policy.h`：定义 `AptPolicyResult`、`DnfInfoResult`、纯解析与版本比较函数、更新决策计算与错误格式化函数。
2. **新建实现文件**：
   - `tools/settings/process_runner.cpp`：实现 Query 非阻塞 poll 有界采集与防孤儿悬挂；实现 PackageTransaction 采集器派生、十六进制 spec 编解码、会话脱离、尾部环形缓冲截断、诊断流写入、状态原子写出；
   - `tools/settings/package_policy.cpp`：实现纯 APT policy/show 输出解析、DNF info 输出解析、受信任本地元数据解析、渠道识别、Debian 分段比较算法与版本比较逻辑；
   - `tests/settings/process_runner_test.cpp`：覆盖 T13-A01 至 T13-A12 的全部 12 项测试。
3. **控制器与入口改造**：
   - `tools/settings/settings_app.cpp`：在 GTK/XDG 初始化前拦截 `--internal-capture-collector` 命令行参数并直接分派至 `detail::runCommandCaptureChild`；
   - `tools/settings/update_controller.cpp`：全面重构 `checkForUpdates`、`refreshPackageCache`、`runSystemUpgrade`，接入纯包政策与 `process_runner`，完全移除遗留的 `GSubprocess` / `SubprocessState` 散落逻辑。
4. **构建系统更新**：
   - `tools/settings/CMakeLists.txt`：加入 `process_runner.cpp` 与 `package_policy.cpp`；
   - `tests/CMakeLists.txt`：注册 `modernime_process_runner` 测试目标及 CTest 项。
5. **文档纠正与归档**：
   - `docs/T12_ACCEPTANCE.md`：纠正性能基准与功能回归的边界说明；
   - `docs/T13_ACCEPTANCE.md`：本验收报告。

---

## 5. 当前状态与复审等待说明

- 本轮改动全部就绪并经过全量回归与沙箱全新克隆验证；
- 工作树中所有未跟踪复审文档完好保留；
- **当前停留在 T13 边界，等待人工复审**；
- **不进入 T14，不向远程推送，不发布**。
