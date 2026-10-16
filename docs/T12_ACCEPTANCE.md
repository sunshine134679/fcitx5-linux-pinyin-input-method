# T12 验收报告：完整包操作所有权（P1-06 第一部分）与复审整改闭环

日期：2026-10-16  
审查基线：`f67eb49dcca50e7c24302d2cf1c97c771aebffdf`  
T11 已验收基线：`505e6e6b186b836eb60af269f39402c0182ad963`  
当前分支：`main`  

---

## 1. 任务概述与验收结论

本轮严格按照 `docs/T12_REVIEW_AND_HANDOFF.md` 第 4 节及 `docs/superpowers/plans/2026-10-16-architecture-remediation.md` 3.3/T12 要求，完成 **T12 完整包操作所有权** 的复审缺陷修复（R1 / P1）、页面生命周期与异常状态验收补齐（R2）、以及全量 Release CTest 证据归档和沙箱全新克隆验证。

**结论：T12 完整包操作所有权及复审整改项全部验收通过。**
- **R1 / P1 缺陷修复**：纠正了 `UpdateController::refreshPackageCache` 同步锁拒绝错误入队未消费结果的缺陷，保持了同步交付与后台待消费队列的严格契约；
- **R1 确定性回归**：连续 9 次锁拒绝保持 Idle、未消费队列不增、`spawnCount == 0`，保留的原后台结果完好并保持一次消费语义，解锁后同进程下次假操作成功且 `spawnCount == 1`；
- **R2 真实页面生命周期验证**：在 `modernime_settings_shell_gtk` 中接入真实 GTK `UpdatesPage` 与 `SettingsShell`，在隔离假包操作运行及栅栏阻塞期间进行页面创建、销毁与重建，断言 `operationId` 不变、槽位持续占用、并发请求拒绝、`spawnCount == 1`，放行后正常完成；
- **R2 真实 Uncertain 路径回归**：在 `modernime_update_controller` 中新增正常等待期被信号终止、以及超时转后台后被信号终止两个真实生产异常路径测试，断言均正确转移至 `Uncertain` 且持续占槽，禁止自动重试与伪完成；
- **全量测试套件通过**：G1 定向 4/4 Passed，G3 Debug 全量 67/67 Passed，G3 Release 全量 68/68 Passed，Release 性能门禁与 8 项负向拒绝自检全部通过；
- **安装生命周期验证**：在隔离沙箱环境中执行 `./tests/verify_fresh_clone.sh`，全新克隆构建、安装、产物检查与干净卸载 100% 通过；
- **严格限定范围**：停在 T12 边界，**未进入 T13**（未引入采集器子进程，未重构 APT/DNF 政策）；未修改已验收 T11 的候选排序、学习、容量、格式、世代、通知与性能门禁代码；物理桌面人工交互受限于无物理显示环境标为“待验”；未推送、未发布。

---

## 2. 核心架构与 R1/R2 复审整改实现

### 2.1 状态机与状态负责人架构
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

### 2.2 R1 / P1 缺陷根因与修复
- **根因分析**：`UpdateController::refreshPackageCache`（`tools/settings/update_controller.cpp:1076–1081`）检测到外部包管理器锁占用时，原调用为 `finishPackageOperation(*opId, res);`，因使用了默认参数 `recordUnconsumed = true`，导致同步失败被重复记录到后台完成队列中。当累计 8 次锁拒绝后，未消费队列达到上限 8 项，导致后续包操作即使解除系统锁也被“队列已满”阻塞。
- **最小修复**：将该调用显式修正为：
  ```cpp
  finishPackageOperation(*opId, res, {}, {}, false);
  ```
  核对全部 18 处 `finishPackageOperation` 调用点，确保所有同步返回分支（缺依赖、锁占用、spawn 失败、认证失败、同步完成）均严格传递 `recordUnconsumed = false`；仅后台线程完成追踪（line 324）才保留待消费结果并传递默认 `true`。
- **独立复现与修复验证**：
  - 修复前运行 `build/t12-review-evidence/locked_refresh_probe`，在第 8 次拒绝后报告 `after_unlock success=0 message=包操作结果队列已满...`，退出码 1，确认缺陷；
  - 修复后重新链接运行该探针，8 次拒绝后未消费队列计数始终为 0，解锁后下一次操作立即成功（`after_unlock success=1 message=已刷新系统软件源缓存...`），探针退出码 0。

