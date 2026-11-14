#!/usr/bin/env python3
"""Check website destinations, assets, and signed repository preservation."""
import hashlib
import json
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


PAGE_ASSETS = {
    "index.html": ("styles.css", "app.js", "interactions.js"),
    "appearance.html": ("styles.css", "app.js", "interactions.js", "studio.js"),
    "download.html": ("styles.css", "support.css", "app.js", "interactions.js", "download.js"),
    "help.html": ("styles.css", "support.css", "app.js", "interactions.js"),
}


def make_website_fixture(source):
    source.mkdir()
    (source / "assets").mkdir()
    (source / "assets/icon.png").write_bytes(b"website asset")
    for page, assets in PAGE_ASSETS.items():
        html = '<!doctype html><html><head>'
        for asset in assets:
            if asset.endswith(".css"):
                html += f'<link rel="stylesheet" href="{asset}">'
            else:
                html += f'<script src="{asset}" defer></script>'
            (source / asset).write_text(f"/* {asset} initial content */")
        html += f'</head><body><h1 id="main">{page}</h1></body></html>'
        (source / page).write_text(html)
    (source / "sitemap.xml").write_text("<urlset/>")
    (source / "robots.txt").write_text("User-agent: *\nAllow: /\n")


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
        website = ROOT / "website"
        pages = {path.resolve(): Page(path.read_text()) for path in website.glob("*.html")}
        self.assertEqual({path.name for path in pages}, set(PAGE_ASSETS))
        readme = (ROOT / "README.md").read_text()
        readme_ids = Page(readme).ids
        def slug(line):
            text = line.lstrip("# ").lower()
            text = "".join(c for c in text if not unicodedata.category(c).startswith("P") or c in "-_")
            return text.replace(" ", "-")
        readme_slugs = {slug(line) for line in readme.splitlines() if line.startswith("#")}
        repository_path = "/sunshine134679/fcitx5-linux-pinyin-input-method"
        readme_paths = {repository_path, repository_path + "/blob/main/README.md"}
        for path, page in pages.items():
            with self.subTest(page=path.name):
                self.assertEqual(len(page.ids), len(set(page.ids)))
                for href in page.links + page.assets:
                    self.assertTrue(href)
                    url = urlparse(href)
                    if url.scheme or url.netloc:
                        self.assertEqual(url.scheme, "https", href)
                        # Repository-root and README links use README headings;
                        # other documentation owns its own anchors.
                        if (url.hostname == "github.com" and
                                url.path.rstrip("/") in readme_paths and url.fragment):
                            fragment = unquote(url.fragment)
                            if not fragment.startswith("2-openeuler-"):
                                self.assertTrue(fragment in readme_ids or fragment in readme_slugs, href)
                        continue
                    target = (path.parent / unquote(url.path)).resolve() if url.path else path
                    if target.is_dir():
                        target /= "index.html"
                    self.assertTrue(target.is_file(), f"{path.name}: {href}")
                    if url.fragment and target.suffix == ".html":
                        self.assertIn(target, pages, href)
                        self.assertIn(unquote(url.fragment), pages[target].ids, f"{path.name}: {href}")

    def test_high_resolution_screenshots_and_publish_metadata(self):
        base = "https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/"
        for page in PAGE_ASSETS:
            html = (ROOT / "website" / page).read_text()
            canonical = base if page == "index.html" else base + page
            self.assertIn(f'rel="canonical" href="{canonical}"', html, page)
            card = "summary_large_image" if page in ("index.html", "appearance.html") else "summary"
            self.assertIn(f'name="twitter:card" content="{card}"', html, page)
        self.assertIn(base + "sitemap.xml", (ROOT / "website/robots.txt").read_text())
        tree = ET.fromstring((ROOT / "website/sitemap.xml").read_text())
        self.assertEqual({entry.text for entry in tree.findall("{*}url/{*}loc")},
                         {base, base + "appearance.html", base + "download.html", base + "help.html"})
        html = (ROOT / "website/index.html").read_text()
        digests = []
        themes = re.findall(r'data-client-theme="([a-z]+)"', html)
        self.assertEqual(set(themes), {"blue", "jade", "amber", "rose", "lavender", "night", "contrast"})
        self.assertEqual(len(themes), 7)
        provenance = json.loads((ROOT / "website/assets/客户端实拍来源.json").read_text())
        self.assertEqual(provenance["客户端版本"], "0.2.13")
        self.assertEqual(provenance["原生尺寸"], [2940, 2160])
        captures = {capture["文件"]: capture for capture in provenance["截图"]}
        expected = {f"client-v0213-{name}-{theme}.png"
                    for name in ("candidates", "input", "learning") for theme in themes}
        self.assertEqual(set(captures), expected)
        self.assertEqual(len(provenance["截图"]), 21)
        gallery_images = [asset for asset in Page(html).assets if "/client-" in asset]
        self.assertTrue(gallery_images)
        for asset in gallery_images:
            self.assertIn(Path(asset).with_suffix(".png").name, expected)
        for name in ("candidates", "input", "learning"):
            for theme in themes:
                with self.subTest(page=name, theme=theme):
                    filename = f"client-v0213-{name}-{theme}.png"
                    capture = captures[filename]
                    data = (ROOT / "website/assets" / filename).read_bytes()
                    self.assertEqual(data[:8], bytes.fromhex("89504e470d0a1a0a"))
                    self.assertEqual(struct.unpack(">II", data[16:24]), (2940, 2160))
                    digest = hashlib.sha256(data).hexdigest()
                    self.assertEqual(capture["SHA256"], digest)
                    self.assertEqual((capture["宽"], capture["高"]), (2940, 2160))
                    self.assertEqual((capture["页面"], capture["主题"]), (name, theme))
                    digests.append(digest)
                    webp = f"client-v0213-{name}-{theme}.webp"
                    data = (ROOT / "website/assets" / webp).read_bytes()
                    self.assertEqual(data[8:16], b"WEBPVP8L")
                    self.assertEqual(capture["WebP文件"], webp)
                    self.assertEqual(capture["WebP_SHA256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(len(set(digests)), 21)
        self.assertIn("/releases/latest", (ROOT / "website/download.html").read_text())

    def test_overlay_preserves_every_package_file_and_is_idempotent(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-") as temp:
            source = Path(temp) / "source"
            destination = Path(temp) / "pages"
            make_website_fixture(source)
            for file in ("apt/dists/noble/InRelease", "apt/pool/package.deb", "rpm/oe2409/x86_64/repodata/repomd.xml", "rpm/RPM-GPG-KEY-modernime"):
                path = destination / file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"signed package repository fixture\x00")
            before = overlay.repository_digest(destination)
            overlay.prepare(source, destination)
            pages_before = {name: (destination / name).read_bytes() for name in PAGE_ASSETS}
            overlay.prepare(source, destination)
            self.assertEqual({name: (destination / name).read_bytes() for name in PAGE_ASSETS}, pages_before)
            self.assertEqual(before, overlay.repository_digest(destination))
            self.assertTrue((destination / "index.html").is_file())

    def test_overlay_publishes_all_four_pages_and_their_assets(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-pages-") as temp:
            source = Path(temp) / "source"
            destination = Path(temp) / "pages"
            make_website_fixture(source)
            overlay.prepare(source, destination)
            for name in PAGE_ASSETS:
                self.assertTrue((destination / name).is_file(), name)
            for name in {asset for assets in PAGE_ASSETS.values() for asset in assets}:
                self.assertEqual((destination / name).read_bytes(), (source / name).read_bytes(), name)
            self.assertTrue((destination / ".nojekyll").is_file())

    def test_deployed_script_and_style_urls_change_with_content(self):
        with tempfile.TemporaryDirectory(prefix="modernime-website-cache-") as temp:
            source = Path(temp) / "source"
            destination = Path(temp) / "pages"
            make_website_fixture(source)
            previous = {}
            for revision in ("initial", "updated"):
                if revision == "updated":
                    for name in ("styles.css", "download.js"):
                        (source / name).write_text(f"/* {name} updated content */")
                overlay.prepare(source, destination)
                for page, assets in PAGE_ASSETS.items():
                    self.assertTrue((destination / page).is_file(), page)
                    html = (destination / page).read_text()
                    for name in assets:
                        digest = hashlib.sha256((source / name).read_bytes()).hexdigest()[:12]
                        self.assertIn(f'{name}?v={digest}', html, f"{page}: {name}")
                        if revision == "updated" and name in ("styles.css", "download.js"):
                            self.assertNotIn(f'{name}?v={previous[name]}', html)
                        if revision == "initial":
                            previous[name] = digest

    def test_overlay_rejects_symlink_before_mutation(self):
        for name in ("app.js", "appearance.html"):
            with self.subTest(path=name), tempfile.TemporaryDirectory(prefix="modernime-website-") as temp:
                source = Path(temp) / "source"
                destination = Path(temp) / "pages"
                make_website_fixture(source)
                destination.mkdir()
                protected = Path(temp) / "protected"
                protected.write_text("keep")
                (destination / name).symlink_to(protected)
                with self.assertRaisesRegex(ValueError, "Unsafe existing website path"):
                    overlay.prepare(source, destination)
                self.assertEqual(protected.read_text(), "keep")
                self.assertFalse((destination / "index.html").exists())

    def test_overlay_rejects_source_destination_overlap(self):
        with self.assertRaises(ValueError):
            overlay.prepare(ROOT / "website", ROOT / "website/assets")


if __name__ == "__main__":
    unittest.main()
