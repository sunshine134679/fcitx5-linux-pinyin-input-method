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
  - 核心语言：C++20
  - 输入法框架：Fcitx5 (`libfcitx5core-dev`, `libfcitx5utils-dev`, `fcitx5-modules-dev`)
  - 拼音引擎内核：LibIME (`libimecore-dev`, `libimepinyin-dev`)
  - 设置客户端与渲染：GTK3 (`libgtk-3-dev`), Pango (`libpango1.0-dev`), Cairo
  - 存储与工具：SQLite3 (`libsqlite3-dev`), Boost (`libboost-dev`), LMDB (`liblmdb-dev`), CMake (>= 3.24)

---

## 2. 核心架构与模块分布 (Architecture)

```text
/home/wsl/ModernIME/
├── CMakeLists.txt              # 顶层 CMake 构建配置 (CMake >= 3.24, C++20)
├── CMakePresets.json           # 预设配置 (fcitx5-debug, asan-debug, strict-debug 等)
├── AGENTS.md                   # 安装生命周期维护准则与算法防退化铁律
├── README.md                   # 用户文档与快速安装指南
├── install.sh                  # 一键安装脚本（自动编译、测试、安装、创建桌面与自启入口）
├── uninstall.sh                # 一键干净卸载脚本（基于 manifest 严格对称回滚）
│
├── core/                       # 核心业务逻辑与数据存储（无 GUI/Fcitx 依赖）
│   ├── include/modernime/core/ # candidate_model, learning_store, personal_engine, settings, commit_event...
│   └── src/                    # learning_store.cpp, personal_engine.cpp, settings.cpp, candidate_ranker.cpp...
│
├── algorithm/                  # 拼音与候选生成算法模块
│   └── pinyin/                 # pinyin_candidate_provider, candidate_pipeline, candidate_features,
│                               # candidate_prefixes, candidate_mixer, recent_choice...
│
├── adapter/                    # 输入法框架适配层
│   └── fcitx5/                 # Fcitx5 引擎、状态机、事件处理 (inputmethod.cpp, engine.cpp)
│
├── ui/                         # 候选栏窗口排版与渲染组件
│   └── fcitx5/                 # candidate_bar_layout, candidate_bar_renderer, ui_addon
│
├── tools/                      # 独立工具与桌面设置客户端
│   └── settings/               # modernime-settings: settings_app, settings_window, settings_shell,
│                               # page_registry (统一页面解析与别名), data_controller, pages/ (八大功能页)
│
└── tests/                      # 自动化测试套件与门禁工具 (动态 Profile 管理)
    ├── verify_required_tests.py# 必需测试与质量门禁校验工具 (支持 full / core-only / storage-tsan)
    ├── core/                   # 核心、存储、候选排序回归测试
    ├── adapter/                # 适配器、故障恢复回归测试
    ├── ui/                     # UI 布局与渲染测试
    ├── settings/               # 设置客户端与 GTK 异步/导航测试
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

### ⑦ 四阶智能拼音容错与按键抗抖去重
- **四阶容错管道**：针对极速打字误碰，构建按键抗抖（`oo->o`，`ee->e`）、物理邻键拓扑（`yop->you`，`xiab->xian`）、音节倒序换位（`ina->ian`，`gn->ng`，`yuedign` -> 约定）及韵尾漏击补全（`*og->ong`，`*eg->eng`，`*ig->ing`，`*yo->you`）；
- **英文前缀白名单免疫**：在容错入口前严格校验是否为合法英文前缀（如 `garag`, `good`, `apple`），绝对不破坏合法英文前缀预测与中英混输。

### ⑧ 微信输入法深度对标宏系统与 `v` 模式大写金额换算
- **`v` 模式数字直通**：底层拦截 Fcitx5 键盘事件，`v` 引导下允许数字和小数点留在 Composition Buffer；
- **金融大写换算**：`v12345` 毫秒级转换为 `壹万贰仟叁佰肆拾伍元整`、`一万二千三百四十五`，`v88.5` 转换为 `捌拾捌元伍角整`；
- **丰富符号与 Emoji 宏**：集成希腊字母（`pi->π`, `alpha->α`）、数学单位（`pingfang->²`, `sheshidu->℃`）、图形箭头（`dui->√`, `jiantou->→`）、法务条文（`jiefu->§`）、带圈序号（`xuhao->①`）及热门 Emoji（`haha->😄`, `zan->👍`）。

### ⑨ 多行业核心词库与精选词库零成本优先权（Curated Zero-Cost Prior）
- **痛点**：LibIME 语言模型偏好单字高频统计，容易将行业专业词（如 `微服务`、`血常规`、`应收账款`）拆分为单字碎片组合（如 `为服务`、`学常规`）；
- **解决机制**：在 `dictionary_prior.h` 中为多字专业词（`!singleCharacter` 且 `cost >= 0.0F`）赋予基础置信度优先权（Base Bonus = 1.0），确保 IT、财务、医疗、法律、学术等六大领域专业词条稳居首选。

---

## 4. 用户的最高准则与工程铁律 (Golden Rules)

> **这是用户反复着重强调的原则，任何接手的 AI 必须严格执行：**

### 🚨 铁律一：每次涉及较大更改，必须同步维护“从 GitHub 拉取直接安装”全流程
已正式写入根目录 `AGENTS.md`：
1. **依赖对齐**：凡引入新头文件或库，必须同步在 `README.md` 的 `sudo apt install` 与 `CMakeLists.txt` 中更新，绝不允许存在本地未声明的隐式依赖；
2. **离线自包含**：所有扩展词库与数据必须随仓库内置或离线自动生成，严禁让 `./install.sh` 依赖外部不可靠的实时网络下载；
3. **安装卸载对称闭环**：`install.sh` 创建的桌面快捷方式、环境变量、autostart 和 manifest，必须能被 `./uninstall.sh` 干净、无残留地全部回滚；
4. **沙箱回归验证**：每次较大变更推送到远程前，必须执行自动化沙箱验证脚本（自动创建隔离环境并验证完整安装与卸载闭环，严禁直接在 `/tmp` 下裸跑无参脚本）：
   ```bash
   ./tests/verify_fresh_clone.sh
   ```
   *防坑警示：`install.sh` 与 `uninstall.sh` 已内置防误删保护，若在 `/tmp` 或 `/var/tmp` 路径下执行且未显式指定 `MODERNIME_PREFIX`，将直接拒绝执行并报警，防止误清宿主用户真实环境配置。*

### 🚨 铁律二：远程机维护红线
在清理或维护远程主机（`192.168.5.150`）时：
- ❌ **绝对不要触碰用户的 `~/.cache/vscode-cpptools` 目录**（该目录内包含用户 17 GB 的项目符号跳转索引库，删除后重建极其繁杂）；
- ❌ **绝对不要触碰用户的 `/home/wsl/Linux` 目录**（这是用户的 IMX6ULL 嵌入式开发与内核开发专属工作区）；
- ❌ **绝对不要在未确认的情况下清理非缓存目录**。

### 🚨 铁律三：测试全绿与门禁核验方可交付
- 仓库支持按 Profile 动态注册测试套件：
  - **Full Profile**（`MODERNIME_REQUIRE_FULL_TESTS=ON` / 预设 `fcitx5-debug`）：包含 74+ 个测试（涵盖基础逻辑、拼音引擎、标点、GTK 契约、日常输入模拟、安装运行及 Benchmark 性能门禁）。无物理显示环境下，GTK/UI 测试通过 `SKIP_RETURN_CODE 77` 明确跳过，严禁伪造通过。
  - **Core-only Profile**（`MODERNIME_BUILD_LIBIME_PINYIN=OFF`）：包含 25+ 个基础核心测试。
  - **Storage-TSAN Profile**：专用于并发与生命周期线程安全验证。
- 任何代码改动必须执行 CTest 并经由门禁工具校验：
  ```bash
  # 1. 导出测试注册列表
  ctest --test-dir build/fcitx5-debug --show-only=json-v1 > build/fcitx5-debug/registered_tests.json

  # 2. 运行 Debug 测试套件并导出 JUnit 执行证据 (注意：--output-junit 使用明确绝对路径以防止与 --test-dir 相对路径重复嵌套)
  ctest --test-dir build/fcitx5-debug --output-junit "$PWD/build/fcitx5-debug/junit.xml" --output-on-failure

  # 3. 校验必需测试注册状态（仅注册检查）
  python3 tests/verify_required_tests.py --registered build/fcitx5-debug/registered_tests.json --profile full --stage final

  # 4. 校验必需测试实际执行证据（执行检查）
  python3 tests/verify_required_tests.py --registered build/fcitx5-debug/registered_tests.json --profile full --stage final --junit "$PWD/build/fcitx5-debug/junit.xml"

  # 5. 独立 Release 性能基准门禁 (G3/G7，Debug 目录不得充当 Release 性能基准)
  # 显式 Release 配置与编译（原计划 G3/G7，不依赖额外 preset 抽象）：
  # cmake -S . -B build/fcitx5-release -DCMAKE_BUILD_TYPE=Release -DMODERNIME_BUILD_FCITX5=ON -DMODERNIME_BUILD_LIBIME_PINYIN=ON -DMODERNIME_BUILD_SETTINGS=ON -DMODERNIME_BUILD_TESTS=ON -DMODERNIME_REQUIRE_FULL_TESTS=ON
  # cmake --build build/fcitx5-release -j$(nproc)
  # ctest --test-dir build/fcitx5-release -R modernime_personal_engine_benchmark --output-junit "$PWD/build/fcitx5-release/release-junit.xml" --output-on-failure

  # 6. 组合 Debug 功能验证与 Release 性能基准执行证据校验
  # python3 tests/verify_required_tests.py --registered build/fcitx5-debug/registered_tests.json --profile full --stage final --junit "$PWD/build/fcitx5-debug/junit.xml" --junit "$PWD/build/fcitx5-release/release-junit.xml"
  ```
- 必须确保所有必需测试 **100% Passed（或白名单 Skipped）** 且 0 Failed。

---

## 5. 常用开发与调试命令速查 (Command Cheatsheet)

### ① 编译与自动化测试
```bash
# 进入工作区
cd /home/wsl/ModernIME

