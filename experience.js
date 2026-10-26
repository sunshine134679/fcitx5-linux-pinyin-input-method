"use strict";
// Finite, local website examples. No engine, clipboard, storage or network access.
(() => {
  const WORDS = Object.freeze({
    ni: ["你", "尼", "呢", "泥", "拟", "逆", "妮", "倪", "霓"],
    nih: ["你好"], niha: ["你好"], nihao: ["你好", "你号", "拟好"],
    zongwen: ["中文", "重温", "正文"],
    shijian: ["时间", "事件", "世间", "试件", "实践", "时见"],
    linux: ["Linux", "林纳斯", "玲珑"],
    fenbushixitong: ["分布式系统", "分布式", "系统"],
    zichanfuzhaibiao: ["资产负债表", "资产", "负债表"],
    cigongzhenchengxiang: ["磁共振成像", "磁共振", "成像"],
    zhishichanquan: ["知识产权", "知识", "产权"],
    wenxianzongshu: ["文献综述", "文献", "综述"],
    daihuerjian: ["待会儿见", "待会儿", "再见"],
    v123: ["一百二十三", "壹佰贰拾叁"]
  });
  const DOMAINS = Object.freeze({it: "fenbushixitong", finance: "zichanfuzhaibiao", medical: "cigongzhenchengxiang", law: "zhishichanquan", research: "wenxianzongshu", daily: "daihuerjian"});
  const CLIPBOARD = Object.freeze(["https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method", "下次见，也带上你的新想法。", "一字一句，慢慢积累。"]);
  const EXAMPLES = Object.freeze({
    typo: {input: "zongwen", title: "拼音容错", caption: "输入 zongwen，试试选「中文」。"},
    learning: {input: "shijian", title: "习惯学习", caption: "选两次「实践」，看看它从第 5 位来到首位。"},
    domain: {input: DOMAINS.it, title: "专业词库", caption: "选择一个领域，输入熟悉的专业词。"},
    clipboard: {input: "v2", title: "V + 2 示例剪贴板", caption: "三条固定样例，点击即可上屏。"}
  });
  const clamp = (value, low, high) => Math.max(low, Math.min(high, Number(value) || low));

  function createModel(initial = "") {
    return {
      input: initial, output: "", mode: "zh", example: "typo", domain: "it", learningSelections: 0, font: 18, capacity: 9, theme: "light",
      get candidates() {
        if (!this.input || this.mode === "en") return [];
        const key = this.input.trim().toLowerCase();
        let words = key === "v2" ? CLIPBOARD : Object.hasOwn(WORDS, key) ? WORDS[key] : null;
        if (!words) return [];
        if (key === "shijian" && this.learningSelections) {
          words = [...words]; words.splice(4, 1); words.splice(this.learningSelections === 1 ? 2 : 0, 0, "实践");
        }
        return words.map(text => ({id: `${key}:${text}`, text}));
      },
      setInput(value) {this.input = String(value);},
      setExample(name) {if (!Object.hasOwn(EXAMPLES, name)) return; this.mode = "zh"; this.example = name; this.input = name === "domain" ? DOMAINS[this.domain] : EXAMPLES[name].input; this.output = "";},
      setDomain(name) {if (!Object.hasOwn(DOMAINS, name)) return; this.mode = "zh"; this.domain = name; this.example = "domain"; this.input = DOMAINS[name]; this.output = "";},
      select(index) {
        const item = this.candidates[index]; if (!item) return false;
        this.output = item.text;
        if (this.example === "learning" && this.input.trim().toLowerCase() === "shijian" && item.text === "实践") {
          this.learningSelections = Math.min(2, this.learningSelections + 1); this.input = "shijian";
        } else this.input = "";
        return true;
      },
      commitRaw() {if (!this.input) return false; this.output = this.input; this.input = ""; return true;},
      clear() {this.input = ""; this.output = "";},
      resetLearning() {this.learningSelections = 0; if (this.example === "learning") {this.mode = "zh"; this.input = "shijian"; this.output = "";}},
      toggleMode() {this.mode = this.mode === "zh" ? "en" : "zh";},
      setSettings({font = this.font, capacity = this.capacity, theme = this.theme}) {
        this.font = clamp(font, 14, 28); this.capacity = clamp(capacity, 3, 9); this.theme = theme === "dark" ? "dark" : "light";
      }
    };
  }

  function mount(root) {
    if (root._imeController) return root._imeController;
    const one = name => root.querySelector(name), many = name => root.querySelectorAll(name);
    const input = one("[data-ime-input]"), candidates = one("[data-ime-candidates]");
    if (!input || !candidates) return null;
    const kind = root.dataset.imeDemo || "lab";
    const model = createModel(root.dataset.imeInitial || ({hero: "nihao", lab: "zongwen", settings: "ni"}[kind] || ""));
    const output = one("[data-ime-output]"), status = one("[data-ime-status]");
    const mode = one("[data-ime-mode]"), prev = one("[data-ime-prev]"), next = one("[data-ime-next]");
    const pageLabel = one("[data-ime-page]"), clear = one("[data-ime-clear]");
    const font = one("[data-ime-font]"), capacity = one("[data-ime-capacity]"), theme = one("[data-ime-theme]");
    const fontValue = one("[data-ime-font-value]"), capacityValue = one("[data-ime-capacity-value]");
    const exampleTitle = one("[data-example-title]"), exampleCaption = one("[data-example-caption]");
    const domainOptions = one("[data-domain-options]"), learningReset = one("[data-learning-reset]");
    const learningRank = one("[data-learning-rank]");
    const nodes = new Map(), takeoverListeners = new Set();
    const motionAPI = typeof window !== "undefined" ? window.ModernIMEMotion : null;
    const motion = motionAPI?.transitions;
    const scenarioNav = one(".scenario-nav");
    let rendered = "", initialized = false;
    let pages = [], page = 0, composing = false, bareShift = false, shiftChord = false;
    function signalTakeover() {for (const listener of takeoverListeners) listener();}

    function pageLayout(items) {
      if (!items.length) return [];
      if (model.input.trim().toLowerCase() === "v2" && model.mode === "zh") return [items];
      const width = Math.max(1, candidates.clientWidth || root.clientWidth || 320);
      const computed = typeof getComputedStyle === "function" ? Number.parseFloat(getComputedStyle(candidates).fontSize) : 0;
      const fontPx = computed > 0 ? computed : kind === "settings" ? model.font * 4 / 3 : 15;
      const result = []; let current = [], used = 0;
      for (const item of items) {
        // Includes the number, button padding and gap. Long words wrap in their one full-width button.
        const amount = Math.min(width, 34 + Array.from(item.text).length * fontPx);
        if (current.length && (current.length >= model.capacity || used + amount > width)) {result.push(current); current = []; used = 0;}
        current.push(item); used += amount;
      }
      if (current.length) result.push(current);
      return result;
    }
    function refresh() {
      if (!composing) input.value = model.input;
      if (output && output.textContent !== model.output) {
        output.textContent = model.output;
        if (model.output) motion?.animate(output, [{opacity: .25, transform: "translateY(7px)"}, {opacity: 1, transform: "none"}], {duration: 200});
        else motion?.cancel(output);
      }
      if (mode) {mode.textContent = model.mode === "zh" ? "中" : "英"; mode.setAttribute("aria-pressed", String(model.mode === "en"));}
      if (kind === "settings") {
        root.style.setProperty("--ime-font", `${model.font}pt`); root.style.setProperty("--ime-capacity", String(model.capacity)); root.dataset.theme = model.theme;
        if (fontValue) fontValue.textContent = `${model.font} pt`;
        if (capacityValue) capacityValue.textContent = `${model.capacity} 个`;
      }
      if (exampleTitle) exampleTitle.textContent = EXAMPLES[model.example].title;
      if (exampleCaption) exampleCaption.textContent = EXAMPLES[model.example].caption;
      if (domainOptions) domainOptions.hidden = model.example !== "domain";
      if (learningReset) learningReset.hidden = model.example !== "learning";
      if (learningRank) {learningRank.hidden = model.example !== "learning"; learningRank.textContent = `实践：第 ${[5, 3, 1][model.learningSelections]} 位`;}
      for (const button of many("[data-example]")) button.setAttribute("aria-pressed", String(button.dataset.example === model.example));
      for (const button of many("[data-domain]")) button.setAttribute("aria-pressed", String(button.dataset.domain === model.domain));
      motionAPI?.moveIndicator(scenarioNav, one("[data-example][aria-pressed=true]"));

      const items = model.candidates; pages = pageLayout(items); page = Math.min(page, Math.max(0, pages.length - 1));
      const shown = pages[page] || [];
      const signature = JSON.stringify([shown.map(item => item.id), model.font, model.capacity, model.theme]);
      const changed = signature !== rendered;
      const oldRects = new Map();
      if (changed) for (const node of candidates.children) {
        if (node.dataset?.candidateId && node.getBoundingClientRect) oldRects.set(node.dataset.candidateId, node.getBoundingClientRect());
        motion?.cancel(node);
      }
      const shownNodes = shown.map((item, local) => {
        let node = nodes.get(item.id);
        if (!node) {
          node = root.ownerDocument.createElement("button"); node.type = "button";
          node.classList.add("ime-candidate"); node.dataset.candidateId = item.id;
          const number = root.ownerDocument.createElement("small"), word = root.ownerDocument.createElement("span");
          word.textContent = item.text; node.append(number, word); nodes.set(item.id, node);
          node.addEventListener("click", () => {if (selectId(item.id)) input.focus();});
        }
        node.children[0].textContent = String(local + 1);
        node.classList.toggle("chosen", local === 0);
        return node;
      });
      candidates.classList.toggle("clipboard-candidates", model.input.trim().toLowerCase() === "v2" && model.mode === "zh");
      if (changed) candidates.replaceChildren(...shownNodes);
      // Read all final geometry before starting animations; unchanged observer notifications do nothing.
      const positions = changed ? shownNodes.map(node => node.getBoundingClientRect?.()) : [];
      if (changed && initialized) shownNodes.forEach((node, index) => {
        const before = oldRects.get(node.dataset.candidateId), after = positions[index];
        if (!before) {
          motion?.animate(node, [{opacity: 0, transform: "translateY(6px)"}, {opacity: 1, transform: "none"}], {duration: 200, delay: model.input === "v2" ? index * 25 : 0, fill: "backwards"});
          return;
        }
        if (!after) return;
        const dx = before.left - after.left, dy = before.top - after.top;
        if (Math.abs(dx) + Math.abs(dy) >= 1) motion?.animate(node, [{transform: `translate(${dx}px, ${dy}px)`}, {transform: "translate(0, 0)"}]);
      });
      rendered = signature; initialized = true;
      if (pageLabel) pageLabel.textContent = pages.length ? `${page + 1} / ${pages.length}` : "0 / 0";
      if (prev) prev.disabled = page === 0;
      if (next) next.disabled = page >= pages.length - 1;
      if (status) {
        status.setAttribute("role", "status");
        status.textContent = !model.input ? "试着输入拼音" : items.length ? `${items.length} 个候选 · 第 ${page + 1} 页` : "继续输入，回车也可上屏原文";
      }
    }
    function selectId(id) {
      if (!(pages[page] || []).some(item => item.id === id)) return false;
      const index = model.candidates.findIndex(item => item.id === id);
      if (index >= 0 && model.select(index)) {page = 0; refresh(); return true;}
      return false;
    }
    function selectVisible(index) {const item = (pages[page] || [])[index]; if (item) selectId(item.id);}
    function movePage(delta) {if (!pages.length) return; page = Math.max(0, Math.min(pages.length - 1, page + delta)); refresh();}
    function setInput(value) {model.setInput(value); page = 0; refresh();}
    const controller = {
      kind, root, model, setInput, refresh, selectVisible, movePage,
      get pageCount() {return pages.length;}, get pageSize() {return (pages[page] || []).length;},
      onTakeover(listener) {takeoverListeners.add(listener); return () => takeoverListeners.delete(listener);},
      prepareReplay() {model.mode = "zh"; model.clear(); page = 0; refresh();},
      commitFirst() {if (pages[page]?.length) selectVisible(0); else {model.commitRaw(); page = 0; refresh();}},
      commitRaw() {model.commitRaw(); page = 0; refresh();}
    };
    root._imeController = controller;
    input.addEventListener("input", () => {signalTakeover(); if (!composing) setInput(input.value);});
    input.addEventListener("focus", signalTakeover); root.addEventListener("pointerdown", signalTakeover);
    input.addEventListener("blur", () => {bareShift = false; shiftChord = false;});
    input.addEventListener("compositionstart", () => {signalTakeover(); composing = true; bareShift = false;});
    input.addEventListener("compositionend", () => {composing = false; setInput(input.value);});
    input.addEventListener("keydown", event => {
      if (composing || event.isComposing || event.keyCode === 229) return;
      if (event.key === "Shift") {bareShift = !(event.ctrlKey || event.altKey || event.metaKey); shiftChord = false; return;}
      if (bareShift) shiftChord = true;
      if (event.ctrlKey || event.altKey || event.metaKey) return;
      if (event.key === "Escape") {event.preventDefault(); model.setInput(""); page = 0; refresh(); return;}
      if (event.key === "PageUp" || event.key === "PageDown") {event.preventDefault(); movePage(event.key === "PageDown" ? 1 : -1); return;}
      if (event.key === "Enter") {if (model.input) {event.preventDefault(); controller.commitRaw();} return;}
      if ((event.key === " " || event.key === "Spacebar") && model.input && model.mode === "zh") {event.preventDefault(); controller.commitFirst(); return;}
      if (/^[1-9]$/.test(event.key) && model.input && model.mode === "zh") {
        // Before V commands form a candidate, their digits still reach the input.
        const index = Number(event.key) - 1;
        if (index < (pages[page] || []).length) {event.preventDefault(); selectVisible(index);}
      }
    });
    input.addEventListener("keyup", event => {
      if (event.key !== "Shift") return;
      if (!composing && !event.isComposing && bareShift && !shiftChord && !event.ctrlKey && !event.altKey && !event.metaKey) {model.toggleMode(); refresh();}
      bareShift = false; shiftChord = false;
    });
    mode?.addEventListener("click", () => {model.toggleMode(); refresh(); input.focus();});
    prev?.addEventListener("click", () => {movePage(-1); input.focus();}); next?.addEventListener("click", () => {movePage(1); input.focus();});
    clear?.addEventListener("click", () => {model.clear(); page = 0; refresh(); input.focus();});
    if (kind === "hero") (one("[data-start-demo]") || root.ownerDocument.querySelector?.("[data-start-demo]"))?.addEventListener("click", () => input.focus());
    for (const button of many("[data-example]")) button.addEventListener("click", () => {model.setExample(button.dataset.example); page = 0; refresh(); motion?.animate(one(".workbench-heading"), [{opacity: .35, transform: "translateY(5px)"}, {opacity: 1, transform: "none"}]); input.focus();});
    for (const button of many("[data-domain]")) button.addEventListener("click", () => {model.setDomain(button.dataset.domain); page = 0; refresh(); input.focus();});
    learningReset?.addEventListener("click", () => {model.resetLearning(); page = 0; refresh(); input.focus();});
    function updateSettings() {model.setSettings({font: font?.value ?? model.font, capacity: capacity?.value ?? model.capacity, theme: theme?.checked ? "dark" : "light"}); page = 0; refresh();}
    font?.addEventListener("input", updateSettings); capacity?.addEventListener("input", updateSettings); theme?.addEventListener("change", updateSettings);
    if (typeof ResizeObserver !== "undefined") {
      try {new ResizeObserver(refresh).observe(candidates);}
      catch {if (typeof window !== "undefined") window.addEventListener("resize", refresh);}
    } else if (typeof window !== "undefined") window.addEventListener("resize", refresh);
    refresh(); return controller;
  }
  function init(scope = document) {return [...scope.querySelectorAll("[data-ime-demo]")].map(mount).filter(Boolean);}
  const api = {createModel, mount, init, DOMAINS, CLIPBOARD};
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (typeof window !== "undefined" && typeof document !== "undefined") {
    window.ModernIMEExperience = api;
    const start = () => {api.controllers = init();};
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, {once: true}); else start();
  }
})();
