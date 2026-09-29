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

cmake --build "${build_dir}" --parallel "$(nproc 2>/dev/null || echo 2)"

printf '==> Staging files...\n'
DESTDIR="${stage_dir}/${tar_name}" cmake --install "${build_dir}"

# Create quick standalone installer script
cat << 'EOF' > "${stage_dir}/${tar_name}/install.sh"
#!/usr/bin/env bash
set -euo pipefail

prefix="${1:-$HOME/.local}"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

printf '==> Installing ModernIME binary package to %s...\n' "$prefix"

mkdir -p "$prefix/bin" "$prefix/lib/fcitx5" "$prefix/share/fcitx5/addon" "$prefix/share/fcitx5/inputmethod" "$prefix/share/modernime/pinyin" "$prefix/share/applications"

# Copy binaries and libraries
cp -f "$script_dir/usr/bin/modernime-settings" "$prefix/bin/"
chmod +x "$prefix/bin/modernime-settings"

# Find .so in usr/lib*
find "$script_dir/usr" -type f -name 'modernime_*.so' -exec cp -f {} "$prefix/lib/fcitx5/" \;

# Copy data and configs
cp -rf "$script_dir/usr/share/fcitx5/"* "$prefix/share/fcitx5/"
cp -rf "$script_dir/usr/share/modernime/"* "$prefix/share/modernime/"
cp -rf "$script_dir/usr/share/applications/"* "$prefix/share/applications/"

# Reload fcitx5 if running
if command -v fcitx5-remote >/dev/null 2>&1; then
    fcitx5-remote -r >/dev/null 2>&1 || true
fi

printf '==> ModernIME successfully installed to %s!\n' "$prefix"
printf 'You can now configure it with: %s/bin/modernime-settings\n' "$prefix"
EOF

chmod +x "${stage_dir}/${tar_name}/install.sh"

cat << EOF > "${stage_dir}/${tar_name}/README.txt"
ModernIME - Modern Fcitx5 Pinyin Input Method
Version: ${version}
Repository: https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method

Installation:
  1. User installation (recommended, no sudo needed):
     ./install.sh

  2. System-wide installation:
     sudo ./install.sh /usr

Dependencies:
  - fcitx5
  - libime / libime-pinyin
  - gtk3
  - sqlite3
EOF

printf '==> Packaging tarball...\n'
tar -czf "${dist_dir}/${tar_name}.tar.gz" -C "${stage_dir}" "${tar_name}"

printf '==> Standalone tarball generated in %s:\n' "${dist_dir}"
ls -lh "${dist_dir}/${tar_name}.tar.gz"
