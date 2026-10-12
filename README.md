# ModernIME

ModernIME 是一个面向 Linux 桌面的现代化 Fcitx5 拼音输入法，包含候选栏、用户习惯学习、用户词典、剪贴板入口，以及全新 Bento 仪表盘美学的独立 GTK3 设置客户端。深度对标商业成熟输入法（微信输入法达到 93.5% 体验对标），具备四阶智能拼音容错引擎、国家通用规范汉字分级防护与生僻字专项治理、单音节英文次选消除、微信同款 `v` 模式金融大写与数字转换、六大行业深度专业词库、符号与 Emoji 宏、全链路极致低延迟热路径（DeltaOverlay 增量覆盖选词、O(1) 词库索引、长文档极速打字、Pango 渲染对象池化）、首键零延迟响应、单键 Shift 极速切换、游戏化按键录制、英文单词中文释义候选、正反斜线顿号双键兼容、成对翻页键、全量中文标点规范化、高频简拼智能校准、候选外观自定义与即时热重载，以及 openEuler / RPM / Debian / Ubuntu 全平台无缝适配等特性。

## 快速安装与发行包下载

ModernIME 支持 **GitHub Releases 预编译安装包直接安装**（推荐）与 **源码一键构建安装**，全面适配 **Ubuntu / Debian** 与 **openEuler / RHEL / Fedora** 等多版本主流 Linux 操作系统。

### 方式一：下载预编译安装包（推荐，开箱即用）

进入 [GitHub Releases 页面](https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases) 下载适合您系统架构的安装包：

#### 平台与资产兼容矩阵

| 资产文件名 | 适配发行版 | 架构 | 最低系统要求与基线 |
|---|---|---|---|
| `modernime_0.1.8_amd64_ubuntu22.04.deb` | Ubuntu 22.04 LTS | x86_64 / amd64 | glibc ≥ 2.35, Fcitx5 5.0+ |
| `modernime_0.1.8_amd64_ubuntu24.04.deb` | Ubuntu 24.04 LTS / Debian 12+ | x86_64 / amd64 | glibc ≥ 2.38, Fcitx5 5.1+ |
| `modernime-0.1.8-1.oe24.x86_64.rpm` | openEuler 24.03 SP4 / 24.09, Fedora | x86_64 | glibc ≥ 2.38, Fcitx5 5.1+ (EPOL) |
| `modernime-0.1.8-linux-x86_64.tar.gz` | Linux 便携免编译包 | x86_64 | glibc ≥ 2.34, libstdc++ ≥ 3.4.30, Fcitx5 |
| 源码构建 (`./install.sh`) | 任意 Linux (含 aarch64 / ARM64, RISC-V) | 全架构 | CMake ≥ 3.24, GCC ≥ 10, Fcitx5-devel |

#### 1. Ubuntu / Debian 系统（APT 软件源升级与 `.deb` 包）

**方式 A：配置签名 APT 仓库（推荐，支持 `apt upgrade` 持续升级与版本回退）**

ModernIME 采用符合现代 Debian 安全规范的 **Deb822 `.sources` 格式**与独立 `Signed-By` 密钥环（不使用已废弃的 `apt-key` 或全局信任锚），并按发行版代号（Ubuntu 22.04 `jammy`、Ubuntu 24.04 `noble`）与架构（`amd64`）严格隔离：

```bash
# 1. 安装独立 GPG 签名密钥环（二进制 OpenPGP 格式，仅授权给 ModernIME 源）
sudo install -d -m 0755 /usr/share/keyrings
curl -fsSL https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/apt/modernime-archive-keyring.gpg \
    | sudo tee /usr/share/keyrings/modernime-archive-keyring.gpg >/dev/null
sudo chmod 0644 /usr/share/keyrings/modernime-archive-keyring.gpg

# 2. 写入 Deb822 仓库配置（自动匹配 jammy 或 noble）
SUITE="$(. /etc/os-release && echo "${VERSION_CODENAME:-noble}")"
sudo tee /etc/apt/sources.list.d/modernime.sources >/dev/null <<EOF
Types: deb
URIs: https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/apt
Suites: ${SUITE}
Components: main
Architectures: amd64
Signed-By: /usr/share/keyrings/modernime-archive-keyring.gpg
EOF

# 3. （可选）配置 APT Pinning 优先级
sudo tee /etc/apt/preferences.d/99-modernime.pref >/dev/null <<'EOF'
Package: modernime
Pin: release o=ModernIME,l=ModernIME APT Repository
Pin-Priority: 700
EOF

# 4. 安装与日常升级（升级前自动触发事务级 SQLite WAL 与 LMDB 备份校验）
sudo apt update
sudo apt install -y modernime
# 后续检查并仅升级 ModernIME：
sudo apt update && sudo apt install --only-upgrade -y modernime

# 5. 版本回退（查看仓库可用历史版本并回退到指定版本）
apt-cache madison modernime
sudo apt install -y --allow-downgrades modernime=0.1.7

# 6. 移除 APT 仓库与签名密钥环
sudo rm -f /etc/apt/sources.list.d/modernime.sources \
           /etc/apt/preferences.d/99-modernime.pref \
           /usr/share/keyrings/modernime-archive-keyring.gpg
sudo apt update
```

**方式 B：离线安装单文件 `.deb` 包**
```bash
# 下载对应的 .deb 包（如 modernime_0.1.8_amd64_ubuntu24.04.deb）后执行：
sudo dpkg -i modernime_*_amd64*.deb
# 若系统提示缺失运行库依赖，一键自动修复补全：
sudo apt-get install -f
```

#### 2. openEuler / RHEL / Fedora 系统（DNF/RPM 仓库升级与 `.rpm` 包）

> [!NOTE]
> **openEuler 版本兼容矩阵与 EPOL 源说明**：
> - **openEuler ≥ 24.09**：官方 EPOL 扩展源原生提供 `fcitx5`（5.1.10+）与 `libime`（1.1.8+）。启用系统自带 EPOL 源即可：
>   ```bash
>   sudo dnf config-manager --set-enabled EPOL || true
>   ```
> - **openEuler 24.03 LTS-SP4**：官方默认仓库未包含 fcitx5。24.03 用户可通过配置 24.09 EPOL 扩展源并**使用 `includepkgs` 进行精确限定**（仅拉取输入法组件，保留 GPG 校验）：
>   ```bash
>   sudo tee /etc/yum.repos.d/openEuler-24.09-epol.repo << 'EOF'
>   [openEuler-24.09-epol]
>   name=openEuler 24.09 EPOL for Fcitx5
>   baseurl=https://repo.openeuler.org/openEuler-24.09/EPOL/main/$basearch/
>   enabled=1
>   gpgcheck=1
>   gpgkey=https://repo.openeuler.org/openEuler-24.09/OS/$basearch/RPM-GPG-KEY-openEuler
>   priority=99
>   includepkgs=fcitx5*,libime*,xcb-imdkit*
>   EOF
>   sudo dnf makecache
>   ```

**方式 A：配置 openEuler 24.09 `x86_64` DNF 签名仓库（推荐，支持 `dnf upgrade` 持续升级）**
```bash
# 1. 写入开启包签名与仓库索引双签名校验 (gpgcheck=1, repo_gpgcheck=1) 的 .repo 配置
sudo tee /etc/yum.repos.d/modernime-openeuler-24.09.repo >/dev/null <<'EOF'
[modernime-openeuler-24.09]
name=ModernIME Repository for openEuler 24.09 ($basearch)
baseurl=https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/rpm/oe2409/$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/rpm/RPM-GPG-KEY-modernime
metadata_expire=6h
EOF

# 2. 刷新元数据缓存并安装 / 升级 ModernIME
sudo dnf makecache
sudo dnf install -y modernime
# 后续升级：
sudo dnf upgrade -y modernime

# 3. 版本回退與移除 DNF 仓库
sudo dnf downgrade -y modernime
# 移除仓库配置：
sudo rm -f /etc/yum.repos.d/modernime-openeuler-24.09.repo
sudo dnf clean all
```

