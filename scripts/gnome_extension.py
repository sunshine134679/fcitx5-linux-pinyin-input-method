#!/usr/bin/env python3
"""Current-user Kimpanel ownership, publication, and activation lifecycle."""
import argparse
import ast
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

UUID = "modernime-panel@sunshine134679.github.io"
LEGACY_UUID = "kimpanel@kde.org"
KEYS = ("enabled-extensions", "disabled-extensions")


class Preserved(Exception):
    """Ownership is absent or has changed; leave user state alone."""


def tree_digest(root):
    entries = []
    for path in [root, *sorted(root.rglob("*"))]:
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            content = None
        elif stat.S_ISREG(mode):
            content = hashlib.sha256(path.read_bytes()).hexdigest()
        else:
            raise Preserved(f"unsupported file or symbolic link: {path}")
        entries.append((str(path.relative_to(root)), mode, content))
    return hashlib.sha256(json.dumps(entries).encode()).hexdigest()


def command(*args):
    try:
        result = subprocess.run(args, text=True, capture_output=True, timeout=5,
                                env={**os.environ, "LC_ALL": "C"})
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"{args[0]}: {error}") from error
    if result.returncode:
        raise RuntimeError(f"{' '.join(args[:2])}: exit {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def settings_list(key):
    value = command("gsettings", "get", "org.gnome.shell", key).strip()
    if value.startswith("@as "):
        value = value[4:]
    try:
        parsed = ast.literal_eval(value)
    except (SyntaxError, ValueError) as error:
        raise RuntimeError(f"Invalid GNOME settings value for {key}") from error
    if not isinstance(parsed, list) or any(not isinstance(item, str) for item in parsed):
        raise RuntimeError(f"Invalid GNOME settings array for {key}")
    return parsed


def shell_info(target):
    info = command("gnome-extensions", "info", target.name)
    fields = dict(line.strip().split(": ", 1) for line in info.splitlines() if ": " in line)
    path = fields.get("Path", "")
    if not path or Path(path).resolve() != target.resolve():
        raise RuntimeError(f"GNOME Shell extension path does not match managed copy: {path or 'unknown'}")
    return fields.get("State", "UNKNOWN")


class ExtensionLifecycle:
    def __init__(self, args, uuid=UUID):
        self.uuid = uuid
        self.prefix = str(Path(args.prefix).resolve())
        self.target = Path(args.data_home).absolute() / "gnome-shell/extensions" / self.uuid
        record_name = "modernime-panel.json" if uuid == UUID else "gnome-extension.json"
        self.state = Path(args.config_home).absolute() / "modernime/session-managed-files" / record_name

    def save(self, record):
        # Atomic record replacement. A failed publication never grants ownership.
        fd, name = tempfile.mkstemp(prefix=".gnome-record-", dir=self.state.parent)
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(record, stream, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            if self.state.is_symlink():
                raise RuntimeError(f"Refusing symbolic ownership record: {self.state}")
            os.replace(name, self.state)
        finally:
            Path(name).unlink(missing_ok=True)

    def owned_record(self):
        if self.target.is_symlink() or self.state.is_symlink():
            raise Preserved("target or ownership record is a symbolic link")
        if not self.state.exists():
            if self.target.exists():
                raise Preserved("existing copy has no ModernIME ownership record")
            return None
        try:
            record = json.loads(self.state.read_text())
            valid = (record["version"] == 1 and record["prefix"] == self.prefix
                     and record["target"] == str(self.target.resolve())
                     and record["state_dir"] == str(self.state.parent.resolve())
                     and isinstance(record["digest"], str)
                     and isinstance(record["activation_checked"], bool))
            activation = record.get("activation")
            if activation is not None:
                valid = valid and (isinstance(activation, dict)
                                   and isinstance(activation["before"], dict)
                                   and set(activation["before"]) == set(KEYS)
                                   and all(type(v) is bool for v in activation["before"].values())
                                   and isinstance(activation.get("remaining", []), list)
                                   and set(activation.get("remaining", KEYS)) <= set(KEYS))
            if not valid:
                raise ValueError("record identity or format mismatch")
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise Preserved(f"unusable ownership record: {error}") from error
        if self.target.exists() and tree_digest(self.target) != record["digest"]:
            raise Preserved("managed copy was modified by the user")
        return record

    def deploy(self, bundle, activate=True):
        record = self.owned_record()
        if record and not self.target.exists():
            raise Preserved("managed copy was removed; run --remove-gnome-extension before deploying again")
        # Validate bundle before copying, including symlinks in the source tree.
        tree_digest(bundle)
        if json.loads((bundle / "metadata.json").read_text()).get("uuid") != self.uuid:
            raise RuntimeError("GNOME extension bundle UUID does not match its deployment directory")
        self.target.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".modernime-gnome-", dir=self.target.parent))
        incoming, backup = staging / "incoming", staging / "previous"
        published = False
        try:
            shutil.copytree(bundle, incoming)
            if (incoming / "schemas").is_dir():
                command("glib-compile-schemas", "--strict", str(incoming / "schemas"))
            digest = tree_digest(incoming)
            if record and digest == record["digest"]:
                if activate:
                    self.activate(record)
                return
            # An editor may have changed the target while schemas were compiling.
            self.owned_record()
            next_record = dict(record or {
                "version": 1, "prefix": self.prefix, "target": str(self.target.resolve()),
                "state_dir": str(self.state.parent.resolve()), "activation_checked": False,
            }, digest=digest)
            if self.target.exists():
                self.target.rename(backup)
            try:
                incoming.rename(self.target)
                published = True
                self.save(next_record)
            except Exception:
                if published:
                    self.target.rename(incoming)
                if backup.exists():
                    backup.rename(self.target)
                raise
            if backup.exists():
                shutil.rmtree(backup)
            print(f"GNOME extension deployed: {self.target}")
            if record:
                print("GNOME extension files updated; log out and back in to load the new code.")
            if activate:
                self.activate(next_record)
        finally:
            # A failed rollback leaves its only previous copy available for recovery.
            if backup.exists():
                print(f"GNOME recovery files preserved: {staging}", file=sys.stderr)
            else:
                shutil.rmtree(staging)

    def activate(self, record):
        if self.uuid == UUID and LEGACY_UUID in settings_list(KEYS[0]) and LEGACY_UUID not in settings_list(KEYS[1]):
            print("GNOME activation pending: existing Kimpanel is enabled and was preserved. "
                  "Disable it in Extensions before enabling the ModernIME Input Panel.")
            return
        try:
            state = shell_info(self.target)
        except RuntimeError as error:
            print(f"GNOME activation pending: {error}. Log out/in, then run modernime-session-init.")
            return
        if not record["activation_checked"]:
            before = {key: self.uuid in settings_list(key) for key in KEYS}
            record["activation_checked"] = True
            if not before[KEYS[0]] and not before[KEYS[1]]:
                # Persist intent before the external call; a partial/failed call
                # can still have changed GSettings and must remain recoverable.
                record["activation"] = {"before": before}
                self.save(record)
                command("gnome-extensions", "enable", self.uuid)
                state = shell_info(self.target)
            else:
                self.save(record)
        if state in ("ACTIVE", "ENABLED"):
            print(f"GNOME extension active: {self.uuid} (current Shell session)")
        else:
            print(f"GNOME extension state: {state}; enablement is not proof of a loaded extension.")

    def restore_activation(self, record):
        activation = record.get("activation")
        if not activation:
            return
        expected = {KEYS[0]: True, KEYS[1]: False}
        if "remaining" not in activation:
            current = {key: self.uuid in settings_list(key) for key in KEYS}
            if current != expected:
                print("GNOME enablement changed or was not applied; preserving current settings.")
                return
            shell_info(self.target)
            activation["remaining"] = list(KEYS)
            self.save(record)
        for key in list(activation["remaining"]):
            # A command can apply its change and then fail. Accept only the
            # persisted step's before/after states; other changes are the user's.
            expected_step = {name: expected[name] if name in activation["remaining"]
                             else activation["before"][name] for name in KEYS}
            applied_step = {**expected_step, key: activation["before"][key]}
            current = {name: self.uuid in settings_list(name) for name in KEYS}
            if current not in (expected_step, applied_step):
                print("GNOME enablement changed after restoration started; preserving current settings.")
                return
            shell_info(self.target)
            values = settings_list(key)
            wanted = activation["before"][key]
            if (self.uuid in values) != wanted:
                values = [value for value in values if value != self.uuid]
                if wanted:
                    values.append(self.uuid)
                command("gsettings", "set", "org.gnome.shell", key, repr(values))
                if (self.uuid in settings_list(key)) != wanted:
                    raise RuntimeError(f"GNOME settings restoration not observed: {key}")
            activation["remaining"].remove(key)
            self.save(record)

    def remove(self):
        record = self.owned_record()
        if record is None:
            print("No managed GNOME extension to remove.")
            return
        if not self.target.exists():
            self.state.unlink()
            print("GNOME managed copy already removed; ownership record released. "
                  "Shell settings retained because file identity can no longer be confirmed.")
            return
        self.restore_activation(record)
        # Recheck after desktop calls before touching the tree.
        self.owned_record()
        if self.target.exists():
            shutil.rmtree(self.target)
        self.state.unlink()
        print(f"GNOME managed extension removed: {self.target}")

    def transfer(self, new_prefix):
        record = self.owned_record()
        if record is None:
            print("No managed GNOME extension ownership to transfer.")
            return
        if not self.target.exists():
            self.remove()
            return
        record["prefix"] = str(new_prefix.resolve())
        self.save(record)
        print(f"GNOME managed extension ownership transferred to: {record['prefix']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("deploy", "remove", "transfer"))
    for key in ("prefix", "config-home", "data-home"):
        parser.add_argument("--" + key, required=True)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--new-prefix", type=Path)
    args = parser.parse_args()
    lifecycle = ExtensionLifecycle(args)
    legacy = ExtensionLifecycle(args, LEGACY_UUID)
    try:
        if args.action in ("remove", "transfer") and not any(
                item.state.exists() or item.state.is_symlink() for item in (lifecycle, legacy)):
            print("No managed GNOME extension to remove.")
            return 0
        lifecycle.state.parent.mkdir(parents=True, exist_ok=True)
        lock = lifecycle.state.parent / "gnome-extension.lock"
        fd = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for recovery in sorted(lifecycle.target.parent.glob(".modernime-gnome-*")):
                print(f"GNOME unfinished deployment materials preserved: {recovery}", file=sys.stderr)
            if args.action == "deploy":
                if args.bundle is None:
                    raise RuntimeError("--bundle is required for deploy")
                migrating = legacy.state.exists() or legacy.state.is_symlink()
                lifecycle.deploy(args.bundle, activate=not migrating)
                # Publish the replacement first. Failed publication leaves the
                # working old extension and its ownership record untouched.
                if migrating:
                    try:
                        legacy_record = legacy.owned_record()
                        state = shell_info(lifecycle.target)
                        if state in ("ERROR", "OUT_OF_DATE"):
                            raise RuntimeError(f"new panel state: {state}")
                    except RuntimeError as error:
                        print(f"GNOME migration pending: {error}. Old panel retained; log out/in and retry.")
                        return 0
                    new_record = lifecycle.owned_record()
                    if legacy_record["activation_checked"] and (
                            LEGACY_UUID not in settings_list(KEYS[0]) or LEGACY_UUID in settings_list(KEYS[1])):
                        # A user who disabled the old panel did not opt into
                        # automatic enablement merely by upgrading its files.
                        new_record["activation_checked"] = True
                        lifecycle.save(new_record)
                    try:
                        legacy.remove()
                    except Preserved as error:
                        print(f"Preserving legacy GNOME extension {legacy.target}: {error}", file=sys.stderr)
                    lifecycle.activate(lifecycle.owned_record())
            else:
                for item in (lifecycle, legacy):
                    if not item.state.exists() and not item.state.is_symlink():
                        continue
                    try:
                        if args.action == "transfer":
                            if args.new_prefix is None:
                                raise RuntimeError("--new-prefix is required for transfer")
                            item.transfer(args.new_prefix)
                        else:
                            item.remove()
                    except Preserved as error:
                        print(f"Preserving GNOME extension {item.target}: {error}", file=sys.stderr)
        return 0
    except Preserved as error:
        print(f"Preserving GNOME extension {lifecycle.target}: {error}", file=sys.stderr)
        return 0
    except (OSError, RuntimeError) as error:
        print(f"GNOME integration incomplete: {error}. Retry from the matching user session.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
