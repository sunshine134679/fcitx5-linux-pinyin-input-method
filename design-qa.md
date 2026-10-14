# 客户端设计核对

日期：2026-10-14。分支：`codex/client-design`。基线：`f49606a`。

## 设计依据与截图

- 整体布局与色调：`/home/wsl/ModernIME/.coordination/design/2026-10-13-client/layout-1.png`。
- 紧凑设置行与数量下拉：`/home/wsl/ModernIME/.coordination/design/2026-10-13-client/layout-3.png`。
- 图 2 的独立试打面板作为备选，本次采用图 1 的顶部静态外观预览。
- 原客户端依据：同目录 `current-client.png`。
- 实际 GTK 渲染：`build/design/candidates-dirty.png`；默认设置：`build/design/candidates-final.png`；悬停状态：`build/design/candidates-hover.png`；小窗口：`build/design/candidates-minimum.png`。
- 完整并排对比：`build/design/comparison-final.png`。设置行局部对比：`build/design/comparison-controls.png`。上述截图位于本分支专属工作区 `/home/wsl/.codex/worktrees/client-design/ModernIME/` 下。

参考图 1、3 都为 1481×1062。实际客户端内容区为 980×720，密度为 1；最小窗口为 720×520。比较时参考图保持比例缩放至 980×703，置于 980×720 的白色画布，内容从左上对齐；忽略其余 17 像素的窗口高度差。截图仅绘制实际 GTK 客户区，未将系统标题栏当作产品内容。

主对比状态：候选与外观页面，草稿每页 5 个、字号 20 pt、数字选词开启、英文释义关闭、存在未保存修改。参考图的 9 个选项和固定 5 个示例，与实现的实时数量预览不同；默认 9 个的实际效果另见 `candidates-final.png`，超出可见宽度时可横向滚动。

## 核对结果

无未处理的 P0/P1/P2 差异。

| 核对面 | 实际结果与有意调整 |
| --- | --- |
| 字体与层级 | Noto Sans / Noto Sans CJK SC / 系统 sans-serif 回退；品牌 24 px、标题 26 px、设置名称 15 px、说明 12.5 px。字号预览使用实际 Pango 点数，修复原 `font='20pt'` 标记未正确指定字号的问题。 |
| 布局与间距 | 220 px 浅灰侧栏，主内容两侧 24 px 留白，分组内设置行按分隔线紧凑排列，底部固定操作区。980×720 可完整显示外观预览与主要四项设置；720×520 可滚动访问内容与侧栏，保存区始终可见。 |
| 色彩 | 强调色 `#1262ff`、侧栏/预览背景 `#f6f7f9`、内容白色、边框 `#dfe4ec`、说明 `#657184`。说明文字比参考图略深以提高可读性；选中导航为浅蓝，开关和主要按钮为蓝色，系统橙色主题不再改变产品色调。 |
| 图像与图标 | 复用 GTK 符号图标，候选外观使用显示器图标，信息提示使用标准信息图标；没有新增图片下载、图标库或系统依赖。 |
| 文案与内容 | 保留实际产品的八个页面名称和功能范围；版本取构建版本。数量改用图 3 的 3–9 下拉，字号使用可键盘操作的原生 GTK 数值控件。导航设置单独保留，避免只做视觉后隐藏已有功能。 |
| 操作状态 | 已查看未修改、未保存、禁用、主按钮悬停及小窗口；搜索定位、焦点回退、Tab 顺序、保存和放弃修改沿用真实模型。静态外观预览不记录学习；设置写入仍由保存按钮触发。 |

其余七页实际渲染见 `build/design/{overview,input,dictionary,clipboard,learning,updates,diagnostics}-final.png`。检查了标题、卡片、说明、空状态与底部操作区，未发现重叠或保存按钮裁切。

## 比较与修复历史

1. 第一轮 `comparison-1.png` 发现侧栏约 330 px，内容被挤压。测量发现导航子控件的 `hexpand` 向父容器传播；显式关闭侧栏横向扩展并设为 220 px。第二轮 `comparison-2.png` 确认比例恢复。
2. 第二轮微调品牌字重、导航/设置名称字号与预览留白；最终完整及局部并排对比见 `comparison-final.png`、`comparison-controls.png`。
3. 独立代码审查发现普通按钮 hover 规则覆盖主要动作的蓝色背景，导致白字浅灰底。增加优先级更高的主按钮 hover/active 规则，保留导航浅蓝与禁用状态；复查通过。`candidates-hover.png` 及实际 GTK 样式读取确认悬停背景为 RGB(18, 98, 255)。
4. 补充 Tab 顺序实测，确认按可见分组依次访问数字选词、释义、方向键和翻页；源码焦点列表同时按视觉顺序整理。

## 验证

- `cmake --preset fcitx5-debug && cmake --build --preset fcitx5-debug -j4`：通过。
- `DISPLAY=:1 XAUTHORITY=/run/user/1000/gdm/Xauthority GTK_IM_MODULE=gtk-im-context-simple ctest --test-dir build/fcitx5-debug --output-on-failure`：65 Passed、0 Skipped、0 Failed。
- 审查后的样式修复与焦点列表整理再次构建，并重验 GTK、窗口/组件契约与候选页相关 7 项：全部通过。新增运行时检查覆盖隐藏控件恢复、焦点定位、3/9 个预览、28 pt 实际字号、关闭编号以及 Tab 顺序。
- `env -u DISPLAY -u XAUTHORITY CMAKE_BUILD_PARALLEL_LEVEL=4 ./tests/verify_fresh_clone.sh`：隔离克隆、构建、安装、元数据检查、卸载与零残留验证通过。沙箱 CTest 62 Passed、3 个 GTK 项因无显示环境 Skipped、0 Failed；GTK 项已在真实 DISPLAY 下验证。
- `git diff --check`：通过。

验证日志保存在 `build/client-ctest.log`、`build/client-final-gtk.log`、`build/client-fresh-clone.log`。截图使用连接真实源码对象的 GTK/Cairo 测试捕获器与临时配置，未安装到宿主目录。

## 保留的实现差异

- 系统窗口装饰由桌面环境提供，未仿造图 3 的标题栏。
- 原生字号控件内部按钮次序与参考图的定制步进器略有区别；保留标准键盘编辑、范围限制和可访问性。
- 顶部为外观示例；图 2 的独立真实试打工具不属于本次选定布局。

final result: passed
