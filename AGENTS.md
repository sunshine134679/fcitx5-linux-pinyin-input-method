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
  2. **模拟全新克隆构建**：运行自动化沙箱验证脚本（自动创建隔离环境并验证完整安装与卸载闭环，严禁直接在 `/tmp` 下裸跑无参脚本）：
     ```bash
     ./tests/verify_fresh_clone.sh
     ```
     *注：`install.sh` 与 `uninstall.sh` 已内置防误删保护，若在 `/tmp` 或 `/var/tmp` 路径下执行且未显式指定 `MODERNIME_PREFIX`，将直接拒绝执行并报警，防止误清宿主用户真实环境配置。*
  3. **文档同步更新**：同步更新 `README.md` 中的特性描述、配置项、快捷键与常见排错指南。

---

## 5. 算法更新与打字体验防退化准则 (Algorithm Evolution & Anti-Regression Baseline)
- 在对 ModernIME 的**候选排序、拼音切分、中英仲裁、自学习、分段造词、纠错或持久化存储算法**进行任何更新、重构或参数调整之前：
  1. **更新前必读**：**必须先完整查阅 [`docs/ALGORITHM_EVOLUTION.md`](file:///home/wsl/ModernIME/docs/ALGORITHM_EVOLUTION.md)** 中的「算法更新前必读核对清单」与「体验不可退化不变量矩阵」；
  2. **防特性覆盖**：严禁任何以“优化某单项指标”为由破坏、覆盖或削弱既有算法特性的行为（例如严禁降低 200,000 词库容量上限、严禁让生僻英文抢占高频中文、严禁破坏艾宾浩斯时间遗忘衰减等）；
  3. **更新后必记**：新算法开发并通过 CTest、GTest 和 Benchmark 验证后，**必须在 `docs/ALGORITHM_EVOLUTION.md` 中追加记录**本次新增特性的算法逻辑、实现的具体代码文件、以及给用户实际打字体验带来的变化。
  4. **架构与契约**：修改前同时查阅 [`docs/ALGORITHM_ARCHITECTURE.md`](docs/ALGORITHM_ARCHITECTURE.md) 的特性契约、参数用途及交互表，按 [`docs/ALGORITHM_CHANGE_GUIDE.md`](docs/ALGORITHM_CHANGE_GUIDE.md) 记录涉及的契约 ID、状态权限、依赖、预期差异与验证证据。新候选来源不得绕过已有隐私、学习资格、持久屏蔽、用户清空和消费范围边界；不同用途的衰减参数不得因重构而盲目统一。
  5. **增删与验收**：新增特性登记稳定 ID；停用或删除时检查依赖、保留退役 ID 与历史记录，禁止把删除算法等同于静默删除用户数据。无意图行为变化的场景保留原测试断言；验收时确认 GTest、Benchmark 等必需测试实际注册，未注册或跳过不能记为通过。架构文档中的待迁移设计和拟建 CI 能力不得描述成已经实现。
  6. **候选链路**：涉及候选召回、排序、混排、前缀派生、分页、绘制或选择映射时，查阅 [`docs/CANDIDATE_PIPELINE_ARCHITECTURE.md`](docs/CANDIDATE_PIPELINE_ARCHITECTURE.md) 的阶段职责和子契约。新来源不得自行插入固定高位；派生项不得用父词证据覆盖自身学习；学习验收必须包含重复选择后的最终全局位置、物理页面和消费范围，不能仅以词频或评分增加作为有效提权的证明。文档中的新晋升目标需经实现与验证后才能宣称达成。

