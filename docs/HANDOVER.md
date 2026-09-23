# ModernIME 项目开发移交与核心记忆档案 (AI Handover Document)

> **本文档专为接手 ModernIME 项目的下一位 AI / 工程师编写**。汇总了本项目的全量技术架构、业务逻辑、重大避坑防线、用户特定工程铁律以及最佳开发测试规范，帮助后续接手者实现**零磨合、零降级、无缝继续开发**。

---

## 1. 项目全貌与基础设施 (Project Profile)

- **项目名称**：ModernIME (现代化 Linux 桌面 Fcitx5 拼音输入法 + GTK3 设置端)
- **开发目标**：面向现代 Linux 桌面（X11 / Wayland），提供首键零延迟、现代 Bento 美学、高度智能词频调频与中英双属性混合打字的顶级中文输入法。
- **远程开发机**：Ubuntu 24.04 on WSL / Linux
  - **登录方式**：`ssh wsl@192.168.5.150`
  - **工作区绝对路径**：`/home/wsl/ModernIME`
  - **默认分支**：`main`（当前同步至最新提交 `8b8a99f`）
- **远程 GitHub 仓库**：
  - HTTPS: `https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method.git`
  - SSH: `git@github.com:sunshine134679/fcitx5-linux-pinyin-input-method.git`
- **核心技术栈**：
  - 核心语言：C++17
  - 输入法框架：Fcitx5 (`libfcitx5core-dev`, `libfcitx5utils-dev`, `fcitx5-modules-dev`)
  - 拼音引擎内核：LibIME (`libimecore-dev`, `libimepinyin-dev`)
  - 设置客户端与渲染：GTK3 (`libgtk-3-dev`), Pango (`libpango1.0-dev`), Cairo
  - 存储与工具：SQLite3 (`libsqlite3-dev`), Boost (`libboost-dev`), CMake (>= 3.20)

---

## 2. 核心架构与模块分布 (Architecture)

```text
/home/wsl/ModernIME/
├── CMakeLists.txt              # 顶层 CMake 构建配置
├── CMakePresets.json           # 预设配置 (fcitx5-debug, asan-debug, strict-debug 等)
├── AGENTS.md                   # 必须严格遵守的从 GitHub 克隆直接安装的维护规范
├── README.md                   # 用户文档与快速安装指南
├── install.sh                  # 一键安装脚本（自动编译、测试、安装、创建桌面与自启入口）
├── uninstall.sh                # 一键干净卸载脚本（基于 manifest 严格对称回滚）
│
├── src/                        # 输入法核心插件源码
│   ├── engine/                 # 核心拼音引擎
│   │   ├── pinyin_engine.cpp   # 顶层按键流分发、候选词生成、异常隔离防线
│   │   ├── punctuation_mapper  # 全量中文标点符号映射与数字/英文半角智能直通
│   │   ├── learning_store      # 本地自学习调频引擎 (幂律调频算法，最大权重 180)
│   │   ├── english_dictionary  # 7.3 万常用英文单词与 Linux 高频命令词库、词频仲裁
│   │   └── dynamic_macros      # 动态系统日期宏 (rq -> 2026-09-22, sj -> 时间, xq -> 星期)
│   ├── ui/                     # 候选栏窗口与渲染组件
│   │   ├── candidate_window    # GTK3/Cairo 无边框自适应浮动窗口，首键预热防卡顿
│   │   ├── renderer            # Cairo + Pango 手绘高精渲染，字号/行距/高亮自适应
│   │   └── config_monitor      # inotify 监听 settings.json，外观变更毫秒级热生效
│   └── data/                   # 离线扩展知识库 (成语、IT、医学、法律等数十万离线词典)
│
├── tools/settings/             # 独立现代化设置客户端 (modernime-settings)
│   ├── modernime_settings_app  # GTK3 应用程序入口，Bento 仪表盘布局
│   ├── pages/                  # 六大功能页：概览、输入体验、个人词典、剪贴板、智能学习、系统与诊断
│   └── theme/                  # 纯净深空灰/石墨黑 CSS，彻底清除了 Adwaita 伪类绿色残影
│
└── tests/                      # CTest 自动化测试套件 (48 个全量用例，必须保持 100% 通过)
    ├── test_punctuation.cpp    # 正反斜线顿号、全角标点、数字直通测试
    ├── test_learning_store.cpp # 调频、置顶与英文调频学习测试
    ├── test_pinyin_daily_simulation.cpp # 日常高频打字模拟与优先级压测
    ├── test_install_runtime    # 端到端全新安装与环境自启测试
    └── ...
```

