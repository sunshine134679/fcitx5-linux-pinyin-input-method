#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
package_root=${1:?Usage: generate_binary_installer.sh PACKAGE_ROOT}
package_version=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "$project_root/CMakeLists.txt")
if [[ ! "$package_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf 'Cannot determine the bundled ModernIME version\n' >&2
    exit 1
fi

mkdir -p "$package_root/scripts"

# Copy lifecycle and utility scripts
cp "$project_root/uninstall.sh" "$package_root/uninstall.sh"
cp "$project_root/scripts/install_state.sh" "$package_root/scripts/install_state.sh"
cp "$project_root/scripts/install_lifecycle.sh" "$package_root/scripts/install_lifecycle.sh"
cp "$project_root/scripts/install_binary_payload.sh" "$package_root/scripts/install_binary_payload.sh"
cp "$project_root/scripts/modernime-data-guard" "$package_root/scripts/modernime-data-guard"
cp "$project_root/scripts/modernime-migrate-channel" "$package_root/scripts/modernime-migrate-channel"

# Generate explicit thin binary installer entrypoint without text-slicing install.sh
cat << 'INSTALLER_EOF' > "$package_root/install.sh"
#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
unset MODERNIME_PREFIX_LOCK_FD MODERNIME_LOCKED_PREFIX MODERNIME_LOCK_FILE 2>/dev/null || true
source "$project_root/scripts/install_state.sh"
source "$project_root/scripts/install_lifecycle.sh"

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
explicit_prefix=false
[[ -n "${MODERNIME_PREFIX:-}" ]] && explicit_prefix=true
allow_missing_deps=false
case "${MODERNIME_ALLOW_MISSING_DEPS:-}" in
    1|true|TRUE|yes|YES) allow_missing_deps=true ;;
esac

for arg in "$@"; do
    if [[ "$arg" == "-u" || "$arg" == "--uninstall" ]]; then
        forward_args=()
        for a in "$@"; do
            [[ "$a" == "-u" || "$a" == "--uninstall" ]] && continue
            forward_args+=("$a")
        done
        exec "$project_root/uninstall.sh" "${forward_args[@]}"
    fi
done

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix=*)
            prefix="${1#*=}"
            explicit_prefix=true
            shift
            ;;
        --prefix|-p)
            prefix="$2"
            explicit_prefix=true
            shift 2
            ;;
        --allow-missing-deps)
            allow_missing_deps=true
            shift
            ;;
        --overwrite-package|--force)
            overwrite_package=true
            shift
            ;;
        -*)
            shift
            ;;
        *)
            prefix="$1"
            explicit_prefix=true
            shift
            ;;
    esac
done

if [[ "$project_root" =~ ^/(tmp|var/tmp)/ && "$explicit_prefix" != true ]]; then
    printf 'Error: Running install.sh from temporary directory %s without MODERNIME_PREFIX is not allowed.\n' "$project_root" >&2
    printf 'This prevents accidental installation or modification of your real user environment (%s).\n' "$HOME" >&2
    printf 'Specify MODERNIME_PREFIX explicitly (e.g. MODERNIME_PREFIX=/tmp/test-prefix) if intended.\n' >&2
    exit 1
fi

overwrite_package=${overwrite_package:-false}
case "${MODERNIME_OVERWRITE_PACKAGE:-}" in
    1|true|TRUE|yes|YES) overwrite_package=true ;;
esac

is_system_prefix=false
if [[ "$prefix" =~ ^/(usr|opt|etc)(/|$) || -n "${MODERNIME_FORCE_SYSTEM_PREFIX:-}" ]]; then
    is_system_prefix=true
fi

if [[ "${EUID:-$(id -u)}" -eq 0 && -n "${SUDO_USER:-}" ]]; then
    if [[ "$is_system_prefix" != true && -z "${MODERNIME_ALLOW_ROOT:-}" ]]; then
        printf 'Error: Running install.sh with sudo for user prefix %s is not recommended and will create files owned by root in %s.\n' "$prefix" "$HOME" >&2
        printf 'Install ModernIME for your current user by running: ./install.sh\n' >&2
        printf 'Or install system-wide to /usr by running: sudo ./install.sh /usr\n' >&2
        printf 'If you really want to install to a user directory as root, set MODERNIME_ALLOW_ROOT=1.\n' >&2
        exit 1
    fi
