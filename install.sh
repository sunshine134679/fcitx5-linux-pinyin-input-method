#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
build_dir=${MODERNIME_BUILD_DIR:-"$project_root/build/install-debug"}
generator=${CMAKE_GENERATOR:-"Unix Makefiles"}
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
environment_dir="$config_home/environment.d"
environment_file="$environment_dir/90-modernime.conf"
system_libdir=$(pkg-config --variable=libdir Fcitx5Utils 2>/dev/null || true)
system_libdir=${system_libdir:-/usr/lib/x86_64-linux-gnu}
system_addon_dir="$system_libdir/fcitx5"

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
} > "$manifest"

printf 'ModernIME installed to %s\n' "$prefix"
printf 'Install manifest: %s\n' "$manifest"
printf 'Select the UI addon with: fcitx5 -u modernime-ui\n'
