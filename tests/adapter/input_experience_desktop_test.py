#!/usr/bin/env python3
"""隔离 X11/DBus 会话中的 GTK、Qt 和终端真实输入回放。

手动专项入口（依赖 Xvfb、xdotool、xterm、GTK3 GI、PyQt5）：
  xvfb-run -a -s '-screen 0 1440x900x24' dbus-run-session -- \
    /usr/bin/python3 tests/adapter/input_experience_desktop_test.py REPORT_DIR
默认加载当前 build 的引擎；MODERNIME_TEST_ADDON_DIR 可指定已安装引擎。
"""
import json
import argparse
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import time


def save(path, value):
    temporary = path.with_suffix('.writing')
    temporary.write_text(json.dumps(value, ensure_ascii=False))
    temporary.replace(path)


def host(kind, state):
    if kind == 'terminal':
        import termios
        attributes = termios.tcgetattr(0)
        attributes[3] &= ~termios.ECHO
        termios.tcsetattr(0, termios.TCSANOW, attributes)
        save(state, {'text': '', 'preedit': '', 'lines': []})
        lines = []
        for line in sys.stdin:
            lines.append(line.rstrip('\n'))
            save(state, {'text': lines[-1], 'preedit': '', 'lines': lines})
        return
    if kind == 'gtk':
        import gi
        gi.require_version('Gtk', '3.0')
        from gi.repository import Gtk
        window = Gtk.Window(title='ModernIME desktop gtk')
        entry = Gtk.Entry()
        preedit = ['']
        def changed(*_):
            save(state, {'text': entry.get_text(), 'preedit': preedit[0]})
        def composition(_, value):
            preedit[0] = value
            changed()
        entry.connect('changed', changed)
        entry.connect('preedit-changed', composition)
        window.add(entry)
        window.set_default_size(1000, 120)
        window.connect('destroy', Gtk.main_quit)
        window.show_all()
        entry.grab_focus()
        changed()
        Gtk.main()
        return
    from PyQt5.QtWidgets import QApplication, QLineEdit
    app = QApplication([])
    class Entry(QLineEdit):
        def inputMethodEvent(self, event):
            self.composition = event.preeditString()
            super().inputMethodEvent(event)
            self.changed()
        def changed(self, *_):
            save(state, {'text': self.text(), 'preedit': getattr(self, 'composition', '')})
    entry = Entry()
    entry.setWindowTitle('ModernIME desktop qt')
    entry.resize(1000, 120)
    entry.textChanged.connect(entry.changed)
    entry.show()
    entry.setFocus()
    entry.changed()
    app.exec_()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report_dir')
    parser.add_argument('--application', choices=['gtk', 'qt', 'terminal'])
    parser.add_argument('--case', action='append', default=[])
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    output = Path(args.report_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / ('run-' + str(os.getpid()) + '-' + str(time.monotonic_ns()))
    fixture.mkdir()
    for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                      ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
        path = fixture / name
        path.mkdir(mode=0o700)
        os.environ[key] = str(path)
    os.environ.update(HOME=str(fixture), GTK_IM_MODULE='fcitx', QT_IM_MODULE='fcitx',
                      XMODIFIERS='@im=fcitx', MODERNIME_SKIP_DESKTOP_SHORTCUT='1',
                      LANG='C.UTF-8')
    share = fixture / 'share'
    installed = Path('/home/wsl/.local')
    for category in ['addon', 'inputmethod']:
        dest = share / 'fcitx5' / category
        dest.mkdir(parents=True)
        for source in (installed / 'share/fcitx5' / category).glob('*.conf'):
            shutil.copy2(source, dest / source.name)
    os.environ['XDG_DATA_DIRS'] = f'{share}:{installed}/share:/usr/local/share:/usr/share'
    addon = Path(os.environ.get('MODERNIME_TEST_ADDON_DIR', str(repo / 'build/adapter/fcitx5')))
    os.environ['FCITX_ADDON_DIRS'] = f'{addon}:{installed}/lib/fcitx5:/usr/lib/x86_64-linux-gnu/fcitx5'
    config = fixture / 'config/fcitx5'
    config.mkdir()
    (config / 'profile').write_text('[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=modernime\n\n[Groups/0/Items/0]\nName=keyboard-us\nLayout=\n\n[Groups/0/Items/1]\nName=modernime\nLayout=\n\n[GroupOrder]\n0=Default\n')
    learning = fixture / 'data/modernime/learning.sqlite3'
    learning.parent.mkdir()
    with sqlite3.connect(learning) as db:
        db.execute("CREATE TABLE learning_entries(pinyin TEXT NOT NULL,phrase TEXT NOT NULL,context_before TEXT NOT NULL DEFAULT '',context_after TEXT NOT NULL DEFAULT '',frequency INTEGER NOT NULL DEFAULT 0,last_selected_ms INTEGER NOT NULL DEFAULT 0,negative_feedback INTEGER NOT NULL DEFAULT 0,suppressed INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(pinyin,phrase,context_before,context_after))")
        db.execute('CREATE TABLE learning_state(id INTEGER PRIMARY KEY,reset_revision INTEGER NOT NULL DEFAULT 0)')
        db.execute('INSERT INTO learning_state VALUES(1,0)')
        db.execute('PRAGMA user_version=1')
        now = int(time.time() * 1000)
        for word, frequency in [('sudo', 19), ('sudoi', 1)]:
            db.execute('INSERT INTO learning_entries VALUES(?,?,?,?,?,?,?,?)', (word, word, '', '', frequency, now, 0, 0))
    def run(*args):
        return subprocess.run(args, check=True, text=True, capture_output=True).stdout.strip()
    def wait(predicate, label, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.025)
        raise AssertionError('等待超时：' + label)
    def read(path):
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return {}
    def keys(*values):
        run('xdotool', 'key', '--clearmodifiers', *values)
    def capture(name):
        import gi
        gi.require_version('Gdk', '3.0')
        from gi.repository import Gdk
        previous, stable = None, 0
        def frame():
            nonlocal previous, stable
            root = Gdk.get_default_root_window()
            pixels = Gdk.pixbuf_get_from_window(root, 0, 0, root.get_width(), root.get_height())
            current = pixels.get_pixels()
            stable = stable + 1 if current == previous else 0
            previous = current
            return pixels if stable >= 4 else None
        wait(frame, '候选画面稳定').savev(str(fixture / name), 'png', [], [])
    cases = [
        ('old-single-observation', [('type', 'sudo'), ('key', 'space')], 'sudo'),
        ('vowel-initial-abbreviation', [('type', 'aly'), ('key', 'space')], '阿里云'),
        ('digit-selection', [('type', 'nihao'), ('key', '1')], '你好'),
        ('full-pinyin', [('type', 'xiexie'), ('key', 'space')], '谢谢'),
        ('apostrophe', [('type', "zhong'guo"), ('key', 'space')], '中国'),
        ('raw-enter', [('type', 'python'), ('key', 'Return')], 'python'),
        ('english-completion', [('type', 'doc'), ('key', 'space')], 'document'),
        ('backspace-correction', [('type', 'shurufx'), ('key', 'BackSpace'), ('type', 'a'), ('key', '1')], '输入法'),
        ('cancel-restart', [('type', 'nihao'), ('key', 'Escape'), ('type', 'zhongwen'), ('key', '1')], '中文'),
        ('initials', [('type', 'nh'), ('key', 'space')], '你好'),
        ('technical-prefix-boundary', [('type', 'syst'), ('key', 'space')], 'system'),
        ('builtin-http-first-use', [('type', 'http'), ('key', 'Return'), ('type', 'ht'), ('key', '3')], 'httphttp'),
        ('builtin-html-first-use', [('type', 'html'), ('key', 'Return'), ('type', 'htm'), ('key', '3')], 'htmlhtml'),
    ]
    if args.case:
        assert set(args.case) <= {case[0] for case in cases}, '未知场景名'
        cases = [case for case in cases if case[0] in args.case]
    applications = [args.application] if args.application else ['gtk', 'qt', 'terminal']
    results = []
    with (fixture / 'engine.log').open('w') as log:
        engine = subprocess.Popen(['fcitx5', '-D', '--disable=classicui,kimpanel,notificationitem,ibusfrontend,wayland,waylandim'], stdout=log, stderr=log)
        try:
            wait(lambda: subprocess.run(['fcitx5-remote', '--check'], capture_output=True).returncode == 0, 'Fcitx DBus')
            for kind in applications:
                state = fixture / (kind + '.json')
                command = ['/usr/bin/python3', str(Path(__file__).resolve()), '--host', kind, str(state)]
                if kind == 'terminal':
                    command = ['xterm', '-u8', '-title', 'ModernIME desktop terminal', '-xrm', '*inputMethod:fcitx', '-xrm', '*preeditType:Root', '-e'] + command
                with (fixture / (kind + '.log')).open('w') as hostlog:
                    process = subprocess.Popen(command, stdout=hostlog, stderr=hostlog)
                    try:
                        wait(lambda: state.exists(), kind + ' 就绪')
                        xid = wait(lambda: subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', 'ModernIME desktop ' + kind], capture_output=True, text=True).stdout.strip(), kind + ' 窗口').splitlines()[-1]
                        run('xdotool', 'windowfocus', xid)
                        def activate():
                            run('fcitx5-remote', '-s', 'modernime')
                            run('fcitx5-remote', '-o')
                            return run('fcitx5-remote', '-n') == 'modernime'
                        wait(activate, 'ModernIME激活')
                        for index, (name, sequence, expected) in enumerate(cases):
                            before = len(read(state).get('lines', []))
                            try:
                                for action, value in sequence:
                                    if action == 'type':
                                        run('xdotool', 'type', '--clearmodifiers', '--delay', '25', value)
                                        if kind != 'terminal':
                                            wait(lambda: read(state).get('preedit'), kind + ' 组合文本')
                                        if index < 2 or name.startswith('builtin-'):
                                            capture(kind + '-' + name + '.png')
                                    else:
                                        keys(value)
                                if kind == 'terminal':
                                    keys('Return')
                                    wait(lambda: len(read(state).get('lines', [])) == before + 1, kind + ' 上屏行')
                                wait(lambda: read(state).get('text') == expected, kind + '/' + name + ' 预期上屏 ' + expected)
                                result = {'application': kind, 'case': name, 'sequence': sequence, 'expected': expected, 'actual': read(state), 'pass': True}
                                print(json.dumps(result, ensure_ascii=False), flush=True)
                                results.append(result)
                                if kind != 'terminal':
                                    keys('ctrl+a', 'BackSpace')
                                    wait(lambda: read(state).get('text') == '' and read(state).get('preedit', '') == '', '清空宿主输入框')
                            except Exception as error:
                                capture(kind + '-FAIL-' + name + '.png')
                                results.append({'application': kind, 'case': name, 'sequence': sequence, 'expected': expected, 'actual': read(state), 'pass': False, 'error': str(error)})
                                raise
                    finally:
                        process.terminate()
                        process.wait(timeout=10)
            maps = Path(f'/proc/{engine.pid}/maps').read_text()
            (fixture / 'loaded-maps.txt').write_text('\n'.join(line for line in maps.splitlines() if 'modernime' in line))
            assert str(addon / 'modernime_fcitx5.so') in maps and not any('(deleted)' in line for line in maps.splitlines() if 'modernime' in line)
        finally:
            engine.terminate()
            engine.wait(timeout=10)
            save(fixture / 'results.json', {'cases': results, 'passed': sum(x['pass'] for x in results), 'failed': sum(not x['pass'] for x in results)})
            print('REPORT=' + str(fixture / 'results.json'), flush=True)
    assert len(results) == len(cases) * len(applications)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--host':
        host(sys.argv[2], Path(sys.argv[3]))
    else:
        main()
