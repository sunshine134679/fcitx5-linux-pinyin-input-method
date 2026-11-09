'use strict';

(() => {
  // Palette values mirror RenderStyle::fromSettings in the native client.
  const palettes = {
    lavender: { name: '雾紫', panel: '#f7f3ff', ink: '#33284b', accent: '#7052ad', selectedInk: '#ffffff', muted: '#706080', border: '#d9ccec', canvas: '#eee9f6', paper: '#fdfbff' },
    jade: { name: '青竹绿', panel: '#f2faf6', ink: '#163c31', accent: '#087f63', selectedInk: '#ffffff', muted: '#527469', border: '#c2ddcf', canvas: '#e8f2ed', paper: '#fbfefa' },
    rose: { name: '玫瑰粉', panel: '#fff4f7', ink: '#452634', accent: '#a83d63', selectedInk: '#ffffff', muted: '#81586b', border: '#e7c4d0', canvas: '#f6e9ef', paper: '#fffbfd' },
    amber: { name: '暖砂橙', panel: '#fff8ed', ink: '#3c2b1b', accent: '#a65f16', selectedInk: '#ffffff', muted: '#7c654d', border: '#e4cdaa', canvas: '#f6eddf', paper: '#fffdf8' },
    'classic-blue': { name: '经典蓝', panel: '#ffffff', ink: '#0d1726', accent: '#1266ed', selectedInk: '#ffffff', muted: '#455266', border: '#dbe3ed', canvas: '#e8effb', paper: '#fcfdff' },
    light: { name: '浅色', panel: '#ffffff', ink: '#212121', accent: '#2b2b2b', selectedInk: '#ffffff', muted: '#6e6e6e', border: '#dbdbdb', canvas: '#eeeff1', paper: '#ffffff' },
    midnight: { name: '深海夜', panel: '#182536', ink: '#edf4ff', accent: '#89b9ff', selectedInk: '#142339', muted: '#b0c1d8', border: '#405570', canvas: '#162231', paper: '#213246', dark: true },
    dark: { name: '深色', panel: '#1f2124', ink: '#f0f2f5', accent: '#e0e3e6', selectedInk: '#1a1c1f', muted: '#adb0b3', border: '#4a4d4f', canvas: '#17191e', paper: '#25282e', dark: true },
    'high-contrast': { name: '高对比', panel: '#090909', ink: '#ffffff', accent: '#ffe066', selectedInk: '#111111', muted: '#d8d8d8', border: '#eeeeee', canvas: '#090909', paper: '#141414', dark: true }
  };
  // These are the same appearance examples as candidates_page.cpp, not query results.
  const samples = {
    single: { preedit: 'ni', words: ['你', '尼', '呢', '泥', '拟', '逆', '妮', '倪', '腻'] },
    double: { preedit: 'nihao', words: ['你好', '你号', '拟好', '拟合', '霓虹', '倪皓', '逆号', '泥壕', '妮好'] },
    mixed: { preedit: 'nihao', words: ['你好', '你号', '拟好', '你好啊', '你好呀', '你好吗', '你还好', '你好吧', '你好哇'] }
  };
  const shapeLabels = { rounded: '圆润', square: '方正', capsule: '胶囊' };
  const spacingLabels = { compact: '紧凑', standard: '标准', spacious: '宽松' };
  const defaults = { theme: 'lavender', shape: 'rounded', spacing: 'standard', fontSize: 20, pageSize: 9 };
  const storageKey = 'modernime-appearance-v1';
  const root = document.querySelector('#appearance-studio');
  const list = document.querySelector('#studio-candidates');
  const panel = document.querySelector('#studio-candidate-panel');
  const writing = document.querySelector('#studio-writing');
  const hint = document.querySelector('#studio-hint');
  let state = { ...defaults };
  try {
    const saved = JSON.parse(localStorage.getItem(storageKey));
    if (saved) {
      if (Object.hasOwn(palettes, saved.theme)) state.theme = saved.theme;
      if (Object.hasOwn(shapeLabels, saved.shape)) state.shape = saved.shape;
      if (Object.hasOwn(spacingLabels, saved.spacing)) state.spacing = saved.spacing;
      if (Number.isInteger(saved.fontSize) && saved.fontSize >= 14 && saved.fontSize <= 28) state.fontSize = saved.fontSize;
      if (Number.isInteger(saved.pageSize) && saved.pageSize >= 3 && saved.pageSize <= 9) state.pageSize = saved.pageSize;
    }
  } catch { /* Browser storage is optional; the live controls still work. */ }
  let sample = 'double';
  let pages = [];
  let pageIndex = 0;
  let cursor = 0;
  let layoutFrame;
  let pageMotion = 0;

  // Each theme has an original motif, drawn with its actual panel and accent colors.
  const paletteArt = {
    lavender: `<path d="M8 29Q22 22 37 28T64 26V40H0V32Z" fill="currentColor" opacity=".08"/>
      <path d="M33 32V19" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>
      <path d="M33 23C19 24 17 14 20 10C28 10 33 15 33 23Z" fill="currentColor" opacity=".35"/>
      <path d="M33 23C46 23 48 14 45 10C37 10 33 15 33 23Z" fill="currentColor" opacity=".6"/>
      <path d="M33 24C24 19 27 9 33 5C39 9 42 19 33 24Z" fill="currentColor"/>
      <path d="M10 33H24M39 31H53" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" opacity=".25"/>`,
    jade: `<path d="M29 35L34 5M29 24L40 15M31 16L22 11" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>
      <path d="M32 25C36 11 45 9 51 10C47 20 41 25 32 25Z" fill="currentColor" opacity=".8"/>
      <path d="M32 18C22 18 17 13 15 7C23 7 29 10 32 18Z" fill="currentColor" opacity=".45"/>
      <path d="M29 31C18 32 13 28 11 23C20 21 26 24 29 31Z" fill="currentColor" opacity=".3"/>
      <path d="M30 18H34M28 29H32" stroke="var(--swatch-panel)" stroke-width="2"/>`,
    rose: `<path d="M33 26V35M33 33C39 26 43 27 46 27C42 33 38 34 33 33Z" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M32 6C40 3 48 12 44 18C47 25 37 30 31 26C23 30 15 23 19 17C15 10 25 3 32 6Z" fill="currentColor" opacity=".15"/>
      <path d="M25 10C34 6 44 13 39 20C34 27 23 22 23 16C23 12 29 10 33 13C37 16 32 20 29 17" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>`,
    amber: `<circle cx="39" cy="14" r="8" fill="currentColor" opacity=".85"/>
      <path d="M0 28Q17 17 35 27T64 24V40H0Z" fill="currentColor" opacity=".2"/>
      <path d="M0 36Q21 24 41 32T64 29V40H0Z" fill="currentColor" opacity=".4"/>
      <path d="M8 12H18M12 16H22" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" opacity=".35"/>`,
    'classic-blue': `<path d="M11 30C5 20 20 16 23 23S14 35 8 34" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-dasharray="2 3" opacity=".4"/>
      <path d="M18 17L52 6L41 34L33 23Z" fill="currentColor" opacity=".18"/>
      <path d="M18 17L52 6L33 23Z" fill="currentColor"/>
      <path d="M33 23L41 34L52 6Z" fill="currentColor" opacity=".55"/>
      <path d="M33 23L29 30V21L52 6" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/>`,
    light: `<circle cx="32" cy="20" r="9" fill="currentColor" opacity=".08"/>
      <circle cx="32" cy="20" r="6" fill="none" stroke="currentColor" stroke-width="1.5"/>
      <path d="M32 5V9M32 31V35M17 20H21M43 20H47M21 9L24 12M40 28L43 31M21 31L24 28M40 12L43 9" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>
      <path d="M7 35H20M44 35H57" stroke="currentColor" stroke-width="1.2" opacity=".2"/>`,
    midnight: `<path d="M39 5A10 10 0 1 0 49 19A9 9 0 0 1 39 5Z" fill="currentColor"/>
      <circle cx="18" cy="11" r="1" fill="currentColor"/><circle cx="25" cy="6" r=".7" fill="currentColor" opacity=".6"/>
      <path d="M0 29Q8 25 16 29T32 29T48 29T64 29M0 35Q8 31 16 35T32 35T48 35T64 35" fill="none" stroke="currentColor" stroke-width="1.5" opacity=".4"/>`,
    dark: `<circle cx="32" cy="20" r="9" fill="currentColor" opacity=".12"/>
      <circle cx="32" cy="20" r="6" fill="currentColor" opacity=".8"/>
      <ellipse cx="32" cy="20" rx="23" ry="8" transform="rotate(-25 32 20)" fill="none" stroke="currentColor" stroke-width="1.2" opacity=".6"/>
      <circle cx="13" cy="29" r="2" fill="currentColor"/><path d="M48 6V10M46 8H50M15 9V12M13.5 10.5H16.5" stroke="currentColor" stroke-width="1.2" stroke-linecap="round"/>`,
    'high-contrast': `<circle cx="22" cy="20" r="11" fill="currentColor"/>
      <path d="M22 9A11 11 0 0 1 22 31Z" fill="var(--swatch-panel)"/>
      <path d="M40 9H47V31H40ZM51 9H54V31H51Z" fill="currentColor"/>
      <path d="M22 9A11 11 0 1 1 22 31" fill="none" stroke="currentColor" stroke-width="1.5"/>`
  };

  const paletteContainer = document.querySelector('#studio-palettes');
  Object.entries(palettes).forEach(([key, palette]) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.palette = key;
    button.setAttribute('aria-label', palette.name);
    const swatch = document.createElement('span');
    swatch.className = 'palette-sample';
    swatch.style.cssText = `--swatch-panel:${palette.panel};background:${palette.panel};color:${palette.accent};border-color:${palette.border}`;
    swatch.innerHTML = `<svg viewBox="0 0 64 40" xmlns="http://www.w3.org/2000/svg" focusable="false">${paletteArt[key]}</svg>`;
    swatch.setAttribute('aria-hidden', 'true');
    const label = document.createElement('span');
    label.textContent = palette.name;
    button.append(swatch, label);
    button.addEventListener('click', () => update({ theme: key }));
    paletteContainer.append(button);
  });

  function settingsSnippet() {
    return `# ModernIME 候选外观配置片段\n# 仅合并以下键到现有 settings.conf，勿覆盖整个文件。\ncandidate.theme=${state.theme}\ncandidate.shape=${state.shape}\ncandidate.font_size=${state.fontSize}\ncandidate.spacing=${state.spacing}\ncandidate.page_size=${state.pageSize}\n`;
  }

  function update(change = {}, persist = true) {
    const changedTheme = change.theme && change.theme !== state.theme;
    const themeSnapshot = changedTheme && persist ? window.siteMotion?.captureTheme() : null;
    state = { ...state, ...change };
    const palette = palettes[state.theme];
    root.dataset.theme = state.theme;
    const properties = {
      panel: palette.panel, ink: palette.ink, accent: palette.accent,
      'selected-ink': palette.selectedInk, muted: palette.muted,
      border: palette.border, canvas: palette.canvas, paper: palette.paper,
      'control-accent': palette.dark ? '#7052ad' : palette.accent,
      'font-size': `${state.fontSize}pt`,
      'number-size': `${Math.max(10, state.fontSize - 4)}px`,
      'preedit-size': `${Math.max(12, state.fontSize - 2)}px`,
      'panel-radius': { rounded: '10px', square: '3px', capsule: '28px' }[state.shape],
      'selected-radius': { rounded: '7px', square: '2px', capsule: '24px' }[state.shape],
      gap: { compact: '4px', standard: '8px', spacious: '14px' }[state.spacing],
      padding: { compact: '5px', standard: '8px', spacious: '12px' }[state.spacing],
      'candidate-width': `${state.pageSize * (state.fontSize * 1.53 + 28) + 20}px`
    };
    Object.entries(properties).forEach(([name, value]) => root.style.setProperty('--studio-' + name, value));
    document.documentElement.style.setProperty('--try-accent', palette.accent);
    document.documentElement.style.setProperty('--try-ink', palette.selectedInk);
    document.querySelectorAll('[data-palette]').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.palette === state.theme)));
    document.querySelectorAll('[data-shape]').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.shape === state.shape)));
    document.querySelectorAll('[data-spacing]').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.spacing === state.spacing)));
    document.querySelector('#studio-font-size').value = state.fontSize;
    document.querySelector('#studio-font-value').value = state.fontSize + ' pt';
    document.querySelector('#studio-width').value = state.pageSize;
    document.querySelector('#studio-width-value').value = state.pageSize + ' 个单字';
    document.querySelector('#studio-current-theme').textContent = palette.name;
    document.querySelector('#studio-selection-summary').textContent = `${palette.name}，${shapeLabels[state.shape]}轮廓，${state.fontSize} pt，${spacingLabels[state.spacing]}间距，${state.pageSize} 个单字栏宽。`;
    document.querySelector('#studio-config').textContent = settingsSnippet();
    if (persist) {
      try { localStorage.setItem(storageKey, JSON.stringify(state)); } catch { /* No text content is persisted. */ }
    }
    pageIndex = 0;
    cursor = 0;
    scheduleLayout();
    if (changedTheme && persist) window.siteMotion?.theme(themeSnapshot, document.querySelector(`[data-palette="${state.theme}"]`));
  }

  function createCandidate(word, index) {
    const button = document.createElement('button');
    button.type = 'button';
    const number = document.createElement('small');
    number.textContent = index + 1;
    number.setAttribute('aria-hidden', 'true');
    const text = document.createElement('span');
    text.textContent = word;
    button.append(number, text);
    button.setAttribute('aria-label', `候选 ${index + 1}：${word}`);
    return button;
  }

  function scheduleLayout(direction = 0) {
    if (direction) pageMotion = direction;
    cancelAnimationFrame(layoutFrame);
    layoutFrame = requestAnimationFrame(measurePages);
  }

  function measurePages() {
    const words = samples[sample].words;
    document.querySelector('#studio-preedit').textContent = samples[sample].preedit;
    const measuring = document.createElement('div');
    measuring.className = 'studio-candidate-list studio-measure';
    measuring.setAttribute('aria-hidden', 'true');
    words.forEach((word, index) => measuring.append(createCandidate(word, index)));
    root.append(measuring);
    const widths = [...measuring.children].map(button => button.getBoundingClientRect().width);
    const gap = parseFloat(getComputedStyle(list).gap) || 0;
    const available = list.clientWidth;
    measuring.remove();
    pages = [];
    let current = [], used = 0;
    words.forEach((word, index) => {
      const required = widths[index] + (current.length ? gap : 0);
      if (current.length && (used + required > available || current.length === state.pageSize)) {
        pages.push(current);
        current = [];
        used = 0;
      }
      used += widths[index] + (current.length ? gap : 0);
      current.push(word);
    });
    if (current.length) pages.push(current);
    pageIndex = Math.min(pageIndex, pages.length - 1);
    cursor = 0;
    renderPage(pageMotion);
    pageMotion = 0;
  }

  function paintSelection() {
    [...list.children].forEach((button, index) => {
      button.classList.toggle('is-selected', cursor === index);
      button.setAttribute('aria-pressed', String(cursor === index));
      button.tabIndex = cursor === index ? 0 : -1;
    });
  }

  function commit(index) {
    const word = pages[pageIndex]?.[index];
    if (!word) return;
    if (writing.value.length - (writing.selectionEnd - writing.selectionStart) + word.length > writing.maxLength) {
      hint.textContent = '试写区已满，可以先删掉一些文字。';
      return;
    }
    cursor = index;
    paintSelection();
    writing.setRangeText(word, writing.selectionStart, writing.selectionEnd, 'end');
    writing.focus({ preventScroll: true });
    window.siteMotion?.commit(list.children[index], writing, word);
    hint.textContent = `已插入「${word}」。也可以直接在上面输入。`;
  }

  function renderPage(direction = 0) {
    list.replaceChildren();
    (pages[pageIndex] || []).forEach((word, index) => {
      const button = createCandidate(word, index);
      button.addEventListener('click', () => commit(index));
      button.addEventListener('focus', () => { cursor = index; paintSelection(); });
      list.append(button);
    });
    cursor = Math.min(cursor, list.children.length - 1);
    paintSelection();
    document.querySelector('#studio-page-info').textContent = `第 ${pageIndex + 1} / ${pages.length} 页`;
    document.querySelector('#studio-prev').disabled = pageIndex <= 0;
    document.querySelector('#studio-next').disabled = pageIndex >= pages.length - 1;
    if (direction) window.siteMotion?.candidates(list, direction);
  }

  function turnPage(offset) {
    const next = pageIndex + offset;
    if (next < 0 || next >= pages.length) return;
    pageIndex = next;
    cursor = 0;
    renderPage(offset);
  }

  document.querySelectorAll('[data-shape]').forEach(button => button.addEventListener('click', () => update({ shape: button.dataset.shape })));
  document.querySelectorAll('[data-spacing]').forEach(button => button.addEventListener('click', () => update({ spacing: button.dataset.spacing })));
  document.querySelector('#studio-font-size').addEventListener('input', event => update({ fontSize: Number(event.target.value) }));
  document.querySelector('#studio-width').addEventListener('input', event => update({ pageSize: Number(event.target.value) }));
  document.querySelectorAll('[data-sample]').forEach(button => button.addEventListener('click', () => {
    sample = button.dataset.sample;
    document.querySelectorAll('[data-sample]').forEach(item => item.setAttribute('aria-pressed', String(item === button)));
    pageIndex = 0;
    cursor = 0;
    scheduleLayout(1);
  }));
  document.querySelector('#studio-reset').addEventListener('click', () => {
    update({ ...defaults });
    hint.textContent = '已恢复推荐的雾紫外观，试写文字保留。';
  });
  document.querySelector('#clear-writing').addEventListener('click', () => {
    writing.value = '';
    writing.focus({ preventScroll: true });
    hint.textContent = '试写区已清空。';
  });
  document.querySelector('#studio-prev').addEventListener('click', () => turnPage(-1));
  document.querySelector('#studio-next').addEventListener('click', () => turnPage(1));
  panel.addEventListener('keydown', event => {
    if (event.isComposing || event.ctrlKey || event.metaKey || event.altKey) return;
    const count = pages[pageIndex]?.length || 0;
    if (!count) return;
    if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
      event.preventDefault();
      cursor = (cursor + (event.key === 'ArrowRight' ? 1 : count - 1)) % count;
      paintSelection();
      list.children[cursor].focus();
    } else if (event.key === 'PageDown' || event.key === 'PageUp') {
      event.preventDefault();
      turnPage(event.key === 'PageDown' ? 1 : -1);
      panel.focus();
    } else if (event.key === ' ' || event.key === 'Enter' || /^[1-9]$/.test(event.key)) {
      event.preventDefault();
      commit(/^[1-9]$/.test(event.key) ? Number(event.key) - 1 : cursor);
    }
  });
  document.querySelector('#copy-appearance').addEventListener('click', async () => {
    if (await copyText(settingsSnippet())) {
      notify('已复制外观配置片段，请合并到现有配置中。');
    } else {
      const range = document.createRange();
      range.selectNodeContents(document.querySelector('#studio-config'));
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      notify('已选中配置，请按 Ctrl+C 复制。');
    }
  });
  document.querySelector('#download-appearance').addEventListener('click', () => {
    const blob = new Blob([settingsSnippet()], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `ModernIME-${palettes[state.theme].name}-外观.conf`;
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    notify('外观配置片段已准备下载。');
  });
  document.querySelector('.studio-controls').addEventListener('submit', event => event.preventDefault());
  let lastWidth = 0;
  const observer = new ResizeObserver(entries => {
    const width = entries[0].contentRect.width;
    if (Math.abs(lastWidth - width) > .5) { lastWidth = width; scheduleLayout(); }
  });
  observer.observe(panel);
  document.fonts.ready.then(() => scheduleLayout());
  update({}, false);
})();
