# ModernIME 官方网站

公开地址：<https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/>

网站使用原生 HTML、CSS、JavaScript，没有 npm 构建步骤、外部字体或新增输入法运行依赖。

## 本地预览与验证

```bash
python3 -m http.server 4173 --directory website
python3 tests/website/website_test.py
node tests/website/app_test.js
```

访问 `http://localhost:4173`。下载区从 GitHub Releases 只读查询最新正式发布；断网、超时或限流时，回退到已核验的 v0.1.10 资产。新增发行版或变更资产命名时，应同步调整 `app.js` 的兼容规则和页面说明。复制按钮在权限不足时会选中命令，供手动复制。

## 发布与软件源共存

`website.yml` 在官网相关代码推送到 `main` 时运行：读取既有 `gh-pages` 分支，通过 `scripts/prepare_website.py` 覆盖首页文件和 `assets/`，完整保留 `/apt`、`/rpm` 的路径和内容，再部署并保存回 `gh-pages`。脚本校验覆盖前后全部软件源文件的 SHA256，不会重新签名或生成软件源。

`release.yml` 也会取 `main` 上的官网覆盖到发布产物，因此重新发布旧产品标签也会保留官网。两个发布任务共用 `modernime-github-pages` 串行组，避免并发覆盖软件源历史。

客户端截图是当前源码的界面示例，不承诺已发布的安装包包含源码之后的所有界面更新。网页候选栏是明确标注的交互示意，不收集键盘输入，不模拟真实输入法转换。

## 视觉与动效维护

首屏采用大字排版与流线图形，三种输入场景支持一次性打字演示。滚动入场使用 IntersectionObserver，元素出现后取消观察；阅读进度通过被动 scroll 监听与单帧合并更新，页面没有持续运行的动画循环。系统减少动态效果偏好会关闭入场、打字、截图切换动效，偏好在页面打开后改变也会生效；浏览器不支持观察器时保持全文可见。

客户端展示使用用户提供的原始 PNG，三个标签对应概览、候选与外观、剪贴板。网页通过 clip-path 清理窗口外缘，没有生成式重绘或有损压缩；查看原图可直接打开原始文件。截图最大展示宽度为 980 CSS px，完整保留窗口内容，不使用旧版的固定高度裁切。源图约 980 px 宽，无法在高像素密度屏幕上提供超出源图的真实细节；更换更高分辨率截图时请同步 width/height 和展示规则。
