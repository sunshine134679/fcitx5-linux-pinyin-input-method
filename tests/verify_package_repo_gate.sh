#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "${script_dir}/.." && pwd)

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-pkg-repo-gate.XXXXXX")
cleanup() {
    rm -rf -- "${work_dir}"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

expected_version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "${repo_root}/CMakeLists.txt")
[ -n "${expected_version}" ] || fail "unable to parse version from CMakeLists.txt"

printf '=== [1/6] Building / verifying DEB and RPM packages (v%s) ===\n' "${expected_version}"
deb_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.deb" 2>/dev/null | head -n 1 || true)
if [ -z "${deb_file}" ]; then
    "${repo_root}/scripts/package_deb.sh"
    deb_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.deb" | head -n 1)
fi
[ -f "${deb_file}" ] || fail "DEB package not produced in dist/"

rpm_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.rpm" 2>/dev/null | head -n 1 || true)
if [ -z "${rpm_file}" ]; then
    "${repo_root}/scripts/package_rpm.sh"
    rpm_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.rpm" | head -n 1)
fi
[ -f "${rpm_file}" ] || fail "RPM package not produced in dist/"

printf '=== [2/6] Verifying DEB & RPM file boundaries, dependencies, licenses, and zero user data ===\n'
deb_info=$(dpkg-deb -I "${deb_file}")
deb_contents=$(dpkg-deb -c "${deb_file}")

for required_path in \
    "modernime_fcitx5.so" \
    "modernime_ui.so" \
    "./usr/bin/modernime-settings" \
    "./usr/bin/modernime-session-init" \
    "./usr/bin/modernime-data-guard" \
    "./usr/bin/modernime-migrate-channel" \
    "./usr/share/modernime/pinyin/modernime-knowledge.dict" \
    "./usr/share/modernime/pinyin/modernime-hotwords.dict" \
    "./usr/share/doc/modernime/SOURCES.json" \
    "./usr/share/doc/modernime/THIRD_PARTY_NOTICES.md" \
    "./usr/share/applications/modernime-settings.desktop" \
    "./usr/share/fcitx5/addon/modernime.conf" \
    "./usr/share/fcitx5/addon/modernime-ui.conf" \
    "./usr/share/fcitx5/inputmethod/modernime.conf" \
    "./usr/share/doc/modernime/LICENSE" \
    "./usr/share/doc/modernime/copyright" \
    "./usr/share/modernime/install-channel.txt"; do
    printf '%s\n' "${deb_contents}" | grep -Fq "${required_path}" ||
        fail "DEB missing required file: ${required_path}"
done

for dep_bound in \
    "fcitx5 (>= 5.0.14)" \
    "libime-bin (>= 1.0.11)" \
    "libgtk-3-0 (>= 3.24.0)" \
    "libpango-1.0-0 (>= 1.44.0)" \
    "libcairo2 (>= 1.16.0)" \
    "libsqlite3-0 (>= 3.31.0)" \
    "liblmdb0 (>= 0.9.24)"; do
    printf '%s\n' "${deb_info}" | grep -Fq "${dep_bound}" ||
        fail "DEB missing runtime dependency bound: ${dep_bound}"
done

for forbidden in \
    "learning.sqlite3" \
    "learning.lmdb" \
    "user-dictionary.txt" \
    "settings.conf" \
    "/home/" \
    "/root/" \
    ".local/share" \
    ".config/modernime"; do
    if printf '%s\n' "${deb_contents}" | grep -Fq "${forbidden}"; then
        fail "DEB contains forbidden user data path: ${forbidden}"
    fi
done

# Inspect RPM binary headers and cpio payload
rpm_extract_dir="${work_dir}/rpm-extracted"
mkdir -p "${rpm_extract_dir}"
python3 - "${rpm_file}" "${rpm_extract_dir}" "${expected_version}" <<'PYEOF'
import gzip
from pathlib import Path
import struct
import subprocess
import sys

rpm_path = Path(sys.argv[1]).resolve()
extract_dir = Path(sys.argv[2]).resolve()
expected_ver = sys.argv[3]

raw = rpm_path.read_bytes()
assert raw[:4] == b"\xed\xab\xee\xdb", "Invalid RPM lead magic"

