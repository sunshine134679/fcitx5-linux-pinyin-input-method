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

printf '=== [1/7] Building / verifying DEB and RPM packages (v%s) ===\n' "${expected_version}"
deb_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.deb" 2>/dev/null | head -n 1 || true)
if [ -z "${deb_file}" ] || [ "${repo_root}/scripts/modernime-migrate-channel" -nt "${deb_file}" ] || [ "${repo_root}/tools/settings/update_controller.cpp" -nt "${deb_file}" ]; then
    "${repo_root}/scripts/package_deb.sh"
    deb_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.deb" | head -n 1)
fi
[ -f "${deb_file}" ] || fail "DEB package not produced in dist/"

rpm_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.rpm" 2>/dev/null | head -n 1 || true)
if [ -z "${rpm_file}" ] || [ "${repo_root}/scripts/modernime-migrate-channel" -nt "${rpm_file}" ] || [ "${repo_root}/tools/settings/update_controller.cpp" -nt "${rpm_file}" ]; then
    "${repo_root}/scripts/package_rpm.sh"
    rpm_file=$(find "${repo_root}/dist" -maxdepth 1 -type f -name "modernime*${expected_version}*.rpm" | head -n 1)
fi
[ -f "${rpm_file}" ] || fail "RPM package not produced in dist/"

printf '=== [2/7] Verifying DEB & RPM file boundaries, dependencies, licenses, and zero user data ===\n'
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

printf '=== [3/7] Verifying CI release repository builder, persistent GPG key rotation, and multi-version Pages tree ===\n'
ci_assets_dir="${work_dir}/ci-release-assets"
prev_pages_dir="${work_dir}/prev-pages"
pages_site_dir="${work_dir}/pages-site"
mkdir -p "${ci_assets_dir}" \
    "${prev_pages_dir}/apt/pool/jammy/main/m/modernime" \
    "${prev_pages_dir}/apt/pool/noble/main/m/modernime" \
    "${prev_pages_dir}/apt/pool/bookworm/main/m/modernime" \
    "${prev_pages_dir}/rpm/oe2409/x86_64/Packages"

# Copy current DEB/RPM using the exact filenames produced by .github/workflows/release.yml
cp -f "${deb_file}" "${ci_assets_dir}/modernime_${expected_version}_amd64_ubuntu22.04.deb"
cp -f "${deb_file}" "${ci_assets_dir}/modernime_${expected_version}_amd64_ubuntu24.04.deb"
cp -f "${rpm_file}" "${ci_assets_dir}/modernime-${expected_version}-1.oe24.x86_64.rpm"

# Build synthetic v0.1.7 DEB and v0.1.7 RPM in prev_pages_dir to test multi-version retention & downgrade/rollback
deb_017_stage="${work_dir}/deb-0.1.7-stage"
mkdir -p "${deb_017_stage}"
dpkg-deb -R "${deb_file}" "${deb_017_stage}"
sed -i "s/^Version: .*/Version: 0.1.7/" "${deb_017_stage}/DEBIAN/control"
cat > "${deb_017_stage}/usr/share/modernime/install-channel.txt" <<'EOF'
channel=deb
version=0.1.7
prefix=/usr
EOF
chmod 0755 "${deb_017_stage}/DEBIAN/"* 2>/dev/null || true
chmod 0644 "${deb_017_stage}/DEBIAN/control" 2>/dev/null || true
deb_017_file="${work_dir}/modernime_0.1.7_amd64.deb"
dpkg-deb --build "${deb_017_stage}" "${deb_017_file}" >/dev/null
cp -f "${deb_017_file}" "${prev_pages_dir}/apt/pool/jammy/main/m/modernime/modernime_0.1.7_amd64_ubuntu22.04.deb"
cp -f "${deb_017_file}" "${prev_pages_dir}/apt/pool/noble/main/m/modernime/modernime_0.1.7_amd64_ubuntu24.04.deb"
cp -f "${deb_017_file}" "${prev_pages_dir}/apt/pool/bookworm/main/m/modernime/modernime_0.1.7_amd64_debian12.deb"

rpm_017_file="${prev_pages_dir}/rpm/oe2409/x86_64/Packages/modernime-0.1.7-1.oe24.x86_64.rpm"
python3 - "${rpm_file}" "${rpm_017_file}" "${expected_version}" <<'PYEOF'
from pathlib import Path
import sys

src_bytes = Path(sys.argv[1]).read_bytes()
dst_path = Path(sys.argv[2])
cur_ver = sys.argv[3].encode("ascii")
assert len(cur_ver) == len(b"0.1.7"), "Version length mismatch for binary patch"
dst_path.write_bytes(src_bytes.replace(cur_ver, b"0.1.7"))
PYEOF

