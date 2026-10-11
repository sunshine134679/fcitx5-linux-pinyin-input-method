#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-settings-install-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

fake_bin="$test_root/bin"
mkdir -p "$fake_bin"
cat >"$fake_bin/cmake" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ " $* " == *" --install "* ]]; then
    mkdir -p "$FAKE_PREFIX/bin" "$FAKE_PREFIX/share/applications"
    : >"$FAKE_PREFIX/bin/modernime-settings"
    cp "$PROJECT_ROOT/config/desktop/modernime-settings.desktop" \
        "$FAKE_PREFIX/share/applications/modernime-settings.desktop"
fi
EOF
cat >"$fake_bin/ctest" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$fake_bin/pkg-config" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' /fake/lib
EOF
cat >"$fake_bin/fcitx5" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$fake_bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
case "${1-}" in
    -n) printf '%s\n' modernime ;;
    '') printf '%s\n' 2 ;;
esac
exit 0
EOF
cat >"$fake_bin/pgrep" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' 100
EOF
chmod +x "$fake_bin"/*

prefix="$test_root/prefix"
config="$test_root/config"
mkdir -p "$prefix/share/applications" "$test_root/home/Desktop"
printf '%s\n' unrelated >"$prefix/share/applications/unrelated.desktop"
printf '%s\n' unrelated >"$test_root/home/Desktop/unrelated.desktop"
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/home" \
    XDG_CONFIG_HOME="$config" \
    MODERNIME_PREFIX="$prefix" \
    MODERNIME_BUILD_DIR="$test_root/build" \
    FAKE_PREFIX="$prefix" \
    PROJECT_ROOT="$project_root" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" >/dev/null

manifest="$prefix/share/modernime/install-manifest.txt"
grep -Fqx "$prefix/share/modernime/pinyin/modernime-knowledge.dict" "$manifest"
grep -Fqx "$prefix/bin/modernime-settings" "$manifest"
grep -Fqx "$prefix/share/applications/modernime-settings.desktop" "$manifest"
grep -Fqx "$test_root/home/Desktop/modernime-settings.desktop" "$manifest"
grep -Fqx 'Exec=modernime-settings' \
    "$prefix/share/applications/modernime-settings.desktop"
grep -Fqx 'Name=ModernIME 设置' \
    "$prefix/share/applications/modernime-settings.desktop"
grep -Fqx 'Comment=配置 ModernIME 输入体验与个人数据' \
    "$prefix/share/applications/modernime-settings.desktop"
grep -Fqx "Exec=$prefix/bin/modernime-settings" \
    "$test_root/home/Desktop/modernime-settings.desktop"

# A preserved symlink must not have its target permissions changed.
desktop_file="$test_root/home/Desktop/modernime-settings.desktop"
mv "$desktop_file" "$test_root/user-shortcut.desktop"
chmod 600 "$test_root/user-shortcut.desktop"
ln -s "$test_root/user-shortcut.desktop" "$desktop_file"
env -u MODERNIME_CONFIG_HOME -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/home" XDG_CONFIG_HOME="$config" \
    MODERNIME_PREFIX="$prefix" MODERNIME_BUILD_DIR="$test_root/build" \
    FAKE_PREFIX="$prefix" PROJECT_ROOT="$project_root" \
    PATH="$fake_bin:/usr/bin:/bin" bash "$project_root/install.sh" >/dev/null
test "$(stat -c %a "$test_root/user-shortcut.desktop")" = 600
rm "$desktop_file"
mv "$test_root/user-shortcut.desktop" "$desktop_file"

# Existing identical files without ownership records remain unowned.
legacy_prefix="$test_root/legacy-prefix"
legacy_config="$test_root/legacy-config"
legacy_desktop="$test_root/legacy-desktop"
legacy_env=(env HOME="$test_root/home" MODERNIME_CONFIG_HOME="$legacy_config"
    MODERNIME_DESKTOP_DIR="$legacy_desktop" MODERNIME_PREFIX="$legacy_prefix"
    MODERNIME_BUILD_DIR="$test_root/legacy-build" FAKE_PREFIX="$legacy_prefix"
    PROJECT_ROOT="$project_root" MODERNIME_SKIP_FCITX_RESTART=1
    PATH="$fake_bin:/usr/bin:/bin")
"${legacy_env[@]}" bash "$project_root/install.sh" >/dev/null
rm -r "$legacy_prefix/share/modernime/managed-files"
"${legacy_env[@]}" bash "$project_root/install.sh" >/dev/null
"${legacy_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
test -f "$legacy_config/environment.d/90-modernime.conf"
test -f "$legacy_config/autostart/modernime-fcitx5-session.desktop"
test -f "$legacy_desktop/modernime-settings.desktop"

# Reinstall must preserve complete user-edited integration files.
environment_file="$config/environment.d/90-modernime.conf"
autostart_file="$config/autostart/modernime-fcitx5-session.desktop"
desktop_file="$test_root/home/Desktop/modernime-settings.desktop"
printf '%s\n' 'USER_SETTING=keep-me' >> "$environment_file"
printf '%s\n' 'X-User-Setting=keep-me' >> "$autostart_file"
printf '%s\n' 'X-User-Setting=keep-me' >> "$desktop_file"
cp "$environment_file" "$test_root/environment.expected"
cp "$autostart_file" "$test_root/autostart.expected"
cp "$desktop_file" "$test_root/desktop.expected"
env -u MODERNIME_CONFIG_HOME -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/home" XDG_CONFIG_HOME="$config" \
    MODERNIME_PREFIX="$prefix" MODERNIME_BUILD_DIR="$test_root/build" \
    FAKE_PREFIX="$prefix" PROJECT_ROOT="$project_root" \
    PATH="$fake_bin:/usr/bin:/bin" bash "$project_root/install.sh" >/dev/null
cmp "$environment_file" "$test_root/environment.expected"
cmp "$autostart_file" "$test_root/autostart.expected"
cmp "$desktop_file" "$test_root/desktop.expected"
# Deletion protection also covers one-line edits, not just added lines.
printf '%s\n' 'FCITX_ADDON_DIRS=/my/custom/fcitx5' > "$environment_file"
cp "$environment_file" "$test_root/environment.expected"

env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/home" \
    XDG_CONFIG_HOME="$config" \
    MODERNIME_PREFIX="$prefix" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/uninstall.sh" >/dev/null
test ! -e "$prefix/bin/modernime-settings"
test ! -e "$prefix/share/applications/modernime-settings.desktop"
test -f "$prefix/share/applications/unrelated.desktop"
cmp "$environment_file" "$test_root/environment.expected"
cmp "$autostart_file" "$test_root/autostart.expected"
cmp "$desktop_file" "$test_root/desktop.expected"
# Missing integration files must be harmless on a repeated uninstall.
rm -f "$environment_file" "$autostart_file" "$desktop_file"
env -u MODERNIME_CONFIG_HOME -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/home" XDG_CONFIG_HOME="$config" \
    MODERNIME_PREFIX="$prefix" PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/uninstall.sh" >/dev/null
test ! -e "$manifest"
test -f "$test_root/home/Desktop/unrelated.desktop"

# A custom config directory must leave HOME/.xprofile untouched.
printf '%s\n' '# user profile' 'export USER_PROFILE=keep' > "$test_root/home/.xprofile"
cp "$test_root/home/.xprofile" "$test_root/profile.expected"
profile_env=(env -u MODERNIME_DESKTOP_DIR -u MODERNIME_SKIP_XPROFILE
    HOME="$test_root/home" XDG_CONFIG_HOME="$config"
    MODERNIME_CONFIG_HOME="$config" MODERNIME_PREFIX="$prefix"
    MODERNIME_BUILD_DIR="$test_root/build" FAKE_PREFIX="$prefix"
    PROJECT_ROOT="$project_root" MODERNIME_SKIP_FCITX_RESTART=1
    PATH="$fake_bin:/usr/bin:/bin")
"${profile_env[@]}" bash "$project_root/install.sh" >/dev/null
cmp "$test_root/home/.xprofile" "$test_root/profile.expected"
"${profile_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
cmp "$test_root/home/.xprofile" "$test_root/profile.expected"

# An explicitly selected profile is recorded and only its unchanged block removed.
profile="$test_root/custom-profile"
printf '%s\n' '# existing' > "$profile"
"${profile_env[@]}" MODERNIME_XPROFILE_FILE="$profile" bash "$project_root/install.sh" >/dev/null
grep -Fqx "$profile" "$manifest"
printf '%s\n' 'export USER_ADDITION=keep' >> "$profile"
# Uninstall uses the recorded path even when the override is no longer present.
"${profile_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
grep -Fqx '# existing' "$profile"
grep -Fqx 'export USER_ADDITION=keep' "$profile"
! grep -Fq '# BEGIN MODERNIME ENV' "$profile"
cmp "$test_root/home/.xprofile" "$test_root/profile.expected"

"${profile_env[@]}" MODERNIME_XPROFILE_FILE="$profile" bash "$project_root/install.sh" >/dev/null
sed -i 's/export GTK_IM_MODULE=fcitx/export GTK_IM_MODULE=custom/' "$profile"
cp "$profile" "$test_root/custom-profile.expected"
"${profile_env[@]}" MODERNIME_XPROFILE_FILE="$profile" bash "$project_root/install.sh" >/dev/null
cmp "$profile" "$test_root/custom-profile.expected"
"${profile_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
cmp "$profile" "$test_root/custom-profile.expected"
test -f "$manifest"
rm "$profile"
"${profile_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
test ! -e "$manifest"

# Switching profile paths must clean up the old unmodified block.
profile_a="$test_root/profile-a"
profile_b="$test_root/profile-b"
printf '%s\n' '# first user profile' > "$profile_a"
printf '%s\n' '# second user profile' > "$profile_b"
"${profile_env[@]}" MODERNIME_XPROFILE_FILE="$profile_a" bash "$project_root/install.sh" >/dev/null
"${profile_env[@]}" MODERNIME_XPROFILE_FILE="$profile_b" bash "$project_root/install.sh" >/dev/null
! grep -Fq '# BEGIN MODERNIME ENV' "$profile_a"
grep -Fq '# BEGIN MODERNIME ENV' "$profile_b"
# A partial uninstall followed by retry must not retain an already-cleaned profile.
printf '%s\n' 'USER_SETTING=keep' >> "$config/environment.d/90-modernime.conf"
"${profile_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
! grep -Fq '# BEGIN MODERNIME ENV' "$profile_b"
rm "$config/environment.d/90-modernime.conf"
"${profile_env[@]}" bash "$project_root/uninstall.sh" >/dev/null
test ! -e "$manifest"
grep -Fqx '# first user profile' "$profile_a"
grep -Fqx '# second user profile' "$profile_b"
