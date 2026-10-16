#!/usr/bin/env python3
"""
ModernIME Required Tests Verification Tool

Enforces test registration, execution, and quality gates across build profiles
(full, storage-tsan) and stages (baseline, final).
"""

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET
from typing import Dict, List, Optional, Set, Tuple

FULL_BASELINE_REQUIRED = [
    "modernime_personal_engine_gtest",
    "modernime_personal_engine_benchmark",
    "modernime_learning_store",
    "modernime_pinyin_match",
    "modernime_pinyin_provider",
    "modernime_pinyin_quality",
    "modernime_english_arbitration",
    "modernime_pinyin_daily_simulation",
    "modernime_pinyin_engine",
    "modernime_engine_state",
    "modernime_fcitx5_input_panel",
    "modernime_fcitx5_candidate_list",
    "modernime_candidate_bar_layout",
    "modernime_candidate_bar_renderer",
    "modernime_update_controller",
    "modernime_runtime_controller",
    "modernime_data_controller",
    "modernime_settings_focus_gtk",
    "modernime_settings_shell_gtk",
    "modernime_settings_save_gtk",
    "modernime_install_runtime",
    "modernime_install_environment",
    "modernime_settings_install",
    "modernime_session_init",
    "modernime_install_transaction",
    "modernime_install_channel_collision",
    "modernime_verify_release_install_upgrade",
    "modernime_upgrade_data_protection",
    "modernime_verify_package_repo_gate",
]

FULL_FINAL_ADDITIONAL = [
    "modernime_required_tests_contract",
    "modernime_cleanse_guard",
    "modernime_configuration_contract",
    "modernime_process_runner",
    "modernime_settings_data_async_gtk",
]

STORAGE_TSAN_REQUIRED = [
    "modernime_personal_engine_gtest",
    "modernime_learning_store",
]

HEADLESS_SKIP_WHITELIST = {
    "modernime_settings_focus_gtk",
    "modernime_settings_shell_gtk",
    "modernime_settings_save_gtk",
    "modernime_settings_data_async_gtk",
}

FINAL_REQUIRED_GTEST_SUITES = [
    "ArchitectureWriterRegression",
    "ArchitectureLmdbRegression",
    "ArchitectureSelectionRegression",
    "ArchitectureLearningOrderRegression",
    "ArchitectureCandidateIdentityRegression",
    "ArchitecturePersonalFileRegression",
]


