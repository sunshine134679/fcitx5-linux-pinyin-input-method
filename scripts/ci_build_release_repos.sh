#!/usr/bin/env bash
# ci_build_release_repos.sh — End-to-end builder for ModernIME APT & RPM/DNF static repositories
# Used by both .github/workflows/release.yml and tests/verify_package_repo_gate.sh to ensure
# identical artifact filename resolution, multi-version retention, GPG signing, and Pages layout.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

assets_dir=""
pages_dir=""
previous_pages_dir=""
require_persistent_gpg=false
ephemeral_gpg=false
gpg_private_key_file=""
gpg_passphrase="${MODERNIME_GPG_PASSPHRASE:-}"
repo_base_url="https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method"

usage() {
    cat <<'EOF'
Usage:
  ci_build_release_repos.sh --assets-dir DIR --pages-dir DIR
                            [--previous-pages-dir DIR]
                            [--require-persistent-gpg | --ephemeral-gpg]
                            [--gpg-private-key-file FILE]
                            [--repo-base-url URL]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --assets-dir=*)
            assets_dir="${1#*=}"
            shift
            ;;
        --assets-dir)
            assets_dir="$2"
            shift 2
            ;;
        --pages-dir=*)
            pages_dir="${1#*=}"
            shift
            ;;
        --pages-dir)
            pages_dir="$2"
            shift 2
            ;;
        --previous-pages-dir=*)
            previous_pages_dir="${1#*=}"
            shift
            ;;
        --previous-pages-dir)
            previous_pages_dir="$2"
            shift 2
            ;;
        --require-persistent-gpg)
            require_persistent_gpg=true
            shift
            ;;
        --ephemeral-gpg)
            ephemeral_gpg=true
            shift
            ;;
        --gpg-private-key-file=*)
            gpg_private_key_file="${1#*=}"
            shift
            ;;
        --gpg-private-key-file)
            gpg_private_key_file="$2"
            shift 2
            ;;
        --gpg-passphrase=*)
            gpg_passphrase="${1#*=}"
            shift
            ;;
        --gpg-passphrase)
            gpg_passphrase="$2"
            shift 2
            ;;
        --repo-base-url=*)
            repo_base_url="${1#*=}"
            shift
            ;;
        --repo-base-url)
            repo_base_url="$2"
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

if [[ -z "$assets_dir" || -z "$pages_dir" ]]; then
    printf 'Error: --assets-dir and --pages-dir are required\n' >&2
    exit 1
fi

if [[ "$require_persistent_gpg" == true ]]; then
    if [[ -z "${MODERNIME_GPG_PRIVATE_KEY:-}" && -z "$gpg_private_key_file" && -z "${MODERNIME_GPG_KEY_ID:-}" ]]; then
        printf 'Error: Official release requires persistent GPG signing key (MODERNIME_GPG_PRIVATE_KEY or --gpg-private-key-file).\n' >&2
        printf 'Refusing to publish repository with an ephemeral GPG key that would break client upgrades.\n' >&2
        exit 1
    fi
fi

# Discover DEB artifacts (Ubuntu 24.04 / noble only; 22.04 support removed)
find_first_match() {
    local dir=$1
    shift
    for pat in "$@"; do
        local found
        found=$(find "$dir" -maxdepth 1 -type f -name "$pat" 2>/dev/null | sort | head -n 1 || true)
        if [[ -n "$found" ]]; then
            printf '%s\n' "$found"
            return 0
        fi
    done
    return 1
}

noble_deb=$(find_first_match "$assets_dir" "modernime_*_amd64_ubuntu24.04.deb" "*ubuntu24.04.deb" || true)

if [[ -z "$noble_deb" ]]; then
    printf 'Error: Could not locate canonical *ubuntu24.04.deb package in %s\n' "$assets_dir" >&2
    exit 1
fi

rpm_pkg=$(find_first_match "$assets_dir" "modernime-*.oe24.x86_64.rpm" "*.rpm" || true)
if [[ -z "$rpm_pkg" ]]; then
    printf 'Error: Could not locate openEuler RPM package in %s\n' "$assets_dir" >&2
    exit 1