def parse_hdr(data, offset):
    assert data[offset:offset+4] == b"\x8e\xad\xe8\x01", f"Invalid RPM header magic at {offset}"
    nindex, hsize = struct.unpack(">II", data[offset+8:offset+16])
    idx_start = offset + 16
    store_start = idx_start + nindex * 16
    store_end = store_start + hsize
    tags = {}
    for i in range(nindex):
        eo = idx_start + i * 16
        tag, dtype, voff, count = struct.unpack(">IIII", data[eo:eo+16])
        pos = store_start + voff
        if dtype in (6, 9):
            end = data.index(b"\x00", pos)
            tags[tag] = data[pos:end].decode("utf-8", errors="replace")
        elif dtype == 8:
            arr = []
            cur = pos
            for _ in range(count):
                end = data.index(b"\x00", cur)
                arr.append(data[cur:end].decode("utf-8", errors="replace"))
                cur = end + 1
            tags[tag] = arr
        elif dtype == 4:
            vals = list(struct.unpack(f">{count}I", data[pos:pos+4*count]))
            tags[tag] = vals if count > 1 else vals[0]
    return tags, store_end

_, sig_end = parse_hdr(raw, 96)
hdr_off = (sig_end + 7) & ~7
if raw[hdr_off:hdr_off+4] != b"\x8e\xad\xe8\x01":
    hdr_off = raw.find(b"\x8e\xad\xe8\x01", sig_end)
tags, payload_off = parse_hdr(raw, hdr_off)

assert tags.get(1000) == "modernime", f"Unexpected RPM name: {tags.get(1000)}"
assert tags.get(1001) == expected_ver, f"Unexpected RPM version: {tags.get(1001)}"
expected_lic = "Apache-2.0 AND LGPL-2.1-or-later AND CC-BY-SA-3.0 AND BSD-3-Clause AND MIT"
assert tags.get(1014) == expected_lic, f"Unexpected RPM license: {tags.get(1014)}"

req_names = tags.get(1049, [])
req_vers = tags.get(1050, [])
req_map = dict(zip(req_names, req_vers))
for req_pkg, min_ver in [
    ("fcitx5", "5.0.14"),
    ("libime", "1.0.11"),
    ("gtk3", "3.24.0"),
    ("sqlite-libs", "3.37.0"),
    ("lmdb-libs", "0.9.24"),
    ("python3", "3.9"),
]:
    assert req_map.get(req_pkg) == min_ver, f"RPM missing dependency {req_pkg} >= {min_ver} (got {req_map.get(req_pkg)})"

# Extract prein, postin, postun scripts for lifecycle test
if 1023 in tags:
    (extract_dir / "prein.sh").write_text(tags[1023], encoding="utf-8")
if 1024 in tags:
    (extract_dir / "postin.sh").write_text(tags[1024], encoding="utf-8")
if 1026 in tags:
    (extract_dir / "postun.sh").write_text(tags[1026], encoding="utf-8")

# Decompress cpio payload and extract into extract_dir/rootfs
rootfs = extract_dir / "rootfs"
rootfs.mkdir(parents=True, exist_ok=True)
cpio_bytes = gzip.decompress(raw[payload_off:])
subprocess.run(
    ["cpio", "-id", "--quiet", "--no-absolute-filenames"],
    cwd=str(rootfs),
    input=cpio_bytes,
    check=True,
)
PYEOF

for rpm_req_file in \
    "usr/bin/modernime-settings" \
    "usr/bin/modernime-session-init" \
    "usr/bin/modernime-data-guard" \
    "usr/bin/modernime-migrate-channel" \
    "usr/share/modernime/pinyin/modernime-knowledge.dict" \
    "usr/share/modernime/pinyin/modernime-hotwords.dict" \
    "usr/share/doc/modernime/SOURCES.json" \
    "usr/share/doc/modernime/THIRD_PARTY_NOTICES.md" \
    "usr/share/applications/modernime-settings.desktop" \
    "usr/share/fcitx5/addon/modernime.conf" \
    "usr/share/fcitx5/addon/modernime-ui.conf" \
    "usr/share/fcitx5/inputmethod/modernime.conf" \
    "usr/share/doc/modernime/LICENSE" \
    "usr/share/doc/modernime/copyright" \
    "usr/share/modernime/install-channel.txt"; do
    [ -f "${rpm_extract_dir}/rootfs/${rpm_req_file}" ] ||
        fail "RPM payload missing file: ${rpm_req_file}"
