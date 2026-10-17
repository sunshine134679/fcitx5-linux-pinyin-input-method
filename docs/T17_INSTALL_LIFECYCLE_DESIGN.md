# ModernIME T17 显式安装生命周期架构设计 (Install Lifecycle Design)

**任务代号**：T17 (P2-04 显式安装阶段)  
**设计基线**：`7f2f05db34073b526a623d31039b4af06c9198e8`  
**文档状态**：PREPARING 阶段设计冻结 (FROZEN)  
**前置约束**：在 T14 路径契约验收并正式移交前，严禁修改 `install.sh`、`install_state.sh`、`scripts/modernime-session-init` 及任何共享路径文件。

---

## 1. 设计目标与背景

当前 ModernIME 的安装与升级链路存在以下耦合与维护风险：
1. **源码安装与二进制安装生命周期逻辑重复**：`install.sh` 既负责编译测试，又负责 payload 拷贝与回滚；而 `generate_binary_installer.sh` 依赖脆弱的 `awk` 文本锚点切片提取 `install.sh` 内容生成二进制安装包，源文件结构稍有变动即可能导致切片失效或打包出半成品。
2. **全局隐式变量与路径穿透**：安装状态维护 (`install_state.sh`) 与会话初始化依赖大量隐式环境变量 (`managed_state_dir`, `prefix`, `config_home` 等)，缺乏参数显式化的独立生命周期函数。
3. **回滚边界必须严密**：安装与升级过程中必须严格遵循“文件级受控备份与恢复”，严禁复制整个安装前缀（如 `~/.local` 或 `/usr`），且回滚过程**绝对不得影响用户数据**（配置 `settings.conf`、SQLite WAL 学习记录、LMDB 词库及用户词典）。
4. **多渠道与原生包所有权边界清晰**：源码安装、二进制预编译包安装与系统原生包 (deb/rpm) 必须具有明确的所有权矩阵。脚本卸载严禁触碰或误删 dpkg/rpm 托管的原生系统文件。

为解决上述问题，T17 引入显式薄入口标准：
- `modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR`
- `modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR`

---

## 2. 软件产物与数据所有权矩阵 (Ownership Matrix)

本矩阵严格冻结 ModernIME 在各安装渠道及运行时的数据所有权与生命周期职责：

