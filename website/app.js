'use strict';

const repository = 'https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method';
const packageRoot = 'https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method';

function landText(element, value) {
  element.textContent = value;
}

let toastTimer;
function notify(message) {
  const toast = document.querySelector('#toast');
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
  const tabs = [...container.querySelectorAll('[role="tab"]')];
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

const clientPages = {
  candidates: { title: '候选与外观', description: '九套候选配色，三种轮廓。字号与间距随你调整，预览后再保存。', alt: 'ModernIME 实际客户端：候选与外观，可调整配色、轮廓、字体和间距' },
  input: { title: '输入与按键', description: '全拼、小鹤与自然码双拼。切换按键和模糊音，都可以按习惯调整。', alt: 'ModernIME 实际客户端：输入与按键，可设置拼音方案、中英文切换与模糊音' },
  learning: { title: '智能学习', description: '候选记忆与上下文学习，由你决定开关。学习记录可以备份后清空。', alt: 'ModernIME 实际客户端：智能学习，展示学习开关和本地记录管理' }
};
const screenshot = document.querySelector('#client-screenshot');
const screenshotDialog = document.querySelector('#screenshot-dialog');
wireTabs(document.querySelector('.client-tabs'), tab => {
  const page = clientPages[tab.dataset.client];
  const src = 'assets/client-' + tab.dataset.client + '.webp';
  if (window.siteMotion) window.siteMotion.image(screenshot, src, page.alt);
  else { screenshot.src = src; screenshot.alt = page.alt; }
  document.querySelector('#client-description').textContent = page.description;
  document.querySelector('#open-screenshot').setAttribute('aria-label', '高清查看' + page.title + '客户端截图');
});
document.querySelector('#open-screenshot').addEventListener('click', () => {
  const full = screenshotDialog.querySelector('img');
  full.src = screenshot.src;
  full.alt = screenshot.alt;
  document.querySelector('#original-screenshot').href = screenshot.src.replace('.webp', '.png');
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

const installOptions = {
  ubuntu: {
    heading: '通过 APT 软件源安装', support: '适用于 Ubuntu 24.04（noble）amd64。', badge: '签名软件源',
    prerequisite: '首次安装需配置独立签名密钥与软件源。复制命令后，在终端执行。',
    documentation: repository + '#快速安装与发行包下载', linkLabel: '完整安装说明与密钥核验',
    command: `(
set -eu
. /etc/os-release
if [ "$ID" != ubuntu ] || [ "$VERSION_ID" != 24.04 ] || [ "$(dpkg --print-architecture)" != amd64 ]; then
  echo '此软件源仅适用于 Ubuntu 24.04 amd64，请参阅源码构建说明。' >&2
  exit 1
fi
sudo install -d -m 0755 /usr/share/keyrings
key_file=$(mktemp)
trap 'rm -f "$key_file"' EXIT
curl -fsSL ${packageRoot}/apt/modernime-archive-keyring.gpg -o "$key_file"
sudo install -m 0644 "$key_file" /usr/share/keyrings/modernime-archive-keyring.gpg
sudo tee /etc/apt/sources.list.d/modernime.sources >/dev/null <<'REPO'
Types: deb
URIs: ${packageRoot}/apt
Suites: noble
Components: main
Architectures: amd64
Signed-By: /usr/share/keyrings/modernime-archive-keyring.gpg
REPO
sudo apt update
sudo apt install modernime
)`
  },
  openeuler: {
    heading: '通过 DNF 软件源安装', support: '适用于 openEuler 24.09 x86_64。', badge: '签名软件源',
    prerequisite: '请先确保系统已配置 24.09 EPOL 依赖源。首次安装按提示核验并导入签名公钥。',
    documentation: repository + '#快速安装与发行包下载', linkLabel: '完整安装说明与 EPOL 配置',
    command: `(
set -eu
. /etc/os-release
if [ "$ID" != openEuler ] || [ "$VERSION_ID" != 24.09 ] || [ "$(uname -m)" != x86_64 ]; then
  echo '此命令仅适用于 openEuler 24.09 x86_64，请参阅完整安装说明。' >&2
  exit 1
fi
sudo tee /etc/yum.repos.d/modernime-openeuler-24.09.repo >/dev/null <<'REPO'
[modernime-openeuler-24.09]
name=ModernIME Repository for openEuler 24.09 ($basearch)
baseurl=${packageRoot}/rpm/oe2409/$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=${packageRoot}/rpm/RPM-GPG-KEY-modernime
metadata_expire=6h
REPO
sudo dnf makecache --refresh
sudo dnf install modernime
)`
  },
  source: {
    heading: '从源码构建安装', support: '其他发行版或架构，请先按文档准备依赖。', badge: '开放源代码',
    prerequisite: '需要 C++20 编译器、CMake ≥ 3.24、Fcitx5、LibIME、GTK 3.24 等依赖。构建安装过程默认离线。',
    documentation: repository + '#方式二从源码一键构建安装', linkLabel: '查看完整构建依赖',
    command: `git clone https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method.git ModernIME
cd ModernIME
# 先按文档安装系统依赖，再执行：
MODERNIME_PREFIX="$HOME/.local" ./install.sh`
  }
};
let currentInstall = 'ubuntu';
wireTabs(document.querySelector('.install-tabs'), tab => {
  currentInstall = tab.dataset.install;
  const option = installOptions[currentInstall];
  if (document.querySelector('#install-command').textContent) window.siteMotion?.panel(document.querySelector('#install-command'));
  document.querySelector('#install-heading').textContent = option.heading;
  document.querySelector('#install-support').textContent = option.support;
  document.querySelector('#install-badge').textContent = option.badge;
  document.querySelector('#install-prerequisite').textContent = option.prerequisite;
  document.querySelector('#install-command').textContent = option.command;
  const link = document.querySelector('#install-doc-link');
  link.href = option.documentation;
  link.firstChild.textContent = option.linkLabel;
  document.querySelector('#copy-install span').textContent = '复制命令';
  document.querySelector('.terminal pre').scrollTo(0, 0);
});
document.querySelector('#copy-install').addEventListener('click', async () => {
  const command = installOptions[currentInstall].command;
  const optionKey = currentInstall;
  const copied = await copyText(command);
  if (copied) {
    if (currentInstall === optionKey) document.querySelector('#copy-install span').textContent = '已复制';
    notify('已复制安装命令，请到终端执行。');
  } else {
    const range = document.createRange();
    range.selectNodeContents(document.querySelector('#install-command'));
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    notify('未能自动复制，已选中命令，请按 Ctrl+C 复制。');
  }
});

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
const mobileQuery = window.matchMedia('(max-width: 540px)');
mobileQuery.addEventListener('change', event => { if (!event.matches) closeMenu(); });
const header = document.querySelector('.site-header');
window.addEventListener('scroll', () => header.classList.toggle('scrolled', window.scrollY > 10), { passive: true });
header.classList.toggle('scrolled', window.scrollY > 10);