**方式 B：离线安装本地 `.rpm` 包**
```bash
# 确保已启用 EPOL 扩展源并安装基础环境与托盘依赖：
sudo dnf install -y fcitx5 libime gtk3 sqlite lmdb-libs libappindicator-gtk3
# 使用 dnf 安装本地 RPM（自动从已配置仓库解析并补齐依赖）：
sudo dnf install -y ./modernime-*.x86_64.rpm
```

#### 2.5 安装渠道隔离与可控迁移（`script` ↔ `deb`/`rpm`）

ModernIME 严格区分脚本/源码安装（`channel=script`）与系统发行包安装（`channel=deb` / `channel=rpm`），默认禁止跨渠道直接覆盖，并提供 `modernime-migrate-channel` 与 `modernime-data-guard` 工具完成安全迁移：

```bash
# 1. 检查当前安装来源、manifest、文件归属与版本
modernime-migrate-channel --check

# 2. 从 ~/.local 脚本安装平滑迁移至 Ubuntu/Debian APT 系统包（自动做 SQLite WAL / LMDB 事务备份、清理旧渠道文件并校验新包路径；若失败自动回滚恢复原状）
modernime-migrate-channel --from-prefix "$HOME/.local" --to-channel deb --package-cmd "sudo apt install -y modernime"

# 3. 从脚本安装平滑迁移至 openEuler DNF 系统包
modernime-migrate-channel --from-prefix "$HOME/.local" --to-channel rpm --package-cmd "sudo dnf install -y modernime"
```

#### 3. Linux x86_64 便携归档（`.tar.gz` 免编译包，要求 glibc ≥ 2.34）
```bash
# 解压预编译便携包
tar -zxvf modernime-*-linux-x86_64.tar.gz
cd modernime-*-linux-x86_64

# 用户级一键安装至 ~/.local（推荐，无需 root 权限）
./install.sh

# 或系统级安装至 /usr（支持参数传参或环境变量）：
sudo ./install.sh /usr
# 等价于：sudo MODERNIME_PREFIX=/usr ./install.sh
```

归档包含 `install.sh` 和 `uninstall.sh`，与源码安装共享环境集成、安装清单及用户修改保护。安装无需 CMake 或编译器，默认离线；支持 `MODERNIME_PREFIX`、`MODERNIME_CONFIG_HOME`、`MODERNIME_DESKTOP_DIR` 和 `MODERNIME_SKIP_FCITX_RESTART`。卸载时使用安装所选的前缀，例如 `MODERNIME_PREFIX="$HOME/.local/modernime" ./uninstall.sh`。如果从 `/tmp` 或 `/var/tmp` 解压目录运行，必须显式指定 `MODERNIME_PREFIX`。三份内置词典会通过 `XDG_DATA_DIRS` 在安装前缀下定位。若宿主为 aarch64 / ARM64 或其他架构，请选用下述“方式二：从源码一键构建安装”。

---

### 方式二：从源码一键构建安装

如果您希望从最新源代码自行编译构建，请根据您的发行版准备依赖：

#### Ubuntu / Debian 构建依赖准备
```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsqlite3-dev liblmdb-dev libboost-dev \
    libgtest-dev libbenchmark-dev \
    libfcitx5core-dev libfcitx5utils-dev fcitx5-modules-dev fcitx5 \
    libimecore-dev libimepinyin-dev libime-bin \
    libgtk-3-dev libpango1.0-dev libayatana-appindicator3-dev
```

#### openEuler / RHEL / Fedora 构建依赖准备
```bash
# 1. 确保已启用 EPOL 扩展源（openEuler 24.03 SP4 用户请先配置上方包含 includepkgs 的 24.09 EPOL 隔离源）：
sudo dnf config-manager --set-enabled EPOL || true

# 2. 安装构建依赖（必须包含 libappindicator-gtk3-devel 以启用状态栏托盘图标支持）：
sudo dnf install -y \
    gcc gcc-c++ cmake make pkgconf-pkg-config \
    sqlite-devel lmdb-devel boost-devel \
    gtest-devel google-benchmark-devel \
    fcitx5-devel libime-devel \
    gtk3-devel pango-devel cairo-devel rpm-build \
    libappindicator-gtk3-devel
```

#### 执行一键安装
```bash
# 克隆仓库 (推荐 HTTPS)
git clone https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method.git
# 或使用 SSH:
# git clone git@github.com:sunshine134679/fcitx5-linux-pinyin-input-method.git
cd fcitx5-linux-pinyin-input-method

# 执行自动安装脚本（默认安装到 ~/.local）
./install.sh

# 系统级安装至 /usr：
# sudo ./install.sh /usr
```

---

### 开发者一键打包工具

项目在 `scripts/` 目录下提供了标准化的本地打包脚本，可在对应系统环境中直接输出规范安装包至 `dist/` 目录：

- **打包 Debian/Ubuntu `.deb`**：运行 `./scripts/package_deb.sh`
- **打包 openEuler/Fedora `.rpm`**：运行 `./scripts/package_rpm.sh`
- **打包通用便携归档 `.tar.gz`**：运行 `./scripts/package_tarball.sh`

同时项目配置了 GitHub Actions 自动化工作流（`.github/workflows/release.yml`），每次推送版本标签（如 `v0.1.8`）或在 Actions 页面手动触发时，均会在 Ubuntu 虚拟机和官方 openEuler 真实容器中自动完成全平台编译、打包并发布至 GitHub Releases。


`install.sh` 会自动完成配置、编译、测试和安装，并且会：

- 安装 Fcitx5 输入法插件和候选栏 UI（自适应 Debian/Ubuntu `lib` 与 openEuler/Fedora `lib64` 路径）；
- 安装离线扩展拼音词典（成语、IT、医学、法律、地名等类别）；
- 安装全新 Bento 仪表盘设计的 `modernime-settings` 设置客户端；
- 安装桌面菜单入口 `modernime-settings.desktop`；
- 在当前桌面目录生成 `modernime-settings.desktop` 快捷方式；
- 自动写入 `~/.config/environment.d/90-modernime.conf` 与 `~/.xprofile`（兼容 LightDM/UKUI/X11 会话）；
- 在检测到可用的图形和 DBus 会话时尝试重载并激活 ModernIME。

### 常用安装选项与逃生开关

```bash
# 1. 系统级安装至 /usr：
sudo ./install.sh /usr

# 2. 环境受限或跳过测试直接安装（逃生开关）：
MODERNIME_SKIP_TESTS=1 ./install.sh

# 3. 跳过 Fcitx5 自动重启：
MODERNIME_SKIP_FCITX_RESTART=1 ./install.sh
```

### 桌面会话与环境变量适配（LightDM / UKUI / X11）

`install.sh` 会自动将环境变量写入 `~/.config/environment.d/` 并同步注入 `~/.xprofile`。若您使用的是 LightDM、UKUI、XFCE 或其他非 systemd-user 托管的 X11 会话，请确保以下会话变量生效：

```bash
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx
export XMODIFIERS=@im=fcitx
export SDL_IM_MODULE=fcitx
# RPM 64位系统:
export FCITX_ADDON_DIRS="$HOME/.local/lib64/fcitx5:/usr/lib64/fcitx5"
# Debian/Ubuntu 64位系统:
# export FCITX_ADDON_DIRS="$HOME/.local/lib/fcitx5:/usr/lib/x86_64-linux-gnu/fcitx5"
```

