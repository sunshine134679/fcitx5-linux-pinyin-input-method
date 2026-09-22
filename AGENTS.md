# ModernIME 开发与维护规范 (Engineering Guidelines)

为了确保用户从 GitHub 拉取代码后能够始终“开箱即用、一键安装、平滑运行”，在涉及功能新增、架构重构、依赖引入或构建调整等较大变更时，必须严格遵守以下**安装生命周期维护准则（Clean-Clone-to-Install Lifecycle）**：

---

## 1. 依赖项同步管理 (Dependency Synchronization)
- 凡是在代码中引入新的系统库、头文件或工具（如 GTK、Pango、SQLite、Boost、Fcitx5、LibIME 等）：
  1. 必须在 `README.md` 中的 `sudo apt install` 命令清单中同步更新；
  2. 必须在根目录及子模块 `CMakeLists.txt` 中使用 `find_package` 或 `pkg_check_modules` 明确声明与校验；
  3. 严禁隐式依赖开发机已存在但在全新系统/全新克隆中未声明的软件包。

---

## 2. GitHub 拉取与一键安装脚本维护 (Clean Clone & Install)
- 保证全新环境下拉取代码后直接执行 `./install.sh` 即可完成完整闭环：
  - **默认克隆地址友好度**：`README.md` 提供通用的 HTTPS 克隆地址（兼容无 GitHub SSH Key 配置的用户），同时提供 SSH 地址备选；
  - **离线与数据自包含**：离线扩展词库、内置拼音数据必须随仓库发布或由构建系统自动离线生成，安装过程默认不发网络请求、无需额外手动下载大文件；
  - **参数与前缀自适应**：`install.sh` 必须支持 `MODERNIME_PREFIX`、`MODERNIME_BUILD_DIR`、`MODERNIME_CONFIG_HOME`、`MODERNIME_DESKTOP_DIR`、`MODERNIME_SKIP_FCITX_RESTART` 等环境变量，确保在无图形环境、CI/CD、沙箱测试或非标准路径下均可平稳构建安装并完全隔离宿主配置；
  - **桌面与环境集成**：自动生成 `modernime-settings.desktop`、环境变量 `90-modernime.conf`、会话自启 `modernime-fcitx5-session.desktop` 与安装清单 `install-manifest.txt`。

---

## 3. 干净卸载与幂等性 (Clean Uninstallation)
- `uninstall.sh` 必须与 `install.sh` 严格对称：
  - 基于 `install-manifest.txt` 进行受控清理；
  - 清理桌面快捷方式、环境配置文件与 autostart 文件；
  - 对用户手动修改过的配置文件进行保护与提示，严禁误删用户其它系统文件。

---

## 4. 重大变更后的端到端验证流程 (Mandatory Verification Workflow)
- 每次完成较大功能更改或架构调整后，推送远程前必须执行以下验证：
  1. **单元测试回归**：运行 `ctest` 确保 47 项及后续新增测试 100% 通过（包括 `modernime_install_runtime`、`modernime_install_environment`、`modernime_settings_install`）；
  2. **模拟全新克隆构建**：在临时隔离目录验证完整流程（需显式传入配置与桌面隔离目录，避免污染宿主自启与快捷方式）：
     ```bash
     git clone . /tmp/test-fresh-clone
     cd /tmp/test-fresh-clone
     MODERNIME_PREFIX=/tmp/test-prefix \
     MODERNIME_CONFIG_HOME=/tmp/test-prefix/config \
     MODERNIME_DESKTOP_DIR=/tmp/test-prefix/Desktop \
     MODERNIME_SKIP_FCITX_RESTART=1 ./install.sh
     MODERNIME_PREFIX=/tmp/test-prefix \
     MODERNIME_CONFIG_HOME=/tmp/test-prefix/config \
     MODERNIME_DESKTOP_DIR=/tmp/test-prefix/Desktop ./uninstall.sh
     ```
  3. **文档同步更新**：同步更新 `README.md` 中的特性描述、配置项、快捷键与常见排错指南。
