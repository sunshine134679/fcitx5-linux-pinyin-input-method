# ModernIME T17 显式安装阶段准备与接口规范报告 (PREPARING)

- **任务**：T17「显式安装阶段（P2-04）」
- **工作树路径**：`/home/wsl/ModernIME/.worktrees/parallel-t17-20261017`
- **对应 Git 分支**：`codex/architecture-t17-20261017`
- **基线提交 B0**：`7f2f05db34073b526a623d31039b4af06c9198e8`
- **准备阶段快照 SHA**：`2552eb55dde4003641c642e52fbfa4e350ef4bf4`
- **当前阶段状态**：`PREPARING`
- **日期**：2026-10-17

---

## 1. 阶段概述与准备准则

在收到协调者正式确认的 T14 路径契约验收与文件移交前，T17 严格遵守并行开发准则：
- **不修改共享安装/路径实现**（不改动 `install.sh`、`scripts/install_state.sh`、`scripts/install_binary_payload.sh`、`scripts/generate_binary_installer.sh`、`uninstall.sh`、`scripts/modernime-session-init`、`scripts/modernime-data-guard` 等共享文件）；
- **不修改用户 main 分支或其他任务工作树代码**；
- **保留用户所有改动与资料**，不 reset/clean/stash，不推送，不发布；
- **在自己的新建文件中独立完成**：三方所有权矩阵冻结、显式薄入口契约设计、失败/回滚夹具准备（包含 `RollbackPreservesConcurrentUserWrites` 与基线反例验证）。

---

## 2. 源码 / 二进制 / 原生包所有权矩阵 (Ownership Matrix Frozen)

| 维度 / 对象 | 源码安装 (`install.sh`) | 二进制安装包 (`install_binary_payload.sh`) | 原生包 (`.deb` / `.rpm`) |
|---|---|---|---|
| **构建与校验责任** | 负责本地 `cmake` 配置、编译、测试、DESTDIR 临时暂存 | 负责校验载荷架构 (`x86_64`)、依赖完整性与 SHA256 | 由系统包管理器打包前构建与校验 |
| **部署入口** | 调用共同 `modernime_deploy_payload` | 调用共同 `modernime_deploy_payload` | `dpkg` / `rpm` 解包写入系统目录 |
| **集成入口** | 调用共同 `modernime_integrate` | 调用共同 `modernime_integrate` | `modernime-session-init` / `modernime-data-guard` |
| **安装渠道标识** | `$prefix/share/modernime/install-channel.txt` (`channel=script`) | 同左 (`channel=script`) | 系统包数据库（APT/RPM 数据库），无 script 渠道文件 |
| **安装清单** | `$prefix/share/modernime/install-manifest.txt` | 同左 | 由 `dpkg -L` / `rpm -ql` 维护 |
| **Prefix 范围文件** | 拥有清单列出的 bin、lib、share、doc 文件；覆盖安装先 unlink 避免原地写 mmap 中的 `.so` | 规则与源码安装一致 | 归包管理器所有；脚本卸载检测到原生包属主时**禁止清理** |
| **Managed 托管文件** | 仅在非系统 prefix 下管理：快捷方式、`90-modernime.conf`、`autostart`、`xprofile` 块；基准快照存于 `MANAGED_STATE_DIR` | 规则与源码安装一致 | 用户会话初始化按需生成，不由安装脚本强行写入用户目录 |
| **用户修改的 Managed 文件** | 内容与 baseline snapshot 不一致时：**保留用户修改**，卸载或覆盖时不强制还原 | 规则与源码安装一致 | 遵守系统配置更新策略 |
| **用户核心数据** | `$data_home/modernime/` (`learning.sqlite3`, `learning.lmdb`, `user-dictionary.txt`)：**严禁删除、严禁清空、严禁在失败回滚时用旧快照覆盖** | 规则与源码安装一致 | 规则与源码安装一致 |
| **用户个性化配置** | `$config_home/modernime/settings.conf`：**严禁清空、严禁在回滚时覆盖并发写入** | 规则与源码安装一致 | 规则与源码安装一致 |
| **非相关文件** | `$prefix/unrelated/*` 及未记录的已有软链：**绝对不备份、不复制、不删除** | 规则与源码安装一致 | 规则与源码安装一致 |

---

## 3. C17 显式生命周期接口契约发布 (C17 Interface Specification Draft)

