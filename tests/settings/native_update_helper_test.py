#!/usr/bin/env python3
"""客户端更新助手的隔离下载、校验和安装验证。"""

import hashlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock


HELPER = Path(__file__).resolve().parents[2] / "scripts/modernime-update"
VERSION = "0.2.1"
ASSET = "modernime-0.2.1-linux-x86_64.tar.gz"
ROOT = "modernime-0.2.1-linux-x86_64"
BASE = "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases/download/v0.2.1/"


class Response(io.BytesIO):
    def __init__(self, data, url):
        super().__init__(data)
        self.headers = {"Content-Length": str(len(data))}
        self.url = url

    def geturl(self):
        return self.url


def archive(extra=None, reported_version="0.2.1"):
    output = io.BytesIO()
    installer = (
        '#!/bin/sh\nset -eu\n'
        'test "$(cat payload-link)" = "payload"\n'
        'printf "%s\\n%s\\n" "$MODERNIME_PREFIX" "$MODERNIME_SKIP_FCITX_RESTART" '
        '> "$MODERNIME_PREFIX/install-observed.txt"\n'
        'cp new-settings "$MODERNIME_PREFIX/bin/modernime-settings"\n'
        'chmod +x "$MODERNIME_PREFIX/bin/modernime-settings"\n'
    ).encode()
    settings = f'#!/bin/sh\nprintf "ModernIME settings {reported_version}\\n"\n'.encode()
    with tarfile.open(fileobj=output, mode="w:gz") as tar:
        for name, data in [(ROOT + "/install.sh", installer), (ROOT + "/payload", b"payload"),
                           (ROOT + "/new-settings", settings),
                           (ROOT + "/usr/bin/modernime-settings", settings),
                           (ROOT + "/usr/lib/fcitx5/modernime_fcitx5.so", b"isolated library fixture")]:
            item = tarfile.TarInfo(name)
            item.size, item.mode = len(data), 0o755
            tar.addfile(item, io.BytesIO(data))
        link = tarfile.TarInfo(ROOT + "/payload-link")
        link.type, link.linkname = tarfile.SYMTYPE, "payload"
        tar.addfile(link)
        if extra is not None:
            tar.addfile(extra, io.BytesIO(b"bad") if extra.isfile() else None)
    return output.getvalue()


class NativeUpdateHelperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HELPER.is_file():
            raise AssertionError("客户端原生更新助手尚未实现")
        loader = importlib.machinery.SourceFileLoader("modernime_update", str(HELPER))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        cls.helper = importlib.util.module_from_spec(spec)
        loader.exec_module(cls.helper)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="modernime-update-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "原安装 prefix"
        (self.prefix / "bin").mkdir(parents=True)
        (self.prefix / "bin/modernime-settings").write_text("old binary")
        metadata = self.prefix / "share/modernime"
        metadata.mkdir(parents=True)
        (metadata / "install-channel.txt").write_text(
            f"channel=script\nversion=0.2.0\nprefix={self.prefix}\n"
        )
        (metadata / "install-manifest.txt").write_text(str(self.prefix / "bin/modernime-settings") + "\n")
        self.directory = self.root / "download"
        self.directory.mkdir(mode=0o700)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(self.helper.platform, "machine", return_value="x86_64").start()
        mock.patch.object(self.helper.platform, "libc_ver", return_value=("glibc", "2.39")).start()
        self.ldd_output = "libc.so.6 => /lib/libc.so.6 (0x00000000)"
        original_query = self.helper.query

        def query(argv):
            if argv[0] == "ldd":
                return self.ldd_output
            return original_query(argv)

        mock.patch.object(self.helper, "query", side_effect=query).start()

    def download(self, data, checksum=None, channel="script", asset=ASSET, prefix=None):
        checksum = checksum or hashlib.sha256(data).hexdigest()
        responses = {
            BASE + "SHA256SUMS.txt": f"{checksum}  {asset}\n".encode(),
            BASE + asset: data,
        }

        def open_url(request, **kwargs):
            self.assertGreater(kwargs["timeout"], 0)
            return Response(responses[request.full_url], request.full_url)

        with mock.patch.object(self.helper.urllib.request.OpenerDirector, "open", side_effect=open_url):
            return self.helper.main([
                "download", "--version", VERSION, "--channel", channel,
                "--prefix", str(prefix or self.prefix), "--directory", str(self.directory),
            ])

    def install(self, channel="script", prefix=None):
        return self.helper.main([
            "install", "--channel", channel, "--prefix", str(prefix or self.prefix),
            "--directory", str(self.directory),
        ])

    def test_download_and_install_keep_prefix_and_skip_restart(self):
        self.assertEqual(self.download(archive()), 0)
        prepared = json.loads((self.directory / "prepared.json").read_text())
        self.assertEqual(prepared["version"], VERSION)
        self.assertEqual(prepared["channel"], "script")
        self.assertEqual(prepared["prefix"], str(self.prefix))
        self.assertEqual(self.install(), 0)
        self.assertEqual((self.prefix / "install-observed.txt").read_text(), f"{self.prefix}\n1\n")
        self.assertIn("phase=complete\n", (self.directory / "progress.ini").read_text())

    def test_installer_success_with_wrong_binary_is_not_complete(self):
        self.assertEqual(self.download(archive(reported_version="0.2.0")), 0)
        self.assertNotEqual(self.install(), 0)
        self.assertNotIn("phase=complete\n", (self.directory / "progress.ini").read_text())

    def test_checksum_failure_does_not_prepare_or_install(self):
        self.assertNotEqual(self.download(archive(), checksum="0" * 64), 0)
        self.assertFalse((self.directory / "prepared.json").exists())
        self.assertNotEqual(self.install(), 0)
        self.assertFalse((self.prefix / "install-observed.txt").exists())

    def test_install_rechecks_downloaded_bytes(self):
        self.assertEqual(self.download(archive()), 0)
        (self.directory / ASSET).write_bytes(b"changed after download")
        self.assertNotEqual(self.install(), 0)
        self.assertFalse((self.prefix / "install-observed.txt").exists())

    def test_missing_payload_dependency_stops_before_install(self):
        self.ldd_output = "libmissing.so.1 => not found"
        self.assertEqual(self.download(archive()), 0)
        self.assertNotEqual(self.install(), 0)
        self.assertFalse((self.prefix / "install-observed.txt").exists())

    def test_archive_escape_is_rejected_before_installer(self):
        path_escape = tarfile.TarInfo(ROOT + "/../../escaped")
        path_escape.size = 3
        link_escape = tarfile.TarInfo(ROOT + "/escape-link")
        link_escape.type, link_escape.linkname = tarfile.SYMTYPE, "../../escaped"
        for member in (path_escape, link_escape):
            with self.subTest(member=member.name):
                self.assertEqual(self.download(archive(member)), 0)
                self.assertNotEqual(self.install(), 0)
                self.assertFalse((self.prefix / "install-observed.txt").exists())
                self.assertFalse((self.root / "escaped").exists())

    def test_channel_mismatch_is_rejected_without_download(self):
        (self.prefix / "share/modernime/install-channel.txt").write_text(
            f"channel=deb\nversion=0.2.0\nprefix={self.prefix}\n"
        )
        with mock.patch.object(self.helper.urllib.request.OpenerDirector, "open") as network:
            self.assertNotEqual(self.download(archive()), 0)
            network.assert_not_called()
        self.assertFalse((self.directory / "prepared.json").exists())

    def test_native_package_install_uses_original_manager_and_local_asset(self):
        cases = [
            ("deb", {"ID": "ubuntu", "VERSION_ID": "24.04"},
             "modernime_0.2.1_amd64_ubuntu24.04.deb", "amd64",
             ["pkexec", "apt-get", "install", "--only-upgrade", "-y"]),
            ("rpm", {"ID": "openEuler", "VERSION_ID": "24.09"},
             "modernime-0.2.1-1.oe24.x86_64.rpm", "x86_64",
             ["pkexec", "dnf", "upgrade", "-y", "--setopt=localpkg_gpgcheck=1"]),
        ]
        for channel, release, asset, architecture, expected in cases:
            with self.subTest(channel=channel), \
                 mock.patch.object(self.helper, "validate_installation"), \
                 mock.patch.object(self.helper.platform, "freedesktop_os_release", return_value=release), \
                 mock.patch.object(self.helper.subprocess, "run") as run:
                def run_package(argv, **kwargs):
                    output = "ModernIME settings 0.2.1\n" if argv[0].endswith("/modernime-settings") else f"modernime\n0.2.1\n{architecture}\n"
                    return subprocess.CompletedProcess(argv, 0, output, "")
                run.side_effect = run_package
                self.assertEqual(self.download(b"package bytes", channel=channel, asset=asset, prefix=Path("/usr")), 0)
                self.assertEqual(self.install(channel=channel, prefix=Path("/usr")), 0)
                installation = [call for call in run.call_args_list if call.args[0][0] == "pkexec"]
                self.assertEqual(len(installation), 1)
                self.assertEqual(installation[0].args[0], expected + [str(self.directory / asset)])
                self.assertNotIn("timeout", installation[0].kwargs)

    def test_unsupported_package_platform_has_no_fallback(self):
        with mock.patch.object(self.helper, "validate_installation"), \
             mock.patch.object(self.helper.platform, "freedesktop_os_release", return_value={"ID": "debian", "VERSION_ID": "12"}):
            self.assertNotEqual(self.download(b"unused", channel="deb", prefix=Path("/usr")), 0)
        self.assertFalse((self.directory / "prepared.json").exists())

    def test_restart_failure_still_opens_updated_client(self):
        executable = self.prefix / "bin/modernime-settings"
        executable.chmod(0o755)
        with mock.patch.object(self.helper, "process_start_time", return_value=None), \
             mock.patch.object(self.helper.subprocess, "Popen", side_effect=FileNotFoundError("fcitx5")), \
             mock.patch.object(self.helper.os, "execv") as execute:
            self.assertEqual(self.helper.main([
                "restart", "--prefix", str(self.prefix), "--parent-pid", "12345",
                "--parent-start-time", "123", "--directory", str(self.directory),
            ]), 0)
            execute.assert_called_once_with(str(executable), [str(executable), "--page", "updates"])


if __name__ == "__main__":
    unittest.main()
