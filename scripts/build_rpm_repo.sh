#!/usr/bin/env bash
# build_rpm_repo.sh — Generates a signed openEuler 24.09 x86_64 RPM/DNF repository for ModernIME
# Enforces distro/architecture isolation, signs all RPM packages (gpgcheck=1) as well as
# repomd.xml.asc (repo_gpgcheck=1), supports persistent GPG keys and key rotation,
# and generates modernime-openeuler-24.09.repo.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

output_dir=""
distro_id="oe2409"
target_arch="x86_64"
repo_url="https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/rpm"
gpg_home="${GNUPGHOME:-}"
gpg_key_id="${MODERNIME_GPG_KEY_ID:-}"
gpg_private_key_file=""
rotation_key_id="${MODERNIME_GPG_ROTATION_KEY_ID:-}"
trusted_pubkeys_dir="${project_root}/packaging/keys/trusted-pubkeys.d"
ephemeral_gpg=false
declare -a rpm_files=()

usage() {
    cat <<'EOF'
Usage:
  build_rpm_repo.sh --output-dir DIR --rpm FILE [--rpm FILE]...
                    [--distro oe2409] [--arch x86_64]
                    [--gpg-home DIR] [--gpg-key-id ID] [--gpg-private-key-file FILE]
                    [--rotation-key-id ID] [--trusted-pubkeys-dir DIR] [--ephemeral-gpg]
                    [--repo-url URL]
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
        --gpg-private-key-file=*)
            gpg_private_key_file="${1#*=}"
            shift
            ;;
        --gpg-private-key-file)
            gpg_private_key_file="$2"
            shift 2
            ;;
        --rotation-key-id=*)
            rotation_key_id="${1#*=}"
            shift
            ;;
        --rotation-key-id)
            rotation_key_id="$2"
            shift 2
            ;;
        --trusted-pubkeys-dir=*)
            trusted_pubkeys_dir="${1#*=}"
            shift
            ;;
        --trusted-pubkeys-dir)
            trusted_pubkeys_dir="$2"
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

if [[ -z "$output_dir" ]]; then
    printf 'Error: --output-dir is required\n' >&2
    exit 1
