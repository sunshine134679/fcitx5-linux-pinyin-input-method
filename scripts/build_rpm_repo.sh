#!/usr/bin/env bash
# build_rpm_repo.sh — Generates a signed openEuler 24.09 x86_64 RPM/DNF repository for ModernIME
# Enforces distro/architecture isolation, generates standard repodata (repomd.xml, primary.xml.gz,
# filelists.xml.gz, other.xml.gz), signs repomd.xml.asc, and generates modernime-openeuler-24.09.repo.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

output_dir=""
distro_id="oe2409"
target_arch="x86_64"
repo_url="https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/rpm"
gpg_home="${GNUPGHOME:-}"
gpg_key_id="${MODERNIME_GPG_KEY_ID:-}"
ephemeral_gpg=false
declare -a rpm_files=()

usage() {
    cat <<'EOF'
Usage:
  build_rpm_repo.sh --output-dir DIR --rpm FILE [--rpm FILE]...
                    [--distro oe2409] [--arch x86_64]
                    [--gpg-home DIR] [--gpg-key-id ID] [--ephemeral-gpg] [--repo-url URL]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir=*)
            output_dir="${1#*=}"
            shift
            ;;
        --output-dir)
            output_dir="$2"
            shift 2
            ;;
        --rpm=*)
            rpm_files+=("${1#*=}")
            shift
            ;;
        --rpm)
            rpm_files+=("$2")
            shift 2
            ;;
        --distro=*)
            distro_id="${1#*=}"
            shift
            ;;
        --distro)
            distro_id="$2"
            shift 2
            ;;
        --arch=*)
            target_arch="${1#*=}"
            shift
            ;;
        --arch)
            target_arch="$2"
            shift 2
            ;;
        --gpg-home=*)
            gpg_home="${1#*=}"
            shift
            ;;
        --gpg-home)
            gpg_home="$2"
            shift 2
            ;;
        --gpg-key-id=*)
            gpg_key_id="${1#*=}"
            shift
            ;;
        --gpg-key-id)
            gpg_key_id="$2"
            shift 2
            ;;
        --ephemeral-gpg)
            ephemeral_gpg=true
            shift
            ;;
        --repo-url=*)
            repo_url="${1#*=}"
            shift
            ;;
        --repo-url)
            repo_url="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Error: Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$output_dir" || ${#rpm_files[@]} -eq 0 ]]; then
    printf 'Error: --output-dir and at least one --rpm are required\n' >&2
    exit 1
fi

for cmd in python3 gpg gpgv sha256sum; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        printf 'Error: Required RPM repository tool not found: %s\n' "$cmd" >&2
        exit 1
    fi
done

cleanup_gpg=""
cleanup() {
    if [[ -n "$cleanup_gpg" && -d "$cleanup_gpg" ]]; then
        rm -rf -- "$cleanup_gpg"
    fi
}
trap cleanup EXIT

if [[ "$ephemeral_gpg" == true || "${MODERNIME_GENERATE_EPHEMERAL_GPG:-0}" == "1" ]]; then
    cleanup_gpg=$(mktemp -d "${TMPDIR:-/tmp}/modernime-rpm-gpg.XXXXXX")
    chmod 700 "$cleanup_gpg"
    gpg_home="$cleanup_gpg"
    export GNUPGHOME="$gpg_home"
    gpg --batch --generate-key <<'GPGEOF' 2>/dev/null
%no-protection
Key-Type: RSA
Key-Length: 3072
Subkey-Type: RSA
Subkey-Length: 3072
Name-Real: ModernIME RPM Repository Signing Key
Name-Email: rpm-release@modernime.invalid
Expire-Date: 2y
%commit
GPGEOF
    gpg_key_id=$(gpg --batch --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
elif [[ -n "$gpg_home" ]]; then
    export GNUPGHOME="$gpg_home"
fi

rm -rf -- "$output_dir"
repo_subdir="$output_dir/$distro_id/$target_arch"
mkdir -p "$repo_subdir/Packages" "$repo_subdir/repodata" "$output_dir/client"

# Validate RPM headers (magic, architecture, distro compatibility) and generate standard repodata XML
python3 - "$repo_subdir" "$distro_id" "$target_arch" "${rpm_files[@]}" <<'PYEOF'
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
import xml.sax.saxutils as saxutils
from pathlib import Path

RPM_LEAD_MAGIC = b"\xed\xab\xee\xdb"
RPM_HDR_MAGIC = b"\x8e\xad\xe8\x01"

# RPM Header Tag IDs
RPMTAG_NAME = 1000
RPMTAG_VERSION = 1001
RPMTAG_RELEASE = 1002
RPMTAG_SUMMARY = 1004
RPMTAG_DESCRIPTION = 1005
RPMTAG_BUILDTIME = 1006
RPMTAG_SIZE = 1009
RPMTAG_LICENSE = 1014
RPMTAG_GROUP = 1016
RPMTAG_URL = 1020
RPMTAG_OS = 1021
RPMTAG_ARCH = 1022
RPMTAG_SOURCERPM = 1044
RPMTAG_PROVIDENAME = 1047
RPMTAG_REQUIREFLAGS = 1048
RPMTAG_REQUIRENAME = 1049
RPMTAG_REQUIREVERSION = 1050
RPMTAG_DIRINDEXES = 1116
RPMTAG_BASENAMES = 1117
RPMTAG_DIRNAMES = 1118


def parse_rpm_header_block(data: bytes, offset: int):
    if data[offset : offset + 4] != RPM_HDR_MAGIC:
        raise ValueError(f"Invalid RPM header magic at offset {offset}")
    nindex, hsize = struct.unpack(">II", data[offset + 8 : offset + 16])
    idx_start = offset + 16
    store_start = idx_start + nindex * 16
    store_end = store_start + hsize
    if store_end > len(data):
        raise ValueError("Truncated RPM header block")

    tags = {}
    for i in range(nindex):
        entry_off = idx_start + i * 16
        tag, dtype, val_off, count = struct.unpack(">IIII", data[entry_off : entry_off + 16])
        pos = store_start + val_off
        if dtype == 6:  # STRING
            end = data.index(b"\x00", pos)
            tags[tag] = data[pos:end].decode("utf-8", errors="replace")
        elif dtype == 9:  # I18NSTRING
            end = data.index(b"\x00", pos)
            tags[tag] = data[pos:end].decode("utf-8", errors="replace")
        elif dtype == 8:  # STRING_ARRAY
            arr = []
            cur = pos
            for _ in range(count):
                end = data.index(b"\x00", cur)
                arr.append(data[cur:end].decode("utf-8", errors="replace"))
                cur = end + 1
            tags[tag] = arr
        elif dtype == 4:  # INT32
            vals = list(struct.unpack(f">{count}I", data[pos : pos + 4 * count]))
            tags[tag] = vals if count > 1 else vals[0]
    return tags, store_end


def inspect_rpm(rpm_path: Path):
    raw = rpm_path.read_bytes()
    if len(raw) < 96 or raw[:4] != RPM_LEAD_MAGIC:
        raise ValueError(f"Not a valid RPM file (bad lead magic): {rpm_path}")
    # Signature header starts at offset 96
    _, sig_end = parse_rpm_header_block(raw, 96)
    # Align sig_end to 8-byte boundary
    hdr_offset = (sig_end + 7) & ~7
    if raw[hdr_offset : hdr_offset + 4] != RPM_HDR_MAGIC:
        # Fallback search for second header magic if unpadded
        idx = raw.find(RPM_HDR_MAGIC, sig_end)
        if idx < 0:
            raise ValueError(f"Main RPM header not found in {rpm_path}")
        hdr_offset = idx
    tags, payload_offset = parse_rpm_header_block(raw, hdr_offset)
    sha256 = hashlib.sha256(raw).hexdigest()

    dirindexes = tags.get(RPMTAG_DIRINDEXES, [])
    if isinstance(dirindexes, int):
        dirindexes = [dirindexes]
    basenames = tags.get(RPMTAG_BASENAMES, [])
    dirnames = tags.get(RPMTAG_DIRNAMES, [])
    files = []
    for idx_val, bname in zip(dirindexes, basenames):
        dname = dirnames[idx_val] if 0 <= idx_val < len(dirnames) else ""
        files.append(f"{dname}{bname}")

    req_names = tags.get(RPMTAG_REQUIRENAME, [])
    req_vers = tags.get(RPMTAG_REQUIREVERSION, [""] * len(req_names))
    req_flags = tags.get(RPMTAG_REQUIREFLAGS, [0] * len(req_names))
    if isinstance(req_flags, int):
        req_flags = [req_flags]

    requires = []
    for rname, rver, rflag in zip(req_names, req_vers, req_flags):
        if rname.startswith("rpmlib("):
            continue
        rel = ""
        if (rflag & 0x02) and (rflag & 0x08):
            rel = "LE"
        elif (rflag & 0x04) and (rflag & 0x08):
            rel = "GE"
        elif rflag & 0x08:
            rel = "EQ"
        elif rflag & 0x04:
            rel = "GT"
        elif rflag & 0x02:
            rel = "LT"
        requires.append((rname, rel, rver))

    provides = tags.get(RPMTAG_PROVIDENAME, [tags.get(RPMTAG_NAME, "modernime")])

    return {
        "filename": rpm_path.name,
        "sha256": sha256,
        "file_size": len(raw),
        "header_start": hdr_offset,
        "header_end": payload_offset,
        "name": tags.get(RPMTAG_NAME, "modernime"),
        "version": tags.get(RPMTAG_VERSION, "0.1.8"),
        "release": tags.get(RPMTAG_RELEASE, "1"),
        "summary": tags.get(RPMTAG_SUMMARY, ""),
        "description": tags.get(RPMTAG_DESCRIPTION, ""),
        "buildtime": int(tags.get(RPMTAG_BUILDTIME, int(time.time()))),
        "installed_size": int(tags.get(RPMTAG_SIZE, len(raw))),
        "license": tags.get(RPMTAG_LICENSE, "LGPL-2.1-or-later AND MIT"),
        "group": tags.get(RPMTAG_GROUP, "Applications/System"),
        "url": tags.get(RPMTAG_URL, ""),
        "os": tags.get(RPMTAG_OS, "linux"),
        "arch": tags.get(RPMTAG_ARCH, ""),
        "sourcerpm": tags.get(RPMTAG_SOURCERPM, ""),
        "files": files,
        "requires": requires,
        "provides": provides,
    }


repo_subdir = Path(sys.argv[1])
distro_id = sys.argv[2]
target_arch = sys.argv[3]
rpm_Args = [Path(p) for p in sys.argv[4:]]

pkgs = []
for rpm_p in rpm_Args:
    info = inspect_rpm(rpm_p)
    if info["arch"] not in (target_arch, "noarch"):
        sys.stderr.write(
            f"Error: RPM architecture mismatch for {rpm_p}: header arch='{info['arch']}', repository requires '{target_arch}'\n"
        )
        sys.exit(1)
    # Prevent copying incompatible distro packages (e.g. .fc40 or .el8 into oe2409)
    rel = info["release"]
    if distro_id.startswith("oe") and any(bad in rel for bad in (".fc", ".el7", ".el8", ".ubuntu")):
        sys.stderr.write(
            f"Error: Distro release mismatch for {rpm_p}: release='{rel}' is incompatible with '{distro_id}'\n"
        )
        sys.exit(1)
    dst_rpm = repo_subdir / "Packages" / rpm_p.name
    shutil.copy2(rpm_p, dst_rpm)
    info["location"] = f"Packages/{rpm_p.name}"
    pkgs.append(info)

createrepo_bin = shutil.which("createrepo_c") or shutil.which("createrepo")
if createrepo_bin:
    subprocess.run([createrepo_bin, str(repo_subdir)], check=True)
    sys.exit(0)

# Native standard RPM repodata XML generator (primary.xml.gz, filelists.xml.gz, other.xml.gz, repomd.xml)
now_ts = int(time.time())
count = len(pkgs)

primary_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<metadata xmlns="http://linux.duke.edu/metadata/common" xmlns:rpm="http://linux.duke.edu/metadata/rpm" packages="{count}">',
]
filelists_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<filelists xmlns="http://linux.duke.edu/metadata/filelists" packages="{count}">',
]
other_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<otherdata xmlns="http://linux.duke.edu/metadata/other" packages="{count}">',
]