| 路径类别 | 具体路径 / 模式 | 归属实体 | 安装生命周期行为 | 回滚 / 卸载保护策略 |
|---|---|---|---|---|
| **核心二进制与插件** | `$PREFIX/bin/modernime-settings`<br>`$PREFIX/bin/modernime-session-init`<br>`$PREFIX/bin/modernime-data-guard`<br>`$PREFIX/bin/modernime-migrate-channel`<br>`$PREFIX/lib*/fcitx5/modernime_fcitx5.so`<br>`$PREFIX/lib*/fcitx5/modernime_ui.so` | **安装器**<br>(源码 `install.sh` 或 二进制包) 或 **系统原生包** (deb/rpm) | 由 `modernime_deploy_payload` 部署至 `$PREFIX`。<br>部署前必须执行 `cp -a --remove-destination`，**先 unlink 目标文件**，确保运行中被 Fcitx5 mmap 映射的旧 `.so` 不被原地覆盖损坏。 | **回滚**：仅恢复本次升级被替换的文件，清理本次新增的文件。<br>**卸载**：若在系统前缀下，先调用 `dpkg -S` 或 `rpm -qf` 探测；若属系统包管理，则**拒绝删除并保留**。 |
| **只读数据与字典** | `$PREFIX/share/modernime/pinyin/*.dict`<br>`$PREFIX/share/modernime/pinyin/*.bin`<br>`$PREFIX/share/fcitx5/addon/*.conf`<br>`$PREFIX/share/fcitx5/inputmethod/*.conf`<br>`$PREFIX/share/icons/...`<br>`$PREFIX/share/doc/modernime/*` | **安装器** 或 **系统原生包** | 由 `modernime_deploy_payload` 部署。<br>随版本发布更新。 | **回滚**：按 `changed.list` 恢复或清理。<br>**卸载**：按清单逐项清理；系统原生包拥有的文件自动跳过。 |
| **渠道与安装元数据** | `$PREFIX/share/modernime/install-channel.txt`<br>`$PREFIX/share/modernime/install-manifest.txt` | **脚本安装器专用**<br>(源码/二进制包) | 由 `modernime_integrate` 生成。<br>记录安装渠道 (`script`)、版本号、部署前缀及完整安装文件清单。 | **回滚**：恢复上一个版本的 channel 文件。<br>**卸载**：完成清理后由卸载器删除。原生包不生成此文件。 |
| **集成托管状态记录** | `$MANAGED_STATE_DIR/*.content`<br>`$MANAGED_STATE_DIR/*.path`<br>(默认 `$PREFIX/share/modernime/managed-files`) | **集成逻辑专用**<br>(`modernime_integrate`) | 记录 desktop、environment、autostart、xprofile 的原始内容基线和实际部署路径。 | **回滚**：与托管文件同步回滚。<br>**卸载**：若托管文件未被用户修改，清理状态目录；若用户改动过，保留基线以防二次误删。 |
| **桌面与环境集成** | `$DESKTOP_DIR/modernime-settings.desktop`<br>`$CONFIG_HOME/environment.d/90-modernime.conf`<br>`$CONFIG_HOME/autostart/modernime-fcitx5-session.desktop`<br>`$PREFIX/share/applications/modernime-settings.desktop`<br>`$HOME/.xprofile` (仅 ModernIME 标记块) | **集成逻辑托管**<br>(受控所有权) | 由 `modernime_integrate` 通过 `modernime_write_managed_file` 写入。<br>支持严格的路径与参数转义 (`modernime_desktop_argument`)。 | **防覆盖保护**：若文件已被用户手动修改（内容与 `.content` 基线不符）或被其它输入法占用，**必须保留用户修改，严禁静默覆盖**。<br>**卸载**：仅删除与基线一致的文件；用户修改过的文件打印提示并予以保留。 |
| **用户配置** | `$CONFIG_HOME/modernime/settings.conf`<br>`$CONFIG_HOME/modernime/settings.conf.bak` | **用户独占** | **安装器与卸载器严禁写入、修改或重置**！<br>仅运行时 `SettingsStore` 在用户通过界面保存时写入。 | **核心不变量**：安装、升级失败回滚、或卸载脚本**绝对不得触碰、备份或恢复**此文件！回滚过程绝不能将用户在升级期间的新改动重置为旧快照。 |
| **用户学习与持久化** | `$DATA_HOME/modernime/learning.sqlite3` (+WAL/SHM)<br>`$DATA_HOME/modernime/learning.lmdb` (+lock)<br>`$DATA_HOME/modernime/clipboard_history.sqlite3` | **用户独占** | 由 `modernime-data-guard` 负责在升级前做**在线事务备份与版本迁移**（使用 SQLite Online Backup API 与 LMDB 事务只读拷贝，严禁直接 cp 活跃库）。 | **核心不变量 (`RollbackPreservesConcurrentUserWrites`)**：<br>当安装/部署阶段发生故障触发回滚时，**严禁回滚恢复用户数据库**！用户在安装期间并发提交的新词、新打字学习记录必须永久保留在活跃数据库中。 |
| **用户自定义词典** | `$DATA_HOME/modernime/user-dictionary.txt`<br>`$DATA_HOME/modernime/user_words.txt`<br>`$DATA_HOME/modernime/custom_words.txt` | **用户独占** | 安装与卸载器完全透明跳过。 | 任何升级、回滚、卸载操作**绝对不得删除或覆盖**用户词典。 |
| **运行时进程与会话** | `$XDG_RUNTIME_DIR/modernime/*`<br>`/tmp/.modernime-*`<br>DBus 会话、Fcitx5 守护进程 | **运行时系统** | 由会话自启或 `modernime-session-init` 探测并拉起。 | 仅在显式请求且非隔离测试模式下通知 Fcitx5 重新加载。 |

---

## 3. 显式薄入口接口规范 (Explicit Thin Entry Points)

