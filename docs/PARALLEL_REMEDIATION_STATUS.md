# ModernIME 并行整改协调状态表 (Parallel Remediation Status)

**更新时间:** 2026-10-17 21:22:00 CST  
**协调分支:** `codex/architecture-integration-20261017`  
**公共起点 B0:** `7f2f05db34073b526a623d31039b4af06c9198e8` (非验收证明)  
**当前集成 Tip:** `5d52e5c` (已合入 T14 `a5dbf74`、T17 `612c32c`、T16 `041e731` 与 T15 完整链 `e211296`)  
**全局机器锁:** `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock`  
**控制目录:** `/home/wsl/ModernIME/build/parallel-control-20261017/` (仅协调者写)

---

## 1. 任务状态汇总 (Task Registry)

| 任务 | 分支 | 实际工作区路径 | Base SHA | Tip SHA | 接口版本 | 状态 | 接受集成 SHA | 关卡门禁 |
|---|---|---|---|---|---|---|---|---|
| **T14** | `main` | `/home/wsl/ModernIME` | `74c19d86` | `4c40cc6d` | `C14-v1` (暂行) | `NEEDS_FIX` (W1/W2/W3) | 中间提交 `d905aa8` (保留非 ACCEPTED) | 第四轮复审：生产修复通过 5 项真实适配器交错（含 6 次 EACCES 恢复收敛）；真实双库与 UI 测试取得有效进展；剩 W1（无头 UI 测试合法 skip 77）、W2（A06 真实环境选址解析）与 W3（故障恢复回归固化入仓）待原 T14 对话收尾 |
| **T15** | `codex/architecture-t15-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t15-20261017` | `7f2f05db` | `e2112967` | `C15-v1` (暂行) | `REVIEWED` | 已合入 `5d52e5c` (非 ACCEPTED) | 最终定向复审通过：S1/S2a/S2b 均闭合，普通 10/10 与 ASAN 10/10 均通过，完整链（2a0460a → 7d49e51 → a7e37d7 → 57e0183 → e211296）已合入，待组合门禁 |
| **T17** | `codex/architecture-t17-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t17-20261017` | `7f2f05db` | `612c32c4` | `C17-v1` (已冻结) | `REVIEWED` | 已合入 `d65ef8a` | 本地 7/7 验证全过，待 T16+T17 组合快照门禁 |
| **T16** | `codex/architecture-t16-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t16-20261017` | `d65ef8a` | `041e731b` | `C16-BUILD-TEST-WIRING` | `REVIEWED` | 已合入 `94f70bb` | 核心 25/25 校验全过，CMake 写权限已移交回协调者 |
| **T18** | `codex/architecture-t18-20261017` | 待建 | - | - | C18 (待定) | `WAITING` | - | 等待 T14/T16/T17 集成与宿主依赖恢复 |
| **T19** | `codex/architecture-t19-20261017` | 待建 | - | - | C19 (待定) | `WAITING` | - | 等待最终快照 |

---

## 2. 共享文件写入负责人表 (File Ownership)

| 文件 / 范围 | 当前唯一写入负责人 | 移交前置条件与规则 |
|---|---|---|
| 全仓 CMake, CI, 测试注册 (`CMakeLists.txt`, `tests/CMakeLists.txt` 等) | **协调者** (自 T16 移交) | T16 交付完成，写权限已正式移交回协调者，由协调者独占编辑处理 wiring request（注意：`modernime_ui` 为 MODULE，测试须编译生产 `ui_addon.cpp` 并链接 `modernime_ui_layout`，设置 `RESOURCE_LOCK modernime_gtk_display`、`TIMEOUT 20`、`SKIP_RETURN_CODE 77`，非 GTK 契约不可随 UI 跳过） |
| `tools/settings/include/modernime/settings/data_controller.h`, `tools/settings/data_controller.cpp`, `tests/settings/settings_data_async_gtk_test.cpp`, `docs/T15_ACCEPTANCE.md` | **原 T15 对话 (实施冻结，已 REVIEWED)** | S1（钩子互斥与 UAF 关闭）、S2a（旧统计反证）、S2b（搜索过滤前断言及即时信号）均已复验关闭，页面与存储实现冻结，不再修改 |
| `tests/ui/ui_addon_test.cpp` (W1 修复) | **原 T14 对话** | 修复无显示环境合法 skip 77 语义，协调者不代写 |
| `tests/configuration_contract_test.sh` (W1/W2 修复) | **原 T14 对话** | 识别合法 UI skip，A06 消费真实环境并通过 SettingsPaths 自动解析路径，协调者不代写 |
| 故障恢复回归测试 (W3 修复) | **原 T14 对话** | 将连续失败恢复及交错探针固化为受版本控制测试，协调者不代写 |
| `docs/T14_ACCEPTANCE.md` | **原 T14 对话** | 订正实际保存参数签名与执行证据，区分 Passed/Skipped/NotRun/Pending |
| `install.sh`, `scripts/install_state.sh`, `scripts/modernime-session-init`, `uninstall.sh`, binary payload/generator, `package_tarball`, `scripts/install_lifecycle.sh` | **T17 (独占写权限)** | 生命周期重构完成，已合并入集成分支，T14 禁止再改动 |
| `tools/settings/` 中的 `settings_shell.cpp/.h` | **协调者** | 维护共享接线，T18 启动时移交 T18 |
| `page_registry`, app/window 统一路由 | **T18** | T14/T16/T17 集成验收前只读盘点，移交后写 |
| `README.md` 与跨任务架构文档 | **协调者** (后交 T18) | 协调者收集阶段说明，T18 最终统一核对 |

---

## 3. 门禁验证进度

- **T16 + T17 组合门禁:** 待宿主开发库就绪后，集中验证全量构建、安装、升级、卸载及性能矩阵。
- **T14 + T15 组合门禁:** T15 定向复审通过（`REVIEWED`）；待 T14（W1-W3）定向复审通过后，统一在明确组合 SHA 执行 G1/G3/G5/G6 门禁，记录真实集成 SHA。不要从已合入或 CTest 绿色推定该阶段验收通过。
- **T18 门禁:** 待 T14/T16/T17 集成验收后执行。
- **T19 最终门禁 G0-G7:** 待最终 HEAD 冻结后完整执行。
