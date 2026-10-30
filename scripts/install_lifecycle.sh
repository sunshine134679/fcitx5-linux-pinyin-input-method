#!/usr/bin/env bash
# ModernIME Explicit Install Lifecycle Library
# Provides explicit, safe, atomic deployment and desktop/environment integration.
#
# Core Interfaces:
#   modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR
#   modernime_rollback_payload PREFIX TRANSACTION_DIR
#   modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR
set -euo pipefail

modernime_canonical_prefix() {
    local raw_prefix=${1:?Usage: modernime_canonical_prefix PREFIX}
    local canon=""
    if command -v realpath >/dev/null 2>&1; then
        canon=$(realpath -m "$raw_prefix" 2>/dev/null || true)
    fi
    if [[ -z "$canon" ]] && command -v readlink >/dev/null 2>&1; then
        canon=$(readlink -m "$raw_prefix" 2>/dev/null || true)
    fi
    if [[ -z "$canon" ]]; then
        canon="$raw_prefix"
    fi
    if [[ "$canon" != "/" ]]; then
        canon="${canon%/}"
    fi
    printf '%s\n' "$canon"
}

modernime_is_safe_removal_target() {
    local target=$1 prefix_clean=$2
    local canon_prefix=${3:-}
    local is_system_prefix=${4:-false}
    if [[ -z "$canon_prefix" ]]; then
        canon_prefix=$(modernime_canonical_prefix "$prefix_clean")
    fi

    # 1. Non-empty, absolute, no traversal components
    [[ -z "$target" ]] && return 1
    [[ "$target" != /* ]] && return 1
    if [[ "$target" == *"/../"* || "$target" == *"/.." || "$target" == "../"* || "$target" == ".." ]]; then
        return 1
    fi

    # 2. String prefix containment (must start with prefix_clean/ or canon_prefix/)
    if [[ "$target" != "$prefix_clean/"* && "$target" != "$canon_prefix/"* ]]; then
        return 1
    fi

    # 3. Canonical prefix must be non-empty
    [[ -z "$canon_prefix" ]] && return 1

    # 4. Canonical parent containment
    local parent_dir canon_parent
    parent_dir=$(dirname -- "$target")
    canon_parent=$(readlink -f -- "$parent_dir" 2>/dev/null || true)
    if [[ -z "$canon_parent" || ( "$canon_parent" != "$canon_prefix" && "$canon_parent" != "$canon_prefix/"* ) ]]; then
        return 1
    fi

    # 5. Canonical target containment if target exists and is not a symlink
    if [[ -e "$target" && ! -L "$target" ]]; then
        local canon_target
        canon_target=$(readlink -f -- "$target" 2>/dev/null || true)
        if [[ -z "$canon_target" || ( "$canon_target" != "$canon_prefix" && "$canon_target" != "$canon_prefix/"* ) ]]; then
            return 1
        fi
    fi

    # 6. Namespace ownership
    local is_allowed=false
    case "$target" in
        "$prefix_clean"/lib*/fcitx5/modernime_*.so|\
        "$prefix_clean"/lib/*-linux-gnu/fcitx5/modernime_*.so|\
        "$prefix_clean"/share/modernime/*|\
        "$prefix_clean"/share/fcitx5/addon/modernime*.conf|\
        "$prefix_clean"/share/fcitx5/inputmethod/modernime*.conf|\
        "$prefix_clean"/bin/modernime*|\
        "$prefix_clean"/share/doc/modernime/*|\
        "$prefix_clean"/share/icons/hicolor/*/apps/modernime*|\
        "$prefix_clean"/share/applications/modernime*.desktop|\
        "$prefix_clean"/share/locale/*/LC_MESSAGES/modernime*.mo|\
        "$canon_prefix"/lib*/fcitx5/modernime_*.so|\
        "$canon_prefix"/lib/*-linux-gnu/fcitx5/modernime_*.so|\
        "$canon_prefix"/share/modernime/*|\
        "$canon_prefix"/share/fcitx5/addon/modernime*.conf|\
        "$canon_prefix"/share/fcitx5/inputmethod/modernime*.conf|\
        "$canon_prefix"/bin/modernime*|\
        "$canon_prefix"/share/doc/modernime/*|\
        "$canon_prefix"/share/icons/hicolor/*/apps/modernime*|\
        "$canon_prefix"/share/applications/modernime*.desktop|\
        "$canon_prefix"/share/locale/*/LC_MESSAGES/modernime*.mo)
            is_allowed=true
            ;;
        *)
            if [[ "$(basename -- "$canon_prefix")" =~ ^[Mm]odern[Ii][Mm][Ee]$ ]]; then
                is_allowed=true
            fi
            ;;
    esac

    [[ "$is_allowed" != true ]] && return 1

    # 7. Native system package protection
    if [[ "$is_system_prefix" == true ]]; then
        local owner=""
        local canonical_target
        canonical_target=$(realpath "$target" 2>/dev/null || true)
        if command -v dpkg >/dev/null 2>&1; then
            owner=$(dpkg -S "$target" 2>/dev/null || true)
            if [[ -z "$owner" && -n "$canonical_target" && "$canonical_target" != "$target" ]]; then
                owner=$(dpkg -S "$canonical_target" 2>/dev/null || true)
            fi
        elif command -v rpm >/dev/null 2>&1; then
            owner=$(rpm -qf "$target" 2>/dev/null || true)
            if [[ -z "$owner" && -n "$canonical_target" && "$canonical_target" != "$target" ]]; then
                owner=$(rpm -qf "$canonical_target" 2>/dev/null || true)
            fi
        fi
        if [[ -n "$owner" ]]; then
            return 1
        fi
    fi

    return 0
}

modernime_acquire_prefix_lock() {
    local prefix_arg=${1:?Usage: modernime_acquire_prefix_lock PREFIX}
    local canonical_prefix
    canonical_prefix=$(modernime_canonical_prefix "$prefix_arg")

    local parent_dir
    parent_dir="${canonical_prefix%/*}"
    [[ -z "$parent_dir" ]] && parent_dir="/"
    local base_name
    base_name="${canonical_prefix##*/}"

    local lock_file
    if [[ "$parent_dir" == "/" ]]; then
        lock_file="/.modernime-${base_name}.lock"
    else
        lock_file="$parent_dir/.modernime-${base_name}.lock"
    fi

    # Re-entrancy guard: if current process already owns the lock on this prefix,
    # verify that the FD is live and points to the exact same lock file inode AND holds the lock.
    if [[ -n "${MODERNIME_PREFIX_LOCK_FD:-}" && "${MODERNIME_LOCKED_PREFIX:-}" == "$canonical_prefix" ]]; then
        local check_fd="${MODERNIME_PREFIX_LOCK_FD}"
        if [[ -e "/proc/self/fd/$check_fd" && -f "$lock_file" ]]; then
            local same_file=false
            if [[ "/proc/self/fd/$check_fd" -ef "$lock_file" ]]; then
                same_file=true
            else
                local fd_inode lock_inode
                fd_inode=$(stat -L -c "%d:%i" "/proc/self/fd/$check_fd" 2>/dev/null || true)
                lock_inode=$(stat -c "%d:%i" "$lock_file" 2>/dev/null || true)
                if [[ -n "$fd_inode" && -n "$lock_inode" && "$fd_inode" == "$lock_inode" ]]; then
                    same_file=true
                fi
            fi
            if [[ "$same_file" == true ]]; then
                # Prove this FD actually holds the flock
                if flock -x -n "$check_fd" >/dev/null 2>&1; then
                    return 0
                fi
            fi
        fi
        # Invalid or unheld lock FD; reset and acquire freshly
        unset MODERNIME_PREFIX_LOCK_FD
        unset MODERNIME_LOCKED_PREFIX
        unset MODERNIME_LOCK_FILE
    fi

    if ! command -v flock >/dev/null 2>&1; then
        printf 'Error: Required utility flock (util-linux) is not installed.\n' >&2
        return 1
    fi

    local parent_dir
    parent_dir="${canonical_prefix%/*}"
    [[ -z "$parent_dir" ]] && parent_dir="/"
    local base_name
    base_name="${canonical_prefix##*/}"

    if [[ ! -d "$parent_dir" ]]; then
        mkdir -p -- "$parent_dir"
    fi

    local lock_file
    if [[ "$parent_dir" == "/" ]]; then
        lock_file="/.modernime-${base_name}.lock"
    else
        lock_file="$parent_dir/.modernime-${base_name}.lock"
    fi

    local lock_fd
    if ! exec {lock_fd}>"$lock_file"; then
        printf 'Error: Cannot open prefix lock file: %s\n' "$lock_file" >&2
        return 1
    fi

    if ! flock -n "$lock_fd"; then
        printf 'Error: Prefix is locked by another ModernIME transaction: %s\n' "$canonical_prefix" >&2
        printf 'Active lock file: %s\n' "$lock_file" >&2
        exec {lock_fd}>&-
        return 1
    fi

    MODERNIME_PREFIX_LOCK_FD="$lock_fd"
    MODERNIME_LOCKED_PREFIX="$canonical_prefix"
    MODERNIME_LOCK_FILE="$lock_file"
    return 0
}

modernime_release_prefix_lock() {
    if [[ -n "${MODERNIME_PREFIX_LOCK_FD:-}" ]]; then
        local fd="$MODERNIME_PREFIX_LOCK_FD"
        unset MODERNIME_PREFIX_LOCK_FD
        unset MODERNIME_LOCKED_PREFIX
        unset MODERNIME_LOCK_FILE
        eval "exec ${fd}>&-"
    fi
    return 0
}

modernime_cleanup_transaction_dir() {
    local transaction_dir=${1:?Usage: modernime_cleanup_transaction_dir TRANSACTION_DIR}
    if [[ ! -d "$transaction_dir" ]]; then
        return 0
    fi
    local status_file="$transaction_dir/status"
    local status=""
    if [[ -f "$status_file" ]]; then
        read -r status < "$status_file" || true
    fi

    case "$status" in
        COMMITTED|ROLLED_BACK|FAILED_COLLISION|FAILED_BACKUP)
            rm -rf -- "$transaction_dir"
            return 0
            ;;
        FAILED_ROLLBACK)
            printf 'Warning: ModernIME transaction rollback failed; recovery materials preserved in: %s\n' "$transaction_dir" >&2
            return 1
            ;;
        INTEGRATION_FAILED)
            printf 'Warning: ModernIME integration failed; payload deployed; transaction materials preserved in: %s\n' "$transaction_dir" >&2
            return 1
            ;;
        *)
            printf 'Warning: ModernIME transaction incomplete or failed (status=%s); materials preserved in: %s\n' "${status:-UNKNOWN}" "$transaction_dir" >&2
            return 1
            ;;
    esac
}

