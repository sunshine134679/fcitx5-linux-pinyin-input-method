#!/usr/bin/env bash
set -euo pipefail
export LANG=C.UTF-8
export LC_ALL=C.UTF-8

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${MODERNIME_ARCH_BUILD_DIR:-${project_root}/build/package-arch}"
dist_dir="${MODERNIME_ARCH_DIST_DIR:-${project_root}/dist}"

mkdir -p "${dist_dir}"

if ! command -v makepkg >/dev/null 2>&1; then
    printf 'Error: makepkg is required to build Arch Linux packages (.pkg.tar.zst)\n' >&2
    exit 1
fi

version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "${project_root}/CMakeLists.txt")
if [[ -z "${version}" ]]; then
    printf 'Error: Unable to determine ModernIME version from CMakeLists.txt\n' >&2
    exit 1
fi
arch="${MODERNIME_ARCH_TARGET:-$(uname -m)}"

rm -rf "${build_dir}"
mkdir -p "${build_dir}/src-stage/modernime-${version}"

printf '==> Staging source archive for Arch Linux makepkg (v%s, %s)...\n' "${version}" "${arch}"
tar -C "${project_root}" \
    --exclude='.git' \
    --exclude='./build' \
    --exclude='./dist' \
    -cf - . | tar -C "${build_dir}/src-stage/modernime-${version}" -xf -

tar -C "${build_dir}/src-stage" -czf "${build_dir}/modernime-${version}.tar.gz" "modernime-${version}"
rm -rf "${build_dir}/src-stage"

cp -f "${project_root}/packaging/arch/PKGBUILD" "${build_dir}/PKGBUILD"
cp -f "${project_root}/packaging/arch/modernime.install" "${build_dir}/modernime.install"
sed -i "s/^pkgver=.*/pkgver=${version}/" "${build_dir}/PKGBUILD"

# makepkg forbids running as root; create/use an unprivileged builder user inside containers.
if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    build_user="${MODERNIME_ARCH_BUILD_USER:-modernime-builder}"
    if ! id "${build_user}" >/dev/null 2>&1; then
        useradd -m -s /bin/bash "${build_user}"
    fi
    chown -R "${build_user}:${build_user}" "${build_dir}"
    run_cmd=(su -s /bin/bash "${build_user}" -c "cd '${build_dir}' && PKGEXT='.pkg.tar.zst' makepkg -f --noconfirm --nodeps")
    if [[ "${MODERNIME_ARCH_CHECK_DEPS:-1}" == "1" ]]; then
        run_cmd=(su -s /bin/bash "${build_user}" -c "cd '${build_dir}' && PKGEXT='.pkg.tar.zst' makepkg -f --noconfirm")
    fi
    "${run_cmd[@]}"
else
    (
        cd "${build_dir}"
        PKGEXT='.pkg.tar.zst' makepkg -f --noconfirm
    )
fi

pkg_file=$(find "${build_dir}" -maxdepth 1 -type f -name "modernime-${version}-*-${arch}.pkg.tar.zst" ! -name '*-debug-*' | head -n 1)
if [[ -z "${pkg_file}" || ! -f "${pkg_file}" ]]; then
    printf 'Error: Expected .pkg.tar.zst artifact not found in %s\n' "${build_dir}" >&2
    exit 1
fi

target_pkg="${dist_dir}/modernime-${version}-1-${arch}.pkg.tar.zst"
cp -f "${pkg_file}" "${target_pkg}"
printf '==> Arch Linux package generated in %s:\n' "${dist_dir}"
ls -lh "${target_pkg}"
