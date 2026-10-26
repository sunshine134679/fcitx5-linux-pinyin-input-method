# ModernIME 官方网站

公开地址：[GitHub Pages](https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/) · [ChatGPT Sites](https://modernime-official.bubblyfig129.chatgpt.site/)

网站使用原生 HTML、CSS、JavaScript，无 npm 构建步骤、外部字体或新增输入法依赖。两处托管使用同一份 `website/` 源码，发布流程各自独立。

## 本地预览与验证

```bash
python3 -m http.server 4173 --directory website
python3 tests/website/website_test.py
node --test tests/website/*_test.js
```

访问 `http://localhost:4173`。Python 测试覆盖资产、SEO、安装命令、发布缓存与软件源保留；Node 测试覆盖下载回退、图库、安装交互、输入示例和动效生命周期。网站变更不等于输入法算法变更，不据此宣称真实引擎或安装生命周期通过。

## 页面与交互

页面按输入 → 候选体验 → 设置预览 → 安装组织，保留原有蓝白配色、中文展示字体与深蓝演示框。重复能力说明合并到可以操作的候选工作台，真实客户端截图、完整安装步骤和低频帮助通过原生 `details` 展开。

`experience.js` 负责三个独立示例实例：

- 首屏输入：有限示例词库，数字键或点击选词，Space 选当前物理页首词，Enter 上屏原文，Escape 清空输入，PageUp / PageDown 翻页，单独 Shift 切换中英文。快捷键只作用于当前输入框，兼容本机输入法 composition，不把 Shift 组合键当作模式切换。
- 候选工作台：`zongwen` 容错、显式选择「实践」后的第 5 → 3 → 1 位示例学习、六个专业领域、`v2` 三条固定示例剪贴板。切换示例恢复中文候选；学习可重置，不写入持久存储。
- 设置预览：14–28 pt 字号、3–9 个单字候选容量、浅色／深色外观即时生效；按实际显示宽度与字号分页，保持候选身份和顺序，不裁掉可选项。预览不修改本机设置。

页面明确标注有限示意；输入只用于当前页面，不上传，不读取系统剪贴板或用户词典，也不运行 LibIME。未匹配的输入可以原文上屏。无 JavaScript 时仍能阅读正文、静态候选、全部安装渠道和展开的说明；交互示例需要 JavaScript。

`motion.js` 只负责首屏一次可选的输入与上屏演示。首次进入视口触发，鼠标或焦点进入、主动输入、后台、播放后离屏均取消旧计时，不自动恢复或循环；用户可手动重播。减少动态效果时不自动播放，重播直接显示结果。候选重排按身份复用节点并用 180 ms 位移解释排序；偏好减少动态效果时直接更新。移除通用滚动入场、整栏淡出和装饰性循环。

## 真实客户端图库

`gallery.js` 独立负责真实 GTK3 客户端截图，使用隔离示例数据。1× / 3× 无损 WebP 配合 `srcset` / `sizes`，点击可查看 2940 × 2160 PNG 原图。替换时同步尺寸、`data-src`、`data-srcset`、`data-original` 与首张图的 `srcset`，不要混用内容不同的旧图。

图库在接近视口、悬停或键盘聚焦时预加载并解码，节省流量模式关闭自动预加载；按路径、宽度及像素比缓存解码结果。成功后同步图片、说明、选中标签与原图链接，并以 240 ms 交叉淡入；减少动态效果时立即切换。准备时显示目标与加载反馈，失败或 12 秒超时保留原图并支持重试，连续请求仅接受最后一次结果。具体功能以发行版本为准，网页演示不代替真实桌面录屏。

## 下载、安装与导航

下载区从 GitHub Releases 只读查询最新正式发布；断网、超时或限流时保留已核验版本的静态链接。新增资产命名时同步 `app.js` 的兼容规则和页面说明。

安装系统通过原生单选框切换 Ubuntu 24.04 / openEuler / 其他 Linux，JavaScript 只隐藏未选中的安装面板。Ubuntu 24.04 LTS（Noble）/ amd64 是当前 DEB 与 APT 支持目标；Ubuntu 22.04 / Debian 12 已停止预编译支持，页面保留升级或源码自行验证提示。完整签名源配置必须按顺序保留，不能只显示最后一条安装命令；APT `Signed-By`、RPM `gpgcheck` / `repo_gpgcheck` 与既有客户端配置 URL 不变。代码块在自身范围横向滚动，不能撑宽页面。

`install.js` 负责渠道切换、分步复制和手机导航。复制成功显示确认状态，失败选中完整命令供手动复制。手机菜单支持选中后关闭、Escape 返回焦点与点外部关闭。安装后三步启用提示只保留一处。

## 发布与软件源共存

`website.yml` 在官网相关代码推送到 `main` 时运行，读取既有 `gh-pages`，由 `scripts/prepare_website.py` 覆盖官网文件和 `assets/` 后部署，再保存回 `gh-pages`。覆盖前后校验 `/apt`、`/rpm` 全部文件 SHA256，不重新签名或生成软件源。`release.yml` 同样获取 `main` 官网，两个发布任务共用 `modernime-pages-sync` 串行组。

发布脚本为脚本和样式加入文件内容 SHA256 前 12 位，避免旧缓存脚本与新页面混用。新增脚本同时维护脚本 FILES、哈希清单和 `website.yml` 的持久化清单。

Sites 使用原 project_id `appgprj_6acf2e74f8d88191a3c1464509905652` 和独立源码仓库；GitHub Pages 发布不会自动更新 Sites。同步时使用 Sites 源码助手保存、打包及部署，保持公开访问范围。导出后将 canonical、分享图片和 sitemap 指向对应托管地址；APT / RPM 命令继续使用 GitHub Pages 签名软件源。

导航和 favicon 使用与客户端同源的蓝色 MI PNG 图标。页面保留 canonical、Open Graph、Twitter Card、SoftwareApplication JSON-LD、sitemap 和 robots；版本更新同步结构化数据与静态下载回退。GitHub Pages 项目路径下的 robots 不代替域名根规则。

## 本轮备份与验收

2026-10-25 改版前基线为 `50d7ce5672cc5d63bf27e0511c03eeb8a3017106`。本机备份位于 `build/官网备份/2026-10-25-改版前-50d7ce5/`，包含原版网站及发布／测试接线归档、校验值和截图。备份不随 Git 分发，恢复时只恢复网站相关文件，避免覆盖其他模块工作。

本轮具体范围、回归修复、验证结果和限制见 [官网交互优化与验收](../docs/官网交互优化与验收-2026-10-26.md)。