modernime_deploy_payload() {
    local payload_dir=${1:?Usage: modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR}
    local prefix_dir=${2:?Usage: modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR}
    local transaction_dir=${3:?Usage: modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR}

    local canon_prefix
    canon_prefix=$(modernime_canonical_prefix "$prefix_dir")
    local prefix_clean="${prefix_dir%/}"
    local lock_acquired_here=false
    local had_prior_lock=false
    if [[ -n "${MODERNIME_PREFIX_LOCK_FD:-}" && "${MODERNIME_LOCKED_PREFIX:-}" == "$canon_prefix" ]]; then
        had_prior_lock=true
    fi
    if ! modernime_acquire_prefix_lock "$prefix_dir"; then
        return 1
    fi
    if [[ "$had_prior_lock" != true ]]; then
        lock_acquired_here=true
    fi

    if [[ ! -d "$payload_dir" ]]; then
        printf 'Error: Payload directory does not exist: %s\n' "$payload_dir" >&2
        if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
        return 1
    fi
    if [[ ! -d "$transaction_dir" ]]; then
        printf 'Error: Transaction directory does not exist: %s\n' "$transaction_dir" >&2
        if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
        return 1
    fi

    local backup_dir="$transaction_dir/backup"
    local payload_list="$transaction_dir/payload.list"
    local changed_list="$transaction_dir/changed.list"
    local created_dirs="$transaction_dir/created_dirs.list"
    local status_file="$transaction_dir/status"
    local log_file="$transaction_dir/transaction.log"

    mkdir -p -- "$backup_dir"
    : > "$changed_list"
    : > "$created_dirs"
    printf 'PREPARING\n' > "$status_file"
    printf '[%s] Starting deployment transaction for prefix: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$prefix_dir" >> "$log_file"

    # Enumerate all files and symlinks in payload into NUL-separated list.
    find "$payload_dir" \( -type f -o -type l \) -print0 > "$payload_list"

    # Phase 1: Pre-flight backup. Complete backup before touching any live destination file.
    while IFS= read -r -d '' src_file; do
        local rel_path="${src_file#"$payload_dir/"}"
        local dest_file="$prefix_dir/$rel_path"

        # Validate canonical parent containment to prevent symlink traversal escaping prefix
        local check_dir
        check_dir=$(dirname -- "$dest_file")
        while [[ -n "$check_dir" && "$check_dir" != "$prefix_clean" && "$check_dir" != "$canon_prefix" && "$check_dir" != "/" && "$check_dir" != "." ]]; do
            if [[ -e "$check_dir" || -L "$check_dir" ]]; then
                local canon_check
                canon_check=$(readlink -f -- "$check_dir" 2>/dev/null || true)
                if [[ -z "$canon_check" || ( "$canon_check" != "$canon_prefix" && "$canon_check" != "$canon_prefix/"* ) ]]; then
                    printf 'Error: Deployment destination escapes prefix boundaries via symlink: %s (in %s -> %s)\n' "$dest_file" "$check_dir" "$canon_check" >&2
                    printf 'FAILED_BOUNDARY\n' > "$status_file"
                    if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
                    return 1
                fi
            fi
            check_dir=$(dirname -- "$check_dir")
        done
        if [[ -e "$dest_file" && ! -L "$dest_file" ]]; then
            local canon_dest
            canon_dest=$(readlink -f -- "$dest_file" 2>/dev/null || true)
            if [[ -z "$canon_dest" || ( "$canon_dest" != "$canon_prefix" && "$canon_dest" != "$canon_prefix/"* ) ]]; then
                printf 'Error: Destination file escapes prefix boundaries: %s -> %s\n' "$dest_file" "$canon_dest" >&2
                printf 'FAILED_BOUNDARY\n' > "$status_file"
                if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
                return 1
            fi
        fi

        if [[ -e "$dest_file" || -L "$dest_file" ]]; then
            if [[ -d "$dest_file" && ! -L "$dest_file" ]]; then
                printf 'Error: Payload destination collision with existing directory: %s\n' "$dest_file" >&2
                printf 'FAILED_COLLISION\n' > "$status_file"
                if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
                return 1
            fi
            local backup_dest="$backup_dir/$rel_path"
            mkdir -p -- "$(dirname -- "$backup_dest")"
            if ! cp -a -- "$dest_file" "$backup_dest"; then
                printf 'Error: Failed to back up existing file: %s\n' "$dest_file" >&2
                printf 'FAILED_BACKUP\n' > "$status_file"
                if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
                return 1
            fi
        fi
    done < "$payload_list"

    # Phase 2: Atomic deployment with unlinking.
    printf 'DEPLOYING\n' > "$status_file"
    local deploy_failed=false
    while IFS= read -r -d '' src_file; do
        local rel_path="${src_file#"$payload_dir/"}"
        local dest_file="$prefix_dir/$rel_path"
        local parent_dir
        parent_dir=$(dirname -- "$dest_file")

        # Track directory hierarchy creation for rollback
        local missing_dir="$parent_dir"
        while [[ ! -d "$missing_dir" ]]; do
            printf '%s\0' "$missing_dir" >> "$created_dirs"
            local next_dir
            next_dir=$(dirname -- "$missing_dir")
            [[ "$next_dir" != "$missing_dir" ]] || break
            missing_dir="$next_dir"
        done
        mkdir -p -- "$parent_dir"

        # Record into changed list before overwriting
        printf '%s\0' "$rel_path" >> "$changed_list"

        # Always unlink destination (--remove-destination) before copying so active memory-mapped
        # libraries (.so) in running processes are never overwritten in-place.
        if ! cp -a --remove-destination -- "$src_file" "$dest_file"; then
            printf 'Error: Failed to deploy %s to %s\n' "$src_file" "$dest_file" >&2
            deploy_failed=true
            break
        fi
    done < "$payload_list"

    if [[ "$deploy_failed" != true && "${MODERNIME_SIMULATE_INSTALL_FAILURE:-0}" == "1" ]]; then
        printf 'Simulated failure triggered via MODERNIME_SIMULATE_INSTALL_FAILURE=1\n' >&2
        deploy_failed=true
    fi

    if [[ "$deploy_failed" == true ]]; then
        printf '[%s] Deployment failure encountered; executing rollback...\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" >> "$log_file"
        modernime_rollback_payload "$prefix_dir" "$transaction_dir"
        if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
        return 1
    fi

    # Record all deployed files into transaction list and managed-state manifest
    local deployed_list="$transaction_dir/deployed_files.list"
    : > "$deployed_list"
    while IFS= read -r -d '' src_file; do
        local rel_path="${src_file#"$payload_dir/"}"
        printf '%s\n' "$prefix_dir/$rel_path" >> "$deployed_list"
    done < "$payload_list"

    local managed_state_dir="$prefix_dir/share/modernime/managed-files"
    mkdir -p -- "$managed_state_dir"
    cp -- "$deployed_list" "$managed_state_dir/payload.manifest"

    printf 'COMMITTED\n' > "$status_file"
    printf '[%s] Deployment successfully committed to %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$prefix_dir" >> "$log_file"
    if [[ "$lock_acquired_here" == true ]]; then modernime_release_prefix_lock; fi
    return 0
}