### 3.1 `modernime_deploy_payload`
```bash
modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR
```
- **参数规格**：
  - `PAYLOAD` (string, 路径)：Staged 安装载荷绝对路径；
  - `PREFIX` (string, 路径)：安装前缀目录；
  - `TRANSACTION_DIR` (string, 路径)：独立事务临时目录（包含 `backup/`、`changed.txt`、`directories.txt`）。
- **行为规范**：
  1. 仅备份 `PREFIX` 中与 `PAYLOAD` 冲突的目标文件至 `TRANSACTION_DIR/backup/`，严禁备份整个 prefix；
  2. 采用 unlink-before-copy (`cp -a --remove-destination`) 避免写坏正在被 Fcitx5 mmap 映射的动态库；
  3. 中途失败（如 `MODERNIME_SIMULATE_INSTALL_FAILURE=1`）原子回滚：删除本次新增文件，还原备份文件，删除空目录；
  4. **失败回滚严禁触碰任何用户数据目录（`DATA_HOME`、`CONFIG_HOME`）**。
  5. 出现无法自动恢复错误时保留 `TRANSACTION_DIR` 并返回非零，输出恢复证据。

### 3.2 `modernime_integrate`
```bash
modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR
```
- **参数规格**：
  - `PREFIX` (string, 路径)：已安装前缀；
  - `CONFIG_HOME` (string, 路径)：XDG 配置目录；
  - `DESKTOP_DIR` (string, 路径)：桌面快捷方式目录；
  - `MANAGED_STATE_DIR` (string, 路径)：托管文件基准快照目录（`$PREFIX/share/modernime/managed-files`）。
- **行为规范**：
  1. 系统前缀（`/usr`, `/opt`）跳过个人桌面快捷方式与 autostart 配置，交由用户会话初始化；
  2. 生成并管理桌面启动项、`90-modernime.conf`、`modernime-fcitx5-session.desktop` 与 `.xprofile`；
  3. 通过 `modernime_write_managed_file` 比较 baseline content；用户修改过的托管文件严格保留，不强制覆盖；
  4. 不使用 `eval` 传递结构体，全显式参数与非零退出码。

---

## 4. 安装 Source 清单与 T16 接线请求 (Wiring Requests)

### 4.1 安装文件清单 (待 Phase 2 实现的生命周期文件)
- `scripts/install_lifecycle.sh`：共同生命周期入口（提供 `modernime_deploy_payload` 与 `modernime_integrate`）；
- `scripts/install_state.sh`：托管文件 baseline 管理与路径记录；
- `install.sh`：源码构建、测试、staging 并调用共同生命周期；
- `scripts/install_binary_payload.sh`：二进制包载荷架构与完整性校验并调用共同生命周期；
- `scripts/generate_binary_installer.sh`：薄入口生成器；
- `uninstall.sh`：严格对称卸载清理。

### 4.2 CTest 接线请求 (提交给 CMake 负责人 / T16)
在 `tests/CMakeLists.txt` 中注册新的回滚并发数据保护与生命周期准备测试：
```cmake
add_test(NAME modernime_install_rollback_concurrent_writes
    COMMAND ${BASH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/install_rollback_concurrent_writes_test.sh
)
set_tests_properties(modernime_install_rollback_concurrent_writes PROPERTIES
    RESOURCE_LOCK modernime_install_transaction
)

add_test(NAME modernime_install_lifecycle_prep
    COMMAND ${BASH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/install_lifecycle_prep_test.sh
)
set_tests_properties(modernime_install_lifecycle_prep PROPERTIES
    RESOURCE_LOCK modernime_install_transaction
)
```

---

## 5. 实际已执行的测试与证据 (Executed Tests & Evidence)

