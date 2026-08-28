#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
readme="$project_root/README.md"

test -f "$readme"
grep -F 'git@github.com:sunshine134679/fcitx5-linux-pinyin-input-method.git' \
    "$readme" >/dev/null
grep -F './install.sh' "$readme" >/dev/null
grep -F 'modernime-settings' "$readme" >/dev/null
grep -F 'modernime-settings.desktop' "$readme" >/dev/null
grep -F './uninstall.sh' "$readme" >/dev/null