done
grep -Fxq "channel=rpm" "${rpm_extract_dir}/rootfs/usr/share/modernime/install-channel.txt" ||
    fail "RPM install-channel.txt does not record channel=rpm"

printf '=== [3/6] Verifying signed APT repository build, metadata, and GPG signature ===\n'
apt_repo_dir="${work_dir}/apt-repo"
"${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${apt_repo_dir}" \
    --deb "${deb_file}" \
    --arch amd64 \
    --prev-version 0.1.7 \
    --ephemeral-gpg

for suite in jammy noble; do
    [ -f "${apt_repo_dir}/dists/${suite}/main/binary-amd64/Packages" ] ||
        fail "APT Packages missing for ${suite}"
    [ -f "${apt_repo_dir}/dists/${suite}/main/binary-amd64/Packages.gz" ] ||
        fail "APT Packages.gz missing for ${suite}"
    [ -f "${apt_repo_dir}/dists/${suite}/InRelease" ] ||
        fail "APT InRelease missing for ${suite}"
    [ -f "${apt_repo_dir}/dists/${suite}/Release.gpg" ] ||
        fail "APT Release.gpg missing for ${suite}"
    gpgv --keyring "${apt_repo_dir}/modernime-archive-keyring.gpg" \
        "${apt_repo_dir}/dists/${suite}/InRelease" >/dev/null 2>&1 ||
        fail "gpgv failed on APT InRelease for ${suite}"
    gpgv --keyring "${apt_repo_dir}/modernime-archive-keyring.gpg" \
        "${apt_repo_dir}/dists/${suite}/Release.gpg" \
        "${apt_repo_dir}/dists/${suite}/Release" >/dev/null 2>&1 ||
        fail "gpgv failed on APT Release.gpg for ${suite}"
    [ -f "${apt_repo_dir}/client/modernime-${suite}.sources" ] ||
        fail "APT modernime-${suite}.sources missing"
done
[ -f "${apt_repo_dir}/client/99-modernime.pref" ] || fail "APT 99-modernime.pref missing"

printf '=== [4/6] Verifying signed RPM/DNF repository build, repodata, and GPG signature ===\n'
rpm_repo_dir="${work_dir}/rpm-repo"
"${repo_root}/scripts/build_rpm_repo.sh" \
    --output-dir "${rpm_repo_dir}" \
    --rpm "${rpm_file}" \
    --distro oe2409 \
    --arch x86_64 \
    --ephemeral-gpg

for repodata_file in \
    "repomd.xml" \
    "repomd.xml.asc" \
    "primary.xml.gz" \
    "filelists.xml.gz" \
    "other.xml.gz"; do
    [ -f "${rpm_repo_dir}/oe2409/x86_64/repodata/${repodata_file}" ] ||
        fail "RPM repodata missing ${repodata_file}"
done
[ -f "${rpm_repo_dir}/RPM-GPG-KEY-modernime" ] || fail "RPM-GPG-KEY-modernime missing"
[ -f "${rpm_repo_dir}/client/modernime-openeuler-24.09.repo" ] ||
    fail "modernime-openeuler-24.09.repo missing"

gpgv --keyring "${rpm_repo_dir}/RPM-GPG-KEY-modernime.gpg" \
    "${rpm_repo_dir}/oe2409/x86_64/repodata/repomd.xml.asc" \
    "${rpm_repo_dir}/oe2409/x86_64/repodata/repomd.xml" >/dev/null 2>&1 ||
    fail "GPG signature verification failed on repomd.xml.asc"

printf '=== [5/6] Simulating v0.1.7 -> v%s package upgrade with active SQLite WAL, LMDB, settings, and user dict ===\n' "${expected_version}"
deb_ctrl_dir="${work_dir}/deb-control"
deb_rootfs_dir="${work_dir}/deb-rootfs"
mkdir -p "${deb_ctrl_dir}" "${deb_rootfs_dir}"
dpkg-deb -e "${deb_file}" "${deb_ctrl_dir}"
dpkg-deb -x "${deb_file}" "${deb_rootfs_dir}"

