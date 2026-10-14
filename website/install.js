"use strict";
// Repository guidance and copying remain independent of release API requests.
(() => {
  const base = "https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method";
  const systems = {noble: "Ubuntu 24.04 LTS", jammy: "Ubuntu 22.04 LTS", bookworm: "Debian 12"};
  function updateSource() {
    const option = document.querySelector("input[name=deb-system]:checked");
    const suite = option?.dataset.suite;
    if (!Object.hasOwn(systems, suite)) return;
    document.getElementById("apt-system-name").textContent = systems[suite];
    document.getElementById("apt-source-command").textContent = `curl -fsSL ${base}/apt/client/modernime-${suite}.sources -o modernime.sources &&\nsudo install -D -m 0644 modernime.sources /etc/apt/sources.list.d/modernime.sources`;
  }
  document.querySelectorAll("input[name=deb-system]").forEach(option => option.addEventListener("change", updateSource));
  updateSource();
  document.querySelectorAll("[data-open-install]").forEach(link => link.addEventListener("click", () => {
    document.getElementById(link.dataset.openInstall).open = true;
  }));
  document.querySelectorAll("[data-copy]").forEach(button => button.addEventListener("click", async () => {
    const code = document.getElementById(button.dataset.copy);
    const status = document.getElementById(button.dataset.status);
    try {
      await navigator.clipboard.writeText(code.textContent);
      status.textContent = "命令已复制。请按顺序在终端执行。";
    } catch {
      const range = document.createRange();
      range.selectNodeContents(code);
      const selection = window.getSelection();
      selection.removeAllRanges(); selection.addRange(range);
      status.textContent = "已选中命令，请按 Ctrl+C 复制。";
    }
  }));
  document.querySelectorAll("[data-mobile-link]").forEach(link => link.addEventListener("click", () => {
    document.getElementById("mobile-menu").open = false;
  }));
  document.addEventListener("pointerdown", event => {
    const menu = document.getElementById("mobile-menu");
    if (menu.open && !menu.contains(event.target)) menu.open = false;
  });
  if (document.addEventListener) document.addEventListener("keydown", event => {
    const menu = document.getElementById("mobile-menu");
    if (event.key === "Escape" && menu.open) {
      menu.open = false;
      document.getElementById("mobile-menu-toggle").focus();
    }
  });
})();
