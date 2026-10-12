#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
data_guard="$project_root/scripts/modernime-data-guard"
test -x "$data_guard"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-upgrade-data-protection.XXXXXX")
cleanup() {
    rm -rf -- "$test_root"
}
trap cleanup EXIT

# Build a lightweight fake binary payload so we can exercise both generated binary installer
# and source/package upgrade hooks rapidly and deterministically.
payload_pkg="$test_root/pkg"
mkdir -p "$payload_pkg/usr/bin" \
         "$payload_pkg/usr/lib/x86_64-linux-gnu/fcitx5" \
         "$payload_pkg/usr/share/modernime/pinyin" \
         "$payload_pkg/usr/share/fcitx5/addon" \
         "$payload_pkg/usr/share/fcitx5/inputmethod" \
         "$payload_pkg/usr/share/applications"

cat > "$payload_pkg/usr/bin/modernime-settings" <<'EOF'
#!/usr/bin/env bash
echo "modernime-settings 0.1.8"
EOF
cp "$project_root/scripts/modernime-session-init" "$payload_pkg/usr/bin/modernime-session-init"
cp "$project_root/scripts/modernime-data-guard" "$payload_pkg/usr/bin/modernime-data-guard"
chmod +x "$payload_pkg/usr/bin/modernime-settings" \
         "$payload_pkg/usr/bin/modernime-session-init" \
         "$payload_pkg/usr/bin/modernime-data-guard"
printf 'ELF-v0.1.8-fcitx5-plugin\n' > "$payload_pkg/usr/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
printf 'ELF-v0.1.8-ui-plugin\n' > "$payload_pkg/usr/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"
printf 'dict-v0.1.8\n' > "$payload_pkg/usr/share/modernime/pinyin/modernime-knowledge.dict"
printf 'hotwords-v0.1.8\n' > "$payload_pkg/usr/share/modernime/pinyin/modernime-hotwords.dict"
printf 'eng-v0.1.8\n' > "$payload_pkg/usr/share/modernime/pinyin/modernime-english-dict.bin"
printf '[Addon]\nName=ModernIME\n' > "$payload_pkg/usr/share/fcitx5/addon/modernime.conf"
printf '[Addon]\nName=ModernIME UI\n' > "$payload_pkg/usr/share/fcitx5/addon/modernime-ui.conf"
printf '[InputMethod]\nName=ModernIME\n' > "$payload_pkg/usr/share/fcitx5/inputmethod/modernime.conf"
printf '[Desktop Entry]\nName=ModernIME Settings\n' > "$payload_pkg/usr/share/applications/modernime-settings.desktop"

"$project_root/scripts/generate_binary_installer.sh" "$payload_pkg"

# Setup fake fcitx5 and fcitx5-remote in PATH
fake_bin="$test_root/fake-bin"
mkdir -p "$fake_bin"
fcitx_log="$test_root/fcitx.log"
cat > "$fake_bin/fcitx5" <<EOF
#!/usr/bin/env bash
echo "fcitx5:\$*" >> "$fcitx_log"
exit 0
EOF
cat > "$fake_bin/fcitx5-remote" <<EOF
#!/usr/bin/env bash
echo "fcitx5-remote:\$*" >> "$fcitx_log"
if [[ "\${1:-}" == "-n" ]]; then
    echo "modernime"
