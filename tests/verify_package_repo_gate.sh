#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "${script_dir}/.." && pwd)

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-pkg-repo-gate.XXXXXX")
cleanup() {
    if [ -d "${work_dir}/gpg-setup" ]; then
        gpgconf --homedir "${work_dir}/gpg-setup" --kill gpg-agent >/dev/null 2>&1 || true
    fi
    rm -rf -- "${work_dir}"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

# Assert that a command exits with a normal non-zero exit code (1 <= rc < 124).
# Timeouts (124), SIGKILL (137), command-not-found (126/127), or any signal/abnormal termination (rc >= 124)
# MUST NOT be counted as a valid expected failure.
assert_normal_nonzero_failure() {
    local desc="$1"
    shift
    local rc=0
    "$@" >/dev/null 2>&1 || rc=$?
    if [ "${rc}" -eq 0 ]; then
        fail "${desc} (expected normal non-zero failure, but command succeeded with exit code 0)"
    fi
    if [ "${rc}" -ge 124 ]; then
        fail "${desc} (expected normal non-zero failure 1..123, but got abnormal/timeout/signal exit code ${rc})"
    fi
}

expected_version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "${repo_root}/CMakeLists.txt")
[ -n "${expected_version}" ] || fail "unable to parse version from CMakeLists.txt"

printf '=== [1/7] Building / verifying DEB and RPM packages (v%s) ===\n' "${expected_version}"
# Validate the current source, including CMake install rules and helper scripts.
# A few selected timestamp checks can silently reuse an obsolete package.
"${repo_root}/scripts/package_deb.sh"
deb_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.deb" | head -n 1)
[ -f "${deb_file}" ] || fail "DEB package not produced in dist/"

"${repo_root}/scripts/package_rpm.sh"
rpm_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.rpm" | head -n 1)
[ -f "${rpm_file}" ] || fail "RPM package not produced in dist/"

assert_normal_nonzero_failure \
    "package_rpm.sh must reject fallback packaging when MODERNIME_REQUIRE_NATIVE_RPM_TOOLS=1" \
    env MODERNIME_REQUIRE_NATIVE_RPM_TOOLS=1 MODERNIME_FORCE_FALLBACK_RPM=1 MODERNIME_RPM_DIST_DIR="${work_dir}/rejected-rpm-dist" \
    "${repo_root}/scripts/package_rpm.sh"

printf '=== [2/7] Verifying DEB & RPM file boundaries, dependencies, licenses, and zero user data ===\n'
deb_info=$(dpkg-deb -I "${deb_file}")
deb_contents=$(dpkg-deb -c "${deb_file}")

for required_path in \
    "modernime_fcitx5.so" \
    "modernime_ui.so" \
    "./usr/bin/modernime-settings" \
    "./usr/bin/modernime-session-init" \
    "./usr/share/modernime/install_state.sh" \
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
    "./usr/share/modernime/install-channel.txt" \
    "./usr/share/modernime/extensions/kimpanel@kde.org/metadata.json"; do
    printf '%s\n' "${deb_contents}" | grep -Fq "${required_path}" ||
        fail "DEB missing required file: ${required_path}"
done

if printf '%s\n' "${deb_contents}" | grep -E '\.mimosa|\.v2c'; then
    fail "DEB contains forbidden uncommitted directories (.mimosa or .v2c)"
fi

for icon_size in 16 32 48 64 128 256 512; do
    icon_path="./usr/share/icons/hicolor/${icon_size}x${icon_size}/apps/modernime.png"
    printf '%s\n' "${deb_contents}" | grep -Fq "${icon_path}" ||
        fail "DEB missing application icon: ${icon_path}"
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

rpm_extract_dir="${work_dir}/rpm-extracted"
mkdir -p "${rpm_extract_dir}"
python3 - "${rpm_file}" "${rpm_extract_dir}" "${expected_version}" <<'PYEOF'
import gzip
import hashlib
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
    dtypes = {}
    for i in range(nindex):
        eo = idx_start + i * 16
        tag, dtype, voff, count = struct.unpack(">IIII", data[eo:eo+16])
        pos = store_start + voff
        dtypes[tag] = dtype
        if dtype in (6, 9):
            end = data.index(b"\x00", pos)
            tags[tag] = data[pos:end].decode("utf-8", errors="replace")
        elif dtype == 7:
            tags[tag] = data[pos:pos+count]
        elif dtype == 8:
            arr = []
            cur = pos
            for _ in range(count):
                end = data.index(b"\x00", cur)
                arr.append(data[cur:end].decode("utf-8", errors="replace"))
                cur = end + 1
            tags[tag] = arr
        elif dtype == 3:
            vals = list(struct.unpack(f">{count}H", data[pos:pos+2*count]))
            tags[tag] = vals if count > 1 else vals[0]
        elif dtype == 4:
            vals = list(struct.unpack(f">{count}I", data[pos:pos+4*count]))
            tags[tag] = vals if count > 1 else vals[0]
    return tags, dtypes, store_end

_, _, sig_end = parse_hdr(raw, 96)
hdr_off = (sig_end + 7) & ~7
if raw[hdr_off:hdr_off+4] != b"\x8e\xad\xe8\x01":
    hdr_off = raw.find(b"\x8e\xad\xe8\x01", sig_end)
tags, dtypes, payload_off = parse_hdr(raw, hdr_off)

assert tags.get(1000) == "modernime", f"Unexpected RPM name: {tags.get(1000)}"
assert tags.get(1001) == expected_ver, f"Unexpected RPM version: {tags.get(1001)}"
valid_licenses = (
    "LGPL-2.1-or-later AND MIT",
    "Apache-2.0 AND LGPL-2.1-or-later AND CC-BY-SA-3.0 AND BSD-3-Clause AND MIT",
)
assert tags.get(1014) in valid_licenses, f"Unexpected RPM license: {tags.get(1014)}"

req_names = tags.get(1049, [])
req_vers = tags.get(1050, [])
req_map = dict(zip(req_names, req_vers))
for req_pkg, min_ver in [
    ("fcitx5", "5.0.14"),
    ("libime", "1.0.11"),
    ("gtk3", "3.24.0"),
    ("sqlite-libs", "3.37.0"),
    ("lmdb-libs", "0.9.24"),
    ("python3", "3.10"),
]:
    assert req_map.get(req_pkg) == min_ver, f"RPM missing dependency {req_pkg} >= {min_ver} (got {req_map.get(req_pkg)})"

if 1023 in tags:
    (extract_dir / "prein.sh").write_text(tags[1023], encoding="utf-8")
if 1024 in tags:
    (extract_dir / "postin.sh").write_text(tags[1024], encoding="utf-8")
if 1026 in tags:
    (extract_dir / "postun.sh").write_text(tags[1026], encoding="utf-8")

rootfs = extract_dir / "rootfs"
rootfs.mkdir(parents=True, exist_ok=True)
cpio_bytes = gzip.decompress(raw[payload_off:])
subprocess.run(
    ["cpio", "-id", "--quiet", "--no-absolute-filenames"],
    cwd=str(rootfs),
    input=cpio_bytes,
    check=True,
)

# Verify complete rpmfi per-file header metadata required by native rpm -qpl, rpmkeys, and dnf
def as_list(val):
    if val is None:
        return []
    return val if isinstance(val, list) else [val]

dirindexes = as_list(tags.get(1116))
basenames = as_list(tags.get(1117))
dirnames = as_list(tags.get(1118))
filesizes = as_list(tags.get(1028))
filemodes = as_list(tags.get(1030))
filerdevs = as_list(tags.get(1033))
filemtimes = as_list(tags.get(1034))
filedigests = as_list(tags.get(1035))
filelinktos = as_list(tags.get(1036))
fileflags = as_list(tags.get(1037))
fileusernames = as_list(tags.get(1039))
filegroupnames = as_list(tags.get(1040))
filedevices = as_list(tags.get(1095))
fileinodes = as_list(tags.get(1096))
filelangs = as_list(tags.get(1097))

nfiles = len(basenames)
assert nfiles >= 18, f"RPM header BASENAMES has too few entries ({nfiles})"
assert dtypes.get(1030) == 3, f"RPMTAG_FILEMODES (1030) must be INT16 (dtype=3), got dtype={dtypes.get(1030)}"
assert dtypes.get(1033) == 3, f"RPMTAG_FILERDEVS (1033) must be INT16 (dtype=3), got dtype={dtypes.get(1033)}"
for tag_name, arr in [
    ("DIRINDEXES(1116)", dirindexes),
    ("FILESIZES(1028)", filesizes),
    ("FILEMODES(1030)", filemodes),
    ("FILERDEVS(1033)", filerdevs),
    ("FILEMTIMES(1034)", filemtimes),
    ("FILEDIGESTS(1035)", filedigests),
    ("FILELINKTOS(1036)", filelinktos),
    ("FILEFLAGS(1037)", fileflags),
    ("FILEUSERNAME(1039)", fileusernames),
    ("FILEGROUPNAME(1040)", filegroupnames),
    ("FILEDEVICES(1095)", filedevices),
    ("FILEINODES(1096)", fileinodes),
    ("FILELANGS(1097)", filelangs),
]:
    assert len(arr) == nfiles, f"RPM header tag {tag_name} length {len(arr)} != BASENAMES length {nfiles}"

