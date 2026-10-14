"use strict";
// Test the shipped script with a minimal DOM: no npm or browser dependency.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../../website/app.js"), "utf8");
class Element {
  constructor(text = "") { this.textContent = text; this.children = []; this.events = {}; this.attributes = {}; this.dataset = {}; this.firstChild = {textContent: text}; this.classList = {toggle: (name, value) => {this.attributes[name] = value;}}; }
  addEventListener(name, action) { this.events[name] = action; }
  setAttribute(name, value) { this.attributes[name] = value; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
}
async function run(release, options = {}) {
  const elements = Object.fromEntries(["demo-pinyin", "demo-candidates", "ubuntu-version", "deb-download", "copy-install", "copy-status", "install-command", "release-version", "release-status", "rpm-download", "portable-download", "checksum-link"].map(id => [id, new Element()]));
  elements["ubuntu-version"].value = "ubuntu24.04";
  elements["install-command"].textContent = "git clone https://github.com/example/project.git\n./install.sh";
  const buttons = ["single", "double"].map(sample => {const b = new Element(); b.dataset.demo = sample; return b;});
  let copied = "";
  let selected = false;
  const document = {getElementById: id => elements[id], querySelectorAll: () => buttons, createElement: () => new Element(), createTextNode: text => new Element(text), createRange: () => ({selectNodeContents: () => { selected = true; }})};
  const context = vm.createContext({document, navigator: {clipboard: {writeText: async text => {if (options.copyFailure) throw Error("denied"); copied = text;}}}, window: {getSelection: () => ({removeAllRanges() {}, addRange() {}})}, AbortController, setTimeout, clearTimeout, fetch: async () => {if (!release) throw Error("offline"); return {ok: true, json: async () => release};}});
  vm.runInContext(source, context);
  await new Promise(resolve => setImmediate(resolve));
  return {elements, buttons, copied: () => copied, selected: () => selected};
}
const tag = "v0.1.11";
const base = `https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases/download/${tag}/`;
const names = ["modernime_0.1.11_amd64_ubuntu24.04.deb", "modernime_0.1.11_amd64_ubuntu22.04.deb", "modernime-0.1.11-1.oe24.x86_64.rpm", "modernime-0.1.11-linux-x86_64.tar.gz", "SHA256SUMS.txt"];
const fixture = {tag_name: tag, assets: names.map(name => ({name, browser_download_url: base + name}))};
(async () => {
  const app = await run(fixture);
  const e = app.elements;
  assert.equal(e["release-version"].textContent, "最新发布 v0.1.11");
  assert.equal(e["deb-download"].href, base + names[0]);
  e["ubuntu-version"].value = "ubuntu22.04";
  e["ubuntu-version"].events.change();
  assert.equal(e["deb-download"].href, base + names[1]);
  assert.equal(e["rpm-download"].href, base + names[2]);
  assert.equal(e["portable-download"].href, base + names[3]);
  assert.equal(e["checksum-link"].href, base + names[4]);
  app.buttons[1].events.click();
  assert.equal(e["demo-candidates"].children.length, 7);
  assert.equal(app.buttons[1].attributes["aria-pressed"], "true");
  app.buttons[0].events.click();
  assert.equal(e["demo-candidates"].children.length, 9);
  await e["copy-install"].events.click();
  assert.equal(app.copied(), e["install-command"].textContent);
  const offline = await run(null, {copyFailure: true});
  offline.elements["ubuntu-version"].events.change();
  assert.match(offline.elements["deb-download"].href, /v0\.1\.10\/.*ubuntu24\.04\.deb$/);
  assert.match(offline.elements["release-status"].textContent, /暂时无法/);
  await offline.elements["copy-install"].events.click();
  assert.equal(offline.selected(), true);
  assert.match(offline.elements["copy-status"].textContent, /Ctrl\+C/);
  const missing = await run({tag_name: tag, assets: [{name: names[0], browser_download_url: "https://evil.example/package.deb"}]});
  assert.match(missing.elements["deb-download"].href, /\/releases\/latest$/);
  assert.match(missing.elements["rpm-download"].href, /\/releases\/latest$/);
  const prerelease = await run({...fixture, prerelease: true});
  assert.match(prerelease.elements["release-status"].textContent, /暂时无法/);
  console.log("Website interactions passed: candidates, package selection, release sync/fallback, safe URLs, copy/fallback.");
})().catch(error => { console.error(error); process.exitCode = 1; });
