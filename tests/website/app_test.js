"use strict";
// Test the shipped script with a minimal DOM: no npm or browser dependency.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../../website/app.js"), "utf8");
class Element {
  constructor(text = "") { this.textContent = text; this.children = []; this.events = {}; this.attributes = {}; this.dataset = {}; this.firstChild = {textContent: text}; this.style = {setProperty: (name, value) => {this.attributes[name] = value;}}; this.classList = {toggle: (name, value) => {this.attributes[name] = value;}, add: name => {this.attributes[name] = true;}, remove: name => {delete this.attributes[name];}}; }
  addEventListener(name, action) { const previous = this.events[name]; this.events[name] = (...args) => {const first = previous ? previous(...args) : undefined; return Promise.all([first, action(...args)]);}; }
  setAttribute(name, value) { this.attributes[name] = value; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
}
async function run(release, options = {}) {
  const elements = Object.fromEntries(["demo-pinyin", "demo-candidates", "ubuntu-version", "deb-download", "copy-install", "copy-status", "install-command", "release-version", "release-status", "rpm-download", "portable-download", "checksum-link", "hero-prelude", "hero-context", "hero-prefix", "hero-word", "hero-pinyin", "hero-candidates", "client-screenshot", "screenshot-original", "screenshot-caption", "screenshot-pagination"].map(id => [id, new Element()]));
  elements["ubuntu-version"].value = "ubuntu24.04";
  elements["install-command"].textContent = "git clone https://github.com/example/project.git\n./install.sh";
  const buttons = ["single", "double"].map(sample => {const b = new Element(); b.dataset.demo = sample; return b;});
  const sceneButtons = ["hello", "express", "linux"].map(scene => {const b = new Element(); b.dataset.scene = scene; return b;});
  const screenshotButtons = ["overview", "appearance", "clipboard"].map(screenshot => {const b = new Element(); b.dataset.screenshot = screenshot; return b;});
  const reveal = new Element();
  const root = new Element();
  root.scrollHeight = 2000;
  root.clientHeight = 800;
  const observers = [];
  const typing = new Map();
  let timerId = 0;
  let mediaChanged;
  const media = {matches: !options.motion, addEventListener: (event, action) => {mediaChanged = action;}};
  if (options.motion) elements["client-screenshot"].animate = (frames, timing) => {elements["client-screenshot"].lastAnimation = timing;};
  let copied = "";
  let selected = false;
  const document = {documentElement: root, addEventListener() {}, getElementById: id => elements[id], querySelectorAll: selector => selector === "[data-demo]" ? buttons : selector === "[data-scene]" ? sceneButtons : selector === "[data-screenshot]" ? screenshotButtons : options.motion ? [reveal] : [], createElement: () => new Element(), createTextNode: text => new Element(text), createRange: () => ({selectNodeContents: () => { selected = true; }})};
  class Observer { constructor(callback) {this.callback = callback; observers.push(this);} observe(element) {this.target = element;} unobserve() {this.unobserved = true;} disconnect() {this.disconnected = true;} }
  const context = vm.createContext({document, IntersectionObserver: options.motion ? Observer : undefined, navigator: {clipboard: {writeText: async text => {if (options.copyFailure) throw Error("denied"); copied = text;}}}, window: {matchMedia: () => media, getSelection: () => ({removeAllRanges() {}, addRange() {}})}, AbortController, setTimeout: (action, delay) => {if (delay === 55) {const id = ++timerId; typing.set(id, action); return id;} return setTimeout(action, delay);}, clearTimeout: id => {if (typeof id === "number") typing.delete(id); else clearTimeout(id);}, fetch: async () => {if (!release) throw Error("offline"); return {ok: true, json: async () => release};}});
  vm.runInContext(source, context);
  await new Promise(resolve => setImmediate(resolve));
  return {elements, buttons, sceneButtons, screenshotButtons, root, reveal, observers, typing, reduce: () => {media.matches = true; mediaChanged();}, copied: () => copied, selected: () => selected};
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
  await app.sceneButtons[2].events.click();
  assert.equal(e["hero-word"].textContent, "自由生长");
  assert.equal(e["hero-pinyin"].textContent, "zi'you'sheng'zhang");
  assert.equal(app.sceneButtons[2].attributes["aria-pressed"], "true");
  await app.screenshotButtons[1].events.click();
  assert.equal(e["client-screenshot"].src, "assets/client-appearance.png");
  assert.equal(e["screenshot-original"].href, "assets/client-appearance.png");
  assert.equal(e["screenshot-pagination"].textContent, "02 / 03");
  await app.screenshotButtons[2].events.click();
  assert.equal(e["screenshot-original"].href, "assets/client-clipboard.png");
  assert.equal(app.screenshotButtons[1].attributes["aria-pressed"], "false");
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
  const motion = await run(fixture, {motion: true});
  assert.equal(motion.root.attributes["motion-ready"], true);
  motion.observers[0].callback([{isIntersecting: true, target: motion.reveal}]);
  assert.equal(motion.reveal.attributes["is-visible"], true);
  assert.equal(motion.observers[0].unobserved, true);
  await motion.screenshotButtons[1].events.click();
  assert.equal(motion.elements["client-screenshot"].lastAnimation.duration, 430);
  motion.reduce();
  assert.equal(motion.observers[0].disconnected, true);
  assert.equal(motion.root.attributes["motion-ready"], undefined);
  assert.equal(motion.typing.size, 0);
  assert.equal(motion.elements["hero-pinyin"].textContent, "hao'hao'biao'da");
  console.log("Website interactions passed: candidates, scenes, gallery/original links, package selection, release sync/fallback, safe URLs, copy/fallback, motion API fallback.");
})().catch(error => { console.error(error); process.exitCode = 1; });
