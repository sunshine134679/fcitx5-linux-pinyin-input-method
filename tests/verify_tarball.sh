#!/usr/bin/env bash
set -euo pipefail

archive=${1:?Usage: verify_tarball.sh ARCHIVE [ADDON_LOAD_PROBE [DATA_PROBE]]}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-tarball-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT
tar -xzf "$archive" -C "$test_root"
package=$(find "$test_root" -mindepth 1 -maxdepth 1 -type d -name 'modernime-*' -print -quit)
mkdir -p "$test_root/home" "$test_root/no-build"
for name in cmake ctest pkg-config curl wget fcitx5 fcitx5-remote; do
    cat > "$test_root/no-build/$name" <<'TOOL'
#!/usr/bin/env bash
printf 'Unexpected build or network command: %s\n' "$0" >&2
exit 99
TOOL
    chmod +x "$test_root/no-build/$name"
done
export HOME="$test_root/home"
export MODERNIME_PREFIX="$test_root/custom-prefix"
export MODERNIME_CONFIG_HOME="$test_root/custom-config"
export MODERNIME_DESKTOP_DIR="$test_root/custom-desktop"
export MODERNIME_SKIP_FCITX_RESTART=1
export PATH="$test_root/no-build:$PATH"

bash "$package/install.sh"
for path in \
    "$MODERNIME_PREFIX/share/modernime/install-manifest.txt" \
    "$MODERNIME_PREFIX/share/modernime/pinyin/modernime-knowledge.dict" \
    "$MODERNIME_PREFIX/share/modernime/pinyin/modernime-hotwords.dict" \
    "$MODERNIME_PREFIX/share/modernime/pinyin/modernime-english-dict.bin" \
    "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf" \
    "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop" \
    "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop" \
    "$package/uninstall.sh"; do
    if [[ ! -f "$path" ]]; then
        printf 'Tarball lifecycle artifact missing: %s\n' "$path" >&2
        exit 1
    fi
done
cmp "$package/usr/bin/modernime-settings" "$MODERNIME_PREFIX/bin/modernime-settings"
grep -F "$MODERNIME_PREFIX/share" "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
if [[ $# -ge 2 ]]; then
    addon=$(find "$MODERNIME_PREFIX" -name modernime_fcitx5.so -print -quit)
    "$2" "$addon"
fi
"$MODERNIME_PREFIX/bin/modernime-settings" --version
if [[ $# -ge 3 ]]; then
    "$3" "$MODERNIME_PREFIX/share"
fi
# Reinstall protects a user modification, and uninstall preserves it too.
printf '\n# User-owned customization\n' >> "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
cp "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf" "$test_root/modified.conf"
bash "$package/install.sh"
cmp "$test_root/modified.conf" "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
bash "$package/uninstall.sh"
cmp "$test_root/modified.conf" "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
test ! -e "$MODERNIME_PREFIX/bin/modernime-settings"
test ! -e "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
test ! -e "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
# Restore the managed snapshot, then finish the idempotent uninstall.
cp "$MODERNIME_PREFIX/share/modernime/managed-files/environment.content" \
    "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
bash "$package/uninstall.sh"
bash "$package/uninstall.sh"
test ! -e "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
test ! -e "$MODERNIME_PREFIX/share/modernime/install-manifest.txt"
printf 'Portable tarball install/uninstall lifecycle passed\n'
