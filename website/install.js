"use strict";
// Repository guidance and copying remain independent of release API requests.
(() => {
  const options = [...document.querySelectorAll("input[name=install-system]")];
  const panels = [...document.querySelectorAll("[data-install-panel]")];
  function updatePlatform() {
    const value = options.find(option => option.checked)?.value;
    if (!panels.some(panel => panel.dataset.installPanel === value)) return;
    panels.forEach(panel => { panel.hidden = panel.dataset.installPanel !== value; });
  }
  options.forEach(option => option.addEventListener("change", updatePlatform));
  updatePlatform();
  document.querySelectorAll("[data-open-install]").forEach(link => link.addEventListener("click", () => {
    document.getElementById(link.dataset.openInstall).open = true;
  }));
  document.querySelectorAll("[data-copy]").forEach(button => button.addEventListener("click", async () => {
    const code = document.getElementById(button.dataset.copy);
    const status = document.getElementById(button.dataset.status);
    document.querySelectorAll("[data-copy]").forEach(item => delete item.dataset.copied);
    try {
      await navigator.clipboard.writeText(code.textContent);
      button.dataset.copied = "true";
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
