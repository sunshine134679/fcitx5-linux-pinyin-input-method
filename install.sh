#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

required_commands=(cmake ctest pkg-config)
for required_command in "${required_commands[@]}"; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        printf 'Required build command not found: %s\n' "$required_command" >&2
        printf 'Install the build dependencies listed in README.md and try again.\n' >&2
        exit 1
    fi
done

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
build_dir=${MODERNIME_BUILD_DIR:-"$project_root/build/install-debug"}
generator=${CMAKE_GENERATOR:-"Unix Makefiles"}
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
environment_dir="$config_home/environment.d"
environment_file="$environment_dir/90-modernime.conf"
autostart_dir="$config_home/autostart"
autostart_file="$autostart_dir/modernime-fcitx5-session.desktop"
system_libdir=$(pkg-config --variable=libdir Fcitx5Utils 2>/dev/null || true)
system_libdir=${system_libdir:-/usr/lib/x86_64-linux-gnu}
system_addon_dir="$system_libdir/fcitx5"

desktop_dir="$HOME/Desktop"
if command -v xdg-user-dir >/dev/null 2>&1; then
    configured_desktop_dir=$(xdg-user-dir DESKTOP || true)
    if [[ -n "$configured_desktop_dir" ]]; then
        desktop_dir="$configured_desktop_dir"
    fi
elif [[ ! -d "$desktop_dir" && -d "$HOME/桌面" ]]; then
    desktop_dir="$HOME/桌面"
fi
desktop_shortcut="$desktop_dir/modernime-settings.desktop"
desktop_tmp=""
cleanup_desktop_tmp() {
    if [[ -n "$desktop_tmp" ]]; then
        rm -f -- "$desktop_tmp"
    fi
}
trap cleanup_desktop_tmp EXIT

fcitx_environment=(
    env
    "FCITX_ADDON_DIRS=$prefix/lib/fcitx5:$system_addon_dir"
)
for environment_name in DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR; do
    if [[ -n "${!environment_name:-}" ]]; then
        fcitx_environment+=("$environment_name=${!environment_name}")
    fi
done

cmake_args=(
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_INSTALL_PREFIX="$prefix"
    -DMODERNIME_BUILD_FCITX5=ON
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON
    -DMODERNIME_BUILD_SETTINGS=ON
    -DMODERNIME_BUILD_TESTS=ON
)
if [[ -n "${MODERNIME_BOOST_ROOT:-}" ]]; then
    cmake_args+=("-DBoost_ROOT=$MODERNIME_BOOST_ROOT")
fi

cmake -S "$project_root" -B "$build_dir" -G "$generator" "${cmake_args[@]}"
cmake --build "$build_dir"
(
    unset MODERNIME_SKIP_FCITX_RESTART
    # The GTK focus integration tests require a controlled compositor. Running
    # them inside an arbitrary desktop session makes window-manager focus
    # stealing prevention look like a product failure. Keep the installer test
    # run headless; those tests return CTest's configured skip code, while the
    # remaining suite still runs normally. The original desktop environment is
    # restored automatically when this subshell exits.
    unset DISPLAY WAYLAND_DISPLAY GDK_BACKEND BROADWAY_DISPLAY
    ctest --test-dir "$build_dir" --output-on-failure
)
cmake --install "$build_dir"

mkdir -p "$desktop_dir"
desktop_tmp=$(mktemp "$desktop_dir/.modernime-settings.XXXXXX")
{
    printf '%s\n' '[Desktop Entry]'
    printf '%s\n' 'Type=Application'
    printf '%s\n' 'Name=ModernIME 设置'
    printf '%s\n' 'Comment=Configure ModernIME input method'
    printf 'Exec=%s\n' "$prefix/bin/modernime-settings"
    printf '%s\n' 'Icon=input-keyboard'
    printf '%s\n' 'Terminal=false'
    printf '%s\n' 'Categories=Settings;Utility;'
} > "$desktop_tmp"
if [[ -e "$desktop_shortcut" ]]; then
    if [[ ! -f "$desktop_shortcut" ]] ||
       ! cmp -s "$desktop_tmp" "$desktop_shortcut"; then
        printf 'Refusing to overwrite existing desktop shortcut: %s\n' \
            "$desktop_shortcut" >&2
        exit 1
    fi
    rm -f -- "$desktop_tmp"
    desktop_tmp=""
else
    mv -- "$desktop_tmp" "$desktop_shortcut"
    desktop_tmp=""
    chmod +x "$desktop_shortcut"
fi

environment_line="FCITX_ADDON_DIRS=$prefix/lib/fcitx5:$system_addon_dir"
if [[ -e "$environment_file" ]]; then
    if [[ "$(wc -l < "$environment_file")" -ne 1 ]] ||
       [[ "$(sed -n '1p' "$environment_file")" != "$environment_line" ]]; then
        printf 'Refusing to overwrite existing file: %s\n' "$environment_file" >&2
        exit 1
    fi
else
    mkdir -p "$environment_dir"
    printf '%s\n' "$environment_line" > "$environment_file"
fi

