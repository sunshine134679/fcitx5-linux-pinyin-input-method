"use strict";
const repository = "sunshine134679/fcitx5-linux-pinyin-input-method";
const releasePage = `https://github.com/${repository}/releases/latest`;
let releaseAssets = null;
function updateDebDownload() {
  const button = document.getElementById("deb-download");
  const distro = "ubuntu24.04";
  if (!releaseAssets) {
    button.href = `https://github.com/${repository}/releases/download/v0.1.23/modernime_0.1.23_amd64_${distro}.deb`;
    return;
  }
  const asset = releaseAssets.find(item => new RegExp(`^modernime[_-].*amd64[_-]${distro}\\.deb$`).test(item.name));
  button.href = asset ? asset.browser_download_url : releasePage;
  button.firstChild.textContent = asset ? "下载 .deb " : "查看可用安装包 ";
}

document.getElementById("copy-install").addEventListener("click", async () => {
  const button = document.getElementById("copy-install");
  const text = document.getElementById("install-command").textContent;
  const status = document.getElementById("copy-status");
  try {
    await navigator.clipboard.writeText(text);
    button.dataset.copied = "true";
    status.textContent = "命令已复制。请先按文档准备构建依赖。";
  } catch {
    delete button.dataset.copied;
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
    document.getElementById("release-status").textContent = "暂时无法获取最新版本，当前显示 v0.1.23 的下载链接。可前往 GitHub Releases 查看最新发布。";
  } finally {
    clearTimeout(timeout);
  }
}
refreshRelease();