def load_registered_tests(registered_path: str) -> Set[str]:
    with open(registered_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    tests = set()
    for item in data.get("tests", []):
        name = item.get("name")
        if name:
            tests.add(name)
    return tests


def parse_junit_file(junit_path: str) -> Dict[str, str]:
    """Returns a dict of test_name -> status ('passed', 'failed', 'skipped')"""
    results: Dict[str, str] = {}
    if not os.path.exists(junit_path):
        raise FileNotFoundError(f"JUnit file not found: {junit_path}")
    tree = ET.parse(junit_path)
    root = tree.getroot()

    # Search for all testcase elements anywhere in the tree
    for testcase in root.iter("testcase"):
        name = testcase.get("name")
        if not name:
            continue
        status = "passed"
        has_failure = testcase.find("failure") is not None or testcase.find("error") is not None
        has_skipped = testcase.find("skipped") is not None or testcase.get("status") == "skipped"

        if has_failure:
            status = "failed"
        elif has_skipped:
            status = "skipped"
        results[name] = status

    return results


def parse_gtest_xml_dir(gtest_dir: str) -> Tuple[int, int, int, Set[str]]:
    """Returns (total_tests, total_failures, total_errors, found_suites_or_cases)"""
    if not os.path.isdir(gtest_dir):
        raise NotADirectoryError(f"GTest XML directory not found: {gtest_dir}")

    total_tests = 0
    total_failures = 0
    total_errors = 0
    found_symbols: Set[str] = set()

    for entry in os.listdir(gtest_dir):
        if not entry.endswith(".xml"):
            continue
        full_path = os.path.join(gtest_dir, entry)
        try:
            tree = ET.parse(full_path)
            root = tree.getroot()
        except Exception as e:
            raise RuntimeError(f"Failed to parse GTest XML {full_path}: {e}")

        # GTest XML typically has root <testsuites> or <testsuite>
        if root.tag == "testsuites":
            total_tests += int(root.get("tests", 0))
            total_failures += int(root.get("failures", 0))
            total_errors += int(root.get("errors", 0))
        elif root.tag == "testsuite":
            total_tests += int(root.get("tests", 0))
            total_failures += int(root.get("failures", 0))
            total_errors += int(root.get("errors", 0))

        for suite in root.iter("testsuite"):
            suite_name = suite.get("name")
            if suite_name:
                found_symbols.add(suite_name)
        for case in root.iter("testcase"):
            case_name = case.get("name")
            classname = case.get("classname")
            if case_name:
                found_symbols.add(case_name)
            if classname:
                found_symbols.add(classname)

    return total_tests, total_failures, total_errors, found_symbols


def verify(
    registered_path: str,
    profile: str,
    stage: str = "baseline",
    junit_paths: Optional[List[str]] = None,
    gtest_dir: Optional[str] = None,
    execution: str = "headless",
) -> Tuple[bool, List[str]]:
    errors: List[str] = []

    # 1. Determine required test names
    if profile == "full":
        required = list(FULL_BASELINE_REQUIRED)
        if stage == "final":
            required.extend(FULL_FINAL_ADDITIONAL)
    elif profile == "storage-tsan":
        required = list(STORAGE_TSAN_REQUIRED)
    else:
        errors.append(f"Unknown profile: {profile}")
        return False, errors

    # 2. Check registration in show-only=json-v1
    try:
        registered = load_registered_tests(registered_path)
    except Exception as e:
        errors.append(f"Failed to load registered tests from {registered_path}: {e}")
        return False, errors

    missing_registered = [t for t in required if t not in registered]
    if missing_registered:
        errors.append(
            f"Missing required tests in registration ({profile}/{stage}): {', '.join(missing_registered)}"
        )

    # 3. Check execution evidence if JUnit or GTest XML provided
    has_execution_input = bool(junit_paths or gtest_dir)
    if has_execution_input:
        combined_junit: Dict[str, str] = {}
        if junit_paths:
            for j_path in junit_paths:
                try:
                    res = parse_junit_file(j_path)
                    for test_name, status in res.items():
                        # If a test already has failure recorded, keep failure
                        if test_name in combined_junit and combined_junit[test_name] == "failed":
                            continue
                        combined_junit[test_name] = status
                except Exception as e:
                    errors.append(f"Error reading JUnit XML {j_path}: {e}")

        # Check that all required tests have execution records
        unexecuted_required = [t for t in required if t not in combined_junit]
        if unexecuted_required:
            errors.append(
                f"Required tests missing execution evidence in JUnit: {', '.join(unexecuted_required)}"
            )

        # Check failures and skips
        for test_name in required:
            status = combined_junit.get(test_name)
            if not status:
                continue
            if status == "failed":
                errors.append(f"Test FAILED: {test_name}")
            elif status == "skipped":
                if execution == "headless" and test_name in HEADLESS_SKIP_WHITELIST:
                    # Allowed in headless mode
                    pass
                else:
                    errors.append(
                        f"Illegal test SKIP for {test_name} (execution={execution}, whitelisted={test_name in HEADLESS_SKIP_WHITELIST})"
                    )

        # Check GTest XML if provided
        if gtest_dir:
            try:
                tests_count, failures, gtest_errors, symbols = parse_gtest_xml_dir(gtest_dir)
                if tests_count == 0:
                    errors.append(f"GTest XML directory {gtest_dir} contains 0 executed tests.")
                if failures > 0:
                    errors.append(f"GTest XML reports {failures} test failures.")
                if gtest_errors > 0:
                    errors.append(f"GTest XML reports {gtest_errors} test errors.")
                if stage == "final":
                    missing_suites = [s for s in FINAL_REQUIRED_GTEST_SUITES if s not in symbols]
                    if missing_suites:
                        errors.append(
                            f"Missing required GTest regression suites in final stage: {', '.join(missing_suites)}"
                        )
            except Exception as e:
                errors.append(f"Error evaluating GTest XML directory {gtest_dir}: {e}")

    return len(errors) == 0, errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify ModernIME required tests registration and execution."
    )
    parser.add_argument(
        "--registered", required=True, help="Path to ctest json output (from --show-only=json-v1)"
    )
    parser.add_argument(
        "--profile", required=True, choices=["full", "storage-tsan"], help="Verification profile"
    )
    parser.add_argument(
        "--stage", choices=["baseline", "final"], default="baseline", help="Verification stage"
    )
    parser.add_argument(
        "--junit", action="append", default=[], help="Path to JUnit XML file (repeatable)"
    )
    parser.add_argument(
        "--gtest-xml", help="Directory containing Google Test XML output files"
    )
    parser.add_argument(
        "--execution", choices=["headless", "desktop"], default="headless", help="Execution mode"
    )

    args = parser.parse_args()

    ok, errors = verify(
        registered_path=args.registered,
        profile=args.profile,
        stage=args.stage,
        junit_paths=args.junit if args.junit else None,
        gtest_dir=args.gtest_xml,
        execution=args.execution,
    )

    if ok:
        print(f"[OK] Required tests check passed (profile={args.profile}, stage={args.stage}, execution={args.execution}).")
        return 0
    else:
        print(f"[FAIL] Required tests check failed with {len(errors)} error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