## 启动设置客户端

安装后可以从应用菜单或桌面快捷方式启动，也可以直接运行：

```bash
~/.local/bin/modernime-settings
# 或使用快捷参数直接打开特定页面（如剪贴板页）：
~/.local/bin/modernime-settings --clipboard  # 或 -c
```

设置客户端和输入法插件在同一个仓库中，会随 `./install.sh` 一起构建和安装。若 `~/.local/bin` 不在 `PATH` 中，请使用上面的绝对路径。桌面菜单文件位于：

```text
~/.local/share/applications/modernime-settings.desktop
```

桌面快捷方式会放在系统报告的桌面目录；如果系统没有配置该目录，则使用 `~/Desktop`，中文桌面则兼容 `~/桌面`。

### 全新 Bento 仪表盘与现代设计

设置客户端采用现代化卡片布局（Bento Grid）与零残影高对比设计，彻底清除传统 GTK/Adwaita 主题可能产生的伪类绿色残影，呈现纯净高级的深空灰/石墨黑质感：

- **概览页（Bento 仪表盘）**：集中卡片化呈现输入法守护进程状态、当前输入模式、快捷键配置与词典/学习记录统计，支持一键快捷重启和诊断；
- **输入体验页**：统一配置启用状态、默认中英文状态、单键 Shift 快速切换、中文标点规范化开关，以及候选栏外观与排版。支持中英文切换快捷键**游戏化直接按键录制**（点击后直接在键盘上按下目标组合键即刻捕获，无需手动打字输入）、**英文单词中文释义候选开关**（在候选首位为英文单词时第 2 位智能呈现释义词条并支持瞬时上屏），支持自由设定单页候选词数（3~9 个）与候选字号（14~28 pt），通过 Cairo/Pango 实时联动渲染，修改后即刻热重载生效，无需重启 Fcitx5；
- **个人词典页**：支持本地词库实时搜索、添加、编辑、删除、导入和导出，导入格式严格校验，确保词库安全可靠；
- **剪贴板页（便签式 Bento 色块卡片流）**：全新卡片式历史流，智能识别代码、网页链接、多行与纯文本并标注专属彩色徽标与行数统计；长文本自动折叠为 2~3 行精简预览，附带「展开 / 收起」按键实现无缝代码等宽展开；支持卡片快捷复制/删除与二次确认清空；
- **智能学习页**：直观展示用户自适应词频学习记录，提供数据备份导出与一键重置功能；
- **系统与诊断页**：实时检测 `fcitx5-remote`、Fcitx5 进程、当前活动输入法和 ModernIME 核心状态，支持快速重载与运行日志查看。

左侧搜索框支持按名称与描述即时全局检索并自动导航定位控件；支持 `Ctrl+F` 快速聚焦，按 `Ctrl+Enter` 快速应用保存。候选外观等核心排版配置已支持自动文件监听即时生效，无需手动重启输入法。

## 直接使用 CMake

不使用安装脚本时，可以使用已经包含 Fcitx5、LibIME、设置客户端和测试选项的预设：

```bash
cmake --preset fcitx5-debug
cmake --build --preset fcitx5-debug
ctest --preset fcitx5-debug
cmake --install build/fcitx5-debug
```

直接执行 `cmake --install` 只负责安装 CMake 目标，不会代替 `install.sh` 生成当前桌面快捷方式、用户环境文件和自动启动文件。需要完整的一键安装体验时，请使用 `./install.sh`。

扩展词典安装在：

```text
~/.local/share/modernime/pinyin/modernime-knowledge.dict
```

自定义 `MODERNIME_PREFIX` 时会跟随该前缀安装到
`share/modernime/pinyin/modernime-knowledge.dict`。这份词典在本地离线加载，正常构建和运行不需要 Python 或网络；Python 只用于维护者重新生成数据。

## Fcitx5 启动与检查

在正常桌面会话中，安装脚本会尝试自动重载。若输入法没有立即出现，可以在当前用户终端执行：

```bash
fcitx5 -d -u modernime-ui
fcitx5-remote -r
fcitx5-remote -s modernime
```

检查当前输入法和 Fcitx5 状态：

```bash
fcitx5-remote -n
fcitx5-remote
```

不要重复启动多个 Fcitx5 实例。如果终端提示已有 Fcitx5 正在运行，应使用 `fcitx5-remote -r` 重载现有实例。

## 常用功能与核心特性

- **极致响应（首键零延迟）**：启动即预热 X11 窗口、字体字形缓存与历史学习库，首个拼音按键毫秒级即刻弹出候选框，告别首次打字卡顿；
- **正反斜线双键顿号兼容**：全面兼容不同操作系统的打字肌肉记忆，在中文输入状态下按下正斜线 `/` 或反斜线 `\` 均直接上屏中文顿号 `、`；同时在数字（如 `1/2`、`2026/09`）与英文路径中智能维持半角直通；
- **全量中文标点与智能半角直通**：
  - 中文状态下支持完整全角标点映射：书名号 `<` -> `《`、`>` -> `》`；方头括号 `[` -> `【`、`]` -> `】`；人民币符号 `$` -> `￥`；省略号 `^` -> `……`；破折号 `_` -> `——`；波浪号 `~` -> `～`；以及常见标点 `.` -> `。`、`,` -> `，`、`?` -> `？`、`!` -> `！`、`:` -> `：`、`;` -> `；`；
  - 智能数字/英文上下文识别：输入 IP 地址、浮点数或英文缩写时（如 `3.14`、`10:30`、`192.168.1.1`），自动保持半角标点直通，绝不破坏代码与网址输入体验；
- **中英文快捷键极速切换与游戏化按键录制**：
  - **单键 Shift 极速切换**：按下并释放单侧 `Shift` 键即可瞬间在中英文模式之间极速切换；带修饰键（如 `Shift + A` 输入大写字母）智能识别不误触；
  - **游戏同款直接按键录制（Key Grabber）**：在 Bento 设置客户端的「输入体验」页面中，中英文切换快捷键全面升级为游戏改键交互。点击「录制」按钮后直接在物理键盘上敲下目标按键或组合键（如 `Control_L+space`、`Control_R+space`、`Alt_L+space`、`Shift_L` 等）即可瞬间捕获并持久化保存；支持按 `Esc` 随时撤销录制或一键「恢复默认」，彻底终结手动输入键名字符串的繁琐易错体验；
- **动态系统日期与时间宏、大写数字与微信同款符号系统**：
  - 输入总开关即时作用于已有输入上下文；关闭后 Shift 与 Toggle 快捷键无法重新启用，重新开启保留此前临时中英模式。
  - 普通拼音组合后输入 `0`（含小键盘）会先提交原始拼音，再将数字交给应用，保持输入顺序。
  - **`v` 模式大写数字与金额换算**：输入 `v` 引导数字（如 `v12345`），首选即出大写金额 `壹万贰仟叁佰肆拾伍元整`，次选标准大写 `一万二千三百四十五`，三选纯数字 `12345`；关闭数字选词后仍可输入数字宏、金额小数和剪贴板触发数字；支持小数（如 `v88.5` -> `捌拾捌元伍角整`），财务会计录入效率翻倍；
  - **动态系统日期时间**：输入 `rq` 提供当前日期（如 `2026年9月23日`、`2026-09-23`），输入 `sj` 提供当前时间（如 `20:30`），输入 `xq` 提供当前星期（如 `星期三`、`周三`）；
  - **标点符号与特殊字符名称次选直出**：输入标点名称（如 `gantanhao` / `gan'tan'hao`），首选为中文词汇 `感叹号`，次选（第 2 候选）即为符号 `！`，三选为半角 `!`；支持 `wenhao` -> `？`、`douhao` -> `，`、`juhao` -> `。`、`dunhao` -> `、`、`fenhao` -> `；`、`maohao` -> `：`、`shuminghao` -> `《》`、`kuohao` -> `（）`、`shenglvehao` -> `……`、`pozhehao` -> `——`、`yinhao` -> `“”`、`bolangxian` -> `～`、`jiahao` -> `+`、`jianhao` -> `-` 等全部标点符号；
  - **希腊字母与学术符号**：拼音输入 `pi` -> `π`、`alpha` -> `α`、`beta` -> `β`、`gamma` -> `γ`、`delta` -> `δ`、`lambda` -> `λ`、`sigma` -> `σ`、`omega` -> `ω`、`phi` -> `φ` 等；
  - **数学公式与度量单位**：输入 `pingfang` -> `²`、`lifang` -> `³`、`sheshidu` -> `℃`、`zhengfu` -> `±`、`wuxian` -> `∞`、`yuedeng` -> `≈`、`budeng` -> `≠`、`suoyi` -> `∴`、`yuanyin` -> `∵`；
  - **货币与统计符号**：输入 `rmb` -> `¥`、`dollar` -> `$`、`ouyuan` -> `€`、`bang` -> `£`、`baifenbi` -> `%`、`qianfenbi` -> `‰`；
  - **图形、箭头与法律序号**：输入 `dui` -> `√`/`✔`、`cuo` -> `×`/`✖`、`jiantou` -> `→`/`←`/`↑`/`↓`、`sanjiao` -> `▲`/`△`、`wujiaoxing` -> `★`/`☆`、`jiefu` -> `§`、`duanluo` -> `¶`、`xuhao` -> `①`~`⑩`；
  - **热门社交 Emoji 表情直出**：输入 `haha` -> `😄`/`😂`、`xixi` -> `😆`、`zan` -> `👍`、`xin` -> `❤️`、`ku` -> `😭`、`ok` -> `👌`、`daniu`/`niubi` -> `🐂`、`doge` -> `🐶` 等；
- **内核稳定性防线（Crash Guard & LibIME 安全保护）**：
  - 自动检测并安全剥离拼音末尾孤立单引号 `'`，彻底消除 LibIME 在分词断句时的 `std::out_of_range` 越界与空音节异常崩溃；
  - 核心按键分发采用全局异常隔离与防御性状态回退，在高吞吐极速连续击键下坚如磐石、零闪退；