# 使用预设编译 (Debug 构建用于日常开发与契约测试)
cmake --preset fcitx5-debug
cmake --build --preset fcitx5-debug -j$(nproc)

# 运行全量测试套件并执行门禁校验 (区分注册检查与实际执行证据检查)
ctest --test-dir build/fcitx5-debug --show-only=json-v1 > build/fcitx5-debug/registered_tests.json
ctest --test-dir build/fcitx5-debug --output-junit "$PWD/build/fcitx5-debug/junit.xml" --output-on-failure
python3 tests/verify_required_tests.py --registered build/fcitx5-debug/registered_tests.json --profile full --stage final --junit "$PWD/build/fcitx5-debug/junit.xml"

# 独立 Release 性能基准门禁 (引用原计划 G3/G7 显式 Release 配置，Debug 构建不用于性能数据发布)
# cmake -S . -B build/fcitx5-release -DCMAKE_BUILD_TYPE=Release -DMODERNIME_BUILD_FCITX5=ON -DMODERNIME_BUILD_LIBIME_PINYIN=ON -DMODERNIME_BUILD_SETTINGS=ON -DMODERNIME_BUILD_TESTS=ON -DMODERNIME_REQUIRE_FULL_TESTS=ON
# cmake --build build/fcitx5-release -j$(nproc)
# ctest --test-dir build/fcitx5-release -R modernime_personal_engine_benchmark --output-junit "$PWD/build/fcitx5-release/release-junit.xml" --output-on-failure
# 组合 Debug 与 Release JUnit 校验:
# python3 tests/verify_required_tests.py --registered build/fcitx5-debug/registered_tests.json --profile full --stage final --junit "$PWD/build/fcitx5-debug/junit.xml" --junit "$PWD/build/fcitx5-release/release-junit.xml"
```

### ② 一键安装与重新加载
```bash
# 本地一键安装（自动安装插件、设置端、词库、桌面入口）
./install.sh

