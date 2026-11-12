#!/usr/bin/env python3
"""Exercise the session launcher with owned foreground child processes."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

runner = Path(__file__).resolve().parents[1] / 'scripts/modernime-session-runner'


def wait_for(predicate, seconds=4):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError('session launcher did not reach the expected state')


with tempfile.TemporaryDirectory(prefix='modernime-session-runner-') as tmp:
    root = Path(tmp)
    (root / 'bin').mkdir()
    fake = root / 'bin/fcitx5'
    fake.write_text(f'''#!{sys.executable}
import os, signal, time
from pathlib import Path
p = Path(os.environ['SESSION_TEST_ROOT'])
(p / 'mode').write_text('1\\nkeyboard-us')
with (p / 'starts').open('a') as out:
    out.write(str(os.getpid()) + '\\n')
signal.signal(signal.SIGTERM, lambda *_: exit(0))
while True: time.sleep(.05)
''')
    fake.chmod(0o755)
    control = root / 'bin/fcitx5-remote'
    control.write_text(f'''#!{sys.executable}
import os, sys
from pathlib import Path
if sys.argv[1:] == ['--help']:
    print('--check'); sys.exit(0)
assert sys.argv[1] == '--check'
p = Path(os.environ['SESSION_TEST_ROOT']) / 'mode'
state, method = p.read_text().splitlines()
args = sys.argv[2:]
if not args: print(state)
elif args == ['-n']: print(method)
elif args[0] == '-s': method = args[1]
elif args == ['-o']: state = '2'
elif args == ['-c']: state = '1'
p.write_text(state + '\\n' + method)
''')
    control.chmod(0o755)
    bus = socket.socket(socket.AF_UNIX)
    bus.bind(str(root / 'bus'))
    env = dict(os.environ, PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'],
               XDG_RUNTIME_DIR=str(root), DISPLAY=':modernime-test',
               DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(root / 'bus'),
               SESSION_TEST_ROOT=str(root))
    env.pop('XDG_SESSION_ID', None)
    env.pop('XAUTHORITY', None)
    starts = lambda: (root / 'starts').read_text().splitlines() if (root / 'starts').exists() else []
    proc = subprocess.Popen([sys.executable, str(runner)], env=env)
    try:
        wait_for(lambda: len(starts()) == 1)
        first = int(starts()[0])
        # A Chinese mode selected before a crash must survive the default-English restart.
        (root / 'mode').write_text('2\nmodernime')
        time.sleep(.8)
        os.kill(first, signal.SIGKILL)
        wait_for(lambda: len(starts()) == 2)
        wait_for(lambda: (root / 'mode').read_text() == '2\nmodernime')
        second = int(starts()[1])
        assert second != first and proc.poll() is None
        # A second launcher must not create a competing daemon.
        duplicate = subprocess.run([sys.executable, str(runner)], env=env, timeout=2)
        assert duplicate.returncode == 0 and len(starts()) == 2
        # A graceful Fcitx5 exit is final, rather than an abnormal-exit retry.
        os.kill(second, signal.SIGTERM)
        assert proc.wait(timeout=2) == 0 and len(starts()) == 2

        proc = subprocess.Popen([sys.executable, str(runner)], env=env)
        wait_for(lambda: len(starts()) == 3)
        # Once the desktop bus is gone, even an abnormal exit must not respawn.
        (root / 'bus').unlink()
        os.kill(int(starts()[2]), signal.SIGKILL)
        assert proc.wait(timeout=2) == 0 and len(starts()) == 3
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=3)
        bus.close()
print('异常恢复、中文激活状态、正常退出、重复启动与会话结束检查通过')
