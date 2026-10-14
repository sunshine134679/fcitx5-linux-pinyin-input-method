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
  getAttribute(name) { return this.attributes[name]; }
  setAttribute(name, value) { this.attributes[name] = value; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
}
async function run(release, options = {}) {
  const elements = Object.fromEntries(["demo-pinyin", "demo-candidates", "deb-download", "copy-install", "copy-status", "install-command", "release-version", "release-status", "rpm-download", "portable-download", "checksum-link", "hero-prelude", "hero-context", "hero-prefix", "hero-word", "hero-pinyin", "hero-candidates", "client-screenshot", "gallery-previous", "screenshot-original", "screenshot-caption", "screenshot-pagination", "gallery-status"].map(id => [id, new Element()]));
  elements["install-command"].textContent = "git clone https://github.com/example/project.git\n./install.sh";
  const buttons = ["single", "double"].map(sample => {const b = new Element(); b.dataset.demo = sample; return b;});
  const sceneButtons = ["hello", "express", "linux"].map(scene => {const b = new Element(); b.dataset.scene = scene; return b;});
  const html = fs.readFileSync(path.join(__dirname, "../../website/index.html"), "utf8");
  const systemOptions = [...html.matchAll(/<input[^>]*name="deb-system"[^>]*>/g)].map(([tag]) => {
    const option = new Element();
    option.value = tag.match(/value="([^"]+)"/)[1];
    option.checked = tag.includes("checked");
    return option;
  });
  const workflowButtons = ["chat", "write", "reuse"].map(name => {const b = new Element(); b.dataset.workflow = name; return b;});
  const workflowPanels = workflowButtons.map(button => {const panel = new Element(); panel.dataset.workflowPanel = button.dataset.workflow; return panel;});
  const screenshotButtons = [...html.matchAll(/<button\b[^>]*data-screenshot=[^>]*>/g)].map(([tag]) => {
    const b = new Element();
    for (const [, key, value] of tag.matchAll(/data-([a-z]+)="([^"]*)"/g)) b.dataset[key] = value;
    return b;
  });
  elements["client-screenshot"].complete = true;
  elements["client-screenshot"].naturalWidth = options.initialFailure ? 0 : 2940;
  elements["client-screenshot"].src = "assets/client-overview-1x.webp";
  elements["client-screenshot"].sizes = "980px";
  elements["screenshot-pagination"].textContent = "01 / 03";
  elements["screenshot-original"].href = "assets/client-overview-3x.png";
  const pendingImages = [];
  const imageRequests = [];
  let finishDecode;
  class Image {
    set src(value) {
      this.path = value;
      imageRequests.push(value);
      if (options.manualImages) pendingImages.push(this);
      else queueMicrotask(() => options.imageFailure ? this.onerror() : this.onload());
    }
  }
  Image.prototype.decode = options.manualDecode ? () => new Promise(resolve => {finishDecode = resolve;}) : async () => {};
  if (options.imageFailure) elements["client-screenshot"].decode = async () => {throw Error("load failed");};
  const reveal = new Element();
  const root = new Element();
  root.scrollHeight = 2000;
  root.clientHeight = 800;
  const observers = [];
  const typing = new Map();
  let timerId = 0;
  const mediaChanged = [];
  const media = {matches: !options.motion, addEventListener: (event, action) => {mediaChanged.push(action);}};
  if (options.motion) for (const id of ["client-screenshot", "gallery-previous"]) elements[id].animate = (frames, timing) => {elements[id].lastAnimation = timing; return {cancel() {elements[id].cancelled = true;}};};
  let copied = "";
  let selected = false;
  const document = {documentElement: root, addEventListener() {}, getElementById: id => elements[id], querySelector: () => systemOptions.find(option => option.checked), querySelectorAll: selector => selector === "input[name=deb-system]" ? systemOptions : selector === "[data-workflow]" ? workflowButtons : selector === "[data-workflow-panel]" ? workflowPanels : selector === "[data-demo]" ? buttons : selector === "[data-scene]" ? sceneButtons : selector === "[data-screenshot]" ? screenshotButtons : options.motion ? [reveal] : [], createElement: () => new Element(), createTextNode: text => new Element(text), createRange: () => ({selectNodeContents: () => { selected = true; }})};
  class Observer { constructor(callback) {this.callback = callback; observers.push(this);} observe(element) {this.target = element;} unobserve() {this.unobserved = true;} disconnect() {this.disconnected = true;} }
  const context = vm.createContext({document, Image, IntersectionObserver: options.motion ? Observer : undefined, navigator: {connection: {saveData: !!options.saveData}, clipboard: {writeText: async text => {if (options.copyFailure) throw Error("denied"); copied = text;}}}, window: {matchMedia: () => media, getSelection: () => ({removeAllRanges() {}, addRange() {}})}, AbortController, setTimeout: (action, delay) => {if (delay === 55) {const id = ++timerId; typing.set(id, action); return id;} return setTimeout(action, delay);}, clearTimeout: id => {if (typeof id === "number") typing.delete(id); else clearTimeout(id);}, fetch: async () => {if (!release) throw Error("offline"); return {ok: true, json: async () => release};}});
  if (options.galleryOnly) {
    // The gallery still initializes when the separate main script is missing.
  } else {
    vm.runInContext(source, context);
  }
  const galleryFile = path.join(__dirname, "../../website/gallery.js");
  vm.runInContext(fs.readFileSync(galleryFile, "utf8"), context);
  await new Promise(resolve => setImmediate(resolve));
  return {pendingImages, imageRequests, finishDecode: () => finishDecode(), elements, systemOptions, workflowButtons, workflowPanels, buttons, sceneButtons, screenshotButtons, root, reveal, observers, typing, reduce: () => {media.matches = true; mediaChanged.forEach(action => action());}, copied: () => copied, selected: () => selected};
}
const tag = "v0.1.12";
const base = `https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases/download/${tag}/`;
const names = ["modernime_0.1.12_amd64_ubuntu24.04.deb", "modernime_0.1.12_amd64_ubuntu22.04.deb", "modernime-0.1.12-1.oe24.x86_64.rpm", "modernime-0.1.12-linux-x86_64.tar.gz", "SHA256SUMS.txt"];
const fixture = {tag_name: tag, assets: names.map(name => ({name, browser_download_url: base + name}))};
(async () => {
  await require("./install_test.js")();
  const standaloneGallery = await run(null, {galleryOnly: true});
  await standaloneGallery.screenshotButtons[1].events.pointerenter?.();
  assert.equal(typeof standaloneGallery.screenshotButtons[1].events.pointerenter, "function", "pointer intent should warm the screenshot before clicking");
  await standaloneGallery.screenshotButtons[1].events.click();
  assert.equal(standaloneGallery.elements["client-screenshot"].src, "assets/client-appearance-1x.webp");
  assert.match(standaloneGallery.elements["client-screenshot"].srcset || "", /980w/, "gallery switches must retain responsive source candidates");
  const failedGallery = await run(fixture, {imageFailure: true});
  await failedGallery.screenshotButtons[1].events.click();
  assert.equal(failedGallery.elements["client-screenshot"].src, "assets/client-overview-1x.webp", "a failed load must keep the last usable screenshot");
  assert.equal(failedGallery.elements["screenshot-pagination"].textContent, "01 / 03");
  assert.match(failedGallery.elements["gallery-status"].textContent, /重试/);
  const initialFailure = await run(null, {initialFailure: true, manualImages: true});
  const recovered = initialFailure.screenshotButtons[0].events.click();
  assert.equal(initialFailure.pendingImages.length, 1, "clicking the current tab must retry a failed initial image");
  initialFailure.pendingImages[0].onload();
  await recovered;
  assert.equal(initialFailure.elements["screenshot-original"].href, "assets/client-overview-3x.png");
  const racingGallery = await run(fixture, {manualImages: true});
  const first = racingGallery.screenshotButtons[1].events.click();
  const latest = racingGallery.screenshotButtons[2].events.click();
  assert.equal(racingGallery.pendingImages[0].srcset, racingGallery.screenshotButtons[1].dataset.srcset, "preloads and displayed images must use the same responsive candidates");
  assert.equal(racingGallery.pendingImages[0].sizes, racingGallery.elements["client-screenshot"].sizes);
  assert.equal(racingGallery.elements["client-screenshot"].src, "assets/client-overview-1x.webp");
  racingGallery.pendingImages[1].onload();
  await latest;
  racingGallery.pendingImages[0].onload();
  await first;
  assert.equal(racingGallery.elements["client-screenshot"].src, "assets/client-clipboard-1x.webp", "a late earlier request must not replace the latest choice");
  assert.equal(racingGallery.elements["screenshot-pagination"].textContent, "03 / 03");
  assert.equal(racingGallery.elements["client-screenshot"].attributes["aria-busy"], "false");
  // Superseded successful loads are still decoded and cached for a later visit.
  await racingGallery.screenshotButtons[1].events.click();
  assert.equal(racingGallery.pendingImages.length, 2);
  assert.equal(racingGallery.elements["screenshot-original"].href, "assets/client-appearance-3x.png");
  const retryGallery = await run(null, {manualImages: true});
  const failed = retryGallery.screenshotButtons[1].events.click();
  retryGallery.pendingImages[0].onerror();
  await failed;
  const retried = retryGallery.screenshotButtons[1].events.click();
  retryGallery.pendingImages[1].onload();
  await retried;
  assert.equal(retryGallery.elements["screenshot-pagination"].textContent, "02 / 03");
  const decoding = await run(null, {manualImages: true, manualDecode: true});
  const decoded = decoding.screenshotButtons[1].events.click();
  decoding.pendingImages[0].onload();
  assert.equal(decoding.elements["client-screenshot"].src, "assets/client-overview-1x.webp", "keep the old frame until decoding finishes");
  decoding.finishDecode();
  await decoded;
  assert.equal(decoding.elements["client-screenshot"].src, "assets/client-appearance-1x.webp");
  const warmed = await run(null, {galleryOnly: true, motion: true});
  warmed.observers[0].callback([{isIntersecting: true}]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(warmed.imageRequests.length, 3, "warm all frames before the gallery enters view");
  await warmed.screenshotButtons[1].events.click();
  await warmed.screenshotButtons[2].events.click();
  await warmed.screenshotButtons[1].events.click();
  await warmed.screenshotButtons[0].events.click();
  assert.equal(warmed.imageRequests.length, 3, "switching decoded frames must not issue another image load");
  assert.equal(warmed.elements["gallery-previous"].hidden, false, "crossfade keeps the outgoing frame visible");
  warmed.reduce();
  assert.equal(warmed.elements["gallery-previous"].hidden, true);
  assert.ok(!fs.readFileSync(path.join(__dirname, "../../website/index.html"), "utf8").includes("<select"), "system versions must be visible choices, not a popup covering download actions");
  const app = await run(fixture);
  const e = app.elements;
  assert.equal(e["release-version"].textContent, "最新发布 v0.1.12");
  assert.equal(e["deb-download"].href, base + names[0]);
  app.systemOptions[0].checked = false;
  app.systemOptions[1].checked = true;
  await app.systemOptions[1].events.change();
  assert.equal(e["deb-download"].href, base + names[1]);
  app.systemOptions[1].checked = false; app.systemOptions[2].checked = true;
  await app.systemOptions[2].events.change();
  assert.equal(e["deb-download"].href, base + names[1], "Debian 12 must retain the compatible package while using its own Bookworm source");
  assert.equal(e["rpm-download"].href, base + names[2]);
  assert.equal(e["portable-download"].href, base + names[3]);
  assert.equal(e["checksum-link"].href, base + names[4]);
  await app.workflowButtons[1].events.click();
  assert.equal(app.workflowPanels[1].hidden, false);
  assert.equal(app.workflowPanels[0].hidden, true);
  assert.equal(app.workflowButtons[1].attributes["aria-pressed"], "true");
  await app.workflowButtons[2].events.click();
  assert.equal(app.workflowPanels[2].hidden, false);
  assert.equal(app.workflowPanels[1].hidden, true);
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
  assert.equal(e["client-screenshot"].src, "assets/client-appearance-1x.webp");
  assert.equal(e["screenshot-original"].href, "assets/client-appearance-3x.png");
  assert.equal(e["screenshot-pagination"].textContent, "02 / 03");
  await app.screenshotButtons[2].events.click();
  assert.equal(e["screenshot-original"].href, "assets/client-clipboard-3x.png");
  assert.equal(app.screenshotButtons[1].attributes["aria-pressed"], "false");
  await e["copy-install"].events.click();
  assert.equal(app.copied(), e["install-command"].textContent);
  const offline = await run(null, {copyFailure: true});
  await offline.systemOptions[0].events.change();
  assert.match(offline.elements["deb-download"].href, /v0\.1\.11\/.*ubuntu24\.04\.deb$/);
  assert.match(offline.elements["release-status"].textContent, /暂时无法/);
  await offline.elements["copy-install"].events.click();
  assert.equal(offline.selected(), true);
  assert.match(offline.elements["copy-status"].textContent, /Ctrl\+C/);
  const missing = await run({tag_name: tag, assets: [{name: names[0], browser_download_url: "https://evil.example/package.deb"}]});
  assert.match(missing.elements["deb-download"].href, /\/releases\/latest$/);
  assert.match(missing.elements["rpm-download"].href, /\/releases\/latest$/);
  const prerelease = await run({...fixture, prerelease: true});
  assert.match(prerelease.elements["release-status"].textContent, /暂时无法/);
  const saving = await run(null, {galleryOnly: true, motion: true, saveData: true});
  assert.equal(saving.observers.length, 0, "data-saving mode must not start automatic preload");
  await saving.screenshotButtons[1].events.click();
  assert.equal(saving.imageRequests.length, 1);
  const motion = await run(fixture, {motion: true});
  assert.equal(motion.root.attributes["motion-ready"], true);
  motion.observers[0].callback([{isIntersecting: true, target: motion.reveal}]);
  assert.equal(motion.reveal.attributes["is-visible"], true);
  assert.equal(motion.observers[0].unobserved, true);
  await motion.screenshotButtons[1].events.click();
  assert.equal(motion.elements["client-screenshot"].lastAnimation.duration, 420);
  motion.reduce();
  assert.equal(motion.observers[0].disconnected, true);
  assert.equal(motion.root.attributes["motion-ready"], undefined);
  assert.equal(motion.typing.size, 0);
  assert.equal(motion.elements["hero-pinyin"].textContent, "hao'hao'biao'da");
  console.log("Website interactions passed: candidates, everyday scenes, decoded gallery cache/crossfade/races/retry/save-data, original links, package selection, release sync/fallback, safe URLs, copy/fallback, motion API fallback.");
})().catch(error => { console.error(error); process.exitCode = 1; });
