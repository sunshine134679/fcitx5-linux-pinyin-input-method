#!/usr/bin/env bash
# Shared ownership checks for generated integration files.
# Callers set managed_state_dir; snapshots contain only installer-generated data.
modernime_desktop_string() {
    # Plain Desktop Entry strings use one escaping layer, unlike Exec arguments.
    local value=$1
    value=${value//\\/\\\\}
    value=${value//$'\n'/\\n}
    value=${value//$'\r'/\\r}
    value=${value//$'\t'/\\t}
    printf '%s' "$value"
}

modernime_desktop_argument() {
    # Desktop Entry values are decoded before Exec quoting is decoded.
    # Escape both layers, including literal percent field-code characters.
    local value=$1 character index
    printf '"'
    for ((index = 0; index < ${#value}; ++index)); do
        character=${value:index:1}
        case "$character" in
            '\') printf '%s' '\\\\' ;;
            '"'|'$'|'`') printf '%s%s' '\\' "$character" ;;
            '%') printf '%%%%' ;;
            $'\n') printf '%s' '\n' ;;
            $'\r') printf '%s' '\r' ;;
            $'\t') printf '%s' '\t' ;;
            *) printf '%s' "$character" ;;
        esac
    done
    printf '"'
}

modernime_write_managed_file() {
    local expected=$1 target=$2 key=$3
    local managed_state_dir=${4:-"${managed_state_dir:-}"}
    if [[ -z "$managed_state_dir" ]]; then
        printf 'Error: managed_state_dir not specified\n' >&2
        return 1
    fi
    local baseline="$managed_state_dir/$key.content"
    local recorded_path="$managed_state_dir/$key.path"
    managed_file_written=false
    if [[ -f "$recorded_path" ]] && [[ "$(cat "$recorded_path")" != "$target" ]]; then
        local old_target
        old_target=$(cat "$recorded_path")
        managed_files_preserved=false
        modernime_remove_managed_file "$old_target" "$key" "$managed_state_dir"
        if [[ "$managed_files_preserved" == true ]]; then
            printf 'Cannot move modified managed file from %s to %s\n' "$old_target" "$target" >&2
            return 0
        fi
        rm -f "$baseline" "$recorded_path"
    fi
    if [[ -e "$target" || -L "$target" ]]; then
        if [[ -L "$target" || ! -f "$target" || ! -f "$baseline" || ! -f "$recorded_path" ]] ||
           [[ "$(cat "$recorded_path")" != "$target" ]] ||
           ! cmp -s "$baseline" "$target"; then
            printf 'Preserving modified or unowned file: %s\n' "$target" >&2
            return 0
        fi
    fi
    mkdir -p -- "$(dirname -- "$target")" "$managed_state_dir" || return 1
    local replacement
    replacement=$(mktemp "$(dirname -- "$target")/.modernime-managed.XXXXXX" 2>/dev/null) || return 1
    if ! cp -- "$expected" "$replacement" 2>/dev/null; then
        rm -f -- "$replacement"
        return 1
    fi
    if ! mv -f -- "$replacement" "$target" 2>/dev/null; then
        rm -f -- "$replacement"
        return 1
    fi
    cp -- "$expected" "$baseline" || return 1
    printf '%s\n' "$target" > "$recorded_path" || return 1
    managed_file_written=true
    return 0
}

modernime_remove_managed_file() {
    local target=$1 key=$2
    local managed_state_dir=${3:-"${managed_state_dir:-}"}
    if [[ -z "$managed_state_dir" ]]; then
        printf 'Error: managed_state_dir not specified\n' >&2
        return 1
    fi
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
        rmdir --ignore-fail-on-non-empty "$(dirname -- "$target")" 2>/dev/null || true
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
    local managed_state_dir=${3:-"${managed_state_dir:-}"}
    if [[ -z "$managed_state_dir" ]]; then
        printf 'Error: managed_state_dir not specified\n' >&2
        return 1
    fi
    local baseline="$managed_state_dir/xprofile.content"
    local recorded_path="$managed_state_dir/xprofile.path"
    if [[ -f "$recorded_path" ]] && [[ "$(cat "$recorded_path")" != "$target" ]]; then
        # Do not forget the previous profile when an explicit path changes.
        local old_target
        old_target=$(cat "$recorded_path")
        managed_files_preserved=false
        modernime_remove_profile_block "$old_target" "$managed_state_dir"
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
    if ! extracted=$(mktemp); then
        return 1
    fi
    if [[ -f "$target" ]] && grep -Fq '# BEGIN MODERNIME ENV' "$target"; then
        if ! modernime_profile_block "$target" > "$extracted" ||
           [[ ! -f "$baseline" || ! -f "$recorded_path" ]] ||
           [[ "$(cat "$recorded_path")" != "$target" ]] ||
           ! cmp -s "$baseline" "$extracted"; then
            rm -f "$extracted"
            printf 'Preserving modified or unowned profile: %s\n' "$target" >&2
            return 0
        fi
        if ! modernime_strip_profile_block "$target" > "$extracted"; then
            rm -f "$extracted"
            return 1
        fi
    elif [[ -f "$target" ]]; then
        if grep -Eq 'GTK_IM_MODULE=|XMODIFIERS=|# END MODERNIME ENV' "$target"; then
            rm -f "$extracted"
            printf 'Preserving existing input method profile: %s\n' "$target" >&2
            return 0
        fi
        if ! cat "$target" > "$extracted"; then
            rm -f "$extracted"
            return 1
        fi
        # Ensure an unterminated last user line cannot swallow the marker.
        if ! printf '\n' >> "$extracted"; then
            rm -f "$extracted"
            return 1
        fi
    fi
    if ! cat "$expected" >> "$extracted"; then
        rm -f "$extracted"
        return 1
    fi
    if ! mkdir -p -- "$(dirname -- "$target")" "$managed_state_dir"; then
        rm -f "$extracted"
        return 1
    fi
    if ! cat "$extracted" > "$target"; then
        rm -f "$extracted"
        return 1
    fi
    rm -f "$extracted"
    if ! cp "$expected" "$baseline"; then
        return 1
    fi
    if ! printf '%s\n' "$target" > "$recorded_path"; then
        return 1
    fi
}

modernime_remove_profile_block() {
    local target=$1
    local managed_state_dir=${2:-"${managed_state_dir:-}"}
    if [[ -z "$managed_state_dir" ]]; then
        printf 'Error: managed_state_dir not specified\n' >&2
        return 1
    fi
    local extracted
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
