#!/usr/bin/env bash
# Sourced by the generated installer, which owns prefix/configuration policy.
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

mkdir -p -- "$prefix"
cp -a -- "$payload_root/." "$prefix/"
printf 'ModernIME binary payload installed to %s\n' "$prefix"
