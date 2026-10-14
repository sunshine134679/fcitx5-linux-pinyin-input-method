#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-install-transaction.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT
fake_bin="$test_root/fake-bin"
mkdir -p "$fake_bin"

# Only the expensive build is replaced; payload backups, copying and rollback
# run through the real installer and filesystem.
cat > "$fake_bin/cmake" <<'CMAKE'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
    --version) printf 'cmake version 3.24.0\n' ;;
    --install)
        target="${DESTDIR:-}$MODERNIME_PREFIX"
        mkdir -p "$target"
        /bin/cp -a "$TEST_PAYLOAD/." "$target/"
        if [[ "${TEST_FAILURE:-}" == stage ]]; then
            printf 'injected staging failure\n' >&2
            exit 1
        fi
        ;;
    --build)
        if [[ "${TEST_FAILURE:-}" == learning ]]; then
            python3 "$TEST_LEARNING_WRITER"
        fi
        ;;
    *)
        mkdir -p "$MODERNIME_BUILD_DIR"
        printf 'CMAKE_INSTALL_LIBDIR:PATH=%s\n' "${TEST_CMAKE_LIBDIR:-lib}" > "$MODERNIME_BUILD_DIR/CMakeCache.txt"
        ;;
esac
CMAKE
cat > "$fake_bin/cp" <<'COPY'
#!/usr/bin/env bash
set -euo pipefail
staged_source=false
for arg in "$@"; do
    [[ "$arg" == */stage/* ]] && staged_source=true
    case "$arg" in
        "$MODERNIME_PREFIX/."|"$MODERNIME_PREFIX/unrelated"*)
            printf 'unrelated prefix contents copied\n' >> "$TEST_COPY_LOG"
            ;;
    esac
    if [[ "${TEST_FAILURE:-}" == backup &&
          ( "$arg" == "$MODERNIME_PREFIX/bin/modernime-settings" ||
            "$arg" == "$MODERNIME_PREFIX/." ) ]]; then
        printf 'injected backup failure\n' >&2
        exit 1
    fi
done
if [[ "${TEST_FAILURE:-}" == binary-learning && "${@: -1}" == "$MODERNIME_PREFIX/" ]]; then
    /bin/cp "$@"
    python3 "$TEST_LEARNING_WRITER"
    exit 0
fi
if [[ "${TEST_FAILURE:-}" == deploy ]]; then
    destination="${@: -1}"
    if [[ "$staged_source" == true && "$destination" == "$MODERNIME_PREFIX/bin/modernime-settings" ]]; then
        /bin/cp "$@"
        printf 'injected deployment failure\n' >&2
        exit 1
    fi
fi
exec /bin/cp "$@"
COPY
for command_name in ctest fcitx5; do
    printf '#!/usr/bin/env bash\nexit 0\n' > "$fake_bin/$command_name"
done
printf '#!/usr/bin/env bash\nprintf /fake/lib\n' > "$fake_bin/pkg-config"
chmod +x "$fake_bin"/*

payload="$test_root/payload"
mkdir -p "$payload/bin" "$payload/lib/fcitx5" "$payload/share/modernime/pinyin"
printf 'new executable\n' > "$payload/bin/modernime-settings"
printf 'new plugin\n' > "$payload/lib/fcitx5/modernime_fcitx5.so"
printf 'new dictionary\n' > "$payload/share/modernime/pinyin/new dictionary.dict"
ln -s modernime-settings "$payload/bin/modernime-link"
mkdir -p "$payload/share/doc/modernime"
for doc in LICENSE copyright THIRD_PARTY_NOTICES.md SOURCES.json; do
    printf 'installed documentation\n' > "$payload/share/doc/modernime/$doc"
done
binary_package="$test_root/binary-package"
mkdir -p "$binary_package/usr"
/bin/cp -a "$payload/." "$binary_package/usr/"
bash "$project_root/scripts/generate_binary_installer.sh" "$binary_package"
learning_writer="$test_root/write-learning.py"
cat > "$learning_writer" <<'WRITE'
import os, sqlite3
from pathlib import Path
with sqlite3.connect(os.environ['TEST_LEARNING_DB']) as conn:
    conn.execute("INSERT INTO learning_entries (pinyin, phrase) VALUES ('new', 'learned during upgrade')")
Path(os.environ['MODERNIME_CONFIG_HOME'], 'modernime', 'settings.conf').write_text('changed during upgrade\n')
WRITE

failures=0
check() {
    if ! "$@"; then
        printf 'FAIL (%s): %s\n' "$scenario" "$*" >&2
        failures=$((failures + 1))
    fi
}
old_settings="$test_root/old-settings"
old_plugin="$test_root/old-plugin"
printf 'old executable\n' > "$old_settings"
printf 'old plugin\n' > "$old_plugin"

expected_version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "$project_root/CMakeLists.txt")
for scenario in rollback fresh backup deploy stage success binary-success learning binary-learning; do
    case_root="$test_root/$scenario"
    prefix="$case_root/prefix"
    mkdir -p "$case_root/home" "$case_root/tmp"
    if [[ "$scenario" != fresh ]]; then
        mkdir -p "$prefix/bin" "$prefix/lib/fcitx5" "$prefix/unrelated"
        /bin/cp "$old_settings" "$prefix/bin/modernime-settings"
        /bin/cp "$old_plugin" "$prefix/lib/fcitx5/modernime_fcitx5.so"
        ln -s old-target "$prefix/bin/modernime-link"
        printf 'keep unrelated\n' > "$prefix/unrelated/user-file"
        unrelated_inode=$(stat -c %i "$prefix/unrelated/user-file")
        exec 9< "$prefix/lib/fcitx5/modernime_fcitx5.so"
    fi
    installer="$project_root/install.sh"
    if [[ "$scenario" == binary-learning || "$scenario" == binary-success ]]; then
        installer="$binary_package/install.sh"
        # A binary installer must use its bundled version, not an unrelated cache.
        mkdir -p "$case_root/build"
        printf 'PROJECT_VERSION:STATIC=99.99.99\n' > "$case_root/build/CMakeCache.txt"
    fi
    if [[ "$scenario" == learning || "$scenario" == binary-learning ]]; then
        mkdir -p "$case_root/data/modernime" "$case_root/config/modernime"
        printf 'before upgrade\n' > "$case_root/config/modernime/settings.conf"
        python3 - "$case_root/data/modernime/learning.sqlite3" <<'SEED'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as conn:
    conn.execute('CREATE TABLE learning_entries (pinyin TEXT, phrase TEXT)')
    conn.execute("INSERT INTO learning_entries VALUES ('old', 'learned before upgrade')")
SEED
    fi
    copy_log="$case_root/copy.log"
    : > "$copy_log"
    simulate=0
    [[ "$scenario" == rollback || "$scenario" == fresh || "$scenario" == learning || "$scenario" == binary-learning ]] && simulate=1
    if env -u SUDO_USER \
        HOME="$case_root/home" TMPDIR="$case_root/tmp" \
        MODERNIME_PREFIX="$prefix" MODERNIME_BUILD_DIR="$case_root/build" \
        MODERNIME_CONFIG_HOME="$case_root/config" MODERNIME_DATA_HOME="$case_root/data" \
        MODERNIME_DESKTOP_DIR="$case_root/Desktop" MODERNIME_SKIP_FCITX_RESTART=1 \
        MODERNIME_SKIP_TESTS=1 MODERNIME_OVERWRITE_PACKAGE=1 \
        MODERNIME_SIMULATE_INSTALL_FAILURE="$simulate" \
        TEST_FAILURE="$scenario" TEST_PAYLOAD="$payload" TEST_COPY_LOG="$copy_log" \
        TEST_LEARNING_DB="$case_root/data/modernime/learning.sqlite3" TEST_LEARNING_WRITER="$learning_writer" \
        PATH="$fake_bin:/usr/bin:/bin" \
        bash "$installer" > "$case_root/install.log" 2>&1; then
        if [[ "$scenario" != success && "$scenario" != binary-success ]]; then
            printf 'FAIL (%s): installer unexpectedly succeeded\n' "$scenario" >&2
            failures=$((failures + 1))
        fi
    elif [[ "$scenario" == success || "$scenario" == binary-success ]]; then
        printf 'FAIL (success): installer failed\n' >&2
        cat "$case_root/install.log" >&2
        failures=$((failures + 1))
    fi
    check test ! -s "$copy_log"
    case "$scenario" in
        backup) check grep -qF 'injected backup failure' "$case_root/install.log" ;;
        deploy) check grep -qF 'injected deployment failure' "$case_root/install.log" ;;
        stage) check grep -qF 'injected staging failure' "$case_root/install.log" ;;
    esac
    if [[ "$scenario" == success || "$scenario" == binary-success ]]; then
        check grep -Fxq "version=$expected_version" "$prefix/share/modernime/install-channel.txt"
        check grep -Fxq 'channel=script' "$prefix/share/modernime/install-channel.txt"
        check cmp "$payload/bin/modernime-settings" "$prefix/bin/modernime-settings"
        check cmp "$payload/lib/fcitx5/modernime_fcitx5.so" "$prefix/lib/fcitx5/modernime_fcitx5.so"
        check cmp "$payload/share/modernime/pinyin/new dictionary.dict" "$prefix/share/modernime/pinyin/new dictionary.dict"
        check test "$(readlink "$prefix/bin/modernime-link")" = modernime-settings
        check test "$(stat -c %i "$prefix/unrelated/user-file")" = "$unrelated_inode"
        check cmp "$old_plugin" /proc/self/fd/9
        check test -z "$(find "$case_root/tmp" -mindepth 1 -print -quit)"
        exec 9<&-
        env HOME="$case_root/home" MODERNIME_PREFIX="$prefix" \
            MODERNIME_CONFIG_HOME="$case_root/config" MODERNIME_DESKTOP_DIR="$case_root/Desktop" \
            bash "$project_root/uninstall.sh" > "$case_root/uninstall.log" 2>&1
        for doc in LICENSE copyright THIRD_PARTY_NOTICES.md SOURCES.json; do
            check test ! -e "$prefix/share/doc/modernime/$doc"
        done
        check test ! -d "$prefix/share/doc/modernime"
        check test -f "$prefix/unrelated/user-file"
        continue
    fi
    check test ! -e "$prefix/share/modernime/pinyin/new dictionary.dict"
    check test ! -e "$case_root/config/environment.d/90-modernime.conf"
    if [[ "$scenario" == fresh ]]; then
        check test ! -d "$prefix"
        check test ! -e "$prefix/bin/modernime-settings"
        check test ! -L "$prefix/bin/modernime-link"
        check test ! -e "$prefix/lib/fcitx5/modernime_fcitx5.so"
    else
        check cmp "$old_settings" "$prefix/bin/modernime-settings"
        check cmp "$old_plugin" "$prefix/lib/fcitx5/modernime_fcitx5.so"
        check test "$(readlink "$prefix/bin/modernime-link")" = old-target
        check test "$(stat -c %i "$prefix/unrelated/user-file")" = "$unrelated_inode"
        check cmp "$old_plugin" /proc/self/fd/9
        exec 9<&-
    fi
    check test -z "$(find "$case_root/tmp" -mindepth 1 -print -quit)"
    if [[ "$scenario" == learning || "$scenario" == binary-learning ]]; then
        check python3 - "$case_root/data/modernime/learning.sqlite3" <<'VERIFY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as conn:
    actual={r[0] for r in conn.execute('SELECT phrase FROM learning_entries')}
    assert actual == {'learned before upgrade', 'learned during upgrade'}, actual
VERIFY
        check grep -qF 'changed during upgrade' "$case_root/config/modernime/settings.conf"
    fi
done

# Compatibility aliases belong to the installer only when it created them.
link_payload="$test_root/link-payload"
mkdir -p "$link_payload"
/bin/cp -a "$payload/." "$link_payload/"
mkdir -p "$link_payload/lib/x86_64-linux-gnu"
mv "$link_payload/lib/fcitx5" "$link_payload/lib/x86_64-linux-gnu/fcitx5"
for scenario in link-preexisting link-created link-modified; do
    case_root="$test_root/$scenario"
    prefix="$case_root/home/.local"
    mkdir -p "$prefix/lib" "$case_root/user-addons"
    printf 'keep user addon\n' > "$case_root/user-addons/user-addon.so"
    if [[ "$scenario" == link-preexisting ]]; then
        ln -s "$case_root/user-addons" "$prefix/lib/fcitx5"
    fi
    : > "$case_root/copy.log"
    env -u SUDO_USER HOME="$case_root/home" MODERNIME_PREFIX="$prefix" \
        MODERNIME_BUILD_DIR="$case_root/build" MODERNIME_CONFIG_HOME="$case_root/config" \
        MODERNIME_DATA_HOME="$case_root/data" MODERNIME_DESKTOP_DIR="$case_root/Desktop" \
        MODERNIME_SKIP_TESTS=1 MODERNIME_SKIP_FCITX_RESTART=1 MODERNIME_OVERWRITE_PACKAGE=1 \
        TEST_FAILURE=success TEST_PAYLOAD="$link_payload" TEST_COPY_LOG="$case_root/copy.log" \
        TEST_CMAKE_LIBDIR=lib/x86_64-linux-gnu PATH="$fake_bin:/usr/bin:/bin" \
        bash "$project_root/install.sh" > "$case_root/install.log" 2>&1
    check test -L "$prefix/lib/fcitx5"
    if [[ "$scenario" == link-modified ]]; then
        ln -sfn "$case_root/user-addons" "$prefix/lib/fcitx5"
    fi
    env HOME="$case_root/home" MODERNIME_PREFIX="$prefix" \
        MODERNIME_CONFIG_HOME="$case_root/config" MODERNIME_DESKTOP_DIR="$case_root/Desktop" \
        bash "$project_root/uninstall.sh" > "$case_root/uninstall.log" 2>&1
    if [[ "$scenario" == link-created ]]; then
        check test ! -L "$prefix/lib/fcitx5"
    else
        check test -L "$prefix/lib/fcitx5"
        check test "$(readlink "$prefix/lib/fcitx5")" = "$case_root/user-addons"
    fi
    check test -f "$case_root/user-addons/user-addon.so"
done

if [[ "$failures" -ne 0 ]]; then
    printf '%s source install transaction assertions failed\n' "$failures" >&2
    exit 1
fi
printf 'Source install transaction tests passed\n'
