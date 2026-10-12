#!/usr/bin/env bash
# build_apt_repo.sh — Generates a signed multi-suite (jammy, noble) APT repository for ModernIME
# Enforces architecture isolation, dpkg version monotonicity, SHA256 checksums,
# InRelease + Release.gpg signing, and Deb822 .sources with isolated Signed-By keyring.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

output_dir=""
repo_url="https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/apt"
target_arch="amd64"
gpg_home="${GNUPGHOME:-}"
gpg_key_id="${MODERNIME_GPG_KEY_ID:-}"
ephemeral_gpg=false
prev_version=""

declare -a jammy_debs=()
declare -a noble_debs=()
declare -a common_debs=()

usage() {
    cat <<'EOF'
Usage:
  build_apt_repo.sh --output-dir DIR [--deb FILE]... [--jammy-deb FILE]... [--noble-deb FILE]...
                    [--arch amd64] [--prev-version VER] [--gpg-home DIR] [--gpg-key-id ID] [--ephemeral-gpg]
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
        --deb=*)
            common_debs+=("${1#*=}")
            shift
            ;;
        --deb)
            common_debs+=("$2")
            shift 2
            ;;
        --jammy-deb=*)
            jammy_debs+=("${1#*=}")
            shift
            ;;
        --jammy-deb)
            jammy_debs+=("$2")
            shift 2
            ;;
        --noble-deb=*)
            noble_debs+=("${1#*=}")
            shift
            ;;
        --noble-deb)
            noble_debs+=("$2")
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
        --prev-version=*)
            prev_version="${1#*=}"
            shift
            ;;
        --prev-version)
            prev_version="$2"
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

if [[ -z "$output_dir" ]]; then
    printf 'Error: --output-dir is required\n' >&2
    exit 1
fi

for cmd in dpkg-deb dpkg-scanpackages apt-ftparchive gpg gpgv sha256sum gzip; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        printf 'Error: Required APT repository tool not found: %s\n' "$cmd" >&2
        exit 1
    fi
done

