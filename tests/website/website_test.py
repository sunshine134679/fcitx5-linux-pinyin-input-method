#!/usr/bin/env python3
"""Check website destinations, assets, and signed repository preservation."""
import importlib.util
from html.parser import HTMLParser
from pathlib import Path
import tempfile
import unittest
import unicodedata
from urllib.parse import urlparse, unquote

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare_website", ROOT / "scripts/prepare_website.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.ids = []
        self.links = []
        self.assets = []
        self.feed(text)

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if "id" in attrs:
            self.ids.append(attrs["id"])
        if tag == "a":
            self.links.append(attrs.get("href", ""))
        if tag in ("img", "script") and attrs.get("src"):
            self.assets.append(attrs["src"])
        if tag == "link" and attrs.get("href"):
            self.assets.append(attrs["href"])


class WebsiteTests(unittest.TestCase):
    def test_local_links_and_assets_exist(self):
        page = Page((ROOT / "website/index.html").read_text())
        self.assertEqual(len(page.ids), len(set(page.ids)))
        for href in page.links:
            self.assertTrue(href)
            if href.startswith("#") and href != "#":
                self.assertIn(href[1:], page.ids)
            elif not href.startswith("#"):
                if urlparse(href).scheme:
                    self.assertEqual(urlparse(href).scheme, "https")
                else:
                    self.assertTrue((ROOT / "website" / href).is_file(), href)
        for asset in page.assets:
            self.assertTrue((ROOT / "website" / asset).is_file(), asset)
        readme = (ROOT / "README.md").read_text()
        for link in page.links:
            if "#" in link and "github.com" in link:
                fragment = unquote(link.split("#", 1)[1])
                # Complex package-installation heading has a GitHub-generated slug.
                if fragment.startswith("2-openeuler-"):
                    continue
                def slug(line):
                    text = line.lstrip("# ").lower()
                    text = "".join(c for c in text if not unicodedata.category(c).startswith("P") or c in "-_")
                    return text.replace(" ", "-")
                self.assertTrue(any(slug(line) == fragment for line in readme.splitlines() if line.startswith("#")), link)

    def test_overlay_preserves_every_package_file_and_is_idempotent(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-") as temp:
            destination = Path(temp) / "pages"
            for file in ("apt/dists/noble/InRelease", "apt/pool/package.deb", "rpm/oe2409/x86_64/repodata/repomd.xml", "rpm/RPM-GPG-KEY-modernime"):
                path = destination / file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"signed package repository fixture\x00")
            before = overlay.repository_digest(destination)
            overlay.prepare(ROOT / "website", destination)
            overlay.prepare(ROOT / "website", destination)
            self.assertEqual(before, overlay.repository_digest(destination))
            self.assertEqual((destination / "index.html").read_bytes(), (ROOT / "website/index.html").read_bytes())

    def test_overlay_rejects_symlink_before_mutation(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-") as temp:
            destination = Path(temp) / "pages"
            destination.mkdir()
            protected = Path(temp) / "protected"
            protected.write_text("keep")
            (destination / "app.js").symlink_to(protected)
            with self.assertRaises(ValueError):
                overlay.prepare(ROOT / "website", destination)
            self.assertEqual(protected.read_text(), "keep")
            self.assertFalse((destination / "index.html").exists())

    def test_overlay_rejects_source_destination_overlap(self):
        with self.assertRaises(ValueError):
            overlay.prepare(ROOT / "website", ROOT / "website/assets")


if __name__ == "__main__":
    unittest.main()
