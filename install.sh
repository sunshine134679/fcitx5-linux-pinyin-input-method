#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
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

fcitx_environment=(
    env
    "FCITX_ADDON_DIRS=$prefix/lib/fcitx5:$system_addon_dir"
)
for environment_name in DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR; do
    if [[ -n "${!environment_name:-}" ]]; then
        fcitx_environment+=("$environment_name=${!environment_name}")
    fi
done

cmake_args=(
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_INSTALL_PREFIX="$prefix"
    -DMODERNIME_BUILD_FCITX5=ON
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON
    -DMODERNIME_BUILD_TESTS=ON
)
if [[ -n "${MODERNIME_BOOST_ROOT:-}" ]]; then
    cmake_args+=("-DBoost_ROOT=$MODERNIME_BOOST_ROOT")
fi

cmake -S "$project_root" -B "$build_dir" -G "$generator" "${cmake_args[@]}"
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
cmake --install "$build_dir"

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
    printf '%s\n' "$prefix/share/fcitx5/addon/modernime.conf"
    printf '%s\n' "$prefix/share/fcitx5/addon/modernime-ui.conf"
    printf '%s\n' "$prefix/share/fcitx5/inputmethod/modernime.conf"
    printf '%s\n' "$environment_file"
    printf '%s\n' "$autostart_file"
} > "$manifest"

if command -v fcitx5 >/dev/null 2>&1; then
    previous_fcitx_pid=$(pgrep -o -x fcitx5 || true)
    "${fcitx_environment[@]}" fcitx5 -d -r -u modernime-ui >/dev/null 2>&1 &
    printf 'Fcitx5 restart requested with the ModernIME UI addon\n'
    if command -v fcitx5-remote >/dev/null 2>&1; then
        activated=false
        for attempt in {1..20}; do
            current_fcitx_pid=$(pgrep -o -x fcitx5 || true)
            if [[ -n "$current_fcitx_pid" &&
                  "$current_fcitx_pid" != "$previous_fcitx_pid" ]] &&
               "${fcitx_environment[@]}" fcitx5-remote -s modernime \
                   >/dev/null 2>&1 &&
               "${fcitx_environment[@]}" fcitx5-remote -o >/dev/null 2>&1 &&
               [[ "$("${fcitx_environment[@]}" fcitx5-remote -n 2>/dev/null)" == "modernime" ]] &&
               [[ "$("${fcitx_environment[@]}" fcitx5-remote 2>/dev/null)" == "2" ]]; then
                activated=true
                break
            fi
            sleep 0.2
        done
        if [[ "$activated" == true ]]; then
            printf 'ModernIME input method activated\n'
        else
            printf 'ModernIME input method could not be activated automatically; use: fcitx5-remote -s modernime\n' >&2
        fi
    fi
else
    printf 'Fcitx5 executable not found; start it after installation with: fcitx5 -u modernime-ui\n' >&2
fi

printf 'ModernIME installed to %s\n' "$prefix"
printf 'Install manifest: %s\n' "$manifest"
printf 'Select the UI addon with: fcitx5 -u modernime-ui\n'
