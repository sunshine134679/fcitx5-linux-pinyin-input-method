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

stage_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-stage.XXXXXX")
cleanup_stage() {
    rm -rf -- "$stage_dir"
}

mkdir -p -- "$stage_dir"
cp -a -- "$payload_root/." "$stage_dir/"

if [[ ! -f "$stage_dir/bin/modernime-settings" || -z "$binary_addon" ]]; then
    printf 'Error: Staged payload verification failed\n' >&2
    cleanup_stage
    exit 1
fi

backup_dir=""
rollback_on_failure() {
    local exit_code=$?
    if [[ $exit_code -ne 0 && -n "$backup_dir" && -d "$backup_dir" ]]; then
        printf 'Error encountered during payload installation; restoring previous version...\n' >&2
        cp -a -- "$backup_dir/." "$prefix/" 2>/dev/null || true
        rm -rf -- "$backup_dir"
        printf 'Rollback to previous version completed.\n' >&2
    fi
    cleanup_stage
}
trap rollback_on_failure EXIT

mkdir -p -- "$prefix"

if [[ -f "$prefix/bin/modernime-settings" ]]; then
    backup_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-rollback.XXXXXX")
    for check_f in "$prefix/bin/modernime-settings" "$prefix/bin/modernime-session-init" \
                   "$prefix/share/modernime/pinyin/modernime-knowledge.dict"; do
        if [[ -f "$check_f" ]]; then
            rel_path="${check_f#"$prefix/"}"
            mkdir -p -- "$backup_dir/$(dirname "$rel_path")"
            cp -a -- "$check_f" "$backup_dir/$rel_path"
        fi
    done
fi

cp -a -- "$stage_dir/." "$prefix/"

if [[ -n "$backup_dir" && -d "$backup_dir" ]]; then
    rm -rf -- "$backup_dir"
fi
cleanup_stage
trap - EXIT

printf 'ModernIME binary payload installed to %s\n' "$prefix"
