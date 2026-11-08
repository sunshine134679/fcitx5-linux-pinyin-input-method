#!/usr/bin/env python3
"""Exercise the real session/uninstall entry points without a desktop side effect."""
import ast
import fcntl
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UUID = "kimpanel@kde.org"

# Only desktop services are simulated. File publication, ownership, and removal
# use the actual scripts and filesystem. GNOME 46 enable updates both keys.
DESKTOP_TOOL = r'''#!/usr/bin/python3
import ast, json, os, sys
from pathlib import Path
tool = Path(sys.argv[0]).name
root = Path(os.environ["GNOME_TEST_ROOT"])
with (root / "calls").open("a") as log:
    log.write(json.dumps([tool, *sys.argv[1:]]) + "\n")
uuid = "kimpanel@kde.org"
settings_file = root / "settings.json"
settings = json.loads(settings_file.read_text())
if tool == "glib-compile-schemas":
    if (root / "schema-fail").exists(): sys.exit(21)
    (Path(sys.argv[-1]) / "gschemas.compiled").write_text("compiled")
elif tool == "gnome-extensions":
    if sys.argv[1] == "enable":
        if (root / "enable-fail").exists(): sys.exit(22)
        if uuid not in settings["enabled-extensions"]:
            settings["enabled-extensions"].append(uuid)
        settings["disabled-extensions"] = [s for s in settings["disabled-extensions"] if s != uuid]
        settings_file.write_text(json.dumps(settings))
    elif sys.argv[1] == "info":
        if (root / "info-fail").exists(): sys.exit(24)
        state = (root / "shell-state").read_text()
        path = os.environ["MODERNIME_DATA_HOME"] + "/gnome-shell/extensions/" + uuid
        if (root / "other-shell-copy").exists(): path = str(root / "other-copy" / uuid)
        print(uuid + "\n  Name: Kimpanel\n  Path: " + path + "\n  State: " + state)
    else: sys.exit(91)
elif tool == "gsettings":
    assert sys.argv[2] == "org.gnome.shell", sys.argv
    key = sys.argv[3]
    if sys.argv[1] == "get":
        if (root / "get-fail").exists(): sys.exit(23)
        print(repr(settings[key]))
    elif sys.argv[1] == "set":
        if (root / ("set-fail-" + key)).exists(): sys.exit(25)
        settings[key] = ast.literal_eval(sys.argv[4])
        settings_file.write_text(json.dumps(settings))
        if (root / ("set-partial-" + key)).exists(): sys.exit(26)
    else: sys.exit(92)
elif tool not in ("gnome-shell", "gio", "gtk-update-icon-cache", "xdg-user-dir"):
    sys.exit(93)
'''


class GnomeExtensionLifecycleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="modernime-gnome-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "prefix with space"
        self.config = self.root / "config"
        self.data = self.root / "data"
        self.target = self.data / "gnome-shell/extensions" / UUID
        self.bundle = self.prefix / "share/modernime/extensions" / UUID
        self.bundle.mkdir(parents=True)
        (self.bundle / "metadata.json").write_text(json.dumps({"uuid": UUID}))
        (self.bundle / "extension.js").write_text("bundled v1")
        (self.bundle / "schemas").mkdir()
        (self.bundle / "schemas/schema.xml").write_text("schema input")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for tool in ("gnome-extensions", "gsettings", "glib-compile-schemas",
                     "gnome-shell", "gio", "gtk-update-icon-cache", "xdg-user-dir"):
            path = self.bin / tool
            path.write_text(DESKTOP_TOOL)
            path.chmod(0o755)
        self.initial = {"enabled-extensions": ["other@example.org"], "disabled-extensions": []}
        self.set_settings(self.initial)
        (self.root / "shell-state").write_text("ACTIVE")
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("MODERNIME_", "XDG_", "GSETTINGS_", "DBUS_"))
                    and k not in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY")}
        self.env.update(HOME=str(self.root / "home"), PATH=f"{self.bin}:/usr/bin:/bin",
                        MODERNIME_PREFIX=str(self.prefix), MODERNIME_CONFIG_HOME=str(self.config),
                        MODERNIME_DATA_HOME=str(self.data), MODERNIME_SKIP_FCITX_RESTART="1",
                        MODERNIME_SKIP_DESKTOP_SHORTCUT="1", MODERNIME_ALLOW_ROOT="1",
                        XDG_CONFIG_HOME=str(self.config), XDG_DATA_HOME=str(self.data),
                        XDG_CACHE_HOME=str(self.root / "cache"), XDG_STATE_HOME=str(self.root / "state"),
                        XDG_RUNTIME_DIR=str(self.root / "runtime"), XDG_CURRENT_DESKTOP="ubuntu:GNOME",
                        DISPLAY=":test", DBUS_SESSION_BUS_ADDRESS=f"unix:path={self.root}/no-bus",
                        GNOME_TEST_ROOT=str(self.root))
        for name in ("home", "runtime"):
            (self.root / name).mkdir(mode=0o700)

    def set_settings(self, value):
        (self.root / "settings.json").write_text(json.dumps(value))

    def settings(self):
        return json.loads((self.root / "settings.json").read_text())

    def run_script(self, script, *args, ok=True):
        result = subprocess.run(["bash", str(ROOT / script), *args], env=self.env,
                                capture_output=True, text=True, timeout=25)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def deploy(self, ok=True):
        return self.run_script("scripts/modernime-session-init", ok=ok)

    def remove(self, ok=True):
        return self.run_script("scripts/modernime-session-init", "--remove-gnome-extension", ok=ok)

    def test_preexisting_user_copy_is_not_overwritten_or_enabled(self):
        self.target.mkdir(parents=True)
        (self.target / "extension.js").write_text("user version")
        self.deploy()
        self.assertEqual((self.target / "extension.js").read_text(), "user version")
        self.assertFalse((self.target / "metadata.json").exists())
        self.assertEqual(self.settings(), self.initial)

    def test_installed_gnome_binary_is_not_an_active_gnome_session(self):
        for desktop, display, bus in (("KDE", ":test", "unix:path=/none"),
                                      ("GNOME", "", ""), ("NOTGNOME", ":test", "unix:path=/none")):
            with self.subTest(desktop=desktop):
                self.env.update(XDG_CURRENT_DESKTOP=desktop, DISPLAY=display, DBUS_SESSION_BUS_ADDRESS=bus)
                self.deploy()
                self.assertFalse(self.target.exists())
                self.assertEqual(self.settings(), self.initial)

    def test_schema_failure_does_not_publish_partial_copy(self):
        (self.root / "schema-fail").touch()
        result = self.deploy(ok=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.target.exists())
        self.assertEqual(self.settings(), self.initial)

    def test_enable_failure_is_reported_after_file_deployment(self):
        (self.root / "enable-fail").touch()
        result = self.deploy(ok=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("22", result.stdout + result.stderr)
        self.assertEqual((self.target / "extension.js").read_text(), "bundled v1")
        self.remove()
        self.assertFalse(self.target.exists())

    def test_owned_update_and_remove_preserve_other_extensions(self):
        self.deploy()
        self.assertIn(UUID, self.settings()["enabled-extensions"])
        (self.bundle / "extension.js").write_text("bundled v2")
        self.deploy()
        self.assertEqual((self.target / "extension.js").read_text(), "bundled v2")
        current = self.settings()
        current["enabled-extensions"].append("added-later@example.org")
        self.set_settings(current)
        self.remove()
        self.assertFalse(self.target.exists())
        self.assertEqual(self.settings(), {"enabled-extensions": ["other@example.org", "added-later@example.org"],
                                          "disabled-extensions": []})
        self.remove()

    def test_user_disabled_extension_stays_disabled_on_reinitialization(self):
        self.deploy()
        changed = {"enabled-extensions": ["other@example.org"], "disabled-extensions": [UUID]}
        self.set_settings(changed)
        self.deploy()
        self.assertEqual(self.settings(), changed)
        self.remove()
        self.assertEqual(self.settings(), changed)
        self.assertFalse(self.target.exists())

    def test_user_modified_tree_is_preserved_on_update_and_remove(self):
        mutations = {
            "content": lambda: (self.target / "extension.js").write_text("user edit"),
            "added-file": lambda: (self.target / "notes").write_text("user notes"),
            "empty-dir": lambda: (self.target / "empty").mkdir(),
            "mode": lambda: (self.target / "extension.js").chmod(0o600),
            "symlink": lambda: (self.target / "link").symlink_to(self.bundle / "extension.js"),
        }
        # Each variant needs its own original ownership, not the previous edited tree.
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                self.deploy()
                before = self.settings()
                mutate()
                (self.bundle / "extension.js").write_text("replacement")
                self.deploy()
                self.remove()
                self.assertTrue(self.target.exists())
                self.assertEqual(self.settings(), before)
                if name == "content":
                    self.assertEqual((self.target / "extension.js").read_text(), "user edit")
                shutil.rmtree(self.target)
                self.remove()

    def test_source_uninstall_removes_owned_copy_before_payload(self):
        self.deploy()
        manifest = self.prefix / "share/modernime/install-manifest.txt"
        manifest.write_text(str(self.bundle / "extension.js") + "\n")
        self.run_script("uninstall.sh")
        self.assertFalse(self.target.exists())
        self.assertFalse((self.bundle / "extension.js").exists())
        self.assertEqual(self.settings(), self.initial)

    def test_partial_settings_restore_can_be_retried(self):
        self.deploy()
        # Removal must not discard the ownership record when a later key fails.
        (self.root / "set-partial-enabled-extensions").touch()
        result = self.remove(ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.target.exists())
        (self.root / "set-partial-enabled-extensions").unlink()
        self.remove()
        self.assertEqual(self.settings(), self.initial)
        self.assertFalse(self.target.exists())

    def test_loaded_state_is_distinct_from_successful_enable_request(self):
        (self.root / "shell-state").write_text("INITIALIZED")
        result = self.deploy()
        self.assertIn("INITIALIZED", result.stdout + result.stderr)
        self.assertNotIn("GNOME extension active:", result.stdout)

    def test_other_prefix_cannot_claim_or_remove_copy(self):
        self.deploy()
        self.env["MODERNIME_PREFIX"] = str(self.root / "other-prefix")
        self.remove()
        self.assertTrue(self.target.exists())
        self.assertIn(UUID, self.settings()["enabled-extensions"])

    def test_shell_identity_mismatch_never_changes_another_copy(self):
        (self.root / "other-shell-copy").touch()
        self.deploy()
        self.assertEqual(self.settings(), self.initial)
        self.remove()
        self.assertFalse(self.target.exists())
        (self.root / "other-shell-copy").unlink()
        self.deploy()
        (self.root / "other-shell-copy").touch()
        result = self.remove(ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.target.exists())
        self.assertIn(UUID, self.settings()["enabled-extensions"])

    def test_record_publication_failure_restores_previous_tree(self):
        self.deploy()
        (self.bundle / "extension.js").write_text("new version")
        record = self.config / "modernime/session-managed-files/gnome-extension.json"
        original_record = record.read_bytes()
        # Fail the actual atomic record publication, after the tree rename. No
        # production hooks; patch only the OS failure boundary in this child.
        code = '''import importlib.util, os, sys
from unittest.mock import patch
spec = importlib.util.spec_from_file_location("gnome_extension", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
sys.argv = [sys.argv[1], *sys.argv[2:]]
def fail_record_replace(source, target):
    raise PermissionError("record publication denied")
with patch.object(module.os, "replace", fail_record_replace):
    sys.exit(module.main())
'''
        result = subprocess.run(["python3", "-c", code, str(ROOT / "scripts/gnome_extension.py"),
                                 "deploy", "--prefix", str(self.prefix), "--config-home", str(self.config),
                                 "--data-home", str(self.data), "--bundle", str(self.bundle)],
                                env=self.env, capture_output=True, text=True, timeout=25)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.target / "extension.js").read_text(), "bundled v1")
        self.assertEqual(record.read_bytes(), original_record)

    def test_installers_report_committed_files_when_session_init_fails(self):
        for channel in ("source", "portable"):
            with self.subTest(channel=channel):
                prefix = self.root / channel
                prefix.mkdir()
                self.env.update(MODERNIME_PREFIX=str(prefix), MODERNIME_BUILD_DIR=str(self.root / "build"),
                                MODERNIME_SKIP_TESTS="1", MODERNIME_ALLOW_MISSING_DEPS="1")
                payload = self.root / "package"
                if channel == "source":
                    destination = prefix
                    installer = ROOT / "install.sh"
                    for tool in ("cmake", "ctest"):
                        path = self.bin / tool
                        path.write_text('''#!/bin/sh
if [ "${1:-}" = --install ]; then
    mkdir -p "$DESTDIR$MODERNIME_PREFIX/bin"
    cp "$MODERNIME_PREFIX/bin/modernime-settings" "$DESTDIR$MODERNIME_PREFIX/bin/"
    cp "$MODERNIME_PREFIX/bin/modernime-session-init" "$DESTDIR$MODERNIME_PREFIX/bin/"
fi
exit 0
''')
                        path.chmod(0o755)
                else:
                    destination = payload / "usr"
                    installer = payload / "install.sh"
                (destination / "bin").mkdir(parents=True)
                (destination / "share/modernime/pinyin").mkdir(parents=True)
                (destination / "lib/x86_64-linux-gnu/fcitx5").mkdir(parents=True)
                for library in ("modernime_fcitx5.so", "modernime_ui.so"):
                    (destination / "lib/x86_64-linux-gnu/fcitx5" / library).write_text("fixture")
                for name in ("modernime-knowledge.dict", "modernime-hotwords.dict", "modernime-english-dict.bin"):
                    (destination / "share/modernime/pinyin" / name).write_text("fixture")
                for name, body in (("modernime-settings", "echo modernime-settings 0.1.25"),
                                   ("modernime-session-init", "echo 'fixture session failure' >&2; exit 37")):
                    path = destination / "bin" / name
                    path.write_text("#!/bin/sh\n" + body + "\n")
                    path.chmod(0o755)
                if channel == "portable":
                    self.run_script("scripts/generate_binary_installer.sh", str(payload))
                else:
                    (prefix / "share/modernime/install-manifest.txt").write_text(
                        str(prefix / "bin/modernime-settings") + "\n")
                    (prefix / "share/modernime/install-channel.txt").write_text("channel=script\nversion=0.1.25\n")
                result = subprocess.run(["bash", str(installer)], env=self.env,
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
                self.assertIn("files installed; session initialization incomplete", result.stdout + result.stderr)
                self.assertTrue((prefix / "share/modernime/install-manifest.txt").exists())
                self.assertTrue((prefix / "bin/modernime-settings").exists())

    def test_uninstall_stops_before_payload_removal_if_settings_restore_fails(self):
        self.deploy()
        manifest = self.prefix / "share/modernime/install-manifest.txt"
        manifest.write_text(str(self.bundle / "extension.js") + "\n")
        (self.root / "get-fail").touch()
        result = self.run_script("uninstall.sh", ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(manifest.exists())
        self.assertTrue(self.target.exists())
        self.assertTrue((self.bundle / "extension.js").exists())

    def test_symbolic_target_and_corrupt_record_cannot_grant_ownership(self):
        external = self.root / "external"
        external.mkdir()
        (external / "extension.js").write_text("external version")
        self.target.parent.mkdir(parents=True)
        self.target.symlink_to(external, target_is_directory=True)
        self.deploy()
        self.remove()
        self.assertTrue(self.target.is_symlink())
        self.assertEqual((external / "extension.js").read_text(), "external version")
        self.target.unlink()
        self.deploy()
        record = self.config / "modernime/session-managed-files/gnome-extension.json"
        record.write_text("broken ownership record")
        (self.bundle / "extension.js").write_text("new version")
        self.deploy()
        self.remove()
        self.assertEqual((self.target / "extension.js").read_text(), "bundled v1")
        self.assertEqual(record.read_text(), "broken ownership record")

    def test_existing_enablement_or_disable_choice_is_not_claimed(self):
        for key in ("enabled-extensions", "disabled-extensions"):
            with self.subTest(key=key):
                before = {"enabled-extensions": ["other@example.org"], "disabled-extensions": []}
                before[key].append(UUID)
                self.set_settings(before)
                self.deploy()
                self.remove()
                self.assertEqual(self.settings(), before)
                self.assertFalse(self.target.exists())

    def test_malformed_activation_record_preserves_copy_without_traceback(self):
        self.deploy()
        record = self.config / "modernime/session-managed-files/gnome-extension.json"
        value = json.loads(record.read_text())
        value["activation"]["before"] = ["enabled-extensions", "disabled-extensions"]
        record.write_text(json.dumps(value))
        before = self.settings()
        self.remove()
        self.assertTrue(self.target.exists())
        self.assertEqual(self.settings(), before)

    def test_unknown_shell_copy_is_deployed_without_enabling(self):
        (self.root / "info-fail").touch()
        result = self.deploy()
        self.assertIn("activation pending", result.stdout)
        self.assertEqual(self.settings(), self.initial)
        self.assertTrue(self.target.exists())
        self.remove()
        self.assertFalse(self.target.exists())

    def test_concurrent_operation_is_bounded_and_does_not_change_copy(self):
        self.deploy()
        lock = self.config / "modernime/session-managed-files/gnome-extension.lock"
        with lock.open("w") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.remove(ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.target.exists())
        self.remove()
        self.assertFalse(self.target.exists())

    def test_real_bundled_schema_compiles_and_invalid_update_preserves_old_copy(self):
        shutil.rmtree(self.bundle)
        shutil.copytree(ROOT / "data/extensions" / UUID, self.bundle)
        (self.bin / "glib-compile-schemas").unlink()
        self.deploy()
        compiled = (self.target / "schemas/gschemas.compiled").read_bytes()
        self.assertGreater(len(compiled), 100)
        previous = (self.target / "extension.js").read_bytes()
        (self.bundle / "schemas/org.gnome.shell.extensions.kimpanel.gschema.xml").write_text("invalid schema")
        result = self.deploy(ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.target / "schemas/gschemas.compiled").read_bytes(), compiled)
        self.assertEqual((self.target / "extension.js").read_bytes(), previous)
        self.remove()
        self.assertFalse(self.target.exists())

    def test_interrupted_publication_preserves_recovery_copy_and_reports_it(self):
        self.deploy()
        (self.bundle / "extension.js").write_text("new version")
        code = '''import importlib.util, os, sys
from unittest.mock import patch
spec = importlib.util.spec_from_file_location("gnome_extension", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
sys.argv = [sys.argv[1], *sys.argv[2:]]
with patch.object(module.ExtensionLifecycle, "save", lambda *_: os._exit(77)):
    module.main()
'''
        result = subprocess.run(["python3", "-c", code, str(ROOT / "scripts/gnome_extension.py"),
                                 "deploy", "--prefix", str(self.prefix), "--config-home", str(self.config),
                                 "--data-home", str(self.data), "--bundle", str(self.bundle)],
                                env=self.env, capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode, 77)
        backups = list(self.target.parent.glob(".modernime-gnome-*/previous/extension.js"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "bundled v1")
        result = self.deploy()
        self.assertIn(str(backups[0].parent.parent), result.stdout + result.stderr)
        self.remove()
        self.assertEqual(backups[0].read_text(), "bundled v1")
        self.assertEqual((self.target / "extension.js").read_text(), "new version")

    def test_user_disable_after_partial_restore_failure_is_preserved(self):
        self.deploy()
        failure = self.root / "set-partial-enabled-extensions"
        failure.touch()
        self.assertNotEqual(self.remove(ok=False).returncode, 0)
        changed = self.settings()
        changed["disabled-extensions"].append(UUID)
        self.set_settings(changed)
        failure.unlink()
        self.remove()
        self.assertEqual(self.settings(), changed)
        self.assertFalse(self.target.exists())

    def test_manually_deleted_copy_can_release_record_without_touching_unknown_shell(self):
        self.deploy()
        shutil.rmtree(self.target)
        (self.root / "info-fail").touch()
        before = self.settings()
        self.remove()
        self.assertFalse((self.config / "modernime/session-managed-files/gnome-extension.json").exists())
        self.assertEqual(self.settings(), before)
        self.deploy()
        self.assertTrue(self.target.exists())

    def make_migration_source(self):
        # Real installed consumer, including the data guard and its dependencies.
        (self.prefix / "bin").mkdir(exist_ok=True)
        for name in ("modernime-session-init", "modernime-migrate-channel", "modernime-data-guard"):
            shutil.copy2(ROOT / "scripts" / name, self.prefix / "bin" / name)
        for name in ("install_state.sh", "install_lifecycle.sh", "gnome_extension.py"):
            shutil.copy2(ROOT / "scripts" / name, self.prefix / "share/modernime" / name)
        settings = self.prefix / "bin/modernime-settings"
        settings.write_text("#!/bin/sh\necho modernime-settings 0.1.25\n")
        settings.chmod(0o755)
        (self.prefix / "lib/fcitx5").mkdir(parents=True)
        (self.prefix / "lib/fcitx5/modernime_fcitx5.so").write_text("plugin fixture")
        (self.prefix / "share/modernime/pinyin").mkdir()
        (self.prefix / "share/modernime/pinyin/modernime-knowledge.dict").write_text("dictionary fixture")
        (self.prefix / "share/modernime/install-channel.txt").write_text("channel=script\nversion=0.1.25\n")
        manifest = self.prefix / "share/modernime/install-manifest.txt"
        manifest.write_text("\n".join(str(p) for p in self.prefix.rglob("*") if p.is_file()) + "\n")

    def test_source_migration_failure_preserves_extension_and_settings(self):
        self.deploy()
        self.make_migration_source()
        record = self.config / "modernime/session-managed-files/gnome-extension.json"
        before_record, before_settings = record.read_bytes(), self.settings()
        result = self.run_script("scripts/modernime-migrate-channel", "--from-prefix", str(self.prefix),
                                 "--target-prefix", str(self.root / "new-native"), "--to-channel", "deb",
                                 "--package-cmd", "false", "--no-session-init", ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Original installation restored", result.stdout + result.stderr)
        self.assertTrue(self.target.exists())
        self.assertEqual((self.target / "extension.js").read_text(), "bundled v1")
        self.assertEqual(self.settings(), before_settings)
        self.assertEqual(record.read_bytes(), before_record)

    def test_installed_migration_transfers_only_unchanged_extension_ownership(self):
        self.deploy()
        self.make_migration_source()
        native = self.root / "new-native"
        payload = self.root / "native-payload"
        shutil.copytree(self.prefix, payload)
        (payload / "share/modernime/install-channel.txt").write_text("channel=deb\nversion=0.1.25\n")
        package_cmd = f"mkdir -p {shlex.quote(str(native))} && cp -a {shlex.quote(str(payload))}/. {shlex.quote(str(native))}/"
        before = self.settings()
        result = subprocess.run(["bash", str(self.prefix / "bin/modernime-migrate-channel"),
                                 "--from-prefix", str(self.prefix), "--target-prefix", str(native),
                                 "--to-channel", "deb", "--package-cmd", package_cmd, "--no-session-init"],
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.target.exists())
        self.assertEqual(self.settings(), before)
        self.assertFalse((self.prefix / "bin/modernime-settings").exists())
        record = json.loads((self.config / "modernime/session-managed-files/gnome-extension.json").read_text())
        self.assertEqual(record["prefix"], str(native))
        self.env["MODERNIME_PREFIX"] = str(native)
        result = subprocess.run(["bash", str(native / "bin/modernime-session-init"), "--remove-gnome-extension"],
                                env=self.env, capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.target.exists())
        self.assertEqual(self.settings(), self.initial)


if __name__ == "__main__":
    unittest.main()
