#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source "$project_root/scripts/install_state.sh"

prefix=${MODERNIME_PREFIX:-"$HOME/.local"}
had_explicit_args=$#
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
            shift
            ;;
        --prefix|-p)
            prefix="$2"
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
            shift
            ;;
    esac
done

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

if [[ "$project_root" =~ ^/(tmp|var/tmp)/ && -z "${MODERNIME_PREFIX:-}" && "$had_explicit_args" -eq 0 ]]; then
    printf 'Error: Running install.sh from temporary directory %s without MODERNIME_PREFIX is not allowed.\n' "$project_root" >&2
    printf 'This prevents accidental installation or modification of your real user environment (%s).\n' "$HOME" >&2
    printf 'Specify MODERNIME_PREFIX explicitly (e.g. MODERNIME_PREFIX=/tmp/test-prefix) if intended.\n' >&2
    exit 1
fi
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
)
for environment_name in DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR; do
    if [[ -n "${!environment_name:-}" ]]; then
        fcitx_environment+=("$environment_name=${!environment_name}")
    fi
done

cmake_args=(
    -DCMAKE_BUILD_TYPE=Debug
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

src_rollback_dir=""
if [[ -d "$prefix" ]]; then
    src_rollback_dir=$(mktemp -d "${TMPDIR:-/tmp}/modernime-src-rollback.XXXXXX")
    cp -a -- "$prefix/." "$src_rollback_dir/" 2>/dev/null || true
fi
restore_src_on_failure() {
    printf 'Error encountered during source installation; restoring previous version...\n' >&2
    if [[ -n "$src_rollback_dir" && -d "$src_rollback_dir" ]]; then
        rm -rf -- "$prefix/bin/modernime-settings" \
                  "$prefix/bin/modernime-session-init" \
                  "$prefix/bin/modernime-data-guard" \
                  "$prefix/lib/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib/fcitx5/modernime_ui.so" \
                  "$prefix/lib64/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib64/fcitx5/modernime_ui.so" \
                  "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so" \
                  "$prefix/lib/aarch64-linux-gnu/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib/aarch64-linux-gnu/fcitx5/modernime_ui.so" 2>/dev/null || true
        cp -a --remove-destination -- "$src_rollback_dir/." "$prefix/" 2>/dev/null || true
        rm -rf -- "$src_rollback_dir"
    fi
    if [[ "$is_system_prefix" != true && -x "$project_root/scripts/modernime-data-guard" ]]; then
        "$project_root/scripts/modernime-data-guard" --restore \
            --data-dir "${MODERNIME_DATA_HOME:-"${XDG_DATA_HOME:-"$HOME/.local/share"}"}/modernime" \
            --config-dir "${MODERNIME_CONFIG_HOME:-"${XDG_CONFIG_HOME:-"$HOME/.config"}"}/modernime" 2>/dev/null || true
    fi
    printf 'Rollback to previous version completed.\n' >&2
    exit 1
}

# Always unlink existing plugin .so files before installing so running Fcitx5 processes never suffer in-place mmap overwrites.
for check_file in "$prefix/bin/modernime-settings" \
                  "$prefix/lib/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib/fcitx5/modernime_ui.so" \
                  "$prefix/lib64/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib64/fcitx5/modernime_ui.so" \
                  "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so" \
                  "$prefix/lib/aarch64-linux-gnu/fcitx5/modernime_fcitx5.so" \
                  "$prefix/lib/aarch64-linux-gnu/fcitx5/modernime_ui.so"; do
    if [[ -e "$check_file" || -L "$check_file" ]]; then
        rm -f -- "$check_file" 2>/dev/null || true
    fi
done
if ! cmake --install "$build_dir" --prefix "$prefix"; then
    restore_src_on_failure
fi
if [[ "${MODERNIME_SIMULATE_INSTALL_FAILURE:-0}" == "1" ]]; then
    printf 'Error: Simulated mid-upgrade installation failure via MODERNIME_SIMULATE_INSTALL_FAILURE=1\n' >&2
    restore_src_on_failure
fi
if [[ -n "$src_rollback_dir" && -d "$src_rollback_dir" ]]; then
    rm -rf -- "$src_rollback_dir"
fi

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
    local candidate_dirs=(
        "$target_prefix/lib/fcitx5"
        "$target_prefix/lib64/fcitx5"
        "$target_prefix/lib/x86_64-linux-gnu/fcitx5"
        "$target_prefix/lib/aarch64-linux-gnu/fcitx5"
    )
    for cdir in "${candidate_dirs[@]}"; do
        if [[ "$cdir" != "$current_addon_dir" && -d "$cdir" ]]; then
            for old_plugin in "$cdir/modernime_fcitx5.so" "$cdir/modernime_ui.so"; do
                if [[ -f "$old_plugin" || -L "$old_plugin" ]]; then
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
                    if [[ "$old_path" != "$current_addon_dir/modernime_fcitx5.so" && \
                          "$old_path" != "$current_addon_dir/modernime_ui.so" && \
                          -f "$old_path" ]]; then
                        printf 'Migrating legacy manifest plugin: removing obsolete %s\n' "$old_path"
                        rm -f -- "$old_path"
                        rmdir --ignore-fail-on-non-empty "$(dirname -- "$old_path")" 2>/dev/null || true
                        rmdir --ignore-fail-on-non-empty "$(dirname -- "$(dirname -- "$old_path")")" 2>/dev/null || true
                    fi
                    ;;
            esac
        done < "$target_prefix/share/modernime/install-manifest.txt"
    fi
}
migrate_legacy_plugins "$prefix" "$prefix_addon_dir"

fcitx_environment=(
    env
    "FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir"
    "XDG_DATA_DIRS=$data_directories"
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
        printf 'Exec=%s\n' "$prefix/bin/modernime-settings"
        printf '%s\n' 'Icon=input-keyboard'
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

    environment_line="FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir"
    {
        printf '%s\n' "$environment_line"
        printf 'XDG_DATA_DIRS="%s:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"\n' "$prefix/share"
        printf 'GTK_IM_MODULE=fcitx\n'
        printf 'QT_IM_MODULE=fcitx\n'
        printf 'XMODIFIERS=@im=fcitx\n'
        printf 'SDL_IM_MODULE=fcitx\n'
    } > "$desktop_tmp"
    modernime_write_managed_file "$desktop_tmp" "$environment_file" environment

    autostart_exec="env FCITX_ADDON_DIRS=$prefix_addon_dir:$system_addon_dir XDG_DATA_DIRS=$data_directories fcitx5 -d -u modernime-ui"
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

manifest_dir="$prefix/share/modernime"
manifest="$manifest_dir/install-manifest.txt"
channel_file="$manifest_dir/install-channel.txt"
mkdir -p "$manifest_dir"
{
    printf 'channel=script\n'
    printf 'version=0.1.9\n'
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
    printf '%s\n' "$prefix/bin/modernime-data-guard"
    printf '%s\n' "$prefix/bin/modernime-migrate-channel"
    printf '%s\n' "$prefix/share/applications/modernime-settings.desktop"
    if [[ "$is_system_prefix" != true ]]; then
        printf '%s\n' "$desktop_shortcut"
        printf '%s\n' "$environment_file"
        printf '%s\n' "$autostart_file"
        if [[ -f "$managed_state_dir/xprofile.path" ]]; then
            cat "$managed_state_dir/xprofile.path"
        fi
    fi
} > "$manifest"

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
