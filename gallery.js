"use strict";
(() => {
  const buttons = [...document.querySelectorAll("[data-screenshot]")];
  const image = document.getElementById("client-screenshot");
  const previous = document.getElementById("gallery-previous");
  const original = document.getElementById("screenshot-original");
  const status = document.getElementById("gallery-status");
  const motion = window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : {matches: true};
  const cache = new Map();
  let revision = 0;
  let displayed = buttons.find(button => button.getAttribute("aria-pressed") === "true") || buttons[0];
  let animations = [];

  // Retain decoded frames: revisiting a tab needs no new image request.
  function loadImage(shot) {
    const width = Math.round(image.clientWidth || 980);
    const key = `${shot.src}|${width}|${window.devicePixelRatio || 1}`;
    if (cache.has(key)) return cache.get(key);
    const promise = new Promise((resolve, reject) => {
      const pending = new Image();
      let settled = false;
      const timeout = setTimeout(() => finish(false), 12000);
      function finish(loaded) {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        pending.onload = pending.onerror = null;
        loaded ? resolve(pending) : reject(new Error("Screenshot unavailable"));
      }
      pending.onload = async () => {
        try {
          if (typeof pending.decode === "function") await pending.decode();
          finish(true);
        } catch { finish(false); }
      };
      pending.onerror = () => finish(false);
      pending.sizes = image.sizes;
      pending.srcset = shot.srcset;
      pending.src = shot.src;
    });
    cache.set(key, promise);
    promise.catch(() => { if (cache.get(key) === promise) cache.delete(key); });
    return promise;
  }
  function stopTransition() {
    animations.forEach(animation => animation.cancel());
    animations = [];
    if (previous) previous.hidden = true;
  }
  function show(button) {
    const shot = button.dataset;
    stopTransition();
    if (previous) previous.src = image.currentSrc || image.src;
    image.srcset = shot.srcset;
    image.src = shot.src;
    image.alt = `ModernIME 设置客户端的${shot.title}页面`;
    original.href = shot.original;
    original.setAttribute("aria-label", `在新标签页查看${shot.title}截图原图`);
    document.getElementById("screenshot-caption").textContent = shot.caption;
    document.getElementById("screenshot-pagination").textContent = shot.number;
    buttons.forEach(item => item.setAttribute("aria-pressed", String(item === button)));
    displayed = button;
    status.textContent = `已显示${shot.title}`;
    if (previous && typeof image.animate === "function" && !motion.matches) {
      previous.hidden = false;
      const timing = {duration: 420, easing: "cubic-bezier(.22,.68,0,1)"};
      const outgoing = previous.animate([{opacity: 1}, {opacity: 0}], timing);
      const incoming = image.animate([{opacity: 0}, {opacity: 1}], timing);
      animations = [outgoing, incoming];
      outgoing.onfinish = () => { previous.hidden = true; };
    }
  }
  function warm(button) { loadImage(button.dataset).catch(() => {}); }
  buttons.forEach(button => {
    button.addEventListener("pointerenter", () => warm(button));
    button.addEventListener("focus", () => warm(button));
    button.addEventListener("click", async () => {
      const current = ++revision;
      buttons.forEach(item => item.setAttribute("data-pending", String(item === button && item !== displayed)));
      if (button === displayed && image.complete && image.naturalWidth > 0) {
        image.setAttribute("aria-busy", "false");
        status.textContent = `已显示${button.dataset.title}`;
        return;
      }
      image.setAttribute("aria-busy", "true");
      status.textContent = `正在准备${button.dataset.title}…`;
      try {
        await loadImage(button.dataset);
        if (current === revision) show(button);
      } catch {
        if (current === revision) status.textContent = "截图加载失败，保留当前图片。请点击标签重试。";
      } finally {
        if (current === revision) {
          image.setAttribute("aria-busy", "false");
          buttons.forEach(item => item.setAttribute("data-pending", "false"));
        }
      }
    });
  });
  // Warm every frame near the viewport, without competing with
  // the first screen or overriding data-saving preferences.
  if (!navigator.connection?.saveData && typeof IntersectionObserver === "function") {
    const observer = new IntersectionObserver(entries => {
      if (entries.some(entry => entry.isIntersecting)) {
        buttons.forEach(warm);
        observer.disconnect();
      }
    }, {rootMargin: "400px 0px"});
    observer.observe(original);
  }
  if (motion.addEventListener) motion.addEventListener("change", () => { if (motion.matches) stopTransition(); });
})();
