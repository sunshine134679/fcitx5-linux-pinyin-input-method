#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source "$project_root/scripts/install_state.sh"

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
explicit_prefix=false
[[ -n "${MODERNIME_PREFIX:-}" ]] && explicit_prefix=true
allow_missing_deps=false
case "${MODERNIME_ALLOW_MISSING_DEPS:-}" in
    1|true|TRUE|yes|YES)
        allow_missing_deps=true
        ;;
esac

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
    1|true|TRUE|yes|YES)
        overwrite_package=true
        ;;
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

required_commands=(cmake ctest pkg-config)
for required_command in "${required_commands[@]}"; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        printf 'Required build command not found: %s\n' "$required_command" >&2
        printf 'Install the build dependencies listed in README.md and try again.\n' >&2
        exit 1
    fi
done

check_cmake_version() {
    if command -v cmake >/dev/null 2>&1; then
        local cmake_ver
        cmake_ver=$(cmake --version 2>/dev/null | head -n1 | grep -oP '\d+\.\d+(\.\d+)?' | head -n1 || true)
        if [[ -n "$cmake_ver" ]]; then
            local major minor
            major=$(echo "$cmake_ver" | cut -d. -f1)
            minor=$(echo "$cmake_ver" | cut -d. -f2)
            if (( major < 3 || (major == 3 && minor < 24) )); then
                printf 'Error: CMake 3.24 or higher is required to build ModernIME from source, but found %s.\n' "$cmake_ver" >&2
                printf 'On Ubuntu 22.04 LTS, upgrade CMake via Kitware APT repository or snap, or use the prebuilt binary package.\n' >&2
                exit 1
            fi
        fi
    fi
}
check_cmake_version

check_runtime_dependencies() {
    local missing_deps=()
    if ! command -v fcitx5 >/dev/null 2>&1; then
        missing_deps+=("fcitx5 (executable not found)")
    fi

    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        if [[ "$allow_missing_deps" == true ]]; then
            printf 'Warning: Missing runtime dependencies: %s\n' "${missing_deps[*]}" >&2
            printf 'Proceeding with installation because --allow-missing-deps or MODERNIME_ALLOW_MISSING_DEPS=1 is set.\n' >&2
        else
            printf 'Error: Missing required runtime dependencies:\n' >&2
            for dep in "${missing_deps[@]}"; do
                printf '  - %s\n' "$dep" >&2
            done
            printf '\n' >&2
            printf 'ModernIME requires the Fcitx5 input method framework to run.\n' >&2
            printf 'Please install Fcitx5 before installing ModernIME:\n' >&2
            printf '  Ubuntu / Debian: sudo apt install fcitx5 fcitx5-chinese-addons\n' >&2
            printf '  openEuler / Fedora: sudo dnf install fcitx5 fcitx5-chinese-addons\n' >&2
            printf '\n' >&2
            printf 'To deploy files without runtime verification (e.g. into an offline root/chroot), run with:\n' >&2
            printf '  ./install.sh --allow-missing-deps\n' >&2
            exit 1
        fi
    fi
}
check_runtime_dependencies

check_runtime_dictionaries() {
    local has_sc_dict=false
    local has_lm=false
    for p in /usr/share/libime/sc.dict /usr/local/share/libime/sc.dict "$prefix/share/libime/sc.dict"; do
        if [[ -f "$p" ]]; then has_sc_dict=true; break; fi
    done
    for p in /usr/share/libime/zh_CN.lm /usr/lib64/libime/zh_CN.lm /usr/local/share/libime/zh_CN.lm "$prefix/share/libime/zh_CN.lm"; do
        if [[ -f "$p" ]]; then has_lm=true; break; fi
    done
    if [[ "$has_sc_dict" != true || "$has_lm" != true ]]; then
        printf 'Notice: System LibIME dictionary (sc.dict) or language model (zh_CN.lm) not detected in standard system locations.\n'
        printf 'ModernIME includes its own offline knowledge dictionary, but recommends installing libime-data / libime-data-language-model for full pinyin vocabulary.\n'
    fi
}
check_runtime_dictionaries

