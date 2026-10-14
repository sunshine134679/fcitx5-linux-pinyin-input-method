"use strict";
const repository = "sunshine134679/fcitx5-linux-pinyin-input-method";
const releasePage = `https://github.com/${repository}/releases/latest`;
const samples = {
  single: {pinyin: "ni", words: ["你", "尼", "呢", "泥", "拟", "逆", "妮", "倪", "霓"]},
  double: {pinyin: "ni'hao", words: ["你好", "你号", "拟好", "拟合", "霓虹", "倪皓", "逆号"]}
};
document.querySelectorAll("[data-demo]").forEach(button => {
  button.addEventListener("click", () => {
    document.querySelectorAll("[data-demo]").forEach(item => {
      const selected = item === button;
      item.classList.toggle("active", selected);
      item.setAttribute("aria-pressed", String(selected));
    });
    const sample = samples[button.dataset.demo];
    const pinyin = document.getElementById("demo-pinyin");
    pinyin.replaceChildren(document.createTextNode(sample.pinyin));
    const caret = document.createElement("span");
    caret.className = "caret";
    caret.setAttribute("aria-hidden", "true");
    pinyin.append(caret);
    document.getElementById("demo-candidates").replaceChildren(...sample.words.map((word, index) => {
      const candidate = document.createElement("span");
      const number = document.createElement("small");
      number.textContent = String(index + 1);
      candidate.append(number, document.createTextNode(word));
      return candidate;
    }));
  });
});

let releaseAssets = null;
function updateDebDownload() {
  const button = document.getElementById("deb-download");
  const distro = document.getElementById("ubuntu-version").value;
  if (!releaseAssets) {
    button.href = `https://github.com/${repository}/releases/download/v0.1.10/modernime_0.1.10_amd64_${distro}.deb`;
    return;
  }
  const asset = releaseAssets.find(item => new RegExp(`^modernime[_-].*amd64[_-]${distro}\\.deb$`).test(item.name));
  button.href = asset ? asset.browser_download_url : releasePage;
  button.firstChild.textContent = asset ? "下载 .deb " : "查看可用安装包 ";
}
document.getElementById("ubuntu-version").addEventListener("change", updateDebDownload);

document.getElementById("copy-install").addEventListener("click", async () => {
  const text = document.getElementById("install-command").textContent;
  const status = document.getElementById("copy-status");
  try {
    await navigator.clipboard.writeText(text);
    status.textContent = "命令已复制。请先按文档准备构建依赖。";
  } catch {
    const selection = window.getSelection();
    const range = document.createRange();
    range.selectNodeContents(document.getElementById("install-command"));
    selection.removeAllRanges();
    selection.addRange(range);
    status.textContent = "无法自动复制，已选中命令；请按 Ctrl+C 复制。";
  }
});

// The static version remains usable when GitHub is unreachable or rate-limited.
// Only verified GitHub release assets are used; no API content enters innerHTML.
async function refreshRelease() {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(`https://api.github.com/repos/${repository}/releases/latest`, {signal: controller.signal, credentials: "omit", headers: {Accept: "application/vnd.github+json"}});
    if (!response.ok) throw new Error("release unavailable");
    const release = await response.json();
    if (release.draft || release.prerelease || !/^v\d+\.\d+\.\d+$/.test(release.tag_name) || !Array.isArray(release.assets)) throw new Error("invalid release");
    const expectedPrefix = `https://github.com/${repository}/releases/download/${release.tag_name}/`;
    releaseAssets = release.assets.filter(asset => typeof asset.name === "string" && typeof asset.browser_download_url === "string" && asset.browser_download_url.startsWith(expectedPrefix));
    document.getElementById("release-version").textContent = `最新发布 ${release.tag_name}`;
    document.getElementById("release-status").textContent = "已同步 GitHub 最新正式发布。请选择对应系统的安装包，并核验下载文件。";
    updateDebDownload();
    for (const [id, pattern, label] of [
      ["rpm-download", /^modernime-.*\.oe24\.x86_64\.rpm$/, "下载 .rpm "],
      ["portable-download", /^modernime-.*-linux-x86_64\.tar\.gz$/, "下载 .tar.gz "]
    ]) {
      const button = document.getElementById(id);
      const asset = releaseAssets.find(item => pattern.test(item.name));
      button.href = asset ? asset.browser_download_url : releasePage;
      button.firstChild.textContent = asset ? label : "查看可用安装包 ";
    }
    const checksums = releaseAssets.find(asset => asset.name === "SHA256SUMS.txt");
    document.getElementById("checksum-link").href = checksums ? checksums.browser_download_url : releasePage;
  } catch {
    document.getElementById("release-status").textContent = "暂时无法获取最新版本，当前显示 v0.1.10 的下载链接。可前往 GitHub Releases 查看最新发布。";
  } finally {
    clearTimeout(timeout);
  }
}
refreshRelease();

