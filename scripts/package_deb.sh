#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${project_root}/build/package-deb"
dist_dir="${project_root}/dist"

mkdir -p "${dist_dir}"

printf '==> Configuring ModernIME for DEB packaging...\n'
cmake -S "${project_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
    -DMODERNIME_BUILD_SETTINGS=ON \
    -DMODERNIME_BUILD_TESTS=OFF

printf '==> Building ModernIME...\n'
cmake --build "${build_dir}" --parallel "$(nproc 2>/dev/null || echo 2)"

printf '==> Generating DEB package with CPack...\n'
(
    cd "${build_dir}"
    cpack -G DEB
)

cp -f "${build_dir}"/*.deb "${dist_dir}/"
printf '==> DEB package generated in %s:\n' "${dist_dir}"
ls -lh "${dist_dir}"/*.deb