check_native_package_collision() {
    if [[ "$overwrite_package" == true ]]; then
        return 0
    fi
    local existing_channel_file="$prefix/share/modernime/install-channel.txt"
    local existing_manifest_file="$prefix/share/modernime/install-manifest.txt"
    local existing_channel=""
    if [[ -f "$existing_channel_file" ]]; then
        existing_channel=$(grep -E '^channel=' "$existing_channel_file" 2>/dev/null | head -n 1 | cut -d= -f2- || true)
    fi
    if [[ -n "$existing_channel" && "$existing_channel" != "script" ]]; then
        printf 'Error: Existing ModernIME installation at %s belongs to channel "%s" (expected "script").\n' "$prefix" "$existing_channel" >&2
        printf 'Overwriting a native package or mismatched installation channel directly via install.sh is blocked.\n' >&2
        printf 'To inspect or migrate channels safely:\n' >&2
        printf '  %s/scripts/modernime-migrate-channel --check --from-prefix "%s"\n' "$project_root" "$prefix" >&2
        printf 'Or rerun with --overwrite-package (or MODERNIME_OVERWRITE_PACKAGE=1) to force overwrite.\n' >&2
        exit 1
    fi

    if [[ "$is_system_prefix" != true ]]; then
        if [[ -e "$prefix/bin/modernime-settings" && ! -f "$existing_manifest_file" && "$existing_channel" != "script" ]]; then
            printf 'Error: Detected an untracked ModernIME installation of unknown origin at %s/bin/modernime-settings (missing install-manifest.txt).\n' "$prefix" >&2
            printf 'Refusing to overwrite an unknown installation source by default.\n' >&2
            printf 'Inspect with: %s/scripts/modernime-migrate-channel --check --from-prefix "%s"\n' "$project_root" "$prefix" >&2
            printf 'Or rerun with --overwrite-package (or MODERNIME_OVERWRITE_PACKAGE=1) to override.\n' >&2
            exit 1
        fi
        return 0
    fi

    local candidate_files=(
        "$prefix/bin/modernime-settings"
        "$prefix/bin/modernime-session-init"
        "$prefix/bin/modernime-data-guard"
        "$prefix/bin/modernime-migrate-channel"
        "$prefix/share/fcitx5/addon/modernime.conf"
        "$prefix/share/fcitx5/addon/modernime-ui.conf"
        "$prefix/share/fcitx5/inputmethod/modernime.conf"
        "$prefix/share/applications/modernime-settings.desktop"
    )
    for cdir in "$prefix/lib/fcitx5" "$prefix/lib64/fcitx5" "$prefix/lib/x86_64-linux-gnu/fcitx5" "$prefix/lib/aarch64-linux-gnu/fcitx5"; do
        candidate_files+=("$cdir/modernime_fcitx5.so" "$cdir/modernime_ui.so")
    done

    for f in "${candidate_files[@]}"; do
        if [[ -e "$f" || -L "$f" ]]; then
            local owner=""
            if command -v dpkg >/dev/null 2>&1; then
                owner=$(dpkg -S "$f" 2>/dev/null || true)
            elif command -v rpm >/dev/null 2>&1; then
                owner=$(rpm -qf "$f" 2>/dev/null || true)
            fi
            if [[ -n "$owner" ]]; then
                printf 'Error: Existing file %s is managed by native system package: %s\n' "$f" "$owner" >&2
                printf 'Installing via script directly into system prefix %s conflicts with your package manager.\n' "$prefix" >&2
                printf 'To avoid broken dependencies and package manager state corruption:\n' >&2
                printf '  1. Upgrade or remove the native package using apt or dnf first, or\n' >&2
                printf '  2. Rerun with --overwrite-package (or MODERNIME_OVERWRITE_PACKAGE=1) to override.\n' >&2
                exit 1
            fi
        fi
    done

    if [[ -e "$prefix/bin/modernime-settings" && ! -f "$existing_manifest_file" && "$existing_channel" != "script" ]]; then
        printf 'Error: Detected an untracked ModernIME installation of unknown origin at %s/bin/modernime-settings.\n' "$prefix" >&2
        printf 'Refusing to overwrite an unknown installation source in system prefix %s.\n' "$prefix" >&2
        printf 'Inspect with: %s/scripts/modernime-migrate-channel --check --from-prefix "%s"\n' "$project_root" "$prefix" >&2
        printf 'Or rerun with --overwrite-package (or MODERNIME_OVERWRITE_PACKAGE=1) to override.\n' >&2
        exit 1
    fi
}
check_native_package_collision