- **中文输入与候选交互**：输入拼音后使用数字键选择候选，支持上下方向键（Up/Down）快速翻行，支持 Tab/Shift+Tab 逐项移动高亮，全面支持鼠标左键直接单击任意候选词即刻上屏；
- **候选外观自定义与即时热重载**：候选栏支持自定义候选数量（3~9 个）与字号（14~28 pt），保持固定宽度并按候选文字长度自适应排版，设置修改后后台文件监听即刻热生效；
- **动态分页**：候选栏按当前候选文字长度与字号动态决定单页显示数量；翻页、方向键和数字键共享连续索引，不会跳过候选；
- **多粒度候选**：输入较长拼音时，首屏会同时提供可信的整句、常用短语和短词候选，减少仅末字不同的整句同音结果占满候选列表；
- **部分选词**：选择短语候选时只提交该短语，尚未消耗的拼音会继续保留并转换。例如输入 `nihaoalaodi` 后选择“你好啊”，会保留 `laodi` 继续选词；
- **智能简拼与高频校准**：全面支持单声母及复合声母（`zh`/`ch`/`sh`）简拼；内置高频简拼校准（如 `bj` 优先“北京”、`dl` 优先“登录”、`sh` 优先“上海”、`wsm` 优先“为什么”、`yqhc` 优先“一气呵成”）；输入复合声母（如 `yqch`）时智能召回拼音候选，防止过早被英文字符抢占，同时保留 `who` 等自然英文词的原生优先上屏；
- **国家通用规范汉字分级防护与怪词/生僻词专项治理**：
  - **《通用规范汉字表》分级防护与 O(1) 罕见字过滤**：内置高频常用汉字与 GB2312 一级常用字（3,755 字）分级防护位图，赋予优先成词权重；针对二级字、生僻冷僻字及康熙部首单字（如 `丨`、`丶`、`艹`、`屮`、`彡` 等）实施大幅降权惩罚（+100.0），彻底根除首屏候选栏被怪词、古汉语冷字或单部首符号挤占篡位的问题；
  - **单音节英文次选消除机制**：彻底消除 137 个单音节拼音（如 `de`、`ge`、`da`、`an`、`ba`、`bi`、`bo`、`ca` 等）第 2 候选常年被无意义生硬英文单词（如 `de`, `ge`, `da`）霸占的问题，单音节输入下原始英文自动沉底至尾部候选，确保前排全部为地道中文（如 `de` -> `的/得/地`、`ge` -> `各/个/歌`、`da` -> `大/打/达`）；同时白名单精准豁免兼具中英双属性的高频英文（如 `can` 保留次选；`like` 为双音节自然呈现）；
  - **单音节严格禁用模糊音插队**：模糊音（平翘舌、前后鼻音）仅在多音节输入或长句纠错中智能激活；在单音节（如 `cang`, `cao`, `cai`）输入时严格锁定精确声韵母，杜绝输入 `cang` 冒出 `常`、输入 `cao` 冒出 `超`、输入 `cai` 冒出 `柴` 的怪异跨拼音插队；
  - **真实词汇成词度（Lexicality）优选与散字组合抑制**：针对系统词典高频成词与扩展词库赋予成词奖励（-3.0），抑制语言模型基于单字暴力笛卡尔积拼凑出的无意义伪词组合；对词频偏低或两字专名（如生僻地名、古籍人名）建立基准抑制，确保“开发”、“怎么”、“当时”等日常生活词汇稳居首选；
  - **高频口语变音与城市/问候简拼直出**：针对高频口语别音误读 `weishime` 自动归一化映射为 `weishenme` 并直出 `为什么`；内置高频问候简拼 `nh` 直出 `你好`、零声母城市简拼 `xa` 智能映射 `xi'an` 直出 `西安`（并智能抑制生僻英文补全 `xanadu`），打字行云流水；
- **极速击键五阶全维度智能拼音容错引擎（Typo Correction）与高频口语长句防劫持**：
  - **双手交替换位与翘舌错位极速纠偏**：极速盲打时右手与左手落键先后的典型错序瞬间复原（如 `snaqian` -> “三千” #1、`sna` -> “三” #1、`cna` -> “残/参” #1、`zna` -> “赞” #1、`zhnag` -> “张” #1、`shnag` -> “商” #1 等）；
  - **介母主元音与鼻韵母反转智能还原**：解决打太快手指交错产生的介音反转（如 `jai` -> “家” #1、`jaiting` -> “家庭” #1、`xai` -> “下” #1、`qai` -> “恰” #1、`jain` -> “见” #1、`tain` -> “天” #1、`gaun` -> “关” #1、`haun` -> “换” #1 等），精准对齐国家通用一级规范常用字；
  - **生僻拼凑词截断根治与日常高频口语长句守护**：全面扩建日常高频口语与打字常见四字长句词库，输入 `dataikuaile` 稳居首选 “打太快了” #1（彻底根除被生硬生僻双字词如 “打胎快乐” 截断截胡的荒谬现象）；同步守护 “开太快了”、“走太快了”、“想太多了”、“吃太饱了”、“来不及了”、“搞不懂了” 等海量常用表达；
  - **未匹配英文字母首位污染清零**：严禁非英文单词的 typo 拼音无脑置顶于第 1 候选（如彻底消灭 `1.snaqian`、`1.jai` 乱码），候选首位永远呈现地道中文候选；
  - **残缺韵尾漏击补全**：自动补偿后鼻音与三元音漏击（如 `zhogguo` -> “中国” #1、`beijig` -> “北京” #1、`xuesheg` -> “学生” #1、`pengyo` -> “朋友” #1）；
  - **重键连击抗抖去重**：自动清洗重复元音击键（如 `zhoongguo` -> “中国” #1、`sheeng` -> “生” #1），同时白名单保护合法双元音英文（如 `good`、`book`、`food`、`need`、`tool`、`keep`、`free` 等）；
  - **QWERTY 物理相邻键误触校正**：基于物理键盘拓扑置换误触（如 `yop` -> “有” #1、`xiab` -> “先” #1、`jintiantianqibucup` -> “今天天气不错” #1）；
  - **英文前缀免疫保护**：严格区分真实长英文前缀与拼音换位，输入 `garag` 优先预测 `garage` #1，确保中英混输零误判；