sim_root="${work_dir}/sim-system"
sim_home="${sim_root}/home/testuser"
sim_config="${sim_home}/.config/modernime"
sim_data="${sim_home}/.local/share/modernime"
mkdir -p "${sim_root}/usr/share/modernime" "${sim_config}" "${sim_data}"

# Seed v0.1.7 deb channel metadata and user state (settings, user dictionary, active SQLite WAL, real LMDB)
cat > "${sim_root}/usr/share/modernime/install-channel.txt" <<'EOF'
channel=deb
version=0.1.7
prefix=/usr
EOF

cat > "${sim_config}/settings.conf" <<'EOF'
page_size=9
enable_cloud_candidates=false
custom_theme=ocean
EOF

cat > "${sim_data}/user-dictionary.txt" <<'EOF'
自定义词条	zi ding yi ci tiao	1200
开源欧拉	kai yuan ou la	980
EOF

python3 - "${sim_data}/learning.sqlite3" "${sim_data}/learning.lmdb" <<'PYEOF'
import ctypes
import sqlite3
import sys

db_path = sys.argv[1]
lmdb_path = sys.argv[2]

conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA synchronous=NORMAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
conn.execute("CREATE TABLE user_ngram (key TEXT PRIMARY KEY, phrase TEXT, freq INTEGER);")
conn.execute("INSERT INTO user_ngram VALUES ('nihao', '你好世界', 42);")
conn.commit()
# Leave conn open briefly in WAL mode while backing up via modernime-data-guard

lib = ctypes.CDLL("liblmdb.so.0")
MDB_NOSUBDIR = 0x4000
env = ctypes.c_void_p()
assert lib.mdb_env_create(ctypes.byref(env)) == 0
assert lib.mdb_env_set_mapsize(env, 1048576) == 0
assert lib.mdb_env_open(env, lmdb_path.encode(), MDB_NOSUBDIR, 0o644) == 0

class MDB_val(ctypes.Structure):
    _fields_ = [("mv_size", ctypes.c_size_t), ("mv_data", ctypes.c_void_p)]

txn = ctypes.c_void_p()
dbi = ctypes.c_uint()
assert lib.mdb_txn_begin(env, None, 0, ctypes.byref(txn)) == 0
assert lib.mdb_dbi_open(txn, None, 0, ctypes.byref(dbi)) == 0
k = b"pinyin:xiandai"
v = "现代输入法".encode("utf-8")
kv = MDB_val(len(k), ctypes.cast(ctypes.c_char_p(k), ctypes.c_void_p))
vv = MDB_val(len(v), ctypes.cast(ctypes.c_char_p(v), ctypes.c_void_p))
assert lib.mdb_put(txn, dbi, ctypes.byref(kv), ctypes.byref(vv), 0) == 0
assert lib.mdb_txn_commit(txn) == 0
lib.mdb_env_close(env)
conn.close()
PYEOF

# Run transactional pre-upgrade data backup via modernime-data-guard, then execute DEB preinst upgrade -> unpack -> postinst configure
upgrade_backup_dir="${work_dir}/pre-upgrade-backup"
"${repo_root}/scripts/modernime-data-guard" --backup \
    --data-dir "${sim_data}" \
    --config-dir "${sim_config}" \
    --backup-dir "${upgrade_backup_dir}"

MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/preinst" upgrade 0.1.7
cp -a "${deb_rootfs_dir}/usr/." "${sim_root}/usr/"
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/postinst" configure 0.1.7

grep -Fxq "channel=deb" "${sim_root}/usr/share/modernime/install-channel.txt" ||
    fail "upgraded DEB system missing channel=deb"
grep -Fxq "version=${expected_version}" "${sim_root}/usr/share/modernime/install-channel.txt" ||
    fail "upgraded DEB system missing version=${expected_version}"
grep -Fxq "custom_theme=ocean" "${sim_config}/settings.conf" ||
    fail "user settings.conf lost during package upgrade"
