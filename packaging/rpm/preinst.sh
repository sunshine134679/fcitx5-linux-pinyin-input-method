#!/bin/sh
set -e

pkg_root="${MODERNIME_PKG_ROOT:-}"

# $1 == 1 for initial install, $1 >= 2 for upgrade
for prefix_dir in "${pkg_root}/usr" "${pkg_root}/usr/local"; do
    if [ -f "${prefix_dir}/share/modernime/install-channel.txt" ]; then
        existing_channel=$(grep -E '^channel=' "${prefix_dir}/share/modernime/install-channel.txt" 2>/dev/null | head -n 1 | cut -d= -f2 || true)
        if [ "$existing_channel" = "script" ] && [ "${MODERNIME_MIGRATE_FROM_SCRIPT:-0}" != "1" ]; then
            echo "Error: Detected an existing script-based ModernIME installation in ${prefix_dir} (channel=script)." >&2
            echo "Please run 'sudo modernime-migrate-channel --to-channel rpm --package-cmd \"sudo dnf install -y modernime\"' or uninstall the script installation first." >&2
            exit 1
        fi
    elif [ -f "${prefix_dir}/share/modernime/install-manifest.txt" ] && [ "${MODERNIME_MIGRATE_FROM_SCRIPT:-0}" != "1" ]; then
        echo "Error: Detected an existing script-managed install-manifest.txt in ${prefix_dir}." >&2
        exit 1
    fi
done

if [ "${1:-1}" -ge 2 ] && [ -x "${pkg_root}/usr/bin/modernime-data-guard" ]; then
    "${pkg_root}/usr/bin/modernime-data-guard" --pre-upgrade
fi

exit 0