---

## 3. 行为测试验证清单

### 3.1 控制器与状态机测试（tests/settings/update_controller_test.cpp）

| 测试项 | 验证行为 | 结果 |
|---|---|---|
| **NormalWaitRejectsConcurrentOperation** | 假 pkexec 在 spawn 后触发确定性栅栏阻塞；此时处于正常等待阶段（`Running` 态）；主线程并发发起 `refreshPackageCache` 与 `runSystemUpgrade` 均立即被拒绝并返回忙提示；栅栏放行后检查 `spawnCount` 严格等于 1。 | **PASS** |
| **AuthenticationFailureReleasesSlot** | 模拟认证取消/失败（pkexec exit 127）及 spawn 失败（无效解释器），均按同一 operationId 正常释放槽位，回到 `Idle`；后续合法操作可立即开始并成功完成。 | **PASS** |
| **CompletionConsumedOnce** | 连续执行两次操作，两个 operationId 结果均正确入队；按 ID 消费第 1 项后，第二次按同一 ID 消费返回 `std::nullopt`；第 2 项不受影响，消费后队列归零。 | **PASS** |
| **OldCompletionCannotFinishNewOperation** | 旧操作结束后，开始新操作；使用旧 operationId 调用 `finishPackageOperation` 或 `advancePackageOperation` 均返回 `false`，新操作的 `operationId` 与 `Running` 状态完好无损。 | **PASS** |
| **UnconsumedResultsHaveBound** | 连续生成 8 项未消费完成结果，队列达到上限 8；第 9 次 `tryBeginPackageOperation`、`refreshPackageCache` 均被拒绝并提示队列已满；消费 1 项后，第 9 次操作成功开始并完成；FIFO 顺序全部得到验证。 | **PASS** |
| **SynchronousLockRejectionDoesNotPolluteUnconsumedQueue (R1 回归)** | 预存 1 项后台完成结果；连续 9 次系统锁拒绝，每次均返回失败、槽位回到 Idle、未消费结果数保持为 1（不递增）、`spawnCount == 0`；原预存结果在 9 次拒绝后内容与 ID 不变且可且仅可消费一次；解锁后下一次操作成功且 `spawnCount == 1`。 | **PASS** |
| **StatusTransitionsUnitContract** | 纯状态机转移契约测试：Starting → Running → Continuing → Uncertain → Completed；处于 Continuing 或 Uncertain 期间拒绝并发操作；Uncertain 不自动完成或重试，仅显式 finish 释放。 | **PASS** |
| **NormalWaitSignalTerminationTransitionsToUncertain (R2 回归)** | 正常等待期子进程被信号中断（`kill -TERM $$`），生产控制器自动将阶段转移至 `Uncertain`，错误信息提示状态无法确定，槽位持续占用，拒绝并发操作，不污染未消费队列，仅人工干预后释放。 | **PASS** |
| **TimeoutBackgroundSignalTerminationTransitionsToUncertain (R2 回归)** | 超时转入后台（Continuing）后，后台跟踪线程检测到子进程被信号中断，原子转移至 `Uncertain`，槽位持续占用，拒绝并发操作，不自动 finish，仅人工干预后释放。 | **PASS** |

### 3.2 真实 GTK 页面生命周期测试（tests/settings/settings_shell_gtk_test.cpp）

| 测试项 | 验证行为 | 结果 |
|---|---|---|
| **UpdatesPage & SettingsShell Lifecycle During Active Op (R2 补齐)** | 启动由生产 `UpdateController` 驱动的隔离假包操作（假 pkexec 阻塞在带看门狗的栅栏）；操作处于 `Running` 态且 `activeOpId` 有效；创建真实 GTK `UpdatesPage` 控件，销毁页面控件，重建页面控件；创建真实 `SettingsShell` 窗口并切换到 Updates 页面，切换至 Overview，销毁 Shell 窗口；在全过程中断言 `hasActivePackageOperation() == true`、`operationId` 严格不变、并发刷新/升级操作均被忙拒绝、`spawnCount == 1`；最后放行栅栏，假进程正常退出，状态机干净回到 `Idle`，最终 `spawnCount` 依然严格为 1。 | **PASS** |