### 3.1 `modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR`

#### 功能定义
将位于 `PAYLOAD` 目录下的完整文件树原子、安全地部署到 `PREFIX` 安装前缀中，所有备份、状态记录与回滚凭证统一保存在 `TRANSACTION_DIR` 内。

#### 参数规范
1. `PAYLOAD` (第 1 参数，必须为非空绝对路径)：包含待安装产物的暂存目录（例如 `$stage_dir` 或解压后的二进制包 `usr` 目录）。
2. `PREFIX` (第 2 参数，必须为非空绝对路径)：目标安装目录（例如 `$HOME/.local` 或 `/usr`）。
3. `TRANSACTION_DIR` (第 3 参数，必须为非空绝对路径且已存在并可写)：本事务专用的临时控制目录。

#### 事务内部目录与元数据布局
```text
TRANSACTION_DIR/
├── payload.list         # NUL 分隔的 PAYLOAD 相对路径清单
├── backup/              # 预备份目录：记录 PREFIX 中将被覆盖的原文件
├── changed.list         # NUL 分隔的已写入 PREFIX 的相对路径清单
├── created_dirs.list    # NUL 分隔的在 PREFIX 中新建的目录清单
├── status               # 状态标识：PREPARED | DEPLOYING | COMMITTED | ROLLED_BACK | FAILED
└── transaction.log      # 事务审计日志（记录时间戳、文件数量、备份校验与异常）
```

#### 执行阶段与原子性保证
1. **参数校验与预检 (Pre-flight)**：
   - 检查 `PAYLOAD` 是否存在且包含合法 ModernIME 产物（例如 `bin/modernime-settings`）。
   - 检查架构兼容性（如为二进制产物，校验 `uname -m` 与 x86_64 匹配）。
   - 生成 `payload.list`（采用 `find "$PAYLOAD" ( -type f -o -type l ) -print0`，**严禁 eval，原生支持任意空格和特殊字符路径**）。
2. **预备份阶段 (Pre-backup)**：
   - 在向 `PREFIX` 写入任何字节前，遍历 `payload.list`。
   - 若 `dest_file="$PREFIX/$rel_path"` 存在：
     - 若为目录或非普通文件/非软链碰撞，立即拒绝并中止；
     - 复制原文件至 `$backup_dir/$rel_path` (`cp -a`)；
     - 若任一备份失败，事务立即中止，`PREFIX` 保持 100% 原始未改动状态。
3. **部署替换阶段 (Atomic Deployment)**：
   - 遍历 `payload.list`。
   - 对于每个目标文件，检测并记录缺失的父目录至 `created_dirs.list` 并 `mkdir -p`。
   - 写入 `changed.list`。
   - 执行 `cp -a --remove-destination -- "$src_file" "$dest_file"`。
     - **关键**：`--remove-destination` 保证复制前先 `unlink` 目标 inode。在 Fcitx5 运行中映射旧 `modernime_fcitx5.so` 的情况下，`unlink` 使得已打开的进程继续持有旧 inode 副本，新文件写入新 inode，彻底杜绝 in-place 覆写引发的 SIGSEGV/SIGBUS。
4. **故障回滚 (Rollback Protocol)**：
   - 若部署阶段发生任何错误（或被 `MODERNIME_SIMULATE_INSTALL_FAILURE=1` 触发）：
     - 自动触发 `modernime_rollback_payload "$PREFIX" "$TRANSACTION_DIR"`；
     - 逐项反向读取 `changed.list`，删除已写入的目标文件；
     - 从 `backup/` 中恢复被替换的原版本文件；
     - 逐项反向清理 `created_dirs.list` 中的空目录；
     - **核心断言**：回滚仅作用于 `changed.list`，**严禁触碰任何 `$CONFIG_HOME` 或 `$DATA_HOME` 中的用户数据**；
     - 失败退出码非 0，若回滚过程有异常，保留 `TRANSACTION_DIR` 以供审计。
5. **提交成功 (Commit)**：
   - 清理 `backup/` 与中间清单，标记 `status=COMMITTED`，返回 0。