backup_user_database() {
    if [[ "$is_system_prefix" == true ]]; then
        return 0
    fi
    local data_dir="${MODERNIME_DATA_HOME:-"${XDG_DATA_HOME:-"$HOME/.local/share"}"}/modernime"
    local cfg_dir="${MODERNIME_CONFIG_HOME:-"${XDG_CONFIG_HOME:-"$HOME/.config"}"}/modernime"
    if [[ ! -d "$data_dir" && ! -d "$cfg_dir" ]]; then
        return 0
    fi
    if [[ ! -f "$project_root/scripts/modernime-data-guard" ]]; then
        printf 'Error: Required data protection tool not found: %s/scripts/modernime-data-guard\n' "$project_root" >&2
        return 1
    fi
    local py_cmd="python3"
    if ! command -v python3 >/dev/null 2>&1 && [[ -x /usr/bin/python3 ]]; then
        py_cmd="/usr/bin/python3"
    fi
    if ! "$py_cmd" "$project_root/scripts/modernime-data-guard" --pre-upgrade \
        --data-dir "$data_dir" \
        --config-dir "$cfg_dir"; then
        printf 'Error: Transactional pre-upgrade backup or integrity check failed; aborting installation to preserve existing version.\n' >&2
        return 1
    fi
}
backup_user_database

build_dir=${MODERNIME_BUILD_DIR:-"$project_root/build/install-debug"}
generator=${CMAKE_GENERATOR:-"Unix Makefiles"}
config_home=${MODERNIME_CONFIG_HOME:-"${XDG_CONFIG_HOME:-"$HOME/.config"}"}
environment_dir="$config_home/environment.d"
environment_file="$environment_dir/90-modernime.conf"
autostart_dir="$config_home/autostart"
autostart_file="$autostart_dir/modernime-fcitx5-session.desktop"
system_libdir=$(pkg-config --variable=libdir Fcitx5Utils 2>/dev/null || true)
if [[ -z "$system_libdir" ]]; then
    if [[ -d "/usr/lib64/fcitx5" ]]; then
        system_libdir="/usr/lib64"
    elif [[ -d "/usr/lib/x86_64-linux-gnu/fcitx5" ]]; then
        system_libdir="/usr/lib/x86_64-linux-gnu"
    elif [[ -d "/usr/lib/aarch64-linux-gnu/fcitx5" ]]; then
        system_libdir="/usr/lib/aarch64-linux-gnu"
    else
        system_libdir="/usr/lib"
    fi
fi
system_addon_dir="$system_libdir/fcitx5"

desktop_dir=${MODERNIME_DESKTOP_DIR:-"$HOME/Desktop"}
if [[ -z "${MODERNIME_DESKTOP_DIR:-}" ]] && command -v xdg-user-dir >/dev/null 2>&1; then
    configured_desktop_dir=$(xdg-user-dir DESKTOP || true)
    if [[ -n "$configured_desktop_dir" ]]; then
        desktop_dir="$configured_desktop_dir"
    fi
elif [[ -z "${MODERNIME_DESKTOP_DIR:-}" && ! -d "$desktop_dir" && -d "$HOME/桌面" ]]; then
    desktop_dir="$HOME/桌面"
fi
desktop_shortcut="$desktop_dir/modernime-settings.desktop"
desktop_tmp=""
cleanup_desktop_tmp() {
    if [[ -n "$desktop_tmp" ]]; then
        rm -f -- "$desktop_tmp"
    fi
}
trap cleanup_desktop_tmp EXIT

prefix_addon_dir=""
if [[ -d "$prefix/lib64/fcitx5" ]]; then
    prefix_addon_dir="$prefix/lib64/fcitx5"
elif [[ -d "$prefix/lib/fcitx5" ]]; then
    prefix_addon_dir="$prefix/lib/fcitx5"
elif [[ -d "/usr/lib64" && ! -d "/usr/lib/x86_64-linux-gnu" ]]; then
    prefix_addon_dir="$prefix/lib64/fcitx5"
else
    prefix_addon_dir="$prefix/lib/fcitx5"
fi

data_directories=${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
case ":$data_directories:" in
    *":$prefix/share:"*) ;;
    *) data_directories="$prefix/share:$data_directories" ;;
esac

fcitx_environment=(
    env
    "FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir"
    "XDG_DATA_DIRS=$data_directories"
    "XDG_CONFIG_HOME=$config_home"
)
for environment_name in DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR; do
    if [[ -n "${!environment_name:-}" ]]; then
        fcitx_environment+=("$environment_name=${!environment_name}")
    fi
done