modernime_rollback_payload() {
    local prefix_dir=${1:?Usage: modernime_rollback_payload PREFIX TRANSACTION_DIR}
    local transaction_dir=${2:?Usage: modernime_rollback_payload PREFIX TRANSACTION_DIR}

    local backup_dir="$transaction_dir/backup"
    local changed_list="$transaction_dir/changed.list"
    local created_dirs="$transaction_dir/created_dirs.list"
    local status_file="$transaction_dir/status"
    local log_file="$transaction_dir/transaction.log"

    printf 'ROLLING_BACK\n' > "$status_file"
    printf '[%s] Executing payload rollback in %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$prefix_dir" >> "$log_file"

    local rollback_error=false

    # Revert modified/created files in reverse order
    if [[ -f "$changed_list" ]]; then
        while IFS= read -r -d '' rel_path; do
            local dest_file="$prefix_dir/$rel_path"
            local backup_file="$backup_dir/$rel_path"
            if ! modernime_is_safe_removal_target "$dest_file" "$prefix_dir"; then
                printf 'Warning: Skipping unsafe rollback removal target: %s\n' "$dest_file" >&2
                rollback_error=true
                continue
            fi
            if ! rm -f -- "$dest_file"; then
                printf 'Warning: Failed to remove deployed file during rollback: %s\n' "$dest_file" >&2
                rollback_error=true
            fi
            if [[ -e "$backup_file" || -L "$backup_file" ]]; then
                if ! cp -a --remove-destination -- "$backup_file" "$dest_file"; then
                    printf 'Error: Failed to restore backup for %s\n' "$dest_file" >&2
                    rollback_error=true
                fi
            fi
        done < "$changed_list"
    fi

    # Clean up empty directories created by this transaction (deepest first)
    if [[ -f "$created_dirs" ]]; then
        while IFS= read -r -d '' created_dir; do
            rmdir -- "$created_dir" 2>/dev/null || true
        done < <(sort -rz "$created_dirs")
    fi

    # Crucial Invariant: Rollback touches ONLY files in changed_list within PREFIX.
    # User databases, WAL frames, learning entries, and settings.conf are NEVER touched.
    if [[ "$rollback_error" == true ]]; then
        printf 'FAILED_ROLLBACK\n' > "$status_file"
        printf 'Error: Rollback completed with errors; recovery files retained in %s\n' "$transaction_dir" >&2
        return 1
    fi

    printf 'ROLLED_BACK\n' > "$status_file"
    printf '[%s] Payload rollback completed successfully\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" >> "$log_file"
    return 0
}

