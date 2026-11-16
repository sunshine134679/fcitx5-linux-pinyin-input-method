#!/usr/bin/env bash
# build_pacman_repo.sh — Generates a signed Arch Linux (Pacman) repository for ModernIME
# Produces signed .pkg.tar.zst.sig, modernime.db(.tar.gz)[.sig], modernime.files(.tar.gz)[.sig],
# modernime-arch-keyring.{gpg,asc}, KEY-FINGERPRINTS.txt, and client/modernime-pacman.conf.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

output_dir=""
repo_url="https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/arch"
target_arch="x86_64"
gpg_home="${GNUPGHOME:-}"
gpg_key_id="${MODERNIME_GPG_KEY_ID:-}"
gpg_private_key_file=""
gpg_passphrase="${MODERNIME_GPG_PASSPHRASE:-}"
rotation_key_id="${MODERNIME_GPG_ROTATION_KEY_ID:-}"
trusted_pubkeys_dir="${project_root}/packaging/keys/trusted-pubkeys.d"
ephemeral_gpg=false
preserve_output=false
declare -a pkg_files=()

usage() {
    cat <<'EOF'
Usage:
  build_pacman_repo.sh --output-dir DIR --pkg FILE [--pkg FILE]...
                       [--arch x86_64|aarch64] [--preserve-output]
                       [--gpg-home DIR] [--gpg-key-id ID] [--gpg-private-key-file FILE]
                       [--rotation-key-id ID] [--trusted-pubkeys-dir DIR] [--ephemeral-gpg]
                       [--repo-url URL]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir=*) output_dir="${1#*=}"; shift ;;
        --output-dir) output_dir="$2"; shift 2 ;;
        --pkg=*) pkg_files+=("${1#*=}"); shift ;;
        --pkg) pkg_files+=("$2"); shift 2 ;;
        --arch=*) target_arch="${1#*=}"; shift ;;
        --arch) target_arch="$2"; shift 2 ;;
        --gpg-home=*) gpg_home="${1#*=}"; shift ;;
        --gpg-home) gpg_home="$2"; shift 2 ;;
        --gpg-key-id=*) gpg_key_id="${1#*=}"; shift ;;
        --gpg-key-id) gpg_key_id="$2"; shift 2 ;;
        --gpg-private-key-file=*) gpg_private_key_file="${1#*=}"; shift ;;
        --gpg-private-key-file) gpg_private_key_file="$2"; shift 2 ;;
        --gpg-passphrase=*) gpg_passphrase="${1#*=}"; shift ;;
        --gpg-passphrase) gpg_passphrase="$2"; shift 2 ;;
        --rotation-key-id=*) rotation_key_id="${1#*=}"; shift ;;
        --rotation-key-id) rotation_key_id="$2"; shift 2 ;;
        --trusted-pubkeys-dir=*) trusted_pubkeys_dir="${1#*=}"; shift ;;
        --trusted-pubkeys-dir) trusted_pubkeys_dir="$2"; shift 2 ;;
        --ephemeral-gpg) ephemeral_gpg=true; shift ;;
        --preserve-output) preserve_output=true; shift ;;
        --repo-url=*) repo_url="${1#*=}"; shift ;;
        --repo-url) repo_url="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Error: Unknown option: %s\n' "$1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$output_dir" ]]; then
    printf 'Error: --output-dir is required\n' >&2
    exit 1
