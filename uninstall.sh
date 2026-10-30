#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
unset MODERNIME_PREFIX_LOCK_FD MODERNIME_LOCKED_PREFIX MODERNIME_LOCK_FILE 2>/dev/null || true
source "$project_root/scripts/install_state.sh"
if [[ -f "$project_root/scripts/install_lifecycle.sh" ]]; then
    source "$project_root/scripts/install_lifecycle.sh"
fi

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
explicit_prefix=false
[[ -n "${MODERNIME_PREFIX:-}" ]] && explicit_prefix=true

if [[ $# -ge 1 && -n "${1:-}" ]]; then
    case "$1" in
        --prefix=*)
            prefix="${1#*=}"
            explicit_prefix=true
            ;;
        --prefix|-p)
            prefix="$2"
            explicit_prefix=true
            shift
            ;;
        -h|--help)
            printf 'Usage: %s [OPTIONS] [PREFIX]\n\n' "$0"
            printf 'Options:\n'
            printf '  -p, --prefix=PATH         Installation prefix to remove (default: $HOME/.local)\n'
            printf '  -h, --help                Show this help message\n'
            exit 0
            ;;
        -*)
            ;;
        *)
            prefix="$1"
            explicit_prefix=true
            ;;
    esac
fi

if [[ "$project_root" =~ ^/(tmp|var/tmp)/ && "$explicit_prefix" != true ]]; then
    printf 'Error: Running uninstall.sh from temporary directory %s without MODERNIME_PREFIX is not allowed.\n' "$project_root" >&2
    printf 'This prevents accidental removal of your real user environment (%s).\n' "$HOME" >&2
    printf 'Specify MODERNIME_PREFIX explicitly (e.g. MODERNIME_PREFIX=/tmp/test-prefix) if intended.\n' >&2
    exit 1
fi

if ! declare -f modernime_acquire_prefix_lock >/dev/null 2>&1; then
    if [[ -f "$prefix/share/modernime/install_lifecycle.sh" ]]; then
        source "$prefix/share/modernime/install_lifecycle.sh"
    fi
fi

if declare -f modernime_acquire_prefix_lock >/dev/null 2>&1; then
    if ! modernime_acquire_prefix_lock "$prefix"; then
        exit 1
    fi
    trap modernime_release_prefix_lock EXIT
fi

is_system_prefix=false
if [[ "$prefix" =~ ^/(usr|opt|etc)(/|$) || -n "${MODERNIME_FORCE_SYSTEM_PREFIX:-}" ]]; then
    is_system_prefix=true
fi

manifest="$prefix/share/modernime/install-manifest.txt"
config_home=${MODERNIME_CONFIG_HOME:-"${XDG_CONFIG_HOME:-"$HOME/.config"}"}
environment_file="$config_home/environment.d/90-modernime.conf"
autostart_file="$config_home/autostart/modernime-fcitx5-session.desktop"
system_libdir=$(pkg-config --variable=libdir Fcitx5Utils 2>/dev/null || true)
if [[ -z "$system_libdir" ]]; then
    if [[ -d "/usr/lib64/fcitx5" ]]; then
        system_libdir="/usr/lib64"
    elif [[ -d "/usr/lib/x86_64-linux-gnu/fcitx5" ]]; then
        system_libdir="/usr/lib/x86_64-linux-gnu"
    elif [[ -d "/usr/lib/aarch64-linux-gnu/fcitx5" ]]; then
        system_libdir="/usr/lib/aarch64-linux-gnu"
    else
        system_libdir="/usr/lib"
    fi
fi
system_addon_dir="$system_libdir/fcitx5"
desktop_dir=${MODERNIME_DESKTOP_DIR:-"$HOME/Desktop"}
if [[ -z "${MODERNIME_DESKTOP_DIR:-}" ]] && command -v xdg-user-dir >/dev/null 2>&1; then
    configured_desktop_dir=$(xdg-user-dir DESKTOP || true)
    if [[ -n "$configured_desktop_dir" ]]; then
        desktop_dir="$configured_desktop_dir"
    fi