for p in pkgs:
    esc_name = saxutils.escape(str(p["name"]))
    esc_arch = saxutils.escape(str(p["arch"]))
    esc_ver = saxutils.escape(str(p["version"]))
    esc_rel = saxutils.escape(str(p["release"]))
    esc_sum = saxutils.escape(str(p["summary"]))
    esc_desc = saxutils.escape(str(p["description"]))
    esc_lic = saxutils.escape(str(p["license"]))
    esc_grp = saxutils.escape(str(p["group"]))
    esc_url = saxutils.escape(str(p["url"]))
    esc_loc = saxutils.escape(str(p["location"]))

    primary_lines.extend([
        '  <package type="rpm">',
        f"    <name>{esc_name}</name>",
        f"    <arch>{esc_arch}</arch>",
        f'    <version epoch="0" ver="{esc_ver}" rel="{esc_rel}"/>',
        f'    <checksum type="sha256" pkgid="YES">{p["sha256"]}</checksum>',
        f"    <summary>{esc_sum}</summary>",
        f"    <description>{esc_desc}</description>",
        f"    <packager>ModernIME Maintainers</packager>",
        f"    <url>{esc_url}</url>",
        f'    <time file="{now_ts}" build="{p["buildtime"]}"/>',
        f'    <size package="{p["file_size"]}" installed="{p["installed_size"]}" archive="{p["installed_size"]}"/>',
        f'    <location href="{esc_loc}"/>',
        "    <format>",
        f"      <rpm:license>{esc_lic}</rpm:license>",
        f"      <rpm:group>{esc_grp}</rpm:group>",
        f'      <rpm:header-range start="{p["header_start"]}" end="{p["header_end"]}"/>',
        "      <rpm:provides>",
    ])
    for prov in p["provides"]:
        primary_lines.append(
            f'        <rpm:entry name="{saxutils.escape(str(prov))}" flags="EQ" epoch="0" ver="{esc_ver}" rel="{esc_rel}"/>'
        )
    primary_lines.append("      </rpm:provides>")
    if p["requires"]:
        primary_lines.append("      <rpm:requires>")
        for rname, rflag, rver in p["requires"]:
            if rflag and rver:
                primary_lines.append(
                    f'        <rpm:entry name="{saxutils.escape(str(rname))}" flags="{rflag}" epoch="0" ver="{saxutils.escape(str(rver))}"/>'
                )
            else:
                primary_lines.append(
                    f'        <rpm:entry name="{saxutils.escape(str(rname))}"/>'
                )
        primary_lines.append("      </rpm:requires>")
    for fpath in p["files"]:
        if fpath.startswith("/usr/bin/") or fpath.startswith("/bin/"):
            primary_lines.append(f"      <file>{saxutils.escape(fpath)}</file>")
    primary_lines.extend(["    </format>", "  </package>"])

    filelists_lines.extend([
        f'  <package pkgid="{p["sha256"]}" name="{esc_name}" arch="{esc_arch}">',
        f'    <version epoch="0" ver="{esc_ver}" rel="{esc_rel}"/>',
    ])
    for fpath in p["files"]:
        filelists_lines.append(f"    <file>{saxutils.escape(fpath)}</file>")
    filelists_lines.append("  </package>")

    other_lines.extend([
        f'  <package pkgid="{p["sha256"]}" name="{esc_name}" arch="{esc_arch}">',
        f'    <version epoch="0" ver="{esc_ver}" rel="{esc_rel}"/>',
        f'    <changelog author="ModernIME Maintainers" date="{p["buildtime"]}">- Release {esc_ver}-{esc_rel}</changelog>',
        "  </package>",
    ])

