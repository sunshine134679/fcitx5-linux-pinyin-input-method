#!/bin/sh
set -e

# $1 == 1 for initial install, $1 >= 2 for upgrade
if [ -f /usr/share/modernime/install-channel.txt ]; then
    existing_channel=$(grep -E '^channel=' /usr/share/modernime/install-channel.txt 2>/dev/null | head -n 1 | cut -d= -f2 || true)
    if [ "$existing_channel" = "script" ] && [ "${MODERNIME_MIGRATE_FROM_SCRIPT:-0}" != "1" ]; then
        echo "Error: Detected an existing script-based ModernIME installation in /usr (channel=script)." >&2
        echo "Please run 'sudo modernime-migrate-channel --prepare-system-migration' or uninstall the script installation first." >&2
        exit 1
    fi
elif [ -f /usr/share/modernime/install-manifest.txt ] && [ "${MODERNIME_MIGRATE_FROM_SCRIPT:-0}" != "1" ]; then
    echo "Error: Detected an existing script-managed install-manifest.txt in /usr." >&2
    exit 1
fi

if [ "${1:-1}" -ge 2 ] && [ -x /usr/bin/modernime-data-guard ]; then
    /usr/bin/modernime-data-guard --pre-upgrade
fi

exit 0