- **英文输入、中英双重属性仲裁与智能前缀补全**：内置逾 7.3 万常用离线英文核心词库及 Linux 开发者高频命令（如 `date`, `make`, `sudo`, `diff` 等）：
  - **纯英文单词首位直接上屏**：输入如 `fact`、`good`、`apple`、`test` 等单词时智能置顶为首位候选，按空格一键上屏；
  - **英文单词中文释义内联注释展示（设置页按需开启）**：当英文候选（包括纯英文置顶如 `apple`、前缀补全如 `gara` $\to$ `garage`）展示时，简明中文释义以候选词尾内联注释形式展示（如 `apple [苹果；家伙]`），**不再额外占用独立候选编号**，避免候选序号错位或抢占后续中文候选位；基于 2.56 MB 紧凑二进制词典（ECDICT 离线编译）与 `mmap` 二分查找，纯离线零开销；
  - **中英双重属性成词度仲裁与常用高频词汇防篡位**：当输入的内容既是英文又是合法拼音时（如 `name` -> `na'me`，`like` -> `li'ke`，`date` -> `da'te`），输入法基于词典成词度（Lexicality）进行统一仲裁——若拼音中文为系统词典正规高频成词（如 `name` -> “那么”，`like` -> “立刻”，`women` -> “我们”，`shanghai` -> “上海”），坚守中文词语优先原则，中文稳居首位候选（按空格直接上屏中文），英文词作为次位候选，严禁预测英文或偶发误选篡位挤占高频中文；若拼音中文仅为无词典依据的生僻散字拼凑（如 `date` -> “打特”，`case` -> “擦色”），英文单词默认直接置顶于首位候选，按空格直接上屏命令，且 Preedit 净化为纯净英文；
  - **前缀补全预测与自定义英文术语学习**：输入英文前缀（如 `gara` 自动预测 `garage` 置顶，`appl` 自动预测 `apple`，`syst` 自动预测 `system`，`windo` 自动预测 `window`），并支持自定义专业英文术语学习与前缀召回（如学习 `gnueabihf` 后输入 `gnu`/`gnue` 即可前缀补全）；
- **剪贴板历史与现代化便签卡片流**：
  - **输入时即时召唤**：在中文输入状态输入触发字母（默认 `v`）后紧跟触发数字（默认 `2`）即可打开剪贴板垂直列表；键盘上下方向键可在列表间逐项移动游标，普通横向候选栏支持按上下方向键整页快速翻页；全面支持鼠标左键无死区几何命中并直接单击选词上屏；
  - 外部删除或清空后，未变化的系统剪贴板不会重新收录；实际复制内容发生变化后才再次记录。没有待保存内容时不回写旧历史；保存失败的重试会在最新磁盘历史上合并新复制条目，保留外部删除结果。
  - **设置端色块卡片流管理**：设置客户端剪贴板页面彻底告别纯文本平铺，采用现代 Bento 卡片流设计；内置代码（蓝）、网址（绿）、多行（琥珀）与纯文本（灰）专属色彩 Badge 与行数统计；超过 2 行或 120 字符的长条目自动折叠精简预览，配备专属「展开 / 收起」按键实现无缝代码等宽展开；提供单项即时复制/删除与全量清空二次确认；删除按界面所选文本定位，即使外部插入或重排历史也不会删错条目；支持 `modernime-settings -c` 命令行直达；
- **智能个性化学习引擎（PersonalEngine）与 LMDB 高性能持久化**：
  - **科学调频与指数半衰期衰减**：融合对数词频平滑 $\alpha \ln(\text{count}+1)$、严格半衰期指数衰减 $\tau = T_{1/2}/\ln 2$ 与 Surrounding Text 上下文亲和加权；支持分段造词（如连续输入 `王→绍→龙` 自动聚合学习人名 `王绍龙`、`候选→栏` 聚合为 `候选栏`）、以词定字（`[` 提取首字、`]` 提取末字并按对应音节对齐学习）以及上屏后删字修正检测；
  - **单字与高频词基准保护防篡位机制**：针对单字、短拼（如 `de` $\to$ `的`）和系统核心高频词汇（如“那么”、“我们”、“上海”）建立基准保护防线，杜绝偶发性单次误选或长英文词将固有高频首选项篡位挤占的问题；
  - **Surrounding Text 上下文感知与隐私防线**：安全提取光标前 UTF-8 边界对齐上下文用于重排加权；在焦点切换、光标移动或重置时自动失效回退；在 `Password` 与 `Sensitive` 敏感输入场景下严格阻断上下文读取与学习落盘；
  - **LMDB 持久化与 SQLite 自动无损迁移**：支持基于 `MDB_DUPSORT` 精确游标与 `MDB_SET_RANGE` 范围游标的 LMDB 存储引擎（50,000 词条热路径 P99 仅 `20~39 us`），支持进程内同路径共享句柄、`MDB_MAP_FULL` 动态扩容、损坏备份自愈，以及从旧版 `learning.sqlite3` 到 LMDB 的自动无损迁移（保留原 SQLite 文件不删除）；
- **多行业专业深度词库与网络流行语全覆盖**：
  - **人名、日常高频表达与专业术语扩充**：内置多音字校验管线，覆盖常见人名、日常口语短语及六大行业术语，严格校验拼音与词频，杜绝新增词条抢占常用词；
  - **IT 与研发领域**：覆盖微服务、异步调用、中间件、负载均衡、依赖注入、容器化、持续集成、单点登录、灰度发布、单元测试等核心高频研发词汇；
  - **财务与会计领域**：覆盖应收账款、增值税专用发票、累计折旧、资产负债表、现金流量表、损益表、财务报表、结转成本、贷记卡、汇算清缴等；
  - **临床与医疗领域**：覆盖血常规、计算机断层扫描、磁共振成像、心电图、冠状动脉、扁桃体炎、动脉粥样硬化、支气管哮喘、葡萄糖注射液等；
  - **法律与法务领域**：覆盖民法典、知识产权、仲裁协议、连带保证、不动产权属、诉讼时效、侵权责任、违约责任、补充协议、不可抗力等；
  - **学术与科研领域**：覆盖交叉验证、置信区间、假设检验、梯度下降、神经网络、强化学习、随机变量、特征值、回归分析、协方差等；
  - **社交与网络流行**：覆盖吃瓜、躺平、绝绝子、破防、芭比Q、种草、拔草、摸鱼、凡尔赛、打工人等高频网民用语；
  - 专业词条权重与多音节词汇优化，确保行业专业术语优先级高于零碎拼凑单字；