---

## 3. 核心设计要点与重大避坑经验 (Crucial Pitfalls & Defenses)

接手该项目的 AI 必须深入理解以下已经解决的血泪教训，切勿在重构时无意回退：

### ① LibIME 孤立单引号 `'` 段错误崩溃防线 (Crash Guard)
- **问题现象**：在快速极速打字、删改拼音或纠错重组时，拼音尾部可能出现孤立的隔音单引号（如 `yue'`、`ni'`）。LibIME 在做分词断句切分时会触发内部空音节异常或 `std::out_of_range`，直接导致 Fcitx5 进程发生 Segmentation Fault (核心转储) 闪退。
- **防御机制**：
  1. 在进入 LibIME 前置切分前，必须强制剥离末尾的孤立 `'`；
  2. 在 `PinyinEngine::process_key` 和候选项构造全流程加上防御性 `try-catch` 异常隔离层，发生异常时安全降级为原始英文字符上屏，确保在任何极端吞吐击键下绝对不闪退。

### ② 顿号双键兼容策略（正斜线 `/` 与反斜线 `\`）
- **核心逻辑**：无论用户习惯于 Windows 布局（习惯打 `\`）还是习惯于某些标准拼音输入法（习惯打 `/`），**在中文输入状态下按下 `/` 或 `\` 均必须直接上屏中文顿号 `、`**。
- **智能半角直通**：
  - 如果前面紧跟数字（如 `1/2`、`2026/09/22`）或处于纯英文上下文，必须智能直通半角符号，不得破坏网址、日期或代码路径。

### ③ 成对主键盘翻页与单键 Shift 切换
- **翻页**：除了 `PageUp / PageDown`，全面支持主键盘成对快捷键 `-`（上一页）与 `=`（下一页），双手无需移出主键盘区；
- **单键 Shift**：单击并释放单侧 `Shift` 键实现毫秒级中英文模式瞬切；若是组合键（如 `Shift + A` 输入大写字母），则自动识别并忽略，绝不触发误切。

### ④ 设置客户端（ModernIME Settings）去绿与 Bento 质感
- **GTK/Adwaita 伪类刺眼绿色问题**：在 Ubuntu 默认 Adwaita 主题下，GtkSwitch 开关、导航侧边栏以及 `.suggested-action` 按钮在 `:hover`、`:active`、`:selected`、`:focus` 等状态下默认会渲染出刺眼的亮绿色残影与粗边框。
- **彻底根治方案**：在 `settings_window.css` 中，针对所有的伪类组合（尤其是 `:backdrop`、`:active`、`:selected`、`:focus`、`outline`）进行了全局强制重写，统一使用精致高对比的深空灰/石墨黑（`#0F172A`, `#1E293B`, `#334155`）与柔和蓝宝石点缀，保持绝对统一的高级质感。
- **候选框 Cairo 联动**：设置端修改字号（14~28 pt）或候选数（3~9 个）时，底层候选窗口通过 inotify 监听实时重新排版，无需重启 Fcitx5。

### ⑤ 自适应学习单字基准保护与防偶发篡位
- **核心逻辑**：针对单字和系统核心高频词建立稳固的基准保护锚点。单次偶发性的冷门词选择绝不直接将原有高频首选词篡位，只有连续多次稳定输入才会跃迁至首位，兼顾灵敏调频与防误触鲁棒性。

### ⑥ 剪贴板历史色块卡片流与长条目智能折叠
- **界面现代化**：将剪贴板历史全面重构为便签式卡片流（Bento Cards），依据内容智能分类并展示专属徽标（代码/网址/多行/文本）；
- **防占屏与展开交互**：超过 2 行或 120 字符的长内容默认折叠 2~3 行精简预览，配备专属「展开 / 收起」按键实现无缝代码等宽展开；提供单项即时复制/删除，并支持 `--clipboard` / `-c` 命令行快捷直达。

