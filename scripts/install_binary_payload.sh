#!/usr/bin/env bash
# Sourced by the generated installer or standalone test, which owns prefix/configuration policy.
set -euo pipefail

payload_root="$project_root/usr"
if [[ ! -f "$payload_root/bin/modernime-settings" ]]; then
    printf 'ModernIME binary package payload is incomplete: %s\n' "$payload_root" >&2
    exit 1
fi
binary_addon=$(find "$payload_root" -type f -path '*/fcitx5/modernime_fcitx5.so' -print -quit)
if [[ -z "$binary_addon" ]]; then
    printf 'ModernIME binary package addon is missing\n' >&2
    exit 1
fi
binary_libdir=${binary_addon#"$payload_root/"}
binary_libdir=${binary_libdir%/fcitx5/modernime_fcitx5.so}

host_arch=$(uname -m)
case "$host_arch" in
    x86_64|amd64)
        ;;
    *)
        printf 'Error: ModernIME binary package is built for x86_64, but host architecture is %s.\n' "$host_arch" >&2
        printf 'Please build ModernIME from source for your architecture.\n' >&2
        exit 1
        ;;
esac

# Source lifecycle library if not already present
if ! declare -f modernime_deploy_payload >/dev/null 2>&1; then
    if [[ -f "$project_root/scripts/install_lifecycle.sh" ]]; then
        source "$project_root/scripts/install_lifecycle.sh"
    elif [[ -f "$(dirname -- "${BASH_SOURCE[0]}")/install_lifecycle.sh" ]]; then
        source "$(dirname -- "${BASH_SOURCE[0]}")/install_lifecycle.sh"
    else
        printf 'Error: install_lifecycle.sh not found\n' >&2
        exit 1
    fi
fi

bin_tx_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-bin-payload-tx.XXXXXX")
if ! modernime_deploy_payload "$payload_root" "$prefix" "$bin_tx_dir"; then
    rm -rf -- "$bin_tx_dir"
    printf 'Error encountered during payload installation; restoring previous version...\n' >&2
    exit 1
fi
rm -rf -- "$bin_tx_dir"

printf 'ModernIME binary payload installed to %s\n' "$prefix"
