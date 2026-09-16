# ModernIME

ModernIME 是一个面向 Linux 桌面的现代化 Fcitx5 拼音输入法，包含候选栏、用户习惯学习、用户词典、剪贴板入口，以及独立的 GTK3 设置客户端。具备首键零延迟响应、高频简拼智能校准、候选外观自定义与即时热重载等特性。

## 快速安装

以下命令适用于 Ubuntu/Debian。安装脚本只写入当前用户的前缀，默认是 `~/.local`，不需要用 `sudo cmake --install`。

```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsqlite3-dev libboost-dev \
    libfcitx5core-dev libfcitx5utils-dev fcitx5-modules-dev fcitx5 \
    libimecore-dev libimepinyin-dev \
    libgtk-3-dev libpango1.0-dev libayatana-appindicator3-dev

git clone git@github.com:sunshine134679/fcitx5-linux-pinyin-input-method.git
cd fcitx5-linux-pinyin-input-method
./install.sh
```

`install.sh` 会自动完成配置、编译、测试和安装，并且会：

- 安装 Fcitx5 输入法插件和候选栏 UI；
- 安装离线扩展拼音词典（成语、IT、医学、法律、地名等类别）；
- 安装 `modernime-settings` 设置客户端；
- 安装桌面菜单入口 `modernime-settings.desktop`；
- 在当前桌面目录生成 `modernime-settings.desktop` 快捷方式；
- 在检测到可用的图形和 DBus 会话时尝试重载并激活 ModernIME。

如果当前是在没有图形会话的终端、容器或 CI 中安装，Fcitx5 自动启动会被跳过，但编译和安装仍会正常完成。也可以明确关闭自动重载：

```bash
MODERNIME_SKIP_FCITX_RESTART=1 ./install.sh
```

## 启动设置客户端

安装后可以从应用菜单或桌面快捷方式启动，也可以直接运行：

```bash
~/.local/bin/modernime-settings
```

设置客户端和输入法插件在同一个仓库中，会随 `./install.sh` 一起构建和安装。若 `~/.local/bin` 不在 `PATH` 中，请使用上面的绝对路径。桌面菜单文件位于：

```text
~/.local/share/applications/modernime-settings.desktop
```

桌面快捷方式会放在系统报告的桌面目录；如果系统没有配置该目录，则使用 `~/Desktop`，中文桌面则兼容 `~/桌面`。

设置客户端包含六页：概览、输入体验、个人词典、剪贴板、智能学习、系统与诊断。

- 概览页集中显示运行状态、默认输入状态、切换快捷键，以及词典、剪贴板和学习记录数量；
- 输入体验页统一配置启用状态、默认中英文状态、切换快捷键、中文标点以及候选外观与排版；支持自由设定单页候选词数（3~9 个）与候选字号（14~28 pt），修改后即刻热重载生效，无需重启 Fcitx5；快捷键格式错误会在对应输入框就地提示；
- 个人词典页支持本地实时搜索、添加、编辑、删除、导入和导出；导入文件必须完整通过校验，失败不会覆盖现有词典；
- 剪贴板页可以选择、复制、删除、清空和刷新本地历史，清空操作需要确认；
- 智能学习页显示本地记录数量，清空前会自动创建并验证备份；
- 系统与诊断页分别显示 `fcitx5-remote`、Fcitx5、当前输入法和 ModernIME 的状态，并提供“刷新状态”和“重新加载 ModernIME”。

左侧搜索框只在客户端已有的设置名称和说明中进行本地搜索，选择结果会打开对应页面并定位控件；可按 `Ctrl+F` 聚焦搜索框。修改设置后，“应用”会保存并保持窗口打开（也可按 `Ctrl+Enter`），“保存并关闭”会保存后关闭；保存失败时草稿会继续保留。成功保存后，客户端会明确提示是否需要重新加载 ModernIME，完成重新加载后状态提示会同步更新。关闭含未保存修改的窗口时，可以选择继续编辑、放弃修改或保存并关闭。

ModernIME 托管数据保存在当前用户配置目录；词典导入与导出使用用户在文件选择器中指定的路径。第一阶段的设置客户端不会发起任何网络请求，也不包含联网入口或后台联网任务。修改配置或词典后，使用系统与诊断页的“重新加载 ModernIME”即可让运行中的 Fcitx5 重新读取配置（注：候选外观等核心排版配置已支持自动文件监听即时生效，无需手动重新加载）。

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

## 常用功能