---

## 4. 全量验证套件与证据清单

### 4.1 G1 任务定向回归
命令：
```bash
ctest --test-dir build/architecture-debug --output-on-failure --no-tests=error \
  --output-junit $(pwd)/build/t12-evidence/g1/g1-junit.xml \
  -R '^modernime_(update_controller|runtime_controller|data_controller|settings_model)$' \
  2>&1 | tee build/t12-evidence/g1/g1.log
```
结果：**4/4 Passed**（0 失败，耗时 3.63s）。
- `modernime_settings_model`: Passed (0.00s)
- `modernime_update_controller`: Passed (1.36s)
- `modernime_runtime_controller`: Passed (2.24s)
- `modernime_data_controller`: Passed (0.02s)

证据文件：`build/t12-evidence/g1/g1.log`、`build/t12-evidence/g1/g1-junit.xml`。

### 4.2 G3 Debug 全量 CTest
命令：
```bash
ctest --test-dir build/architecture-debug --output-on-failure --no-tests=error \
  -E '^modernime_personal_engine_benchmark$' \
  --output-junit $(pwd)/build/t12-evidence/g3/debug-junit.xml \
  2>&1 | tee build/t12-evidence/g3/debug-ctest.log
```
结果：**67/67 Passed**（0 失败，耗时 340.43s）。
覆盖全部 GTest、LMDB/SQLite 存储测试、Fcitx5 适配层、渲染与布局、包安装与隔离测试等。

证据文件：`build/t12-evidence/g3/debug-ctest.log`、`build/t12-evidence/g3/debug-junit.xml`。

### 4.3 G3 Release 全量 CTest（补齐本轮优化构建完整证据）
命令：
```bash
ctest --test-dir build/architecture-release --output-on-failure \
  --output-junit $(pwd)/build/t12-evidence/g3/release-ctest-junit.xml \
  2>&1 | tee build/t12-evidence/g3/release-ctest.log
```
结果：**68/68 Passed**（0 失败，耗时 281.84s）。
- 注册测试数：68
- 执行测试数：68
- 通过测试数：68
- 失败测试数：0
- 跳过测试数：0

证据文件：`build/t12-evidence/g3/release-ctest.log`、`build/t12-evidence/g3/release-ctest-junit.xml`。

### 4.4 G3 Release 性能门禁与负向拒绝自检
1. **负向拒绝自检（`--self_test_gate_rejection`）**：
   ```bash
   build/architecture-release/tests/modernime_personal_engine_benchmark --self_test_gate_rejection \
     2>&1 | tee build/t12-evidence/g3/release-negative-gate.log
   ```
   - 8 项延迟拒绝分支全部独立验证（超限即退出码 1）；自检程序整体退出码 0。
   - 证据文件：`build/t12-evidence/g3/release-negative-gate.log`。
2. **Release Benchmark 串行真实负载运行**：
   - LMDB 200k Writer P99: **3066.03 us**（门限 <5000 us）
   - LMDB 200k Writer max: **4445.52 us**（门限 <30000 us）
   - LMDB 并发剪枝 P99: **1389.73 us**（门限 <5000 us）
   - LMDB 并发剪枝 max: **3091.19 us**（门限 <30000 us）
   - 并发 SQLite max: **19792.18 us**（门限 <30000 us）
   - LMDB 首 Observer 延迟: **1331.51 us**（门限 <5000 us）
   - 清空后首键延迟: **3108.39 us**（门限 <5000 us）
   - 进程整体退出码 0。
   - 证据文件：`build/t12-evidence/g3/release-performance.log`、`build/t12-evidence/g3/performance-junit.xml`。

