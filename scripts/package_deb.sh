#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${MODERNIME_DEB_BUILD_DIR:-${project_root}/build/package-deb}"
dist_dir="${MODERNIME_DEB_DIST_DIR:-${project_root}/dist}"
distro_tag="${MODERNIME_DEB_DISTRO_TAG:-}"

mkdir -p "${dist_dir}"
chmod 0755 "${project_root}/packaging/debian/preinst" \
           "${project_root}/packaging/debian/postinst" \
           "${project_root}/packaging/debian/postrm" 2>/dev/null || true

printf '==> Configuring ModernIME for DEB packaging...\n'
cmake -S "${project_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMODERNIME_PACKAGE_CHANNEL=deb \
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
if [ -n "${distro_tag}" ]; then
    version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "${project_root}/CMakeLists.txt")
    arch=$(dpkg --print-architecture 2>/dev/null || uname -m)
    deb_src=$(ls -1 "${build_dir}"/*.deb | head -n 1)
    cp -f "${deb_src}" "${dist_dir}/modernime_${version}_${arch}_${distro_tag}.deb"
fi
printf '==> DEB package generated in %s:\n' "${dist_dir}"
ls -lh "${dist_dir}"/*.deb