grep -Fq "开源欧拉" "${sim_data}/user-dictionary.txt" ||
    fail "user-dictionary.txt lost during package upgrade"
"${repo_root}/scripts/modernime-data-guard" --verify \
    --data-dir "${sim_data}" \
    --config-dir "${sim_config}"

# Also verify RPM preinst upgrade ($1=2)
cat > "${sim_root}/usr/share/modernime/install-channel.txt" <<'EOF'
channel=rpm
version=0.1.7
prefix=/usr
EOF
MODERNIME_PKG_ROOT="${sim_root}" sh "${repo_root}/packaging/rpm/preinst.sh" 2

printf '=== [6/6] Verifying explicit rejection of arch mismatch, version regression, channel collision, and missing deps ===\n'
# 6a. Channel collision rejection in DEB preinst and RPM preinst
collision_root="${work_dir}/collision-root"
mkdir -p "${collision_root}/usr/local/lib/fcitx5" "${collision_root}/usr/local/share/modernime"
printf 'fake-script-plugin\n' > "${collision_root}/usr/local/lib/fcitx5/modernime_fcitx5.so"
cat > "${collision_root}/usr/local/share/modernime/install-channel.txt" <<'EOF'
channel=script
version=0.1.8
prefix=/usr/local
EOF

if MODERNIME_PKG_ROOT="${collision_root}" sh "${deb_ctrl_dir}/preinst" install >/dev/null 2>&1; then
    fail "DEB preinst should reject unmanaged script installation in /usr/local"
fi
if MODERNIME_PKG_ROOT="${collision_root}" sh "${repo_root}/packaging/rpm/preinst.sh" 1 >/dev/null 2>&1; then
    fail "RPM preinst should reject unmanaged script installation in /usr/local"
fi

# 6b. Architecture mismatch rejection in APT repo builder
fake_arm_deb_dir="${work_dir}/fake-arm64-deb/DEBIAN"
mkdir -p "${fake_arm_deb_dir}"
cat > "${fake_arm_deb_dir}/control" <<EOF
Package: modernime
Version: ${expected_version}
Architecture: arm64
Maintainer: ModernIME Developers <noreply@modernime.org>
Description: Fake arm64 package for rejection test
EOF
fake_arm_deb="${work_dir}/modernime_${expected_version}_arm64.deb"
dpkg-deb --build "${work_dir}/fake-arm64-deb" "${fake_arm_deb}" >/dev/null

if "${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${work_dir}/apt-arch-fail" \
    --deb "${fake_arm_deb}" \
    --arch amd64 \
    --ephemeral-gpg >/dev/null 2>&1; then
    fail "build_apt_repo.sh should reject arm64 DEB when building amd64 repository"
fi

# 6c. Non-monotonic version regression rejection in APT repo builder
if "${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${work_dir}/apt-ver-fail" \
    --deb "${deb_file}" \
    --prev-version "${expected_version}" \
    --arch amd64 \
    --ephemeral-gpg >/dev/null 2>&1; then
    fail "build_apt_repo.sh should reject non-monotonic version when --prev-version >= candidate version"
fi

# 6d. Missing runtime dependency rejection in install.sh
empty_bin="${work_dir}/empty-bin"
mkdir -p "${empty_bin}"
ln -sf "$(command -v bash)" "${empty_bin}/bash"
ln -sf "$(command -v sh)" "${empty_bin}/sh"
ln -sf "$(command -v uname)" "${empty_bin}/uname"
ln -sf "$(command -v dirname)" "${empty_bin}/dirname"
ln -sf "$(command -v basename)" "${empty_bin}/basename"
ln -sf "$(command -v mkdir)" "${empty_bin}/mkdir"
ln -sf "$(command -v rm)" "${empty_bin}/rm"

if PATH="${empty_bin}" MODERNIME_SKIP_FCITX_RESTART=1 \
    bash "${repo_root}/install.sh" "${work_dir}/missing-dep-prefix" >/dev/null 2>&1; then
    fail "install.sh should reject installation when fcitx5 runtime dependency is missing"
fi

printf 'PASS: All 6 package & repository release gate checks succeeded.\n'