elif [[ \$# -eq 0 ]]; then
    echo "2"
fi
exit 0
EOF
chmod +x "$fake_bin/fcitx5" "$fake_bin/fcitx5-remote"
export PATH="$fake_bin:$PATH"

prefix="$test_root/prefix"
config_home="$test_root/config"
data_home="$test_root/data"
desktop_dir="$test_root/Desktop"
home_dir="$test_root/home"
mkdir -p "$prefix/bin" \
         "$prefix/lib/x86_64-linux-gnu/fcitx5" \
         "$prefix/share/modernime/pinyin" \
         "$config_home/modernime" \
         "$data_home/modernime" \
         "$desktop_dir" \
         "$home_dir"

# Seed v0.1.7 old installation
cat > "$prefix/bin/modernime-settings" <<'EOF'
#!/usr/bin/env bash
echo "modernime-settings 0.1.7"
EOF
chmod +x "$prefix/bin/modernime-settings"
printf 'ELF-v0.1.7-fcitx5-plugin\n' > "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
printf 'ELF-v0.1.7-ui-plugin\n' > "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"
printf 'channel=script\nversion=0.1.7\nprefix=%s\n' "$prefix" > "$prefix/share/modernime/install-channel.txt"

# === Scenario 2 & 3 & 4 & 6: Seed active WAL SQLite (v0 schema), populated LMDB, and user-modified config/dicts ===
printf 'input.toggle_key=Alt+Space\ncandidate.page_size=7\n' > "$config_home/modernime/settings.conf"
printf 'zhuan\x27ye\t专业术语\t100\n' > "$data_home/modernime/user-dictionary.txt"
printf 'zi\x27ding\x27yi\t自定义词\t80\n' > "$data_home/modernime/user_words.txt"
printf 'ke\x27hu\t客户专有词\t50\n' > "$data_home/modernime/custom_words.txt"

"$data_guard" --init-lmdb "$data_home/modernime/learning.lmdb"

# Create SQLite database with legacy schema (user_version=0, missing suppressed column) AND active uncheckpointed WAL frames
python3 - "$data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys, time
db_path = sys.argv[1]
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
conn.execute("CREATE TABLE learning_entries (pinyin TEXT NOT NULL, phrase TEXT NOT NULL, context_before TEXT NOT NULL DEFAULT '', context_after TEXT NOT NULL DEFAULT '', frequency INTEGER NOT NULL DEFAULT 0, last_selected_ms INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (pinyin, phrase, context_before, context_after));")
conn.execute("INSERT INTO learning_entries VALUES ('ni''hao', '你好世界', '', '', 12, 1700000001);")
conn.commit()
conn.execute("INSERT INTO learning_entries VALUES ('huo''yue', '活跃WAL词条', '上下文', '', 9, 1700000002);")
conn.commit()
# Leave conn open briefly in background or verify WAL file is non-empty before closing without checkpoint
import pathlib
wal_file = pathlib.Path(db_path + "-wal")
assert wal_file.is_file() and wal_file.stat().st_size > 0, "Expected active non-empty WAL file"
PYEOF

# Keep a background reader/writer holding the WAL open during backup to prove online WAL backup works under active WAL lock
python3 - "$data_home/modernime/learning.sqlite3" "$test_root/wal_holder.ready" "$test_root/wal_holder.stop" <<'PYEOF' &
import sqlite3, sys, time, pathlib
db_path, ready_path, stop_path = sys.argv[1], sys.argv[2], sys.argv[3]
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
conn.execute("INSERT OR REPLACE INTO learning_entries VALUES ('bing''fa', '并发WAL词条', '', '', 7, 1700000003);")
conn.commit()
pathlib.Path(ready_path).write_text("ready\n")
while not pathlib.Path(stop_path).exists():
    time.sleep(0.05)
conn.close()
PYEOF
wal_holder_pid=$!
for _ in {1..50}; do
    [[ -f "$test_root/wal_holder.ready" ]] && break
    sleep 0.05
done
test -f "$test_root/wal_holder.ready"
test -s "$data_home/modernime/learning.sqlite3-wal"

# === Scenario 7: Hold open file descriptor (simulating mmap in running Fcitx5) on old .so plugin ===
old_so_path="$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
old_so_inode=$(stat -c '%i' "$old_so_path")
python3 - "$old_so_path" "$test_root/so_holder.ready" "$test_root/so_holder.stop" "$test_root/so_holder.content" <<'PYEOF' &
import mmap, os, sys, time, pathlib
so_path, ready_path, stop_path, out_path = sys.argv[1:5]
fd = os.open(so_path, os.O_RDONLY)
mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
pathlib.Path(ready_path).write_text("ready\n")
while not pathlib.Path(stop_path).exists():
    time.sleep(0.05)
mm.seek(0)
pathlib.Path(out_path).write_bytes(mm.read())
mm.close()
os.close(fd)
PYEOF
so_holder_pid=$!
for _ in {1..50}; do
    [[ -f "$test_root/so_holder.ready" ]] && break
    sleep 0.05
done
test -f "$test_root/so_holder.ready"

# Stop WAL holder right after online backup verification so schema migration can acquire write lock cleanly
"$data_guard" --backup --data-dir "$data_home/modernime" --config-dir "$config_home/modernime" --tag "activewal"
touch "$test_root/wal_holder.stop"
wait "$wal_holder_pid"

# Verify activewal backup captured all 3 WAL entries ('你好世界', '活跃WAL词条', '并发WAL词条')
python3 - "$data_home/modernime/learning.sqlite3.backup-activewal" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
assert conn.execute("PRAGMA integrity_check;").fetchone()[0] == "ok"
phrases = {r[0] for r in conn.execute("SELECT phrase FROM learning_entries;").fetchall()}
assert phrases == {"你好世界", "活跃WAL词条", "并发WAL词条"}, f"Missing WAL phrases: {phrases}"
conn.close()
PYEOF

# === Scenario 5: Mid-upgrade failure must abort upgrade and preserve v0.1.7 ===
if env \
    HOME="$home_dir" \
    MODERNIME_PREFIX="$prefix" \
    MODERNIME_CONFIG_HOME="$config_home" \
    MODERNIME_DATA_HOME="$data_home" \
    MODERNIME_DESKTOP_DIR="$desktop_dir" \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    MODERNIME_SIMULATE_INSTALL_FAILURE=1 \
    bash "$payload_pkg/install.sh" "$prefix" >/dev/null 2>&1; then
    echo "Expected mid-upgrade failure to abort install.sh" >&2
    exit 1
fi
grep -F '0.1.7' "$prefix/bin/modernime-settings" >/dev/null
grep -F 'version=0.1.7' "$prefix/share/modernime/install-channel.txt" >/dev/null

# Also test backup failure aborts upgrade before touching prefix
if env \
    HOME="$home_dir" \
    MODERNIME_PREFIX="$prefix" \
    MODERNIME_CONFIG_HOME="$config_home" \
    MODERNIME_DATA_HOME="$data_home" \
    MODERNIME_DESKTOP_DIR="$desktop_dir" \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    MODERNIME_SIMULATE_BACKUP_FAILURE=1 \
    bash "$payload_pkg/install.sh" "$prefix" >/dev/null 2>&1; then
    echo "Expected backup failure to abort install.sh" >&2
    exit 1
fi
grep -F '0.1.7' "$prefix/bin/modernime-settings" >/dev/null

# === Scenario 1, 4, 6, 7: Perform normal upgrade with Fcitx5 restart and schema migration ===
env \
    HOME="$home_dir" \
    MODERNIME_PREFIX="$prefix" \
    MODERNIME_CONFIG_HOME="$config_home" \
    MODERNIME_DATA_HOME="$data_home" \
    MODERNIME_DESKTOP_DIR="$desktop_dir" \
    DISPLAY=":99" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=/tmp/fake-bus" \
    bash "$payload_pkg/install.sh" "$prefix" >/dev/null

touch "$test_root/so_holder.stop"
wait "$so_holder_pid"

# Verify running Fcitx5 mmap content was NEVER overwritten in-place (inode unlinked cleanly)
test "$(cat "$test_root/so_holder.content")" = "ELF-v0.1.7-fcitx5-plugin"
new_so_inode=$(stat -c '%i' "$old_so_path")
test "$old_so_inode" != "$new_so_inode"
test "$(cat "$old_so_path")" = "ELF-v0.1.8-fcitx5-plugin"

# Verify Fcitx5 restart was triggered
grep -F 'fcitx5:-d -r -u modernime-ui' "$fcitx_log" >/dev/null
grep -F 'fcitx5-remote:-s modernime' "$fcitx_log" >/dev/null

# Verify schema migration upgraded user_version to 1 and added suppressed + negative_feedback columns
python3 - "$data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
assert conn.execute("PRAGMA user_version;").fetchone()[0] == 1
cols = {r[1] for r in conn.execute("PRAGMA table_info(learning_entries);").fetchall()}
assert "suppressed" in cols and "negative_feedback" in cols, f"Missing migrated columns: {cols}"
phrases = {r[0] for r in conn.execute("SELECT phrase FROM learning_entries;").fetchall()}
assert phrases == {"你好世界", "活跃WAL词条", "并发WAL词条"}
conn.close()
PYEOF

# Verify user-modified config and custom dictionaries were preserved intact
grep -F 'input.toggle_key=Alt+Space' "$config_home/modernime/settings.conf" >/dev/null
grep -F '专业术语' "$data_home/modernime/user-dictionary.txt" >/dev/null
grep -F '自定义词' "$data_home/modernime/user_words.txt" >/dev/null
grep -F '客户专有词' "$data_home/modernime/custom_words.txt" >/dev/null

# Verify directory-mode LMDB backup and verification also work
dir_lmdb_root="$test_root/dir-lmdb-test"
mkdir -p "$dir_lmdb_root/learning.lmdb"
python3 - "$dir_lmdb_root/learning.lmdb" "$data_guard" <<'PYEOF'
import ctypes, sys
from importlib.machinery import SourceFileLoader
dg = SourceFileLoader("dg", sys.argv[2]).load_module()
lib = dg._load_liblmdb()
env = ctypes.c_void_p()
lib.mdb_env_create(ctypes.byref(env))
lib.mdb_env_set_maxdbs(env, 4)
lib.mdb_env_set_mapsize(env, 1048576)
lib.mdb_env_open(env, sys.argv[1].encode(), dg.MDB_NOTLS, 0o644)
txn = ctypes.c_void_p()
lib.mdb_txn_begin(env, None, 0, ctypes.byref(txn))
dbi = ctypes.c_uint()
lib.mdb_dbi_open(txn, None, 0, ctypes.byref(dbi))
lib.mdb_txn_commit(txn)
lib.mdb_env_close(env)
PYEOF
"$data_guard" --pre-upgrade --data-dir "$dir_lmdb_root" --config-dir "$config_home/modernime" >/dev/null
test -d "$(find "$dir_lmdb_root" -maxdepth 1 -name 'learning.lmdb.backup-*' -print -quit)"

# === Scenario 8: Post-upgrade data recovery from verified backup ===
printf 'CORRUPTED-SQLITE\n' > "$data_home/modernime/learning.sqlite3"
printf 'CORRUPTED-LMDB\n' > "$data_home/modernime/learning.lmdb"
printf 'CORRUPTED-CONFIG\n' > "$config_home/modernime/settings.conf"
printf 'CORRUPTED-DICT\n' > "$data_home/modernime/user-dictionary.txt"

"$data_guard" --restore --data-dir "$data_home/modernime" --config-dir "$config_home/modernime"
"$data_guard" --verify --data-dir "$data_home/modernime" --config-dir "$config_home/modernime"
grep -F 'input.toggle_key=Alt+Space' "$config_home/modernime/settings.conf" >/dev/null
grep -F '专业术语' "$data_home/modernime/user-dictionary.txt" >/dev/null
python3 - "$data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
assert conn.execute("PRAGMA integrity_check;").fetchone()[0] == "ok"
phrases = {r[0] for r in conn.execute("SELECT phrase FROM learning_entries;").fetchall()}
assert phrases == {"你好世界", "活跃WAL词条", "并发WAL词条"}
conn.close()
PYEOF
