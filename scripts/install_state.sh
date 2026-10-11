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

# Accept one complete, non-nested installer block. Surrounding user lines are
# intentionally excluded from the ownership snapshot.
modernime_profile_block() {
    awk '
        $0 == "# BEGIN MODERNIME ENV" { begins++; inside=1 }
        inside { print }
        $0 == "# END MODERNIME ENV" { ends++; if (!inside) invalid=1; inside=0 }
        END { if (begins != 1 || ends != 1 || inside || invalid) exit 1 }
    ' "$1"
}

modernime_strip_profile_block() {
    awk '
        $0 == "# BEGIN MODERNIME ENV" { inside=1; next }
        $0 == "# END MODERNIME ENV" { inside=0; next }
        !inside { print }
    ' "$1"
}

modernime_write_profile_block() {
    local expected=$1 target=$2
    local baseline="$managed_state_dir/xprofile.content"
    local recorded_path="$managed_state_dir/xprofile.path"
    if [[ -f "$recorded_path" ]] && [[ "$(cat "$recorded_path")" != "$target" ]]; then
        # Do not forget the previous profile when an explicit path changes.
        local old_target
        old_target=$(cat "$recorded_path")
        managed_files_preserved=false
        modernime_remove_profile_block "$old_target"
        if [[ "$managed_files_preserved" == true ]]; then
            printf 'Cannot move modified profile block from %s to %s\n' "$old_target" "$target" >&2
            return 0
        fi
        rm -f "$baseline" "$recorded_path"
    fi
    if [[ -L "$target" || ( -e "$target" && ! -f "$target" ) ]]; then
        printf 'Preserving modified or unowned profile: %s\n' "$target" >&2
        return 0
    fi
    local extracted
    extracted=$(mktemp)
    if [[ -f "$target" ]] && grep -Fq '# BEGIN MODERNIME ENV' "$target"; then
        if ! modernime_profile_block "$target" > "$extracted" ||
           [[ ! -f "$baseline" || ! -f "$recorded_path" ]] ||
           [[ "$(cat "$recorded_path")" != "$target" ]] ||
           ! cmp -s "$baseline" "$extracted"; then
            rm -f "$extracted"
            printf 'Preserving modified or unowned profile: %s\n' "$target" >&2
            return 0
        fi
        modernime_strip_profile_block "$target" > "$extracted"
    elif [[ -f "$target" ]]; then
        if grep -Eq 'GTK_IM_MODULE=|XMODIFIERS=|# END MODERNIME ENV' "$target"; then
            rm -f "$extracted"
            printf 'Preserving existing input method profile: %s\n' "$target" >&2
            return 0
        fi
        cat "$target" > "$extracted"
        # Ensure an unterminated last user line cannot swallow the marker.
        printf '\n' >> "$extracted"
    fi
    cat "$expected" >> "$extracted"
    mkdir -p -- "$(dirname -- "$target")" "$managed_state_dir"
    cat "$extracted" > "$target"
    rm -f "$extracted"
    cp "$expected" "$baseline"
    printf '%s\n' "$target" > "$recorded_path"
}

modernime_remove_profile_block() {
    local target=$1 extracted
    if [[ ! -e "$target" && ! -L "$target" ]]; then return 0; fi
    if [[ ! -L "$target" && -f "$target" ]] &&
       ! grep -Eq '^# (BEGIN|END) MODERNIME ENV$' "$target"; then
        # The recorded block was already removed; user content is untouched.
        return 0
    fi
    extracted=$(mktemp)
    if [[ ! -L "$target" && -f "$target" && -f "$managed_state_dir/xprofile.content" ]] &&
       modernime_profile_block "$target" > "$extracted" &&
       cmp -s "$managed_state_dir/xprofile.content" "$extracted"; then
        modernime_strip_profile_block "$target" > "$extracted"
        cat "$extracted" > "$target"
        if [[ ! -s "$target" ]]; then rm -f "$target"; fi
    else
        printf 'Preserving modified or unowned profile: %s\n' "$target" >&2
        managed_files_preserved=true
    fi
    rm -f "$extracted"
}