modernime_integrate() {
    local prefix=${1:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME] [PAYLOAD_SOURCE]}
    local config_home=${2:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME] [PAYLOAD_SOURCE]}
    local desktop_dir=${3:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME] [PAYLOAD_SOURCE]}
    local managed_state_dir=${4:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME] [PAYLOAD_SOURCE]}
    local data_home=${5:-"${MODERNIME_DATA_HOME:-"${XDG_DATA_HOME:-"$HOME/.local/share"}"}"}
    local payload_source=${6:-""}

    local is_system_prefix=false
    if [[ "$prefix" =~ ^/(usr|opt|etc)(/|$) || -n "${MODERNIME_FORCE_SYSTEM_PREFIX:-}" ]]; then
        is_system_prefix=true
    fi

    mkdir -p -- "$managed_state_dir"

    # Detect addon directories
    local candidate_libdirs=("lib/x86_64-linux-gnu" "lib/aarch64-linux-gnu" "lib64" "lib")
    local prefix_addon_dir=""
    for libdir in "${candidate_libdirs[@]}"; do
        if [[ -d "$prefix/$libdir/fcitx5" && -f "$prefix/$libdir/fcitx5/modernime_fcitx5.so" ]]; then
            prefix_addon_dir="$prefix/$libdir/fcitx5"
            break
        fi
    done
    if [[ -z "$prefix_addon_dir" ]]; then
        for libdir in "${candidate_libdirs[@]}"; do
            if [[ -d "$prefix/$libdir/fcitx5" ]]; then
                prefix_addon_dir="$prefix/$libdir/fcitx5"
                break
            fi
        done
    fi
    prefix_addon_dir=${prefix_addon_dir:-"$prefix/lib/fcitx5"}

    local system_addon_dir=""
    local system_libdir
    system_libdir=$(pkg-config --variable=libdir Fcitx5Utils 2>/dev/null || true)
    if [[ -n "$system_libdir" && -d "$system_libdir/fcitx5" ]]; then
        system_addon_dir="$system_libdir/fcitx5"
    fi
    if [[ -z "$system_addon_dir" ]]; then
        for sys_dir in /usr/lib64/fcitx5 /usr/lib/x86_64-linux-gnu/fcitx5 /usr/lib/aarch64-linux-gnu/fcitx5 /usr/lib/fcitx5; do
            if [[ -d "$sys_dir" ]]; then
                system_addon_dir="$sys_dir"
                break
            fi
        done
    fi
    system_addon_dir=${system_addon_dir:-"/usr/lib/fcitx5"}

    local data_directories=${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
    case ":$data_directories:" in
        *":$prefix/share:"*) ;;
        *) data_directories="$prefix/share:$data_directories" ;;
    esac

    local desktop_tmp
    desktop_tmp=$(mktemp "${TMPDIR:-/tmp}/modernime-integ.XXXXXX")
    cleanup_desktop_tmp() {
        rm -f -- "${desktop_tmp:-}"
    }
    trap cleanup_desktop_tmp RETURN

    # 1. Desktop shortcut
    if [[ "$is_system_prefix" != true ]]; then
        mkdir -p -- "$desktop_dir" || return 1
        local desktop_shortcut="$desktop_dir/modernime-settings.desktop"
        local settings_desktop_exec=""
        if [[ -n "${MODERNIME_CONFIG_HOME:-}" || -n "${MODERNIME_DATA_HOME:-}" ]]; then
            settings_desktop_exec="env"
            if [[ -n "${MODERNIME_CONFIG_HOME:-}" ]]; then
                settings_desktop_exec="$settings_desktop_exec $(modernime_desktop_argument "XDG_CONFIG_HOME=$config_home")"
            fi
            if [[ -n "${MODERNIME_DATA_HOME:-}" ]]; then
                settings_desktop_exec="$settings_desktop_exec $(modernime_desktop_argument "XDG_DATA_HOME=$data_home")"
            fi
            settings_desktop_exec="$settings_desktop_exec $(modernime_desktop_argument "$prefix/bin/modernime-settings")"
        else
            settings_desktop_exec="$(modernime_desktop_argument "$prefix/bin/modernime-settings")"
        fi
        {
            printf '%s\n' '[Desktop Entry]'
            printf '%s\n' 'Type=Application'
            printf '%s\n' 'Name=ModernIME 设置'
            printf '%s\n' 'Comment=Configure ModernIME input method'
            printf 'Exec=%s\n' "$settings_desktop_exec"
            printf 'Icon=%s\n' "$(modernime_desktop_string "$prefix/share/icons/hicolor/512x512/apps/modernime.png")"
            printf '%s\n' 'Terminal=false'
            printf '%s\n' 'Categories=Settings;Utility;'
        } > "$desktop_tmp" || return 1
        modernime_write_managed_file "$desktop_tmp" "$desktop_shortcut" desktop "$managed_state_dir" || return 1
        if [[ "$managed_file_written" == true ]]; then
            chmod +x "$desktop_shortcut" || return 1
            if command -v gio >/dev/null 2>&1; then
                gio set "$desktop_shortcut" metadata::trusted true 2>/dev/null || true
            fi
        elif [[ -f "$desktop_shortcut" && ! -L "$desktop_shortcut" && -w "$desktop_shortcut" ]]; then
            local current_icon target_icon
            target_icon="$(modernime_desktop_string "$prefix/share/icons/hicolor/512x512/apps/modernime.png")"
            current_icon=$(grep -m1 '^Icon=' "$desktop_shortcut" 2>/dev/null | cut -d= -f2- || true)
            if [[ "$current_icon" != "$target_icon" ]]; then
                awk -v new_icon="$target_icon" '
                    BEGIN { replaced = 0 }
                    /^Icon=/ { print "Icon=" new_icon; replaced = 1; next }
                    { print }
                    END { if (!replaced) print "Icon=" new_icon }
                ' "$desktop_shortcut" > "$desktop_tmp" || return 1
                cp -f -- "$desktop_tmp" "$desktop_shortcut" || return 1
                chmod +x "$desktop_shortcut" 2>/dev/null || true
                if command -v gio >/dev/null 2>&1; then
                    gio set "$desktop_shortcut" metadata::trusted true 2>/dev/null || true
                fi
            fi
        fi

        # 2. Update installed desktop entry under share/applications
        local application_desktop="$prefix/share/applications/modernime-settings.desktop"
        if [[ -f "$application_desktop" ]]; then
            MODERNIME_DESKTOP_EXEC="$settings_desktop_exec" \
            MODERNIME_DESKTOP_ICON="$(modernime_desktop_string "$prefix/share/icons/hicolor/512x512/apps/modernime.png")" \
                awk '/^Exec=/ { print "Exec=" ENVIRON["MODERNIME_DESKTOP_EXEC"]; next } /^Icon=/ { print "Icon=" ENVIRON["MODERNIME_DESKTOP_ICON"]; next } { print }' \
                    "$application_desktop" > "$desktop_tmp" || return 1
            cp -- "$desktop_tmp" "$application_desktop" || return 1
        fi

        # 3. Environment configuration
        local environment_dir="$config_home/environment.d"
        local environment_file="$environment_dir/90-modernime.conf"
        mkdir -p -- "$environment_dir" || return 1
        {
            printf 'FCITX_ADDON_DIRS="%s:%s"\n' "$prefix_addon_dir" "$system_addon_dir"
            printf 'XDG_DATA_DIRS="%s:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"\n' "$prefix/share"
            if [[ -n "${MODERNIME_CONFIG_HOME:-}" ]]; then
                printf 'XDG_CONFIG_HOME="%s"\n' "$config_home"
            fi
            if [[ -n "${MODERNIME_DATA_HOME:-}" ]]; then
                printf 'XDG_DATA_HOME="%s"\n' "$data_home"
            fi
            printf 'GTK_IM_MODULE=fcitx\n'
            printf 'QT_IM_MODULE=fcitx\n'
            printf 'XMODIFIERS=@im=fcitx\n'
            printf 'SDL_IM_MODULE=fcitx\n'
        } > "$desktop_tmp" || return 1
        modernime_write_managed_file "$desktop_tmp" "$environment_file" environment "$managed_state_dir" || return 1

        # 4. Autostart entry
        local autostart_dir="$config_home/autostart"
        local autostart_file="$autostart_dir/modernime-fcitx5-session.desktop"
        mkdir -p -- "$autostart_dir" || return 1
        local autostart_exec="env $(modernime_desktop_argument "FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir") $(modernime_desktop_argument "XDG_DATA_DIRS=$data_directories") $(modernime_desktop_argument "XDG_CONFIG_HOME=$config_home") $(modernime_desktop_argument "XDG_DATA_HOME=$data_home") fcitx5 -d -u modernime-ui"
        {
            printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=ModernIME Fcitx5'
            printf '%s\n' 'Comment=Start Fcitx5 with the ModernIME candidate bar'
            printf 'Exec=%s\n' "$autostart_exec"
            printf '%s\n' 'Terminal=false' 'X-GNOME-Autostart-enabled=true' 'NoDisplay=true'
        } > "$desktop_tmp" || return 1
        modernime_write_managed_file "$desktop_tmp" "$autostart_file" autostart "$managed_state_dir" || return 1

        # 5. Profile block in ~/.xprofile if applicable
        local skip_xprofile=false
        case "${MODERNIME_SKIP_XPROFILE:-}" in
            1|true|TRUE|yes|YES) skip_xprofile=true ;;
        esac
        local xprofile_file=${MODERNIME_XPROFILE_FILE:-"$HOME/.xprofile"}
        if [[ -z "${MODERNIME_XPROFILE_FILE:-}" && "$config_home" != "$HOME/.config" ]]; then
            skip_xprofile=true
        fi
        if [[ "$skip_xprofile" != true ]]; then
            cat > "$desktop_tmp" <<EOF
