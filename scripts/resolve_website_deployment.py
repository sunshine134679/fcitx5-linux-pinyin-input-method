#!/usr/bin/env python3
"""Let the release workflow own automatic deployment of tag-bound commits."""
import argparse
from pathlib import Path
import re
import subprocess


def should_deploy(event, source_sha, tag_ref, remote_refs):
    if event == "workflow_dispatch":
        return True
    if event != "push":
        raise ValueError(f"Unsupported website event: {event}")
    refs = {}
    for line in remote_refs.splitlines():
        fields = line.split()
        if len(fields) == 2:
            refs[fields[1]] = fields[0]
    if refs.get("refs/heads/main") != source_sha:
        return False
    target = refs.get(tag_ref + "^{}", refs.get(tag_ref))
    return target is not None and target != source_sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True, choices=("push", "workflow_dispatch"))
    parser.add_argument("--repo-dir", type=Path, default=Path("."))
    parser.add_argument("--github-output", type=Path, required=True)
    args = parser.parse_args()
    source = subprocess.check_output(
        ["git", "-C", str(args.repo_dir), "rev-parse", "HEAD"], text=True).strip()
    cmake = (args.repo_dir / "CMakeLists.txt").read_text()
    version = re.search(
        r"^project\(ModernIME VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$",
        cmake, re.MULTILINE)
    if not version:
        raise ValueError("Missing authoritative ModernIME version")
    tag = "refs/tags/v" + version[1]
    refs = ""
    if args.event == "push":
        refs = subprocess.check_output(
            ["git", "-C", str(args.repo_dir), "ls-remote", "origin", "refs/heads/main", tag, tag + "^{}"],
            text=True)
    deploy = should_deploy(args.event, source, tag, refs)
    with args.github_output.open("a") as output:
        output.write(f"deploy={'true' if deploy else 'false'}\n")
    print("Website deployment enabled" if deploy else
          "Unreleased, tag-bound or superseded product commit: Release owns the combined website and repositories")


if __name__ == "__main__":
    main()
