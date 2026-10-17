# ModernIME 并行整改协调状态表 (Parallel Remediation Status)

**更新时间:** 2026-10-17 18:37:30 CST  
**协调分支:** `codex/architecture-integration-20261017`  
**公共起点 B0:** `7f2f05db34073b526a623d31039b4af06c9198e8` (非验收证明)  
**全局机器锁:** `/home/wsl/ModernIME/build/parallel-control-20261017/machine.lock`  
**控制目录:** `/home/wsl/ModernIME/build/parallel-control-20261017/` (仅协调者写)

---

## 1. 任务状态汇总 (Task Registry)

| 任务 | 分支 | 实际工作区路径 | Base SHA | Tip SHA | 接口版本 | 状态 | 接受集成 SHA | 关卡门禁 |
|---|---|---|---|---|---|---|---|---|
| **T14** | `main` | `/home/wsl/ModernIME` | `7f2f05db` | In-flight (未提交) | C14 (待定) | `IN_PROGRESS` (R1-R4) | - | 待独立复审 |
| **T15** | `codex/architecture-t15-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t15-20261017` | `7f2f05db` | `7f2f05db` | C15 (待冻结) | `PREPARING` | - | 待实施 |
| **T17** | `codex/architecture-t17-20261017` | `/home/wsl/ModernIME/.worktrees/parallel-t17-20261017` | `7f2f05db` | `7f2f05db` | C17 (设计中) | `PREPARING` | - | 待准备完成 |
| **T16** | `codex/architecture-t16-20261017` | 未建 (待 C15 冻结) | `7f2f05db` | - | C16 (待定) | `WAITING` | - | 等待 C15 冻结 |
| **T18** | `codex/architecture-t18-20261017` | 未建 (待 T14/T16/T17) | - | - | C18 (待定) | `WAITING` | - | 等待 T14/T16/T17 |
| **T19** | `codex/architecture-t19-20261017` | 未建 (待最终快照) | - | - | C19 (待定) | `WAITING` | - | 等待所有交付 |

---

## 2. 共享文件写入负责人表 (File Ownership)

| 文件 / 范围 | 当前唯一写入负责人 | 移交前置条件与规则 |
|---|---|---|
| `core/include/modernime/core/settings.h`, `core/src/settings.cpp` | **T14** | T15 不改配置读取 API，T14 独立完成 |
| `adapter/fcitx5/` 配置状态与 `ui/fcitx5/` addon/布局 | **T14** | T15 不重做候选链路 |
| `tools/settings/include/modernime/settings/runtime_environment.h` | **T14** | T15 消费既有接口 |
| `install.sh`, `tests/configuration_contract_test.sh` (在途修改) | **T14** | T14 完成路径契约修复并移交后转协调者/T17 |
| `scripts/modernime-session-init` (在途修改) | **T14** | T14 冻结路径协议后明确移交 T17 |
| `learning_store`, `personal_engine` 轻量计数 | **T15** | 只加 `entryCount()`，不改 T11 生命周期/学习政策 |
| `DataController`, `Dictionary/Learning/Overview/Clipboard` 页面 | **T15** | 异步 GTask/失活令牌与纯函数过滤 |
| `tools/settings/` 中的 `settings_shell.cpp/.h` | **协调者** | T15 提交精确接线申请，协调者维护 glue；T18 启动时移交 T18 |
| 全仓 CMake, CI, 测试注册 | **协调者** (待移交 T16) | 工作任务提交精确 target/source/test 清单；T16 实施时一次性移交 T16，协调者停止并发编辑 |
| `uninstall.sh`, binary payload/generator, `package_tarball`, `install_lifecycle.sh` | **T17** | 准备期不改，T14 移交后由 T17 实现 |
| `page_registry`, app/window 统一路由 | **T18** | T14/T16/T17 集成验收前只读盘点，移交后写 |
| `README.md` 与跨任务架构文档 | **协调者** (后交 T18) | 协调者收集阶段说明，T18 最终统一核对 |

---

## 3. 门禁验证进度

- **T14 + T15 组合门禁:** 待 T14 提交并复审、T15 本地验证并通过复审后合并验证。
- **T16 + T17 组合门禁:** 待 T16 接管 CMake、T17 完成安装生命周期后集中验证。
- **T18 门禁:** 待 T14/T16/T17 集成验收后执行。
- **T19 最终门禁 G0-G7:** 待最终 HEAD 冻结后完整执行。
