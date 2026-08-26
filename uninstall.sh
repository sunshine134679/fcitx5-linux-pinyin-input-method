#!/usr/bin/env bash
set -euo pipefail

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
manifest="$prefix/share/modernime/install-manifest.txt"

if [[ ! -f "$manifest" ]]; then
    printf 'No ModernIME install manifest found at %s\n' "$manifest"
    exit 0
fi

while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    case "$path" in
        "$prefix/lib/fcitx5/modernime_fcitx5.so"|\
        "$prefix/lib/fcitx5/modernime_ui.so"|\
        "$prefix/share/fcitx5/addon/modernime.conf"|\
        "$prefix/share/fcitx5/addon/modernime-ui.conf"|\
        "$prefix/share/fcitx5/inputmethod/modernime.conf")
            rm -f -- "$path"
            ;;
        *)
            printf 'Refusing unexpected manifest path: %s\n' "$path" >&2
            exit 1
            ;;
    esac
done < "$manifest"

rm -f -- "$manifest"
rmdir --ignore-fail-on-non-empty "$prefix/share/modernime" 2>/dev/null || true
printf 'ModernIME files removed from %s\n' "$prefix"
