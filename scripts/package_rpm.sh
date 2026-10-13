#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${MODERNIME_RPM_BUILD_DIR:-${project_root}/build/package-rpm}"
dist_dir="${MODERNIME_RPM_DIST_DIR:-${project_root}/dist}"

mkdir -p "${dist_dir}"

if [ "${MODERNIME_REQUIRE_NATIVE_RPM_TOOLS:-0}" = "1" ]; then
    if [ "${MODERNIME_FORCE_FALLBACK_RPM:-0}" = "1" ] || ! command -v rpmbuild >/dev/null 2>&1; then
        printf 'Error: MODERNIME_REQUIRE_NATIVE_RPM_TOOLS=1 is set, but native rpmbuild is unavailable or fallback was forced\n' >&2
        exit 1
    fi
fi

printf '==> Configuring ModernIME for RPM packaging (openEuler / Fedora / RHEL)...\n'
cmake -S "${project_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_SO_NO_EXE=0 \
    -DMODERNIME_PACKAGE_CHANNEL=rpm \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
    -DMODERNIME_BUILD_SETTINGS=ON \
    -DMODERNIME_BUILD_TESTS=OFF

printf '==> Building ModernIME...\n'
cmake --build "${build_dir}" --parallel "$(nproc 2>/dev/null || echo 2)"