---

### 3.2 `modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR`

#### 功能定义
在产物部署完成后，建立桌面快捷方式、环境配置文件、自启动条目与安装清单。所有参数完全显式化，废除对外部隐式环境变量的依赖。

#### 参数规范
1. `PREFIX` (第 1 参数)：已完成部署的安装前缀。
2. `CONFIG_HOME` (第 2 参数)：用户配置主目录（`XDG_CONFIG_HOME`，如 `$HOME/.config`）。
3. `DESKTOP_DIR` (第 3 参数)：用户桌面目录（`$HOME/Desktop` 或 `xdg-user-dir DESKTOP`）。
4. `MANAGED_STATE_DIR` (第 4 参数)：托管集成状态基线存储目录（通常为 `$PREFIX/share/modernime/managed-files`）。

#### 集成动作与保护规范
1. **插件目录自适应与链接**：
   - 探测系统与前缀插件目录 (`system_addon_dir`, `prefix_addon_dir`)。
   - 若前缀为 `$HOME/.local` 且实际插件位于多架构目录，在 `$PREFIX/lib/fcitx5` 创建受控软链接，并在 `$MANAGED_STATE_DIR/addon-link.target` 记录。
2. **桌面快捷方式 (`desktop`)**：
   - 生成 `modernime-settings.desktop`，通过 `modernime_desktop_argument` 与 `modernime_desktop_string` 转义 Exec 与 Icon。
   - 调用 `modernime_write_managed_file` 写入 `$DESKTOP_DIR`。若用户已修改此文件，保留用户版本。
3. **环境配置 (`environment`)**：
   - 生成 `$CONFIG_HOME/environment.d/90-modernime.conf`，写入 `FCITX_ADDON_DIRS`、`XDG_DATA_DIRS`、`GTK_IM_MODULE=fcitx` 等。
   - 通过基线保护写入。
4. **自启动条目 (`autostart`)**：
   - 生成 `$CONFIG_HOME/autostart/modernime-fcitx5-session.desktop`。
   - 通过基线保护写入。
5. **Xprofile 环境变量块 (`xprofile`)**：
   - 若非系统前缀且未显式跳过，使用 `modernime_write_profile_block` 将 `# BEGIN MODERNIME ENV` 块受控插入 `$HOME/.xprofile`，保留用户已有其它环境变量。
6. **安装清单与渠道事实 (`manifest & channel`)**：
   - 写入 `$PREFIX/share/modernime/install-channel.txt`（`channel=script`, `version=...`, `prefix=...`）。
   - 生成 `$PREFIX/share/modernime/install-manifest.txt`，列清所有已部署文件、文档、图标及托管文件路径，作为对称卸载的唯一事实依据。

---

## 4. 核心断言：`RollbackPreservesConcurrentUserWrites`

### 4.1 故障模式与防退化证明

在实际生产场景中，安装或升级 ModernIME（无论是源码安装还是二进制升级）可能耗时数秒到数分钟。在此期间，用户可能正持续打字输入、产生新的 SQLite 学习条目（提交至 WAL 帧），或在设置界面中修改了候选词个数或主题。

**历史严重缺陷**：
早期的安装脚本在失败时可能尝试将备份目录整个拷回，或者调用恢复例程把升级前备份的用户配置与数据库覆盖回去，导致：
1. 升级期间用户新增的学习词汇、词频更新因被旧数据库快照覆盖而丢失。
2. 升级期间用户修改的配置项（如快捷键、字号）被旧配置覆盖。

**T17 防退化铁律**：
```text
[安装前状态: Version A] ---> 开始安装 Version B
       |
       +---> 用户在后台并发打字:
       |     * SQLite WAL 产生新词条: ('bing''fa', '并发学习词条')
       |     * settings.conf 修改为: candidate.page_size=9, theme=dark
       |
       +---> 发生中途部署故障 (例如磁盘满或注入失败)
       |
       v
[触发回滚: Rollback Protocol]
       |
       +---> 严格只针对 changed.list 中的 PREFIX 产物恢复 Version A
       |
       v
[断言 RollbackPreservesConcurrentUserWrites]
       1. learning.sqlite3 WAL 完整性 OK，且包含升级期间写入的 '并发学习词条'！
       2. settings.conf 完好无损，保持 candidate.page_size=9 和 theme=dark！
       3. 用户词典、LMDB 没有任何数据丢失！
       4. PREFIX 成功恢复至 Version A，且正在运行的 Fcitx5 进程持有的 .so 句柄未被损坏！
```