assert len(set(fileinodes)) == nfiles, "RPMTAG_FILEINODES (1096) must be unique per entry to avoid bogus hardlink grouping"
assert tags.get(5011) == 8, f"RPMTAG_FILEDIGESTALGO (5011) must be 8 (SHA256), got {tags.get(5011)}"

hdr_reg_files = {}
for i in range(nfiles):
    d_idx = dirindexes[i]
    assert 0 <= d_idx < len(dirnames), f"Invalid DIRINDEXES[{i}]={d_idx}"
    full_path = f"{dirnames[d_idx]}{basenames[i]}"
    mode = filemodes[i]
    assert fileusernames[i] == "root", f"Unexpected FILEUSERNAME for {full_path}: {fileusernames[i]}"
    assert filegroupnames[i] == "root", f"Unexpected FILEGROUPNAME for {full_path}: {filegroupnames[i]}"
    if (mode & 0o170000) == 0o100000:
        hdr_reg_files[full_path] = {
            "size": filesizes[i],
            "mode": mode,
            "digest": filedigests[i],
        }

payload_reg_files = sorted(p for p in rootfs.rglob("*") if p.is_file() and not p.is_symlink())
icon_paths = [rootfs / f"usr/share/icons/hicolor/{size}x{size}/apps/modernime.png"
              for size in (16, 32, 48, 64, 128, 256, 512)]
for icon in icon_paths:
    assert icon.is_file(), f"RPM missing application icon: {icon}"
    assert icon.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"), f"Invalid PNG icon: {icon}"
ext_paths = sorted(p for p in (rootfs / "usr/share/modernime/extensions/kimpanel@kde.org").rglob("*") if p.is_file() and not p.is_symlink())
assert (rootfs / "usr/share/modernime/extensions/kimpanel@kde.org/metadata.json").is_file(), "RPM missing kimpanel metadata.json"
assert len(ext_paths) == 20, f"Expected exactly 20 kimpanel extension files in RPM, got {len(ext_paths)}: {[p.name for p in ext_paths]}"
for p in ext_paths:
    assert ".mimosa" not in p.parts and ".v2c" not in p.parts, f"RPM payload contains dirty files: {p}"
expected_regular_files = 20 + len(icon_paths) + len(ext_paths)
assert len(payload_reg_files) == expected_regular_files, (
    f"Expected {expected_regular_files} regular files in RPM payload, got {len(payload_reg_files)}"
)
assert len(hdr_reg_files) == len(payload_reg_files), (
    f"RPM header regular file count ({len(hdr_reg_files)}) != payload regular file count ({len(payload_reg_files)})"
)

for pf in payload_reg_files:
    rel_path = "/" + pf.relative_to(rootfs).as_posix()
    assert rel_path in hdr_reg_files, f"Payload file {rel_path} missing from RPM header file table"
    meta = hdr_reg_files[rel_path]
    actual_bytes = pf.read_bytes()
    actual_sha = hashlib.sha256(actual_bytes).hexdigest()
    assert meta["size"] == len(actual_bytes), (
        f"RPM FILESIZES mismatch for {rel_path}: header={meta['size']}, payload={len(actual_bytes)}"
    )
    assert meta["digest"] == actual_sha, (
        f"RPM FILEDIGESTS mismatch for {rel_path}: header={meta['digest']}, payload={actual_sha}"
    )
    perm = meta["mode"] & 0o777
    if rel_path.startswith("/usr/bin/") or rel_path.endswith(".so"):
        assert perm == 0o755, f"Expected 0755 mode for executable/shared library {rel_path}, got {oct(perm)}"
    else:
        assert perm == 0o644, f"Expected 0644 mode for regular file {rel_path}, got {oct(perm)}"
PYEOF

if command -v rpm >/dev/null 2>&1; then
    rpm_qpl_out=$(rpm -qpl "${rpm_file}")
    if printf '%s\n' "${rpm_qpl_out}" | grep -Fq "(contains no files)"; then
        fail "native rpm -qpl reported '(contains no files)' for ${rpm_file}"
    fi
    for req_rpm_path in \
        "/usr/bin/modernime-settings" \
        "/usr/bin/modernime-session-init" \
        "/usr/share/modernime/install_state.sh" \
        "/usr/bin/modernime-data-guard" \
        "/usr/bin/modernime-migrate-channel" \
        "/usr/share/modernime/install-channel.txt" \
        "/usr/share/modernime/pinyin/modernime-knowledge.dict" \
        "/usr/share/modernime/pinyin/modernime-hotwords.dict" \
        "/usr/share/modernime/pinyin/modernime-english-dict.bin" \
        "/usr/share/applications/modernime-settings.desktop"; do
        printf '%s\n' "${rpm_qpl_out}" | grep -Fxq "${req_rpm_path}" ||
            fail "native rpm -qpl missing expected file: ${req_rpm_path}"
    done
    printf '  -> [NATIVE RPM]: rpm -qpl verified (%d entries)\n' "$(printf '%s\n' "${rpm_qpl_out}" | wc -l)"
else
    printf '  -> [SIMULATED HARNESS / PYTHON RPMFI VERIFIER]: native rpm binary not installed on host; verified every RPM header file-table entry (FILESIZES, FILEMODES, FILEDIGESTS, FILEUSERNAME/GROUPNAME, DIRNAMES/DIRINDEXES/BASENAMES)\n'
fi

for rpm_req_file in \
    "usr/bin/modernime-settings" \
    "usr/bin/modernime-session-init" \
    "usr/share/modernime/install_state.sh" \
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

printf '=== [3/7] Verifying CI release repository builder, persistent GPG key rotation, and multi-version Pages tree ===\n'
ci_assets_dir="${work_dir}/ci-release-assets"
prev_pages_dir="${work_dir}/prev-pages"
pages_site_dir="${work_dir}/pages-site"
mkdir -p "${ci_assets_dir}" \
    "${prev_pages_dir}/apt/pool/noble/main/m/modernime" \
    "${prev_pages_dir}/rpm/oe2409/x86_64/Packages"

# Copy current DEB/RPM using the exact filenames produced by .github/workflows/release.yml
cp -f "${deb_file}" "${ci_assets_dir}/modernime_${expected_version}_amd64_ubuntu24.04.deb"
cp -f "${rpm_file}" "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm"

# Build synthetic v0.1.8 DEB and v0.1.8 RPM in prev_pages_dir to test multi-version retention & downgrade/rollback
deb_018_stage="${work_dir}/deb-0.1.8-stage"
mkdir -p "${deb_018_stage}"
dpkg-deb -R "${deb_file}" "${deb_018_stage}"
sed -i "s/^Version: .*/Version: 0.1.8/" "${deb_018_stage}/DEBIAN/control"
cat > "${deb_018_stage}/usr/share/modernime/install-channel.txt" <<'EOF'
channel=deb
version=0.1.8
prefix=/usr
EOF
chmod 0755 "${deb_018_stage}/DEBIAN/"* 2>/dev/null || true
chmod 0644 "${deb_018_stage}/DEBIAN/control" 2>/dev/null || true
deb_018_file="${work_dir}/modernime_0.1.8_amd64.deb"
dpkg-deb --build "${deb_018_stage}" "${deb_018_file}" >/dev/null
cp -f "${deb_018_file}" "${prev_pages_dir}/apt/pool/noble/main/m/modernime/modernime_0.1.8_amd64_ubuntu24.04.deb"

rpm_018_file="${prev_pages_dir}/rpm/oe2409/x86_64/Packages/modernime-0.1.8-1.oe24.x86_64.rpm"
python3 - "${rpm_file}" "${rpm_018_file}" "${expected_version}" <<'PYEOF'
import gzip
import hashlib
from pathlib import Path
import struct
import sys

raw = Path(sys.argv[1]).read_bytes()
dst_path = Path(sys.argv[2])
cur_ver = sys.argv[3]
assert len(cur_ver) >= len("0.1.8"), "Current version cannot be shorter than the fixture"
# Leave header offsets and CPIO file sizes intact when patch numbers gain digits.
# RPM strings may have spare NUL bytes; channel metadata may have blank lines.
padding = len(cur_ver) - len("0.1.8")
lead_name = raw[10:76].rstrip(b"\x00").replace(cur_ver.encode("ascii"), b"0.1.8")
lead = raw[:10] + lead_name.ljust(66, b"\x00") + raw[76:96]
nindex, hsize = struct.unpack(">II", raw[104:112])
sig_end = (112 + nindex * 16 + hsize + 7) & ~7
if raw[sig_end : sig_end + 4] != b"\x8e\xad\xe8\x01":
    sig_end = raw.find(b"\x8e\xad\xe8\x01", 112 + nindex * 16 + hsize)
m_nindex, m_hsize = struct.unpack(">II", raw[sig_end + 8 : sig_end + 16])
payload_off = sig_end + 16 + m_nindex * 16 + m_hsize

main_hdr = raw[sig_end:payload_off]
old_payload_gz = raw[payload_off:]
old_cpio = gzip.decompress(old_payload_gz)

old_chan = f"channel=rpm\nversion={cur_ver}\nprefix=/usr\n".encode("ascii")
new_chan = b"channel=rpm\nversion=0.1.8\nprefix=/usr\n" + b"\n" * padding
new_cpio = old_cpio.replace(old_chan, new_chan)
new_payload_gz = gzip.compress(new_cpio, compresslevel=6)

