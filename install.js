"use strict";
// Repository guidance and copying remain independent of release API requests.
(() => {
  const options = [...document.querySelectorAll("input[name=install-system]")];
  const panels = [...document.querySelectorAll("[data-install-panel]")];
  const motion = window.ModernIMEMotion;
  let currentPanel = null, copyGeneration = 0;
  function updatePlatform() {
    const value = options.find(option => option.checked)?.value;
    if (!panels.some(panel => panel.dataset.installPanel === value)) return;
    panels.forEach(panel => { panel.hidden = panel.dataset.installPanel !== value; });
    const selected = panels.find(panel => !panel.hidden);
    if (currentPanel && selected !== currentPanel) motion?.transitions.animate(selected, [{opacity: .4, transform: "translateY(8px)"}, {opacity: 1, transform: "none"}], {duration: 220});
    if (currentPanel && selected !== currentPanel) motion?.transitions.cancel(currentPanel);
    currentPanel = selected;
    motion?.moveIndicator(document.querySelector(".install-tabs"), options.find(option => option.checked)?.nextElementSibling);
  }
  options.forEach(option => option.addEventListener("change", updatePlatform));
  updatePlatform();
  if (typeof ResizeObserver !== "undefined") {
    const tabs = document.querySelector(".install-tabs");
    if (tabs) new ResizeObserver(() => motion?.moveIndicator(tabs, options.find(option => option.checked)?.nextElementSibling)).observe(tabs);
  }
  document.querySelectorAll("[data-open-install]").forEach(link => link.addEventListener("click", () => {
    document.getElementById(link.dataset.openInstall).open = true;
  }));
  document.querySelectorAll("[data-copy]").forEach(button => button.addEventListener("click", async () => {
    const generation = ++copyGeneration;
    const code = document.getElementById(button.dataset.copy);
    const status = document.getElementById(button.dataset.status);
    document.querySelectorAll("[data-copy]").forEach(item => delete item.dataset.copied);
    try {
      await navigator.clipboard.writeText(code.textContent);
      if (generation !== copyGeneration) return;
      button.dataset.copied = "true";
      status.textContent = "命令已复制。请按顺序在终端执行。";
      motion?.transitions.animate(button, [{transform: "scale(.96)"}, {transform: "scale(1)"}], {duration: 180});
    } catch {
      if (generation !== copyGeneration) return;
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