### 4.5 G6 GTK 接口与页面生命周期验证
命令：
```bash
ctest --test-dir build/architecture-debug --output-on-failure --no-tests=error \
  --output-junit $(pwd)/build/t12-evidence/g6/desktop-junit.xml \
  -R '^modernime_(settings_focus_gtk|settings_shell_gtk|settings_save_gtk)$' \
  2>&1 | tee build/t12-evidence/g6/desktop-gtk.log
```
结果：**3/3 Passed**（0 失败，耗时 6.53s）。
- `modernime_settings_focus_gtk`: Passed (0.32s)
- `modernime_settings_shell_gtk`: Passed (2.86s，包含新 real GTK UpdatesPage/SettingsShell 生命周期验证)
- `modernime_settings_save_gtk`: Passed (3.35s)

证据文件：`build/t12-evidence/g6/desktop-gtk.log`、`build/t12-evidence/g6/desktop-junit.xml`。

### 4.6 测试注册与完整性校验（tests/verify_required_tests.py）
命令：
```bash
python3 tests/verify_required_tests.py \
  --registered build/t12-evidence/g3/debug-registered.json \
  --profile full --stage baseline \
  --junit build/t12-evidence/g3/debug-junit.xml \
  --junit build/t12-evidence/g3/release-ctest-junit.xml \
  --junit build/t12-evidence/g3/performance-junit.xml \
  --execution headless
```
结果：`[OK] Required tests check passed (profile=full, stage=baseline, execution=headless).`  
同时验证 desktop profile：`[OK] Required tests check passed (profile=full, stage=baseline, execution=desktop).`

### 4.7 全新克隆与全生命周期安装卸载验证（tests/verify_fresh_clone.sh）
命令：
```bash
CMAKE_BUILD_PARALLEL_LEVEL=2 ./tests/verify_fresh_clone.sh
```
结果：
- 步骤 1：克隆代码仓库与工作树 patch 到沙箱隔离目录；
- 步骤 2：在沙箱环境中执行 `./install.sh` 构建与安装；
- 步骤 3：验证沙箱安装产物完整性（可执行文件、Fcitx5 插件 .so、桌面启动项、7 种尺寸图标、环境配置）；
- 步骤 4：在沙箱环境中执行 `./uninstall.sh`；
- 步骤 5：验证沙箱卸载清理完全性（产物及配置文件干净移除，无残留）；
- 步骤 6：沙箱全新克隆与全生命周期安装卸载验证 100% 通过（退出码 0）。

---

## 5. 变更文件与范围审查

本次整改严格限制在 T12 所需范围内：
- `tools/settings/update_controller.cpp`：
  - 修正第 1079 行：`finishPackageOperation(*opId, res, {}, {}, false);`，消除同步锁拒绝错误入队未消费结果（R1 / P1）；
- `tests/settings/update_controller_test.cpp`：
  - 新增测试 14：`SynchronousLockRejectionDoesNotPolluteUnconsumedQueue`，覆盖 9+ 次连续锁拒绝与单次消费/恢复语义（R1 回归）；
  - 修正测试 15：`StatusTransitionsUnitContract`，移除无连接的 `MockPageLifetime`，明确其为纯状态转移单元测试；
  - 新增测试 16：`NormalWaitSignalTerminationTransitionsToUncertain`，覆盖正常等待期信号中断进入 Uncertain（R2 回归）；
  - 新增测试 17：`TimeoutBackgroundSignalTerminationTransitionsToUncertain`，覆盖超时转后台后信号中断进入 Uncertain（R2 回归）；
- `tests/settings/settings_shell_gtk_test.cpp`：
  - 新增真实 GTK `UpdatesPage` 与 `SettingsShell` 在包操作运行期间的创建、销毁与重建测试，验证 ID 保留、占槽、并发拒绝与单次 spawn（R2 补齐）；
- `docs/T12_ACCEPTANCE.md`：
  - 更新本验收报告，记录 R1 缺陷定位与修复、R2 真实页面生命周期及 Uncertain 验收证据、完整 Release CTest 证据及全新克隆验证结果。

**未修改**：
- 未修改任何已验收的 T11 候选排序、世代、容量契约、通知或构建配置；
- 未实施 T13 的独立内部采集子进程或流式预算管理；
- 未修改或放宽任何性能门限参数。

---

## 6. 状态结论与复审等待

T12 完整包操作所有权及复审提出的 R1/R2 全部整改项已完全闭环并通过所有测试验证。  
按照要求，本轮到此停下，不推送、不发布、不进入 T13，等待用户复审。