old_chan_sha = hashlib.sha256(old_chan).hexdigest().encode("ascii")
new_chan_sha = hashlib.sha256(new_chan).hexdigest().encode("ascii")
old_gz_sha = hashlib.sha256(old_payload_gz).hexdigest().encode("ascii")
new_gz_sha = hashlib.sha256(new_payload_gz).hexdigest().encode("ascii")
old_cpio_sha = hashlib.sha256(old_cpio).hexdigest().encode("ascii")
new_cpio_sha = hashlib.sha256(new_cpio).hexdigest().encode("ascii")

new_main_hdr = (
    main_hdr.replace(old_chan_sha, new_chan_sha)
    .replace(old_gz_sha, new_gz_sha)
    .replace(old_cpio_sha, new_cpio_sha)
    .replace(cur_ver.encode("ascii") + b"\x00", b"0.1.8" + b"\x00" * (padding + 1))
)
dst_path.write_bytes(lead + raw[96:sig_end] + new_main_hdr + new_payload_gz)
PYEOF

# Gate check: --require-persistent-gpg MUST fail when no persistent key is provided
assert_normal_nonzero_failure \
    "ci_build_release_repos.sh --require-persistent-gpg must fail when persistent GPG key is absent" \
    env -u MODERNIME_GPG_PRIVATE_KEY -u MODERNIME_GPG_KEY_ID \
    "${repo_root}/scripts/ci_build_release_repos.sh" \
    --assets-dir "${ci_assets_dir}" \
    --pages-dir "${work_dir}/pages-should-fail" \
    --require-persistent-gpg

# Generate persistent primary GPG key (passphrase-protected) AND rotation GPG key to verify key persistence + dual-key rotation
gpg_setup_home="${work_dir}/gpg-setup"
mkdir -p "${gpg_setup_home}"
chmod 700 "${gpg_setup_home}"
passphrase="PassphraseProtectedKey2026!"
gpg --homedir "${gpg_setup_home}" --batch --pinentry-mode loopback --passphrase "${passphrase}" --generate-key <<GPGEOF >/dev/null 2>&1
Key-Type: RSA
Key-Length: 2048
Passphrase: ${passphrase}
Name-Real: ModernIME Release Signing
Name-Email: release@modernime.org
Expire-Date: 0
%commit
GPGEOF
gpg --homedir "${gpg_setup_home}" --batch --generate-key <<'GPGEOF' >/dev/null 2>&1
%no-protection
Key-Type: RSA
Key-Length: 2048
Name-Real: ModernIME Next Rotation Key
Name-Email: rotation@modernime.org
Expire-Date: 0
%commit
GPGEOF

primary_fpr=$(gpg --homedir "${gpg_setup_home}" --list-secret-keys --with-colons "release@modernime.org" | awk -F: '$1 == "fpr" { print $10; exit }')
rotation_fpr=$(gpg --homedir "${gpg_setup_home}" --list-secret-keys --with-colons "rotation@modernime.org" | awk -F: '$1 == "fpr" { print $10; exit }')
[ -n "${primary_fpr}" ] || fail "failed to generate primary GPG key"
[ -n "${rotation_fpr}" ] || fail "failed to generate rotation GPG key"

primary_sec_file="${work_dir}/primary-secret.asc"
rotation_sec_value=$(gpg --homedir "${gpg_setup_home}" --armor --export-secret-keys "${rotation_fpr}")
gpg --homedir "${gpg_setup_home}" --armor --pinentry-mode loopback --passphrase "${passphrase}" --export-secret-keys "${primary_fpr}" > "${primary_sec_file}"

primary_only_keyring="${work_dir}/primary-only.gpg"
rotation_only_keyring="${work_dir}/rotation-only.gpg"
gpg --homedir "${gpg_setup_home}" --export "${primary_fpr}" > "${primary_only_keyring}"
gpg --homedir "${gpg_setup_home}" --export "${rotation_fpr}" > "${rotation_only_keyring}"

# Gate check: passphrase-protected key without MODERNIME_GPG_PASSPHRASE MUST fail with normal non-zero exit code
assert_normal_nonzero_failure \
    "ci_build_release_repos.sh must fail when passphrase-protected key is missing MODERNIME_GPG_PASSPHRASE" \
    timeout 10s env -u MODERNIME_GPG_PASSPHRASE \
    "${repo_root}/scripts/ci_build_release_repos.sh" \
    --assets-dir "${ci_assets_dir}" \
    --pages-dir "${work_dir}/pages-should-fail-passphrase" \
    --gpg-private-key-file "${primary_sec_file}" \
    --require-persistent-gpg

# 3a. Inaugural release deployment check (empty gh-pages baseline: v0.1.9 only, no --previous-pages-dir)
inaugural_pages_dir="${work_dir}/pages-inaugural"
MODERNIME_GPG_KEY_ID="${primary_fpr}" \
MODERNIME_GPG_PASSPHRASE="${passphrase}" \
MODERNIME_GPG_ROTATION_PRIVATE_KEY="${rotation_sec_value}" \
MODERNIME_GPG_ROTATION_KEY_ID="${rotation_fpr}" \
"${repo_root}/scripts/ci_build_release_repos.sh" \
    --assets-dir "${ci_assets_dir}" \
    --pages-dir "${inaugural_pages_dir}" \
    --gpg-private-key-file "${primary_sec_file}" \
    --require-persistent-gpg \
    --repo-base-url "file://${inaugural_pages_dir}"

grep -Fxq "Version: ${expected_version}" "${inaugural_pages_dir}/apt/dists/noble/main/binary-amd64/Packages" ||
    fail "Inaugural APT repository missing Version: ${expected_version}"
if grep -Fxq "Version: 0.1.8" "${inaugural_pages_dir}/apt/dists/noble/main/binary-amd64/Packages"; then
    fail "Inaugural APT repository without --previous-pages-dir should only contain ${expected_version}"
fi
[ -f "${inaugural_pages_dir}/rpm/oe2409/x86_64/Packages/modernime-${expected_version}-1.oe2409.x86_64.rpm" ] ||
    fail "Inaugural RPM repository missing modernime-${expected_version}-1.oe2409.x86_64.rpm"
[ -f "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm" ] ||
    fail "Canonical RPM filename missing from ci_assets_dir after sync"
[ "$(find "${ci_assets_dir}" -maxdepth 1 -type f -name '*.rpm' | wc -l)" -eq 1 ] ||
    fail "ci_assets_dir should contain exactly 1 canonical RPM file"
cmp -s "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm" \
    "${inaugural_pages_dir}/rpm/oe2409/x86_64/Packages/modernime-${expected_version}-1.oe2409.x86_64.rpm" ||
    fail "Release asset RPM is not byte-identical to inaugural Pages RPM"
printf '==> [Gate 3a] Inaugural release repository baseline & synced release-assets RPM (v%s): VERIFIED\n' "${expected_version}"

# Gate check: if an input RPM was signed by an ephemeral test key, production build MUST overwrite it with the production key,
# and --require-persistent-gpg MUST reject an ephemeral signing key itself.
ephemeral_repo_dir="${work_dir}/ephemeral-rpm-repo"
"${repo_root}/scripts/build_rpm_repo.sh" \
    --output-dir "${ephemeral_repo_dir}" \
    --rpm "${rpm_file}" \
    --distro oe2409 \
    --arch x86_64 \
    --ephemeral-gpg >/dev/null
# Seed ci_assets_dir with the ephemeral-signed RPM to verify that ci_build_release_repos.sh replaces the ephemeral signature
cp -f "${ephemeral_repo_dir}/oe2409/x86_64/Packages/modernime-${expected_version}-1.oe2409.x86_64.rpm" \
    "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm"

# Include the current version in prior Pages to exercise release retry idempotency.
for suite in noble; do
    cp -f "${inaugural_pages_dir}/apt/pool/${suite}/main/m/modernime/modernime_${expected_version}_amd64.deb" \
        "${prev_pages_dir}/apt/pool/${suite}/main/m/modernime/"
done
cp -f "${inaugural_pages_dir}/rpm/oe2409/x86_64/Packages/modernime-${expected_version}-1.oe2409.x86_64.rpm" \
    "${prev_pages_dir}/rpm/oe2409/x86_64/Packages/"

# 3b. Subsequent release multi-version retention check (simulated prior release pool in gh-pages + current release)
MODERNIME_GPG_KEY_ID="${primary_fpr}" \
MODERNIME_GPG_PASSPHRASE="${passphrase}" \
MODERNIME_GPG_ROTATION_PRIVATE_KEY="${rotation_sec_value}" \
MODERNIME_GPG_ROTATION_KEY_ID="${rotation_fpr}" \
"${repo_root}/scripts/ci_build_release_repos.sh" \
    --assets-dir "${ci_assets_dir}" \
    --pages-dir "${pages_site_dir}" \
    --previous-pages-dir "${prev_pages_dir}" \
    --gpg-private-key-file "${primary_sec_file}" \
    --require-persistent-gpg \
    --repo-base-url "file://${pages_site_dir}"
cmp -s "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm" \
    "${pages_site_dir}/rpm/oe2409/x86_64/Packages/modernime-${expected_version}-1.oe2409.x86_64.rpm" ||
    fail "Release asset RPM is not byte-identical to Pages RPM in Gate 3b"