- 极致响应（首键零延迟）：启动即预热 X11 窗口、字体字形缓存与历史学习库，首个拼音按键毫秒级即刻弹出候选框，告别首次打字卡顿；
- 中文输入：输入拼音后使用数字键选择候选，支持上下方向键（Up/Down）快速切页翻行，支持 Tab/Shift+Tab 逐项移动高亮，全面支持鼠标左键直接单击任意候选词即刻上屏；
- 候选外观自定义与即时热重载：候选栏支持自定义候选数量（3~9 个）与字号（14~28 pt），保持固定宽度并按候选文字长度自适应排版，设置修改后在输入循环中即刻热生效；
- 动态分页：候选栏按当前候选文字长度与字号动态决定单页显示数量；翻页、方向键和数字键共享连续索引，不会跳过候选；
- 多粒度候选：输入较长拼音时，首屏会同时提供可信的整句、常用短语和短词候选，减少仅末字不同的整句同音结果占满候选列表；
- 部分选词：选择短语候选时只提交该短语，尚未消耗的拼音会继续保留并转换。例如输入 `nihaoalaodi` 后选择“你好啊”，会保留 `laodi` 继续选词；
- 智能简拼与高频校准：全面支持单声母及复合声母（`zh`/`ch`/`sh`）简拼；内置高频简拼校准（如 `bj` 优先“北京”、`dl` 优先“登录”、`sh` 优先“上海”、`wsm` 优先“为什么”、`yqhc` 优先“一气呵成”）；输入复合声母（如 `yqch`）时智能召回拼音候选，防止过早被英文字符抢占，同时保留 `who` 等自然英文词的原生优先上屏；
- 拼音恢复：常见模糊音和拼写颠倒可智能恢复中文候选；
- 英文输入与智能预测补全：内置逾 7.3 万常用离线英文核心词库，当输入常用英文单词（如 fact、good、apple、test）时智能置顶为首位候选，敲击空格一键上屏；支持前缀预测补全（如输入 gara 自动预测 garage 置顶，appl 自动预测 apple，syst 自动预测 system，windo 自动预测 window），同时在次选保留原始输入，兼顾中英混输与高频补全；
- 剪贴板：在中文输入状态输入触发字母（默认 `v`）后紧跟触发数字（默认 `2`）即可打开剪贴板列表；触发字母本身会正常进入拼音组合，不会被吞掉或改写；支持键盘上下键浏览高亮，全面支持鼠标左键直接单击任意历史条目即刻上屏；
- 用户学习：候选选择会记录到当前用户的学习数据中，并用于后续排序；
- 用户词典：设置客户端可管理本地词条和专业词汇；
- 默认离线：候选转换、动态分页、模糊音与拼写恢复、简拼匹配、用户学习和扩展知识库均在本地运行，默认不发起网络请求。

## Wayland 与会话说明

候选栏在 X11 会话中锚定到输入光标下方（靠近屏幕底部时自动翻到光标上方，并钳制在显示器工作区内）。Wayland 协议不允许客户端自行定位浮动窗口，`gtk_window_move` 是空操作；在接入 Fcitx5 窗口系统（windowing）接口之前，Wayland 会话中的候选栏位置由合成器决定，这不影响输入法本身的全部功能。安装脚本在检测到图形会话时会以 `fcitx5 -d -u modernime-ui` 启动。

所有 ModernIME 自己的配置和数据都保存在当前用户目录下，不修改系统其他输入法：

```text
${XDG_CONFIG_HOME:-$HOME/.config}/modernime/
${XDG_DATA_HOME:-$HOME/.local/share}/modernime/
```

## 自定义安装位置

可以通过环境变量指定用户前缀和构建目录：

```bash
MODERNIME_PREFIX="$HOME/.local/modernime" \
MODERNIME_BUILD_DIR="$HOME/.cache/modernime-build" \
./install.sh
```

自定义安装后，设置客户端路径、桌面入口和卸载清单都会使用该前缀。

## 卸载

在仓库目录执行：

```bash
./uninstall.sh
```

卸载脚本根据安装清单移除 ModernIME 自己安装的文件，并保留桌面目录中的其他文件。如果某个文件被用户修改过，脚本会拒绝覆盖或删除它，并提示手动处理。

## 故障排查

- CMake 提示找不到 `Fcitx5Core`、`LibIMEPinyin`、`libime_pinyindict` 或 GTK3：确认已安装上面的开发包，然后重新执行 `./install.sh`；
- `fcitx5-remote` 没有输出：先确认 Fcitx5 已运行，再执行 `fcitx5-remote -r`；
- 设置客户端找不到：直接运行 `~/.local/bin/modernime-settings`，并确认 `~/.local/share/applications/modernime-settings.desktop` 存在；
- 在 SSH、容器或无桌面的终端安装：这是受支持的，安装脚本会跳过 Fcitx5 自动启动；回到图形会话后手动启动 Fcitx5 即可。
