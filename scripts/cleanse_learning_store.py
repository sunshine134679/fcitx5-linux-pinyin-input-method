#!/usr/bin/env python3
"""
DEPRECATED AND PERMANENTLY DISABLED.

Historical Context:
This script was an ad-hoc emergency workaround created during the 2026-10-12
incident to quarantine an external dictionary file (user_personal_dict.bin)
and rewrite learning.sqlite3/LMDB in-place.

Architectural Incident Risk (P0-04):
Directly altering runtime databases without transactional coordination,
safeguards, or synchronization causes data loss, WAL corruption, and race
conditions with active input method instances.

As mandated by the ModernIME Architecture Remediation Plan (P0-04 / A08),
this script is permanently neutralized and refuses execution unconditionally
under all invocation patterns, flags, and environments.

Supported safe alternatives:
- Backup & restore: scripts/modernime-data-guard
- In-memory clear / safe reset: modernime::core::LearningStore::clear() via
  the ModernIME Settings application or official runtime APIs.
"""

import sys

def main():
    sys.stderr.write(
        "ERROR: cleanse_learning_store.py has been permanently disabled (P0-04).\n"
        "Directly mutating or quarantining runtime learning stores in-place is unsafe\n"
        "and violates ModernIME data protection invariants.\n"
        "\n"
        "For safe backup and restoration, use scripts/modernime-data-guard.\n"
        "For resetting learning state, use the ModernIME Settings application.\n"
    )
    sys.exit(1)

if __name__ == '__main__':
    main()