已在独立工作树编写并执行：
1. [`tests/install_rollback_concurrent_writes_test.sh`](file:///home/wsl/ModernIME/.worktrees/parallel-t17-20261017/tests/install_rollback_concurrent_writes_test.sh)
2. [`tests/install_lifecycle_prep_test.sh`](file:///home/wsl/ModernIME/.worktrees/parallel-t17-20261017/tests/install_lifecycle_prep_test.sh)

| 套件 | 测试项 | 验证行为 | 结果 |
|---|---|---|---|
| `install_rollback_concurrent_writes` | `test_baseline_anti_example` | **基线反例验证**：模拟“幼稚回滚机制”（安装前快照整个用户数据目录，安装失败回滚时覆盖回用户数据目录）。用户在安装期间并发提交 WAL 帧与配置修改，该错误回滚导致并发数据丢失。测试断言成功捕获该数据丢失并判为失败，证明本测试套件对回滚数据丢失具有确定性敏感度与阻断能力。 | **PASS** |
| `install_rollback_concurrent_writes` | `test_rollback_preserves_concurrent_user_writes` | **真实并发写入回滚保护**：在含空格前缀（`/modernime test prefix with spaces`）与含空格用户目录下，启动并发 SQLite WAL 写入者持续提交新词条，同时修改 `settings.conf` 与 `user-dictionary.txt`。载荷部署中途注入失败（`deploy_failed`），执行受控回滚。断言 prefix 二进制与 `.so` 干净还原至 v0.1.7，无用新文件被清除；同时 `learning.sqlite3` 完整保留初始词条与所有并发写入词条（`PRAGMA integrity_check == ok`），`settings.conf` 与 `user-dictionary.txt` 的并发修改 100% 保留。 | **PASS** |
| `install_rollback_concurrent_writes` | `test_lifecycle_library_deploy_rollback_with_concurrent_writes` | **生命周期库直接驱动与并发写入保护**：直接调用新实现的 `scripts/install_lifecycle.sh` 中的 `modernime_deploy_payload` 函数，在含空格 prefix 与含空格用户数据目录下注入 `MODERNIME_SIMULATE_INSTALL_FAILURE=1`。验证函数非零退出、事务状态置为 `ROLLED_BACK`、prefix 文件干净回滚至 v0.1.7、且并发 SQLite WAL 词条与用户配置完整保留（0 丢失）。 | **PASS** |
| `install_lifecycle_prep` | `Test 1: RollbackPreservesConcurrentUserWrites` | 验证多线程高频 WAL 写入及用户配置变更在 `modernime_deploy_payload` 故障回滚时的零丢失与完整恢复。 | **PASS** |
| `install_lifecycle_prep` | `Test 2: Pre-flight Destination Collision Protection` | 验证部署前目的地目录碰撞检测，先于任何目标修改前拒绝退出并标记 `FAILED_COLLISION`。 | **PASS** |
| `install_lifecycle_prep` | `Test 3: Clean Deployment Success & Unlink Protection` | 验证成功部署事务提交为 `COMMITTED`，运行中进程内存映射（mmap）动态库通过先 unlink 保护不被损坏。 | **PASS** |
| `install_lifecycle_prep` | `Test 4: modernime_integrate & User Edit Protection` | 验证快捷方式、autostart、`90-modernime.conf`、manifest 和 channel 文件生成；验证用户修改过的托管文件在重新集成时被自动保护而不被覆盖。 | **PASS** |

- **执行日志 1**：[`build/t17-prep-evidence/rollback-concurrent-writes.log`](file:///home/wsl/ModernIME/.worktrees/parallel-t17-20261017/build/t17-prep-evidence/rollback-concurrent-writes.log)
- **执行日志 2**：[`build/t17-prep-evidence/lifecycle-prep-test.log`](file:///home/wsl/ModernIME/.worktrees/parallel-t17-20261017/build/t17-prep-evidence/lifecycle-prep-test.log)

---

## 6. 待移交后执行项与剩余风险 (Pending & Risks)

### 待执行项 (Pending T14 Handover)
1. 共享安装脚本实施与对接：`scripts/install_lifecycle.sh` 已在新文件中实现并经单元验证；待 T14 移交后重构共享的 `install.sh`、`scripts/install_binary_payload.sh`、`scripts/generate_binary_installer.sh`、`uninstall.sh`；
2. 完整矩阵验证：
   - 默认前缀与含空格前缀；
   - Headless（无 DISPLAY / 无 DBus）；
   - 运行中 in-memory mmap `.so` 的 unlink 替换；
   - 用户修改托管文件保护；
   - 幂等重复安装与重复卸载；
   - 新产物生成、SHA256 记录与 Tarball 验证；
   - 沙箱 `./tests/verify_fresh_clone.sh` 闭环。

### 剩余风险
- T14 路径契约尚未验收与合入集成树，共享 `install.sh` / `scripts/modernime-session-init` 仍处于在途修改状态。在协调者正式通知移交前，严禁修改这些共享文件；
- 真实原生包（DEB/RPM）验证受限于无授权容器，如实标注为待验，不使用 mock 冒充原生包通过。