if cmp -s "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm" \
    "${ephemeral_repo_dir}/oe2409/x86_64/Packages/modernime-${expected_version}-1.oe2409.x86_64.rpm"; then
    fail "Release asset RPM retained ephemeral test key signature instead of being re-signed with production key"
fi
printf '==> [Gate 3b] Subsequent release multi-version retention & production RPM sync (--previous-pages-dir): VERIFIED\n'

[ -f "${pages_site_dir}/.nojekyll" ] || fail "pages-site/.nojekyll missing"
[ -f "${pages_site_dir}/apt/KEY-FINGERPRINTS.txt" ] || fail "apt/KEY-FINGERPRINTS.txt missing"
[ -f "${pages_site_dir}/rpm/KEY-FINGERPRINTS.txt" ] || fail "rpm/KEY-FINGERPRINTS.txt missing"
grep -Fq "${primary_fpr}" "${pages_site_dir}/apt/KEY-FINGERPRINTS.txt" || fail "primary fingerprint missing from KEY-FINGERPRINTS.txt"
grep -Fq "${rotation_fpr}" "${pages_site_dir}/apt/KEY-FINGERPRINTS.txt" || fail "rotation fingerprint missing from KEY-FINGERPRINTS.txt"

for suite in noble; do
    [ -f "${pages_site_dir}/apt/dists/${suite}/main/binary-amd64/Packages" ] ||
        fail "APT Packages missing for ${suite}"
    [ -f "${pages_site_dir}/apt/dists/${suite}/main/binary-amd64/Packages.gz" ] ||
        fail "APT Packages.gz missing for ${suite}"
    [ -f "${pages_site_dir}/apt/dists/${suite}/InRelease" ] ||
        fail "APT InRelease missing for ${suite}"
    [ -f "${pages_site_dir}/apt/dists/${suite}/Release.gpg" ] ||
        fail "APT Release.gpg missing for ${suite}"
    # Verify InRelease contains valid signatures for both primary and rotation keys
    inrelease_status=$(gpgv --keyring "${pages_site_dir}/apt/modernime-archive-keyring.gpg" \
        --status-fd 1 "${pages_site_dir}/apt/dists/${suite}/InRelease" 2>&1)
    printf '%s\n' "${inrelease_status}" | grep -Fq "VALIDSIG ${primary_fpr}" ||
        fail "InRelease missing valid signature for primary key ${primary_fpr} on ${suite}"
    printf '%s\n' "${inrelease_status}" | grep -Fq "VALIDSIG ${rotation_fpr}" ||
        fail "InRelease missing valid signature for rotation key ${rotation_fpr} on ${suite}"

    gpgv --keyring "${pages_site_dir}/apt/modernime-archive-keyring.gpg" \
        "${pages_site_dir}/apt/dists/${suite}/Release.gpg" \
        "${pages_site_dir}/apt/dists/${suite}/Release" >/dev/null 2>&1 ||
        fail "gpgv failed on APT Release.gpg for ${suite}"
    [ -f "${pages_site_dir}/apt/client/modernime-${suite}.sources" ] ||
        fail "APT modernime-${suite}.sources missing"
done

printf '=== [4/7] Running REAL apt-get update & apt-cache policy against signed local APT repository (noble) ===\n'
for suite in noble; do
    apt_root="${work_dir}/apt-client-${suite}"
    mkdir -p \
        "${apt_root}/etc/apt/sources.list.d" \
        "${apt_root}/etc/apt/preferences.d" \
        "${apt_root}/etc/apt/apt.conf.d" \
        "${apt_root}/var/lib/apt/lists/partial" \
        "${apt_root}/var/cache/apt/archives/partial" \
        "${apt_root}/var/lib/dpkg"

    cat > "${apt_root}/var/lib/dpkg/status" <<'EOF'
Package: modernime
Status: install ok installed
Priority: optional
Section: utils
Installed-Size: 16000
Maintainer: ModernIME Developers <noreply@modernime.org>
Architecture: amd64
Version: 0.1.8
Description: Modern Chinese Pinyin Input Method for Fcitx5
EOF

    cp -f "${pages_site_dir}/apt/client/modernime-${suite}.sources" \
        "${apt_root}/etc/apt/sources.list.d/modernime.sources"
    sed -i "s|^Signed-By:.*|Signed-By: ${pages_site_dir}/apt/modernime-archive-keyring.gpg|" \
        "${apt_root}/etc/apt/sources.list.d/modernime.sources"
    cp -f "${pages_site_dir}/apt/client/99-modernime.pref" \
        "${apt_root}/etc/apt/preferences.d/99-modernime.pref"

    apt_opts=(
        -o "Dir=${apt_root}"
        -o "Dir::State::status=${apt_root}/var/lib/dpkg/status"
        -o "APT::Architecture=amd64"
        -o "APT::Get::AllowUnauthenticated=false"
        -o "Acquire::AllowInsecureRepositories=false"
    )

    LC_ALL=C LANG=C apt-get "${apt_opts[@]}" update >/dev/null ||
        fail "real apt-get update failed on signed APT repository for suite ${suite}"

    policy_out=$(LC_ALL=C LANG=C apt-cache "${apt_opts[@]}" policy modernime)
    printf '%s\n' "${policy_out}" | grep -Fq "Installed: 0.1.8" ||
        fail "apt-cache policy did not report Installed: 0.1.8 for ${suite}"
    printf '%s\n' "${policy_out}" | grep -Fq "Candidate: ${expected_version}" ||
        fail "apt-cache policy did not resolve Candidate: ${expected_version} for ${suite}"
    printf '%s\n' "${policy_out}" | grep -Fq "${expected_version} 700" ||
        fail "apt-cache policy did not apply Pin-Priority 700 to ${expected_version} from 99-modernime.pref for ${suite}"
    printf '%s\n' "${policy_out}" | grep -Fq "file:${pages_site_dir}/apt ${suite}/main amd64 Packages" ||
        fail "apt-cache policy missing expected repository source URL for ${suite}"

    madison_out=$(LC_ALL=C LANG=C apt-cache "${apt_opts[@]}" madison modernime)
    printf '%s\n' "${madison_out}" | grep -Fq "${expected_version}" ||
        fail "apt-cache madison missing ${expected_version} for ${suite}"
    printf '%s\n' "${madison_out}" | grep -Fq "0.1.8" ||
        fail "apt-cache madison missing retained rollback version 0.1.8 for ${suite}"
done

printf '=== [5/7] Verifying signed RPM/DNF repository repomd.xml.asc AND embedded RPM package OpenPGP signatures ===\n'
for repodata_file in \
    "repomd.xml" \
    "repomd.xml.asc" \
    "primary.xml.gz" \
    "filelists.xml.gz" \
    "other.xml.gz"; do
    [ -f "${pages_site_dir}/rpm/oe2409/x86_64/repodata/${repodata_file}" ] ||
        fail "RPM repodata missing ${repodata_file}"
done
[ -f "${pages_site_dir}/rpm/RPM-GPG-KEY-modernime" ] || fail "RPM-GPG-KEY-modernime missing"
[ -f "${pages_site_dir}/rpm/client/modernime-openeuler-24.09.repo" ] ||
    fail "modernime-openeuler-24.09.repo missing"

repomd_status=$(gpgv --keyring "${pages_site_dir}/rpm/RPM-GPG-KEY-modernime.gpg" \
    --status-fd 1 \
    "${pages_site_dir}/rpm/oe2409/x86_64/repodata/repomd.xml.asc" \
    "${pages_site_dir}/rpm/oe2409/x86_64/repodata/repomd.xml" 2>&1)
printf '%s\n' "${repomd_status}" | grep -Fq "VALIDSIG ${primary_fpr}" ||
    fail "repomd.xml.asc missing valid signature for primary key ${primary_fpr}"

# Verify every RPM in Packages/ has valid embedded RPMSIGTAG_RSA (268) and RPMSIGTAG_PGP (259) OpenPGP signatures
# AND matches the sha256 checksum & size recorded in primary.xml.gz
python3 - "${pages_site_dir}/rpm/oe2409/x86_64" "${pages_site_dir}/rpm/RPM-GPG-KEY-modernime.gpg" "${expected_version}" <<'PYEOF'
import gzip
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import sys

arch_dir = Path(sys.argv[1]).resolve()
keyring = Path(sys.argv[2]).resolve()
expected_ver = sys.argv[3]

primary_xml = gzip.decompress((arch_dir / "repodata/primary.xml.gz").read_bytes())
root = ET.fromstring(primary_xml)
ns = {"common": "http://linux.duke.edu/metadata/common"}

pkgs_in_xml = {}
for pkg in root.findall("common:package", ns):
    ver_el = pkg.find("common:version", ns)
    csum_el = pkg.find("common:checksum", ns)
    size_el = pkg.find("common:size", ns)
    loc_el = pkg.find("common:location", ns)
    ver = ver_el.attrib["ver"]
    pkgs_in_xml[ver] = {
        "sha256": csum_el.text,
        "size": int(size_el.attrib["package"]),
        "href": loc_el.attrib["href"],
    }

assert "0.1.8" in pkgs_in_xml, "primary.xml.gz missing retained version 0.1.8"
assert expected_ver in pkgs_in_xml, f"primary.xml.gz missing current version {expected_ver}"