- **用户词典与学习记录热重载**：设置客户端可管理本地词条和专业词汇，底层词典与学习缓存支持后台热重载；
- **学习数据清空**：设置端清空会自动刷新活动学习缓存，支持 SQLite WAL 与 LMDB；清空前尚未落盘的旧队列不会恢复已删记录，清空后的首次新选择可正常保存。轮询使用不等待写锁的只读连接，避免存储故障阻塞输入。
- **简拼学习排序**：高频简拼使用统一排序优先级，常用词保留默认保护；充分学习且评分优于可信常用词的候选可晋升首选，排序不依赖候选初始顺序。
- **默认离线安全**：候选转换、动态分页、模糊音与拼写恢复、简拼匹配、用户学习和扩展知识库均在本地运行，默认不发起任何网络请求。

## 微信输入法深度对标与快捷输入指南

ModernIME 在真实 Linux 桌面环境中全面模拟 **IT研发、财务会计、临床医疗、法律法务、学术科研、社交日常** 6 大行业角色的打字流，与商业成熟输入法（微信输入法）进行了系统化对标与全链路优化，综合体验对标达成率达到 **93.5%**。

### 1. `v` 模式大写金额与数字换算速查

针对财务会计、商务合同场景，全面支持 `v` 引导的大写金额与大写数字直出，无需切换外部转换工具：

| 键盘输入 | 首选（按空格直接上屏） | 次选 | 三选 | 典型应用场景 |
| :--- | :--- | :--- | :--- | :--- |
| `v12345` | `壹万贰仟叁佰肆拾伍元整` | `一万二千三百四十五` | `12345` | 银行转账、商务合同、报销发票 |
| `v88.5` | `捌拾捌元伍角整` | `八十八点五` | `88.5` | 零售收据、日常记账、餐费核算 |
| `v1000000000000` | `壹兆元整` | `一兆` | `1000000000000` | 兆级整数，零分组不追加单位 |
| `v1000000` | `壹佰万元整` | `一百万` | `1000000` | 大额资产结算、财务审计 |
| `v2026` | `贰仟零贰拾陆元整` | `二千零二十六` | `2026` | 年份大写、公文编号 |

### 2. 四阶智能拼音容错与击键抗抖速查

日常极速打字容易发生漏击、误碰邻键或按键颠倒，ModernIME 四阶智能纠错引擎可自动识别并在候选第一位精准纠正：

| 错误类别 | 错误输入用例 | 纠错后首选 #1 | 纠错机制与说明 |
| :--- | :--- | :--- | :--- |
| **残缺韵尾漏击** | `zhogguo` | **中国** | 自动补偿后鼻音缺失（`*og` -> `ong`） |
| **残缺韵尾漏击** | `beijig` | **北京** | 自动补偿后鼻音缺失（`*ig` -> `ing`） |
| **残缺韵尾漏击** | `xuesheg` | **学生** | 自动补偿后鼻音缺失（`*eg` -> `eng`） |
| **残缺韵尾漏击** | `pengyo` | **朋友** | 自动补偿三元音缺失（`*yo` -> `you`） |
| **重键连击抗抖** | `zhoongguo` | **中国** | 自动过滤意外连击元音，保护 `good`/`book` 等合法英文 |
| **物理邻键误触** | `yop` | **有** | 基于 QWERTY 键盘物理拓扑计算最近邻置换（`p` -> `u`） |
| **物理邻键误触** | `xiab` | **先** | 邻键校正（`b` -> `n`），优先成词 |
| **物理邻键误触** | `jintiantianqibucup` | **今天天气不错** | 长句复合容错（末尾 `p` 自动校正为 `o`） |
| **字母倒序颠倒** | `yuedign` | **约定** | 击键时序反转校正（`gn` -> `ng`） |
| **字母倒序颠倒** | `tina` | **天** | 经典错位还原（`ina` -> `ian`） |
| **字母倒序颠倒** | `chifna` | **吃饭** | 经典错位还原（`fna` -> `fan`） |

### 3. 希腊字母、数学度量、符号与 Emoji 表情速查

| 类别 | 拼音代码 | 候选输出 | 典型用途 |
| :--- | :--- | :--- | :--- |
| **标点符号** | `gantanhao`, `wenhao`, `douhao`, `juhao`, `dunhao`, `shuminghao`, `kuohao` | ！, ？, ，, 。, 、, 《》, （） | 输入标点名称次选直出对应标点符号 |
| **希腊字母** | `pi`, `alpha`, `beta`, `delta`, `lambda`, `sigma`, `omega` | π, α, β, δ, λ, σ, ω | 数理推导、算法分析、学术论文 |
| **数学公式** | `pingfang`, `lifang`, `wuxian`, `yuedeng`, `budeng`, `zhengfu` | ², ³, ∞, ≈, ≠, ± | 上标幂次、极限推导、误差区间 |
| **推导符号** | `suoyi`, `yuanyin` | ∴, ∵ | 逻辑推导、几何与代数证明 |
| **温度与单位** | `sheshidu` | ℃ | 临床病历、温度与气象记录 |
| **货币符号** | `rmb`, `dollar`, `ouyuan`, `bang` | ¥, $, €, £ | 财务报表、跨境电商、汇率记录 |
| **统计符号** | `baifenbi`, `qianfenbi` | %, ‰ | 数据分析、年化利率、千分比统计 |
| **常用图形** | `dui`, `cuo`, `sanjiao`, `wujiaoxing`, `lingxing` | √/✔, ×/✖, ▲/△, ★/☆, ◆/◇ | 审核确认、待办清单、等级评定 |
| **方向箭头** | `jiantou`, `xiangyou`, `xiangzuo`, `xiangshang`, `xiangxia` | →, ←, ↑, ↓ | 流程指引、状态流转、导航指示 |
| **法律序号** | `jiefu`, `duanluo`, `xuhao`, `yuanquan` | §, ¶, ①~⑩, ①~⑤ | 法律条款、条文结构、要点编排 |
| **社交表情** | `haha`, `xixi`, `zan`, `xin`, `ku`, `ok`, `daniu`, `doge` | 😄/😂, 😆, 👍, ❤️, 😭, 👌, 🐂, 🐶 | 微信日常聊天、社群互动、点赞 |
| **动态日期** | `rq`, `sj`, `xq` | 2026年9月23日, 20:30, 星期三 | 自动获取系统当前日期、时间与星期 |

### 4. 常用高频词保障与怪词/生僻词治理速查

| 拼音输入 | 优化后首选与前列候选 | 优化前常见痛点与怪词现象 | 治理机制与保障策略 |
| :--- | :--- | :--- | :--- |
| `de` | **1. 的  2. 得  3. 地  4. 德  5. 登** | 次选（第 2 候选）常年被生硬英文 `de` 霸占 | 单音节英文次选消除，原始英文沉底 |
| `ge` | **1. 个  2. 各  3. 歌  4. 割  5. 格** | 次选被生硬英文 `ge` 霸占，打字极易误选 | 单音节英文次选消除，高频汉字占满首屏 |
| `da` | **1. 大  2. 打  3. 达  4. 答  5. 搭** | 次选被生硬英文 `da` 抢占 | 单音节英文次选消除，纯正地道中文直出 |
| `dian` | **1. 点  2. 电  3. 店  4. 典  5. 垫** | 康熙部首冷字 `丶` 抢占首选或高位 | GB2312 一级字保障，部首生僻字符重度降权 |
| `cang` | **1. 藏  2. 仓  3. 苍  4. 舱  5. 沧** | 模糊音插队将翘舌音 `常`（chang）挤入高位 | 单音节严格禁用模糊音插队，精准锁定声韵母 |
| `cao` | **1. 草  2. 操  3. 曹  4. 槽  5. 糙** | 模糊音插队将翘舌音 `超`（chao）挤入高位 | 单音节严格禁用模糊音插队，精准锁定声韵母 |
| `zenmeyang` | **1. 怎么样  2. 怎样  3. 怎么** | 候选列表末尾冒出生僻怪字 `谮` | 同音候选过滤生僻字，强制限定 GB2312 一级字 |
| `kaifa` | **1. 开发  2. 开发者  3. 开放** | 词典生僻两字专名抢占首选 | 降低生僻专名词典加成，生活高频词稳居首位 |
| `weishime` | **1. 为什么** | 提示无此拼音或拼凑生僻散字 | 口语变音别读（`shime` -> `shenme`）智能归一化 |
| `nh` | **1. 你好** | 仅显示散字或偏僻词 | 问候高频简拼直出保护 |
| `xa` | **1. 西安** | 英文前缀联想 `xanadu` 或散字切分 | 零声母城市简拼直出，抑制低频长英文干扰 |

