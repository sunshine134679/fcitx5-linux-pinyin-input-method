#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
build_dir=${MODERNIME_BUILD_DIR:-"$project_root/build/install-debug"}
generator=${CMAKE_GENERATOR:-"Unix Makefiles"}

cmake -S "$project_root" -B "$build_dir" -G "$generator" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_TESTS=ON
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
cmake --install "$build_dir"

manifest_dir="$prefix/share/modernime"
manifest="$manifest_dir/install-manifest.txt"
mkdir -p "$manifest_dir"
{
    printf '%s\n' "$prefix/lib/fcitx5/modernime_fcitx5.so"
    printf '%s\n' "$prefix/lib/fcitx5/modernime_ui.so"
    printf '%s\n' "$prefix/share/fcitx5/addon/modernime.conf"
    printf '%s\n' "$prefix/share/fcitx5/addon/modernime-ui.conf"
    printf '%s\n' "$prefix/share/fcitx5/inputmethod/modernime.conf"
} > "$manifest"

printf 'ModernIME installed to %s\n' "$prefix"
printf 'Install manifest: %s\n' "$manifest"
printf 'Select the UI addon with: fcitx5 -u modernime-ui\n'