fi
if [[ ${#rpm_files[@]} -eq 0 ]]; then
    printf 'Error: At least one --rpm FILE is required\n' >&2
    exit 1
fi

for cmd in python3 gpg gpgv sha256sum gzip; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        printf 'Error: Required tool not found: %s\n' "$cmd" >&2
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

if [[ -n "$gpg_private_key_file" || -n "${MODERNIME_GPG_PRIVATE_KEY:-}" ]]; then
    cleanup_gpg=$(mktemp -d "${TMPDIR:-/tmp}/modernime-rpm-gpg.XXXXXX")
    chmod 700 "$cleanup_gpg"
    gpg_home="$cleanup_gpg"
    export GNUPGHOME="$gpg_home"
    if [[ -n "$gpg_private_key_file" ]]; then
        gpg --batch --quiet --import "$gpg_private_key_file"
    else
        printf '%s\n' "$MODERNIME_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
    fi
    if [[ -n "${MODERNIME_GPG_ROTATION_PRIVATE_KEY:-}" ]]; then
        printf '%s\n' "$MODERNIME_GPG_ROTATION_PRIVATE_KEY" | gpg --batch --quiet --import
        if [[ -z "$rotation_key_id" ]]; then
            rotation_key_id=$(gpg --batch --with-colons --list-secret-keys | awk -F: '/^sec:/ {id=$5} END {print id}')
        fi
    fi
    if [[ -z "$gpg_key_id" ]]; then
        gpg_key_id=$(gpg --batch --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
    fi
elif [[ "$ephemeral_gpg" == true || "${MODERNIME_GENERATE_EPHEMERAL_GPG:-0}" == "1" ]]; then
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
    if [[ -z "$gpg_key_id" ]]; then
        gpg_key_id=$(gpg --batch --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
    fi
fi

if [[ -n "$gpg_key_id" && -d "$trusted_pubkeys_dir" ]]; then
    for pub_asc in "$trusted_pubkeys_dir"/*.asc; do
        [[ -f "$pub_asc" ]] || continue
        gpg --batch --quiet --import "$pub_asc" || true
    done
fi

rm -rf -- "$output_dir"
repo_subdir="$output_dir/$distro_id/$target_arch"
mkdir -p "$repo_subdir/Packages" "$repo_subdir/repodata" "$output_dir/client"

# Inspect, validate, sign (gpgcheck=1), and generate repodata for all RPM files
python3 - "$repo_subdir" "$target_arch" "$distro_id" "$gpg_key_id" "${rpm_files[@]}" <<'PYEOF'
import gzip
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import xml.sax.saxutils as saxutils

repo_subdir = Path(sys.argv[1]).resolve()
target_arch = sys.argv[2]
distro_id = sys.argv[3]
gpg_key_id = sys.argv[4]
rpm_inputs = [Path(p) for p in sys.argv[5:]]

RPM_LEAD_MAGIC = b"\xed\xab\xee\xdb"
RPM_HDR_MAGIC = b"\x8e\xad\xe8\x01"

# RPM Signature Header Tag IDs
RPMSIGTAG_PGP = 259     # BIN: OpenPGP detached signature over Header + Payload
RPMSIGTAG_RSA = 268     # BIN: OpenPGP detached signature over Header only
RPMSIGTAG_SHA1 = 269    # STRING: SHA1 hex of Header
RPMSIGTAG_SHA256 = 273  # STRING: SHA256 hex of Header
RPMSIGTAG_SIZE = 1000   # INT32: Header + Payload size
RPMSIGTAG_MD5 = 1004    # BIN (16 bytes): MD5 of Header + Payload

# RPM Main Header Tag IDs
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
        if dtype in (6, 9):  # STRING / I18NSTRING
            end = data.index(b"\x00", pos)
            tags[tag] = data[pos:end].decode("utf-8", errors="replace")
        elif dtype == 7:  # BIN
            tags[tag] = data[pos : pos + count]
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


def build_header_block(entries):
    # entries: list of (tag, dtype, value)
    # dtype: 4=INT32, 6=STRING, 7=BIN (bytes), 8=STRING_ARRAY
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


def gpg_detached_binary_sign(payload_bytes: bytes, key_id: str) -> bytes:
    proc = subprocess.run(
        ["gpg", "--batch", "--yes", "-u", key_id, "--digest-algo", "SHA256", "-bs"],
        input=payload_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return proc.stdout


def sign_rpm_file(rpm_path: Path, key_id: str) -> None:
    if not key_id:
        return
    if shutil.which("rpmsign"):
        gpg_path = os.environ.get("GNUPGHOME", os.path.expanduser("~/.gnupg"))
        res = subprocess.run(
            [
                "rpmsign",
                "--define",
                f"_gpg_name {key_id}",
                "--define",
                f"_gpg_path {gpg_path}",
                "--addsign",
                str(rpm_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if res.returncode == 0:
            return

    raw = rpm_path.read_bytes()
    if len(raw) < 96 or raw[:4] != RPM_LEAD_MAGIC:
        raise ValueError(f"Not a valid RPM file: {rpm_path}")
    lead = raw[:96]
    _, sig_end = parse_rpm_header_block(raw, 96)
    hdr_offset = (sig_end + 7) & ~7
    if raw[hdr_offset : hdr_offset + 4] != RPM_HDR_MAGIC:
        idx = raw.find(RPM_HDR_MAGIC, sig_end)
        if idx < 0:
            raise ValueError(f"Main RPM header not found in {rpm_path}")
        hdr_offset = idx
    _, payload_offset = parse_rpm_header_block(raw, hdr_offset)
    main_hdr = raw[hdr_offset:payload_offset]
    payload = raw[payload_offset:]

    rsa_sig = gpg_detached_binary_sign(main_hdr, key_id)
    pgp_sig = gpg_detached_binary_sign(main_hdr + payload, key_id)
    sha1_hex = hashlib.sha1(main_hdr).hexdigest()
    sha256_hex = hashlib.sha256(main_hdr).hexdigest()
    md5_digest = hashlib.md5(main_hdr + payload).digest()
    total_hp_size = len(main_hdr) + len(payload)

    new_sig_hdr = build_header_block([
        (RPMSIGTAG_PGP, 7, pgp_sig),
        (RPMSIGTAG_RSA, 7, rsa_sig),
        (RPMSIGTAG_SHA1, 6, sha1_hex),
        (RPMSIGTAG_SHA256, 6, sha256_hex),
        (RPMSIGTAG_SIZE, 4, total_hp_size),
        (RPMSIGTAG_MD5, 7, md5_digest),
    ])
    if len(new_sig_hdr) % 8 != 0:
        new_sig_hdr += b"\x00" * (8 - (len(new_sig_hdr) % 8))

    rpm_path.write_bytes(lead + new_sig_hdr + main_hdr + payload)


def inspect_rpm(rpm_path: Path):
    raw = rpm_path.read_bytes()
    if len(raw) < 96 or raw[:4] != RPM_LEAD_MAGIC:
        raise ValueError(f"Not a valid RPM file (bad lead magic): {rpm_path}")
    sig_tags, sig_end = parse_rpm_header_block(raw, 96)
    hdr_offset = (sig_end + 7) & ~7
    if raw[hdr_offset : hdr_offset + 4] != RPM_HDR_MAGIC:
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

    return {
        "name": tags.get(RPMTAG_NAME, "modernime"),
        "version": tags.get(RPMTAG_VERSION, ""),
        "release": tags.get(RPMTAG_RELEASE, "1"),
        "summary": tags.get(RPMTAG_SUMMARY, "ModernIME Fcitx5 Pinyin Input Method"),
        "description": tags.get(RPMTAG_DESCRIPTION, "ModernIME Fcitx5 Pinyin Input Method"),
        "buildtime": int(tags.get(RPMTAG_BUILDTIME, int(time.time()))),
        "installed_size": int(tags.get(RPMTAG_SIZE, 0)),
        "license": tags.get(RPMTAG_LICENSE, "LGPL-2.1-or-later AND MIT"),
        "group": tags.get(RPMTAG_GROUP, "Applications/System"),
        "url": tags.get(RPMTAG_URL, ""),
        "os": tags.get(RPMTAG_OS, "linux"),
        "arch": tags.get(RPMTAG_ARCH, ""),
        "sourcerpm": tags.get(RPMTAG_SOURCERPM, ""),
        "provides": tags.get(RPMTAG_PROVIDENAME, ["modernime"]),
        "requires": list(zip(req_names, req_flags, req_vers)),
        "files": files,
        "sha256": sha256,
        "package_size": len(raw),
        "header_start": hdr_offset,
        "header_end": payload_offset,
        "has_gpg_sig": (RPMSIGTAG_PGP in sig_tags and RPMSIGTAG_RSA in sig_tags),
    }


pkgs = []
seen_versions = set()
for rpm_in in rpm_inputs:
    if not rpm_in.is_file():
        sys.stderr.write(f"Error: RPM file not found: {rpm_in}\n")
        sys.exit(1)
    pre_meta = inspect_rpm(rpm_in)
    if not pre_meta["name"] or not pre_meta["version"] or not pre_meta["arch"]:
        sys.stderr.write(f"Error: Incomplete RPM header metadata in {rpm_in}\n")
        sys.exit(1)
    if pre_meta["arch"] not in (target_arch, "noarch"):
        sys.stderr.write(
            f'Error: RPM architecture mismatch for {rpm_in}: package is "{pre_meta["arch"]}", repository requires "{target_arch}"\n'
        )
        sys.exit(1)
    if "fc" in pre_meta["release"] and distro_id.startswith("oe"):
        sys.stderr.write(
            f'Error: Distro mismatch: refusing Fedora package ({pre_meta["release"]}) in openEuler ({distro_id}) repository\n'
        )
        sys.exit(1)
    ver_key = f'{pre_meta["version"]}-{pre_meta["release"]}'
    if ver_key in seen_versions:
        sys.stderr.write(f'Error: Duplicate RPM version-release "{ver_key}"\n')
        sys.exit(1)
    seen_versions.add(ver_key)

    base_name = f'{pre_meta["name"]}-{pre_meta["version"]}-{pre_meta["release"]}.{pre_meta["arch"]}.rpm'
    dest_rpm = repo_subdir / "Packages" / base_name
    shutil.copy2(rpm_in, dest_rpm)
    # Sign the RPM package itself so DNF gpgcheck=1 succeeds
    if gpg_key_id:
        sign_rpm_file(dest_rpm, gpg_key_id)
    meta = inspect_rpm(dest_rpm)
    if gpg_key_id and not meta["has_gpg_sig"]:
        sys.stderr.write(f"Error: Failed to embed OpenPGP package signature in {dest_rpm}\n")
        sys.exit(1)
    meta["filename"] = base_name
    meta["href"] = f"Packages/{base_name}"
    pkgs.append(meta)

# Always build deterministic XML repodata matching the signed RPMs
now_ts = int(time.time())
primary_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<metadata xmlns="http://linux.duke.edu/metadata/common" xmlns:rpm="http://linux.duke.edu/metadata/rpm" packages="{len(pkgs)}">',
]
filelists_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<filelists xmlns="http://linux.duke.edu/metadata/filelists" packages="{len(pkgs)}">',
]
other_lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<otherdata xmlns="http://linux.duke.edu/metadata/other" packages="{len(pkgs)}">',
]

for p in pkgs:
    esc_name = saxutils.escape(p["name"])
    esc_arch = saxutils.escape(p["arch"])
    esc_ver = saxutils.escape(p["version"])
    esc_rel = saxutils.escape(p["release"])
    esc_sum = saxutils.escape(p["summary"])
    esc_desc = saxutils.escape(p["description"])
    esc_lic = saxutils.escape(p["license"])
    esc_grp = saxutils.escape(p["group"])
    esc_url = saxutils.escape(p["url"])
    esc_href = saxutils.escape(p["href"])

    primary_lines.extend([
        '  <package type="rpm">',
        f"    <name>{esc_name}</name>",
        f"    <arch>{esc_arch}</arch>",
        f'    <version epoch="0" ver="{esc_ver}" rel="{esc_rel}"/>',
        f'    <checksum type="sha256" pkgid="YES">{p["sha256"]}</checksum>',
        f"    <summary>{esc_sum}</summary>",
        f"    <description>{esc_desc}</description>",
        "    <packager>ModernIME Maintainers</packager>",
        f"    <url>{esc_url}</url>",
        f'    <time file="{now_ts}" build="{p["buildtime"]}"/>',
        f'    <size package="{p["package_size"]}" installed="{p["installed_size"]}" archive="{p["installed_size"]}"/>',
        f'    <location href="{esc_href}"/>',
        "    <format>",
        f"      <rpm:license>{esc_lic}</rpm:license>",
        "      <rpm:vendor>ModernIME Community</rpm:vendor>",
        f"      <rpm:group>{esc_grp}</rpm:group>",
        f'      <rpm:header-range start="{p["header_start"]}" end="{p["header_end"]}"/>',
        "      <rpm:provides>",
    ])
    for prov in p["provides"]:
        primary_lines.append(
            f'        <rpm:entry name="{saxutils.escape(prov)}" flags="EQ" epoch="0" ver="{esc_ver}" rel="{esc_rel}"/>'
        )
    primary_lines.append("      </rpm:provides>")
    if p["requires"]:
        primary_lines.append("      <rpm:requires>")
        for rname, rflag, rver in p["requires"]:
            if rname.startswith("rpmlib("):
                continue
            if rver:
                primary_lines.append(
                    f'        <rpm:entry name="{saxutils.escape(rname)}" flags="GE" epoch="0" ver="{saxutils.escape(rver)}"/>'
                )
            else:
                primary_lines.append(f'        <rpm:entry name="{saxutils.escape(rname)}"/>')
        primary_lines.append("      </rpm:requires>")
    primary_lines.extend([
        "    </format>",
        "  </package>",
    ])

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
    local_sign_args=(--batch --yes --digest-algo SHA256 -u "$gpg_key_id")
    if [[ -n "$rotation_key_id" && "$rotation_key_id" != "$gpg_key_id" ]]; then
        local_sign_args+=(-u "$rotation_key_id")
    fi
    gpg "${local_sign_args[@]}" -abs \
        -o "$repo_subdir/repodata/repomd.xml.asc" "$repo_subdir/repodata/repomd.xml"
    gpg --batch --yes --armor --export > "$output_dir/RPM-GPG-KEY-modernime"
    gpg --batch --yes --export > "$output_dir/RPM-GPG-KEY-modernime.gpg"
    {
        printf '# ModernIME RPM Repository Keyring Fingerprints\n'
        gpg --batch --with-colons --fingerprint | awk -F: '
            /^pub:/ { keyid=$5 }
            /^fpr:/ && keyid != "" { printf "KeyID: %s\nFingerprint: %s\n", keyid, $10; keyid="" }
        '
        printf 'Keyring-SHA256: %s\n' "$(sha256sum "$output_dir/RPM-GPG-KEY-modernime" | awk '{print $1}')"
    } > "$output_dir/KEY-FINGERPRINTS.txt"

    # Verify repomd.xml.asc signature (repo_gpgcheck=1)
    gpgv --keyring "$output_dir/RPM-GPG-KEY-modernime.gpg" \
        "$repo_subdir/repodata/repomd.xml.asc" \
        "$repo_subdir/repodata/repomd.xml" >/dev/null 2>&1

    # Verify every RPM package's embedded OpenPGP signatures (gpgcheck=1)
    python3 - "$repo_subdir/Packages" "$output_dir/RPM-GPG-KEY-modernime.gpg" <<'PYVERIFY'
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

pkgs_dir = Path(sys.argv[1])
keyring_path = Path(sys.argv[2])
RPM_HDR_MAGIC = b"\x8e\xad\xe8\x01"

def parse_hdr(data: bytes, offset: int):
    nindex, hsize = struct.unpack(">II", data[offset + 8 : offset + 16])
    idx_start = offset + 16
    store_start = idx_start + nindex * 16
    store_end = store_start + hsize
    tags = {}
    for i in range(nindex):
        eo = idx_start + i * 16
        tag, dtype, voff, count = struct.unpack(">IIII", data[eo : eo + 16])
        if dtype == 7:
            tags[tag] = data[store_start + voff : store_start + voff + count]
    return tags, store_end

for rpm_file in sorted(pkgs_dir.glob("*.rpm")):
    raw = rpm_file.read_bytes()
    sig_tags, sig_end = parse_hdr(raw, 96)
    hdr_off = (sig_end + 7) & ~7
    if raw[hdr_off : hdr_off + 4] != RPM_HDR_MAGIC:
        hdr_off = raw.find(RPM_HDR_MAGIC, sig_end)
    _, payload_off = parse_hdr(raw, hdr_off)
    main_hdr = raw[hdr_off:payload_off]
    payload = raw[payload_off:]

    if 268 not in sig_tags or 259 not in sig_tags:
        raise SystemExit(f"Error: {rpm_file} is missing RPMSIGTAG_RSA (268) or RPMSIGTAG_PGP (259)")

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        hdr_f = td_path / "header.bin"
        hdr_sig = td_path / "header.sig"
        hp_f = td_path / "header_payload.bin"
        hp_sig = td_path / "header_payload.sig"

        hdr_f.write_bytes(main_hdr)
        hdr_sig.write_bytes(sig_tags[268])
        hp_f.write_bytes(main_hdr + payload)
        hp_sig.write_bytes(sig_tags[259])

        subprocess.run(
            ["gpgv", "--keyring", str(keyring_path), str(hdr_sig), str(hdr_f)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        subprocess.run(
            ["gpgv", "--keyring", str(keyring_path), str(hp_sig), str(hp_f)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
PYVERIFY
fi

sed \
    -e "s|@MODERNIME_RPM_REPO_URL@|$repo_url|g" \
    "$project_root/packaging/rpm/modernime-openeuler.repo.in" > "$output_dir/client/modernime-openeuler-24.09.repo"

(
    cd "$output_dir"
    find "$distro_id" client -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)

printf 'RPM repository generated at: %s\n' "$output_dir"
