"use strict";
// Presentation runs independently from release queries and screenshot loading.
(() => {
  const root = document.documentElement;
  const reducedMotion = window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : {matches: true};
  const hero = document.querySelector(".hero-demo");
  const toggle = document.getElementById("hero-motion-toggle");
  const replay = document.getElementById("hero-motion-replay");
  const scenes = {
    hello: {prelude: "你好，Linux。", context: "每次新的相遇，", prefix: "先说一句", word: "你好", pinyin: "ni'hao", candidates: ["你好", "你号", "拟好"]},
    express: {prelude: "你好，Linux。", context: "今天也有很多想法，", prefix: "值得被", word: "好好表达", pinyin: "hao'hao'biao'da", candidates: ["好好表达", "好好", "表达"]},
    linux: {prelude: "热爱，始于自由。", context: "熟悉的桌面，新的灵感，", prefix: "正在", word: "自由生长", pinyin: "zi'you'sheng'zhang", candidates: ["自由生长", "自由", "生长"]}
  };
  const order = Object.keys(scenes);
  let currentScene = "express";
  let typingTimer = null, rotationTimer = null;
  let paused = false, hovered = false, focused = false, visible = true;
  let revealObserver;

  function transitionContent(element) {
    if (element && typeof element.animate === "function" && !reducedMotion.matches && !document.hidden) {
      element.animate([{opacity: .25, transform: "translateY(7px)"}, {opacity: 1, transform: "translateY(0)"}], {duration: 430, easing: "cubic-bezier(.16,1,.3,1)"});
    }
  }
  function canAnimate() { return !reducedMotion.matches && !document.hidden && visible; }
  function stopTimers() {
    clearTimeout(typingTimer); clearTimeout(rotationTimer);
    typingTimer = rotationTimer = null;
  }
  function scheduleRotation() {
    clearTimeout(rotationTimer);
    rotationTimer = null;
    if (!hero || !canAnimate() || paused || hovered || focused) return;
    rotationTimer = setTimeout(() => renderScene(order[(order.indexOf(currentScene) + 1) % order.length]), 6500);
  }
  function renderScene(name, type = true) {
    const scene = scenes[name];
    if (!scene) return;
    currentScene = name;
    stopTimers();
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
      if (type) transitionContent(candidates);
    }
    const composition = document.getElementById("hero-pinyin");
    if (composition) {
      if (!type || !canAnimate()) composition.textContent = scene.pinyin;
      else {
        let length = 0;
        function next() {
          composition.textContent = scene.pinyin.slice(0, ++length);
          typingTimer = length < scene.pinyin.length ? setTimeout(next, 55) : null;
        }
        next();
      }
    }
    scheduleRotation();
  }
  function updateControls() {
    if (!toggle) return;
    toggle.hidden = false;
    if (replay) replay.hidden = false;
    toggle.disabled = reducedMotion.matches;
    toggle.setAttribute("aria-pressed", String(paused));
    toggle.textContent = reducedMotion.matches ? "已减少动态效果" : paused ? "继续轮播" : "暂停轮播";
    root.classList.toggle("demo-paused", paused || !canAnimate());
  }
  document.querySelectorAll("[data-scene]").forEach(button => button.addEventListener("click", () => renderScene(button.dataset.scene)));
  if (toggle) toggle.addEventListener("click", () => {
    paused = !paused;
    // An explicit resume wins over the focus/hover that activated this button.
    if (!paused) { hovered = false; focused = false; }
    renderScene(currentScene, false);
    updateControls();
  });
  if (replay) replay.addEventListener("click", () => renderScene(currentScene));
  if (hero) {
    hero.addEventListener("pointerenter", () => { hovered = true; scheduleRotation(); });
    hero.addEventListener("pointerleave", () => { hovered = false; scheduleRotation(); });
    hero.addEventListener("focusin", () => { focused = true; scheduleRotation(); });
    hero.addEventListener("focusout", event => {
      if (hero.contains(event.relatedTarget)) return;
      focused = false; scheduleRotation();
    });
  }
  document.addEventListener("visibilitychange", () => {
    renderScene(currentScene, false);
    updateControls();
  });

  document.querySelectorAll("[data-demo]").forEach(button => button.addEventListener("click", () => transitionContent(document.getElementById("demo-candidates"))));
  document.querySelectorAll("[data-workflow]").forEach(button => button.addEventListener("click", () => {
    document.querySelectorAll("[data-workflow]").forEach(item => item.setAttribute("aria-pressed", String(item === button)));
    document.querySelectorAll("[data-workflow-panel]").forEach(panel => {
      const selected = panel.dataset.workflowPanel === button.dataset.workflow;
      panel.hidden = !selected;
      if (selected) transitionContent(panel);
    });
  }));

  // Content stays visible by default. Observers only add one-shot entrance effects.
  function configureMotion() {
    if (revealObserver) revealObserver.disconnect();
    root.classList.remove("motion-ready");
    root.classList.toggle("hero-motion-ready", !reducedMotion.matches);
    if (!reducedMotion.matches && typeof IntersectionObserver === "function") {
      try {
        const elements = document.querySelectorAll(".section-heading, .interactive-demo, .feature-stack, .detail-strip, .client-heading, .gallery-navigation, .client-figure, .client-benefits, .download-card, .source-install, .help-grid, .open-source, .everyday-heading, .workflow-grid, .vocabulary-strip");
        revealObserver = new IntersectionObserver(entries => {
          entries.forEach(entry => {
            if (entry.isIntersecting) {
              entry.target.classList.add("is-visible");
              revealObserver.unobserve(entry.target);
            }
          });
        }, {threshold: .08});
        elements.forEach(element => { element.setAttribute("data-reveal", ""); revealObserver.observe(element); });
        root.classList.add("motion-ready");
      } catch { root.classList.remove("motion-ready"); }
    }
    renderScene(currentScene, !reducedMotion.matches && !paused);
    updateControls();
  }
  configureMotion();
  if (hero && typeof IntersectionObserver === "function") {
    try {
      visible = false;
      stopTimers();
      const observer = new IntersectionObserver(entries => {
        const wasVisible = visible;
        visible = entries.some(entry => entry.isIntersecting);
        renderScene(currentScene, visible && !wasVisible && !paused);
        updateControls();
      }, {threshold: 0});
      observer.observe(hero);
    } catch { visible = true; renderScene(currentScene); }
  }
  if (reducedMotion.addEventListener) reducedMotion.addEventListener("change", configureMotion);
  else if (reducedMotion.addListener) reducedMotion.addListener(configureMotion);

  if (typeof window.addEventListener === "function" && typeof requestAnimationFrame === "function") {
    let scheduled = false;
    function updateProgress() {
      const total = root.scrollHeight - root.clientHeight;
      root.style.setProperty("--reading-progress", total > 0 ? Math.min(1, Math.max(0, window.scrollY / total)) : 0);
      scheduled = false;
    }
    function scheduleProgress() {
      if (!scheduled) { scheduled = true; requestAnimationFrame(updateProgress); }
    }
    window.addEventListener("scroll", scheduleProgress, {passive: true});
    window.addEventListener("resize", scheduleProgress);
    window.addEventListener("pageshow", scheduleProgress);
    if (typeof ResizeObserver === "function") new ResizeObserver(scheduleProgress).observe(document.body);
    updateProgress();
  }
})();