---

## 4. 用户的最高准则与工程铁律 (Golden Rules)

> **这是用户反复着重强调的原则，任何接手的 AI 必须严格执行：**

### 🚨 铁律一：每次涉及较大更改，必须同步维护“从 GitHub 拉取直接安装”全流程
已正式写入根目录 `AGENTS.md`：
1. **依赖对齐**：凡引入新头文件或库，必须同步在 `README.md` 的 `sudo apt install` 与 `CMakeLists.txt` 中更新，绝不允许存在本地未声明的隐式依赖；
2. **离线自包含**：所有扩展词库与数据必须随仓库内置或离线自动生成，严禁让 `./install.sh` 依赖外部不可靠的实时网络下载；
3. **安装卸载对称闭环**：`install.sh` 创建的桌面快捷方式、环境变量、autostart 和 manifest，必须能被 `./uninstall.sh` 干净、无残留地全部回滚；
4. **沙箱回归验证**：每次较大变更推送到远程前，必须在临时目录完整验证一次克隆与安装（需显式传入配置与桌面隔离目录）：
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
   rm -rf /tmp/test-fresh-clone /tmp/test-prefix
   ```

### 🚨 铁律二：远程机维护红线
在清理或维护远程主机（`192.168.5.150`）时：
- ❌ **绝对不要触碰用户的 `~/.cache/vscode-cpptools` 目录**（该目录内包含用户 17 GB 的项目符号跳转索引库，删除后重建极其繁杂）；
- ❌ **绝对不要触碰用户的 `/home/wsl/Linux` 目录**（这是用户的 IMX6ULL 嵌入式开发与内核开发专属工作区）；
- ❌ **绝对不要在未确认的情况下清理非缓存目录**。

### 🚨 铁律三：测试全绿方可交付
- 仓库内包含 48 个全量测试（涵盖基础逻辑、拼音引擎、标点、GTK契约、日常输入模拟、安装运行测试）。
- 任何代码改动必须执行 `ctest --test-dir build/fcitx5-debug --output-on-failure`，必须确保 **48/48 全部 Passed**。

---

## 5. 常用开发与调试命令速查 (Command Cheatsheet)

### ① 编译与自动化测试
```bash
# 进入工作区
cd /home/wsl/ModernIME

# 使用预设编译
cmake --preset fcitx5-debug
cmake --build --preset fcitx5-debug -j$(nproc)

# 运行全量测试套件 (需 100% 通过)
ctest --test-dir build/fcitx5-debug --output-on-failure
```

### ② 一键安装与重新加载
```bash
# 本地一键安装（自动安装插件、设置端、词库、桌面入口）
./install.sh

# 如果已在图形桌面中，平滑重载 Fcitx5 输入法守护进程
fcitx5-remote -r
fcitx5-remote -s modernime

# 启动设置客户端
~/.local/bin/modernime-settings
```

### ③ 提交与远程推送
```bash
git status
git diff
git add -A
git commit -m "feat/fix: 详细说明"
git push origin main
```

---

## 6. 后续可选演进路线 (Future Roadmap)

如果后续用户需要继续扩展功能，建议的优先级方向：
1. **Wayland 深度集成**：目前候选框在 X11 下像素级完美锚定光标；Wayland 下依赖合成器默认位置，未来可考虑接入 `zwp_input_method_v2` 或 layer-shell 协议进行原生浮动定位。
2. **云端/局域网 WebDAV 词库同步**：为个人词典与自学习模型提供可选的本地 WebDAV / 私有云备份与多设备同步。
3. **多样化主题预设**：在设置客户端输入体验页中增加一套“浅色雅致 / 深色酷黑 / 赛博朋克”的主题预设切换。
4. **高级输入模式扩展**：双拼方案（微软双拼、小鹤双拼、自然码）与模糊音配置（平翘舌、前后鼻音可选开关）。