if [ "${MODERNIME_FORCE_FALLBACK_RPM:-0}" != "1" ] && command -v rpmbuild >/dev/null 2>&1; then
    printf '==> Generating RPM package with native CPack / rpmbuild...\n'
    rm -f "${build_dir}"/*.rpm "${dist_dir}"/modernime-*.rpm
    (
        cd "${build_dir}"
        cpack -G RPM
    )
    version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "${project_root}/CMakeLists.txt")
    release="${MODERNIME_RPM_RELEASE:-1.oe2409}"
    arch="${MODERNIME_RPM_ARCH:-x86_64}"
    cpack_rpm=$(ls -1 "${build_dir}"/*.rpm | head -n 1)
    cp -f "${cpack_rpm}" "${dist_dir}/modernime-${version}-${release}.${arch}.rpm"
else
    if [ "${MODERNIME_REQUIRE_NATIVE_RPM_TOOLS:-0}" = "1" ]; then
        printf 'Error: MODERNIME_REQUIRE_NATIVE_RPM_TOOLS=1 is set, but native rpmbuild is unavailable or fallback was forced\n' >&2
        exit 1
    fi
    printf '==> Staging install tree and building deterministic RPM v4 binary package (fallback)...\n'
    rm -f "${dist_dir}"/modernime-*.rpm
    stage_dir="${build_dir}/_rpm_stage"
    rm -rf "${stage_dir}"
    DESTDIR="${stage_dir}" cmake --install "${build_dir}"
    python3 - "${project_root}" "${stage_dir}" "${dist_dir}" <<'PYEOF'
import gzip
import hashlib
import os
from pathlib import Path
import re
import stat
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
license_str = "LGPL-2.1-or-later AND MIT"
summary_str = "Modern Fcitx5 Pinyin Input Method for Linux Desktops"
desc_str = "ModernIME Chinese Pinyin Input Method for Fcitx5 with GTK3 settings client, LibIME dictionaries, and transactional SQLite/LMDB learning store."
url_str = "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method"

# Collect staged files and populate complete per-file metadata required by native rpm/rpmkeys/dnf
dirnames = []
dir_map = {}
dirindexes = []
basenames = []
filesizes = []
filemodes = []
filerdevs = []
filemtimes = []
filedigests = []
filelinktos = []
fileflags = []
fileusernames = []
filegroupnames = []
filedevices = []
fileinodes = []
filelangs = []
total_size = 0

for dpath in stage_dir.rglob("*"):
    if dpath.is_dir() and not dpath.is_symlink():
        os.chmod(dpath, 0o755)

all_files = sorted(p for p in stage_dir.rglob("*") if p.is_file() or p.is_symlink())
if not all_files:
    raise SystemExit("ERROR: RPM stage directory is empty")

for idx, fpath in enumerate(all_files, start=1):
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

    st = fpath.lstat()
    if fpath.is_symlink():
        fsize = len(os.readlink(fpath))
        fmode = stat.S_IFLNK | 0o777
        fdigest = ""
        flink = os.readlink(fpath)
    else:
        is_exec = rel.startswith("/usr/bin/") or rel.endswith(".so")
        perm = 0o755 if is_exec else 0o644
        os.chmod(fpath, perm)
        st = fpath.lstat()
        fsize = int(st.st_size)
        fmode = stat.S_IFREG | perm
        fdigest = hashlib.sha256(fpath.read_bytes()).hexdigest()
        flink = ""
        total_size += fsize

    filesizes.append(fsize)
    filemodes.append(fmode & 0xFFFF)
    filerdevs.append(0)
    filemtimes.append(int(st.st_mtime))
    filedigests.append(fdigest)
    filelinktos.append(flink)
    # RPMFILE_DOC = (1 << 1) = 2 for documentation files under /usr/share/doc/
    fileflags.append(2 if rel.startswith("/usr/share/doc/") else 0)
    fileusernames.append("root")
    filegroupnames.append("root")
    filedevices.append(1)
    fileinodes.append(idx)
    filelangs.append("")

# Build cpio.gz payload (deterministic newc archive with root:root ownership)
rel_paths = ["./" + p.relative_to(stage_dir).as_posix() for p in all_files]
cpio_cmd = ["cpio", "-o", "-H", "newc", "-R", "0:0", "--quiet"]
cpio_proc = subprocess.run(
    cpio_cmd,
    cwd=str(stage_dir),
    input=("\n".join(rel_paths) + "\n").encode("utf-8"),
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
if cpio_proc.returncode != 0:
    cpio_proc = subprocess.run(
        ["cpio", "-o", "-H", "newc", "--quiet"],
        cwd=str(stage_dir),
        input=("\n".join(rel_paths) + "\n").encode("utf-8"),
        stdout=subprocess.PIPE,
        check=True,
    )
payload_gz = gzip.compress(cpio_proc.stdout, compresslevel=6)

def build_header_block(entries, region_tag=None):
    # entries: list of (tag, dtype, value)
    # dtype: 3=INT16, 4=INT32, 6/9=STRING, 7=BIN, 8=STRING_ARRAY
    entries = sorted([e for e in entries if e[0] not in (62, 63)], key=lambda x: x[0])
    idx_buf = bytearray()
    store_buf = bytearray()
    for tag, dtype, val in entries:
        if dtype == 3:
            if len(store_buf) % 2 != 0:
                store_buf.extend(b"\x00")
            vals = val if isinstance(val, list) else [val]
            off = len(store_buf)
            for v in vals:
                store_buf.extend(struct.pack(">H", int(v) & 0xFFFF))
            idx_buf.extend(struct.pack(">IIII", tag, 3, off, len(vals)))
        elif dtype == 4:
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
    total_entries = len(entries)
    if region_tag is not None:
        total_entries += 1
        region_off = len(store_buf)
        trailer = struct.pack(">IIiI", region_tag, 7, -(total_entries * 16), 16)
        region_idx = struct.pack(">IIII", region_tag, 7, region_off, 16)
        idx_buf = bytearray(region_idx) + idx_buf
        store_buf.extend(trailer)
    magic = b"\x8e\xad\xe8\x01\x00\x00\x00\x00"
    counts = struct.pack(">II", total_entries, len(store_buf))
    return magic + counts + bytes(idx_buf) + bytes(store_buf)

prein_script = (project_root / "packaging/rpm/preinst.sh").read_text(encoding="utf-8")
postin_script = (project_root / "packaging/rpm/postinst.sh").read_text(encoding="utf-8")
postun_script = (project_root / "packaging/rpm/postun.sh").read_text(encoding="utf-8")

# RPMSENSE_GREATER | RPMSENSE_EQUAL = (1 << 2) | (1 << 3) = 12
GE = 12
EQ = 8
ANY = 0
req_list = [
    ("fcitx5", GE, "5.0.14"),
    ("libime", GE, "1.0.11"),
    ("gtk3", GE, "3.24.0"),
    ("pango", GE, "1.44.0"),
    ("cairo", GE, "1.16.0"),
    ("sqlite", GE, "3.31.0"),
    ("sqlite-libs", GE, "3.37.0"),
    ("lmdb-libs", GE, "0.9.24"),
    ("python3", GE, "3.10"),
    ("coreutils", ANY, ""),
    ("desktop-file-utils", ANY, ""),
    ("hicolor-icon-theme", ANY, ""),
    ("/bin/sh", ANY, ""),
]

hdr_entries = [
    (100, 8, ["C"]),                                       # RPMTAG_HEADERI18NTABLE
    (1000, 6, pkg_name),                                   # RPMTAG_NAME
    (1001, 6, version),                                    # RPMTAG_VERSION
    (1002, 6, release),                                    # RPMTAG_RELEASE
    (1004, 9, summary_str),                                # RPMTAG_SUMMARY
    (1005, 9, desc_str),                                   # RPMTAG_DESCRIPTION
    (1006, 4, int(time.time())),                           # RPMTAG_BUILDTIME
    (1009, 4, total_size),                                 # RPMTAG_SIZE
    (1014, 6, license_str),                                # RPMTAG_LICENSE
    (1016, 9, "Applications/System"),                      # RPMTAG_GROUP
    (1020, 6, url_str),                                    # RPMTAG_URL
    (1021, 6, "linux"),                                    # RPMTAG_OS
    (1022, 6, arch),                                       # RPMTAG_ARCH
    (1023, 6, prein_script),                               # RPMTAG_PREIN
    (1024, 6, postin_script),                              # RPMTAG_POSTIN
    (1026, 6, postun_script),                              # RPMTAG_POSTUN
    (1028, 4, filesizes),                                  # RPMTAG_FILESIZES
    (1030, 3, filemodes),                                  # RPMTAG_FILEMODES (INT16)
    (1033, 3, filerdevs),                                  # RPMTAG_FILERDEVS (INT16)
    (1034, 4, filemtimes),                                 # RPMTAG_FILEMTIMES
    (1035, 8, filedigests),                                # RPMTAG_FILEDIGESTS
    (1036, 8, filelinktos),                                # RPMTAG_FILELINKTOS
    (1037, 4, fileflags),                                  # RPMTAG_FILEFLAGS
    (1039, 8, fileusernames),                              # RPMTAG_FILEUSERNAME
    (1040, 8, filegroupnames),                             # RPMTAG_FILEGROUPNAME
    (1044, 6, f"{pkg_name}-{version}-{release}.src.rpm"),  # RPMTAG_SOURCERPM
    (1047, 8, [pkg_name, f"{pkg_name}(x86-64)"]),          # RPMTAG_PROVIDENAME
    (1048, 4, [r[1] for r in req_list]),                   # RPMTAG_REQUIREFLAGS
    (1049, 8, [r[0] for r in req_list]),                   # RPMTAG_REQUIRENAME
    (1050, 8, [r[2] for r in req_list]),                   # RPMTAG_REQUIREVERSION
    (1085, 6, "/bin/sh"),                                  # RPMTAG_PREINPROG
    (1086, 6, "/bin/sh"),                                  # RPMTAG_POSTINPROG
    (1088, 6, "/bin/sh"),                                  # RPMTAG_POSTUNPROG
    (1095, 4, filedevices),                                # RPMTAG_FILEDEVICES
    (1096, 4, fileinodes),                                 # RPMTAG_FILEINODES
    (1097, 8, filelangs),                                  # RPMTAG_FILELANGS
    (1112, 4, [EQ, EQ]),                                   # RPMTAG_PROVIDEFLAGS
    (1113, 8, [f"{version}-{release}", f"{version}-{release}"]),  # RPMTAG_PROVIDEVERSION
    (1116, 4, dirindexes),                                 # RPMTAG_DIRINDEXES
    (1117, 8, basenames),                                  # RPMTAG_BASENAMES
    (1118, 8, dirnames),                                   # RPMTAG_DIRNAMES
    (1124, 6, "cpio"),                                     # RPMTAG_PAYLOADFORMAT
    (1125, 6, "gzip"),                                     # RPMTAG_PAYLOADCOMPRESSOR
    (5011, 4, 8),                                          # RPMTAG_FILEDIGESTALGO (8 = SHA256)
    (5092, 8, [hashlib.sha256(payload_gz).hexdigest()]),   # RPMTAG_PAYLOADDIGEST
    (5093, 4, 8),                                          # RPMTAG_PAYLOADDIGESTALGO (8 = SHA256)
]

main_hdr = build_header_block(hdr_entries, region_tag=63)
sig_hdr = build_header_block([
    (269, 6, hashlib.sha1(main_hdr).hexdigest()),                 # RPMSIGTAG_SHA1 (40 hex chars)
    (273, 6, hashlib.sha256(main_hdr).hexdigest()),               # RPMSIGTAG_SHA256 (64 hex chars)
    (1000, 4, len(main_hdr) + len(payload_gz)),                   # RPMSIGTAG_SIZE
    (1004, 7, hashlib.md5(main_hdr + payload_gz).digest()),        # RPMSIGTAG_MD5 (16 bytes)
], region_tag=62)
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
