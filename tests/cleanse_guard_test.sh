#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cleanse_script="$project_root/scripts/cleanse_learning_store.py"
test -f "$cleanse_script"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-cleanse-guard.XXXXXX")
holder_pid=""

cleanup() {
    if [[ -n "$holder_pid" ]]; then
        kill -9 "$holder_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_root"
}
trap cleanup EXIT

# Isolate environment completely
export HOME="$test_root/home"
export XDG_DATA_HOME="$HOME/.local/share"
mkdir -p "$XDG_DATA_HOME/modernime"
data_dir="$XDG_DATA_HOME/modernime"

# Populate synthetic store fixture
printf 'SQLite-dummy-content-v2\n' > "$data_dir/learning.sqlite3"
printf 'wal-dummy-data\n' > "$data_dir/learning.sqlite3-wal"
printf 'shm-dummy-data\n' > "$data_dir/learning.sqlite3-shm"
printf 'lmdb-dummy-data\n' > "$data_dir/learning.lmdb"
printf 'lmdb-lock-dummy\n' > "$data_dir/learning.lmdb-lock"
printf 'dict-bin-dummy\n' > "$data_dir/user_personal_dict.bin"
printf 'backup-dummy-data\n' > "$data_dir/learning.sqlite3.backup-before-34k-import-20261012135317"

compute_tree_hash() {
    local target_dir="$1"
    (
        cd "$target_dir"
        find . -type f | sort | while IFS= read -r f; do
            sha256sum "$f"
        done
    ) | sha256sum | awk '{print $1}'
}

before_tree_hash=$(compute_tree_hash "$data_dir")

echo "[1/4] Testing default invocation without arguments..."
if python3 "$cleanse_script" > "$test_root/stdout.1" 2> "$test_root/stderr.1"; then
    echo "ERROR: cleanse_learning_store.py exited 0, expected non-zero rejection!" >&2
    exit 1
fi
grep -q "ERROR: cleanse_learning_store.py has been permanently disabled" "$test_root/stderr.1"
after_hash=$(compute_tree_hash "$data_dir")
if [[ "$before_tree_hash" != "$after_hash" ]]; then
    echo "ERROR: Data directory content altered during default invocation!" >&2
    exit 1
fi
if [[ -d "$data_dir/quarantine" ]]; then
    echo "ERROR: Quarantine directory created by disabled script!" >&2
    exit 1
fi

echo "[2/4] Testing invocation with various bypass flags..."
for arg in "--force" "--yes" "-f" "-y" "--clean" "--quarantine" "all" "run"; do
    if python3 "$cleanse_script" "$arg" > "$test_root/stdout.arg" 2> "$test_root/stderr.arg"; then
        echo "ERROR: cleanse_learning_store.py exited 0 with argument '$arg'!" >&2
        exit 1
    fi
    after_hash=$(compute_tree_hash "$data_dir")
    if [[ "$before_tree_hash" != "$after_hash" ]]; then
        echo "ERROR: Data directory content altered with argument '$arg'!" >&2
        exit 1
    fi
done

echo "[3/4] Testing invocation while a simulated active process holds learning files open..."
tail -f "$data_dir/learning.sqlite3" > /dev/null 2>&1 &
holder_pid=$!

if python3 "$cleanse_script" > "$test_root/stdout.holder" 2> "$test_root/stderr.holder"; then
    echo "ERROR: cleanse_learning_store.py exited 0 while file was held!" >&2
    exit 1
fi

kill -9 "$holder_pid" 2>/dev/null || true
holder_pid=""

after_hash=$(compute_tree_hash "$data_dir")
if [[ "$before_tree_hash" != "$after_hash" ]]; then
    echo "ERROR: Data directory content altered during file holder test!" >&2
    exit 1
fi

echo "[4/4] Testing invocation in an empty directory without pre-existing files..."
empty_home="$test_root/empty_home"
export HOME="$empty_home"
export XDG_DATA_HOME="$empty_home/.local/share"
if python3 "$cleanse_script" > "$test_root/stdout.empty" 2> "$test_root/stderr.empty"; then
    echo "ERROR: cleanse_learning_store.py exited 0 in empty environment!" >&2
    exit 1
fi
if [[ -d "$empty_home" ]]; then
    echo "ERROR: directories were created in an empty home environment!" >&2
    exit 1
fi

echo "All cleanse guard assertions passed successfully."
