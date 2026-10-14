#!/usr/bin/env python3
"""Overlay the official website without changing signed package repositories."""
import argparse
import hashlib
from pathlib import Path
import shutil

FILES = ("index.html", "styles.css", "app.js")


def repository_digest(root):
    result = {}
    for name in ("apt", "rpm"):
        folder = root / name
        if folder.is_symlink():
            raise ValueError(f"Refusing symlink repository directory: {folder}")
        if folder.exists():
            for entry in sorted(folder.rglob("*")):
                if entry.is_symlink():
                    raise ValueError(f"Refusing symlink repository entry: {entry}")
                if entry.is_file():
                    result[str(entry.relative_to(root))] = hashlib.sha256(entry.read_bytes()).hexdigest()
    return result


def prepare(source, destination):
    source = Path(source).resolve(strict=True)
    destination = Path(destination)
    if destination.is_symlink():
        raise ValueError("Refusing symlink Pages directory")
    destination = destination.resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError("Website source and Pages destination must not overlap")
    for name in FILES + ("assets",):
        entry = source / name
        if not entry.exists() or entry.is_symlink():
            raise ValueError(f"Missing or unsafe website source: {entry}")
    if not (source / "assets").is_dir():
        raise ValueError("Website assets must be a directory")
    for entry in (source / "assets").rglob("*"):
        if entry.is_symlink():
            raise ValueError(f"Refusing symlink website asset: {entry}")
    # Fail before copying any files when the destination is unsafe.
    for name in FILES + ("assets", ".nojekyll"):
        target = destination / name
        if target.is_symlink() or (target.exists() and target.is_dir() != (name == "assets")):
            raise ValueError(f"Unsafe existing website path: {target}")
    assets = destination / "assets"
    if assets.exists() and any(entry.is_symlink() for entry in assets.rglob("*")):
        raise ValueError("Refusing symlinks in destination website assets")
    before = repository_digest(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        shutil.copyfile(source / name, destination / name)
    shutil.copytree(source / "assets", destination / "assets", dirs_exist_ok=True)
    (destination / ".nojekyll").touch()
    if before != repository_digest(destination):
        raise RuntimeError("Package repository changed while preparing the website")
    print(f"Official website ready: {destination}; {len(before)} package repository files unchanged")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parent.parent / "website")
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.destination)