# 如果已在图形桌面中，平滑重载 Fcitx5 输入法守护进程
fcitx5-remote -r
fcitx5-remote -s modernime

# 启动设置客户端（支持参数如 --candidates / -k, --clipboard / -c 等）
~/.local/bin/modernime-settings
```

### ③ 本地分支提交与协同规范
```bash
# 检查当前改动状态，确认无未受控文件
git status

# 精确审查差异与添加受控文件（严禁 git add -A / git add .）
git diff <file>
git add <file1> <file2>

# 提交变更
git commit -m "feat/fix: 详细说明"

# 注意：并行开发必须经由独立 worktree / 分支与协调者验收门禁，严禁直接 git push origin main！
```

---

## 6. 后续可选演进路线 (Future Roadmap)

如果后续用户需要继续扩展功能，建议的优先级方向：
1. **Wayland 深度集成**：目前候选框在 X11 下像素级完美锚定光标；Wayland 下依赖合成器默认位置，未来可考虑接入 `zwp_input_method_v2` 或 layer-shell 协议进行原生浮动定位。
2. **云端/局域网 WebDAV 词库同步**：为个人词典与自学习模型提供可选的本地 WebDAV / 私有云备份与多设备同步。
3. **多样化主题预设**：在设置客户端输入体验页中增加一套“浅色雅致 / 深色酷黑 / 赛博朋克”的主题预设切换。
4. **高级输入模式扩展**：双拼方案（微软双拼、小鹤双拼、自然码）与模糊音配置（平翘舌、前后鼻音可选开关）。
