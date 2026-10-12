#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${MODERNIME_RPM_BUILD_DIR:-${project_root}/build/package-rpm}"
dist_dir="${MODERNIME_RPM_DIST_DIR:-${project_root}/dist}"

mkdir -p "${dist_dir}"

printf '==> Configuring ModernIME for RPM packaging (openEuler / Fedora / RHEL)...\n'
cmake -S "${project_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMODERNIME_PACKAGE_CHANNEL=rpm \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
    -DMODERNIME_BUILD_SETTINGS=ON \
    -DMODERNIME_BUILD_TESTS=OFF

printf '==> Building ModernIME...\n'
cmake --build "${build_dir}" --parallel "$(nproc 2>/dev/null || echo 2)"

if command -v rpmbuild >/dev/null 2>&1; then
    printf '==> Generating RPM package with CPack...\n'
    (
        cd "${build_dir}"
        cpack -G RPM
    )
    cp -f "${build_dir}"/*.rpm "${dist_dir}/"
else
    printf '==> rpmbuild not found on host; staging install tree and building RPM v3 binary package...\n'
    stage_dir="${build_dir}/_rpm_stage"
    rm -rf "${stage_dir}"
    DESTDIR="${stage_dir}" cmake --install "${build_dir}"
    python3 - "${project_root}" "${stage_dir}" "${dist_dir}" <<'PYEOF'
import gzip
import hashlib
import io
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

project_root = Path(sys.argv[1]).resolve()
stage_dir = Path(sys.argv[2]).resolve()
dist_dir = Path(sys.argv[3]).resolve()

cmake_text = (project_root / "CMakeLists.txt").read_text(encoding="utf-8")
m = re.search(r"project\(ModernIME\s+VERSION\s+([0-9.]+)", cmake_text)
version = m.group(1) if m else "0.1.9"
release = os.environ.get("MODERNIME_RPM_RELEASE", "1.oe2409")
arch = os.environ.get("MODERNIME_RPM_ARCH", "x86_64")
pkg_name = "modernime"
license_str = "Apache-2.0 AND LGPL-2.1-or-later AND CC-BY-SA-3.0 AND BSD-3-Clause AND MIT"
summary_str = "Modern Chinese Pinyin Input Method for Fcitx5"
desc_str = "ModernIME Chinese Pinyin Input Method for Fcitx5 with GTK3 settings client, LibIME dictionaries, and transactional SQLite/LMDB learning store."
url_str = "https://github.com/Mapped/ModernIME"

# Collect staged files
dirnames = []
dir_map = {}
dirindexes = []
basenames = []
total_size = 0

all_files = sorted(p for p in stage_dir.rglob("*") if p.is_file() or p.is_symlink())
if not all_files:
    raise SystemExit("ERROR: RPM stage directory is empty")

for fpath in all_files:
    rel = "/" + fpath.relative_to(stage_dir).as_posix()
    dname = os.path.dirname(rel)
    if not dname.endswith("/"):
        dname += "/"
    bname = os.path.basename(rel)
    if dname not in dir_map:
        dir_map[dname] = len(dirnames)
        dirnames.append(dname)
    dirindexes.append(dir_map[dname])
    basenames.append(bname)
    if fpath.is_file() and not fpath.is_symlink():
        total_size += fpath.stat().st_size

# Build cpio.gz payload (deterministic newc archive via cpio if available, or gzip tar/cpio stream)
rel_paths = ["./" + p.relative_to(stage_dir).as_posix() for p in all_files]
cpio_proc = subprocess.run(
    ["cpio", "-o", "-H", "newc", "--quiet"],
    cwd=str(stage_dir),
    input=("\n".join(rel_paths) + "\n").encode("utf-8"),
    stdout=subprocess.PIPE,
    check=True,
)
payload_gz = gzip.compress(cpio_proc.stdout, compresslevel=6)

def build_header_block(entries):
    # entries: list of (tag, dtype, value)
    # dtype: 4=INT32, 6/9=STRING, 7=BIN, 8=STRING_ARRAY
    entries = sorted(entries, key=lambda x: x[0])
    idx_buf = bytearray()
    store_buf = bytearray()
    for tag, dtype, val in entries:
        if dtype == 4:
            if len(store_buf) % 4 != 0:
                store_buf.extend(b"\x00" * (4 - (len(store_buf) % 4)))
            vals = val if isinstance(val, list) else [val]
            off = len(store_buf)
            for v in vals:
                store_buf.extend(struct.pack(">I", int(v)))
            idx_buf.extend(struct.pack(">IIII", tag, 4, off, len(vals)))
        elif dtype in (6, 9):
            off = len(store_buf)
            b = val.encode("utf-8") + b"\x00"
            store_buf.extend(b)
            idx_buf.extend(struct.pack(">IIII", tag, dtype, off, 1))
        elif dtype == 7:
            off = len(store_buf)
            b = bytes(val)
            store_buf.extend(b)
            idx_buf.extend(struct.pack(">IIII", tag, 7, off, len(b)))
        elif dtype == 8:
            off = len(store_buf)
            for s in val:
                store_buf.extend(s.encode("utf-8") + b"\x00")
            idx_buf.extend(struct.pack(">IIII", tag, 8, off, len(val)))
    magic = b"\x8e\xad\xe8\x01\x00\x00\x00\x00"
    counts = struct.pack(">II", len(entries), len(store_buf))
    return magic + counts + bytes(idx_buf) + bytes(store_buf)

prein_script = (project_root / "packaging/rpm/preinst.sh").read_text(encoding="utf-8")
postin_script = (project_root / "packaging/rpm/postinst.sh").read_text(encoding="utf-8")
postun_script = (project_root / "packaging/rpm/postun.sh").read_text(encoding="utf-8")

# RPMSENSE_GREATER | RPMSENSE_EQUAL = (1 << 2) | (1 << 3) = 12
GE = 12
ANY = 0
req_list = [
    ("fcitx5", GE, "5.0.14"),
    ("libime", GE, "1.0.11"),
    ("gtk3", GE, "3.24.0"),
    ("pango", GE, "1.50.0"),
    ("cairo", GE, "1.16.0"),
    ("sqlite-libs", GE, "3.37.0"),
    ("lmdb-libs", GE, "0.9.24"),
    ("python3", GE, "3.9"),
    ("desktop-file-utils", ANY, ""),
    ("hicolor-icon-theme", ANY, ""),
    ("/bin/sh", ANY, ""),
]

hdr_entries = [
    (1000, 6, pkg_name),                                  # RPMTAG_NAME
    (1001, 6, version),                                   # RPMTAG_VERSION
    (1002, 6, release),                                   # RPMTAG_RELEASE
    (1004, 9, summary_str),                               # RPMTAG_SUMMARY
    (1005, 9, desc_str),                                  # RPMTAG_DESCRIPTION
    (1006, 4, int(time.time())),                          # RPMTAG_BUILDTIME
    (1009, 4, total_size),                                # RPMTAG_SIZE
    (1014, 6, license_str),                               # RPMTAG_LICENSE
    (1016, 9, "System Environment/Libraries"),            # RPMTAG_GROUP
    (1020, 6, url_str),                                   # RPMTAG_URL
    (1021, 6, "linux"),                                   # RPMTAG_OS
    (1022, 6, arch),                                      # RPMTAG_ARCH
    (1023, 6, prein_script),                              # RPMTAG_PREIN
    (1024, 6, postin_script),                             # RPMTAG_POSTIN
    (1026, 6, postun_script),                             # RPMTAG_POSTUN
    (1044, 6, f"{pkg_name}-{version}-{release}.src.rpm"), # RPMTAG_SOURCERPM
    (1047, 8, [pkg_name, f"{pkg_name}(x86-64)"]),         # RPMTAG_PROVIDENAME
    (1048, 4, [r[1] for r in req_list]),                  # RPMTAG_REQUIREFLAGS
    (1049, 8, [r[0] for r in req_list]),                  # RPMTAG_REQUIRENAME
    (1050, 8, [r[2] for r in req_list]),                  # RPMTAG_REQUIREVERSION
    (1116, 4, dirindexes),                                # RPMTAG_DIRINDEXES
    (1117, 8, basenames),                                 # RPMTAG_BASENAMES
    (1118, 8, dirnames),                                  # RPMTAG_DIRNAMES
    (1124, 6, "cpio"),                                    # RPMTAG_PAYLOADFORMAT
    (1125, 6, "gzip"),                                    # RPMTAG_PAYLOADCOMPRESSOR
    (5092, 8, [hashlib.sha256(payload_gz).hexdigest()]),  # RPMTAG_PAYLOADDIGEST
    (5093, 4, 8),                                         # RPMTAG_PAYLOADDIGESTALGO (8 = SHA256)
]

main_hdr = build_header_block(hdr_entries)
sig_hdr = build_header_block([
    (269, 6, hashlib.sha1(main_hdr).hexdigest()),                 # RPMSIGTAG_SHA1 (40 hex chars)
    (273, 6, hashlib.sha256(main_hdr).hexdigest()),               # RPMSIGTAG_SHA256 (64 hex chars)
    (1000, 4, len(main_hdr) + len(payload_gz)),                   # RPMSIGTAG_SIZE
    (1004, 7, hashlib.md5(main_hdr + payload_gz).digest()),        # RPMSIGTAG_MD5 (16 bytes)
])
if len(sig_hdr) % 8 != 0:
    sig_hdr += b"\x00" * (8 - (len(sig_hdr) % 8))

# 96-byte RPM Lead
lead_name = f"{pkg_name}-{version}-{release}".encode("utf-8")[:65]
lead_name = lead_name + b"\x00" * (66 - len(lead_name))
lead = (
    b"\xed\xab\xee\xdb"
    + struct.pack(">BBHH", 3, 0, 0, 1)
    + lead_name
    + struct.pack(">HH", 1, 5)
    + b"\x00" * 16
)
assert len(lead) == 96

out_rpm = dist_dir / f"{pkg_name}-{version}-{release}.{arch}.rpm"
out_rpm.write_bytes(lead + sig_hdr + main_hdr + payload_gz)
PYEOF
fi

printf '==> RPM package generated in %s:\n' "${dist_dir}"
ls -lh "${dist_dir}"/*.rpm
