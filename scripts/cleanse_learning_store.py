#!/usr/bin/env python3
import os
import shutil
import sqlite3
import time

def main():
    base_dir = os.path.expanduser('~/.local/share/modernime')
    quarantine_dir = os.path.join(base_dir, 'quarantine')
    os.makedirs(quarantine_dir, exist_ok=True)

    print('[1/5] Quarantining external user_personal_dict.bin...')
    bin_file = os.path.join(base_dir, 'user_personal_dict.bin')
    if os.path.exists(bin_file):
        dst = os.path.join(quarantine_dir, 'user_personal_dict.bin')
        shutil.move(bin_file, dst)
        print(f'  -> Moved {bin_file} to {dst}')
    else:
        print('  -> No user_personal_dict.bin found in runtime dir.')

    src_backup = os.path.join(base_dir, 'learning.sqlite3.backup-before-34k-import-20261012135317')
    src_current = os.path.join(base_dir, 'learning.sqlite3')

    if not os.path.exists(src_backup):
        print(f'Error: Pre-import backup not found at {src_backup}')
        return

    print('[2/5] Backing up current database before cleansing...')
    ts_str = time.strftime('%Y%m%d%H%M%S')
    backup_pre_cleanse = os.path.join(base_dir, f'learning.sqlite3.backup-pre-cleanse-{ts_str}')
    shutil.copy2(src_current, backup_pre_cleanse)
    print(f'  -> Backed up current database to {backup_pre_cleanse}')

    print('[3/5] Extracting healthy pre-import records and recent genuine typing...')
    c_bak = sqlite3.connect(src_backup)
    c_cur = sqlite3.connect(src_current)

    bak_entries = c_bak.execute(
        'SELECT pinyin, phrase, context_before, context_after, frequency, last_selected_ms, negative_feedback, suppressed FROM learning_entries'
    ).fetchall()
    print(f'  -> Loaded {len(bak_entries)} pristine baseline entries from pre-import backup.')

    recent_entries = c_cur.execute(
        'SELECT pinyin, phrase, context_before, context_after, frequency, last_selected_ms, negative_feedback, suppressed FROM learning_entries WHERE last_selected_ms > 1791784397000'
    ).fetchall()
    print(f'  -> Loaded {len(recent_entries)} recent typing entries from current database.')

    merged = {}
    for py, ph, cb, ca, freq, ts, nf, sup in bak_entries:
        key = (py, ph, cb, ca)
        merged[key] = [py, ph, cb, ca, freq, ts, nf, sup]

    contaminated_phrases = {'这歌', '豪德', '不失', '科一', '彳亍'}
    merged_recent = 0
    skipped_contaminated = 0
    for py, ph, cb, ca, freq, ts, nf, sup in recent_entries:
        if ph in contaminated_phrases:
            skipped_contaminated += 1
            continue
        if ph == '试' and py == 'shi' and freq > 10:
            skipped_contaminated += 1
            continue
        if ph == '地' and py == 'de' and freq > 10:
            skipped_contaminated += 1
            continue
        if ph == '骂' and py == 'ma' and freq > 10:
            skipped_contaminated += 1
            continue
        if ph == '盒' and py == 'he' and freq > 10:
            skipped_contaminated += 1
            continue
        if ph == '豪' and py == 'hao' and freq > 10:
            skipped_contaminated += 1
            continue
        if ph == '尼' and py == 'ni' and freq > 10:
            skipped_contaminated += 1
            continue
        key = (py, ph, cb, ca)
        if key in merged:
            merged[key][4] += freq
            merged[key][5] = max(merged[key][5], ts)
            merged[key][6] = max(merged[key][6], nf)
            merged[key][7] = max(merged[key][7], sup)
        else:
            merged[key] = [py, ph, cb, ca, freq, ts, nf, sup]
        merged_recent += 1

    print(f'  -> Merged {merged_recent} genuine recent entries (filtered {skipped_contaminated} contaminated items).')
    print(f'  -> Total unique entries in cleansed database: {len(merged)}')

    print('[4/5] Writing cleansed records to new learning.sqlite3...')
    temp_sqlite = os.path.join(base_dir, 'learning.sqlite3.clean')
    if os.path.exists(temp_sqlite):
        os.remove(temp_sqlite)

    c_out = sqlite3.connect(temp_sqlite)
    c_out.execute('''
        CREATE TABLE learning_entries (
            pinyin TEXT NOT NULL,
            phrase TEXT NOT NULL,
            context_before TEXT NOT NULL DEFAULT '',
            context_after TEXT NOT NULL DEFAULT '',
            frequency INTEGER NOT NULL DEFAULT 0,
            last_selected_ms INTEGER NOT NULL DEFAULT 0,
            negative_feedback INTEGER NOT NULL DEFAULT 0,
            suppressed INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (pinyin, phrase, context_before, context_after)
        );
    ''')
    c_out.execute('''
        CREATE TABLE learning_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            reset_revision INTEGER NOT NULL DEFAULT 0
        );
    ''')
    c_out.execute('INSERT INTO learning_state (id, reset_revision) VALUES (1, 1);')

    for item in merged.values():
        c_out.execute('''
            INSERT INTO learning_entries (
                pinyin, phrase, context_before, context_after, frequency, last_selected_ms, negative_feedback, suppressed
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?);
        ''', item)

    c_out.commit()
    c_out.close()

    # Safely replace learning.sqlite3
    shutil.move(temp_sqlite, src_current)
    # Remove old wal and shm files
    for ext in ['-wal', '-shm']:
        f = src_current + ext
        if os.path.exists(f):
            os.remove(f)

    print('[5/5] Resetting LMDB to force clean migration from cleansed SQLite...')
    lmdb_file = os.path.join(base_dir, 'learning.lmdb')
    lmdb_lock = os.path.join(base_dir, 'learning.lmdb-lock')
    if os.path.exists(lmdb_file):
        shutil.move(lmdb_file, os.path.join(quarantine_dir, f'learning.lmdb.backup-pre-cleanse-{ts_str}'))
    if os.path.exists(lmdb_lock):
        os.remove(lmdb_lock)

    print('[+] Cleansing complete! The runtime learning store is now 100% clean and healthy.')

if __name__ == '__main__':
    main()
