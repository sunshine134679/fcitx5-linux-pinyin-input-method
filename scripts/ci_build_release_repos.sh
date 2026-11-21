#!/usr/bin/env bash
# ci_build_release_repos.sh — End-to-end builder for ModernIME APT, RPM/DNF, and Arch/Pacman static repositories
# Used by both .github/workflows/release.yml and tests/verify_package_repo_gate.sh to ensure
# identical artifact filename resolution, multi-version retention, GPG signing, and Pages layout.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

assets_dir=""
pages_dir=""
previous_pages_dir=""
retain_versions="${MODERNIME_RETAIN_VERSIONS:-3}"
max_repo_mib="${MODERNIME_MAX_REPO_MIB:-900}"
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
                            [--retain-versions N]
                            [--max-repo-mib N]
                            [--require-persistent-gpg | --ephemeral-gpg]
                            [--gpg-private-key-file FILE]
                            [--repo-base-url URL]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --assets-dir=*) assets_dir="${1#*=}"; shift ;;
        --assets-dir) assets_dir="$2"; shift 2 ;;
        --pages-dir=*) pages_dir="${1#*=}"; shift ;;
        --pages-dir) pages_dir="$2"; shift 2 ;;
        --previous-pages-dir=*) previous_pages_dir="${1#*=}"; shift ;;
        --previous-pages-dir) previous_pages_dir="$2"; shift 2 ;;
        --retain-versions=*) retain_versions="${1#*=}"; shift ;;
        --retain-versions) retain_versions="$2"; shift 2 ;;
        --max-repo-mib=*) max_repo_mib="${1#*=}"; shift ;;
        --max-repo-mib) max_repo_mib="$2"; shift 2 ;;
        --require-persistent-gpg) require_persistent_gpg=true; shift ;;
        --ephemeral-gpg) ephemeral_gpg=true; shift ;;
        --gpg-private-key-file=*) gpg_private_key_file="${1#*=}"; shift ;;
        --gpg-private-key-file) gpg_private_key_file="$2"; shift 2 ;;
        --gpg-passphrase=*) gpg_passphrase="${1#*=}"; shift ;;
        --gpg-passphrase) gpg_passphrase="$2"; shift 2 ;;
        --repo-base-url=*) repo_base_url="${1#*=}"; shift ;;
        --repo-base-url) repo_base_url="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Error: Unknown option: %s\n' "$1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$assets_dir" || -z "$pages_dir" ]]; then
    printf 'Error: --assets-dir and --pages-dir are required\n' >&2
    exit 1
fi

if ! [[ "$retain_versions" =~ ^[0-9]+$ ]] || ! [[ "$max_repo_mib" =~ ^[0-9]+$ ]]; then
    printf 'Error: --retain-versions and --max-repo-mib must be non-negative integers\n' >&2
    exit 1
fi

if [[ "$require_persistent_gpg" == true ]]; then
    if [[ -z "${MODERNIME_GPG_PRIVATE_KEY:-}" && -z "$gpg_private_key_file" && -z "${MODERNIME_GPG_KEY_ID:-}" ]]; then
        printf 'Error: Official release requires persistent GPG signing key (MODERNIME_GPG_PRIVATE_KEY or --gpg-private-key-file).\n' >&2
        printf 'Refusing to publish repository with an ephemeral GPG key that would break client upgrades.\n' >&2
        exit 1
    fi
fi

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

# --- 1. Discover DEB artifacts ---
noble_amd64_deb=$(find_first_match "$assets_dir" "modernime_*_amd64_ubuntu24.04.deb" "*ubuntu24.04.deb" || true)
if [[ -z "$noble_amd64_deb" ]]; then
    printf 'Error: Could not locate canonical *ubuntu24.04.deb package in %s\n' "$assets_dir" >&2
    exit 1
fi
cand_ver=$(dpkg-deb -f "$noble_amd64_deb" Version)

declare -A apt_suite_flags=(
    [noble]="--noble-deb"
    [bookworm]="--bookworm-deb"
    [trixie]="--trixie-deb"
)
declare -A apt_distro_tags=(
    [noble]="ubuntu24.04"
    [bookworm]="debian12"
    [trixie]="debian13"
)

