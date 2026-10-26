"use strict";
// A single optional hero illustration. Input and candidates belong to experience.js.
(() => {
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
  const api = {createDemo};
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (typeof window !== "undefined" && typeof document !== "undefined") {
    const start = () => {
      const experience = window.ModernIMEExperience;
      if (!experience) return;
      const controllers = experience.controllers || experience.init?.() || [];
      const hero = controllers.find(item => item.kind === "hero");
      if (hero) api.demo = createDemo(hero);
    };
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, {once: true});
    else start();
  }
})();