if [[ ${#common_debs[@]} -gt 0 ]]; then
    if [[ ${#jammy_debs[@]} -eq 0 ]]; then
        jammy_debs+=("${common_debs[@]}")
    fi
    if [[ ${#noble_debs[@]} -eq 0 ]]; then
        noble_debs+=("${common_debs[@]}")
    fi
fi

if [[ ${#jammy_debs[@]} -eq 0 && ${#noble_debs[@]} -eq 0 ]]; then
    printf 'Error: At least one .deb file must be supplied via --deb, --jammy-deb, or --noble-deb\n' >&2
    exit 1
fi

cleanup_gpg=""
cleanup() {
    if [[ -n "$cleanup_gpg" && -d "$cleanup_gpg" ]]; then
        rm -rf -- "$cleanup_gpg"
    fi
}
trap cleanup EXIT

if [[ "$ephemeral_gpg" == true || "${MODERNIME_GENERATE_EPHEMERAL_GPG:-0}" == "1" ]]; then
    cleanup_gpg=$(mktemp -d "${TMPDIR:-/tmp}/modernime-apt-gpg.XXXXXX")
    chmod 700 "$cleanup_gpg"
    gpg_home="$cleanup_gpg"
    export GNUPGHOME="$gpg_home"
    gpg --batch --generate-key <<'GPGEOF' 2>/dev/null
%no-protection
Key-Type: RSA
Key-Length: 3072
Subkey-Type: RSA
Subkey-Length: 3072
Name-Real: ModernIME Archive Automatic Signing Key
Name-Email: release@modernime.invalid
Expire-Date: 2y
%commit
GPGEOF
    gpg_key_id=$(gpg --batch --with-colons --list-secret-keys | awk -F: '/^sec:/ {print $5; exit}')
elif [[ -n "$gpg_home" ]]; then
    export GNUPGHOME="$gpg_home"
fi

validate_deb_for_suite() {
    local deb_file=$1
    local suite=$2
    if [[ ! -f "$deb_file" ]]; then
        printf 'Error: DEB file not found: %s\n' "$deb_file" >&2
        return 1
    fi
    local pkg_name pkg_ver pkg_arch
    pkg_name=$(dpkg-deb -f "$deb_file" Package 2>/dev/null || true)
    pkg_ver=$(dpkg-deb -f "$deb_file" Version 2>/dev/null || true)
    pkg_arch=$(dpkg-deb -f "$deb_file" Architecture 2>/dev/null || true)

    if [[ -z "$pkg_name" || -z "$pkg_ver" || -z "$pkg_arch" ]]; then
        printf 'Error: Invalid DEB control metadata in %s\n' "$deb_file" >&2
        return 1
    fi
    if [[ "$pkg_arch" != "$target_arch" && "$pkg_arch" != "all" ]]; then
        printf 'Error: Architecture mismatch for %s: package is "%s", repository requires "%s"\n' \
            "$deb_file" "$pkg_arch" "$target_arch" >&2
        return 1
    fi
    # Reject cross-suite contamination if version explicitly tags another Ubuntu release
    if [[ "$suite" == "jammy" && "$pkg_ver" == *"ubuntu24.04"* ]] || \
       [[ "$suite" == "jammy" && "$pkg_ver" == *"~noble"* ]]; then
        printf 'Error: Suite mismatch: refusing to place noble-specific package (%s) into jammy suite\n' "$pkg_ver" >&2
        return 1
    fi
    if [[ "$suite" == "noble" && "$pkg_ver" == *"ubuntu22.04"* ]] || \
       [[ "$suite" == "noble" && "$pkg_ver" == *"~jammy"* ]]; then
        printf 'Error: Suite mismatch: refusing to place jammy-specific package (%s) into noble suite\n' "$pkg_ver" >&2
        return 1
    fi
    # Validate Debian version syntax and monotonicity
    if ! dpkg --compare-versions "$pkg_ver" gt "0.0.0"; then
        printf 'Error: Invalid Debian version "%s" in %s\n' "$pkg_ver" "$deb_file" >&2
        return 1
    fi
    if [[ -n "$prev_version" ]]; then
        if ! dpkg --compare-versions "$pkg_ver" gt "$prev_version"; then
            printf 'Error: Package version "%s" in %s is not strictly greater than previous version "%s"\n' \
                "$pkg_ver" "$deb_file" "$prev_version" >&2
            return 1
        fi
    fi
    return 0
}

rm -rf -- "$output_dir"
mkdir -p "$output_dir/client"

build_suite() {
    local suite=$1
    local suite_ver=$2
    local suite_desc=$3
    shift 3
    local debs=("$@")
    if [[ ${#debs[@]} -eq 0 ]]; then
        return 0
    fi

    local pool_dir="$output_dir/pool/$suite/main/m/modernime"
    local bin_dir="$output_dir/dists/$suite/main/binary-$target_arch"
    local suite_dir="$output_dir/dists/$suite"
    mkdir -p "$pool_dir" "$bin_dir"

    local prev_in_suite=""
    for deb in "${debs[@]}"; do
        validate_deb_for_suite "$deb" "$suite"
        local ver
        ver=$(dpkg-deb -f "$deb" Version)
        if [[ -n "$prev_in_suite" ]]; then
            if [[ "$ver" == "$prev_in_suite" ]]; then
                printf 'Error: Duplicate version "%s" in suite "%s"\n' "$ver" "$suite" >&2
                exit 1
            fi
        fi
        prev_in_suite="$ver"
        local base_name="modernime_${ver}_${target_arch}.deb"
        cp -f -- "$deb" "$pool_dir/$base_name"
    done

    (
        cd "$output_dir"
        dpkg-scanpackages --multiversion "pool/$suite/main" /dev/null > "dists/$suite/main/binary-$target_arch/Packages"
        gzip -9cn "dists/$suite/main/binary-$target_arch/Packages" > "dists/$suite/main/binary-$target_arch/Packages.gz"
    )

    cat > "$suite_dir/main/binary-$target_arch/Release" <<EOF
Archive: $suite
Version: $suite_ver
Component: main
Origin: ModernIME
Label: ModernIME APT Repository
Architecture: $target_arch
EOF

    (
        cd "$suite_dir"
        apt-ftparchive \
            -o "APT::FTPArchive::Release::Origin=ModernIME" \
            -o "APT::FTPArchive::Release::Label=ModernIME APT Repository" \
            -o "APT::FTPArchive::Release::Suite=$suite" \
            -o "APT::FTPArchive::Release::Version=$suite_ver" \
            -o "APT::FTPArchive::Release::Codename=$suite" \
            -o "APT::FTPArchive::Release::Architectures=$target_arch" \
            -o "APT::FTPArchive::Release::Components=main" \
            -o "APT::FTPArchive::Release::Description=$suite_desc" \
            release . > Release
    )

    if [[ -n "$gpg_key_id" ]]; then
        gpg --batch --yes --default-key "$gpg_key_id" -- digest-algo SHA256 --clearsign -o "$suite_dir/InRelease" "$suite_dir/Release" 2>/dev/null || \
            gpg --batch --yes --default-key "$gpg_key_id" --digest-algo SHA256 --clearsign -o "$suite_dir/InRelease" "$suite_dir/Release"
        gpg --batch --yes --default-key "$gpg_key_id" --digest-algo SHA256 -abs -o "$suite_dir/Release.gpg" "$suite_dir/Release"
    fi

    sed \
        -e "s|@MODERNIME_APT_REPO_URL@|$repo_url|g" \
        -e "s|@MODERNIME_APT_SUITE@|$suite|g" \
        "$project_root/packaging/apt/modernime.sources.in" > "$output_dir/client/modernime-$suite.sources"
}

build_suite "jammy" "22.04" "ModernIME APT Repository for Ubuntu 22.04 LTS (Jammy Jellyfish)" "${jammy_debs[@]}"
build_suite "noble" "24.04" "ModernIME APT Repository for Ubuntu 24.04 LTS (Noble Numbat)" "${noble_debs[@]}"

cp -f -- "$project_root/packaging/apt/99-modernime.pref" "$output_dir/client/99-modernime.pref"

if [[ -n "$gpg_key_id" ]]; then
    gpg --batch --yes --export "$gpg_key_id" > "$output_dir/modernime-archive-keyring.gpg"
    gpg --batch --yes --armor --export "$gpg_key_id" > "$output_dir/modernime-archive-keyring.asc"
    # Verify signatures using gpgv against the isolated binary keyring
    for suite in jammy noble; do
        if [[ -f "$output_dir/dists/$suite/Release" ]]; then
            gpgv --keyring "$output_dir/modernime-archive-keyring.gpg" \
                "$output_dir/dists/$suite/InRelease" >/dev/null 2>&1
            gpgv --keyring "$output_dir/modernime-archive-keyring.gpg" \
                "$output_dir/dists/$suite/Release.gpg" \
                "$output_dir/dists/$suite/Release" >/dev/null 2>&1
        fi
    done
fi

(
    cd "$output_dir"
    find pool dists client -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)

printf 'APT repository generated at: %s\n' "$output_dir"