cmake_args=(
    -DCMAKE_BUILD_TYPE="${MODERNIME_BUILD_TYPE:-Release}"
    -DCMAKE_INSTALL_PREFIX:PATH="$prefix"
    -DMODERNIME_BUILD_FCITX5=ON
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON
    -DMODERNIME_BUILD_SETTINGS=ON
    -DMODERNIME_BUILD_TESTS=ON
)
if [[ -n "${MODERNIME_BOOST_ROOT:-}" ]]; then
    cmake_args+=("-DBoost_ROOT=$MODERNIME_BOOST_ROOT")
fi

build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc 2>/dev/null || echo 2)}"
cmake -S "$project_root" -B "$build_dir" -G "$generator" "${cmake_args[@]}"
cmake --build "$build_dir" --parallel "$build_jobs"

skip_tests=false
case "${MODERNIME_SKIP_TESTS:-}" in
    1|true|TRUE|yes|YES)
        skip_tests=true
        ;;
esac

if [[ "$skip_tests" == true ]]; then
    printf 'Tests skipped by MODERNIME_SKIP_TESTS=1\n'
else
    (
        unset MODERNIME_SKIP_FCITX_RESTART
        unset MODERNIME_PREFIX MODERNIME_BUILD_DIR MODERNIME_CONFIG_HOME MODERNIME_DATA_HOME MODERNIME_DESKTOP_DIR XDG_DATA_HOME
        # The GTK focus integration tests require a controlled compositor. Running
        # them inside an arbitrary desktop session makes window-manager focus
        # stealing prevention look like a product failure. Keep the installer test
        # run headless; those tests return CTest's configured skip code, while the
        # remaining suite still runs normally. The original desktop environment is
        # restored automatically when this subshell exits.
        unset DISPLAY WAYLAND_DISPLAY GDK_BACKEND BROADWAY_DISPLAY
        ctest --test-dir "$build_dir" --output-on-failure --parallel "$build_jobs"
    )
fi

# Stage the CMake payload before touching the live prefix. Only payload paths
# participate in the transaction: a shared ~/.local or /usr can be arbitrarily
# large and must never be copied or restored wholesale.
src_transaction_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-src-transaction.XXXXXX")
src_stage_dir="$src_transaction_dir/stage"
src_backup_dir="$src_transaction_dir/backup"
src_payload_list="$src_transaction_dir/payload"
src_changed_list="$src_transaction_dir/changed"
src_created_dirs="$src_transaction_dir/directories"
src_deployment_started=false
restore_src_on_failure() {
    local dest_file rel_path created_dir restore_failed=false
    printf 'Error encountered during source installation; restoring previous version...\n' >&2
    while IFS= read -r -d '' rel_path; do
        dest_file="$prefix/$rel_path"
        if ! rm -f -- "$dest_file"; then
            restore_failed=true
            continue
        fi
        if [[ -e "$src_backup_dir/$rel_path" || -L "$src_backup_dir/$rel_path" ]]; then
            if ! cp -a --remove-destination -- "$src_backup_dir/$rel_path" "$dest_file"; then
                restore_failed=true
            fi
        fi
    done < "$src_changed_list"
    # Deeper paths sort after their parents; reverse order removes children first.
    while IFS= read -r -d '' created_dir; do
        rmdir -- "$created_dir" 2>/dev/null || true
    done < <(sort -rz "$src_created_dirs")
    # Payload deployment never rewrites live learning or user configuration.
    # Restoring their pre-build snapshot would discard writes made during upgrade.
    if [[ "$restore_failed" == true ]]; then
        printf 'Error: Rollback incomplete; recovery files retained at %s\n' "$src_transaction_dir" >&2
        return 1
    fi
    printf 'Rollback to previous version completed.\n' >&2
}
cleanup_src_transaction() {
    local exit_code=$? retain_backup=false
    trap - EXIT
    if [[ "$exit_code" -ne 0 && "$src_deployment_started" == true ]]; then
        if ! restore_src_on_failure; then
            retain_backup=true
        fi
    fi
    if [[ "$retain_backup" != true ]]; then
        rm -rf -- "$src_transaction_dir"
    fi
    cleanup_desktop_tmp
    exit "$exit_code"
}
trap cleanup_src_transaction EXIT
mkdir -p -- "$src_stage_dir" "$src_backup_dir"
: > "$src_changed_list"
: > "$src_created_dirs"

DESTDIR="$src_stage_dir" cmake --install "$build_dir" --prefix "$prefix"
src_payload_dir="$src_stage_dir/${prefix#/}"
# A CMake project with no selected install targets can have an empty payload.
mkdir -p -- "$src_payload_dir"
find "$src_payload_dir" \( -type f -o -type l \) -print0 > "$src_payload_list"

