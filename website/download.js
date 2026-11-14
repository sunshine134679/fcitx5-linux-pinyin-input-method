'use strict';

(() => {
  const root = document.querySelector('[data-download]');
  if (!root) return;
  const platformTabs = root.querySelector('.download-platforms');
  const commandElement = root.querySelector('#install-command');
  const copyButton = root.querySelector('#copy-install');
  if (!platformTabs || !commandElement || !copyButton) return;

  const repository = 'https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method';
  const packageRoot = 'https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method';

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

  const updateOptions = {
    ubuntu: {
      heading: '更新已安装的 APT 版本',
      prerequisite: '以下命令仅适用于已通过 APT 安装并配置好 ModernIME 软件源的用户。源码或便携安装请先查看渠道迁移说明。',
      command: 'sudo apt update\nsudo apt install --only-upgrade modernime'
    },
    openeuler: {
      heading: '更新已安装的 DNF 版本',
      prerequisite: '以下命令仅适用于已通过 DNF 安装并配置好 ModernIME 软件源的用户。更新会刷新软件源信息。',
      command: 'sudo dnf makecache --refresh\nsudo dnf upgrade modernime'
    },
    source: {
      heading: '更新原有源码安装',
      prerequisite: '在原源码目录执行，并沿用原安装前缀。便携包用户请从发行页获取新版，使用其中的安装脚本更新。',
      command: 'git pull --ff-only && ./install.sh'
    }
  };
  let currentInstall = 'ubuntu';
  let currentStage = root.querySelector('input[name="install-stage"]:checked')?.value === 'update' ? 'update' : 'new';

  function setText(selector, text) {
    const element = root.querySelector(selector);
    if (element) element.textContent = text;
  }

  function render() {
    const base = installOptions[currentInstall];
    const option = currentStage === 'update' ? { ...base, ...updateOptions[currentInstall] } : base;
    setText('#install-heading', option.heading);
    setText('#install-support', option.support);
    setText('#install-badge', currentStage === 'update' ? '沿用原安装渠道' : option.badge);
    setText('#install-prerequisite', option.prerequisite);
    commandElement.textContent = option.command;
    const updateNote = root.querySelector('#install-update-note');
    if (updateNote) updateNote.hidden = currentStage !== 'update';
    const documentation = root.querySelector('#install-doc-link');
    if (documentation) {
      documentation.href = currentStage === 'update' ? repository + '#在线更新与缓存' : option.documentation;
      documentation.textContent = (currentStage === 'update' ? '完整更新说明' : option.linkLabel) + ' ↗';
    }
    setText('#copy-install span', '复制命令');
    commandElement.parentElement?.scrollTo(0, 0);
  }

  const tabs = [...platformTabs.querySelectorAll('[role="tab"]')];
  function choosePlatform(tab, focus = false) {
    if (!installOptions[tab.dataset.install]) return;
    currentInstall = tab.dataset.install;
    tabs.forEach(item => {
      item.setAttribute('aria-selected', String(item === tab));
      item.tabIndex = item === tab ? 0 : -1;
    });
    const panel = root.querySelector('#install-panel');
    if (panel) panel.setAttribute('aria-labelledby', tab.id);
    render();
    if (focus) tab.focus();
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => choosePlatform(tab));
    tab.addEventListener('keydown', event => {
      let target;
      if (event.key === 'ArrowRight') target = (index + 1) % tabs.length;
      if (event.key === 'ArrowLeft') target = (index - 1 + tabs.length) % tabs.length;
      if (event.key === 'Home') target = 0;
      if (event.key === 'End') target = tabs.length - 1;
      if (target === undefined) return;
      event.preventDefault();
      choosePlatform(tabs[target], true);
    });
  });
  root.querySelectorAll('input[name="install-stage"]').forEach(input => {
    input.addEventListener('change', () => {
      currentStage = input.value === 'update' ? 'update' : 'new';
      render();
    });
  });
  copyButton.addEventListener('click', async () => {
    const command = commandElement.textContent;
    const copied = typeof copyText === 'function' && await copyText(command);
    if (copied) {
      if (commandElement.textContent === command) setText('#copy-install span', '已复制');
      if (typeof notify === 'function') notify('已复制命令，请到终端执行。');
    } else {
      const range = document.createRange();
      range.selectNodeContents(commandElement);
      const selection = window.getSelection();
      selection?.removeAllRanges();
      selection?.addRange(range);
      if (typeof notify === 'function') notify('未能自动复制，已选中命令，请按 Ctrl+C 复制。');
    }
  });
  render();
})();