fi

declare -a apt_args=(
    --output-dir "$pages_dir/apt"
    --repo-url "${repo_base_url%/}/apt"
    --arch amd64
)
declare -a rpm_args=(
    --output-dir "$pages_dir/rpm"
    --repo-url "${repo_base_url%/}/rpm"
    --distro oe2409
    --arch x86_64
)

# A retry replaces the current version once, while preserving other released versions.
cand_ver=$(dpkg-deb -f "$noble_deb" Version)
if [[ -n "$previous_pages_dir" && -d "$previous_pages_dir" ]]; then
    for old_deb in "$previous_pages_dir"/apt/pool/noble/main/m/modernime/*.deb; do
        [[ -f "$old_deb" ]] || continue
        [[ "$(dpkg-deb -f "$old_deb" Version)" != "$cand_ver" ]] || continue
        apt_args+=("--noble-deb" "$old_deb")
    done
    for old_rpm in "$previous_pages_dir"/rpm/oe2409/x86_64/Packages/*.rpm; do
        [[ -f "$old_rpm" ]] || continue
        [[ "$(basename "$old_rpm")" != modernime-"${cand_ver}"-*.rpm ]] || continue
        rpm_args+=(--rpm "$old_rpm")
    done
fi

apt_args+=(--noble-deb "$noble_deb")
rpm_args+=(--rpm "$rpm_pkg")

if [[ -n "$gpg_private_key_file" ]]; then
    apt_args+=(--gpg-private-key-file "$gpg_private_key_file")
    rpm_args+=(--gpg-private-key-file "$gpg_private_key_file")
elif [[ "$ephemeral_gpg" == true ]]; then
    apt_args+=(--ephemeral-gpg)
    rpm_args+=(--ephemeral-gpg)
fi

if [[ -n "$gpg_passphrase" ]]; then
    apt_args+=(--gpg-passphrase "$gpg_passphrase")
    rpm_args+=(--gpg-passphrase "$gpg_passphrase")
fi

rm -rf -- "$pages_dir"
mkdir -p "$pages_dir"
: > "$pages_dir/.nojekyll"

"$project_root/scripts/build_apt_repo.sh" "${apt_args[@]}"
"$project_root/scripts/build_rpm_repo.sh" "${rpm_args[@]}"

if [[ "$require_persistent_gpg" == true ]]; then
    for key_file in "$pages_dir/apt/modernime-archive-keyring.asc" "$pages_dir/rpm/RPM-GPG-KEY-modernime"; do
        key_meta=$(gpg --batch --with-colons --import-options show-only --import "$key_file" 2>/dev/null || true)
        if printf '%s\n' "$key_meta" | grep -Eiq 'modernime\.invalid|Ephemeral'; then
            printf 'Error: Ephemeral test GPG key detected in %s; refusing production release.\n' "$key_file" >&2
            exit 1
        fi
    done
fi

# Sync the signed current-version RPM from Pages repository back to assets_dir, preserving its canonical filename
signed_pages_rpm=$(find_first_match "$pages_dir/rpm/oe2409/x86_64/Packages" "modernime-${cand_ver}-*.rpm" || true)
if [[ -z "$signed_pages_rpm" || ! -f "$signed_pages_rpm" ]]; then
    printf 'Error: Signed RPM for version %s not found in %s/rpm/oe2409/x86_64/Packages\n' "$cand_ver" "$pages_dir" >&2
    exit 1
fi
cp -f "$signed_pages_rpm" "$rpm_pkg"

# Assert byte-for-byte identity between release-assets RPM and Pages repository RPM
if ! cmp -s "$rpm_pkg" "$signed_pages_rpm"; then
    printf 'Error: Release asset RPM %s is not byte-identical to Pages RPM %s\n' "$rpm_pkg" "$signed_pages_rpm" >&2
    exit 1
fi

# Verify embedded OpenPGP signatures (tags 268 & 259) on the synced release-assets RPM
if [[ -f "$pages_dir/rpm/RPM-GPG-KEY-modernime.gpg" ]]; then
    python3 - "$rpm_pkg" "$pages_dir/rpm/RPM-GPG-KEY-modernime.gpg" "$pages_dir/rpm/KEY-FINGERPRINTS.txt" <<'PYEOF'
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

rpm_path = Path(sys.argv[1]).resolve()
keyring_path = Path(sys.argv[2]).resolve()
fpr_path = Path(sys.argv[3]).resolve()

allowed_fprs = re.findall(r"Fingerprint:\s*([0-9A-Fa-f]{40})", fpr_path.read_text(encoding="utf-8"))
if not allowed_fprs:
    raise SystemExit(f"Error: No valid 40-hex fingerprints found in {fpr_path}")

raw = rpm_path.read_bytes()
if raw[96:100] != b"\x8e\xad\xe8\x01":
    raise SystemExit(f"Error: Invalid RPM signature header magic in {rpm_path}")
nindex, hsize = struct.unpack(">II", raw[104:112])
idx_start = 112
store_start = idx_start + nindex * 16
store_end = store_start + hsize
sig_end = (store_end + 7) & ~7
if raw[sig_end : sig_end + 4] != b"\x8e\xad\xe8\x01":
    sig_end = raw.find(b"\x8e\xad\xe8\x01", store_end)
main_nindex, main_hsize = struct.unpack(">II", raw[sig_end + 8 : sig_end + 16])
main_end = sig_end + 16 + main_nindex * 16 + main_hsize
main_hdr = raw[sig_end:main_end]
hdr_and_payload = raw[sig_end:]

sig_bin = {}
for i in range(nindex):
    eo = idx_start + i * 16
    tag, dtype, voff, count = struct.unpack(">IIII", raw[eo : eo + 16])
    if dtype == 7:
        sig_bin[tag] = raw[store_start + voff : store_start + voff + count]

if 268 not in sig_bin or 259 not in sig_bin:
    raise SystemExit(f"Error: Release asset RPM {rpm_path} missing RPMSIGTAG_RSA (268) or RPMSIGTAG_PGP (259)")

with tempfile.TemporaryDirectory() as td:
    td_p = Path(td)
    (td_p / "hdr.bin").write_bytes(main_hdr)
    (td_p / "hdr.sig").write_bytes(sig_bin[268])
    (td_p / "all.bin").write_bytes(hdr_and_payload)
    (td_p / "all.sig").write_bytes(sig_bin[259])
    for sig_f, data_f in [("hdr.sig", "hdr.bin"), ("all.sig", "all.bin")]:
        res = subprocess.run(
            ["gpgv", "--keyring", str(keyring_path), "--status-fd", "1", str(td_p / sig_f), str(td_p / data_f)],
            capture_output=True,
            text=True,
            check=True,
        )
        if not any(f"VALIDSIG {fpr}" in res.stdout for fpr in allowed_fprs):
            raise SystemExit(
                f"Error: Signature {sig_f} on {rpm_path} did not match any fingerprint in {allowed_fprs}"
            )
PYEOF
fi

# Copy client configs and keyrings into assets_dir for Release attachment convenience
cp -f "$pages_dir/apt/modernime-archive-keyring.gpg" "$assets_dir/"
cp -f "$pages_dir/apt/modernime-archive-keyring.asc" "$assets_dir/"
cp -f "$pages_dir/apt/KEY-FINGERPRINTS.txt" "$assets_dir/"
cp -f "$pages_dir/apt/client/modernime-noble.sources" "$assets_dir/"
cp -f "$pages_dir/apt/client/99-modernime.pref" "$assets_dir/"
cp -f "$pages_dir/rpm/RPM-GPG-KEY-modernime" "$assets_dir/"
cp -f "$pages_dir/rpm/client/modernime-openeuler-24.09.repo" "$assets_dir/"

printf 'Static Pages repository tree ready at: %s\n' "$pages_dir"
