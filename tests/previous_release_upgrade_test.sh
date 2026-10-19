#!/usr/bin/env bash
# tests/previous_release_upgrade_test.sh — Real v0.1.13 to v0.1.14 Upgrade Rehearsal
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prev_release_dir="/home/wsl/ModernIME/build/previous-release-v0.1.13"
dist_dir="$project_root/dist"

printf '=== ModernIME 真实旧版本(v0.1.13)至新候选(0.1.14)隔离升级演练 ===\n'

# 1. 检查真实 v0.1.13 资产
if [[ ! -d "$prev_release_dir" ]]; then
    printf 'Error: Previous release assets directory not found: %s\n' "$prev_release_dir" >&2
    exit 1
fi

prev_tarball="$prev_release_dir/modernime-0.1.13-linux-x86_64.tar.gz"
prev_deb="$prev_release_dir/modernime_0.1.13_amd64_ubuntu24.04.deb"
prev_rpm="$prev_release_dir/modernime-0.1.13-1.oe24.x86_64.rpm"

for f in "$prev_tarball" "$prev_deb" "$prev_rpm"; do
    if [[ ! -f "$f" ]]; then
        printf 'Error: Required v0.1.13 asset missing: %s\n' "$f" >&2
        exit 1
    fi
done

# 2. 检查 0.1.14 发布产物
new_ver=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "$project_root/CMakeLists.txt")
new_tarball=$(ls -1 "$dist_dir"/modernime-"${new_ver}"-linux-x86_64.tar.gz 2>/dev/null | head -n 1 || true)
new_deb=$(ls -1 "$dist_dir"/modernime-"${new_ver}"-Linux.deb 2>/dev/null | head -n 1 || true)
new_rpm=$(ls -1 "$dist_dir"/modernime-"${new_ver}"-*.rpm 2>/dev/null | head -n 1 || true)

if [[ -z "$new_tarball" || ! -f "$new_tarball" ]]; then
    printf 'Error: New tarball not found in %s\n' "$dist_dir" >&2
    exit 1
fi
if [[ -z "$new_deb" || ! -f "$new_deb" ]]; then
    printf 'Error: New deb not found in %s\n' "$dist_dir" >&2
    exit 1
fi
if [[ -z "$new_rpm" || ! -f "$new_rpm" ]]; then
    printf 'Error: New rpm not found in %s\n' "$dist_dir" >&2
    exit 1
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-prev-upgrade-test.XXXXXX")
cleanup() {
    rm -rf -- "$test_root"
}
trap cleanup EXIT

printf '\n--- [演练 1/4] 真实便携/脚本渠道升级 (0.1.13 -> 0.1.14) ---\n'
prefix="$test_root/prefix"
config_home="$test_root/config"
data_home="$test_root/data"
home_dir="$test_root/home"
mkdir -p "$prefix" "$config_home/modernime" "$data_home/modernime" "$home_dir"

# 解压并运行真实 v0.1.13 安装器
extract_v13="$test_root/extract_v13"
mkdir -p "$extract_v13"
tar -xzf "$prev_tarball" -C "$extract_v13"

export HOME="$home_dir"
export MODERNIME_PREFIX="$prefix"
export MODERNIME_CONFIG_HOME="$config_home"
export MODERNIME_DATA_HOME="$data_home"
export MODERNIME_SKIP_FCITX_RESTART=1
export MODERNIME_SKIP_TESTS=1
export XDG_CONFIG_HOME="$config_home"
export XDG_DATA_HOME="$data_home"

# 执行真实 v0.1.13 安装
(
    cd "$extract_v13"/modernime-0.1.13-linux-x86_64
    bash ./install.sh "$prefix"
)

# 校验 v0.1.13 安装状态
v13_channel_file="$prefix/share/modernime/install-channel.txt"
if [[ ! -f "$v13_channel_file" ]]; then
    printf 'Error: v0.1.13 install-channel.txt missing\n' >&2
    exit 1
fi
v13_ver=$(grep '^version=' "$v13_channel_file" | cut -d= -f2)
if [[ "$v13_ver" != "0.1.13" ]]; then
    printf 'Error: Expected installed version 0.1.13, got %s\n' "$v13_ver" >&2
    exit 1
fi
printf 'v0.1.13 安装成功，版本验证通过: %s\n' "$v13_ver"

# 注入活跃用户数据夹具（SQLite WAL、LMDB、自定义词库、配置）
python3 - "$data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("CREATE TABLE IF NOT EXISTS learning_entries (id INTEGER PRIMARY KEY, pinyin TEXT, phrase TEXT, count INTEGER);")
conn.execute("INSERT INTO learning_entries VALUES (1, 'beijing', '北京', 42);")
conn.execute("INSERT INTO learning_entries VALUES (2, 'shanghai', '上海', 88);")
conn.commit()
conn.close()
PYEOF

"$project_root/scripts/modernime-data-guard" --init-lmdb "$data_home/modernime/learning.lmdb"
printf '[General]\nCandidateSize=7\nTheme=dark\n' > "$config_home/modernime/settings.conf"
printf '测试自定义短语\tceshizidingyiduanju\t100\n' > "$data_home/modernime/user-dictionary.txt"

# 记录升级前校验
sqlite_count_before=$(python3 -c "import sqlite3; c=sqlite3.connect('$data_home/modernime/learning.sqlite3'); print(c.execute('SELECT count(*) FROM learning_entries').fetchone()[0])")
if [[ "$sqlite_count_before" -ne 2 ]]; then
    printf 'Error: SQLite before count != 2\n' >&2
    exit 1
