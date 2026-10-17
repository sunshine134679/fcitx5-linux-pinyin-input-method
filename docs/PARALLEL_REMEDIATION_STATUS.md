# ModernIME 并行整改协调状态表 (Parallel Remediation Status)

**更新时间:** 2026-10-17 19:14:30 CST  
**协调分支:** `codex/architecture-integration-20261017`  
**公共起点 B0:** `7f2f05db34073b526a623d31039b4af06c9198e8` (非验收证明)  
**当前集成 Tip:** `d65ef8a` (已合入 T14 `a5dbf74`、T17 `612c32c` 与 T15 `2a0460a`)  
**全局机器锁:** `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock`  
**控制目录:** `/home/wsl/ModernIME/build/parallel-control-20261017/` (仅协调者写)

---

## 1. 任务状态汇总 (Task Registry)

| 任务 | 分支 | 实际工作区路径 | Base SHA | Tip SHA | 接口版本 | 状态 | 接受集成 SHA | 关卡门禁 |
|---|---|---|---|---|---|---|---|---|
| **T14** | `main` | `/home/wsl/ModernIME` | `7f2f05db` | `a5dbf742` | `C14-v1` (暂行) | `NEEDS_FIX` (S1/S2) | 中间提交 `d905aa8` (非 ACCEPTED) | 第二轮复审：热载/路径 PASS，首次启动 S1 FAIL，待定向收尾 |
| **T15** | `codex/architecture-t15-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t15-20261017` | `7f2f05db` | `2a0460a4` | `C15-v1` (已冻结) | `REVIEWED` | 已合入 `d65ef8a` | 本地验证 100%，待组合快照全量门禁 |
| **T17** | `codex/architecture-t17-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t17-20261017` | `7f2f05db` | `612c32c4` | `C17-v1` (已冻结) | `REVIEWED` | 已合入 `d65ef8a` | 本地 7/7 验证全过，待组合快照门禁 |
| **T16** | `codex/architecture-t16-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t16-20261017` | `d65ef8a` | In-flight | C16 (实施中) | `IN_PROGRESS` | - | 独占接管 CMake 写权限，正在实施 |
| **T18** | `codex/architecture-t18-20261017` | 待建 | - | - | C18 (待定) | `WAITING` | - | 等待 T14/T16/T17 集成 |
| **T19** | `codex/architecture-t19-20261017` | 待建 | - | - | C19 (待定) | `WAITING` | - | 等待最终快照 |

---

## 2. 共享文件写入负责人表 (File Ownership)

| 文件 / 范围 | 当前唯一写入负责人 | 移交前置条件与规则 |
|---|---|---|
| 全仓 CMake, CI, 测试注册 (`CMakeLists.txt`, `tests/CMakeLists.txt` 等) | **T16 (独占写权限)** | C15 冻结后已一次性移交给 T16，协调者与其他任务停止并发编辑 |
| `adapter/fcitx5/src/inputmethod.cpp` (S1 修复) | **原 T14 对话** | 解决首次启动事实与已应用版本分离，协调者不代写 |
| `tests/adapter/engine_state_test.cpp` (S2 修复) | **原 T14 对话** | 恢复被删除的三条正向学习断言 |
| `tests/configuration_contract_test.sh` (S2 修复) | **原 T14 对话** | 补齐真实消费者验证（非仅 Python 字典解析） |
| `docs/T14_ACCEPTANCE.md` | **原 T14 对话** | 订正实际保存参数签名与执行证据 |
| `install.sh`, `scripts/install_state.sh`, `scripts/modernime-session-init`, `uninstall.sh`, binary payload/generator, `package_tarball`, `scripts/install_lifecycle.sh` | **T17 (独占写权限)** | 生命周期重构完成，已合并入集成分支，T14 禁止再改动 |
| `learning_store`, `personal_engine` 轻量计数 | **T15** | 已完成 `entryCount()` 并合入集成分支 |
| `DataController`, `Dictionary/Learning/Overview/Clipboard` 页面 | **T15** | 已完成异步与分批并合入集成分支 |
| `tools/settings/` 中的 `settings_shell.cpp/.h` | **协调者** | 维护共享接线，T18 启动时移交 T18 |
| `page_registry`, app/window 统一路由 | **T18** | T14/T16/T17 集成验收前只读盘点，移交后写 |
| `README.md` 与跨任务架构文档 | **协调者** (后交 T18) | 协调者收集阶段说明，T18 最终统一核对 |

---

## 3. 门禁验证进度

- **T14 + T15 组合门禁:** 待 T14 S1/S2 增量修复复审通过后，统一执行 G1/G3/G5/G6 门禁，记录真实集成 SHA。
- **T16 + T17 组合门禁:** 待 T16 接管完成 CMake 目标统一与测试接线后，集中验证构建、安装、卸载及性能矩阵。
- **T18 门禁:** 待 T14/T16/T17 集成验收后执行。
- **T19 最终门禁 G0-G7:** 待最终 HEAD 冻结后完整执行。
