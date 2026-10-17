# ModernIME 并行整改协调状态表 (Parallel Remediation Status)

**更新时间:** 2026-10-17 20:05:00 CST  
**协调分支:** `codex/architecture-integration-20261017`  
**公共起点 B0:** `7f2f05db34073b526a623d31039b4af06c9198e8` (非验收证明)  
**当前集成 Tip:** `94f70bb` (已合入 T14 `a5dbf74`、T17 `612c32c`、T15 `2a0460a` 与 T16 `041e731`)  
**全局机器锁:** `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock`  
**控制目录:** `/home/wsl/ModernIME/build/parallel-control-20261017/` (仅协调者写)

---

## 1. 任务状态汇总 (Task Registry)

| 任务 | 分支 | 实际工作区路径 | Base SHA | Tip SHA | 接口版本 | 状态 | 接受集成 SHA | 关卡门禁 |
|---|---|---|---|---|---|---|---|---|
| **T14** | `main` | `/home/wsl/ModernIME` | `7f2f05db` | `74c19d86` | `C14-v1` (暂行) | `NEEDS_FIX` (V1/V2) | 中间提交 `d905aa8` (保留非 ACCEPTED) | 第三轮复审：委托构造/同源读取/单次错误/四项探针 PASS；剩 V1（连续失败5次后同 stamp 停止恢复）与 V2（假 LMDB/UI addon 集成/缺夹具假通过） |
| **T15** | `codex/architecture-t15-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t15-20261017` | `7f2f05db` | `2a0460a4` (增量待提) | `C15-v1` (暂行) | `NEEDS_FIX` (R1-R3 验证中) | 中间提交 `d65ef8a` (保留非 ACCEPTED) | R1–R3 增量代码就绪，非 GTK 测试通过；宿主已恢复 libgtk-3-dev，正运行 GTK 测试与 ASAN |
| **T17** | `codex/architecture-t17-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t17-20261017` | `7f2f05db` | `612c32c4` | `C17-v1` (已冻结) | `REVIEWED` | 已合入 `d65ef8a` | 本地 7/7 验证全过，待 T16+T17 组合快照门禁 |
| **T16** | `codex/architecture-t16-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t16-20261017` | `d65ef8a` | `041e731b` | `C16-BUILD-TEST-WIRING` | `REVIEWED` | 已合入 `94f70bb` | 核心 25/25 校验全过，CMake 写权限移交回协调者 |
| **T18** | `codex/architecture-t18-20261017` | 待建 | - | - | C18 (待定) | `WAITING` | - | 等待 T14/T16/T17 集成与宿主依赖恢复 |
| **T19** | `codex/architecture-t19-20261017` | 待建 | - | - | C19 (待定) | `WAITING` | - | 等待最终快照 |

---

## 2. 共享文件写入负责人表 (File Ownership)

| 文件 / 范围 | 当前唯一写入负责人 | 移交前置条件与规则 |
|---|---|---|
| 全仓 CMake, CI, 测试注册 (`CMakeLists.txt`, `tests/CMakeLists.txt` 等) | **协调者** (自 T16 移交) | T16 交付完成，写权限已正式移交回协调者，由协调者独占编辑处理 wiring request |
| `learning_page.cpp`, `dictionary_page.cpp`, `settings_data_async_gtk_test.cpp` | **原 T15 对话 (独占收尾)** | 负责 R1（学习页代次/防重入）、R2（词典异步化与保存）、R3（真实慢 I/O 与连续搜索），协调者不代写 |
| `adapter/fcitx5/src/inputmethod.cpp` (V1 修复) | **原 T14 对话** | 解决重试耗尽后同 stamp 持续故障恢复契约，协调者不代写 |
| `tests/configuration_contract_test.sh` (V2 修复) | **原 T14 对话** | 解决 A06 真实 LMDB 校验、A07 缺失 fake 工具退出非零判定，协调者不代写 |
| UI addon / candidate layout 集成测试 (V2 修复) | **原 T14 对话** | 补齐真实 UI 流程配置刷新与布局选择断言，协调者不代写 |
| `docs/T14_ACCEPTANCE.md` | **原 T14 对话** | 订正实际保存参数签名与执行证据 |
| `install.sh`, `scripts/install_state.sh`, `scripts/modernime-session-init`, `uninstall.sh`, binary payload/generator, `package_tarball`, `scripts/install_lifecycle.sh` | **T17 (独占写权限)** | 生命周期重构完成，已合并入集成分支，T14 禁止再改动 |
| `tools/settings/` 中的 `settings_shell.cpp/.h` | **协调者** | 维护共享接线，T18 启动时移交 T18 |
| `page_registry`, app/window 统一路由 | **T18** | T14/T16/T17 集成验收前只读盘点，移交后写 |
| `README.md` 与跨任务架构文档 | **协调者** (后交 T18) | 协调者收集阶段说明，T18 最终统一核对 |

---

## 3. 门禁验证进度

- **T16 + T17 组合门禁:** 待宿主开发库就绪后，集中验证全量构建、安装、升级、卸载及性能矩阵。
- **T14 + T15 组合门禁:** 待 T14（V1/V2）与 T15（R1-R3）定向复审通过后，统一执行 G1/G3/G5/G6 门禁，记录真实集成 SHA。不要从已合入或 CTest 绿色推定该阶段验收通过。
- **T18 门禁:** 待 T14/T16/T17 集成验收后执行。
- **T19 最终门禁 G0-G7:** 待最终 HEAD 冻结后完整执行。
