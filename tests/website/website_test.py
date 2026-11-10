#!/usr/bin/env python3
"""Check website destinations, assets, and signed repository preservation."""
import hashlib
import re
import xml.etree.ElementTree as ET
import struct
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
        if tag == "link" and attrs.get("href") and attrs.get("rel") in ("icon", "stylesheet"):
            self.assets.append(attrs["href"])


class WebsiteTests(unittest.TestCase):
    def test_tag_bound_release_owns_automatic_deployment(self):
        spec = importlib.util.spec_from_file_location(
            "website_deployment", ROOT / "scripts/resolve_website_deployment.py")
        policy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(policy)
        tag = "refs/tags/v0.1.17"
        source = "a" * 40
        annotated = "b" * 40 + "\t" + tag + "\n" + source + "\t" + tag + "^{}\n"
        def refs(tags, main=source):
            return tags + "\n" + main + "\trefs/heads/main"
        self.assertFalse(policy.should_deploy("push", source, tag, refs(annotated)))
        self.assertFalse(policy.should_deploy("push", source, tag, refs(source + "\t" + tag)))
        self.assertTrue(policy.should_deploy("workflow_dispatch", source, tag, ""))
        self.assertTrue(policy.should_deploy("push", "c" * 40, tag, refs(annotated, "c" * 40)))
        self.assertFalse(policy.should_deploy("push", source, tag, refs("")))
        self.assertFalse(policy.should_deploy("push", source, tag, refs(source + "\trefs/tags/v0.1.14")))
        self.assertFalse(policy.should_deploy("push", "c" * 40, tag, refs(annotated)),
                         "superseded website commits must not publish an older homepage")
        workflow = (ROOT / ".github/workflows/website.yml").read_text()
        self.assertEqual(workflow.count("ref: " + chr(36) + "{{ github.sha }}"), 2,
                         "ownership and publication must use the same immutable triggering SHA")
        self.assertNotIn("    paths:", workflow,
                         "every newer main tip must schedule a replacement for superseded runs")
        self.assertEqual(workflow.count("python3 scripts/resolve_website_deployment.py"), 2,
                         "recheck ownership after acquiring the shared publishing lock")

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
                self.assertTrue(fragment in Page(readme).ids or any(slug(line) == fragment for line in readme.splitlines() if line.startswith("#")), link)

    def test_high_resolution_screenshots_and_publish_metadata(self):
        html = (ROOT / "website/index.html").read_text()
        base = "https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/"
        self.assertIn(f'rel="canonical" href="{base}"', html)
        self.assertIn('name="twitter:card" content="summary_large_image"', html)
        self.assertIn(base + "sitemap.xml", (ROOT / "website/robots.txt").read_text())
        tree = ET.fromstring((ROOT / "website/sitemap.xml").read_text())
        self.assertEqual(tree.find("{*}url/{*}loc").text, base)
        digests = []
        themes = re.findall(r'data-client-theme="([a-z]+)"', html)
        self.assertEqual(set(themes), {"blue", "jade", "amber", "rose", "lavender", "night", "contrast"})
        for name in ("candidates", "input", "learning"):
            for theme in themes:
                with self.subTest(page=name, theme=theme):
                    data = (ROOT / f"website/assets/client-v021-{name}-{theme}.png").read_bytes()
                    self.assertEqual(data[:8], bytes.fromhex("89504e470d0a1a0a"))
                    self.assertEqual(struct.unpack(">II", data[16:24]), (2940, 2160))
                    digests.append(hashlib.sha256(data).hexdigest())
                    data = (ROOT / f"website/assets/client-v021-{name}-{theme}.webp").read_bytes()
                    self.assertEqual(data[8:16], b"WEBPVP8L")
        self.assertEqual(len(set(digests)), 21)
        self.assertIn("/releases/latest", html)

    def test_overlay_preserves_every_package_file_and_is_idempotent(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-") as temp:
            destination = Path(temp) / "pages"
            for file in ("apt/dists/noble/InRelease", "apt/pool/package.deb", "rpm/oe2409/x86_64/repodata/repomd.xml", "rpm/RPM-GPG-KEY-modernime"):
                path = destination / file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"signed package repository fixture\x00")
            before = overlay.repository_digest(destination)
            overlay.prepare(ROOT / "website", destination)
            page_before = (destination / "index.html").read_bytes()
            overlay.prepare(ROOT / "website", destination)
            self.assertEqual((destination / "index.html").read_bytes(), page_before)
            self.assertEqual(before, overlay.repository_digest(destination))
            self.assertTrue((destination / "index.html").is_file())

    def test_deployed_script_and_style_urls_change_with_content(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-cache-") as temp:
            destination = Path(temp) / "pages"
            overlay.prepare(ROOT / "website", destination)
            html = (destination / "index.html").read_text()
            for name in ("app.js", "studio.js", "interactions.js", "styles.css"):
                digest = hashlib.sha256((ROOT / "website" / name).read_bytes()).hexdigest()[:12]
                self.assertIn(f'{name}?v={digest}', html)

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
