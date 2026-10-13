#!/usr/bin/env bash
# resolve_release_target.sh — Resolves and validates the immutable git tag and product source commit SHA
# for ModernIME release builds (both push.tags and workflow_dispatch).
#
# Enforces:
# 1. workflow_dispatch MUST specify an existing vX.Y.Z tag_name (never falls back to building main HEAD).
# 2. push event MUST be a tag ref (refs/tags/vX.Y.Z).
# 3. The tag MUST exist in refs/tags/<tag> and resolve via refs/tags/<tag>^{commit} to a 40-hex commit SHA.
# 4. CMakeLists.txt at that exact commit SHA MUST declare project(ModernIME VERSION X.Y.Z LANGUAGES CXX)
#    matching the tag version.
# 5. Optional --assert-checkout-sha Verify mode ensures a build job's checked-out HEAD matches the bound SHA.
set -euo pipefail

event_name="${GITHUB_EVENT_NAME:-}"
ref_type="${GITHUB_REF_TYPE:-}"
ref_name="${GITHUB_REF_NAME:-}"
input_tag=""
repo_dir="."
github_output="${GITHUB_OUTPUT:-}"
assert_checkout_sha=""
assert_version=""

usage() {
    cat <<'EOF'
Usage:
  resolve_release_target.sh [--event-name EVENT] [--ref-type TYPE] [--ref-name NAME]
                            [--input-tag TAG] [--repo-dir DIR] [--github-output FILE]
  resolve_release_target.sh --assert-checkout-sha SHA --assert-version VER [--repo-dir DIR]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --event-name=*)
            event_name="${1#*=}"
            shift
            ;;
        --event-name)
            event_name="$2"
            shift 2
            ;;
        --ref-type=*)
            ref_type="${1#*=}"
            shift
            ;;
        --ref-type)
            ref_type="$2"
            shift 2
            ;;
        --ref-name=*)
            ref_name="${1#*=}"
            shift
            ;;
        --ref-name)
            ref_name="$2"
            shift 2
            ;;
        --input-tag=*)
            input_tag="${1#*=}"
            shift
            ;;
        --input-tag)
            input_tag="$2"
            shift 2
            ;;
        --repo-dir=*)
            repo_dir="${1#*=}"
            shift
            ;;
        --repo-dir)
            repo_dir="$2"
            shift 2
            ;;
        --github-output=*)
            github_output="${1#*=}"
            shift
            ;;
        --github-output)
            github_output="$2"
            shift 2
            ;;
        --assert-checkout-sha=*)
            assert_checkout_sha="${1#*=}"
            shift
            ;;
        --assert-checkout-sha)
            assert_checkout_sha="$2"
            shift 2
            ;;
        --assert-version=*)
            assert_version="${1#*=}"
            shift
            ;;
        --assert-version)
            assert_version="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Error: Unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -n "$assert_checkout_sha" ]]; then
    actual_head=$(git -C "$repo_dir" rev-parse --verify HEAD 2>/dev/null || true)
    if [[ -z "$actual_head" || "$actual_head" != "$assert_checkout_sha" ]]; then
        printf 'Error: Checked-out HEAD (%s) does not match bound product source SHA (%s). Refusing to build from unbound commit.\n' \
            "${actual_head:-<none>}" "$assert_checkout_sha" >&2
        exit 1
    fi
    if [[ -n "$assert_version" ]]; then
        cmake_file="$repo_dir/CMakeLists.txt"
        if [[ ! -f "$cmake_file" ]]; then
            printf 'Error: CMakeLists.txt not found in %s\n' "$repo_dir" >&2
            exit 1
        fi
        actual_ver=$(sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p' "$cmake_file")
        if [[ "$actual_ver" != "$assert_version" ]]; then
            printf 'Error: Checked-out CMakeLists.txt version (%s) does not match expected release version (%s).\n' \
                "${actual_ver:-<none>}" "$assert_version" >&2
            exit 1
        fi
    fi
    printf 'Verified bound product source checkout: SHA=%s VERSION=%s\n' "$actual_head" "${assert_version:-verified}"
    exit 0
fi

resolved_tag=""
case "$event_name" in
    workflow_dispatch)
        if [[ -z "$input_tag" ]]; then
            printf 'Error: workflow_dispatch requires an explicit existing tag_name input (e.g. v0.1.9). Building directly from branch HEAD is forbidden.\n' >&2
            exit 1
        fi
        resolved_tag="$input_tag"
        ;;
    push)
        if [[ "$ref_type" != "tag" || -z "$ref_name" ]]; then
            printf 'Error: push release trigger requires ref_type=tag and non-empty ref_name (got ref_type=%s, ref_name=%s).\n' \
                "${ref_type:-<empty>}" "${ref_name:-<empty>}" >&2
            exit 1
        fi
        resolved_tag="$ref_name"
        ;;
    *)
        printf 'Error: Unsupported release event_name=%s (expected workflow_dispatch or push).\n' "${event_name:-<empty>}" >&2
        exit 1
        ;;
esac

if [[ ! "$resolved_tag" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
    printf 'Error: Invalid release tag format "%s" (expected vX.Y.Z, e.g. v0.1.9).\n' "$resolved_tag" >&2
    exit 1
fi
expected_version="${BASH_REMATCH[1]}"

if ! git -C "$repo_dir" show-ref --tags --verify --quiet "refs/tags/${resolved_tag}"; then
    printf 'Error: Release tag "%s" (refs/tags/%s) does not exist in repository.\n' "$resolved_tag" "$resolved_tag" >&2
    exit 1
fi

source_sha=$(git -C "$repo_dir" rev-parse --verify "refs/tags/${resolved_tag}^{commit}" 2>/dev/null || true)
if [[ ! "$source_sha" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'Error: Failed to resolve full 40-char commit SHA for tag "%s" (got "%s").\n' "$resolved_tag" "$source_sha" >&2
    exit 1
fi

cmake_content=$(git -C "$repo_dir" show "${source_sha}:CMakeLists.txt" 2>/dev/null || true)
if [[ -z "$cmake_content" ]]; then
    printf 'Error: Unable to read CMakeLists.txt at commit %s (tag %s).\n' "$source_sha" "$resolved_tag" >&2
    exit 1
fi

cmake_version=$(printf '%s\n' "$cmake_content" | sed -n 's/^project(ModernIME VERSION \([0-9.][0-9.]*\) LANGUAGES CXX)$/\1/p')
if [[ -z "$cmake_version" ]]; then
    printf 'Error: Unable to parse project(ModernIME VERSION ...) from CMakeLists.txt at commit %s.\n' "$source_sha" >&2
    exit 1
fi

if [[ "$cmake_version" != "$expected_version" ]]; then
    printf 'Error: Tag/version mismatch! Tag "%s" expects version "%s", but CMakeLists.txt at commit %s declares version "%s".\n' \
        "$resolved_tag" "$expected_version" "$source_sha" "$cmake_version" >&2
    exit 1
fi

printf 'Resolved Release Tag: %s\n' "$resolved_tag"
printf 'Resolved Release Version: %s\n' "$expected_version"
printf 'Resolved Product Source SHA: %s\n' "$source_sha"

if [[ -n "$github_output" ]]; then
    {
        printf 'tag=%s\n' "$resolved_tag"
        printf 'version=%s\n' "$expected_version"
        printf 'source_sha=%s\n' "$source_sha"
    } >> "$github_output"
fi