declare -a apt_debs_args=()
declare -a apt_arches=()
declare -a built_apt_suites=()

for suite in noble bookworm trixie; do
    dtag="${apt_distro_tags[$suite]}"
    flag="${apt_suite_flags[$suite]}"
    suite_has_current=false
    for arch in amd64 arm64; do
        deb_match=""
        if [[ "$suite" == "noble" && "$arch" == "amd64" ]]; then
            deb_match="$noble_amd64_deb"
        else
            deb_match=$(find_first_match "$assets_dir" "modernime_*_${arch}_${dtag}.deb" || true)
        fi
        if [[ -n "$deb_match" ]]; then
            apt_debs_args+=("$flag" "$deb_match")
            suite_has_current=true
            if [[ " ${apt_arches[*]-} " != *" $arch "* ]]; then
                apt_arches+=("$arch")
            fi
        fi
    done
    if [[ "$suite_has_current" == true ]]; then
        built_apt_suites+=("$suite")
    fi
done

# --- 2. Discover RPM artifacts ---
oe2409_x86_64_rpm=$(find_first_match "$assets_dir" "modernime-*.oe24.x86_64.rpm" "modernime-*.oe2409.x86_64.rpm" "*.rpm" || true)
if [[ -z "$oe2409_x86_64_rpm" ]]; then
    printf 'Error: Could not locate openEuler x86_64 RPM package in %s\n' "$assets_dir" >&2
    exit 1
fi

declare -a rpm_targets=()
declare -A rpm_asset_map=()

for distro in oe2409 fc41 fc42; do
    for arch in x86_64 aarch64; do
        rpm_match=""
        if [[ "$distro" == "oe2409" && "$arch" == "x86_64" ]]; then
            rpm_match="$oe2409_x86_64_rpm"
        elif [[ "$distro" == "oe2409" ]]; then
            rpm_match=$(find_first_match "$assets_dir" "modernime-*.oe24.${arch}.rpm" "modernime-*.oe2409.${arch}.rpm" || true)
        else
            rpm_match=$(find_first_match "$assets_dir" "modernime-*.${distro}.${arch}.rpm" || true)
        fi
        if [[ -n "$rpm_match" ]]; then
            key="${distro}:${arch}"
            rpm_targets+=("$key")
            rpm_asset_map["$key"]="$rpm_match"
        fi
    done
done

# --- 3. Discover Arch Linux (.pkg.tar.zst) artifacts ---
declare -a arch_targets=()
declare -A arch_asset_map=()
for arch in x86_64 aarch64; do
    pkg_match=$(find_first_match "$assets_dir" "modernime-*-${arch}.pkg.tar.zst" || true)
    if [[ -n "$pkg_match" ]]; then
        arch_targets+=("$arch")
        arch_asset_map["$arch"]="$pkg_match"
    fi
done

# --- 4. Select retained historical versions within count and MiB limits ---
declare -A retained_rpm_files=()
declare -A retained_arch_files=()

