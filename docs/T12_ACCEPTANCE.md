# T12 验收报告：完整包操作所有权（P1-06 第一部分）

日期：2026-10-16  
基线提交：`505e6e6b186b836eb60af269f39402c0182ad963`（T11 最终复审通过基准）  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮严格按照 `docs/T11_FINAL_REVIEW_AND_T12_HANDOFF.md` 第 4 节及 `docs/superpowers/plans/2026-10-16-architecture-remediation.md` 第 3.3 节要求，完成 **T12 完整包操作所有权** 的最小化闭环实现与确定性验证。

**结论：T12 完整包操作所有权验收通过。**
- 实现了 spawn 前原子占槽与全生命周期单一状态负责人；
- 实现了基于 `operationId` 的状态转移、旧 ID 防篡改与一次性消费；
- 实现了最多 8 项未消费完成结果的有界队列，满时拒绝新操作且不丢旧结果；
- 实现了 `Idle → Starting → Running → Continuing → Completed` 与异常 `Uncertain` 状态转移及占槽语义；
- 6 项必需测试全部通过，G1 定向测试全部通过，G3 Debug 全量 67 项测试 100% 通过；
- Release 性能门禁与 8 项负向拒绝自检全部通过，保留了历史阈值及退出码校验；
- 严格停在 T12 边界，**未进入 T13**（未引入采集器子进程，未重构 APT/DNF 政策）；未推送、未发布。真实桌面交互受限于无物理显示环境标为“待验”。

---

## 2. 核心架构与实现

### 2.1 状态机与状态负责人
在 `tools/settings/include/modernime/settings/package_operation_state.h` 与 `tools/settings/package_operation_state.cpp` 中定义并实现了无锁竞争冲突、线程安全的包操作状态管理：

```cpp
enum class PackageOperationKind {
    RefreshCache,
    SystemUpgrade,
};

enum class PackageOperationPhase {
    Idle,
    Starting,
    Running,
    Continuing,
    Uncertain,
    Completed,
};
```

- **原子占槽 (`tryBeginPackageOperation`)**：在 spawn 之前原子将状态从 `Idle` 转移到 `Starting`，并分配单调自增的 `operationId`。若当前非 `Idle` 或未消费结果队列已满（$\ge 8$ 项），原子拒绝并返回 `std::nullopt`。
- **阶段转移 (`advancePackageOperation`)**：
  - `Starting → Running`：在子进程成功被 `g_subprocess_launcher_spawnv` 创建后立即转移；
  - `Running → Continuing`：当主线程同步等待超时且检测到子进程仍在运行时转移，交由后台线程跟踪；
  - `Starting/Running/Continuing → Uncertain`：当进程非正常退出（如信号中断、状态无法确认）时转移，持续占槽并禁止自动重试或重复触发；
  - `Starting/Running/Continuing/Uncertain → Completed`：完成并释放占槽，回归 `Idle`。
- **单 ID 绑定与防串扰 (`finishPackageOperation`)**：只有传入的 ID 与当前活跃的 `activeOperationId` 一致时才允许结束或转移，旧 ID 或错误 ID 的调用直接拒绝，绝不影响新操作。
- **有界未消费结果队列**：最多保留 8 项未消费完成结果（`kMaxUnconsumedResults = 8`）。通过 `takeCompletedPackageOperationResult(id)` 支持按 ID 一次性消费；若队列满则新操作在 `tryBegin` 处显式报告过载拒绝，保护历史结果不被覆盖。
- **API 兼容门面**：`UpdateController` 静态方法完整保留并代理至底层包操作状态，页面 `busy` 仅作为 UI 展示与敏感度控制，不再维护独立的并发或占槽状态。

---

## 3. 六项必需行为测试验证（tests/settings/update_controller_test.cpp）

| 测试项 | 验证行为 | 结果 |
|---|---|---|
| **NormalWaitRejectsConcurrentOperation** | 假 pkexec 在 spawn 后触发确定性栅栏阻塞；此时 worker 处于正常等待阶段（`Running` 态）；主线程并发发起 `refreshPackageCache` 与 `runSystemUpgrade` 均立即被拒绝并返回忙提示；栅栏放行后检查 `spawnCount` 严格等于 1。 | **PASS** |
| **AuthenticationFailureReleasesSlot** | 模拟认证取消/失败（pkexec exit 127）及 spawn 失败（无效解释器），均按同一 operationId 正常释放槽位，回到 `Idle`；后续合法操作可立即开始并成功完成。 | **PASS** |
| **CompletionConsumedOnce** | 连续执行两次操作，两个 operationId 结果均正确入队；按 ID 消费第 1 项后，第二次按同一 ID 消费返回 `std::nullopt`；第 2 项不受影响，消费后队列归零。 | **PASS** |
| **OldCompletionCannotFinishNewOperation** | 旧操作结束后，开始新操作；使用旧 operationId 调用 `finishPackageOperation` 或 `advancePackageOperation` 均返回 `false`，新操作的 `operationId` 与 `Running` 状态完好无损。 | **PASS** |
| **UnconsumedResultsHaveBound** | 连续生成 8 项未消费完成结果，队列达到上限 8；第 9 次 `tryBeginPackageOperation`、`refreshPackageCache` 均被拒绝并提示队列已满；消费 1 项后，第 9 次操作成功开始并完成；FIFO 顺序全部得到验证。 | **PASS** |
| **PageOpenCloseAndStatusTransitions** | 操作启动后模拟页面销毁与重开，操作未被意外释放；超时转 `Continuing` 继续占槽；异常转 `Uncertain` 继续占槽且拒绝自动重试；仅在明确人工处置调用 `finishPackageOperation` 后才释放回到 `Idle`。 | **PASS** |

