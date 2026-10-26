"use strict";
const assert = require("node:assert/strict");
const {test} = require("node:test");
const experience = require("../../website/experience.js");

class Element {
  constructor() {
    this.children = []; this.listeners = {}; this.dataset = {}; this.attributes = {}; this.value = "";
    this.textContent = ""; this.hidden = false; this.clientWidth = 500;
    this.style = {setProperty: (key, value) => {this.style[key] = value;}};
    const values = new Set(); this.classList = {add: x => values.add(x), toggle: (x, yes) => yes ? values.add(x) : values.delete(x), contains: x => values.has(x)};
  }
  addEventListener(name, fn) {(this.listeners[name] ||= []).push(fn);}
  emit(name, options = {}) {const event = {target: this, ...options, preventDefault() {this.prevented = true;}}; for (const fn of this.listeners[name] || []) fn(event); return event;}
  querySelector(name) {return this.named?.[name] || null;}
  querySelectorAll(name) {return this.groups?.[name] || [];}
  setAttribute(name, value) {this.attributes[name] = value;}
  append(...nodes) {this.children.push(...nodes);}
  replaceChildren(...nodes) {this.children = nodes;}
  focus() {this.focused = true; this.emit("focus");}
}
function fixture(kind = "lab") {
  const root = new Element(); root.dataset.imeDemo = kind;
  const start = new Element(); root.ownerDocument = {createElement: () => new Element(), querySelector: name => name === "[data-start-demo]" ? start : null};
  const names = ["input", "candidates", "output", "status", "mode", "prev", "next", "page", "clear", "font", "capacity", "theme", "font-value", "capacity-value"];
  const fields = Object.fromEntries(names.map(name => [name, new Element()]));
  root.named = Object.fromEntries(names.map(name => [`[data-ime-${name}]`, fields[name]]));
  for (const name of ["domain-options", "example-title", "example-caption", "learning-reset", "learning-rank"]) {fields[name] = new Element(); root.named[`[data-${name}]`] = fields[name];}
  const examples = ["typo", "learning", "domain", "clipboard"].map(example => {const e = new Element(); e.dataset.example = example; return e;});
  const domains = ["it", "finance", "medical", "law", "research", "daily"].map(domain => {const e = new Element(); e.dataset.domain = domain; return e;});
  root.groups = {"[data-example]": examples, "[data-domain]": domains};
  fields.font.value = "18"; fields.capacity.value = "9";
  return {root, fields, start, examples, domains, controller: experience.mount(root)};
}

test("finite examples and progressive nihao candidates", () => {
  const model = experience.createModel();
  model.setInput("n"); assert.equal(model.candidates.length, 0);
  for (const value of ["ni", "nih", "niha", "nihao"]) {model.setInput(value); assert.equal(model.candidates[0].text, value === "ni" ? "你" : "你好");}
  model.setInput("zongwen"); assert.equal(model.candidates[0].text, "中文");
  model.setInput("unknown"); model.commitRaw(); assert.equal(model.output, "unknown");
});

test("learning advances only on explicit 实践 selection and can reset", () => {
  const model = experience.createModel(); model.setExample("learning");
  assert.equal(model.candidates.findIndex(x => x.text === "实践") + 1, 5);
  model.select(0); model.setInput("shijian"); assert.equal(model.candidates.findIndex(x => x.text === "实践") + 1, 5);
  model.select(4); assert.equal(model.input, "shijian"); assert.equal(model.candidates.findIndex(x => x.text === "实践") + 1, 3);
  model.select(2); assert.equal(model.candidates.findIndex(x => x.text === "实践") + 1, 1);
  model.resetLearning(); assert.equal(model.candidates.findIndex(x => x.text === "实践") + 1, 5);
});

test("six domains and fixed clipboard examples remain within finite model", () => {
  const model = experience.createModel(); const words = {it: "分布式系统", finance: "资产负债表", medical: "磁共振成像", law: "知识产权", research: "文献综述", daily: "待会儿见"};
  for (const [name, word] of Object.entries(words)) {model.setDomain(name); assert.equal(model.candidates[0].text, word);}
  model.setExample("clipboard"); assert.equal(model.input, "v2"); assert.equal(model.candidates.length, 3);
  model.setInput("v123"); assert.ok(model.candidates.length > 0);
});

test("Space selects first candidate on current physical page; digits do not intercept V input", () => {
  const {controller, fields} = fixture(); fields.candidates.clientWidth = 75;
  controller.setInput("shijian"); assert.ok(controller.pageCount > 1);
  fields.next.emit("click"); const selected = fields.candidates.children[0].children[1].textContent;
  assert.equal(fields.candidates.children[0].classList.contains("chosen"), true);
  fields.input.emit("keydown", {key: " "}); assert.equal(fields.output.textContent, selected);
  controller.setInput("v"); const event = fields.input.emit("keydown", {key: "2"}); assert.equal(event.prevented, undefined);
  assert.equal(fields.input.value, "v");
  controller.setInput("v2");
  const chooseClipboard = fields.input.emit("keydown", {key: "2"});
  assert.equal(chooseClipboard.prevented, true);
  assert.equal(fields.output.textContent, "下次见，也带上你的新想法。");
});