primary_lines.append("</metadata>")
filelists_lines.append("</filelists>")
other_lines.append("</otherdata>")

repodata_dir = repo_subdir / "repodata"
repomd_entries = []

for dtype, lines in [
    ("primary", primary_lines),
    ("filelists", filelists_lines),
    ("other", other_lines),
]:
    xml_bytes = ("\n".join(lines) + "\n").encode("utf-8")
    open_sha = hashlib.sha256(xml_bytes).hexdigest()
    open_size = len(xml_bytes)
    gz_bytes = gzip.compress(xml_bytes, compresslevel=9, mtime=now_ts)
    gz_sha = hashlib.sha256(gz_bytes).hexdigest()
    gz_size = len(gz_bytes)
    fname = f"{gz_sha}-{dtype}.xml.gz"
    (repodata_dir / fname).write_bytes(gz_bytes)
    # Also create convenience symlink/copy primary.xml.gz etc.
    (repodata_dir / f"{dtype}.xml.gz").write_bytes(gz_bytes)
    repomd_entries.append((dtype, fname, gz_sha, gz_size, open_sha, open_size))

repomd_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<repomd xmlns="http://linux.duke.edu/metadata/repo" xmlns:rpm="http://linux.duke.edu/metadata/rpm">',
    f"  <revision>{now_ts}</revision>",
]
for dtype, fname, gz_sha, gz_size, open_sha, open_size in repomd_entries:
    repomd_lines.extend([
        f'  <data type="{dtype}">',
        f'    <checksum type="sha256">{gz_sha}</checksum>',
        f'    <open-checksum type="sha256">{open_sha}</open-checksum>',
        f'    <location href="repodata/{fname}"/>',
        f"    <timestamp>{now_ts}</timestamp>",
        f"    <size>{gz_size}</size>",
        f"    <open-size>{open_size}</open-size>",
        "  </data>",
    ])