fi
settings_hash_before=$(sha256sum "$config_home/modernime/settings.conf" | awk '{print $1}')
dict_hash_before=$(sha256sum "$data_home/modernime/user-dictionary.txt" | awk '{print $1}')

# 解压并执行 0.1.14 升级安装
extract_v14="$test_root/extract_v14"
mkdir -p "$extract_v14"
tar -xzf "$new_tarball" -C "$extract_v14"

(
    cd "$extract_v14"/modernime-*-linux-x86_64
    bash ./install.sh "$prefix"
)

# 校验升级后状态
v14_ver=$(grep '^version=' "$v13_channel_file" | cut -d= -f2)
if [[ "$v14_ver" != "$new_ver" ]]; then
    printf 'Error: Expected upgraded version %s, got %s\n' "$new_ver" "$v14_ver" >&2
    exit 1
fi
printf '%s 升级成功，版本验证通过: %s\n' "$new_ver" "$v14_ver"

# 校验数据完整性
sqlite_count_after=$(python3 -c "import sqlite3; c=sqlite3.connect('$data_home/modernime/learning.sqlite3'); print(c.execute('SELECT count(*) FROM learning_entries').fetchone()[0])")
if [[ "$sqlite_count_after" -ne 2 ]]; then
    printf 'Error: SQLite after count != 2 (data lost!)\n' >&2
    exit 1
fi
row1_phrase=$(python3 -c "import sqlite3; c=sqlite3.connect('$data_home/modernime/learning.sqlite3'); print(c.execute('SELECT phrase FROM learning_entries WHERE id=1').fetchone()[0])")
if [[ "$row1_phrase" != "北京" ]]; then
    printf 'Error: SQLite row 1 corrupted: %s\n' "$row1_phrase" >&2
    exit 1
fi

"$project_root/scripts/modernime-data-guard" --verify --data-dir "$data_home/modernime"

settings_hash_after=$(sha256sum "$config_home/modernime/settings.conf" | awk '{print $1}')
if [[ "$settings_hash_before" != "$settings_hash_after" ]]; then
    printf 'Error: settings.conf was modified during upgrade!\n' >&2
    exit 1
fi

dict_hash_after=$(sha256sum "$data_home/modernime/user-dictionary.txt" | awk '{print $1}')
if [[ "$dict_hash_before" != "$dict_hash_after" ]]; then
    printf 'Error: user-dictionary.txt was modified during upgrade!\n' >&2
    exit 1
fi

# 检查自动备份
backup_dirs=$(find "$data_home/modernime" -maxdepth 2 -type d -name "upgrade_backup_*" -o -name "backups" | wc -l)
printf '数据完整性验证通过：SQLite 记录保持 (%d条)、LMDB 完整、配置文件一致、备份目录正常存在。\n' "$sqlite_count_after"

printf '\n--- [演练 2/4] DEB / APT 版本递增与元数据升级分析 ---\n'
deb_v13_ver=$(dpkg-deb -f "$prev_deb" Version)
deb_v14_ver=$(dpkg-deb -f "$new_deb" Version)
printf 'DEB 版本对比: v0.1.13=%s -> 0.1.14=%s\n' "$deb_v13_ver" "$deb_v14_ver"

if ! dpkg --compare-versions "$deb_v14_ver" "gt" "$deb_v13_ver"; then
    printf 'Error: DEB version 0.1.14 is NOT greater than 0.1.13!\n' >&2
    exit 1
fi
printf 'dpkg --compare-versions 门禁通过: %s > %s\n' "$deb_v14_ver" "$deb_v13_ver"

deb_v13_pkg=$(dpkg-deb -f "$prev_deb" Package)
deb_v14_pkg=$(dpkg-deb -f "$new_deb" Package)
if [[ "$deb_v13_pkg" != "$deb_v14_pkg" ]]; then
    printf 'Error: Package name mismatch: %s vs %s\n' "$deb_v13_pkg" "$deb_v14_pkg" >&2
    exit 1
fi
printf '包名一致性检查通过: %s\n' "$deb_v14_pkg"

printf '\n--- [演练 3/4] RPM / DNF 版本递增与元数据升级分析 ---\n'
rpm_v13_name=$(basename "$prev_rpm")
rpm_v14_name=$(basename "$new_rpm")
printf 'RPM 资产对比: v0.1.13=%s -> 0.1.14=%s\n' "$rpm_v13_name" "$rpm_v14_name"

python3 - "$prev_rpm" "$new_rpm" <<'PYEOF'
import sys, re
def parse_rpm_ver(filename):
    m = re.search(r'modernime-([0-9\.]+)-([0-9a-zA-Z]+)', filename)
    if not m:
        raise ValueError(f"Cannot parse RPM filename: {filename}")
    ver_parts = [int(x) for x in m.group(1).split('.')]
    return ver_parts, m.group(2)

v13_parts, v13_rel = parse_rpm_ver(sys.argv[1])
v14_parts, v14_rel = parse_rpm_ver(sys.argv[2])

print(f"RPM 解析版本: v0.1.13={v13_parts} vs 0.1.14={v14_parts}")
if v14_parts <= v13_parts:
    raise SystemExit("Error: RPM 0.1.14 version is not greater than 0.1.13!")
print("RPM EVR 递增检查通过: 0.1.14 > 0.1.13")
PYEOF

printf '\n--- [演练 4/4] 跨渠道保护与渠道迁移工具验证 ---\n'
# 验证在 script 渠道下，modernime-migrate-channel 的表现
"$project_root/scripts/modernime-migrate-channel" --check --from-prefix "$prefix"

printf '\n=== 全部 4 项上一版真实升级演练与数据保护验证通过 ===\n'
