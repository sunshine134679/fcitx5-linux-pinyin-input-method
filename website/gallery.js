"use strict";
// Keep the gallery independent of release queries and presentation effects.
(() => {
  const buttons = [...document.querySelectorAll("[data-screenshot]")];
  const image = document.getElementById("client-screenshot");
  const original = document.getElementById("screenshot-original");
  const status = document.getElementById("gallery-status");
  const motion = window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : {matches: true};
  let revision = 0;

  function loadImage(src) {
    return new Promise((resolve, reject) => {
      const pending = new Image();
      const timeout = setTimeout(() => finish(false), 15000);
      function finish(loaded) {
        clearTimeout(timeout);
        pending.onload = pending.onerror = null;
        loaded ? resolve() : reject(new Error("Screenshot unavailable"));
      }
      pending.onload = () => finish(true);
      pending.onerror = () => finish(false);
      pending.src = src;
    });
  }

  buttons.forEach(button => button.addEventListener("click", async () => {
    const current = ++revision;
    const shot = button.dataset;
    image.setAttribute("aria-busy", "true");
    status.textContent = `正在加载${shot.title}…`;
    try {
      await loadImage(shot.src);
      if (current !== revision) return;
      image.src = shot.src;
      image.alt = `ModernIME 设置客户端的${shot.title}页面`;
      original.href = shot.src;
      original.setAttribute("aria-label", `在新标签页查看${shot.title}高清原图`);
      document.getElementById("screenshot-caption").textContent = shot.caption;
      document.getElementById("screenshot-pagination").textContent = shot.number;
      buttons.forEach(item => item.setAttribute("aria-pressed", String(item === button)));
      status.textContent = `已显示${shot.title}`;
      if (typeof image.animate === "function" && !motion.matches) {
        image.animate([{opacity: .4}, {opacity: 1}], {duration: 260, easing: "ease-out"});
      }
    } catch {
      if (current === revision) status.textContent = "截图加载失败，保留当前图片。请点击标签重试。";
    } finally {
      if (current === revision) image.setAttribute("aria-busy", "false");
    }
  }));
})();
