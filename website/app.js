'use strict';

// Reload begins at the top; direct links and browser Back retain their meaning.
if (performance.getEntriesByType('navigation')[0]?.type === 'reload') {
  history.scrollRestoration = 'manual';
  if (location.hash) history.replaceState(history.state, '', location.pathname + location.search);
  addEventListener('pageshow', () => {
    scrollTo({ top: 0, left: 0, behavior: 'instant' });
    requestAnimationFrame(() => { history.scrollRestoration = 'auto'; });
  }, { once: true });
}


const repository = 'https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method';
const packageRoot = 'https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method';

function landText(element, value) {
  element.textContent = value;
}

let toastTimer;
function notify(message) {
  const toast = document.querySelector('#toast');
  if (!toast) return;
  clearTimeout(toastTimer);
  toast.textContent = message;
  toast.classList.add('visible');
  toastTimer = setTimeout(() => toast.classList.remove('visible'), 3600);
}

// Used by both installation and appearance configuration exports.
async function copyText(value) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(value);
      return true;
    }
  } catch { /* Local previews may require the synchronous fallback. */ }
  const focused = document.activeElement;
  const textarea = document.createElement('textarea');
  textarea.value = value;
  textarea.style.cssText = 'position:fixed;top:0;left:-9999px;';
  textarea.setAttribute('aria-label', '待复制的内容');
  document.body.append(textarea);
  textarea.select();
  let copied = false;
  try { copied = document.execCommand('copy'); } catch { copied = false; }
  textarea.remove();
  focused?.focus({ preventScroll: true });
  return copied;
}

// All tabs use roving focus, including arrow, Home and End navigation.
function wireTabs(container, onChange) {
  if (!container) return;
  const tabs = [...container.querySelectorAll('[role="tab"]')];
  if (!tabs.length) return;
  function select(tab, focus = false) {
    tabs.forEach(item => {
      item.setAttribute('aria-selected', String(item === tab));
      item.tabIndex = item === tab ? 0 : -1;
    });
    const panel = document.getElementById(tab.getAttribute('aria-controls'));
    panel.setAttribute('aria-labelledby', tab.id);
    onChange(tab);
    if (focus) tab.focus();
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => select(tab));
    tab.addEventListener('keydown', event => {
      const vertical = container.getAttribute('aria-orientation') === 'vertical';
      const next = vertical ? 'ArrowDown' : 'ArrowRight';
      const previous = vertical ? 'ArrowUp' : 'ArrowLeft';
      let target;
      if (event.key === next) target = (index + 1) % tabs.length;
      if (event.key === previous) target = (index - 1 + tabs.length) % tabs.length;
      if (event.key === 'Home') target = 0;
      if (event.key === 'End') target = tabs.length - 1;
      if (target === undefined) return;
      event.preventDefault();
      select(tabs[target], true);
    });
  });
  select(tabs.find(tab => tab.getAttribute('aria-selected') === 'true') || tabs[0]);
}