if [[ -n "$previous_pages_dir" && -d "$previous_pages_dir" ]]; then
    cur_pkg_files=()
    for ((i = 1; i < ${#apt_debs_args[@]}; i += 2)); do
        cur_pkg_files+=("${apt_debs_args[$i]}")
    done
    for key in "${rpm_targets[@]}"; do
        cur_pkg_files+=("${rpm_asset_map[$key]}")
    done
    if [[ ${#arch_targets[@]} -gt 0 ]]; then
        for arch in "${arch_targets[@]}"; do
            cur_pkg_files+=("${arch_asset_map[$arch]}")
        done
    fi

    retention_manifest=$(
        python3 - "$cand_ver" "$retain_versions" "$max_repo_mib" "$previous_pages_dir" \
            "${built_apt_suites[*]-}" "${apt_arches[*]-}" "${rpm_targets[*]-}" "${arch_targets[*]-}" \
            "${cur_pkg_files[@]}" <<'PYEOF'
from pathlib import Path
import re
import subprocess
import sys

cand_ver = sys.argv[1]
retain_versions = int(sys.argv[2])
max_repo_mib = int(sys.argv[3])
prev_dir = Path(sys.argv[4])
apt_suites = [s for s in sys.argv[5].split() if s]
apt_arches = set(s for s in sys.argv[6].split() if s)
rpm_targets = [s for s in sys.argv[7].split() if s]
arch_targets = [s for s in sys.argv[8].split() if s]
cur_files = [Path(p) for p in sys.argv[9:] if p]

current_bytes = sum(p.stat().st_size for p in cur_files if p.is_file())
max_repo_bytes = max_repo_mib * 1024 * 1024

def ver_key(ver: str):
    parts = [int(x) for x in re.findall(r"\d+", ver)]
    return (parts, ver)

prev_entries = []  # (kind, key_or_suite, ver, path, size)

for suite in apt_suites:
    pool_dir = prev_dir / "apt" / "pool" / suite / "main" / "m" / "modernime"
    if not pool_dir.is_dir():
        continue
    for deb in sorted(pool_dir.glob("*.deb")):
        meta = subprocess.check_output(
            ["dpkg-deb", "--show", "--showformat=${Version}\t${Architecture}", str(deb)],
            text=True,
        ).strip().split("\t")
        if len(meta) != 2:
            continue
        ver, arch = meta
        if ver == cand_ver or arch not in apt_arches:
            continue
        prev_entries.append(("DEB", suite, ver, deb, deb.stat().st_size))

for target in rpm_targets:
    distro, arch = target.split(":", 1)
    pkg_dir = prev_dir / "rpm" / distro / arch / "Packages"
    if not pkg_dir.is_dir():
        continue
    for rpm in sorted(pkg_dir.glob("*.rpm")):
        m = re.match(r"^modernime-(.+)-[^-]+\.[^.]+\.[^.]+\.rpm$", rpm.name)
        if not m:
            continue
        ver = m.group(1)
        if ver == cand_ver:
            continue
        prev_entries.append(("RPM", target, ver, rpm, rpm.stat().st_size))

for arch in arch_targets:
    arch_dir = prev_dir / "arch" / arch
    if not arch_dir.is_dir():
        continue
    for pkg in sorted(arch_dir.glob("*.pkg.tar.zst")):
        m = re.match(r"^modernime-(.+)-[^-]+-(?:x86_64|aarch64|any)\.pkg\.tar\.zst$", pkg.name)
        if not m:
            continue
        ver = m.group(1)
        if ver == cand_ver:
            continue
        prev_entries.append(("PAC", arch, ver, pkg, pkg.stat().st_size))

ver_bytes = {}
for _, _, ver, _, sz in prev_entries:
    ver_bytes[ver] = ver_bytes.get(ver, 0) + sz

sorted_prev = sorted(ver_bytes.keys(), key=ver_key, reverse=True)
kept = []
pruned = []
running_bytes = current_bytes

for idx, ver in enumerate(sorted_prev):
    if retain_versions > 0 and (len(kept) + 1) >= retain_versions:
        pruned.extend(sorted_prev[idx:])
        break
    if max_repo_bytes > 0 and (running_bytes + ver_bytes[ver]) > max_repo_bytes:
        pruned.extend(sorted_prev[idx:])
        break
    kept.append(ver)
    running_bytes += ver_bytes[ver]

kept_set = set(kept)
sys.stderr.write(
    f"Repository retention (retain_versions={retain_versions}, max_repo_mib={max_repo_mib}): "
    f"current={cand_ver} ({current_bytes / (1024 * 1024):.1f} MiB), "
    f"retained_previous={kept}, pruned_previous={pruned}, "
    f"projected_repo_packages={running_bytes / (1024 * 1024):.1f} MiB\n"
)

for kind, target, ver, path, _ in prev_entries:
    if ver in kept_set:
        print(f"{kind}\t{target}\t{path}")
PYEOF
    )

    while IFS=$'\t' read -r kind target pkg_path; do
        [[ -n "$kind" ]] || continue
        case "$kind" in
            DEB)
                apt_debs_args+=("${apt_suite_flags[$target]}" "$pkg_path")
                ;;
            RPM)
                if [[ -n "${retained_rpm_files[$target]-}" ]]; then
                    retained_rpm_files["$target"]+=$'\n'"$pkg_path"
                else
                    retained_rpm_files["$target"]="$pkg_path"
                fi
                ;;
            PAC)
                if [[ -n "${retained_arch_files[$target]-}" ]]; then
                    retained_arch_files["$target"]+=$'\n'"$pkg_path"
                else
                    retained_arch_files["$target"]="$pkg_path"
                fi
                ;;
        esac
    done <<< "$retention_manifest"
fi

apt_arches_csv=$(IFS=','; printf '%s' "${apt_arches[*]}")
declare -a apt_args=(
    --output-dir "$pages_dir/apt"
    --repo-url "${repo_base_url%/}/apt"
    --arches "$apt_arches_csv"
    "${apt_debs_args[@]}"
)

ephemeral_gpg_home=""
cleanup_ephemeral_gpg() {
    if [[ -n "$ephemeral_gpg_home" && -d "$ephemeral_gpg_home" ]]; then
        gpgconf --homedir "$ephemeral_gpg_home" --kill gpg-agent >/dev/null 2>&1 || true
        rm -rf -- "$ephemeral_gpg_home"
    fi
}
trap cleanup_ephemeral_gpg EXIT

declare -a common_gpg_args=()
if [[ -n "$gpg_private_key_file" ]]; then
    common_gpg_args+=(--gpg-private-key-file "$gpg_private_key_file")
elif [[ "$ephemeral_gpg" == true ]]; then
    # Share one ephemeral GPG key across APT, all RPM distros/arches, and all Pacman arches
    # so exported keyrings match every artifact produced in this run.
    ephemeral_gpg_home=$(mktemp -d "${TMPDIR:-/tmp}/modernime-ci-ephemeral-gpg.XXXXXX")
    chmod 700 "$ephemeral_gpg_home"
    cat > "$ephemeral_gpg_home/gpg-batch.conf" <<'EOF'
%echo Generating ModernIME ephemeral test GPG key
Key-Type: RSA
Key-Length: 3072
Name-Real: ModernIME Ephemeral Test Signing
Name-Email: ephemeral@modernime.invalid
Expire-Date: 0
%no-protection
%commit
EOF
    gpg --homedir "$ephemeral_gpg_home" --batch --quiet --generate-key "$ephemeral_gpg_home/gpg-batch.conf"
    ephemeral_key_id=$(gpg --homedir "$ephemeral_gpg_home" --list-secret-keys --with-colons | awk -F: '/^sec:/ {print $5; exit}')
    common_gpg_args+=(--gpg-home "$ephemeral_gpg_home" --gpg-key-id "$ephemeral_key_id")
fi
if [[ -n "$gpg_passphrase" ]]; then
    common_gpg_args+=(--gpg-passphrase "$gpg_passphrase")
fi

rm -rf -- "$pages_dir"
mkdir -p "$pages_dir"
: > "$pages_dir/.nojekyll"

# Build APT repository
"$project_root/scripts/build_apt_repo.sh" "${apt_args[@]}" "${common_gpg_args[@]}"

# Build RPM repositories across all discovered distro/arch pairs
rm -rf -- "$pages_dir/rpm"
for key in "${rpm_targets[@]}"; do
    distro="${key%%:*}"
    arch="${key##*:}"
    rpm_pkg="${rpm_asset_map[$key]}"
    declare -a rpm_args=(
        --output-dir "$pages_dir/rpm"
        --repo-url "${repo_base_url%/}/rpm"
        --distro "$distro"
        --arch "$arch"
        --preserve-output
    )
    if [[ -n "${retained_rpm_files[$key]-}" ]]; then
        while IFS= read -r old_rpm; do
            [[ -f "$old_rpm" ]] || continue
            rpm_args+=(--rpm "$old_rpm")
        done <<< "${retained_rpm_files[$key]}"
    fi
    rpm_args+=(--rpm "$rpm_pkg" "${common_gpg_args[@]}")
    "$project_root/scripts/build_rpm_repo.sh" "${rpm_args[@]}"
done

# Build Pacman repositories across all discovered Arch architectures
if [[ ${#arch_targets[@]} -gt 0 ]]; then
    rm -rf -- "$pages_dir/arch"
    for arch in "${arch_targets[@]}"; do
        arch_pkg="${arch_asset_map[$arch]}"
        declare -a pac_args=(
            --output-dir "$pages_dir/arch"
            --repo-url "${repo_base_url%/}/arch"
            --arch "$arch"
            --preserve-output
        )
        if [[ -n "${retained_arch_files[$arch]-}" ]]; then
            while IFS= read -r old_pkg; do
                [[ -f "$old_pkg" ]] || continue
                pac_args+=(--pkg "$old_pkg")
            done <<< "${retained_arch_files[$arch]}"
        fi
        pac_args+=(--pkg "$arch_pkg" "${common_gpg_args[@]}")
        "$project_root/scripts/build_pacman_repo.sh" "${pac_args[@]}"
    done
fi

if [[ "$require_persistent_gpg" == true ]]; then
    declare -a keys_to_check=("$pages_dir/apt/modernime-archive-keyring.asc" "$pages_dir/rpm/RPM-GPG-KEY-modernime")
    if [[ -f "$pages_dir/arch/modernime-arch-keyring.asc" ]]; then
        keys_to_check+=("$pages_dir/arch/modernime-arch-keyring.asc")
    fi
    for key_file in "${keys_to_check[@]}"; do
        key_meta=$(gpg --batch --with-colons --import-options show-only --import "$key_file" 2>/dev/null || true)
        if printf '%s\n' "$key_meta" | grep -Eiq 'modernime\.invalid|Ephemeral'; then
            printf 'Error: Ephemeral test GPG key detected in %s; refusing production release.\n' "$key_file" >&2
            exit 1
        fi
    done
fi

# Sync each signed current-version RPM from Pages repository back to assets_dir,
# preserving its canonical filename, and verify embedded OpenPGP signatures (tags 268 & 259).
for key in "${rpm_targets[@]}"; do
    distro="${key%%:*}"
    arch="${key##*:}"
    rpm_pkg="${rpm_asset_map[$key]}"
    signed_pages_rpm=$(find_first_match "$pages_dir/rpm/${distro}/${arch}/Packages" "modernime-${cand_ver}-*.rpm" || true)
    if [[ -z "$signed_pages_rpm" || ! -f "$signed_pages_rpm" ]]; then
        printf 'Error: Signed RPM for version %s not found in %s/rpm/%s/%s/Packages\n' "$cand_ver" "$pages_dir" "$distro" "$arch" >&2
        exit 1
    fi
    cp -f "$signed_pages_rpm" "$rpm_pkg"

    if ! cmp -s "$rpm_pkg" "$signed_pages_rpm"; then
        printf 'Error: Release asset RPM %s is not byte-identical to Pages RPM %s\n' "$rpm_pkg" "$signed_pages_rpm" >&2
        exit 1
    fi

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
done

# Copy client configs and keyrings into assets_dir for Release attachment convenience
cp -f "$pages_dir/apt/modernime-archive-keyring.gpg" "$assets_dir/"
cp -f "$pages_dir/apt/modernime-archive-keyring.asc" "$assets_dir/"
cp -f "$pages_dir/apt/KEY-FINGERPRINTS.txt" "$assets_dir/"
for suite in "${built_apt_suites[@]}"; do
    cp -f "$pages_dir/apt/client/modernime-${suite}.sources" "$assets_dir/"
done
cp -f "$pages_dir/apt/client/99-modernime.pref" "$assets_dir/"
cp -f "$pages_dir/rpm/RPM-GPG-KEY-modernime" "$assets_dir/"
for repo_file in "$pages_dir/rpm/client"/*.repo; do
    [[ -f "$repo_file" ]] || continue
    cp -f "$repo_file" "$assets_dir/"
done
if [[ -d "$pages_dir/arch/client" ]]; then
    cp -f "$pages_dir/arch/modernime-arch-keyring.asc" "$assets_dir/"
    cp -f "$pages_dir/arch/modernime-arch-keyring.gpg" "$assets_dir/"
    cp -f "$pages_dir/arch/client/modernime-pacman.conf" "$assets_dir/"
fi

printf 'Static Pages repository tree ready at: %s\n' "$pages_dir"