---

## 4. 全量验证套件与证据清单

### 4.1 G1 任务定向回归
命令：
```bash
ctest --test-dir build/architecture-debug --output-on-failure --no-tests=error \
  -R '^modernime_(update_controller|runtime_controller|data_controller|settings_model)$'
```
结果：**4/4 Passed**（0 失败，耗时 3.58s）。
- `modernime_settings_model`: Passed (0.01s)
- `modernime_update_controller`: Passed (1.28s)
- `modernime_runtime_controller`: Passed (2.25s)
- `modernime_data_controller`: Passed (0.04s)

证据文件：`build/t12-evidence/g1/g1.log`、`build/t12-evidence/g1/g1-junit.xml`。

### 4.2 G3 Debug 全量 CTest
命令：
```bash
ctest --test-dir build/architecture-debug --output-on-failure --no-tests=error \
  -E '^modernime_personal_engine_benchmark$'
```
结果：**67/67 Passed**（0 失败，耗时 384.56s）。
覆盖全部 GTest、LMDB/SQLite 存储测试、Fcitx5 适配层、渲染与布局、包安装与隔离测试等。

证据文件：`build/t12-evidence/g3/debug-ctest.log`、`build/t12-evidence/g3/debug-junit.xml`。

### 4.3 G3 Release 性能门禁与负向拒绝自检
1. **负向拒绝自检（`--self_test_gate_rejection`）**：
   - 8 项延迟拒绝分支全部独立验证（超限即退出码 1）；自检程序整体退出码 0。
   - 证据文件：`build/t12-evidence/g3/release-negative-gate.log`。
2. **Release Benchmark 真实负载运行**：
   - LMDB 200k Writer P99: **3045.06 us**（门限 <5000 us）
   - LMDB 200k Writer max: **5256.73 us**（门限 <30000 us）
   - LMDB 并发剪枝 P99: **3543.12 us**（门限 <5000 us）
   - LMDB 并发剪枝 max: **6897.18 us**（门限 <30000 us）
   - 并发 SQLite max: **29794.44 us**（门限 <30000 us）
   - LMDB 首 Observer 延迟: **2000.63 us**（门限 <5000 us）
   - 清空后首键延迟: **4654.87 us**（门限 <5000 us）
   - 进程整体退出码 0。
   - 证据文件：`build/t12-evidence/g3/release-performance.log`、`build/t12-evidence/g3/performance-junit.xml`。

### 4.4 G6 GTK 接口验证
命令：
```bash
ctest --test-dir build/architecture-debug --output-on-failure --no-tests=error \
  -R '^modernime_(settings_focus_gtk|settings_shell_gtk|settings_save_gtk)$'
```
结果：**3/3 Passed**（0 失败，耗时 6.61s）。
证据文件：`build/t12-evidence/g6/desktop-gtk.log`、`build/t12-evidence/g6/desktop-junit.xml`。

### 4.5 测试注册与完整性校验（tests/verify_required_tests.py）
命令：
```bash
python3 tests/verify_required_tests.py \
  --registered build/t12-evidence/g3/debug-registered.json \
  --profile full --stage baseline \
  --junit build/t12-evidence/g3/debug-junit.xml \
  --junit build/t12-evidence/g3/performance-junit.xml \
  --execution headless
```
结果：`[OK] Required tests check passed (profile=full, stage=baseline, execution=headless).`

---

## 5. 变更文件与范围审查

本次提交严格限制在 T12 所需范围内：
- `tools/settings/include/modernime/settings/package_operation_state.h`：新建，包操作状态定义与管理器接口；
- `tools/settings/package_operation_state.cpp`：新建，状态转移、单 ID 校验、有界队列与线程安全实现；
- `tools/settings/include/modernime/settings/update_controller.h`：更新，引入包操作状态并声明代理静态方法；
- `tools/settings/update_controller.cpp`：更新，接入 `tryBeginPackageOperation`、`advancePackageOperation`、`finishPackageOperation`，前台同步完成与后台超时跟踪；
- `tools/settings/CMakeLists.txt`：将 `package_operation_state.cpp` 加入 `modernime-settings` 构建；
- `tests/CMakeLists.txt`：将 `package_operation_state.cpp` 加入测试目标构建；
- `tests/settings/update_controller_test.cpp`：新增 6 项必需的 T12 验收测试。
- `docs/T12_ACCEPTANCE.md`：本验收文档。

**未修改**：
- 未修改任何已验收的 T11 候选排序、世代、容量契约、通知或构建配置；
- 未实施 T13 的独立内部采集子进程或流式预算管理；
- 未修改或放宽任何性能门限参数。

---

## 6. 状态结论与复审等待

T12 完整包操作所有权已经全部实现并自动验证通过。  
按照要求，本轮到此停下，不推送、不发布、不进入 T13，等待用户复审。
