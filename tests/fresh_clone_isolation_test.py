#!/usr/bin/env python3
"""Exercise the real verifier with disposable Git repositories, never a deployment."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


VERIFIER = Path(__file__).with_name("verify_fresh_clone.sh")
SESSION_VARIABLES = (
    "DISPLAY", "WAYLAND_DISPLAY", "GDK_BACKEND", "BROADWAY_DISPLAY",
    "XAUTHORITY", "SESSION_MANAGER", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE",
    "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
    "DESKTOP_SESSION", "GNOME_DESKTOP_SESSION_ID", "FCITX_ADDON_DIRS", "XDG_DESKTOP_DIR",
)


class FreshCloneIsolationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="modernime-verifier-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source with spaces"
        self.sentinel = self.root / "caller"
        self.tmp = self.root / "temporary files"
        self.tmp.mkdir()
        self.sentinel.mkdir()
        # Only fixture controls enter the subprocess; never print the caller's
        # unrelated environment in an assertion or saved failure report.
        self.env = {"PATH": os.defpath, "LANG": "C.UTF-8", "CMAKE_BUILD_PARALLEL_LEVEL": "2"}
        self.env.update(HOME=str(self.sentinel), TMPDIR=str(self.tmp))
        self.git("init", "--quiet", str(self.source))
        (self.source / "tests").mkdir()
        shutil.copyfile(VERIFIER, self.source / "tests/verify_fresh_clone.sh")
        shutil.copyfile(VERIFIER.with_name("verify_required_tests.py"),
                        self.source / "tests/verify_required_tests.py")
        installer = self.source / "install.sh"
        installer.write_text('''#!/usr/bin/env python3
import json, os
from pathlib import Path
print("FIXTURE_ENV=" + json.dumps({
    "env": dict(os.environ), "cwd": str(Path.cwd()),
    "markers": {p: Path(p).read_text() for p in ("tracked", "staged", "untracked")},
}), flush=True)
raise SystemExit(42)
''', encoding="utf-8")
        installer.chmod(0o755)
        (self.source / "tracked").write_text("committed\n")
        (self.source / "staged").write_text("committed\n")
        self.git("-C", str(self.source), "add", ".")
        self.git("-C", str(self.source), "-c", "user.name=Fixture", "-c",
                 "user.email=fixture@localhost", "-c", "commit.gpgsign=false", "-c",
                 "core.hooksPath=/dev/null", "commit", "--quiet", "-m", "fixture")
        (self.source / "tracked").write_text("unstaged change\n")
        (self.source / "staged").write_text("staged change\n")
        self.git("-C", str(self.source), "add", "staged")
        (self.source / "untracked").write_text("untracked change\n")
        for name in ("data", "build", "prefix", "config", "desktop", "cache", "state", "runtime"):
            directory = self.sentinel / name
            directory.mkdir()
            (directory / "keep").write_text("caller-owned\n")
        (self.sentinel / "xprofile").write_text("caller-owned\n")

    def git(self, *args):
        return subprocess.run(["git", *args], env=self.env, check=True,
                              capture_output=True, text=True, timeout=20)

    @staticmethod
    def contents(root):
        return {str(p.relative_to(root)): p.read_bytes()
                for p in root.rglob("*") if p.is_file()}

    def polluted_environment(self):
        env = self.env.copy()
        for key, name in (
            ("PREFIX", "prefix"), ("BUILD_DIR", "build"), ("CONFIG_HOME", "config"),
            ("DATA_HOME", "data"), ("DESKTOP_DIR", "desktop"), ("XPROFILE_FILE", "xprofile"),
        ):
            env[f"MODERNIME_{key}"] = str(self.sentinel / name)
        env.update(MODERNIME_SKIP_TESTS="1", MODERNIME_SKIP_FCITX_RESTART="0",
                   MODERNIME_FORCE_OVERWRITE="1", MODERNIME_ALLOW_ROOT="1",
                   GTEST_FILTER="NoTests.*", GTEST_OUTPUT=f"xml:{self.sentinel}/gtest.xml",
                   GTEST_TOTAL_SHARDS="2", GTEST_SHARD_INDEX="1", GTEST_REPEAT="0",
                   CTEST_PARALLEL_LEVEL="99", CMAKE_BUILD_PARALLEL_LEVEL="2")
        for key, name in (("CONFIG", "config"), ("DATA", "data"), ("CACHE", "cache"),
                          ("STATE", "state")):
            env[f"XDG_{key}_HOME"] = str(self.sentinel / name)
        env["XDG_RUNTIME_DIR"] = str(self.sentinel / "runtime")
        for key in SESSION_VARIABLES:
            env[key] = "caller-session"
        env["XDG_DESKTOP_DIR"] = str(self.sentinel / "desktop")
        env.update(DBUS_SESSION_BUS_ADDRESS=f"unix:path={self.sentinel}/runtime/bus",
                   XDG_DATA_DIRS=str(self.sentinel / "data"), GSETTINGS_BACKEND="dconf")
        return env

    def run_verifier(self, args=(), *, git_pollution=False, polluted=True):
        env = self.polluted_environment() if polluted else self.env.copy()
        if git_pollution:
            other = self.sentinel / "other-repo"
            self.git("clone", "--quiet", str(self.source), str(other))
            env.update(GIT_DIR=str(other / ".git"), GIT_WORK_TREE=str(other),
                       GIT_INDEX_FILE=str(other / ".git/index"),
                       GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="core.bare", GIT_CONFIG_VALUE_0="true")
        source_before = self.contents(self.source)
        sentinel_before = self.contents(self.sentinel)
        result = subprocess.run(["bash", str(self.source / "tests/verify_fresh_clone.sh"), *args],
                                env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(source_before, self.contents(self.source), "source/index changed")
        self.assertEqual(sentinel_before, self.contents(self.sentinel), "caller data changed")
        return result

    def assert_isolated_failure(self, result, *, keep):
        self.assertEqual(42, result.returncode, result.stdout + result.stderr)
        records = [line.removeprefix("FIXTURE_ENV=") for line in result.stdout.splitlines()
                   if line.startswith("FIXTURE_ENV=")]
        self.assertEqual(1, len(records), result.stdout + result.stderr)
        record = json.loads(records[0])
        env = record["env"]
        owned = Path(record["cwd"]).parent
        self.assertEqual(self.tmp, owned.parent)
        self.assertEqual({"tracked": "unstaged change\n", "staged": "staged change\n",
                          "untracked": "untracked change\n"}, record["markers"])
        for key in ("HOME", "TMPDIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                    "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "MODERNIME_PREFIX", "MODERNIME_BUILD_DIR",
                    "MODERNIME_CONFIG_HOME", "MODERNIME_DATA_HOME", "MODERNIME_DESKTOP_DIR"):
            with self.subTest(variable=key):
                self.assertIn(key, env)
                self.assertTrue(Path(env[key]).is_relative_to(owned), f"{key}={env[key]}")
        for key in (*SESSION_VARIABLES, "MODERNIME_SKIP_TESTS", "MODERNIME_FORCE_OVERWRITE",
                    "MODERNIME_XPROFILE_FILE"):
            with self.subTest(absent=key):
                self.assertNotIn(key, list(env))
        self.assertFalse(any(key.startswith(("GTEST_", "GIT_")) for key in env))
        self.assertEqual("1" if os.geteuid() == 0 else None, env.get("MODERNIME_ALLOW_ROOT"))
        self.assertEqual("1", env["MODERNIME_SKIP_FCITX_RESTART"])
        self.assertEqual("1", env["CTEST_PARALLEL_LEVEL"])
        self.assertEqual("2", env["CMAKE_BUILD_PARALLEL_LEVEL"])
        self.assertEqual("memory", env["GSETTINGS_BACKEND"])
        self.assertEqual("/usr/local/share:/usr/share", env["XDG_DATA_DIRS"])
        self.assertEqual(f"unix:path={env['XDG_RUNTIME_DIR']}/bus", env["DBUS_SESSION_BUS_ADDRESS"])
        self.assertFalse((Path(env["XDG_RUNTIME_DIR"]) / "bus").exists())
        self.assertEqual(keep, owned.exists(), result.stdout + result.stderr)
        if keep:
            self.assertIn(str(owned), result.stdout + result.stderr)
            self.assertEqual(0o700, Path(env["XDG_RUNTIME_DIR"]).stat().st_mode & 0o777)
            self.assertTrue((owned / "repo/untracked").is_file())
        else:
            self.assertEqual([], list(self.tmp.iterdir()))

    def test_caller_paths_sessions_and_test_filters_do_not_reach_install(self):
        self.assert_isolated_failure(self.run_verifier(), keep=False)

    def test_git_environment_cannot_redirect_snapshot_or_modify_caller_index(self):
        self.assert_isolated_failure(self.run_verifier(git_pollution=True), keep=False)

    def test_keep_retains_failed_snapshot_without_masking_exit_status(self):
        self.assert_isolated_failure(self.run_verifier(["--keep"]), keep=True)

    def test_unset_caller_controls_still_produce_isolated_environment(self):
        self.assert_isolated_failure(self.run_verifier(polluted=False), keep=False)

    def test_successful_install_without_each_native_report_is_rejected(self):
        installer = self.source / "install.sh"
        original = installer.read_text()
        reports = ("install-registered.json", "install-junit.xml", "install-gtest.xml")
        for missing in reports:
            with self.subTest(missing=missing):
                emit = (
                    'reports = Path(os.environ["MODERNIME_BUILD_DIR"]) / "Testing"\n'
                    'reports.mkdir(parents=True)\n'
                    f'for name in {tuple(name for name in reports if name != missing)!r}:\n'
                    '    (reports / name).write_text("fixture")\n'
                    'raise SystemExit(0)'
                )
                installer.write_text(original.replace("raise SystemExit(42)", emit))
                result = self.run_verifier()
                self.assertNotEqual(0, result.returncode)
                self.assertNotIn("=== 3.", result.stdout, "artifact checks ran without test evidence")
                self.assertIn(missing, result.stderr)
                self.assertEqual([], list(self.tmp.iterdir()))

    def test_present_but_invalid_reports_are_rejected_by_required_checker(self):
        installer = self.source / "install.sh"
        installer.write_text(installer.read_text().replace("raise SystemExit(42)", (
            'reports = Path(os.environ["MODERNIME_BUILD_DIR"]) / "Testing"\n'
            'reports.mkdir(parents=True)\n'
            'for name in ("install-registered.json", "install-junit.xml", "install-gtest.xml"):\n'
            '    (reports / name).write_text("invalid")\n'
            'raise SystemExit(0)'
        )))
        result = self.run_verifier()
        self.assertNotEqual(0, result.returncode)
        self.assertNotIn("=== 3.", result.stdout, "artifact checks ran with invalid test evidence")
        self.assertIn("Required tests check failed", result.stderr)
        self.assertEqual([], list(self.tmp.iterdir()))

    def test_unknown_argument_fails_before_install(self):
        result = self.run_verifier(["--unknown-option"])
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertNotIn("FIXTURE_ENV=", result.stdout)
        self.assertEqual([], list(self.tmp.iterdir()))


if __name__ == "__main__":
    unittest.main()
