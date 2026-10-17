# ModernIME 并行整改协调状态表 (Parallel Remediation Status)

**更新时间:** 2026-10-17 19:04:00 CST  
**协调分支:** `codex/architecture-integration-20261017`  
**公共起点 B0:** `7f2f05db34073b526a623d31039b4af06c9198e8` (非验收证明)  
**当前集成 Tip:** `d905aa8` (保留中间提交 `a5dbf742`，不 revert)  
**全局机器锁:** `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock`  
**控制目录:** `/home/wsl/ModernIME/build/parallel-control-20261017/` (仅协调者写)

---

## 1. 任务状态汇总 (Task Registry)

| 任务 | 分支 | 实际工作区路径 | Base SHA | Tip SHA | 接口版本 | 状态 | 接受集成 SHA | 关卡门禁 |
|---|---|---|---|---|---|---|---|---|
| **T14** | `main` | `/home/wsl/ModernIME` | `7f2f05db` | `a5dbf742` | `C14-v1` (暂行) | `NEEDS_FIX` (S1/S2) | 中间提交 `d905aa8` (非 ACCEPTED) | 第二轮复审：热载/路径 PASS，首次启动 S1 FAIL，待定向收尾 |
| **T15** | `codex/architecture-t15-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t15-20261017` | `7f2f05db` | In-flight | C15 (进行中) | `IN_PROGRESS` | - | 独立推进，消费当前 C14 接口 |
| **T17** | `codex/architecture-t17-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t17-20261017` | `7f2f05db` | 同步 `a5dbf742` 实施中 | `C17` (实施中) | `IN_PROGRESS` | - | 路径错配已关闭，全权接管生命周期脚本 |
| **T16** | `codex/architecture-t16-20261017` | 待建 | `7f2f05db` | - | C16 (待定) | `WAITING` | - | 等待 C15 接口与清单冻结 |
| **T18** | `codex/architecture-t18-20261017` | 待建 | - | - | C18 (待定) | `WAITING` | - | 等待 T14/T16/T17 集成 |
| **T19** | `codex/architecture-t19-20261017` | 待建 | - | - | C19 (待定) | `WAITING` | - | 等待最终快照 |

---

## 2. 共享文件写入负责人表 (File Ownership)

| 文件 / 范围 | 当前唯一写入负责人 | 移交前置条件与规则 |
|---|---|---|
| `adapter/fcitx5/src/inputmethod.cpp` (S1 修复) | **原 T14 对话** | 解决首次启动事实与已应用版本分离，协调者不代写 |
| `tests/adapter/engine_state_test.cpp` (S2 修复) | **原 T14 对话** | 恢复被删除的三条正向学习断言 |
| `tests/configuration_contract_test.sh` (S2 修复) | **原 T14 对话** | 补齐真实消费者验证（非仅 Python 字典解析） |
| `docs/T14_ACCEPTANCE.md` | **原 T14 对话** | 订正实际保存参数签名与执行证据 |
| `install.sh`, `scripts/install_state.sh`, `scripts/modernime-session-init`, `uninstall.sh`, binary payload/generator, `package_tarball`, `scripts/install_lifecycle.sh` | **T17 (独占写权限)** | 原 session-init 路径错配已独立复验关闭，T17 保持唯一写入权，T14 禁止再改动 |
| `learning_store`, `personal_engine` 轻量计数 | **T15** | 只加 `entryCount()`，不改 T11 生命周期/学习政策 |
| `DataController`, `Dictionary/Learning/Overview/Clipboard` 页面 | **T15** | 异步 GTask/失活令牌与纯函数过滤 |
| `tools/settings/` 中的 `settings_shell.cpp/.h` | **协调者** | T15 提交精确接线申请，协调者维护 glue；T18 启动时移交 T18 |
| 全仓 CMake, CI, 测试注册 | **协调者** (待移交 T16) | 工作任务提交精确 target/source/test 清单；T16 实施时一次性移交 T16，协调者停止并发编辑 |
| `page_registry`, app/window 统一路由 | **T18** | T14/T16/T17 集成验收前只读盘点，移交后写 |
| `README.md` 与跨任务架构文档 | **协调者** (后交 T18) | 协调者收集阶段说明，T18 最终统一核对 |

---

## 3. T14 第二轮复审判断记录 (T14 Second Review Verdicts)

- **复审基线:** `a5dbf742fd204e93f1ff68dfb644d8167411376f`
- **已有效项:**
  - `swap_after_read`: **PASS** (`SettingsStore::load` 单缓冲解析并提取 Hash，并发替换被检测并重试)
  - `read_failure`: **PASS** (热载分支检查状态，保留有效配置，generation 不推进)
  - 路径错配关闭: `session-init` 运行时数据根与 `data-guard` 保护根一致，空格路径解析正确
  - `CandidateBarLayout` 字号/数量/代次不匹配拒绝点击有效，`--build-dir` 绑定生效
- **未通过项 (NEEDS_FIX):**
  - `startup_swap`: **FAIL** (S1: 首次启动 `loadSettings()` 丢弃版本元数据，构造函数另行 stat/hash 导致旧设置被标新 Hash)
  - `startup_read_failure`: **FAIL** (S1: 首次启动 I/O 错误未建立受控重试状态，默认启用被误标为文件 Hash)
  - S2 验收覆盖缺口: 待补齐真实消费者（A01/A02 观察代次收敛、A04 敏感热载入口、A05 UI addon 点击边界、A06 真实活动库选址、A07 `RuntimeController::probe` 实测未知字段），恢复删除的三条正向学习旧断言。
- **集成处理政策:** 保留已集成的中间提交 `d905aa8`，不 reset/revert；后续仅集成 T14 增量修复与 wiring requests；C14 现有接口保持提供给 T15/T17/T16 开发使用。

---

## 4. 门禁验证进度

- **T14 + T15 组合门禁:** 待 T14 S1/S2 增量修复复审通过、T15 交付合入集成树后，统一执行 G1/G3/G5/G6 门禁，记录真实集成 SHA。
- **T16 + T17 组合门禁:** 待 T16 接管 CMake、T17 完成安装生命周期后集中验证构建、安装、卸载及性能矩阵。
- **T18 门禁:** 待 T14/T16/T17 集成验收后执行。
- **T19 最终门禁 G0-G7:** 待最终 HEAD 冻结后完整执行。