elif [[ -z "${MODERNIME_DESKTOP_DIR:-}" && ! -d "$desktop_dir" && -d "$HOME/桌面" ]]; then
    desktop_dir="$HOME/桌面"
fi
desktop_shortcut="$desktop_dir/modernime-settings.desktop"
desktop_exec="$prefix/bin/modernime-settings"

if [[ ! -f "$manifest" ]]; then
    printf 'No ModernIME install manifest found at %s\n' "$manifest"
    exit 0
fi

managed_state_dir="$prefix/share/modernime/managed-files"
managed_files_preserved=false
recorded_xprofile=""
if [[ -f "$managed_state_dir/xprofile.path" ]]; then
    recorded_xprofile=$(cat "$managed_state_dir/xprofile.path")
fi
recorded_desktop=""
if [[ -f "$managed_state_dir/desktop.path" ]]; then
    recorded_desktop=$(cat "$managed_state_dir/desktop.path")
fi
recorded_environment=""
if [[ -f "$managed_state_dir/environment.path" ]]; then
    recorded_environment=$(cat "$managed_state_dir/environment.path")
fi
recorded_autostart=""
if [[ -f "$managed_state_dir/autostart.path" ]]; then
    recorded_autostart=$(cat "$managed_state_dir/autostart.path")
fi

prefix_clean="${prefix%/}"
canon_prefix=$(readlink -f -- "$prefix_clean" 2>/dev/null || echo "$prefix_clean")

while IFS= read -r path; do
    [[ -z "$path" ]] && continue

    # 1. Reject path traversal
    if [[ "$path" == *"/../"* || "$path" == *"/.." || "$path" == "../"* || "$path" == ".." ]]; then
        printf 'Refusing unexpected manifest path: %s\n' "$path" >&2
        exit 1
    fi

    # 2. Path must be absolute
    if [[ "$path" != /* ]]; then
        printf 'Refusing unexpected manifest path: %s\n' "$path" >&2
        exit 1
    fi

    # 3. External managed file handling (when not inside prefix)
    if [[ "$path" != "$prefix_clean/"* ]]; then
        case "$path" in
            "$recorded_desktop"|"$desktop_shortcut"|*/modernime-settings.desktop)
                modernime_remove_managed_file "$path" desktop "$managed_state_dir"
                continue
                ;;
            "$recorded_environment"|"$environment_file"|*/environment.d/90-modernime.conf)
                modernime_remove_managed_file "$path" environment "$managed_state_dir"
                continue
                ;;
            "$recorded_autostart"|"$autostart_file"|*/autostart/modernime-fcitx5-session.desktop)
                modernime_remove_managed_file "$path" autostart "$managed_state_dir"
                continue
                ;;
            "$recorded_xprofile")
                if [[ -n "$recorded_xprofile" ]]; then
                    modernime_remove_profile_block "$path" "$managed_state_dir"
                    continue
                fi
                ;;
            *)
                printf 'Refusing unexpected manifest path: %s\n' "$path" >&2
                exit 1
                ;;
        esac
    fi

    # 4. Canonical parent & target containment and namespace ownership check
    if ! modernime_is_safe_removal_target "$path" "$prefix_clean" "$canon_prefix" "$is_system_prefix"; then
        # Check if it was preserved due to system package ownership
        if [[ "$is_system_prefix" == true ]]; then
            owner=""
            canon_target=$(readlink -f -- "$path" 2>/dev/null || true)
            for check_target in "$path" "$canon_target"; do
                [[ -z "$check_target" ]] && continue
                if command -v dpkg >/dev/null 2>&1; then
                    owner=$(dpkg -S "$check_target" 2>/dev/null || true)
                elif command -v rpm >/dev/null 2>&1; then
                    owner=$(rpm -qf "$check_target" 2>/dev/null || true)
                fi
                [[ -n "$owner" ]] && break
            done
            if [[ -n "$owner" ]]; then
                printf 'Preserving file managed by native system package (%s): %s\n' "$owner" "$path" >&2
                managed_files_preserved=true
                continue
            fi
        fi
        printf 'Refusing unexpected manifest path: %s\n' "$path" >&2
        exit 1
    fi

    # 5. Safe removal
    rm -f -- "$path"