# Complete every backup before the first live file is replaced. Reject directory
# and special-file collisions rather than backing up unrelated directory trees.
while IFS= read -r -d '' src_file; do
    rel_path=${src_file#"$src_payload_dir/"}
    dest_file="$prefix/$rel_path"
    if [[ -e "$dest_file" || -L "$dest_file" ]]; then
        if [[ ! -f "$dest_file" && ! -L "$dest_file" ]]; then
            printf 'Error: Install payload destination is not a file: %s\n' "$dest_file" >&2
            exit 1
        fi
        mkdir -p -- "$src_backup_dir/$(dirname -- "$rel_path")"
        if ! cp -a -- "$dest_file" "$src_backup_dir/$rel_path"; then
            printf 'Error: Failed to back up %s; installation aborted before deployment.\n' "$dest_file" >&2
            exit 1
        fi
    fi
done < "$src_payload_list"

src_deployment_started=true
while IFS= read -r -d '' src_file; do
    rel_path=${src_file#"$src_payload_dir/"}
    dest_file="$prefix/$rel_path"
    parent_dir=$(dirname -- "$dest_file")
    missing_dir="$parent_dir"
    while [[ ! -d "$missing_dir" ]]; do
        printf '%s\0' "$missing_dir" >> "$src_created_dirs"
        next_dir=$(dirname -- "$missing_dir")
        [[ "$next_dir" != "$missing_dir" ]] || break
        missing_dir="$next_dir"
    done
    mkdir -p -- "$parent_dir"
    # Record before copying, since a failed copy may already have replaced the
    # destination. Unlinking also protects plugins mapped by running Fcitx5.
    printf '%s\0' "$rel_path" >> "$src_changed_list"
    cp -a --remove-destination -- "$src_file" "$dest_file"
done < "$src_payload_list"
if [[ "${MODERNIME_SIMULATE_INSTALL_FAILURE:-0}" == "1" ]]; then
    printf 'Error: Simulated mid-upgrade installation failure via MODERNIME_SIMULATE_INSTALL_FAILURE=1\n' >&2
    exit 1
fi
src_deployment_started=false
rm -rf -- "$src_transaction_dir"
trap cleanup_desktop_tmp EXIT

cmake_libdir=$(grep -m1 '^CMAKE_INSTALL_LIBDIR:PATH=' "$build_dir/CMakeCache.txt" 2>/dev/null || true)
cmake_libdir=${cmake_libdir#*=}
cmake_libdir=${cmake_libdir:-lib}

for candidate_dir in "$prefix/$cmake_libdir/fcitx5" \
                     "$prefix/lib/x86_64-linux-gnu/fcitx5" \
                     "$prefix/lib/aarch64-linux-gnu/fcitx5" \
                     "$prefix/lib64/fcitx5" \
                     "$prefix/lib/fcitx5"; do
    if [[ -d "$candidate_dir" && -f "$candidate_dir/modernime_fcitx5.so" ]]; then
        prefix_addon_dir="$candidate_dir"
        break
    fi
done
if [[ -z "$prefix_addon_dir" ]]; then
    if [[ -d "$prefix/$cmake_libdir/fcitx5" ]]; then
        prefix_addon_dir="$prefix/$cmake_libdir/fcitx5"
    elif [[ -d "/usr/lib/x86_64-linux-gnu" && -d "$prefix/lib/x86_64-linux-gnu/fcitx5" ]]; then
        prefix_addon_dir="$prefix/lib/x86_64-linux-gnu/fcitx5"
    elif [[ -d "/usr/lib64" && ! -d "/usr/lib/x86_64-linux-gnu" ]]; then
        prefix_addon_dir="$prefix/lib64/fcitx5"
    else
        prefix_addon_dir="$prefix/lib/fcitx5"
    fi
fi

migrate_legacy_plugins() {
    local target_prefix=$1
    local current_addon_dir=$2
    local current_addon_real
    current_addon_real=$(readlink -f "$current_addon_dir" 2>/dev/null || true)
    local candidate_dirs=(
        "$target_prefix/lib/fcitx5"
        "$target_prefix/lib64/fcitx5"
        "$target_prefix/lib/x86_64-linux-gnu/fcitx5"
        "$target_prefix/lib/aarch64-linux-gnu/fcitx5"
    )
    for cdir in "${candidate_dirs[@]}"; do
        if [[ -L "$cdir" ]]; then
            continue
        fi
        local cdir_real
        cdir_real=$(readlink -f "$cdir" 2>/dev/null || true)
        if [[ -n "$current_addon_real" && -n "$cdir_real" && "$cdir_real" == "$current_addon_real" ]]; then
            continue
        fi
        if [[ "$cdir" != "$current_addon_dir" && -d "$cdir" ]]; then
            for old_plugin in "$cdir/modernime_fcitx5.so" "$cdir/modernime_ui.so"; do
                if [[ -f "$old_plugin" || -L "$old_plugin" ]]; then
                    local plugin_real
                    plugin_real=$(readlink -f "$old_plugin" 2>/dev/null || true)
                    if [[ -n "$current_addon_real" && "$plugin_real" == "$current_addon_real/$(basename -- "$old_plugin")" ]]; then
                        continue
                    fi
                    printf 'Migrating legacy plugin layout: removing obsolete %s\n' "$old_plugin"
                    rm -f -- "$old_plugin"
                fi
            done
            rmdir --ignore-fail-on-non-empty "$cdir" 2>/dev/null || true
            rmdir --ignore-fail-on-non-empty "$(dirname -- "$cdir")" 2>/dev/null || true
        fi
    done
    if [[ -f "$target_prefix/share/modernime/install-manifest.txt" ]]; then
        while IFS= read -r old_path; do
            [[ -z "$old_path" ]] && continue
            case "$old_path" in
                "$target_prefix"/lib*/fcitx5/modernime_fcitx5.so|\
                "$target_prefix"/lib*/fcitx5/modernime_ui.so|\
                "$target_prefix"/lib/*-linux-gnu/fcitx5/modernime_fcitx5.so|\
                "$target_prefix"/lib/*-linux-gnu/fcitx5/modernime_ui.so)
                    local base_name
                    base_name=$(basename -- "$old_path")
                    if [[ "$old_path" != "$current_addon_dir/$base_name" && -f "$old_path" ]]; then
                        local old_real
                        old_real=$(readlink -f "$old_path" 2>/dev/null || true)
                        if [[ -n "$current_addon_real" && "$old_real" != "$current_addon_real/$base_name" ]]; then
                            printf 'Migrating legacy manifest plugin: removing obsolete %s\n' "$old_path"
                            rm -f -- "$old_path"
                            rmdir --ignore-fail-on-non-empty "$(dirname -- "$old_path")" 2>/dev/null || true
                            rmdir --ignore-fail-on-non-empty "$(dirname -- "$(dirname -- "$old_path")")" 2>/dev/null || true
                        fi
                    fi
                    ;;
            esac
        done < "$target_prefix/share/modernime/install-manifest.txt"
    fi
}
migrate_legacy_plugins "$prefix" "$prefix_addon_dir"

if [[ "$prefix" == "$HOME/.local" && "$prefix_addon_dir" != "$prefix/lib/fcitx5" && -d "$prefix_addon_dir" ]]; then
    mkdir -p "$prefix/lib"
    if [[ ! -e "$prefix/lib/fcitx5" && ! -L "$prefix/lib/fcitx5" ]]; then
        if ln -s "$prefix_addon_dir" "$prefix/lib/fcitx5" 2>/dev/null; then
            mkdir -p "$prefix/share/modernime/managed-files"
            printf '%s\n' "$prefix_addon_dir" > "$prefix/share/modernime/managed-files/addon-link.target"
        fi
    fi
fi

fcitx_environment=(
    env
    "FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir"
    "XDG_DATA_DIRS=$data_directories"
    "XDG_CONFIG_HOME=$config_home"
)
for environment_name in DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR; do
    if [[ -n "${!environment_name:-}" ]]; then
        fcitx_environment+=("$environment_name=${!environment_name}")
    fi
done

if [[ "$is_system_prefix" != true ]]; then
    mkdir -p "$desktop_dir"
    desktop_tmp=$(mktemp "$desktop_dir/.modernime-settings.XXXXXX")
    {
        printf '%s\n' '[Desktop Entry]'
        printf '%s\n' 'Type=Application'
        printf '%s\n' 'Name=ModernIME 设置'
        printf '%s\n' 'Comment=Configure ModernIME input method'
        printf 'Exec=%s\n' "$(modernime_desktop_argument "$prefix/bin/modernime-settings")"
        printf 'Icon=%s\n' "$(modernime_desktop_string "$prefix/share/icons/hicolor/512x512/apps/modernime.png")"
        printf '%s\n' 'Terminal=false'
        printf '%s\n' 'Categories=Settings;Utility;'
    } > "$desktop_tmp"
    manifest_dir="$prefix/share/modernime"
    managed_state_dir="$manifest_dir/managed-files"
    modernime_write_managed_file "$desktop_tmp" "$desktop_shortcut" desktop
    if [[ "$managed_file_written" == true ]]; then
        chmod +x "$desktop_shortcut"
        if command -v gio >/dev/null 2>&1; then
            gio set "$desktop_shortcut" metadata::trusted true 2>/dev/null || true
        fi
    fi

    application_desktop="$prefix/share/applications/modernime-settings.desktop"
    if [[ -f "$application_desktop" ]]; then
        MODERNIME_DESKTOP_EXEC="$(modernime_desktop_argument "$prefix/bin/modernime-settings")" \
        MODERNIME_DESKTOP_ICON="$(modernime_desktop_string "$prefix/share/icons/hicolor/512x512/apps/modernime.png")" \
            awk '/^Exec=/ { print "Exec=" ENVIRON["MODERNIME_DESKTOP_EXEC"]; next } /^Icon=/ { print "Icon=" ENVIRON["MODERNIME_DESKTOP_ICON"]; next } { print }' \
                "$application_desktop" > "$desktop_tmp"
        cp -- "$desktop_tmp" "$application_desktop"
    fi

    environment_line="FCITX_ADDON_DIRS=\"$prefix_addon_dir:$system_addon_dir\""
    {
        printf '%s\n' "$environment_line"
        printf 'XDG_DATA_DIRS="%s:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"\n' "$prefix/share"
        printf 'GTK_IM_MODULE=fcitx\n'
        printf 'QT_IM_MODULE=fcitx\n'
        printf 'XMODIFIERS=@im=fcitx\n'
        printf 'SDL_IM_MODULE=fcitx\n'
    } > "$desktop_tmp"
    modernime_write_managed_file "$desktop_tmp" "$environment_file" environment

    autostart_exec="env $(modernime_desktop_argument "FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir") $(modernime_desktop_argument "XDG_DATA_DIRS=$data_directories") $(modernime_desktop_argument "XDG_CONFIG_HOME=$config_home") fcitx5 -d -u modernime-ui"
    {
        printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=ModernIME Fcitx5'
        printf '%s\n' 'Comment=Start Fcitx5 with the ModernIME candidate bar'
        printf 'Exec=%s\n' "$autostart_exec"
        printf '%s\n' 'Terminal=false' 'X-GNOME-Autostart-enabled=true' 'NoDisplay=true'
    } > "$desktop_tmp"
    modernime_write_managed_file "$desktop_tmp" "$autostart_file" autostart
fi

skip_xprofile=false
case "${MODERNIME_SKIP_XPROFILE:-}" in
    1|true|TRUE|yes|YES)
        skip_xprofile=true
        ;;
esac
xprofile_file=${MODERNIME_XPROFILE_FILE:-"$HOME/.xprofile"}
if [[ -z "${MODERNIME_XPROFILE_FILE:-}" && "$config_home" != "$HOME/.config" ]]; then
    skip_xprofile=true
fi
if [[ "$skip_xprofile" != true && "$is_system_prefix" != true ]]; then
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
    modernime_write_profile_block "$desktop_tmp" "$xprofile_file"
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -d "$prefix/share/icons/hicolor" ]]; then
    gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" 2>/dev/null || true
fi

manifest_dir="$prefix/share/modernime"
manifest="$manifest_dir/install-manifest.txt"
channel_file="$manifest_dir/install-channel.txt"
mkdir -p "$manifest_dir"
installed_version=${binary_version:-}
if [[ -z "$installed_version" ]]; then
    installed_version=$(grep -m1 '^PROJECT_VERSION:STATIC=' "$build_dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
fi
if [[ -z "$installed_version" ]]; then
    installed_version=$(grep -m1 'project(ModernIME VERSION' "$project_root/CMakeLists.txt" 2>/dev/null | grep -oP '\d+\.\d+\.\d+' || echo "unknown")
fi
{
    printf 'channel=script\n'
    printf 'version=%s\n' "$installed_version"
    printf 'prefix=%s\n' "$prefix"
} > "$channel_file"
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
    if [[ "$is_system_prefix" != true ]]; then
        printf '%s\n' "$desktop_shortcut"
        printf '%s\n' "$environment_file"
        printf '%s\n' "$autostart_file"
        if [[ -f "$managed_state_dir/xprofile.path" ]]; then
            cat "$managed_state_dir/xprofile.path"
        fi
    fi
} > "$manifest"

if [[ "$is_system_prefix" != true && -x "$prefix/bin/modernime-session-init" ]]; then
    MODERNIME_PREFIX="$prefix" MODERNIME_CONFIG_HOME="$config_home" \
        MODERNIME_SKIP_FCITX_RESTART=1 "$prefix/bin/modernime-session-init"
fi

run_fcitx5_remote() {
    "${fcitx_environment[@]}" bash -c '
        if command -v timeout >/dev/null 2>&1; then
            timeout --kill-after=0.5s 1s fcitx5-remote "$@" || exit 1
        else
            fcitx5-remote "$@" || exit 1
        fi
    ' modernime-fcitx5-remote "$@" 2>/dev/null
}

start_fcitx5_safely() {
    "${fcitx_environment[@]}" bash -c '
        if command -v timeout >/dev/null 2>&1; then
            timeout --kill-after=1s 3s fcitx5 -d -r -u modernime-ui || true
        else
            fcitx5 -d -r -u modernime-ui || true
        fi
    ' modernime-fcitx5-start >/dev/null 2>&1
}

skip_fcitx_restart=false
case "${MODERNIME_SKIP_FCITX_RESTART:-}" in
    1|true|TRUE|yes|YES)
        skip_fcitx_restart=true
        ;;
esac

if [[ "$skip_fcitx_restart" == true ]]; then
    printf 'Fcitx5 restart skipped by MODERNIME_SKIP_FCITX_RESTART=1\n'
elif ! command -v fcitx5 >/dev/null 2>&1; then
    printf 'Fcitx5 executable not found; file deployment completed (runtime activation skipped due to missing fcitx5)\n' >&2
elif ! command -v fcitx5-remote >/dev/null 2>&1; then
    printf 'fcitx5-remote executable not found; file deployment completed (runtime activation skipped due to missing fcitx5-remote)\n' >&2
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'No graphical session detected; Fcitx5 auto-start skipped; start it from a graphical session with: fcitx5 -u modernime-ui\n' >&2
elif [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    printf 'DBus session not detected; Fcitx5 auto-start skipped; start it from a graphical session with: fcitx5 -u modernime-ui\n' >&2
else
    start_fcitx5_safely &
    printf 'Fcitx5 restart requested with the ModernIME UI addon\n'
    activated=false
    for attempt in {1..20}; do
        if run_fcitx5_remote -s modernime >/dev/null &&
           run_fcitx5_remote -o >/dev/null; then
            current_input_method=$(run_fcitx5_remote -n) || current_input_method=""
            current_input_status=$(run_fcitx5_remote) || current_input_status=""
            if [[ "$current_input_method" == "modernime" &&
                  "$current_input_status" == "2" ]]; then
                activated=true
                break
            fi
        fi
        sleep 0.2
    done
    if [[ "$activated" == true ]]; then
        printf 'ModernIME input method activated\n'
    else
        printf 'ModernIME input method could not be activated automatically; use: fcitx5-remote -s modernime\n' >&2
    fi
fi

printf 'ModernIME installed to %s\n' "$prefix"
printf 'Install manifest: %s\n' "$manifest"
printf 'Offline pinyin knowledge dictionary: %s/share/modernime/pinyin/modernime-knowledge.dict\n' "$prefix"
printf 'Launch settings client: %s\n' "$prefix/bin/modernime-settings"
if [[ "$is_system_prefix" == true ]]; then
    printf 'Desktop user session configuration: Run "%s/bin/modernime-session-init" as regular user to initialize session.\n' "$prefix"
else
    printf 'Desktop shortcut: %s\n' "$desktop_shortcut"
fi
printf 'Select the UI addon with: fcitx5 -u modernime-ui\n'
printf '\n'
printf '【会话环境变量配置提示】\n'
printf ' 若您在 LightDM/UKUI/X11 等桌面会话中无法调出输入法，请确保以下变量已生效：\n'
printf '   export GTK_IM_MODULE=fcitx\n'
printf '   export QT_IM_MODULE=fcitx\n'
printf '   export XMODIFIERS=@im=fcitx\n'
printf '   export SDL_IM_MODULE=fcitx\n'
printf '   export FCITX_ADDON_DIRS="%s:%s"\n' "$prefix_addon_dir" "$system_addon_dir"
if [[ "$skip_xprofile" != true && "$is_system_prefix" != true && -f "$managed_state_dir/xprofile.path" ]]; then
    printf ' (脚本已自动同步写入至 %s 与 %s)\n' "$environment_file" "$xprofile_file"
fi