---

## 5. 对称卸载与原生系统包隔离 (Uninstall Symmetry & Native Package Guard)

`uninstall.sh` 必须与 `modernime_deploy_payload` 和 `modernime_integrate` 严格对称：
1. **基于 Manifest 清理**：仅清理 `$PREFIX/share/modernime/install-manifest.txt` 中显式记录的文件。
2. **原生包保护 (Native Guard)**：
   在系统前缀（`/usr`, `/opt` 等）下执行卸载时，每个待删除文件前先调用：
   ```bash
   if command -v dpkg >/dev/null 2>&1 && dpkg -S "$file" >/dev/null 2>&1; then
       # 属于 dpkg 管理，跳过删除
       continue
   fi
   if command -v rpm >/dev/null 2>&1 && rpm -qf "$file" >/dev/null 2>&1; then
       # 属于 rpm 管理，跳过删除
       continue
   fi
   ```
   **严禁让脚本卸载清理 native 原生包归属的文件**！
3. **托管文件受控清理**：
   通过 `modernime_remove_managed_file` 校验 baseline，用户自行修改过的桌面快捷方式、环境文件或 `.xprofile` 条目自动保留，只清理未被修改的托管文件。
4. **用户数据绝不清理**：
   卸载脚本**绝对不删除** `$CONFIG_HOME/modernime` 与 `$DATA_HOME/modernime`，保证用户重装或更换输入法后个人词库与习惯依然完好。

---

## 6. 二进制打包生成器改造规范 (Binary Installer Generator Refactoring)

在实施阶段（T14 移交后）：
- 废除 `scripts/generate_binary_installer.sh` 中对 `install.sh` 进行 `awk` 行文本切片的脆弱机制。
- 二进制安装包的 `install.sh` 将直接由预置的标准化薄包装脚本生成，结构如下：
  ```bash
  #!/usr/bin/env bash
  set -euo pipefail
  project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  source "$project_root/scripts/install_state.sh"
  source "$project_root/scripts/install_lifecycle.sh"
  
  # 显式参数解析与前缀推导
  ...
  
  # 调用显式生命周期
  modernime_deploy_payload "$project_root/usr" "$prefix" "$transaction_dir"
  modernime_integrate "$prefix" "$config_home" "$desktop_dir" "$managed_state_dir"
  ```
- 打包脚本 `generate_binary_installer.sh` 仅需复制 payload、`install_state.sh`、`install_lifecycle.sh`、`modernime-data-guard` 等必要工具，不再对源码脚本进行正则/行匹配切片。

---

## 7. 准备阶段验收与整改交接路线图

1. **当前阶段 (PREPARING)**：
   - 冻结本文档（所有权矩阵与薄入口设计）。
   - 编写独立的失败与回滚夹具测试 `tests/install_lifecycle_prep_test.sh`。
   - 提供 `scripts/install_lifecycle.sh` 作为独立生命周期核心函数库。
   - 独立验证 `RollbackPreservesConcurrentUserWrites` 在隔离沙箱中 100% 成立。
   - 写入 `build/parallel-handoff/T17.json`，标记状态为 `PREPARING`。
   - 发送通知给协调者，等待 T14 路径契约复审与移交通知。
2. **实施阶段 (IMPLEMENTING)**：
   - 协调者验收 T14 路径契约后，将路径实现移交 T17。
   - 将 `install.sh`、`install_binary_payload.sh`、`generate_binary_installer.sh`、`uninstall.sh` 迁移至 `modernime_deploy_payload` 与 `modernime_integrate`。
   - 运行完整 G1、G3、G5 及全新克隆沙箱测试。