if (document.querySelector('#hero-result')) {
  const heroChoices = [...document.querySelectorAll('.candidate-options button')];
  function setHero(word, initial = false, source = null) {
    landText(document.querySelector('#hero-result'), word);
    heroChoices.forEach(button => {
      button.classList.toggle('chosen', button.dataset.word === word);
      button.setAttribute('aria-pressed', String(button.dataset.word === word));
    });
    document.querySelector('#hero-hint').textContent = initial ? '点击候选词，让文字落下' : '已选「' + word + '」，下一句由你开始';
    if (source) window.siteMotion?.heroChoice(source, document.querySelector('#hero-result'), word);
  }
  heroChoices.forEach(button => button.addEventListener('click', () => setHero(button.dataset.word, false, button)));
  document.querySelector('#hero-replay').addEventListener('click', () => {
    setHero('你好', true);
    window.siteMotion?.hero();
  });
  heroChoices.forEach((button, i) => button.setAttribute('aria-pressed', String(i === 0)));
}
if (document.querySelector('#client')) {
  const clientPages = {
    candidates: { title: '候选与外观', description: '界面主题与候选栏配色独立选择。调整轮廓、字号和间距，即时预览后保存。', alt: 'ModernIME 实际客户端：候选与外观，可调整配色、轮廓、字体和间距' },
    input: { title: '输入习惯', description: '全拼、小鹤与自然码双拼。切换按键和模糊音，都可以按习惯调整。', alt: 'ModernIME 实际客户端：输入习惯，可设置拼音方案、中英文切换与模糊音' },
    learning: { title: '学习记录', description: '在「我的词语」中查看学习记录、备份或清理；学习开关在「输入习惯」中管理。', alt: 'ModernIME 实际客户端：学习记录，展示我的词语中的本地学习记录' }
  };
  const screenshot = document.querySelector('#client-screenshot');
  const screenshotDialog = document.querySelector('#screenshot-dialog');
  // Theme swatches follow the current native ClientPalette. Screenshots retain
  // their documented capture baseline; candidate palettes remain independent.
  const clientThemes = {
    lavender: { name: '雾紫', canvas: '#efedf3', accent: '#765b9f', ink: '#706c78', border: '#cbc7d3' },
    blue: { name: '晴空蓝', canvas: '#edf0f4', accent: '#3869d4', ink: '#6b727d', border: '#c8cdd5' },
    jade: { name: '青竹绿', canvas: '#edf1ef', accent: '#347b65', ink: '#69726d', border: '#c6cec9' },
    amber: { name: '暖砂橙', canvas: '#f1eee8', accent: '#966033', ink: '#746e66', border: '#cec8bf' },
    rose: { name: '玫瑰粉', canvas: '#f2edef', accent: '#a04d70', ink: '#756b70', border: '#d0c6cb' },
    night: { name: '深海夜', canvas: '#192635', accent: '#8bb5ed', ink: '#abbcd0', border: '#394b60' },
    contrast: { name: '高对比', canvas: '#0c1013', accent: '#ffdf70', ink: '#ffffff', border: '#d0dbe5' }
  };
  const clientSection = document.querySelector('#client');
  let clientPage = 'candidates';
  let clientTheme = 'lavender';
  function updateClientPreview(motion = {}) {
    const page = clientPages[clientPage];
    const theme = clientThemes[clientTheme];
    const src = `assets/client-v0213-${clientPage}-${clientTheme}.webp`;
    const alt = `${page.alt}，${theme.name}界面主题`;
    if (window.siteMotion) window.siteMotion.image(screenshot, src, alt, motion);
    else { screenshot.src = src; screenshot.alt = alt; }
    clientSection.dataset.previewTheme = clientTheme;
    for (const key of ['canvas', 'accent', 'ink', 'border']) clientSection.style.setProperty('--client-' + key, theme[key]);
    document.querySelectorAll('.client-theme-options button').forEach(button => {
      button.setAttribute('aria-pressed', String(button.dataset.clientTheme === clientTheme));
      button.style.setProperty('--swatch', clientThemes[button.dataset.clientTheme].accent);
    });
    document.querySelector('#client-description').textContent = page.description;
    document.querySelector('#client-capture-label').textContent = `${theme.name} · 2940 × 2160 实拍`;
    document.querySelector('#open-screenshot').setAttribute('aria-label', `高清查看${theme.name}主题的${page.title}客户端截图`);
  }
  wireTabs(document.querySelector('.client-tabs'), tab => {
    const direction = Object.keys(clientPages).indexOf(tab.dataset.client) > Object.keys(clientPages).indexOf(clientPage) ? 1 : -1;
    clientPage = tab.dataset.client;
    updateClientPreview({ direction });
  });
  document.querySelectorAll('.client-theme-options button').forEach(button => {
    button.addEventListener('click', () => {
      if (clientTheme === button.dataset.clientTheme) return;
      clientTheme = button.dataset.clientTheme;
      updateClientPreview({ origin: button });
    });
  });
  document.querySelector('#open-screenshot').addEventListener('click', () => {
    const full = screenshotDialog.querySelector('img');
    full.src = screenshot.src;
    full.alt = screenshot.alt;
    document.querySelector('#original-screenshot').href = screenshot.src.replace('.webp', '.png');
    document.querySelector('#screenshot-caption').textContent = `${clientThemes[clientTheme].name}主题 · ${clientPages[clientPage].title} · 2940 × 2160 原生截图`;
    screenshotDialog.showModal();
    window.siteMotion?.openDialog(screenshotDialog);
    document.body.style.overflow = 'hidden';
  });
  function closeScreenshot() {
    if (window.siteMotion) window.siteMotion.closeDialog(screenshotDialog);
    else screenshotDialog.close();
  }
  screenshotDialog.querySelector('.dialog-close').addEventListener('click', closeScreenshot);
  screenshotDialog.addEventListener('click', event => {
    if (event.target === screenshotDialog) closeScreenshot();
  });
  screenshotDialog.addEventListener('cancel', event => { event.preventDefault(); closeScreenshot(); });
  screenshotDialog.addEventListener('close', () => { document.body.style.overflow = ''; });
}
const menuButton = document.querySelector('.menu-toggle');
const navigation = document.querySelector('#main-nav');
function closeMenu(returnFocus = false) {
  menuButton.setAttribute('aria-expanded', 'false');
  menuButton.setAttribute('aria-label', '展开导航');
  navigation.classList.remove('open');
  if (returnFocus) menuButton.focus();
}
menuButton.addEventListener('click', () => {
  const expanded = menuButton.getAttribute('aria-expanded') !== 'true';
  menuButton.setAttribute('aria-expanded', String(expanded));
  menuButton.setAttribute('aria-label', expanded ? '收起导航' : '展开导航');
  navigation.classList.toggle('open', expanded);
});
navigation.querySelectorAll('a').forEach(link => link.addEventListener('click', () => closeMenu()));
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && menuButton.getAttribute('aria-expanded') === 'true') closeMenu(true);
});
const mobileQuery = window.matchMedia('(max-width: 760px)');
mobileQuery.addEventListener('change', event => { if (!event.matches) closeMenu(); });
const header = document.querySelector('.site-header');
window.addEventListener('scroll', () => header.classList.toggle('scrolled', window.scrollY > 10), { passive: true });
header.classList.toggle('scrolled', window.scrollY > 10);