done < "$manifest"

if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -d "$prefix/share/icons/hicolor" ]]; then
    gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" 2>/dev/null || true
fi

for legacy_plugin in "$prefix/lib/fcitx5/modernime_fcitx5.so" \
                     "$prefix/lib/fcitx5/modernime_ui.so" \
                     "$prefix/lib64/fcitx5/modernime_fcitx5.so" \
                     "$prefix/lib64/fcitx5/modernime_ui.so"; do
    if [[ -f "$legacy_plugin" || -L "$legacy_plugin" ]]; then
        if modernime_is_safe_removal_target "$legacy_plugin" "$prefix_clean" "$canon_prefix" "$is_system_prefix"; then
            rm -f -- "$legacy_plugin"
        fi
    fi
done

addon_link="$prefix/lib/fcitx5"
addon_link_record="$managed_state_dir/addon-link.target"
if [[ -f "$addon_link_record" ]]; then
    if [[ -L "$addon_link" && "$(readlink -- "$addon_link")" == "$(cat "$addon_link_record")" ]]; then
        rm -f -- "$addon_link" "$addon_link_record"
    elif [[ ! -e "$addon_link" && ! -L "$addon_link" ]]; then
        rm -f -- "$addon_link_record"
    else
        printf 'Preserving modified addon directory alias: %s\n' "$addon_link" >&2
        managed_files_preserved=true
    fi
fi

if [[ "$managed_files_preserved" != true ]]; then
    rm -f -- "$manifest"
    for key in desktop environment autostart xprofile payload; do
        rm -f -- "$managed_state_dir/$key.content" "$managed_state_dir/$key.path" "$managed_state_dir/$key.manifest"
    done
    rmdir "$managed_state_dir" 2>/dev/null || true
fi

rmdir --ignore-fail-on-non-empty "$prefix/lib/x86_64-linux-gnu/fcitx5" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/lib/x86_64-linux-gnu" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/lib/aarch64-linux-gnu/fcitx5" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/lib/aarch64-linux-gnu" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/lib64/fcitx5" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/lib/fcitx5" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/share/modernime/pinyin" 2>/dev/null || true
if [[ -d "$prefix/share/modernime/extensions" ]]; then
    find "$prefix/share/modernime/extensions" -depth -type d -exec rmdir {} + 2>/dev/null || true
    rmdir --ignore-fail-on-non-empty "$prefix/share/modernime/extensions" 2>/dev/null || true
fi
rmdir --ignore-fail-on-non-empty "$prefix/share/modernime" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$prefix/share/doc/modernime" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$config_home/environment.d" 2>/dev/null || true
rmdir --ignore-fail-on-non-empty "$config_home/autostart" 2>/dev/null || true
if [[ -n "$recorded_environment" ]]; then
    rmdir --ignore-fail-on-non-empty "$(dirname -- "$recorded_environment")" 2>/dev/null || true
fi
if [[ -n "$recorded_autostart" ]]; then
    rmdir --ignore-fail-on-non-empty "$(dirname -- "$recorded_autostart")" 2>/dev/null || true
fi
if [[ -n "$recorded_desktop" ]]; then
    rmdir --ignore-fail-on-non-empty "$(dirname -- "$recorded_desktop")" 2>/dev/null || true
fi
if declare -f modernime_release_prefix_lock >/dev/null 2>&1; then
    modernime_release_prefix_lock
fi
printf 'ModernIME files removed from %s\n' "$prefix"
