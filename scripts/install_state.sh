#!/usr/bin/env bash
# Shared ownership checks for generated integration files.
# Callers set managed_state_dir; snapshots contain only installer-generated data.
modernime_write_managed_file() {
    local expected=$1 target=$2 key=$3
    local baseline="$managed_state_dir/$key.content"
    local recorded_path="$managed_state_dir/$key.path"
    managed_file_written=false
    if [[ -e "$target" || -L "$target" ]]; then
        if [[ -L "$target" || ! -f "$target" || ! -f "$baseline" || ! -f "$recorded_path" ]] ||
           [[ "$(cat "$recorded_path")" != "$target" ]] ||
           ! cmp -s "$baseline" "$target"; then
            printf 'Preserving modified or unowned file: %s\n' "$target" >&2
            return 0
        fi
    fi
    mkdir -p -- "$(dirname -- "$target")" "$managed_state_dir"
    local replacement
    replacement=$(mktemp "$(dirname -- "$target")/.modernime-managed.XXXXXX")
    cp -- "$expected" "$replacement"
    mv -f -- "$replacement" "$target"
    cp -- "$expected" "$baseline"
    printf '%s\n' "$target" > "$recorded_path"
    managed_file_written=true
}

modernime_remove_managed_file() {
    local target=$1 key=$2
    local baseline="$managed_state_dir/$key.content"
    local recorded_path="$managed_state_dir/$key.path"
    if [[ ! -e "$target" && ! -L "$target" ]]; then
        return 0
    fi
    if [[ ! -L "$target" && -f "$target" && -f "$baseline" &&
          -f "$recorded_path" ]] &&
       [[ "$(cat "$recorded_path")" == "$target" ]] &&
       cmp -s "$baseline" "$target"; then
        rm -f -- "$target"
    else
        printf 'Preserving modified or unowned file: %s\n' "$target" >&2
        managed_files_preserved=true
    fi
}
