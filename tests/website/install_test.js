"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
async function tests() {
  const file = path.join(__dirname, "../../website/install.js");
  assert.ok(fs.existsSync(file), "recommended repository installation needs an independent copy/selection controller");
  const source = fs.readFileSync(file, "utf8");
  async function run(denied = false) {
    const elements = {};
    function element(id) {return elements[id] ||= {textContent: "", dataset: {}, events: {}, focus() {this.focused = true;}, addEventListener(name, action) {this.events[name] = action;}};}
    const options = ["ubuntu24.04", "openeuler", "other"].map(value => ({...element(value), value, checked: value === "ubuntu24.04"}));
    const panels = options.map(option => ({...element("panel-" + option.value), dataset: {installPanel: option.value}, hidden: false}));
    element("apt-source-command").textContent = "curl -fsSL https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/apt/client/modernime-noble.sources";
    const copy = element("copy"); copy.dataset.copy = "apt-source-command"; copy.dataset.status = "apt-copy-status";
    const open = element("open"); open.dataset.openInstall = "apt-install";
    const mobileLink = element("mobile-link");
    element("mobile-menu").contains = target => target === mobileLink; element("mobile-menu-toggle");
    const keyboard = {};
    let copied, selected = false;
    const document = {getElementById: element, addEventListener: (name, action) => {keyboard[name] = action;}, querySelector: () => options.find(item => item.checked), querySelectorAll: selector => selector === "input[name=install-system]" ? options : selector === "[data-install-panel]" ? panels : selector === "[data-copy]" ? [copy] : selector === "[data-open-install]" ? [open] : selector === "[data-mobile-link]" ? [mobileLink] : [], createRange: () => ({selectNodeContents: () => {selected = true;}})};
    const window = {getSelection: () => ({removeAllRanges() {}, addRange() {}})};
    vm.runInNewContext(source, {document, window, navigator: {clipboard: {writeText: async text => {if (denied) throw Error("denied"); copied = text;}}}});
    return {elements, options, panels, copy, open, mobileLink, keyboard, copied: () => copied, selected: () => selected};
  }
  const app = await run();
  assert.deepEqual(app.panels.map(panel => panel.hidden), [false, true, true]);
  app.options[0].checked = false; app.options[1].checked = true;
  app.options[1].events.change();
  assert.deepEqual(app.panels.map(panel => panel.hidden), [true, false, true]);
  app.options[1].checked = false; app.options[2].checked = true;
  app.options[2].events.change();
  assert.deepEqual(app.panels.map(panel => panel.hidden), [true, true, false]);
await app.copy.events.click();
  assert.equal(app.copied(), app.elements["apt-source-command"].textContent);
  app.open.events.click();
  assert.equal(app.elements["apt-install"].open, true);
  app.elements["mobile-menu"].open = true;
  app.mobileLink.events.click();
  assert.equal(app.elements["mobile-menu"].open, false);
  app.elements["mobile-menu"].open = true;
  app.keyboard.keydown({key: "Escape"});
  assert.equal(app.elements["mobile-menu"].open, false);
  assert.equal(app.elements["mobile-menu-toggle"].focused, true);
  app.elements["mobile-menu"].open = true;
  app.keyboard.pointerdown({target: app.mobileLink});
  assert.equal(app.elements["mobile-menu"].open, true, "inside clicks must retain native menu behavior");
  app.keyboard.pointerdown({target: {}});
  assert.equal(app.elements["mobile-menu"].open, false, "outside clicks must dismiss the menu");
  for (const value of ["jammy", "bookworm", "../../unsafe"]) {
    app.options[2].value = value;
    app.options[2].events.change();
    assert.deepEqual(app.panels.map(panel => panel.hidden), [true, true, false], "invalid systems must retain current instructions");
  }
  const denied = await run(true);
  await denied.copy.events.click();
  assert.equal(denied.selected(), true);
  assert.match(denied.elements["apt-copy-status"].textContent, /Ctrl\+C/);
  console.log("Repository installation passed: suite mapping, copy/fallback, native disclosure, invalid-suite rejection.");
}
module.exports = tests;
if (require.main === module) tests().catch(error => {console.error(error); process.exitCode = 1;});