autostart_exec="env FCITX_ADDON_DIRS=$prefix/lib/fcitx5:$system_addon_dir fcitx5 -d -u modernime-ui"
if [[ -e "$autostart_file" ]]; then
    if [[ "$(wc -l < "$autostart_file")" -ne 8 ]] ||
       ! grep -Fqx "Exec=$autostart_exec" "$autostart_file"; then
        printf 'Refusing to overwrite existing file: %s\n' "$autostart_file" >&2
        exit 1
    fi
else
    mkdir -p "$autostart_dir"
    {
        printf '%s\n' '[Desktop Entry]'
        printf '%s\n' 'Type=Application'
        printf '%s\n' 'Name=ModernIME Fcitx5'
        printf '%s\n' 'Comment=Start Fcitx5 with the ModernIME candidate bar'
        printf 'Exec=%s\n' "$autostart_exec"
        printf '%s\n' 'Terminal=false'
        printf '%s\n' 'X-GNOME-Autostart-enabled=true'
        printf '%s\n' 'NoDisplay=true'
    } > "$autostart_file"
fi

manifest_dir="$prefix/share/modernime"
manifest="$manifest_dir/install-manifest.txt"
mkdir -p "$manifest_dir"
{
    printf '%s\n' "$prefix/lib/fcitx5/modernime_fcitx5.so"
    printf '%s\n' "$prefix/lib/fcitx5/modernime_ui.so"
    printf '%s\n' "$prefix/share/modernime/pinyin/modernime-knowledge.dict"
    printf '%s\n' "$prefix/share/fcitx5/addon/modernime.conf"
    printf '%s\n' "$prefix/share/fcitx5/addon/modernime-ui.conf"
    printf '%s\n' "$prefix/share/fcitx5/inputmethod/modernime.conf"
    printf '%s\n' "$prefix/bin/modernime-settings"
    printf '%s\n' "$prefix/share/applications/modernime-settings.desktop"
    printf '%s\n' "$desktop_shortcut"
    printf '%s\n' "$environment_file"
    printf '%s\n' "$autostart_file"
} > "$manifest"

run_fcitx5_remote() {
    "${fcitx_environment[@]}" bash -c '
        if command -v timeout >/dev/null 2>&1; then
            timeout --kill-after=0.5s 1s fcitx5-remote "$@" || exit 1
        else
            fcitx5-remote "$@" || exit 1
        fi
    ' modernime-fcitx5-remote "$@" 2>/dev/null
}

start_fcitx5_safely() {
    "${fcitx_environment[@]}" bash -c '
        if command -v timeout >/dev/null 2>&1; then
            timeout --kill-after=1s 3s fcitx5 -d -r -u modernime-ui || true
        else
            fcitx5 -d -r -u modernime-ui || true
        fi
    ' modernime-fcitx5-start >/dev/null 2>&1
}

skip_fcitx_restart=false
case "${MODERNIME_SKIP_FCITX_RESTART:-}" in
    1|true|TRUE|yes|YES)
        skip_fcitx_restart=true
        ;;
esac

if [[ "$skip_fcitx_restart" == true ]]; then
    printf 'Fcitx5 restart skipped by MODERNIME_SKIP_FCITX_RESTART=1\n'
elif ! command -v fcitx5 >/dev/null 2>&1; then
    printf 'Fcitx5 executable not found; installation completed; start it after installation with: fcitx5 -u modernime-ui\n' >&2
elif ! command -v fcitx5-remote >/dev/null 2>&1; then
    printf 'fcitx5-remote executable not found; installation completed; start it after installation with: fcitx5 -u modernime-ui\n' >&2
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'No graphical session detected; Fcitx5 auto-start skipped; start it from a graphical session with: fcitx5 -u modernime-ui\n' >&2
elif [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    printf 'DBus session not detected; Fcitx5 auto-start skipped; start it from a graphical session with: fcitx5 -u modernime-ui\n' >&2
else
    start_fcitx5_safely &
    printf 'Fcitx5 restart requested with the ModernIME UI addon\n'
    activated=false
    for attempt in {1..20}; do
        if run_fcitx5_remote -s modernime >/dev/null &&
           run_fcitx5_remote -o >/dev/null; then
            current_input_method=$(run_fcitx5_remote -n) || current_input_method=""
            current_input_status=$(run_fcitx5_remote) || current_input_status=""
            if [[ "$current_input_method" == "modernime" &&
                  "$current_input_status" == "2" ]]; then
                activated=true
                break
            fi
        fi
        sleep 0.2
    done
    if [[ "$activated" == true ]]; then
        printf 'ModernIME input method activated\n'
    else
        printf 'ModernIME input method could not be activated automatically; use: fcitx5-remote -s modernime\n' >&2
    fi
fi

printf 'ModernIME installed to %s\n' "$prefix"
printf 'Install manifest: %s\n' "$manifest"
printf 'Offline pinyin knowledge dictionary: %s/share/modernime/pinyin/modernime-knowledge.dict\n' "$prefix"
printf 'Launch settings client: %s\n' "$prefix/bin/modernime-settings"
printf 'Desktop shortcut: %s\n' "$desktop_shortcut"
printf 'Select the UI addon with: fcitx5 -u modernime-ui\n'