# BEGIN MODERNIME ENV
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx
export XMODIFIERS=@im=fcitx
export SDL_IM_MODULE=fcitx
export FCITX_ADDON_DIRS="$prefix_addon_dir:$system_addon_dir"
export XDG_DATA_DIRS="$prefix/share:\${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
# END MODERNIME ENV
EOF
            modernime_write_profile_block "$desktop_tmp" "$xprofile_file" "$managed_state_dir" || return 1
        fi
    fi

    # 6. Compatibility alias symlink for ~/.local/lib/fcitx5
    if [[ "$prefix" == "$HOME/.local" && "$prefix_addon_dir" != "$prefix/lib/fcitx5" && -d "$prefix_addon_dir" ]]; then
        mkdir -p -- "$prefix/lib"
        if [[ ! -e "$prefix/lib/fcitx5" && ! -L "$prefix/lib/fcitx5" ]]; then
            if ln -s "$prefix_addon_dir" "$prefix/lib/fcitx5" 2>/dev/null; then
                printf '%s\n' "$prefix_addon_dir" > "$managed_state_dir/addon-link.target"
            fi
        fi
    fi

    # 7. Write channel metadata
    local manifest_dir="$prefix/share/modernime"
    local channel_file="$manifest_dir/install-channel.txt"
    local manifest_file="$manifest_dir/install-manifest.txt"
    mkdir -p -- "$manifest_dir" || return 1

    local installed_version=${MODERNIME_INSTALLED_VERSION:-}
    if [[ -z "$installed_version" ]]; then
        installed_version="unknown"
    fi
    {
        printf 'channel=script\n'
        printf 'version=%s\n' "$installed_version"
        printf 'prefix=%s\n' "$prefix"
    } > "$channel_file" || return 1

    # 8. Write install manifest from staging payload
    local prefix_clean="${prefix%/}"
    local new_manifest_tmp
    new_manifest_tmp=$(mktemp "${TMPDIR:-/tmp}/modernime-manifest.XXXXXX") || return 1

    # Collect payload items
    local found_payload_source=false
    if [[ -n "$payload_source" && -f "$payload_source" ]]; then
        cat "$payload_source" >> "$new_manifest_tmp"
        found_payload_source=true
    elif [[ -n "$payload_source" && -d "$payload_source" ]]; then
        find "$payload_source" \( -type f -o -type l \) | while IFS= read -r f; do
            printf '%s\n' "$prefix_clean/${f#"$payload_source/"}"
        done >> "$new_manifest_tmp"
        found_payload_source=true
    elif [[ -f "$managed_state_dir/payload.manifest" ]]; then
        cat "$managed_state_dir/payload.manifest" >> "$new_manifest_tmp"
        found_payload_source=true
    fi

    # Fallback to static expected payload if neither payload source was available
    if [[ "$found_payload_source" != true ]]; then
        {
            printf '%s\n' "$prefix_addon_dir/modernime_fcitx5.so"
            printf '%s\n' "$prefix_addon_dir/modernime_ui.so"
            printf '%s\n' "$prefix/share/modernime/pinyin/modernime-knowledge.dict"
            printf '%s\n' "$prefix/share/modernime/pinyin/modernime-hotwords.dict"
            printf '%s\n' "$prefix/share/modernime/pinyin/modernime-english-dict.bin"
            printf '%s\n' "$prefix/share/fcitx5/addon/modernime.conf"
            printf '%s\n' "$prefix/share/fcitx5/addon/modernime-ui.conf"
            printf '%s\n' "$prefix/share/fcitx5/inputmethod/modernime.conf"
            printf '%s\n' "$prefix/bin/modernime-settings"
            printf '%s\n' "$prefix/bin/modernime-session-init"
            printf '%s\n' "$prefix/share/modernime/install_state.sh"
            printf '%s\n' "$prefix/share/modernime/install_lifecycle.sh"
            printf '%s\n' "$prefix/bin/modernime-data-guard"
            printf '%s\n' "$prefix/bin/modernime-migrate-channel"
            printf '%s\n' "$prefix/share/applications/modernime-settings.desktop"
            for icon_size in 16 32 48 64 128 256 512; do
                printf '%s\n' "$prefix/share/icons/hicolor/${icon_size}x${icon_size}/apps/modernime.png"
            done
            printf '%s\n' "$prefix/share/doc/modernime/LICENSE"
            printf '%s\n' "$prefix/share/doc/modernime/copyright"
            printf '%s\n' "$prefix/share/doc/modernime/THIRD_PARTY_NOTICES.md"
            printf '%s\n' "$prefix/share/doc/modernime/SOURCES.json"
            if [[ -d "$prefix/share/modernime/extensions" ]]; then
                find "$prefix/share/modernime/extensions" -type f | sort
            fi
        } >> "$new_manifest_tmp"
    fi

    # Add channel file
    printf '%s\n' "$channel_file" >> "$new_manifest_tmp"

    # Add external managed files
    if [[ "$is_system_prefix" != true ]]; then
        printf '%s\n' "$desktop_dir/modernime-settings.desktop" >> "$new_manifest_tmp"
        printf '%s\n' "$config_home/environment.d/90-modernime.conf" >> "$new_manifest_tmp"
        printf '%s\n' "$config_home/autostart/modernime-fcitx5-session.desktop" >> "$new_manifest_tmp"
        if [[ -f "$managed_state_dir/xprofile.path" ]]; then
            cat "$managed_state_dir/xprofile.path" >> "$new_manifest_tmp"
        fi
    fi

    # Deduplicate and sort new manifest
    local sorted_manifest_tmp
    sorted_manifest_tmp=$(mktemp "${TMPDIR:-/tmp}/modernime-sorted-manifest.XXXXXX") || return 1
    sort -u "$new_manifest_tmp" > "$sorted_manifest_tmp" || return 1
    rm -f -- "$new_manifest_tmp"

    # Prune obsolete files from existing install manifest that are no longer part of new payload
    if [[ -f "$manifest_file" ]]; then
        local canon_prefix
        canon_prefix=$(modernime_canonical_prefix "$prefix")
        while IFS= read -r old_path; do
            [[ -z "$old_path" ]] && continue
            if grep -qxF "$old_path" "$sorted_manifest_tmp" 2>/dev/null; then
                continue
            fi
            if modernime_is_safe_removal_target "$old_path" "$prefix_clean" "$canon_prefix" "$is_system_prefix"; then
                if [[ -f "$old_path" || -L "$old_path" ]]; then
                    rm -f -- "$old_path"
                fi
            fi
        done < "$manifest_file"
    fi

    # Commit final sorted manifest
    mv -f -- "$sorted_manifest_tmp" "$manifest_file" || return 1

    return 0
}
