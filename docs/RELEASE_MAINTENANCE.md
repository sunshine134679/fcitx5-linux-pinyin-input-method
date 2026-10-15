# Release 与在线更新维护

产品版本由根目录 `CMakeLists.txt` 的 `project(ModernIME VERSION ...)` 定义。正式发布以不可变的 `vX.Y.Z` 标签绑定源码；官网使用 `releases/latest` 查询正式版，并保留静态下载后备入口。

## 每次发布

1. 同步 CMake 版本、README 当前产物名和版本记录、`docs/releases/vX.Y.Z.md`、官网 `index.html` 与 `app.js` 后备版本，以及官网版本断言。历史发布说明保留原版本。
2. 按 AGENTS.md 验证算法契约、完整 CTest、实际注册的 GTest/Benchmark、官网回归与隔离全新克隆安装卸载。没有图形会话时明确记录 GTK 跳过，不能记为通过。含私有未跟踪数据的工作区应先制作仅含源码的隔离快照再运行 `tests/verify_fresh_clone.sh`，该脚本会复制未跟踪文件。
3. 提交并正常推送代码到发布分支/主分支；核对远程主分支的新提交。不可强推覆盖别人改动。创建新标签并推送，触发 `.github/workflows/release.yml`。
4. Release 工作流校验标签版本与源码 SHA，两套 Ubuntu 构建先检查必需算法及安装测试的注册与执行，再生成 DEB；openEuler 原生容器生成 RPM；同时构建便携包。
5. 发布门禁验证资产、升级备份、回滚、卸载及签名，再生成生产签名 APT/DNF 仓库。保留历史版本，同版本重试替换包。持久 GPG 私钥和口令由 GitHub Actions Secrets 提供，禁止临时测试密钥进入生产。
6. 同一流水线部署 Pages、持久化 `gh-pages` 并创建带源码 SHA 的 GitHub Release。官网独立流水线与 Release 共用 Pages 并发组，并保留 APT/RPM 目录，避免覆盖在线源。

## 发布后验收

- Actions 中 Release 构建、签名仓库与 Pages 部署全部成功；`releases/latest` 的标签、说明、两份 DEB、RPM、便携归档和 SHA256 清单与版本一致。
- 官网 API 正常与失败后备都指向当前版；`apt/dists/{jammy,noble,bookworm}/InRelease` 和 Packages 包含新版本，`rpm/oe2409/x86_64/repodata/repomd.xml` 引用的 primary 元数据包含新版本。
- 使用站点公钥验签 InRelease、RPM 元数据，并核对软件源包体与 Release 资产的 SHA256。公钥指纹必须保持可信；密钥轮换通过既有轮换配置进行。
- 本机安装后核对 `share/modernime/install-channel.txt`、清单及运行中的 Fcitx 插件。文件更新与会话激活分别记录，未重启不能宣称运行的插件已升级。

## 用户更新渠道

- DEB：设置客户端刷新 APT 缓存后更新，或 `sudo apt update && sudo apt install --only-upgrade modernime`。
- RPM：设置客户端刷新 DNF 缓存后更新，或 `sudo dnf upgrade --refresh modernime`。
- 源码：先确认工作区无未提交改动，在跟踪发布代码的分支执行 `git pull --ff-only && ./install.sh`。
- 便携：下载新版归档并校验，执行其 `install.sh`。源码和便携均记录 `channel=script`，不会通过 APT/DNF 直接跨渠道覆盖；需要转换时使用 `modernime-migrate-channel`。

Google Benchmark 的最短运行时间使用 `--benchmark_min_time=0.02`，兼容旧版纯浮点解析器；不要改成旧版不接受的 `0.02s`。参见 [v1.6.1 参数解析实现](https://github.com/google/benchmark/blob/v1.6.1/src/commandlineflags.cc)。

## 失败与修复

发布失败先查失败作业，不把仅推送标签记为发布成功。网络或基础设施故障可重试原标签；源码修复必须创建更高版本的新标签，不移动已发布标签。严禁为了通过门禁跳过必需算法测试、降低性能阈值或换用临时签名密钥。仓库签名或生产 Secrets 缺失时保留旧的在线源并明确报告阻塞。