repomd_lines.append("</repomd>")
(repodata_dir / "repomd.xml").write_text("\n".join(repomd_lines) + "\n", encoding="utf-8")
PYEOF

if [[ -n "$gpg_key_id" ]]; then
    gpg --batch --yes --default-key "$gpg_key_id" --digest-algo SHA256 -abs \
        -o "$repo_subdir/repodata/repomd.xml.asc" "$repo_subdir/repodata/repomd.xml"
    gpg --batch --yes --armor --export "$gpg_key_id" > "$output_dir/RPM-GPG-KEY-modernime"
    gpg --batch --yes --export "$gpg_key_id" > "$output_dir/RPM-GPG-KEY-modernime.gpg"
    gpgv --keyring "$output_dir/RPM-GPG-KEY-modernime.gpg" \
        "$repo_subdir/repodata/repomd.xml.asc" \
        "$repo_subdir/repodata/repomd.xml" >/dev/null 2>&1
fi

sed \
    -e "s|@MODERNIME_RPM_REPO_URL@|$repo_url|g" \
    "$project_root/packaging/rpm/modernime-openeuler.repo.in" > "$output_dir/client/modernime-openeuler-24.09.repo"

(
    cd "$output_dir"
    find "$distro_id" client -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)

printf 'RPM repository generated at: %s\n' "$output_dir"