fi
if [[ ${#pkg_files[@]} -eq 0 ]]; then
    printf 'Error: At least one --pkg FILE is required\n' >&2
    exit 1
fi

for cmd in python3 zstd tar gpg gpgv sha256sum; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        printf 'Error: Required Pacman repo tool not found: %s\n' "$cmd" >&2
        exit 1
    fi
done

unset DISPLAY WAYLAND_DISPLAY
export GPG_TTY=""

cleanup_gpg=""
cleanup() {
    if [[ -n "$cleanup_gpg" && -d "$cleanup_gpg" ]]; then
        gpgconf --homedir "$cleanup_gpg" --kill gpg-agent >/dev/null 2>&1 || true
        rm -rf -- "$cleanup_gpg"
    fi
}
trap cleanup EXIT

init_isolated_gpg_home() {
    cleanup_gpg=$(mktemp -d "${TMPDIR:-/tmp}/modernime-arch-gpg.XXXXXX")
    chmod 700 "$cleanup_gpg"
    cat > "$cleanup_gpg/gpg-agent.conf" <<'EOF'
default-cache-ttl 0
max-cache-ttl 0
allow-loopback-pinentry
pinentry-program /bin/false
EOF
    chmod 600 "$cleanup_gpg/gpg-agent.conf"
    gpg_home="$cleanup_gpg"
    export GNUPGHOME="$gpg_home"
}

if [[ -n "$gpg_private_key_file" || -n "${MODERNIME_GPG_PRIVATE_KEY:-}" ]]; then
    init_isolated_gpg_home
    import_flags=(--batch --no-tty --quiet --pinentry-mode loopback --passphrase "${gpg_passphrase:-}")
    if [[ -n "$gpg_private_key_file" ]]; then
        gpg "${import_flags[@]}" --import "$gpg_private_key_file"
    else
        printf '%s\n' "$MODERNIME_GPG_PRIVATE_KEY" | gpg "${import_flags[@]}" --import
    fi
    if [[ -n "${MODERNIME_GPG_ROTATION_PRIVATE_KEY:-}" ]]; then
        printf '%s\n' "$MODERNIME_GPG_ROTATION_PRIVATE_KEY" | gpg "${import_flags[@]}" --import
        if [[ -z "$rotation_key_id" ]]; then
            rotation_key_id=$(gpg --batch --no-tty --with-colons --list-secret-keys | awk -F: '/^sec:/ {id=$5} END {print id}')
        fi
    fi
    if [[ -z "$gpg_key_id" ]]; then
        gpg_key_id=$(gpg --batch --no-tty --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
    fi
    gpg-connect-agent --homedir "$gpg_home" reloadagent /bye >/dev/null 2>&1 || true
elif [[ "$ephemeral_gpg" == true || "${MODERNIME_GENERATE_EPHEMERAL_GPG:-0}" == "1" ]]; then
    init_isolated_gpg_home
    gpg --batch --no-tty --generate-key <<'GPGEOF' 2>/dev/null
%no-protection
Key-Type: RSA
Key-Length: 3072
Name-Real: ModernIME Arch Repository Signing Key
Name-Email: arch-release@modernime.invalid
Expire-Date: 2y
%commit
GPGEOF
    gpg_key_id=$(gpg --batch --no-tty --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
    gpg-connect-agent --homedir "$gpg_home" reloadagent /bye >/dev/null 2>&1 || true
elif [[ -n "$gpg_home" ]]; then
    src_gpg_home="$gpg_home"
    init_isolated_gpg_home
    for item in "$src_gpg_home"/*; do
        [[ -e "$item" ]] || continue
        base_item=$(basename "$item")
        case "$base_item" in
            S.gpg-agent*|*.lock|gpg-agent.conf|.gpg-v21-migrated) continue ;;
        esac
        cp -a "$item" "$cleanup_gpg/"
    done
    chmod -R go-rwx "$cleanup_gpg"
    if [[ -z "$gpg_key_id" ]]; then
        gpg_key_id=$(gpg --batch --no-tty --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
    fi
    gpg-connect-agent --homedir "$gpg_home" reloadagent /bye >/dev/null 2>&1 || true
fi

if [[ -n "$gpg_key_id" && -d "$trusted_pubkeys_dir" ]]; then
    for pub_asc in "$trusted_pubkeys_dir"/*.asc; do
        [[ -f "$pub_asc" ]] || continue
        gpg --batch --no-tty --quiet --import "$pub_asc" || true
    done
fi

if [[ "$preserve_output" == true ]]; then
    rm -rf -- "$output_dir/$target_arch"
else
    rm -rf -- "$output_dir"
fi
repo_subdir="$output_dir/$target_arch"
mkdir -p "$repo_subdir" "$output_dir/client"
export MODERNIME_GPG_PASSPHRASE="$gpg_passphrase"

python3 - "$repo_subdir" "$target_arch" "$gpg_key_id" "${pkg_files[@]}" <<'PYEOF'
import base64
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

repo_subdir = Path(sys.argv[1]).resolve()
target_arch = sys.argv[2]
gpg_key_id = sys.argv[3]
pkg_inputs = [Path(p) for p in sys.argv[4:]]


def gpg_sign_file(src: Path, dst_sig: Path) -> bytes:
    gpg_path = os.environ.get("GNUPGHOME", os.path.expanduser("~/.gnupg"))
    passphrase = os.environ.get("MODERNIME_GPG_PASSPHRASE", "")
    fd, pass_file = tempfile.mkstemp(prefix=".arch-pass-", dir=gpg_path)
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, passphrase.encode("utf-8"))
    finally:
        os.close(fd)
    try:
        cmd = [
            "gpg", "--batch", "--no-tty", "--yes",
            "--homedir", gpg_path,
            "--pinentry-mode", "loopback",
            "--passphrase-file", pass_file,
            "-u", gpg_key_id,
            "--digest-algo", "SHA256",
            "-bs", "-o", str(dst_sig), str(src),
        ]
        subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return dst_sig.read_bytes()
    finally:
        Path(pass_file).unlink(missing_ok=True)


def read_pkg_tar_zst(pkg_path: Path):
    proc = subprocess.run(["zstd", "-dc", str(pkg_path)], capture_output=True, check=True)
    pkginfo = {}
    pkglists = {"depend": [], "optdepend": [], "makedepend": [], "provides": [], "conflict": [], "replaces": [], "license": [], "groups": []}
    files = []
    with tarfile.open(fileobj=io.BytesIO(proc.stdout), mode="r:") as tf:
        for member in tf:
            name = member.name
            while name.startswith("./"):
                name = name[2:]
            if not name:
                continue
            if name == ".PKGINFO":
                text = tf.extractfile(member).read().decode("utf-8", errors="replace")
                for line in text.splitlines():
                    line = line.strip()
                    if not line or line.startswith("#") or " = " not in line:
                        continue
                    k, v = line.split(" = ", 1)
                    if k in pkglists:
                        pkglists[k].append(v)
                    else:
                        pkginfo[k] = v
                continue
            if name.startswith("."):
                continue
            files.append(name + ("/" if member.isdir() and not name.endswith("/") else ""))
    return pkginfo, pkglists, sorted(files)


def build_desc(filename: str, csize: int, sha256_hex: str, pgpsig_b64: str, info: dict, lists: dict) -> str:
    sections = []
    def add(header, values):
        vals = [v for v in values if v]
        if vals:
            sections.append(f"%{header}%\n" + "\n".join(vals))

    add("FILENAME", [filename])
    add("NAME", [info.get("pkgname", "modernime")])
    add("BASE", [info.get("pkgbase") or info.get("pkgname", "modernime")])
    add("VERSION", [info.get("pkgver", "")])
    add("DESC", [info.get("pkgdesc", "")])
    add("GROUPS", lists["groups"])
    add("CSIZE", [str(csize)])
    add("ISIZE", [info.get("size", "0")])
    add("SHA256SUM", [sha256_hex])
    if pgpsig_b64:
        add("PGPSIG", [pgpsig_b64])
    add("URL", [info.get("url", "")])
    add("LICENSE", lists["license"])
    add("ARCH", [info.get("arch", target_arch)])
    add("BUILDDATE", [info.get("builddate", "")])
    add("PACKAGER", [info.get("packager", "ModernIME Maintainers")])
    add("REPLACES", lists["replaces"])
    add("CONFLICTS", lists["conflict"])
    add("PROVIDES", lists["provides"])
    add("DEPENDS", lists["depend"])
    add("OPTDEPENDS", lists["optdepend"])
    add("MAKEDEPENDS", lists["makedepend"])
    return "\n\n".join(sections) + "\n"


seen_vers = set()
db_tar_gz = repo_subdir / "modernime.db.tar.gz"
files_tar_gz = repo_subdir / "modernime.files.tar.gz"

with tarfile.open(db_tar_gz, "w:gz") as db_tf, tarfile.open(files_tar_gz, "w:gz") as files_tf:
    for pkg_in in pkg_inputs:
        if not pkg_in.is_file():
            raise SystemExit(f"Error: Pacman package not found: {pkg_in}")
        info, lists, file_entries = read_pkg_tar_zst(pkg_in)
        pkgname = info.get("pkgname", "")
        pkgver = info.get("pkgver", "")
        pkgarch = info.get("arch", "")
        if not pkgname or not pkgver or not pkgarch:
            raise SystemExit(f"Error: Incomplete .PKGINFO metadata in {pkg_in}")
        if pkgarch not in (target_arch, "any"):
            raise SystemExit(f'Error: Architecture mismatch for {pkg_in}: package is "{pkgarch}", repo requires "{target_arch}"')
        if pkgver in seen_vers:
            raise SystemExit(f'Error: Duplicate package version "{pkgver}" in Arch repository')
        seen_vers.add(pkgver)

        dest_name = f"{pkgname}-{pkgver}-{pkgarch}.pkg.tar.zst"
        dest_pkg = repo_subdir / dest_name
        shutil.copy2(pkg_in, dest_pkg)
        raw = dest_pkg.read_bytes()
        sha256_hex = hashlib.sha256(raw).hexdigest()
        pgpsig_b64 = ""
        if gpg_key_id:
            sig_bytes = gpg_sign_file(dest_pkg, repo_subdir / f"{dest_name}.sig")
            pgpsig_b64 = base64.b64encode(sig_bytes).decode("ascii")

        entry_dir = f"{pkgname}-{pkgver}"
        desc_bytes = build_desc(dest_name, len(raw), sha256_hex, pgpsig_b64, info, lists).encode("utf-8")
        files_bytes = ("%FILES%\n" + "\n".join(file_entries) + "\n").encode("utf-8")

        for tf in (db_tf, files_tf):
            ti_dir = tarfile.TarInfo(entry_dir)
            ti_dir.type = tarfile.DIRTYPE
            ti_dir.mode = 0o755
            tf.addfile(ti_dir)

            ti_desc = tarfile.TarInfo(f"{entry_dir}/desc")
            ti_desc.size = len(desc_bytes)
            ti_desc.mode = 0o644
            tf.addfile(ti_desc, io.BytesIO(desc_bytes))

        ti_files = tarfile.TarInfo(f"{entry_dir}/files")
        ti_files.size = len(files_bytes)
        ti_files.mode = 0o644
        files_tf.addfile(ti_files, io.BytesIO(files_bytes))

shutil.copy2(db_tar_gz, repo_subdir / "modernime.db")
shutil.copy2(files_tar_gz, repo_subdir / "modernime.files")

if gpg_key_id:
    gpg_sign_file(db_tar_gz, repo_subdir / "modernime.db.tar.gz.sig")
    shutil.copy2(repo_subdir / "modernime.db.tar.gz.sig", repo_subdir / "modernime.db.sig")
    gpg_sign_file(files_tar_gz, repo_subdir / "modernime.files.tar.gz.sig")
    shutil.copy2(repo_subdir / "modernime.files.tar.gz.sig", repo_subdir / "modernime.files.sig")
PYEOF

if [[ -n "$gpg_key_id" ]]; then
    gpg --batch --no-tty --yes --armor --export > "$output_dir/modernime-arch-keyring.asc"
    gpg --batch --no-tty --yes --export > "$output_dir/modernime-arch-keyring.gpg"
    {
        printf '# ModernIME Arch Linux Repository Keyring Fingerprints\n'
        gpg --batch --with-colons --fingerprint | awk -F: '
            /^pub:/ { keyid=$5 }
            /^fpr:/ && keyid != "" { printf "KeyID: %s\nFingerprint: %s\n", keyid, $10; keyid="" }
        '
        printf 'Keyring-SHA256: %s\n' "$(sha256sum "$output_dir/modernime-arch-keyring.gpg" | awk '{print $1}')"
    } > "$output_dir/KEY-FINGERPRINTS.txt"

    gpgv --keyring "$output_dir/modernime-arch-keyring.gpg" \
        "$repo_subdir/modernime.db.tar.gz.sig" \
        "$repo_subdir/modernime.db.tar.gz" >/dev/null 2>&1
    gpgv --keyring "$output_dir/modernime-arch-keyring.gpg" \
        "$repo_subdir/modernime.files.tar.gz.sig" \
        "$repo_subdir/modernime.files.tar.gz" >/dev/null 2>&1
    for sig_f in "$repo_subdir"/*.pkg.tar.zst.sig; do
        [[ -f "$sig_f" ]] || continue
        gpgv --keyring "$output_dir/modernime-arch-keyring.gpg" \
            "$sig_f" "${sig_f%.sig}" >/dev/null 2>&1
    done
fi

cat > "$output_dir/client/modernime-pacman.conf" <<EOF
# Append to /etc/pacman.conf (or Include from /etc/pacman.d/modernime.conf)
# Trust the ModernIME signing key first (verify fingerprint via ${repo_url}/KEY-FINGERPRINTS.txt):
#   curl -fsSL ${repo_url}/modernime-arch-keyring.asc | sudo pacman-key --add -
#   sudo pacman-key --lsign-key <FINGERPRINT>
[modernime]
SigLevel = Required DatabaseRequired
Server = ${repo_url}/\$arch
EOF

(
    cd "$output_dir"
    find . -mindepth 1 -maxdepth 1 -type d \( -name 'x86_64' -o -name 'aarch64' -o -name 'client' \) -printf '%P\0' | \
        sort -z | xargs -0 -I{} find {} -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)

printf 'Pacman repository generated at: %s\n' "$output_dir"
