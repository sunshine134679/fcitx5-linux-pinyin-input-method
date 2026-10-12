#!/usr/bin/env bash
set -euo pipefail
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
package_root=${1:?Usage: generate_binary_installer.sh PACKAGE_ROOT}
mkdir -p "$package_root/scripts"
# Keep the source installer's lifecycle as the single authority. These exact,
# checked anchors delimit its build-only requirements and build/install block;
# source layout changes fail packaging rather than silently changing policy.
awk '
    $0 == "required_commands=(cmake ctest pkg-config)" {
        requirements++; skip_requirements = 1; next
    }
    skip_requirements {
        if ($0 == "check_cmake_version") { skip_requirements = 0 }
        next
    }
    $0 == "cmake_args=(" {
        builds++; skip_build = 1; next
    }
    skip_build {
        if ($0 == "cmake_libdir=${cmake_libdir:-lib}") {
            skip_build = 0
            print "source \"$project_root/scripts/install_binary_payload.sh\""
            print "cmake_libdir=\"$binary_libdir\""
        }
        next
    }
    { print }
    END {
        if (requirements != 1 || builds != 1 || skip_requirements || skip_build) {
            print "Source install.sh build anchors changed; cannot generate binary installer" > "/dev/stderr"
            exit 1
        }
    }
' "$project_root/install.sh" > "$package_root/install.sh"
cp "$project_root/uninstall.sh" "$package_root/uninstall.sh"
cp "$project_root/scripts/install_state.sh" "$package_root/scripts/install_state.sh"
cp "$project_root/scripts/install_binary_payload.sh" "$package_root/scripts/install_binary_payload.sh"
cp "$project_root/scripts/modernime-data-guard" "$package_root/scripts/modernime-data-guard"
chmod +x "$package_root/install.sh" "$package_root/uninstall.sh" "$package_root/scripts/modernime-data-guard"
bash -n "$package_root/install.sh" "$package_root/uninstall.sh"