// Existing shared section links retain their destinations after the page split.
const legacySections = { experience: 'appearance.html', install: 'download.html', questions: 'help.html' };
if (/\/(?:index\.html)?$/.test(location.pathname) && Object.hasOwn(legacySections, location.hash.slice(1))) {
  location.replace(legacySections[location.hash.slice(1)]);
}
function revealLinkedAnswer() {
  let id;
  try { id = decodeURIComponent(location.hash.slice(1)); } catch { return; }
  const target = document.getElementById(id);
  const details = target?.closest('details');
  if (details) details.open = true;
  if (target) requestAnimationFrame(() => target.scrollIntoView({block:'start', behavior:'instant'}));
}
addEventListener('hashchange', revealLinkedAnswer);
if (location.hash) revealLinkedAnswer();
// Touch navigation closes when users leave it, including a click on page content.
document.addEventListener('click', event => {
  if (!header.contains(event.target) && menuButton.getAttribute('aria-expanded') === 'true') closeMenu();
});

// A link to an already active, collapsed answer must open it again as well.
document.addEventListener('click', event => {
  const link = event.target.closest('a[href]');
  if (!link) return;
  const destination = new URL(link.href, location.href);
  if (destination.origin !== location.origin || destination.pathname !== location.pathname || !destination.hash) return;
  let target;
  try { target = document.getElementById(decodeURIComponent(destination.hash.slice(1))); } catch { return; }
  const answer = target?.closest('details');
  if (answer) answer.open = true;
});
