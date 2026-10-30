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
from collections import defaultdict
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
    "modernime_ui_addon",
    "modernime_adapter_recovery",
    "modernime_settings_navigation_dbus",
    "modernime_runtime_controller_concurrency",
]

STORAGE_TSAN_REQUIRED = [
    "modernime_personal_engine_gtest",
    "modernime_learning_store",
]

CORE_ONLY_BASELINE_REQUIRED = [
    "modernime_settings",
    "modernime_clipboard_history",
    "modernime_persistent_clipboard_history",
    "modernime_dictionary_prior",
    "modernime_learning_store",
    "modernime_input_state",
    "modernime_english_dictionary",
    "modernime_english_definition",
    "modernime_pinyin_match",
    "modernime_punctuation",
    "modernime_engine_state",
    "modernime_personal_engine_benchmark",
]

CORE_ONLY_FINAL_ADDITIONAL = [
    "modernime_required_tests_contract",
]

HEADLESS_SKIP_WHITELIST = {
    "modernime_settings_focus_gtk",
    "modernime_settings_shell_gtk",
    "modernime_settings_save_gtk",
    "modernime_settings_data_async_gtk",
    "modernime_ui_addon",
    "modernime_settings_navigation_dbus",
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
    if os.path.getsize(junit_path) == 0:
        raise ValueError(f"JUnit file is empty (0 bytes): {junit_path}")
    tree = ET.parse(junit_path)
    root = tree.getroot()

    testcase_count = 0
    # Search for all testcase elements anywhere in the tree
    for testcase in root.iter("testcase"):
        testcase_count += 1
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

        # Fail-closed: if duplicate test name exists, failure always dominates pass/skip
        if name in results and results[name] == "failed":
            pass
        else:
            results[name] = status

    if testcase_count == 0:
        raise ValueError(f"JUnit file contains 0 testcases: {junit_path}")

    return results


def parse_gtest_xml_dir(
    gtest_dir: str,
) -> Tuple[int, int, int, Dict[str, int], Set[str]]:
    """Returns (total_executed, total_failures, total_errors, executed_by_suite, found_symbols)"""
    if not os.path.isdir(gtest_dir):
        raise NotADirectoryError(f"GTest XML directory not found: {gtest_dir}")

    total_executed = 0
    total_failures = 0
    total_errors = 0
    found_symbols: Set[str] = set()
    executed_by_suite: Dict[str, int] = defaultdict(int)
    xml_files_count = 0

    for entry in os.listdir(gtest_dir):
        if not entry.endswith(".xml"):
            continue
        full_path = os.path.join(gtest_dir, entry)
        if os.path.getsize(full_path) == 0:
            raise ValueError(f"GTest XML file is empty (0 bytes): {full_path}")
        xml_files_count += 1
        try:
            tree = ET.parse(full_path)
            root = tree.getroot()
        except Exception as e:
            raise RuntimeError(f"Failed to parse GTest XML {full_path}: {e}")

        for suite in root.iter("testsuite"):
            suite_name = suite.get("name")
            if suite_name:
                found_symbols.add(suite_name)

        for case in root.iter("testcase"):
            case_name = case.get("name", "")
            classname = case.get("classname", "")
            status = case.get("status", "")
            result = case.get("result", "")

            if case_name:
                found_symbols.add(case_name)
            if classname:
                found_symbols.add(classname)

            has_failure = len(case.findall("failure")) > 0
            has_error = len(case.findall("error")) > 0
            has_skipped = len(case.findall("skipped")) > 0
            is_disabled = (
                case_name.startswith("DISABLED_")
                or status == "notrun"
                or result == "suppressed"
                or classname.startswith("DISABLED_")
            )

            if has_failure:
                total_failures += 1
            if has_error:
                total_errors += 1

            if not is_disabled and not has_skipped and status == "run":
                total_executed += 1
                if classname:
                    executed_by_suite[classname] += 1
                    base_class = classname.split("/")[0]
                    executed_by_suite[base_class] += 1

    if xml_files_count == 0:
        return 0, 0, 0, {}, set()

    return total_executed, total_failures, total_errors, executed_by_suite, found_symbols


def verify(
    registered_path: str,
    profile: str,
    stage: str = "baseline",
    check_mode: Optional[str] = None,
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
    elif profile in ("core-only", "core"):
        required = list(CORE_ONLY_BASELINE_REQUIRED)
        if stage == "final":
            required.extend(CORE_ONLY_FINAL_ADDITIONAL)
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

    # If explicitly registration-only check, stop here
    if check_mode == "registration":
        return len(errors) == 0, errors

    # 3. Check execution evidence
    if check_mode == "execution":
        if not junit_paths:
            errors.append("Execution verification mode requires at least one --junit XML file.")
        if profile == "full" and not gtest_dir:
            errors.append(
                "Execution verification mode under 'full' profile requires --gtest-xml directory containing native GTest reports."
            )

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

            # Check that not all tests in JUnit were skipped/disabled
            if combined_junit and all(s == "skipped" for s in combined_junit.values()):
                errors.append("All executed tests in JUnit reports were skipped/disabled (0 active runs).")

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
                executed_count, failures, gtest_errors, suite_executed_counts, symbols = (
                    parse_gtest_xml_dir(gtest_dir)
                )
                if executed_count == 0:
                    errors.append(f"GTest XML directory {gtest_dir} contains 0 executed tests.")
                if failures > 0:
                    errors.append(f"GTest XML reports {failures} test failures.")
                if gtest_errors > 0:
                    errors.append(f"GTest XML reports {gtest_errors} test errors.")
                if stage == "final":
                    for required_suite in FINAL_REQUIRED_GTEST_SUITES:
                        executed_in_suite = suite_executed_counts.get(required_suite, 0)
                        if executed_in_suite == 0:
                            if required_suite not in symbols:
                                errors.append(
                                    f"Missing required GTest regression suite in final stage: {required_suite}"
                                )
                            else:
                                errors.append(
                                    f"Required GTest regression suite {required_suite} has 0 executed testcases (all disabled or skipped)"
                                )
            except Exception as e:
                errors.append(f"Error evaluating GTest XML directory {gtest_dir}: {e}")

    return len(errors) == 0, errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify ModernIME required tests registration and execution."
    )
    parser.add_argument(
        "--check",
        choices=["registration", "execution"],
        default=None,
        help="Check mode: 'registration' (only verify ctest registration) or 'execution' (verify registration + test execution evidence)",
    )
    parser.add_argument(
        "--registered", required=True, help="Path to ctest json output (from --show-only=json-v1)"
    )
    parser.add_argument(
        "--profile",
        required=True,
        choices=["full", "storage-tsan", "core-only", "core"],
        help="Verification profile",
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
        check_mode=args.check,
        junit_paths=args.junit if args.junit else None,
        gtest_dir=args.gtest_xml,
        execution=args.execution,
    )

    if ok:
        mode_desc = f", check_mode={args.check}" if args.check else ""
        print(f"[OK] Required tests check passed (profile={args.profile}, stage={args.stage}, execution={args.execution}{mode_desc}).")
        return 0
    else:
        print(f"[FAIL] Required tests check failed with {len(errors)} error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
