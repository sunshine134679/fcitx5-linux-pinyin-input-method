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
created_manifest=""
rollback_on_failure() {
    local exit_code=$?
    if [[ $exit_code -ne 0 ]]; then
        printf 'Error encountered during payload installation; restoring previous version...\n' >&2
        if [[ -n "$created_manifest" && -f "$created_manifest" ]]; then
            while IFS= read -r new_file; do
                if [[ -e "$new_file" || -L "$new_file" ]]; then
                    rm -f -- "$new_file"
                fi
            done < "$created_manifest"
            rm -f -- "$created_manifest"
        fi
        if [[ -n "$backup_dir" && -d "$backup_dir" ]]; then
            cp -a --remove-destination -- "$backup_dir/." "$prefix/" 2>/dev/null || true
            rm -rf -- "$backup_dir"
        fi
        # Roll back payload files only; live user writes made after the backup
        # remain valid and must not be replaced with a pre-upgrade snapshot.
        printf 'Rollback to previous version completed.\n' >&2
    fi
    cleanup_stage
}
trap rollback_on_failure EXIT

mkdir -p -- "$prefix"

if [[ -d "$prefix" ]]; then
    backup_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-rollback.XXXXXX")
    created_manifest=$(mktemp "${TMPDIR:-/tmp}/modernime-created.XXXXXX")
    while IFS= read -r -d '' src_file; do
        rel_path="${src_file#"$stage_dir/"}"
        dest_file="$prefix/$rel_path"
        if [[ -e "$dest_file" || -L "$dest_file" ]]; then
            mkdir -p -- "$backup_dir/$(dirname "$rel_path")"
            cp -a -- "$dest_file" "$backup_dir/$rel_path"
        else
            printf '%s\n' "$dest_file" >> "$created_manifest"
        fi
    done < <(find "$stage_dir" \( -type f -o -type l \) -print0)
fi

# Always unlink destination files before copying so running .so plugins mapped in memory are never overwritten in-place.
cp -a --remove-destination -- "$stage_dir/." "$prefix/"

if [[ "${MODERNIME_SIMULATE_INSTALL_FAILURE:-0}" == "1" ]]; then
    printf 'Error: Simulated mid-upgrade installation failure via MODERNIME_SIMULATE_INSTALL_FAILURE=1\n' >&2
    exit 1
fi

if [[ -n "$backup_dir" && -d "$backup_dir" ]]; then
    rm -rf -- "$backup_dir"
fi
if [[ -n "$created_manifest" && -f "$created_manifest" ]]; then
    rm -f -- "$created_manifest"
fi
cleanup_stage
trap - EXIT

printf 'ModernIME binary payload installed to %s\n' "$prefix"
