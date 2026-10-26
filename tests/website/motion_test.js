"use strict";
const assert = require("node:assert/strict");
const {test} = require("node:test");
const fs = require("node:fs"), vm = require("node:vm");
const {createDemo, createMotion} = require("../../website/motion.js");
class Target {
  constructor() {this.listeners = {}; this.children = {}; this.hidden = false; this.matches = false; this.textContent = ""; this.attributes = {};}
  addEventListener(name, fn) {(this.listeners[name] ||= []).push(fn);}
  emit(name, event = {}) {for (const fn of this.listeners[name] || []) fn(event);}
  querySelector(name) {return this.children[name] || null;}
  setAttribute(name, value) {this.attributes[name] = value;}
}
function setup({reduced = false, observer = false} = {}) {
  const root = new Target(), document = new Target(), media = new Target(); media.matches = reduced;
  const replay = new Target(), pause = new Target(); pause.hidden = true;
  root.children["[data-ime-replay]"] = replay; root.children["[data-ime-pause]"] = pause;
  const controller = {kind: "hero", root, input: "nihao", output: "", takeover: null,
    prepared: 0, prepareReplay() {this.prepared++; this.output = "";},
    setInput(value) {this.input = value;}, commitFirst() {this.output = "你好"; this.input = "";}, onTakeover(fn) {this.takeover = fn;}};
  let time = 0, nextId = 0, watcher; const timers = new Map();
  const env = {document, media, setTimeout: (fn, ms) => {timers.set(++nextId, {fn, at: time + ms}); return nextId;}, clearTimeout: id => timers.delete(id)};
  if (observer) env.IntersectionObserver = class {constructor(fn) {watcher = fn;} observe() {}};
  const demo = createDemo(controller, env);
  function advance(ms) {
    const end = time + ms;
    for (;;) {const task = [...timers].sort((a, b) => a[1].at - b[1].at)[0]; if (!task || task[1].at > end) break; time = task[1].at; timers.delete(task[0]); task[1].fn();}
    time = end;
  }
  return {controller, root, document, media, replay, pause, timers, advance, get watcher() {return watcher;}, demo};
}
test("one playback commits once then hides pause control", () => {
  const app = setup(); assert.equal(app.pause.hidden, false);
  app.advance(3000); assert.equal(app.controller.output, "你好"); assert.equal(app.pause.hidden, true); assert.equal(app.timers.size, 0);
  app.advance(30000); assert.equal(app.controller.output, "你好"); assert.equal(app.timers.size, 0);
});
test("takeover stops all delayed actions and hides pause", () => {
  const app = setup(); app.advance(100); app.controller.takeover(); app.controller.setInput("mine");
  app.advance(3000); assert.equal(app.controller.input, "mine"); assert.equal(app.controller.output, ""); assert.equal(app.pause.hidden, true);
});
test("replay cancels old playback and begins a fresh one", () => {
  const app = setup(); app.advance(150); app.replay.emit("click"); app.advance(500);
  assert.equal(app.controller.output, "");
  app.advance(1000); assert.equal(app.controller.output, "你好"); assert.equal(app.timers.size, 0);
});
test("pause, hidden, offscreen and reduced motion never revive stale actions", () => {
  const paused = setup(); paused.pause.emit("click"); paused.advance(3000); assert.equal(paused.controller.output, ""); assert.equal(paused.pause.hidden, true);
  const hidden = setup(); hidden.document.hidden = true; hidden.document.emit("visibilitychange"); hidden.document.hidden = false; hidden.document.emit("visibilitychange"); hidden.advance(3000); assert.equal(hidden.controller.output, "");
  const off = setup({observer: true}); off.watcher([{isIntersecting: true}]); off.advance(100); off.watcher([{isIntersecting: false}]); off.watcher([{isIntersecting: true}]); off.advance(3000); assert.equal(off.controller.output, "");
  const reduced = setup(); reduced.media.matches = true; reduced.media.emit("change"); reduced.media.matches = false; reduced.media.emit("change"); reduced.advance(3000); assert.equal(reduced.controller.output, "");
});
test("reduced motion preserves input and explicit replay shows final state", () => {
  const app = setup({reduced: true}); assert.equal(app.controller.input, "nihao"); assert.equal(app.timers.size, 0);
  app.replay.emit("click"); assert.equal(app.controller.output, "你好"); assert.equal(app.timers.size, 0);
});

test("initially offscreen demo plays on first entry and does not restart after leaving", () => {
  const app = setup({observer: true});
  app.watcher([{isIntersecting: false}]);
  assert.equal(app.timers.size, 0);
  app.watcher([{isIntersecting: true}]);
  app.advance(3000);
  assert.equal(app.controller.output, "你好");
  app.controller.setInput("mine");
  app.watcher([{isIntersecting: false}]);
  app.watcher([{isIntersecting: true}]);
  app.advance(3000);
  assert.equal(app.controller.input, "mine");
});

test("replay prepares a fresh input demonstration", () => {
  const app=setup({reduced:true}); app.controller.output="旧结果";
  app.replay.emit("click"); assert.equal(app.controller.prepared,1);
});

test("local animation lifecycle cancels superseded, reduced and background motion", () => {
  const document=new Target(), media=new Target();
  const motion=createMotion({document,media});
  const made=[];
  const node={animate(frames,options) {const a={frames,options,cancelled:false,cancel(){this.cancelled=true;}}; made.push(a); return a;}};
  const first=motion.animate(node,[{opacity:0},{opacity:1}]);
  motion.animate(node,[{opacity:.5},{opacity:1}]); assert.equal(first.cancelled,true);
  media.matches=true; media.emit("change"); assert.equal(made[1].cancelled,true);
  assert.equal(motion.animate(node,[{opacity:0},{opacity:1}]),null);
  media.matches=false; document.hidden=false;
  const last=motion.animate(node,[{opacity:0},{opacity:1}]);
  document.hidden=true; document.emit("visibilitychange"); assert.equal(last.cancelled,true);
});

test("deferred motion waits for the following experience script while document is interactive", () => {
  const document=new Target(), media=new Target(), root=new Target(), replay=new Target();
  document.readyState="interactive"; document.querySelectorAll=()=>[]; media.matches=true;
  root.children["[data-ime-replay]"]=replay;
  let committed=0;
  const controller={kind:"hero",root,setInput(){},prepareReplay(){},commitFirst(){committed++;},onTakeover(){}};
  const window={matchMedia:()=>media};
  vm.runInNewContext(fs.readFileSync(require.resolve("../../website/motion.js"),"utf8"),{window,document,setTimeout,clearTimeout});
  window.ModernIMEExperience={controllers:[controller]};
  document.emit("DOMContentLoaded"); replay.emit("click");
  assert.equal(committed,1,"replay must be wired after both deferred scripts finish");
});