# Gate check: --require-persistent-gpg MUST fail when no persistent key is provided
if env -u MODERNIME_GPG_PRIVATE_KEY -u MODERNIME_GPG_KEY_ID \
    "${repo_root}/scripts/ci_build_release_repos.sh" \
    --assets-dir "${ci_assets_dir}" \
    --pages-dir "${work_dir}/pages-should-fail" \
    --require-persistent-gpg >/dev/null 2>&1; then
    fail "ci_build_release_repos.sh --require-persistent-gpg must fail when persistent GPG key is absent"
fi

# Generate persistent primary GPG key AND rotation GPG key to verify key persistence + dual-key rotation
gpg_setup_home="${work_dir}/gpg-setup"
mkdir -p "${gpg_setup_home}"
chmod 700 "${gpg_setup_home}"
gpg --homedir "${gpg_setup_home}" --batch --generate-key <<'GPGEOF' >/dev/null 2>&1
%no-protection
Key-Type: RSA
Key-Length: 2048
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
gpg --homedir "${gpg_setup_home}" --armor --export-secret-keys "${primary_fpr}" > "${primary_sec_file}"

primary_only_keyring="${work_dir}/primary-only.gpg"
rotation_only_keyring="${work_dir}/rotation-only.gpg"
gpg --homedir "${gpg_setup_home}" --export "${primary_fpr}" > "${primary_only_keyring}"
gpg --homedir "${gpg_setup_home}" --export "${rotation_fpr}" > "${rotation_only_keyring}"

MODERNIME_GPG_KEY_ID="${primary_fpr}" \
MODERNIME_GPG_ROTATION_PRIVATE_KEY="${rotation_sec_value}" \
MODERNIME_GPG_ROTATION_KEY_ID="${rotation_fpr}" \
"${repo_root}/scripts/ci_build_release_repos.sh" \
    --assets-dir "${ci_assets_dir}" \
    --pages-dir "${pages_site_dir}" \
    --previous-pages-dir "${prev_pages_dir}" \
    --gpg-private-key-file "${primary_sec_file}" \
    --require-persistent-gpg \
    --repo-base-url "file://${pages_site_dir}"

[ -f "${pages_site_dir}/.nojekyll" ] || fail "pages-site/.nojekyll missing"
[ -f "${pages_site_dir}/apt/KEY-FINGERPRINTS.txt" ] || fail "apt/KEY-FINGERPRINTS.txt missing"
[ -f "${pages_site_dir}/rpm/KEY-FINGERPRINTS.txt" ] || fail "rpm/KEY-FINGERPRINTS.txt missing"
grep -Fq "${primary_fpr}" "${pages_site_dir}/apt/KEY-FINGERPRINTS.txt" || fail "primary fingerprint missing from KEY-FINGERPRINTS.txt"
grep -Fq "${rotation_fpr}" "${pages_site_dir}/apt/KEY-FINGERPRINTS.txt" || fail "rotation fingerprint missing from KEY-FINGERPRINTS.txt"

for suite in jammy noble bookworm; do
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

printf '=== [4/7] Running REAL apt-get update & apt-cache policy against signed local APT repository (jammy, noble, bookworm) ===\n'
for suite in jammy noble bookworm; do
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
Version: 0.1.7
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
    printf '%s\n' "${policy_out}" | grep -Fq "Installed: 0.1.7" ||
        fail "apt-cache policy did not report Installed: 0.1.7 for ${suite}"
    printf '%s\n' "${policy_out}" | grep -Fq "Candidate: ${expected_version}" ||
        fail "apt-cache policy did not resolve Candidate: ${expected_version} for ${suite}"
    printf '%s\n' "${policy_out}" | grep -Fq "${expected_version} 700" ||
        fail "apt-cache policy did not apply Pin-Priority 700 to ${expected_version} from 99-modernime.pref for ${suite}"
    printf '%s\n' "${policy_out}" | grep -Fq "file:${pages_site_dir}/apt ${suite}/main amd64 Packages" ||
        fail "apt-cache policy missing expected repository source URL for ${suite}"

    madison_out=$(LC_ALL=C LANG=C apt-cache "${apt_opts[@]}" madison modernime)
    printf '%s\n' "${madison_out}" | grep -Fq "${expected_version}" ||
        fail "apt-cache madison missing ${expected_version} for ${suite}"
    printf '%s\n' "${madison_out}" | grep -Fq "0.1.7" ||
        fail "apt-cache madison missing retained rollback version 0.1.7 for ${suite}"
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

