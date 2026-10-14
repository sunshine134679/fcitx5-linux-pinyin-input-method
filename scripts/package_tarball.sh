#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(grep -oP 'project\(ModernIME VERSION \K[0-9.]+' "${project_root}/CMakeLists.txt")
build_dir="${project_root}/build/package-tarball"
stage_dir="${build_dir}/stage"
dist_dir="${project_root}/dist"
tar_name="modernime-${version}-linux-x86_64"

rm -rf "${stage_dir}"
mkdir -p "${stage_dir}/${tar_name}" "${dist_dir}"

printf '==> Building ModernIME for standalone tarball...\n'
cmake -S "${project_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="/usr" \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
    -DMODERNIME_BUILD_SETTINGS=ON \
    -DMODERNIME_BUILD_TESTS=OFF

cmake --build "${build_dir}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc 2>/dev/null || echo 2)}"

printf '==> Staging files...\n'
DESTDIR="${stage_dir}/${tar_name}" cmake --install "${build_dir}"

# Reuse the source install/uninstall lifecycle around the binary payload.
"${project_root}/scripts/generate_binary_installer.sh" "${stage_dir}/${tar_name}"

cat << EOF > "${stage_dir}/${tar_name}/README.txt"
ModernIME - Modern Fcitx5 Pinyin Input Method
Version: ${version}
Target Platform: Linux x86_64 (glibc >= 2.34, libstdc++ >= 3.4.30)
Repository: https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method

Requirements:
  - Architecture: x86_64 / amd64
    (For aarch64 / ARM64 or other architectures, build from source using ./install.sh)
  - Linux distribution with glibc >= 2.34 (Ubuntu 22.04+, openEuler 24.03+, Debian 12+, Fedora 38+)
  - Fcitx5 input method framework installed and configured

Installation:
  1. User installation (recommended, no sudo needed):
     ./install.sh

  2. System-wide installation:
     sudo ./install.sh /usr

  Custom isolated installation:
     MODERNIME_PREFIX=/custom/prefix MODERNIME_CONFIG_HOME=/custom/config \
       MODERNIME_DESKTOP_DIR=/custom/desktop MODERNIME_SKIP_FCITX_RESTART=1 ./install.sh

  Uninstallation (use the same environment/prefix as installation):
     ./uninstall.sh

The installer works completely offline and requires no compiler, cmake, or pkg-config.

Dependencies:
  - python3 >= 3.10 (upgrade backups and migration)
  - coreutils (timeout for session status checks)
  - fcitx5 (with fcitx5-chinese-addons)
  - libime / libime-pinyin
  - gtk3
  - sqlite3
  - lmdb (liblmdb0 / lmdb-libs)
EOF

printf '==> Packaging tarball...\n'
tar -czf "${dist_dir}/${tar_name}.tar.gz" -C "${stage_dir}" "${tar_name}"

printf '==> Standalone tarball generated in %s:\n' "${dist_dir}"
ls -lh "${dist_dir}/${tar_name}.tar.gz"
