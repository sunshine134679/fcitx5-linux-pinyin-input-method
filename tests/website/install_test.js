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
    const options = ["noble"].map(suite => ({...element(suite), dataset: {suite}, checked: suite === "noble"}));
    const copy = element("copy"); copy.dataset.copy = "apt-source-command"; copy.dataset.status = "apt-copy-status";
    const open = element("open"); open.dataset.openInstall = "apt-install";
    const mobileLink = element("mobile-link");
    element("mobile-menu").contains = target => target === mobileLink; element("mobile-menu-toggle");
    const keyboard = {};
    let copied, selected = false;
    const document = {getElementById: element, addEventListener: (name, action) => {keyboard[name] = action;}, querySelector: () => options.find(item => item.checked), querySelectorAll: selector => selector === "input[name=deb-system]" ? options : selector === "[data-copy]" ? [copy] : selector === "[data-open-install]" ? [open] : selector === "[data-mobile-link]" ? [mobileLink] : [], createRange: () => ({selectNodeContents: () => {selected = true;}})};
    const window = {getSelection: () => ({removeAllRanges() {}, addRange() {}})};
    vm.runInNewContext(source, {document, window, navigator: {clipboard: {writeText: async text => {if (denied) throw Error("denied"); copied = text;}}}});
    return {elements, options, copy, open, mobileLink, keyboard, copied: () => copied, selected: () => selected};
  }
  const app = await run();
assert.match(app.elements["apt-source-command"].textContent, /apt\/client\/modernime-noble\.sources/);
assert.match(app.elements["apt-system-name"].textContent, /Ubuntu 24\.04/);
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
  const existing = app.elements["apt-source-command"].textContent;
  app.options[0].dataset.suite = "../../unsafe";
  app.options[0].events.change();
  assert.equal(app.elements["apt-source-command"].textContent, existing, "unrecognized suites must not enter copied commands");
  const denied = await run(true);
  await denied.copy.events.click();
  assert.equal(denied.selected(), true);
  assert.match(denied.elements["apt-copy-status"].textContent, /Ctrl\+C/);
  console.log("Repository installation passed: suite mapping, copy/fallback, native disclosure, invalid-suite rejection.");
}
module.exports = tests;
if (require.main === module) tests().catch(error => {console.error(error); process.exitCode = 1;});