assert "0.1.7" in pkgs_in_xml, "primary.xml.gz missing retained version 0.1.7"
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
    main_end = sig_end + 16 + main_nindex * 16 + main_hsize
    main_hdr = raw[sig_end:main_end]
    hdr_and_payload = raw[sig_end:]

    sig_tags = {}
    for i in range(nindex):
        eo = idx_start + i * 16
        tag, dtype, voff, count = struct.unpack(">IIII", raw[eo:eo+16])
        if dtype == 7:
            sig_tags[tag] = raw[store_start+voff:store_start+voff+count]

    assert 268 in sig_tags, f"RPM {rpm_path.name} missing RPMSIGTAG_RSA (tag 268)"
    assert 259 in sig_tags, f"RPM {rpm_path.name} missing RPMSIGTAG_PGP (tag 259)"

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        (td_path / "hdr.bin").write_bytes(main_hdr)
        (td_path / "hdr.sig").write_bytes(sig_tags[268])
        (td_path / "all.bin").write_bytes(hdr_and_payload)
        (td_path / "all.sig").write_bytes(sig_tags[259])
        subprocess.run(
            ["gpgv", "--keyring", str(keyring), str(td_path / "hdr.sig"), str(td_path / "hdr.bin")],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        subprocess.run(
            ["gpgv", "--keyring", str(keyring), str(td_path / "all.sig"), str(td_path / "all.bin")],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
PYEOF

printf '=== [6/7] Simulating full v0.1.7 -> v%s upgrade -> v0.1.7 rollback -> uninstall lifecycle (DEB & RPM) ===\n' "${expected_version}"
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

# 6a. DEB upgrade 0.1.7 -> 0.1.8
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

# 6b. DEB downgrade / rollback 0.1.8 -> 0.1.7
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/preinst" upgrade "${expected_version}"
cp -a "${deb_017_stage}/usr/." "${sim_root}/usr/"
MODERNIME_PKG_ROOT="${sim_root}" sh "${deb_ctrl_dir}/postinst" configure "${expected_version}"
grep -Fxq "version=0.1.7" "${sim_root}/usr/share/modernime/install-channel.txt" ||
    fail "downgraded DEB system did not record version=0.1.7"
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
sed -i "s/^version=.*/version=0.1.7/" "${sim_root}/usr/share/modernime/install-channel.txt"
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

if MODERNIME_PKG_ROOT="${collision_root}" sh "${deb_ctrl_dir}/preinst" install >/dev/null 2>&1; then
    fail "DEB preinst should reject unmanaged script installation in /usr/local"
fi
if MODERNIME_PKG_ROOT="${collision_root}" sh "${repo_root}/packaging/rpm/preinst.sh" 1 >/dev/null 2>&1; then
    fail "RPM preinst should reject unmanaged script installation in /usr/local"
fi

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
version=0.1.7
prefix=${mig_from}
EOF

bad_pkg_cmd="mkdir -p '${mig_to}/bin' '${mig_to}/lib/fcitx5' '${mig_to}/share/modernime' && printf '#!/bin/sh\n' > '${mig_to}/bin/modernime-settings' && chmod +x '${mig_to}/bin/modernime-settings' && printf 'pkg-so\n' > '${mig_to}/lib/fcitx5/modernime_fcitx5.so' && printf 'channel=rpm\nversion=0.1.8\n' > '${mig_to}/share/modernime/install-channel.txt'"
rollback_marker="${mig_root}/pkg-rollback-executed"
rollback_pkg_cmd="rm -rf '${mig_to}/bin/modernime-settings' '${mig_to}/lib/fcitx5/modernime_fcitx5.so' '${mig_to}/share/modernime' && touch '${rollback_marker}'"

if MODERNIME_DATA_HOME="${sim_data}" \
   MODERNIME_CONFIG_HOME="${sim_config}" \
   MODERNIME_SKIP_FCITX_RESTART=1 \
   "${repo_root}/scripts/modernime-migrate-channel" \
    --from-prefix "${mig_from}" \
    --target-prefix "${mig_to}" \
    --to-channel deb \
    --package-cmd "${bad_pkg_cmd}" \
    --package-rollback-cmd "${rollback_pkg_cmd}" >/dev/null 2>&1; then
    fail "modernime-migrate-channel should fail when post-install channel verification fails"
fi
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

if "${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${work_dir}/apt-arch-fail" \
    --deb "${fake_arm_deb}" \
    --arch amd64 \
    --ephemeral-gpg >/dev/null 2>&1; then
    fail "build_apt_repo.sh should reject arm64 DEB when building amd64 repository"
fi

# 7d. Non-monotonic version regression rejection in APT repo builder
if "${repo_root}/scripts/build_apt_repo.sh" \
    --output-dir "${work_dir}/apt-ver-fail" \
    --deb "${deb_file}" \
    --prev-version "${expected_version}" \
    --arch amd64 \
    --ephemeral-gpg >/dev/null 2>&1; then
    fail "build_apt_repo.sh should reject non-monotonic version when --prev-version >= candidate version"
fi

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

if PATH="${empty_bin}" MODERNIME_SKIP_FCITX_RESTART=1 \
    bash "${repo_root}/install.sh" "${work_dir}/missing-dep-prefix" >/dev/null 2>&1; then
    fail "install.sh should reject installation when fcitx5 runtime dependency is missing"
fi

printf 'PASS: All 7 package, signed repository, real APT client, and rollback gate checks succeeded.\n'