for ver, meta in pkgs_in_xml.items():
    rpm_path = arch_dir / meta["href"]
    assert rpm_path.is_file(), f"Missing RPM file {rpm_path}"
    raw = rpm_path.read_bytes()
    assert len(raw) == meta["size"], f"RPM size mismatch for {ver}: {len(raw)} != {meta['size']}"
    actual_sha = hashlib.sha256(raw).hexdigest()
    assert actual_sha == meta["sha256"], f"RPM sha256 mismatch for {ver}: {actual_sha} != {meta['sha256']}"

    # Parse signature header and verify tags 268 (RSA header sig) and 259 (PGP header+payload sig)
    assert raw[96:100] == b"\x8e\xad\xe8\x01", "Invalid RPM signature header magic"
    nindex, hsize = struct.unpack(">II", raw[104:112])
    idx_start = 112
    store_start = idx_start + nindex * 16
    store_end = store_start + hsize
    sig_end = (store_end + 7) & ~7
    if raw[sig_end:sig_end+4] != b"\x8e\xad\xe8\x01":
        sig_end = raw.find(b"\x8e\xad\xe8\x01", store_end)

    main_nindex, main_hsize = struct.unpack(">II", raw[sig_end+8:sig_end+16])
    main_idx_start = sig_end + 16
    main_store_start = main_idx_start + main_nindex * 16
    main_end = main_store_start + main_hsize
    main_hdr = raw[sig_end:main_end]
    payload_bytes = raw[main_end:]
    hdr_and_payload = raw[sig_end:]

    sig_bin_tags = {}
    sig_str_tags = {}
    sig_int_tags = {}
    for i in range(nindex):
        eo = idx_start + i * 16
        tag, dtype, voff, count = struct.unpack(">IIII", raw[eo:eo+16])
        if dtype == 7:
            sig_bin_tags[tag] = raw[store_start+voff:store_start+voff+count]
        elif dtype in (6, 8):
            end_str = raw.find(b"\x00", store_start + voff)
            sig_str_tags[tag] = raw[store_start+voff:end_str].decode("ascii")
        elif dtype == 4 and count == 1:
            sig_int_tags[tag] = struct.unpack(">I", raw[store_start+voff:store_start+voff+4])[0]

    assert 268 in sig_bin_tags, f"RPM {rpm_path.name} missing RPMSIGTAG_RSA (tag 268)"
    assert 259 in sig_bin_tags, f"RPM {rpm_path.name} missing RPMSIGTAG_PGP (tag 259)"
    assert sig_str_tags.get(269) == hashlib.sha1(main_hdr).hexdigest(), \
        f"RPM {rpm_path.name} RPMSIGTAG_SHA1 (tag 269) mismatch: {sig_str_tags.get(269)}"
    assert sig_str_tags.get(273) == hashlib.sha256(main_hdr).hexdigest(), \
        f"RPM {rpm_path.name} RPMSIGTAG_SHA256 (tag 273) mismatch: {sig_str_tags.get(273)}"
    assert sig_int_tags.get(1000) == len(hdr_and_payload), \
        f"RPM {rpm_path.name} RPMSIGTAG_SIZE (tag 1000) mismatch: {sig_int_tags.get(1000)} != {len(hdr_and_payload)}"
    assert sig_bin_tags.get(1004) == hashlib.md5(hdr_and_payload).digest(), \
        f"RPM {rpm_path.name} RPMSIGTAG_MD5 (tag 1004) mismatch"

    main_str_tags = {}
    main_int_tags = {}
    for i in range(main_nindex):
        eo = main_idx_start + i * 16
        tag, dtype, voff, count = struct.unpack(">IIII", raw[eo:eo+16])
        if dtype in (6, 8):
            end_str = raw.find(b"\x00", main_store_start + voff)
            main_str_tags[tag] = raw[main_store_start+voff:end_str].decode("ascii", errors="replace")
        elif dtype == 4 and count == 1:
            main_int_tags[tag] = struct.unpack(">I", raw[main_store_start+voff:main_store_start+voff+4])[0]

    assert main_str_tags.get(5092) == hashlib.sha256(payload_bytes).hexdigest(), \
        f"RPM {rpm_path.name} RPMTAG_PAYLOADDIGEST (tag 5092) mismatch: {main_str_tags.get(5092)}"
    assert main_int_tags.get(5093) == 8, \
        f"RPM {rpm_path.name} RPMTAG_PAYLOADDIGESTALGO (tag 5093) must be 8 (SHA256), got {main_int_tags.get(5093)}"

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        (td_path / "hdr.bin").write_bytes(main_hdr)
        (td_path / "hdr.sig").write_bytes(sig_bin_tags[268])
        (td_path / "all.bin").write_bytes(hdr_and_payload)
        (td_path / "all.sig").write_bytes(sig_bin_tags[259])
        subprocess.run(
            ["gpgv", "--keyring", str(keyring), str(td_path / "hdr.sig"), str(td_path / "hdr.bin")],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        subprocess.run(
            ["gpgv", "--keyring", str(keyring), str(td_path / "all.sig"), str(td_path / "all.bin")],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
PYEOF

rpmkeys_cmd=()
if [ -n "${MODERNIME_RPMKEYS_BIN:-}" ]; then
    rpmkeys_cmd=("${MODERNIME_RPMKEYS_BIN}")
elif command -v rpmkeys >/dev/null 2>&1; then
    rpmkeys_cmd=(rpmkeys)
fi

rpmkeys_status_summary=""
if [ "${#rpmkeys_cmd[@]}" -gt 0 ]; then
    rpm_client_root="${work_dir}/rpm-client-root"
    mkdir -p "${rpm_client_root}/var/lib/rpm"
    "${rpmkeys_cmd[@]}" --root "${rpm_client_root}" --import "${pages_site_dir}/rpm/RPM-GPG-KEY-modernime"
    for r in "${pages_site_dir}/rpm/oe2409/x86_64/Packages"/*.rpm; do
        rpmkeys_out=$("${rpmkeys_cmd[@]}" --root "${rpm_client_root}" -v -K "${r}")
        printf '%s\n' "${rpmkeys_out}" | grep -qE "Header V4 RSA/SHA256 Signature.*: OK" || \
            fail "rpmkeys verification failed for ${r}: ${rpmkeys_out}"
        printf '%s\n' "${rpmkeys_out}" | grep -Fq "Header SHA256 digest: OK" || \
            fail "rpmkeys Header SHA256 digest failed for ${r}: ${rpmkeys_out}"
        printf '%s\n' "${rpmkeys_out}" | grep -Fq "Payload SHA256 digest: OK" || \
            fail "rpmkeys Payload SHA256 digest failed for ${r}: ${rpmkeys_out}"
    done
    printf '==> [Gate 5] Native rpmkeys -v -K client check: EXECUTED and PASSED\n'
    rpmkeys_status_summary="native rpmkeys: EXECUTED & PASSED"
else
    if [ "${MODERNIME_REQUIRE_RPMKEYS:-0}" = "1" ]; then
        fail "MODERNIME_REQUIRE_RPMKEYS=1 is set, but rpmkeys is not installed in PATH"
    fi
    printf '==> [Gate 5] Native rpmkeys -v -K client check: NOT EXECUTED (rpmkeys binary not installed on this host; OpenPGP signatures [tags 268/259] and SHA1/SHA256/MD5/Payload digests [tags 269/273/1000/1004/5092/5093] verified via gpgv + binary header inspector; native rpmkeys/dnf runs in openEuler 24.09 CI container)\n'
    rpmkeys_status_summary="native rpmkeys: NOT EXECUTED on this host (verified via gpgv + digest inspector; native rpmkeys/dnf runs in openEuler CI)"
fi

printf '=== [6/7] Simulating full v0.1.8 -> v%s upgrade -> v0.1.8 rollback -> uninstall lifecycle (DEB & RPM) ===\n' "${expected_version}"
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

cat > "${sim_root}/usr/share/modernime/install-channel.txt" <<'EOF'
channel=deb
version=0.1.8
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

# 6a. DEB upgrade 0.1.8 -> expected_version
upgrade_backup_dir="${work_dir}/pre-upgrade-backup"
"${repo_root}/scripts/modernime-data-guard" --backup \
    --data-dir "${sim_data}" \
    --config-dir "${sim_config}" \
    --backup-dir "${upgrade_backup_dir}"

MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/preinst" upgrade 0.1.8
cp -a "${deb_rootfs_dir}/usr/." "${sim_root}/usr/"
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/postinst" configure 0.1.8

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

# 6b. DEB downgrade / rollback expected_version -> 0.1.8
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/preinst" upgrade "${expected_version}"
cp -a "${deb_018_stage}/usr/." "${sim_root}/usr/"
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/postinst" configure "${expected_version}"
grep -Fxq "version=0.1.8" "${sim_root}/usr/share/modernime/install-channel.txt" ||
    fail "downgraded DEB system did not record version=0.1.8"
"${repo_root}/scripts/modernime-data-guard" --verify \
    --data-dir "${sim_data}" \
    --config-dir "${sim_config}"

# 6c. DEB uninstall (remove) preserves user learning data & config while removing /usr binaries
rm -rf "${sim_root}/usr/bin/modernime-settings" \
       "${sim_root}/usr/bin/modernime-session-init" \
       "${sim_root}/usr/share/modernime"
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/postrm" remove
[ ! -e "${sim_root}/usr/bin/modernime-settings" ] || fail "DEB remove left modernime-settings behind"
grep -Fxq "custom_theme=ocean" "${sim_config}/settings.conf" ||
    fail "user settings.conf lost during DEB remove"
"${repo_root}/scripts/modernime-data-guard" --verify \
    --data-dir "${sim_data}" \
    --config-dir "${sim_config}"

# 6d. RPM install ($1=1) -> upgrade ($1=2) -> downgrade ($1=2) -> erase ($1=0)
mkdir -p "${sim_root}/usr"
cp -a "${rpm_extract_dir}/rootfs/usr/." "${sim_root}/usr/"
sed -i "s/^version=.*/version=0.1.8/" "${sim_root}/usr/share/modernime/install-channel.txt"
MODERNIME_PKG_ROOT="${sim_root}" sh "${repo_root}/packaging/rpm/preinst.sh" 2
cp -a "${rpm_extract_dir}/rootfs/usr/." "${sim_root}/usr/"
MODERNIME_PKG_ROOT="${sim_root}" sh "${repo_root}/packaging/rpm/postinst.sh" 2
grep -Fxq "channel=rpm" "${sim_root}/usr/share/modernime/install-channel.txt" ||
    fail "RPM upgrade missing channel=rpm"
grep -Fxq "version=${expected_version}" "${sim_root}/usr/share/modernime/install-channel.txt" ||
    fail "RPM upgrade missing version=${expected_version}"
MODERNIME_PKG_ROOT="${sim_root}" sh "${repo_root}/packaging/rpm/postun.sh" 0
"${repo_root}/scripts/modernime-data-guard" --verify \
    --data-dir "${sim_data}" \
    --config-dir "${sim_config}"

printf '=== [7/7] Verifying rejection gates & post-package-install migration rollback ===\n'
# 7a. Channel collision rejection in DEB preinst and RPM preinst
collision_root="${work_dir}/collision-root"
mkdir -p "${collision_root}/usr/local/lib/fcitx5" "${collision_root}/usr/local/share/modernime"
printf 'fake-script-plugin\n' > "${collision_root}/usr/local/lib/fcitx5/modernime_fcitx5.so"
cat > "${collision_root}/usr/local/share/modernime/install-channel.txt" <<'EOF'
channel=script
version=0.1.8
prefix=/usr/local
EOF

assert_normal_nonzero_failure \
    "DEB preinst should reject unmanaged script installation in /usr/local" \
    env MODERNIME_PKG_ROOT="${collision_root}" sh "${deb_ctrl_dir}/preinst" install
assert_normal_nonzero_failure \
    "RPM preinst should reject unmanaged script installation in /usr/local" \
    env MODERNIME_PKG_ROOT="${collision_root}" sh "${repo_root}/packaging/rpm/preinst.sh" 1

# 7b. Post-package-install failure rollback in modernime-migrate-channel
mig_root="${work_dir}/mig-rollback"
mig_from="${mig_root}/local"
mig_to="${mig_root}/usr"
mkdir -p "${mig_from}/bin" "${mig_from}/lib/fcitx5" "${mig_from}/share/modernime"
printf '#!/bin/sh\nexit 0\n' > "${mig_from}/bin/modernime-settings"
chmod +x "${mig_from}/bin/modernime-settings"
printf 'script-so\n' > "${mig_from}/lib/fcitx5/modernime_fcitx5.so"
cat > "${mig_from}/share/modernime/install-manifest.txt" <<EOF
${mig_from}/bin/modernime-settings
${mig_from}/lib/fcitx5/modernime_fcitx5.so
EOF
cat > "${mig_from}/share/modernime/install-channel.txt" <<EOF
channel=script
version=0.1.8
prefix=${mig_from}
EOF

bad_pkg_cmd="mkdir -p '${mig_to}/bin' '${mig_to}/lib/fcitx5' '${mig_to}/share/modernime' && printf '#!/bin/sh\n' > '${mig_to}/bin/modernime-settings' && chmod +x '${mig_to}/bin/modernime-settings' && printf 'pkg-so\n' > '${mig_to}/lib/fcitx5/modernime_fcitx5.so' && printf 'channel=rpm\nversion=${expected_version}\n' > '${mig_to}/share/modernime/install-channel.txt'"
rollback_marker="${mig_root}/pkg-rollback-executed"
rollback_pkg_cmd="rm -rf '${mig_to}/bin/modernime-settings' '${mig_to}/lib/fcitx5/modernime_fcitx5.so' '${mig_to}/share/modernime' && touch '${rollback_marker}'"

assert_normal_nonzero_failure \
    "modernime-migrate-channel should fail when post-install channel verification fails" \
    env MODERNIME_DATA_HOME="${sim_data}" \
    MODERNIME_CONFIG_HOME="${sim_config}" \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    "${repo_root}/scripts/modernime-migrate-channel" \
    --from-prefix "${mig_from}" \
    --target-prefix "${mig_to}" \
    --to-channel deb \
    --package-cmd "${bad_pkg_cmd}" \
    --package-rollback-cmd "${rollback_pkg_cmd}"
[ -f "${rollback_marker}" ] || fail "modernime-migrate-channel did not execute --package-rollback-cmd"
[ ! -e "${mig_to}/lib/fcitx5/modernime_fcitx5.so" ] || fail "half-installed target package files were not cleaned up on rollback"
[ -f "${mig_from}/lib/fcitx5/modernime_fcitx5.so" ] || fail "original script installation was not restored after rollback"

# 7c. Architecture mismatch rejection in APT repo builder
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

assert_normal_nonzero_failure \
    "build_apt_repo.sh should reject arm64 DEB when building amd64 repository" \
    "${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${work_dir}/apt-arch-fail" \
    --deb "${fake_arm_deb}" \
    --arch amd64 \
    --ephemeral-gpg

# 7d. Non-monotonic version regression rejection in APT repo builder
assert_normal_nonzero_failure \
    "build_apt_repo.sh should reject non-monotonic version when --prev-version >= candidate version" \
    "${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${work_dir}/apt-ver-fail" \
    --deb "${deb_file}" \
    --prev-version "${expected_version}" \
    --arch amd64 \
    --ephemeral-gpg

# 7e. Missing runtime dependency rejection in install.sh
empty_bin="${work_dir}/empty-bin"
mkdir -p "${empty_bin}"
ln -sf "$(command -v bash)" "${empty_bin}/bash"
ln -sf "$(command -v sh)" "${empty_bin}/sh"
ln -sf "$(command -v uname)" "${empty_bin}/uname"
ln -sf "$(command -v dirname)" "${empty_bin}/dirname"
ln -sf "$(command -v basename)" "${empty_bin}/basename"
ln -sf "$(command -v mkdir)" "${empty_bin}/mkdir"
ln -sf "$(command -v rm)" "${empty_bin}/rm"

assert_normal_nonzero_failure \
    "install.sh should reject installation when fcitx5 runtime dependency is missing" \
    env PATH="${empty_bin}" MODERNIME_SKIP_FCITX_RESTART=1 \
    bash "${repo_root}/install.sh" "${work_dir}/missing-dep-prefix"

# 7f. Isolated zero-cache GPG passphrase matrix (correct / missing / wrong passphrase) for rpmsign & fallback branches
rpmsign_test_bin=""
rpmsign_mode_label=""
rpmsign_status_summary=""
if [ -n "${MODERNIME_RPMSIGN_BIN:-}" ]; then
    rpmsign_test_bin="${MODERNIME_RPMSIGN_BIN}"
    rpmsign_mode_label="custom rpmsign (${rpmsign_test_bin})"
    rpmsign_status_summary="custom rpmsign (${rpmsign_test_bin}): EXECUTED & PASSED"
elif command -v rpmsign >/dev/null 2>&1; then
    rpmsign_test_bin="$(command -v rpmsign)"
    rpmsign_mode_label="native rpmsign (${rpmsign_test_bin}): EXECUTED & PASSED"
    rpmsign_status_summary="native rpmsign: EXECUTED & PASSED"
else
    if [ "${MODERNIME_REQUIRE_NATIVE_RPM_TOOLS:-0}" = "1" ]; then
        fail "MODERNIME_REQUIRE_NATIVE_RPM_TOOLS=1 is set, but native rpmsign is not installed in PATH"
    fi
    rpmsign_shim_dir="${work_dir}/rpmsign-shim"
    mkdir -p "${rpmsign_shim_dir}"
    rpmsign_test_bin="${rpmsign_shim_dir}/rpmsign"
    rpmsign_mode_label="native rpmsign: NOT EXECUTED on this host; simulated rpmsign harness: EXECUTED & PASSED"
    rpmsign_status_summary="native rpmsign: NOT EXECUTED on this host (simulated rpmsign harness + fallback verified; native rpmsign runs in openEuler CI)"
    cat > "${rpmsign_test_bin}" <<'PYEOF'
#!/usr/bin/env python3
import hashlib
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile

RPM_HDR_MAGIC = b"\x8e\xad\xe8\x01"
RPMSIGTAG_PGP = 259
RPMSIGTAG_RSA = 268
RPMSIGTAG_SHA1 = 269
RPMSIGTAG_SHA256 = 273
RPMSIGTAG_SIZE = 1000
RPMSIGTAG_MD5 = 1004

macros = {}
rpm_target = None
args = sys.argv[1:]
i = 0
while i < len(args):
    if args[i] == "--define" and i + 1 < len(args):
        defn = args[i + 1].strip()
        parts = defn.split(None, 1)
        if len(parts) == 2:
            macros[parts[0]] = parts[1]
        i += 2
    elif args[i] == "--addsign" and i + 1 < len(args):
        rpm_target = Path(args[i + 1])
        i += 2
    else:
        i += 1

if not rpm_target or "__gpg_sign_cmd" not in macros:
    sys.stderr.write("rpmsign shim: missing --addsign target or __gpg_sign_cmd macro\n")
    sys.exit(1)

raw = rpm_target.read_bytes()
nindex, hsize = struct.unpack(">II", raw[104:112])
sig_end = (112 + nindex * 16 + hsize + 7) & ~7
if raw[sig_end : sig_end + 4] != RPM_HDR_MAGIC:
    sig_end = raw.find(RPM_HDR_MAGIC, 112 + nindex * 16 + hsize)
m_nindex, m_hsize = struct.unpack(">II", raw[sig_end + 8 : sig_end + 16])
payload_off = sig_end + 16 + m_nindex * 16 + m_hsize
main_hdr = raw[sig_end:payload_off]
payload = raw[payload_off:]

def run_gpg_sign_macro(plain_bytes: bytes) -> bytes:
    with tempfile.TemporaryDirectory() as td:
        plain_f = Path(td) / "plain.bin"
        sig_f = Path(td) / "sig.bin"
        plain_f.write_bytes(plain_bytes)
        cmd_str = macros["__gpg_sign_cmd"]
        for k, v in [
            ("%{__gpg}", macros.get("__gpg", "/usr/bin/gpg")),
            ("%{_gpg_path}", macros.get("_gpg_path", "")),
            ("%{_gpg_name}", macros.get("_gpg_name", "")),
            ("%{__plaintext_filename}", str(plain_f)),
            ("%{__signature_filename}", str(sig_f)),
        ]:
            cmd_str = cmd_str.replace(k, v)
        argv = shlex.split(cmd_str)
        # RPM's rpmgensig.c executes argv[0] with argv[1:] (where argv[1] is "gpg")
        exec_bin = argv[0]
        exec_args = [exec_bin] + argv[2:] if len(argv) > 1 and argv[1].endswith("gpg") else argv
        res = subprocess.run(exec_args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if res.returncode != 0:
            sys.stderr.write(res.stderr.decode("utf-8", errors="replace"))
            sys.exit(res.returncode)
        return sig_f.read_bytes()

rsa_sig = run_gpg_sign_macro(main_hdr)
pgp_sig = run_gpg_sign_macro(main_hdr + payload)

entries = sorted([
    (RPMSIGTAG_PGP, 7, pgp_sig),
    (RPMSIGTAG_RSA, 7, rsa_sig),
    (RPMSIGTAG_SHA1, 6, hashlib.sha1(main_hdr).hexdigest()),
    (RPMSIGTAG_SHA256, 6, hashlib.sha256(main_hdr).hexdigest()),
    (RPMSIGTAG_SIZE, 4, len(main_hdr) + len(payload)),
    (RPMSIGTAG_MD5, 7, hashlib.md5(main_hdr + payload).digest()),
], key=lambda x: x[0])

idx_buf = bytearray()
store_buf = bytearray()
for tag, dtype, val in entries:
    if dtype == 4:
        if len(store_buf) % 4 != 0:
            store_buf.extend(b"\x00" * (4 - (len(store_buf) % 4)))
        off = len(store_buf)
        store_buf.extend(struct.pack(">I", int(val)))
        idx_buf.extend(struct.pack(">IIII", tag, 4, off, 1))
    elif dtype == 6:
        off = len(store_buf)
        b = val.encode("utf-8") + b"\x00"
        store_buf.extend(b)
        idx_buf.extend(struct.pack(">IIII", tag, 6, off, 1))
    elif dtype == 7:
        off = len(store_buf)
        store_buf.extend(val)
        idx_buf.extend(struct.pack(">IIII", tag, 7, off, len(val)))

total_entries = len(entries) + 1
region_off = len(store_buf)
trailer = struct.pack(">IIiI", 62, 7, -(total_entries * 16), 16)
region_idx = struct.pack(">IIII", 62, 7, region_off, 16)
idx_buf = bytearray(region_idx) + idx_buf
store_buf.extend(trailer)

new_sig = b"\x8e\xad\xe8\x01\x00\x00\x00\x00" + struct.pack(">II", total_entries, len(store_buf)) + bytes(idx_buf) + bytes(store_buf)
if len(new_sig) % 8 != 0:
    new_sig += b"\x00" * (8 - (len(new_sig) % 8))
rpm_target.write_bytes(raw[:96] + new_sig + main_hdr + payload)
PYEOF
    chmod +x "${rpmsign_test_bin}"
fi

# Verify both rpmsign branch and fallback branch with isolated zero-cache --gpg-home
for branch_mode in "rpmsign" "fallback"; do
    branch_env=()
    if [ "${branch_mode}" = "rpmsign" ]; then
        branch_env=("MODERNIME_RPMSIGN_BIN=${rpmsign_test_bin}" "MODERNIME_DISABLE_RPMSIGN=0")
    else
        branch_env=("MODERNIME_DISABLE_RPMSIGN=1" "MODERNIME_REQUIRE_NATIVE_RPM_TOOLS=0")
    fi

    # (1) Correct passphrase in isolated zero-cache --gpg-home MUST succeed within 10s
    timeout 10s env "${branch_env[@]}" \
        "${repo_root}/scripts/build_rpm_repo.sh" \
        --output-dir "${work_dir}/rpm-pass-ok-${branch_mode}" \
        --rpm "${rpm_file}" \
        --distro oe2409 \
        --arch x86_64 \
        --gpg-home "${gpg_setup_home}" \
        --gpg-key-id "${primary_fpr}" \
        --gpg-passphrase "${passphrase}" >/dev/null ||
        fail "build_rpm_repo.sh (${branch_mode} branch) failed with correct passphrase in isolated --gpg-home"

    # (2) Missing passphrase in isolated zero-cache --gpg-home MUST fail fast with normal non-zero exit code (1..123, NOT 124/137)
    assert_normal_nonzero_failure \
        "build_rpm_repo.sh (${branch_mode} branch) must fail normally when passphrase is missing in isolated zero-cache --gpg-home" \
        timeout 10s env -u MODERNIME_GPG_PASSPHRASE "${branch_env[@]}" \
        "${repo_root}/scripts/build_rpm_repo.sh" \
        --output-dir "${work_dir}/rpm-pass-missing-${branch_mode}" \
        --rpm "${rpm_file}" \
        --distro oe2409 \
        --arch x86_64 \
        --gpg-home "${gpg_setup_home}" \
        --gpg-key-id "${primary_fpr}"

    # (3) Wrong passphrase in isolated zero-cache --gpg-home MUST fail fast with normal non-zero exit code (1..123, NOT 124/137)
    assert_normal_nonzero_failure \
        "build_rpm_repo.sh (${branch_mode} branch) must fail normally when passphrase is wrong in isolated zero-cache --gpg-home" \
        timeout 10s env "${branch_env[@]}" \
        "${repo_root}/scripts/build_rpm_repo.sh" \
        --output-dir "${work_dir}/rpm-pass-wrong-${branch_mode}" \
        --rpm "${rpm_file}" \
        --distro oe2409 \
        --arch x86_64 \
        --gpg-home "${gpg_setup_home}" \
        --gpg-key-id "${primary_fpr}" \
        --gpg-passphrase "WrongPassphrase2026!"
done

# (4) Also verify --gpg-private-key-file with wrong passphrase fails fast with normal non-zero exit code (1..123)
assert_normal_nonzero_failure \
    "build_rpm_repo.sh must fail normally when --gpg-private-key-file is given wrong passphrase" \
    timeout 10s env MODERNIME_RPMSIGN_BIN="${rpmsign_test_bin}" \
    "${repo_root}/scripts/build_rpm_repo.sh" \
    --output-dir "${work_dir}/rpm-privkey-wrong-pass" \
    --rpm "${rpm_file}" \
    --distro oe2409 \
    --arch x86_64 \
    --gpg-private-key-file "${primary_sec_file}" \
    --gpg-key-id "${primary_fpr}" \
    --gpg-passphrase "WrongPassphrase2026!"

# (5) Verify rpmsign failure does NOT silently fall back to Python signing
failing_rpmsign="${work_dir}/failing-rpmsign"
printf '#!/bin/sh\necho "simulated rpmsign error" >&2\nexit 2\n' > "${failing_rpmsign}"
chmod +x "${failing_rpmsign}"
assert_normal_nonzero_failure \
    "build_rpm_repo.sh must fail immediately when rpmsign exits non-zero (must not silently fall back)" \
    timeout 10s env MODERNIME_RPMSIGN_BIN="${failing_rpmsign}" \
    "${repo_root}/scripts/build_rpm_repo.sh" \
    --output-dir "${work_dir}/rpm-rpmsign-no-fallback" \
    --rpm "${rpm_file}" \
    --distro oe2409 \
    --arch x86_64 \
    --gpg-home "${gpg_setup_home}" \
    --gpg-key-id "${primary_fpr}" \
    --gpg-passphrase "${passphrase}"

# (6) Deliberately hanging mock rpmsign: verify build_rpm_repo.sh kills the hanging process group on timeout (exit 124)
# AND verify that the gate's assert_normal_nonzero_failure REJECTS exit codes 124 (timeout) and 137 (SIGKILL).
hanging_rpmsign="${work_dir}/hanging-rpmsign"
printf '#!/bin/sh\nsleep 30\n' > "${hanging_rpmsign}"
chmod +x "${hanging_rpmsign}"

hang_rc=0
timeout 5s env MODERNIME_RPMSIGN_BIN="${hanging_rpmsign}" MODERNIME_RPMSIGN_TIMEOUT_SEC=1 \
    "${repo_root}/scripts/build_rpm_repo.sh" \
    --output-dir "${work_dir}/rpm-rpmsign-hang" \
    --rpm "${rpm_file}" \
    --distro oe2409 \
    --arch x86_64 \
    --gpg-home "${gpg_setup_home}" \
    --gpg-key-id "${primary_fpr}" \
    --gpg-passphrase "${passphrase}" >/dev/null 2>&1 || hang_rc=$?
[ "${hang_rc}" -eq 124 ] || fail "Expected hanging rpmsign to be terminated with timeout exit code 124, got ${hang_rc}"

if (
    assert_normal_nonzero_failure \
        "hanging rpmsign timeout must be rejected by expected-failure check" \
        timeout 5s env MODERNIME_RPMSIGN_BIN="${hanging_rpmsign}" MODERNIME_RPMSIGN_TIMEOUT_SEC=1 \
        "${repo_root}/scripts/build_rpm_repo.sh" \
        --output-dir "${work_dir}/rpm-rpmsign-hang-gate-check" \
        --rpm "${rpm_file}" \
        --distro oe2409 \
        --arch x86_64 \
        --gpg-home "${gpg_setup_home}" \
        --gpg-key-id "${primary_fpr}" \
        --gpg-passphrase "${passphrase}"
) >/dev/null 2>&1; then
    fail "assert_normal_nonzero_failure falsely accepted timeout exit code 124 as a normal expected failure"
fi

sigkill_rpmsign="${work_dir}/sigkill-rpmsign"
printf '#!/bin/sh\nkill -9 $$\n' > "${sigkill_rpmsign}"
chmod +x "${sigkill_rpmsign}"
if (
    assert_normal_nonzero_failure \
        "SIGKILL rpmsign (137) must be rejected by expected-failure check" \
        timeout 5s env MODERNIME_RPMSIGN_BIN="${sigkill_rpmsign}" \
        "${repo_root}/scripts/build_rpm_repo.sh" \
        --output-dir "${work_dir}/rpm-rpmsign-sigkill-gate-check" \
        --rpm "${rpm_file}" \
        --distro oe2409 \
        --arch x86_64 \
        --gpg-home "${gpg_setup_home}" \
        --gpg-key-id "${primary_fpr}" \
        --gpg-passphrase "${passphrase}"
) >/dev/null 2>&1; then
    fail "assert_normal_nonzero_failure falsely accepted SIGKILL (137) as a normal expected failure"
fi

printf '==> [Gate 7f] Isolated zero-cache GPG passphrase matrix (correct/missing/wrong) & timeout/signal (124/137) rejection: VERIFIED [%s; fallback branch: EXECUTED & PASSED]\n' "${rpmsign_mode_label}"

# 7g. Release tag resolution, immutable product source SHA binding, and version-mismatch rejection
tag_test_repo="${work_dir}/tag-bind-repo"
mkdir -p "${tag_test_repo}"
git -C "${tag_test_repo}" init -q
git -C "${tag_test_repo}" config user.name "Test Release Bot"
git -C "${tag_test_repo}" config user.email "release-test@modernime.org"
cat > "${tag_test_repo}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(ModernIME VERSION 0.1.9 LANGUAGES CXX)
EOF
git -C "${tag_test_repo}" add CMakeLists.txt
git -C "${tag_test_repo}" commit -q -m "release: v0.1.9 product source"
tag_commit_019=$(git -C "${tag_test_repo}" rev-parse --verify HEAD)
git -C "${tag_test_repo}" tag -a v0.1.9 -m "ModernIME v0.1.9"
# Create a mismatched annotated tag v0.1.8 pointing to the 0.1.9 commit
git -C "${tag_test_repo}" tag -a v0.1.8 -m "Mismatched tag pointing to 0.1.9 source"

# Advance main HEAD with subsequent workflow/Version commits to verify workflow_dispatch binds to v0.1.9's commit, NOT main HEAD
cat > "${tag_test_repo}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(ModernIME VERSION 0.2.0 LANGUAGES CXX)
EOF
git -C "${tag_test_repo}" add CMakeLists.txt
git -C "${tag_test_repo}" commit -q -m "chore: advance main past v0.1.9"
main_head_sha=$(git -C "${tag_test_repo}" rev-parse --verify HEAD)
[ "${tag_commit_019}" != "${main_head_sha}" ] || fail "tag_commit_019 should differ from main_head_sha"

gh_out_dispatch="${work_dir}/gh-out-dispatch.txt"
"${repo_root}/scripts/resolve_release_target.sh" \
    --event-name workflow_dispatch \
    --ref-type branch \
    --ref-name main \
    --input-tag v0.1.9 \
    --repo-dir "${tag_test_repo}" \
    --github-output "${gh_out_dispatch}" >/dev/null
grep -Fxq "tag=v0.1.9" "${gh_out_dispatch}" || fail "resolve_release_target.sh did not output tag=v0.1.9 for workflow_dispatch"
grep -Fxq "version=0.1.9" "${gh_out_dispatch}" || fail "resolve_release_target.sh did not output version=0.1.9 for workflow_dispatch"
grep -Fxq "source_sha=${tag_commit_019}" "${gh_out_dispatch}" || fail "resolve_release_target.sh did not bind source_sha to v0.1.9 commit (${tag_commit_019})"

gh_out_push="${work_dir}/gh-out-push.txt"
"${repo_root}/scripts/resolve_release_target.sh" \
    --event-name push \
    --ref-type tag \
    --ref-name v0.1.9 \
    --repo-dir "${tag_test_repo}" \
    --github-output "${gh_out_push}" >/dev/null
grep -Fxq "tag=v0.1.9" "${gh_out_push}" || fail "resolve_release_target.sh did not output tag=v0.1.9 for push tag"
grep -Fxq "source_sha=${tag_commit_019}" "${gh_out_push}" || fail "resolve_release_target.sh did not bind source_sha for push tag"

# Reject workflow_dispatch without tag_name
assert_normal_nonzero_failure \
    "resolve_release_target.sh must reject workflow_dispatch when tag_name is empty" \
    "${repo_root}/scripts/resolve_release_target.sh" \
    --event-name workflow_dispatch \
    --ref-type branch \
    --ref-name main \
    --input-tag "" \
    --repo-dir "${tag_test_repo}"

# Reject non-existent tag
assert_normal_nonzero_failure \
    "resolve_release_target.sh must reject non-existent release tag" \
    "${repo_root}/scripts/resolve_release_target.sh" \
    --event-name workflow_dispatch \
    --input-tag v0.9.9 \
    --repo-dir "${tag_test_repo}"

# Reject tag whose CMakeLists.txt version does not match tag version
assert_normal_nonzero_failure \
    "resolve_release_target.sh must reject tag v0.1.8 when target commit CMakeLists.txt version is 0.1.9" \
    "${repo_root}/scripts/resolve_release_target.sh" \
    --event-name workflow_dispatch \
    --input-tag v0.1.8 \
    --repo-dir "${tag_test_repo}"

# Reject push event on a branch instead of a tag
assert_normal_nonzero_failure \
    "resolve_release_target.sh must reject push event when ref_type is branch" \
    "${repo_root}/scripts/resolve_release_target.sh" \
    --event-name push \
    --ref-type branch \
    --ref-name main \
    --repo-dir "${tag_test_repo}"

# Reject building from main HEAD when bound to v0.1.9 source_sha
assert_normal_nonzero_failure \
    "resolve_release_target.sh --assert-checkout-sha must reject building from main HEAD instead of bound tag commit" \
    "${repo_root}/scripts/resolve_release_target.sh" \
    --assert-checkout-sha "${tag_commit_019}" \
    --assert-version "0.1.9" \
    --repo-dir "${tag_test_repo}"

# Checkout bound tag commit and verify --assert-checkout-sha succeeds
git -C "${tag_test_repo}" checkout -q "${tag_commit_019}"
"${repo_root}/scripts/resolve_release_target.sh" \
    --assert-checkout-sha "${tag_commit_019}" \
    --assert-version "0.1.9" \
    --repo-dir "${tag_test_repo}" >/dev/null || fail "--assert-checkout-sha failed on valid bound tag checkout"

printf '==> [Gate 7g] Release tag resolution, immutable source SHA binding & version-mismatch rejection: VERIFIED\n'

printf 'PASS: All 7 package, signed repository, real APT client, and rollback gate checks succeeded (%s; %s).\n' "${rpmkeys_status_summary}" "${rpmsign_status_summary}"