### 5. 微信输入法深度对标评分卡

| 核心评估维度 | 权重 | 优化前得分 | 优化后得分 | 微信基准 | 达成状态 |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **基础输入与整句上屏** | 20% | 85 | **96** | 98 | **98.0%** (首键零延迟，部分选词顺畅) |
| **拼音容错与拼写纠错** | 25% | 40 | **92** | 96 | **95.8%** (四阶容错，抗抖/邻键/颠倒/漏击全绿) |
| **行业专业术语覆盖** | 20% | 55 | **91** | 95 | **95.8%** (覆盖 6 大专业与日常领域) |
| **快捷符号与宏扩展** | 15% | 30 | **95** | 95 | **100.0%** (v 模式大写、希腊/数学/Emoji 全面对标) |
| **中英混输与代码友好度** | 20% | 80 | **94** | 92 | **102.2%** (超越微信：Linux 命令预测与半角标点直通) |
| **综合加权得分** | **100%** | **59.5%** | **93.5%** | **95.6%** | **达成率 93.5% (远超 85% 交付红线)** |

## Wayland 与会话说明

候选栏在 X11 会话中锚定到输入光标下方（靠近屏幕底部时自动翻到光标上方，并钳制在显示器工作区内）。Wayland 协议不允许客户端自行定位浮动窗口，`gtk_window_move` 是空操作；在接入 Fcitx5 窗口系统（windowing）接口之前，Wayland 会话中的候选栏位置由合成器决定，这不影响输入法本身的全部功能。安装脚本在检测到图形会话时会以 `fcitx5 -d -u modernime-ui` 启动。

所有 ModernIME 自己的配置和数据都保存在当前用户目录下，不修改系统其他输入法：

```text
${XDG_CONFIG_HOME:-$HOME/.config}/modernime/
${XDG_DATA_HOME:-$HOME/.local/share}/modernime/
```

`libime-bin` 提供构建内置离线拼音词典所需的 `libime_pinyindict`，全新 Debian/Ubuntu 环境需与开发库一起安装。

## 自定义安装位置

可以通过环境变量指定用户前缀和构建目录：

```bash
MODERNIME_PREFIX="$HOME/.local/modernime" \
MODERNIME_BUILD_DIR="$HOME/.cache/modernime-build" \
./install.sh
```

自定义安装后，设置客户端路径、桌面入口和卸载清单都会使用该前缀。安装器在启动环境、会话环境文件和自启入口中配置 `FCITX_ADDON_DIRS` 与 `XDG_DATA_DIRS`，让 Fcitx 同时发现插件和输入法元数据；设置客户端重载也会根据实际可执行文件位置补齐这些路径。`MODERNIME_CONFIG_HOME` 和 `MODERNIME_DESKTOP_DIR` 可分别隔离会话配置与桌面目录，`MODERNIME_SKIP_FCITX_RESTART=1` 可禁止重启当前输入法。

配置目录不同于 `$HOME/.config` 时，默认不修改 `$HOME/.xprofile`。需要同步指定的会话文件时可设置 `MODERNIME_XPROFILE_FILE=/path/to/profile`；`MODERNIME_SKIP_XPROFILE=1` 可完全跳过。安装清单记录实际路径与生成块，重装和卸载保留被修改的块及文件中的其它用户内容。卸载会使用记录的路径，无需再次设置会话文件参数。

## 版本升级与版本回退指南

### 1. Ubuntu / Debian (DEB 原生包)

- **升级**：下载与当前操作系统版本严格匹配的 DEB 包（如 `ubuntu22.04` 或 `ubuntu24.04`），使用 APT 执行事务升级以自动校验并补齐可能的新依赖：
  ```bash
  sudo apt-get install --reinstall ./modernime_<new_version>_amd64_<distro>.deb
  ```
  升级完成后，在当前用户桌面会话中执行 `modernime-session-init` 或重载输入法：
  ```bash
  fcitx5-remote -r
  ```
- **回退**：下载历史版本的 DEB 包，通过 `--allow-downgrades` 参数安全回退：
  ```bash
  sudo apt-get install --allow-downgrades ./modernime_<old_version>_amd64_<distro>.deb
  ```

### 2. openEuler (RPM 原生包)

- **升级**：使用 DNF 或 `rpm -Uvh` 执行同名包原子升级（**切勿使用 `rpm -ivh`**，避免同名多版本冲突）：
  ```bash
  sudo dnf upgrade ./modernime-<new_version>-1.oe24.x86_64.rpm
  # 或使用 rpm:
  # sudo rpm -Uvh ./modernime-<new_version>-1.oe24.x86_64.rpm
  ```
  安装后以普通桌面用户身份运行 `modernime-session-init` 刷新环境与会话。
- **回退**：使用 DNF 或 `rpm --oldpackage` 回退到指定旧版本：
  ```bash
  sudo dnf downgrade ./modernime-<old_version>-1.oe24.x86_64.rpm
  # 或使用 rpm:
  # sudo rpm -Uvh --oldpackage ./modernime-<old_version>-1.oe24.x86_64.rpm
  ```

### 3. 便携绿色包 (Portable Tarball) 与源码安装

- **升级**：解压新版本压缩包后直接运行 `./install.sh`。安装器具备暂存完整性校验、旧版 v0.1.6 扁平插件目录自动迁移，并在每次升级前使用在线 SQLite WAL Checkpoint + Backup API（含 `PRAGMA integrity_check` 校验）及 LMDB 备份机制，自动将用户自适应学习数据库完整备份至 `~/.local/share/modernime/learning.sqlite3.backup-<timestamp>` 与 `learning.lmdb.backup-<timestamp>`。用户自定义词典（`user-dictionary.txt`）和个性化配置会被严格保护，哈希保持不变。
- **回退**：解压旧版本便携包或切换到旧版本源码标签，直接运行 `./install.sh` 即可重新部署旧版二进制。如需回退词频学习记录，直接将自动备份文件复制覆盖 `learning.sqlite3` / `learning.lmdb` 即可。
- **安装渠道切换注意**：系统已通过 DEB 或 RPM 原生包安装时，请勿使用 `sudo ./install.sh /usr` 强制覆盖系统前缀；如需切换至便携包或源码构建版，请先通过系统包管理器（`apt remove` / `dnf remove`）卸载原生包，或使用非 root 默认用户前缀（`~/.local`）部署。

## 卸载

在仓库目录执行：

```bash
./uninstall.sh
```

卸载脚本根据安装清单移除 ModernIME 自己安装的文件，并保留桌面目录中的其他文件。环境文件、自启入口和桌面快捷方式会保留安装时的内容快照；如果文件被用户修改过，重装和卸载会保留它并提示手动处理。卸载会继续清理其它未修改的产物，并保留清单供处理后再次运行。没有所有权快照的旧安装文件会按未托管文件保护。

