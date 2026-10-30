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

if ! modernime_acquire_prefix_lock "$prefix"; then
    exit 1
fi

bin_tx_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-bin-payload-tx.XXXXXX")
if ! modernime_deploy_payload "$payload_root" "$prefix" "$bin_tx_dir"; then
    tx_status=$(cat "$bin_tx_dir/status" 2>/dev/null || echo "UNKNOWN")
    if [[ "$tx_status" == "ROLLED_BACK" ]]; then
        modernime_cleanup_transaction_dir "$bin_tx_dir"
        modernime_release_prefix_lock
        printf 'Error encountered during payload installation; rollback completed.\n' >&2
    elif [[ "$tx_status" == "FAILED_ROLLBACK" ]]; then
        modernime_release_prefix_lock
        printf 'Error: Payload rollback failed; recovery files retained in %s\n' "$bin_tx_dir" >&2
    else
        modernime_release_prefix_lock
        printf 'Error: Payload installation failed (status=%s); transaction materials retained in %s\n' "$tx_status" "$bin_tx_dir" >&2
    fi
    exit 1
fi
modernime_cleanup_transaction_dir "$bin_tx_dir"
modernime_release_prefix_lock

printf 'ModernIME binary payload installed to %s\n' "$prefix"