fi

check_runtime_dependencies() {
    local missing_deps=()
    if ! command -v fcitx5 >/dev/null 2>&1; then
        missing_deps+=("fcitx5 (executable not found)")
    fi
    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        if [[ "$allow_missing_deps" == true ]]; then
            printf 'Warning: Missing runtime dependencies: %s\n' "${missing_deps[*]}" >&2
        else
            printf 'Error: Missing required runtime dependencies:\n' >&2
            for dep in "${missing_deps[@]}"; do
                printf '  - %s\n' "$dep" >&2
            done
            exit 1
        fi
    fi
}
check_runtime_dependencies

check_native_package_collision() {
    if [[ "$overwrite_package" == true ]]; then
        return 0
    fi
    if [[ "$is_system_prefix" == true ]]; then
        local collision_files=()
        for check_file in "$prefix/bin/modernime-settings" \
                          "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so" \
                          "$prefix/lib64/fcitx5/modernime_fcitx5.so" \
                          "$prefix/lib/fcitx5/modernime_fcitx5.so"; do
            if [[ -f "$check_file" || -L "$check_file" ]]; then
                local owner=""
                if command -v dpkg >/dev/null 2>&1; then
                    owner=$(dpkg -S "$check_file" 2>/dev/null || true)
                elif command -v rpm >/dev/null 2>&1; then
                    owner=$(rpm -qf "$check_file" 2>/dev/null || true)
                fi
                if [[ -n "$owner" ]]; then
                    collision_files+=("$check_file ($owner)")
                fi
            fi
        done
        if [[ ${#collision_files[@]} -gt 0 ]]; then
            printf 'Error: Detected collision with native package manager:\n' >&2
            for f in "${collision_files[@]}"; do
                printf '  - %s\n' "$f" >&2
            done
            printf 'Installing via script over native system packages may break system package manager state.\n' >&2
            printf 'To proceed anyway, run with --overwrite-package or MODERNIME_OVERWRITE_PACKAGE=1.\n' >&2
            exit 1
        fi
    fi
}
check_native_package_collision

backup_user_database() {
    local guard_cmd=""
    if [[ -x "$project_root/scripts/modernime-data-guard" ]]; then
        guard_cmd="$project_root/scripts/modernime-data-guard"
    elif [[ -x "$prefix/bin/modernime-data-guard" ]]; then
        guard_cmd="$prefix/bin/modernime-data-guard"
    elif command -v modernime-data-guard >/dev/null 2>&1; then
        guard_cmd="modernime-data-guard"
    fi
    if [[ -n "$guard_cmd" ]]; then
        local guard_args=(--pre-upgrade)
        if [[ -n "${MODERNIME_CONFIG_HOME:-}" ]]; then
            guard_args+=(--config-dir "$MODERNIME_CONFIG_HOME/modernime")
        fi
        if [[ -n "${MODERNIME_DATA_HOME:-}" ]]; then
            guard_args+=(--data-dir "$MODERNIME_DATA_HOME/modernime")
        fi
        if ! "$guard_cmd" "${guard_args[@]}"; then
            printf 'Error: Pre-upgrade user data backup/verification failed; aborting installation.\n' >&2
            exit 1
        fi
    fi
}
backup_user_database

config_home=${MODERNIME_CONFIG_HOME:-"${XDG_CONFIG_HOME:-"$HOME/.config"}"}
data_home=${MODERNIME_DATA_HOME:-"${XDG_DATA_HOME:-"$HOME/.local/share"}"}
desktop_dir=${MODERNIME_DESKTOP_DIR:-"$HOME/Desktop"}
if [[ -z "${MODERNIME_DESKTOP_DIR:-}" ]] && command -v xdg-user-dir >/dev/null 2>&1; then
    configured_desktop_dir=$(xdg-user-dir DESKTOP || true)
    [[ -n "$configured_desktop_dir" ]] && desktop_dir="$configured_desktop_dir"
elif [[ -z "${MODERNIME_DESKTOP_DIR:-}" && ! -d "$desktop_dir" && -d "$HOME/桌面" ]]; then
    desktop_dir="$HOME/桌面"
fi

payload_root="$project_root/usr"
if [[ ! -d "$payload_root" ]]; then
    printf 'Error: Binary package payload missing: %s\n' "$payload_root" >&2
    exit 1
fi

if ! modernime_acquire_prefix_lock "$prefix"; then
    exit 1
fi

bin_tx_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-bin-tx.XXXXXX")
cleanup_bin_transaction() {
    local exit_code=$?
    trap - EXIT
    if [[ -n "${bin_tx_dir:-}" && -d "${bin_tx_dir:-}" ]]; then
        local tx_status
        tx_status=$(cat "$bin_tx_dir/status" 2>/dev/null || echo "UNKNOWN")
        case "$tx_status" in
            COMMITTED|ROLLED_BACK)
                rm -rf -- "$bin_tx_dir"
                ;;
            FAILED_ROLLBACK|INTEGRATION_FAILED|*)
                printf 'Transaction materials preserved in: %s\n' "$bin_tx_dir" >&2
                ;;
        esac
    fi
    modernime_release_prefix_lock
    exit "$exit_code"
}
trap cleanup_bin_transaction EXIT

if ! modernime_deploy_payload "$payload_root" "$prefix" "$bin_tx_dir"; then
    tx_status=$(cat "$bin_tx_dir/status" 2>/dev/null || echo "UNKNOWN")
    if [[ "$tx_status" == "ROLLED_BACK" ]]; then
        printf 'Error: Binary payload deployment failed; rollback completed.\n' >&2
    elif [[ "$tx_status" == "FAILED_ROLLBACK" ]]; then
        printf 'Error: Binary payload deployment failed; rollback failed. Recovery files preserved in %s\n' "$bin_tx_dir" >&2
    else
        printf 'Error: Binary payload deployment failed (status=%s). Transaction materials preserved in %s\n' "$tx_status" "$bin_tx_dir" >&2
    fi
    exit 1
fi

printf 'INTEGRATING\n' > "$bin_tx_dir/status"

manifest_dir="$prefix/share/modernime"
managed_state_dir="$manifest_dir/managed-files"
if ! MODERNIME_INSTALLED_VERSION="__PACKAGE_VERSION__" \
    modernime_integrate "$prefix" "$config_home" "$desktop_dir" "$managed_state_dir" "$data_home" "$bin_tx_dir/deployed_files.list"; then
    printf 'INTEGRATION_FAILED\n' > "$bin_tx_dir/status"
    printf 'Error: Integration failed after payload deployment. Transaction materials preserved in: %s\n' "$bin_tx_dir" >&2
    exit 1
fi

printf 'COMMITTED\n' > "$bin_tx_dir/status"
modernime_cleanup_transaction_dir "$bin_tx_dir"
modernime_release_prefix_lock

skip_fcitx_restart=false
case "${MODERNIME_SKIP_FCITX_RESTART:-}" in
    1|true|TRUE|yes|YES) skip_fcitx_restart=true ;;
esac
if [[ "$skip_fcitx_restart" != true && -n "${DISPLAY:-${WAYLAND_DISPLAY:-}}" ]]; then
    if command -v fcitx5-remote >/dev/null 2>&1; then
        fcitx5 -d -r -u modernime-ui 2>/dev/null || true
        fcitx5-remote -s modernime 2>/dev/null || true
    fi
fi

printf 'ModernIME __PACKAGE_VERSION__ installation complete.\n'
INSTALLER_EOF

# Substitute actual bundled package version
sed -i "s/__PACKAGE_VERSION__/$package_version/g" "$package_root/install.sh"

chmod +x "$package_root/install.sh" "$package_root/uninstall.sh" \
         "$package_root/scripts/modernime-data-guard" \
         "$package_root/scripts/modernime-migrate-channel" \
         "$package_root/scripts/install_lifecycle.sh" \
         "$package_root/scripts/install_binary_payload.sh"

bash -n "$package_root/install.sh" "$package_root/uninstall.sh" "$package_root/scripts/modernime-migrate-channel"
