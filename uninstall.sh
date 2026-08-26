#!/usr/bin/env bash
set -euo pipefail

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
manifest="$prefix/share/modernime/install-manifest.txt"
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
environment_file="$config_home/environment.d/90-modernime.conf"
system_libdir=$(pkg-config --variable=libdir Fcitx5Utils 2>/dev/null || true)
system_libdir=${system_libdir:-/usr/lib/x86_64-linux-gnu}
system_addon_dir="$system_libdir/fcitx5"
environment_line="FCITX_ADDON_DIRS=$prefix/lib/fcitx5:$system_addon_dir"

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
        "$environment_file")
            if [[ -f "$path" ]] &&
               [[ "$(wc -l < "$path")" -eq 1 ]] &&
               [[ "$(sed -n '1p' "$path")" == "$environment_line" ]]; then
                rm -f -- "$path"
            else
                printf 'Refusing to remove modified environment file: %s\n' "$path" >&2
                exit 1
            fi
            ;;
        *)
            printf 'Refusing unexpected manifest path: %s\n' "$path" >&2
            exit 1
            ;;
    esac
done < "$manifest"

rm -f -- "$manifest"
rmdir --ignore-fail-on-non-empty "$prefix/share/modernime" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$config_home/environment.d" 2>/dev/null || true
printf 'ModernIME files removed from %s\n' "$prefix"