test("keyboard boundaries, composition and raw submission stay in input", () => {
  const {controller, fields} = fixture(); controller.setInput("nihao");
  let takeovers = 0; controller.onTakeover(() => {takeovers++;});
  fields.input.emit("keydown", {key: "Shift"}); fields.input.emit("keyup", {key: "Shift"}); assert.equal(controller.model.mode, "en");
  fields.input.emit("keydown", {key: "Shift"}); fields.input.emit("keydown", {key: "A", shiftKey: true}); fields.input.emit("keyup", {key: "Shift"}); assert.equal(controller.model.mode, "en");
  fields.input.emit("keydown", {key: "Shift", ctrlKey: true}); fields.input.emit("keyup", {key: "Shift", ctrlKey: true}); assert.equal(controller.model.mode, "en");
  fields.input.emit("compositionstart"); assert.equal(takeovers, 1);
  fields.input.emit("keydown", {key: " ", isComposing: true}); fields.input.emit("compositionend"); assert.equal(fields.output.textContent, "");
  controller.setInput("unknown"); fields.input.emit("keydown", {key: "Enter"}); assert.equal(fields.output.textContent, "unknown");
  controller.setInput("nihao"); fields.input.emit("keydown", {key: "Escape"}); assert.equal(controller.model.input, "");
});

test("settings only changes preview, and learning rank stays scene-specific", () => {
  const app = fixture("settings"); const before = app.controller.model.candidates.map(x => x.text); assert.equal(before.length, 9);
  assert.equal(app.root.style["--ime-font"], "18pt");
  app.fields.font.value = "28"; app.fields.font.emit("input"); app.fields.capacity.value = "3"; app.fields.capacity.emit("input"); app.fields.theme.checked = true; app.fields.theme.emit("change");
  assert.deepEqual(app.controller.model.candidates.map(x => x.text), before); assert.equal(app.root.dataset.theme, "dark"); assert.equal(app.root.style["--ime-font"], "28pt");
  const lab = fixture(); assert.equal(lab.root.style["--ime-font"], undefined); assert.equal(lab.fields["learning-rank"].hidden, true);
  lab.examples[1].emit("click"); assert.equal(lab.fields["learning-rank"].hidden, false);
  lab.examples[0].emit("click"); assert.equal(lab.fields["learning-rank"].hidden, true);
});

test("computed font size controls physical pages; clipboard displays all rows", () => {
  const old = global.getComputedStyle; global.getComputedStyle = () => ({fontSize: "38px"});
  try {
    const {controller, fields, examples} = fixture("settings"); fields.candidates.clientWidth = 180;
    controller.setInput("shijian"); assert.equal(controller.pageCount, 6);
    for (let i = 0; i < 5; i++) fields.next.emit("click"); fields.candidates.children[0].emit("click"); assert.equal(fields.output.textContent, "时见");
    const lab = fixture(); lab.fields.candidates.clientWidth = 90; examples[3].emit("click");
    lab.examples[3].emit("click"); assert.equal(lab.fields.candidates.children.length, 3);
  } finally {global.getComputedStyle = old;}
});

test("candidate buttons use the new component class without the legacy fixed font class", () => {
  const {fields} = fixture("settings");
  const first = fields.candidates.children[0];
  assert.equal(first.classList.contains("ime-candidate"), true);
  assert.equal(first.classList.contains("candidate"), false);
});

test("detached candidate cannot select and external hero start focuses input", () => {
  const app = fixture("lab"); app.fields.candidates.clientWidth = 70; app.controller.setInput("shijian"); const stale = app.fields.candidates.children[0];
  app.fields.next.emit("click"); stale.emit("click"); assert.equal(app.fields.output.textContent, "");
  const hero = fixture("hero"); hero.start.emit("click"); assert.equal(hero.fields.input.focused, true);
});

test("ordinary text matching Object prototype names remains raw input", () => {
  const model = experience.createModel();
  for (const text of ["constructor", "toString", "__proto__", "hasOwnProperty"]) {
    model.setInput(text);
    assert.deepEqual(model.candidates, []);
    model.commitRaw();
    assert.equal(model.output, text);
  }
});

test("layout refresh does not overwrite an active native composition", () => {
  const {controller, fields} = fixture("hero");
  fields.input.emit("compositionstart");
  fields.input.value = "正在输入";
  controller.refresh();
  assert.equal(fields.input.value, "正在输入");
  fields.input.emit("compositionend");
  assert.equal(controller.model.input, "正在输入");
});

test("choosing a sample restores Chinese candidates after English mode", () => {
  const model = experience.createModel();
  model.toggleMode(); model.setExample("learning");
  assert.equal(model.mode, "zh");
  assert.equal(model.candidates[4].text, "实践");
  model.toggleMode(); model.setDomain("medical");
  assert.equal(model.candidates[0].text, "磁共振成像");
});