// Presentation effects are optional: controls and content work without motion APIs.
const reducedMotion = window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : {matches: true};
function transitionContent(element) {
  if (element && typeof element.animate === "function" && !reducedMotion.matches) {
    element.animate([{opacity: .25, transform: "translateY(7px)"}, {opacity: 1, transform: "translateY(0)"}], {duration: 430, easing: "cubic-bezier(.16,1,.3,1)"});
  }
}
const scenes = {
  hello: {prelude: "你好，Linux。", context: "每次新的相遇，", prefix: "先说一句", word: "你好", pinyin: "ni'hao", candidates: ["你好", "你号", "拟好"]},
  express: {prelude: "你好，Linux。", context: "今天也有很多想法，", prefix: "值得被", word: "好好表达", pinyin: "hao'hao'biao'da", candidates: ["好好表达", "好好", "表达"]},
  linux: {prelude: "热爱，始于自由。", context: "熟悉的桌面，新的灵感，", prefix: "正在", word: "自由生长", pinyin: "zi'you'sheng'zhang", candidates: ["自由生长", "自由", "生长"]}
};
let typingTimer = null;
let currentScene = "express";
function renderScene(name, type = true) {
  const scene = scenes[name];
  if (!scene) return;
  currentScene = name;
  clearTimeout(typingTimer);
  for (const field of ["prelude", "context", "prefix", "word"]) {
    const element = document.getElementById(`hero-${field}`);
    if (element) element.textContent = scene[field];
  }
  document.querySelectorAll("[data-scene]").forEach(button => button.setAttribute("aria-pressed", String(button.dataset.scene === name)));
  const candidates = document.getElementById("hero-candidates");
  if (candidates) {
    candidates.replaceChildren(...scene.candidates.map((word, index) => {
      const candidate = document.createElement("span");
      candidate.className = index === 0 ? "candidate chosen" : "candidate";
      const number = document.createElement("small");
      number.textContent = String(index + 1);
      candidate.append(number, document.createTextNode(word));
      return candidate;
    }));
    transitionContent(candidates);
  }
  const composition = document.getElementById("hero-pinyin");
  if (!composition) return;
  if (!type || reducedMotion.matches || document.hidden) {
    composition.textContent = scene.pinyin;
    return;
  }
  let length = 0;
  function next() {
    composition.textContent = scene.pinyin.slice(0, ++length);
    if (length < scene.pinyin.length) typingTimer = setTimeout(next, 55);
  }
  next();
}
document.querySelectorAll("[data-scene]").forEach(button => button.addEventListener("click", () => renderScene(button.dataset.scene)));
document.querySelectorAll("[data-demo]").forEach(button => button.addEventListener("click", () => transitionContent(document.getElementById("demo-candidates"))));
if (typeof document.addEventListener === "function") {
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) renderScene(currentScene, false);
  });
}

const screenshots = {
  overview: {src: "assets/client-overview.png", title: "概览", caption: "概览 · 运行状态与常用设置，一目了然。", number: "01 / 03"},
  appearance: {src: "assets/client-appearance.png", title: "候选与外观", caption: "候选与外观 · 每一处显示细节，按你的喜好来。", number: "02 / 03"},
  clipboard: {src: "assets/client-clipboard.png", title: "剪贴板", caption: "剪贴板 · 常用内容，随取随用。", number: "03 / 03"}
};
let screenshotRevision = 0;
document.querySelectorAll("[data-screenshot]").forEach(button => button.addEventListener("click", async () => {
  const shot = screenshots[button.dataset.screenshot];
  if (!shot) return;
  const revision = ++screenshotRevision;
  const image = document.getElementById("client-screenshot");
  image.src = shot.src;
  image.alt = `ModernIME 设置客户端的${shot.title}页面`;
  document.getElementById("screenshot-original").href = shot.src;
  document.getElementById("screenshot-original").setAttribute("aria-label", `在新标签页查看${shot.title}截图原图`);
  document.getElementById("screenshot-caption").textContent = shot.caption;
  document.getElementById("screenshot-pagination").textContent = shot.number;
  document.querySelectorAll("[data-screenshot]").forEach(item => item.setAttribute("aria-pressed", String(item === button)));
  if (typeof image.decode === "function") {
    try { await image.decode(); } catch { return; }
  }
  if (revision === screenshotRevision) transitionContent(image);
}));

// Observe once, then disconnect each element. No perpetual animation or scroll loop.
if (typeof IntersectionObserver === "function" && !reducedMotion.matches) {
  const elements = document.querySelectorAll(".section-heading, .interactive-demo, .feature-stack, .detail-strip, .client-heading, .gallery-navigation, .client-figure, .client-benefits, .download-card, .source-install, .help-grid, .open-source");
  const observer = new IntersectionObserver(entries => {
    entries.forEach(entry => {
      if (entry.isIntersecting) {
        entry.target.classList.add("is-visible");
        observer.unobserve(entry.target);
      }
    });
  }, {threshold: .08});
  elements.forEach(element => {
    element.setAttribute("data-reveal", "");
    observer.observe(element);
  });
  document.documentElement.classList.add("motion-ready");
  renderScene(currentScene);
  if (reducedMotion.addEventListener) reducedMotion.addEventListener("change", () => {
    if (reducedMotion.matches) {
      observer.disconnect();
      document.documentElement.classList.remove("motion-ready");
      renderScene(currentScene, false);
    }
  });
}
if (typeof window.addEventListener === "function" && typeof requestAnimationFrame === "function") {
  let scheduled = false;
  function updateProgress() {
    const root = document.documentElement;
    const total = root.scrollHeight - root.clientHeight;
    root.style.setProperty("--reading-progress", total > 0 ? Math.min(1, Math.max(0, window.scrollY / total)) : 0);
    scheduled = false;
  }
  window.addEventListener("scroll", () => {
    if (!scheduled) { scheduled = true; requestAnimationFrame(updateProgress); }
  }, {passive: true});
  window.addEventListener("resize", updateProgress);
  updateProgress();
}
