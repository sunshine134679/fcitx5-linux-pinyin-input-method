# 客户端修正设计核对

日期：2026-10-14。分支：`main`。基线：`df38eb0`。本次按用户要求直接在主分支修改，并完成自行审查。

## 设计依据与实际截图

保持 `.coordination/design/2026-10-13-client/layout-1.png` 和 `layout-3.png` 的蓝、白、浅灰色调；本次以用户提供的四张实际截图及修正要求为准。

- 候选页问题截图：`/home/wsl/.codex/attachments/7d0b0b5a-de8a-4271-a3d3-c8d201aebdcc/codex-clipboard-33d10f81-bc6c-4ae6-ad88-7fa9eb7397ed.png`，970×756。
- 快捷键截图：`/home/wsl/.codex/attachments/6c856b74-2138-462b-849b-b3a5b88aad69/codex-clipboard-4381f3bd-c0e6-4886-89f8-c19110f93b5f.png`，696×83。
- 更新页截图：`/home/wsl/.codex/attachments/3e6d5624-d2ce-4b95-aa6b-a69f46607024/codex-clipboard-f2e09bd7-97e9-4287-94a9-7c4a86424ba1.png`，756×714。
- 导航焦点截图：`/home/wsl/.codex/attachments/b2616d85-aada-4cd4-8fc2-78728c8021a9/codex-clipboard-016dda8c-a57a-40a2-942f-65f2d7c38d76.png`，244×450。

最终 GTK 实际渲染位于 `/home/wsl/ModernIME/build/design-corrections/`：`candidates-{single,double,mixed,minimum}.png`、`input.png`、`updates.png`、`updates-minimum.png`、`overview-focused.png`。截图捕获器连接最终客户端源码对象，使用真实 DISPLAY 和隔离临时配置。标准客户端内容区 980×720、最小窗口 720×520，密度为 1。窗口装饰由系统提供。

整体并排比较：`candidates-comparison.png`，去除问题截图顶部 36 px 系统装饰后，以内容区原尺寸比较；实现与源图宽度差 10 px 留白处理，未拉伸。重点局部比较：`navigation-comparison.png`，保留导航截图和实际导航的原始密度。

## 核对与修正

| 核对面 | 实际结果 |
| --- | --- |
| 候选定宽与文案 | 预览复用输入法的 CandidateBarMetrics、CandidatePagination、CandidateBarLayout 和 Cairo 绘制；默认容量 9 时单字显示 9 项、双字显示 7 项，栏宽相同。混合词组按测量结果分页。名称改为“候选栏长度”，说明以单字容量为基准；搜索保留旧名称关键词。 |
| 字体与内容 | 保留 Noto Sans / Noto Sans CJK SC 层级；预览采用实际输入法的候选和编号字号。预览示例切换不写设置、不记录学习。数字选词开关影响按键行为，编号显示与实际输入法一致。 |
| 排版与间距 | 软件源和终端命令独立为等宽、可选中复制的浅灰文本块；长路径按字符边界换行。状态说明使用信息图标与浅蓝提示框，更新摘要另起一组，保持左对齐。720×520 下没有横向撑宽，底部操作区可见，内容纵向滚动。 |
| 颜色与切换状态 | 导航使用独立选中类，避免主按钮的白字规则覆盖；选中、焦点和悬停保持蓝字浅蓝底。仅背景、边框作 150 ms 过渡，文字立即切换，避免淡入期间对比度下降。移除重复导航选择器和废弃预览样式。 |
| 图标与资源 | 复用 GTK 信息与导航图标和现有 Cairo/Pango 共享库；没有新增图像、下载或系统依赖。 |
| Shift 操作闭环 | 新增直接选择 Shift 的按钮；左右 Shift 录制值均通过核心校验并可保存、重新加载，界面统一显示 Shift。原组合键录制仍可使用，输入禁用时新增入口同时禁用。 |

## 验证与审查记录

- 新增导航焦点回归测试先在原代码失败，复现白字覆盖；修复后实际 GTK 检查通过。
- Shift 校验回归测试先在原代码失败，确认录制值被拒绝；修复后左右 Shift 校验、保存、重新加载通过。
- `cmake --build build/install-debug -j4`：通过，无新增编译警告。
- 配置真实 `DISPLAY=:1`、XAUTHORITY 和会话 DBus 后，`ctest --test-dir build/install-debug --output-on-failure`：65 Passed、0 Skipped、0 Failed。
- 自审后清理样式并补充左右 Shift 持久化检查，再跑设置核心、三项 GTK 和相关契约共 7 项：全部通过。
- `env -u DISPLAY -u XAUTHORITY CMAKE_BUILD_PARALLEL_LEVEL=4 ./tests/verify_fresh_clone.sh`：隔离克隆、离线构建、安装、元数据发现、卸载及零残留检查通过。通过仅作用于验证进程的 Git excludesFile 排除私人未跟踪目录；沙箱 CTest 62 Passed、3 GTK Skipped、0 Failed，GTK 项已在真实显示环境验证。
- 自行审查共享布局链接、绘制资源释放、设置兼容性、焦点与禁用状态；`git diff --check` 通过。

日志：`build/client-corrections-{build,final-build,ctest,final-gtk,fresh-clone}.log`。本次核对替代旧版关于“固定显示 9 个词组并横向滚动”的错误预览结论。

final result: passed