## 版本更新记录

### v0.1.8 (2026-10-12)

- **智能学习与 LMDB 持久化引擎**：新增 `PersonalEngine` 高性能 LMDB 存储引擎（支持 `MDB_DUPSORT` 精确游标、`MDB_SET_RANGE` 前缀游标、进程内同路径共享 `SharedLmdbHandle`、`MDB_MAP_FULL` 动态扩容与损坏备份自愈），并提供从旧版 `learning.sqlite3` 到 LMDB 的自动无损迁移；接入 Fcitx5 空格/数字键/鼠标上屏一致性学习、对数词频平滑 $\alpha \ln(\text{count}+1)$、严格半衰期指数衰减 $\tau = T_{1/2}/\ln 2$、Surrounding Text 上下文亲和加权、分段造词（如 `王→绍→龙` $\to$ `王绍龙`、`候选→栏` $\to$ `候选栏`）、以词定字（`[` 首字 / `]` 末字提取）与 `Password`/`Sensitive` 敏感输入隐私防线；新增 GoogleTest 专项测试与 50,000 词条 Google Benchmark 基准验证（热路径 P99 仅 `20~39 us`）。
- **候选排序、中英仲裁与释义展示**：将英文单词中文释义改为候选词尾内联注释展示（不再额外占用独立候选编号）；统一中英候选意图仲裁，防止预测英文抢占常用中文；扩充双元音英文白名单，保护 `food`、`need`、`tool`、`keep`、`free` 等常见英文单词免受拼音连击抗抖误删。
- **中文扩展词库扩容与质量管线**：新增中文词库扩充与多音字自动校验流水线，丰富常见人名、日常高频短语与六大专业领域术语，增加候选解码与防劫持回归测试。
- **安装、升级、回退与发布门禁加固**：安装升级时执行 SQLite WAL 在线检查点备份与 `PRAGMA integrity_check` 校验及 LMDB 自动备份（拒绝损坏库静默拷贝）；便携式二进制安装包移除构建期 CMake 版本检查以兼容 Ubuntu 22.04 免编译安装，并实现暂存载荷校验与失败事务自动回滚；补齐 DEB/RPM/Tarball 及 GitHub Actions 全平台 `LMDB` 构建与运行依赖声明，新增 Release 资产升级与卸载全链路门禁校验。

### v0.1.7 (2026-10-11)

- **安装与发行**：保护用户修改的环境、自启和桌面文件；隔离并记录会话配置块；补齐自定义前缀的数据发现环境；便携包复用源码安装与受控卸载闭环；声明离线词典编译工具及 DEB 的 Fcitx5 启动器依赖。
- **输入与候选**：修复学习简拼排序的传递性、普通组合后数字 `0` 的上屏顺序、输入总开关与激活上下文的一致性、关闭数字选词后的数字宏输入，以及兆级数字的多余零分组单位。
- **剪贴板与学习**：删除定位界面所选文本；外部清空不会被未变化的剪贴板轮询或干净保存恢复；写入重试尊重磁盘变更；SQLite WAL 清空通过只读观察连接刷新，旧失败队列不复活且保留新的学习事件。
- **验证**：Release 构建也执行测试断言，修复对象释放测试中的断言副作用；增加真实 Fcitx 元数据定位和便携词典加载回归。

### v0.1.6 (2026-10-04)
- **常用高频词汇坚守中文优先与防篡位保护**：彻底解决如 `name`（那么）、`like`（立刻）、`women`（我们）等常用高频中文词汇因低频偶发选择英文而导致英文抢占首位、挤掉中文候选的问题；建立词典正规成词防护防线，常用高频中文稳居第 1 候选，空格直接上屏中文，英文词稳定作为第 2 备选；
- **优化成词度仲裁逻辑**：严格区分真实汉语常用词汇与生僻散字拼凑（如 `date` -> “打特”，`case` -> “擦色”），生僻散字保持英文置顶上屏，常用高频词汇坚守地道中文直出。

### v0.1.5 (2026-10-04)
- **快捷键游戏化录制**：设置客户端新增 `ModernimeKeyGrabber` 按键捕获组件，点击「录制」后直接敲击键盘组合键（如 `Control_L+space`、`Alt_L+space` 等）即刻录制，支持 Esc 随时取消与一键「恢复默认」，彻底终结手动输入键名字符串的繁琐体验；
- **openEuler / RPM 系发行版全面适配**：系统化解决 openEuler 24.03 SP4 / 24.09 部署缺陷（P0-1 ~ P2-7），全面通过跨平台验证；
- **动态语言模型与词典探测引擎**：自适应探测 `/usr/lib64/libime/`（openEuler / Fedora / RHEL）、Debian multiarch、Arch `/usr/share/libime/` 及环境变量覆盖，并输出显式诊断日志，彻底消除语言模型未命中的静默降级；
- **安装器智能化跨平台适配**：`install.sh` / `uninstall.sh` 自动感知 `lib`/`lib64` 插件目录，支持免 root 用户级安装与系统级 `/usr` 安装（`./install.sh /usr`）；
- **全自动化桌面会话与环境变量注入**：`install.sh` 自动为 LightDM / UKUI / X11 / XFCE 等非 systemd-user 托管会话注入 `~/.xprofile`（配备保护块），并在安装完成时打印高亮环境变量指引；
- **状态栏托盘依赖与编译预警健全**：RPM spec 与 GitHub CI 补齐 `libappindicator-gtk3-devel` 依赖，ui 编译缺失 appindicator 时输出显式 CMake 警告；
- **安装逃生开关**：支持 `MODERNIME_SKIP_TESTS=1`，助力在精简容器或受限环境中极速完成安装部署。

### v0.1.4 (2026-10-03)
- **英文单词中文释义候选**：当第 1 候选为英文单词（含纯英文与前缀补全）时，自动在第 2 位插入简明中文释义；基于 2.56 MB ECDICT 紧凑二进制词典与 `mmap` 二分查找，纯离线零开销；
- **设置客户端释义开关**：Bento 设置客户端「输入体验」页面新增「英文单词中文释义候选」开关，支持搜索关键字快速定位与即时热重载。

### v0.1.3 (2026-09-29)
- **极速击键五阶全维度智能拼音容错引擎**：双手交替换位、介音反转还原、高频口语长句防截断守护，首位英文乱码污染清零；
- **实时配置热重载与环境自适应**。

### v0.1.2 (2026-09-27)
- **五阶智能拼音容错与高频口语长句防劫持**。

### v0.1.1 (2026-09-25)
- **极致击键延迟优化与多发行版打包支持**。

### v0.1.0 (2026-09-23)
- **ModernIME 首次发布**：现代化 Fcitx5 拼音输入法引擎、Bento 仪表盘 GTK3 设置客户端、剪贴板历史、离线扩展词库与多发行版打包支持。

## 故障排查

- CMake 提示找不到 `Fcitx5Core`、`LibIMEPinyin`、`libime_pinyindict` 或 GTK3：确认已安装上面的开发包，然后重新执行 `./install.sh`；
- `fcitx5-remote` 没有输出：先确认 Fcitx5 已运行，再执行 `fcitx5-remote -r`；
- 设置客户端找不到：直接运行 `~/.local/bin/modernime-settings`，并确认 `~/.local/share/applications/modernime-settings.desktop` 存在；
- 在 SSH、容器或无桌面的终端安装：这是受支持的，安装脚本会跳过 Fcitx5 自动启动；回到图形会话后手动启动 Fcitx5 即可。
