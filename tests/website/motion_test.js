"use strict";
// Exercise the shipped controller with deterministic browser events and time.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const motionPath = path.join(__dirname, "../../website/motion.js");
const source = fs.readFileSync(motionPath, "utf8");
class Element {
  constructor() {
    this.events = {}; this.attributes = {}; this.dataset = {}; this.textContent = "";
    this.classList = {add: key => {this.attributes[key] = true;}, remove: key => {delete this.attributes[key];}, toggle: (key, value) => {this.attributes[key] = value;}};
  }
  addEventListener(name, fn) { (this.events[name] ||= []).push(fn); }
  emit(name, event = {}) { (this.events[name] || []).forEach(fn => fn(event)); }
  setAttribute(name, value) { this.attributes[name] = value; }
  append() {}
  replaceChildren() {}
  contains(target) { return target === this; }
}
function setup({reduce = false, observer = false, brokenObserver = false} = {}) {
  const elements = Object.fromEntries(["hero-prelude", "hero-context", "hero-prefix", "hero-word", "hero-pinyin", "hero-candidates", "hero-motion-toggle", "hero-motion-replay"].map(id => [id, new Element()]));
  elements["hero-word"].textContent = "好好表达";
  elements["hero-pinyin"].textContent = "hao'hao'biao'da";
  const hero = new Element(), root = new Element(), document = new Element();
  const buttons = ["hello", "express", "linux"].map(scene => {const b = new Element(); b.dataset.scene = scene; return b;});
  const media = new Element(); media.matches = reduce;
  const observers = [], tasks = new Map(); let now = 0, id = 0;
  Object.assign(document, {hidden: false, documentElement: root, getElementById: id => elements[id], querySelector: selector => selector === ".hero-demo" ? hero : null, querySelectorAll: selector => selector === "[data-scene]" ? buttons : [], createElement: () => new Element(), createTextNode: text => ({textContent: text})});
  class Observer {
    constructor(fn) { if (brokenObserver) throw Error("Observer unavailable"); this.fn = fn; observers.push(this); }
    observe(target) { this.target = target; }
    unobserve() {}
    disconnect() {}
  }
  vm.runInNewContext(source, {document, window: {matchMedia: () => media}, IntersectionObserver: observer ? Observer : undefined, setTimeout: (fn, delay) => {tasks.set(++id, {fn, at: now + delay}); return id;}, clearTimeout: key => tasks.delete(key)});
  function advance(ms) {
    const end = now + ms;
    for (;;) {
      const next = [...tasks].sort((a,b) => a[1].at - b[1].at)[0];
      if (!next || next[1].at > end) break;
      now = next[1].at; tasks.delete(next[0]); next[1].fn();
    }
    now = end;
  }
  return {elements, hero, root, document, buttons, media, observers, tasks, advance};
}
// Missing IntersectionObserver must not disable the independent hero demo.
const app = setup();
app.advance(8000);
assert.notEqual(app.elements["hero-word"].textContent, "好好表达", "visible hero should advance even without IntersectionObserver");
app.elements["hero-motion-toggle"].emit("click");
const paused = app.elements["hero-word"].textContent;
app.advance(20000);
assert.equal(app.elements["hero-word"].textContent, paused, "explicit pause must stop automatic scene changes");
assert.equal(app.tasks.size, 0, "pause must clear typing and rotation timers");
app.elements["hero-motion-replay"].emit("click");
app.advance(1000);
assert.ok(app.elements["hero-pinyin"].textContent.length > 4, "replay must finish typing while automatic rotation remains paused");
app.advance(20000);
assert.equal(app.elements["hero-word"].textContent, paused);
app.hero.emit("focusin");
app.elements["hero-motion-toggle"].emit("click");
app.advance(8000);
assert.notEqual(app.elements["hero-word"].textContent, paused, "explicit resume must work while its button still has focus");
const beforeHide = app.elements["hero-word"].textContent;
app.document.hidden = true; app.document.emit("visibilitychange");
assert.equal(app.tasks.size, 0, "hidden tabs must stop all demo timers");
app.document.hidden = false; app.document.emit("visibilitychange");
app.advance(8000);
assert.notEqual(app.elements["hero-word"].textContent, beforeHide, "returning to the page should resume rotation");
app.media.matches = true; app.media.emit("change");
assert.equal(app.tasks.size, 0);
app.buttons[0].emit("click");
assert.equal(app.elements["hero-pinyin"].textContent, "ni'hao", "reduced motion keeps manual scene selection immediate");
app.media.matches = false; app.media.emit("change");
app.advance(8000);
assert.notEqual(app.elements["hero-word"].textContent, "你好", "motion should recover when the system preference is restored");
const beforeHover = app.elements["hero-word"].textContent;
app.hero.emit("pointerenter"); app.advance(20000);
assert.equal(app.elements["hero-word"].textContent, beforeHover, "hovering must keep the current scene available to read");
app.hero.emit("pointerleave");
app.hero.emit("focusin"); app.advance(20000);
assert.equal(app.elements["hero-word"].textContent, beforeHover, "keyboard focus must also suspend rotation");
app.hero.emit("focusout", {relatedTarget: null}); app.advance(8000);
assert.notEqual(app.elements["hero-word"].textContent, beforeHover);
const reduced = setup({reduce: true});
assert.equal(reduced.tasks.size, 0);
const observed = setup({observer: true});
const heroObserver = observed.observers.find(item => item.target === observed.hero);
heroObserver.fn([{isIntersecting: true}]); observed.advance(500);
heroObserver.fn([{isIntersecting: false}]);
assert.equal(observed.tasks.size, 0, "offscreen hero must release all timers");
heroObserver.fn([{isIntersecting: true}]); observed.advance(8000);
assert.notEqual(observed.elements["hero-word"].textContent, "好好表达");
const fallback = setup({observer: true, brokenObserver: true});
fallback.advance(8000);
assert.notEqual(fallback.elements["hero-word"].textContent, "好好表达", "observer failure must retain the hero and controls");
console.log("Motion lifecycle passed: no-observer fallback, pause/replay, hidden/offscreen suspension, live reduced-motion recovery.");
