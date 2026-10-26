"use strict";
// Interruptible local transitions and one optional hero illustration.
(() => {
  function createMotion(options = {}) {
    const doc = options.document || (typeof document !== "undefined" ? document : null);
    const media = options.media || (typeof window !== "undefined" && window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : {matches: true});
    const active = new Map();
    function cancel(node) {const animation = active.get(node); if (animation) {active.delete(node); animation.cancel();}}
    function cancelAll() {for (const node of [...active.keys()]) cancel(node);}
    function animate(node, frames, settings = {}) {
      cancel(node);
      if (!node?.animate || media.matches || doc?.hidden) return null;
      let animation;
      try {animation = node.animate(frames, {duration: 220, easing: "cubic-bezier(.22,1,.36,1)", ...settings});}
      catch {return null;} // The final DOM state remains usable without WAAPI.
      active.set(node, animation);
      const release = () => {if (active.get(node) === animation) active.delete(node);};
      animation.onfinish = release; animation.oncancel = release;
      return animation;
    }
    const preferenceChanged = () => {if (media.matches) cancelAll();};
    if (media.addEventListener) media.addEventListener("change", preferenceChanged);
    else media.addListener?.(preferenceChanged);
    doc?.addEventListener("visibilitychange", () => {if (doc.hidden) cancelAll();});
    return {animate, cancel, cancelAll};
  }
  function moveIndicator(group, selected) {
    if (!group || !selected || !group.getBoundingClientRect || !selected.getBoundingClientRect) return;
    const from = group.getBoundingClientRect(), to = selected.getBoundingClientRect();
    if (!to.width || !to.height) return;
    for (const [name, value] of Object.entries({x: to.left - from.left + group.scrollLeft, y: to.top - from.top + group.scrollTop, width: to.width, height: to.height})) group.style.setProperty(`--active-${name}`, `${value}px`);
    group.dataset.indicator = "ready";
  }
  function createDemo(controller, options = {}) {
    if (!controller || controller.kind !== "hero") return null;
    const doc = options.document || (typeof document !== "undefined" ? document : null);
    const media = options.media || (typeof window !== "undefined" && window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : {matches: true});
    const schedule = options.setTimeout || setTimeout, unschedule = options.clearTimeout || clearTimeout;
    const Observer = options.IntersectionObserver ?? (typeof IntersectionObserver !== "undefined" ? IntersectionObserver : null);
    const replay = controller.root.querySelector("[data-ime-replay]");
    const pause = controller.root.querySelector("[data-ime-pause]");
    const timers = new Set();
    let generation = 0, started = false, cancelled = false;
    function cancel() {
      cancelled = true; generation++;
      for (const id of timers) unschedule(id);
      timers.clear();
      if (pause) pause.hidden = true;
    }
    function later(fn, delay) {
      const token = generation;
      const id = schedule(() => {
        timers.delete(id);
        if (token === generation && !cancelled && !doc?.hidden && !media.matches) fn();
      }, delay);
      timers.add(id);
    }
    function play(explicit = false) {
      if (doc?.hidden || (!explicit && (started || cancelled))) return;
      cancel(); cancelled = false; started = true;
      controller.prepareReplay?.();
      controller.setInput("nihao");
      if (media.matches) {
        if (explicit) controller.commitFirst();
        return;
      }
      if (pause) {pause.hidden = false; pause.setAttribute?.("aria-pressed", "false");}
      controller.setInput("");
      const spelling = "nihao";
      for (let index = 1; index <= spelling.length; index++) later(() => controller.setInput(spelling.slice(0, index)), index * 80);
      later(() => {controller.commitFirst(); cancel();}, spelling.length * 80 + 380);
    }
    replay?.addEventListener("click", () => play(true));
    pause?.addEventListener("click", () => {cancel(); pause.setAttribute?.("aria-pressed", "true");});
    controller.onTakeover(cancel);
    controller.root.addEventListener("pointerenter", cancel);
    controller.root.addEventListener("focusin", cancel);
    doc?.addEventListener("visibilitychange", () => {if (doc.hidden) cancel();});
    const onMotionPreference = () => {if (media.matches) cancel();};
    if (media.addEventListener) media.addEventListener("change", onMotionPreference);
    else if (media.addListener) media.addListener(onMotionPreference);
    if (!media.matches && !doc?.hidden) {
      if (Observer) {
        try {
          const observer = new Observer(entries => {
            if (entries.some(entry => entry.isIntersecting)) play();
            else if (started) cancel();
          }, {threshold: 0});
          observer.observe(controller.root);
        } catch {play();}
      } else play();
    }
    return {cancel, replay: () => play(true)};
  }
  const api = {createDemo, createMotion, moveIndicator};
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (typeof window !== "undefined" && typeof document !== "undefined") {
    api.transitions = createMotion();
    window.ModernIMEMotion = api;
    const start = () => {
      const experience = window.ModernIMEExperience;
      if (!experience) return;
      const controllers = experience.controllers || experience.init?.() || [];
      const hero = controllers.find(item => item.kind === "hero");
      if (hero) api.demo = createDemo(hero);
      // Headings establish the reading rhythm; controls and content are never hidden.
      if (typeof IntersectionObserver !== "undefined") {
        const observer = new IntersectionObserver(entries => {
          for (const entry of entries) if (entry.isIntersecting) {
            observer.unobserve(entry.target);
            [...entry.target.children].forEach((node, index) => api.transitions.animate(node, [{opacity: .35, transform: "translateY(12px)"}, {opacity: 1, transform: "none"}], {duration: 420, delay: index * 45, fill: "backwards"}));
          }
        }, {threshold: .25});
        document.querySelectorAll(".section-heading").forEach(node => observer.observe(node));
      }
    };
    if (document.readyState === "loading" || !window.ModernIMEExperience) document.addEventListener("DOMContentLoaded", start, {once: true});
    else start();
  }
})();
