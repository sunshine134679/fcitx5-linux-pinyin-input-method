#!/usr/bin/env bash
# ModernIME Explicit Install Lifecycle Library
# Provides explicit, safe, atomic deployment and desktop/environment integration.
#
# Core Interfaces:
#   modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR
#   modernime_rollback_payload PREFIX TRANSACTION_DIR
#   modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR
set -euo pipefail

modernime_deploy_payload() {
    local payload_dir=${1:?Usage: modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR}
    local prefix_dir=${2:?Usage: modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR}
    local transaction_dir=${3:?Usage: modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR}

    if [[ ! -d "$payload_dir" ]]; then
        printf 'Error: Payload directory does not exist: %s\n' "$payload_dir" >&2
        return 1
    fi
    if [[ ! -d "$transaction_dir" ]]; then
        printf 'Error: Transaction directory does not exist: %s\n' "$transaction_dir" >&2
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
        if [[ -e "$dest_file" || -L "$dest_file" ]]; then
            if [[ -d "$dest_file" && ! -L "$dest_file" ]]; then
                printf 'Error: Payload destination collision with existing directory: %s\n' "$dest_file" >&2
                printf 'FAILED_COLLISION\n' > "$status_file"
                return 1
            fi
            local backup_dest="$backup_dir/$rel_path"
            mkdir -p -- "$(dirname -- "$backup_dest")"
            if ! cp -a -- "$dest_file" "$backup_dest"; then
                printf 'Error: Failed to back up existing file: %s\n' "$dest_file" >&2
                printf 'FAILED_BACKUP\n' > "$status_file"
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
        return 1
    fi

    printf 'COMMITTED\n' > "$status_file"
    printf '[%s] Deployment successfully committed to %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$prefix_dir" >> "$log_file"
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
    local prefix=${1:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME]}
    local config_home=${2:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME]}
    local desktop_dir=${3:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME]}
    local managed_state_dir=${4:?Usage: modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME]}
    local data_home=${5:-"${MODERNIME_DATA_HOME:-"${XDG_DATA_HOME:-"$HOME/.local/share"}"}"}

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
        rm -f -- "$desktop_tmp"
    }
    trap cleanup_desktop_tmp RETURN

    # 1. Desktop shortcut
    if [[ "$is_system_prefix" != true ]]; then
        mkdir -p -- "$desktop_dir"
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
        } > "$desktop_tmp"
        modernime_write_managed_file "$desktop_tmp" "$desktop_shortcut" desktop "$managed_state_dir"
        if [[ "$managed_file_written" == true ]]; then
            chmod +x "$desktop_shortcut"
            if command -v gio >/dev/null 2>&1; then
                gio set "$desktop_shortcut" metadata::trusted true 2>/dev/null || true
            fi
        fi

        # 2. Update installed desktop entry under share/applications
        local application_desktop="$prefix/share/applications/modernime-settings.desktop"
        if [[ -f "$application_desktop" ]]; then
            MODERNIME_DESKTOP_EXEC="$settings_desktop_exec" \
            MODERNIME_DESKTOP_ICON="$(modernime_desktop_string "$prefix/share/icons/hicolor/512x512/apps/modernime.png")" \
                awk '/^Exec=/ { print "Exec=" ENVIRON["MODERNIME_DESKTOP_EXEC"]; next } /^Icon=/ { print "Icon=" ENVIRON["MODERNIME_DESKTOP_ICON"]; next } { print }' \
                    "$application_desktop" > "$desktop_tmp"
            cp -- "$desktop_tmp" "$application_desktop"
        fi

        # 3. Environment configuration
        local environment_dir="$config_home/environment.d"
        local environment_file="$environment_dir/90-modernime.conf"
        mkdir -p -- "$environment_dir"
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
        } > "$desktop_tmp"
        modernime_write_managed_file "$desktop_tmp" "$environment_file" environment "$managed_state_dir"

        # 4. Autostart entry
        local autostart_dir="$config_home/autostart"
        local autostart_file="$autostart_dir/modernime-fcitx5-session.desktop"
        mkdir -p -- "$autostart_dir"
        local autostart_exec="env $(modernime_desktop_argument "FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir") $(modernime_desktop_argument "XDG_DATA_DIRS=$data_directories") $(modernime_desktop_argument "XDG_CONFIG_HOME=$config_home") $(modernime_desktop_argument "XDG_DATA_HOME=$data_home") fcitx5 -d -u modernime-ui"
        {
            printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=ModernIME Fcitx5'
            printf '%s\n' 'Comment=Start Fcitx5 with the ModernIME candidate bar'
            printf 'Exec=%s\n' "$autostart_exec"
            printf '%s\n' 'Terminal=false' 'X-GNOME-Autostart-enabled=true' 'NoDisplay=true'
        } > "$desktop_tmp"
        modernime_write_managed_file "$desktop_tmp" "$autostart_file" autostart "$managed_state_dir"

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
            modernime_write_profile_block "$desktop_tmp" "$xprofile_file" "$managed_state_dir"
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
    mkdir -p -- "$manifest_dir"

    local installed_version=${MODERNIME_INSTALLED_VERSION:-}
    if [[ -z "$installed_version" ]]; then
        installed_version="unknown"
    fi
    {
        printf 'channel=script\n'
        printf 'version=%s\n' "$installed_version"
        printf 'prefix=%s\n' "$prefix"
    } > "$channel_file"

    # 8. Write install manifest
    {
        printf '%s\n' "$prefix_addon_dir/modernime_fcitx5.so"
        printf '%s\n' "$prefix_addon_dir/modernime_ui.so"
        printf '%s\n' "$prefix/share/modernime/pinyin/modernime-knowledge.dict"
        printf '%s\n' "$prefix/share/modernime/pinyin/modernime-hotwords.dict"
        printf '%s\n' "$prefix/share/modernime/pinyin/modernime-english-dict.bin"
        printf '%s\n' "$channel_file"
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
        if [[ "$is_system_prefix" != true ]]; then
            printf '%s\n' "$desktop_dir/modernime-settings.desktop"
            printf '%s\n' "$config_home/environment.d/90-modernime.conf"
            printf '%s\n' "$config_home/autostart/modernime-fcitx5-session.desktop"
            if [[ -f "$managed_state_dir/xprofile.path" ]]; then
                cat "$managed_state_dir/xprofile.path"
            fi
        fi
    } > "$manifest_file"

    return 0
}
