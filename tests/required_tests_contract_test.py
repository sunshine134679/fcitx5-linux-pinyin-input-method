#!/usr/bin/env python3
"""
Contract test for verify_required_tests.py
Validates negative and positive cases for test registration and execution verification.
"""

import json
import os
import shutil
import tempfile
import unittest

from verify_required_tests import (
    CORE_ONLY_BASELINE_REQUIRED,
    CORE_ONLY_FINAL_ADDITIONAL,
    FULL_BASELINE_REQUIRED,
    FULL_FINAL_ADDITIONAL,
    HEADLESS_SKIP_WHITELIST,
    verify,
)


class VerifyRequiredTestsContractTest(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp(prefix="modernime_test_contract_")
        self.registered_json = os.path.join(self.test_dir, "registered.json")
        self.junit_xml = os.path.join(self.test_dir, "junit.xml")
        self.gtest_dir = os.path.join(self.test_dir, "gtest")
        os.makedirs(self.gtest_dir, exist_ok=True)

        # Create valid baseline registration JSON
        self.create_registered_json(FULL_BASELINE_REQUIRED)
        # Create valid baseline JUnit XML
        self.create_junit_xml({t: "passed" for t in FULL_BASELINE_REQUIRED})

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def create_registered_json(self, test_names):
        data = {"tests": [{"name": name} for name in test_names]}
        with open(self.registered_json, "w", encoding="utf-8") as f:
            json.dump(data, f)

    def create_junit_xml(self, test_results, file_path=None):
        target = file_path or self.junit_xml
        lines = ['<?xml version="1.0" encoding="UTF-8"?>', "<testsuites>", '  <testsuite name="ModernIME">']
        for name, status in test_results.items():
            if status == "passed":
                lines.append(f'    <testcase classname="ModernIME" name="{name}" time="0.1"/>')
            elif status == "failed":
                lines.append(f'    <testcase classname="ModernIME" name="{name}" time="0.1">')
                lines.append('      <failure message="test failed"/>')
                lines.append("    </testcase>")
            elif status == "skipped":
                lines.append(f'    <testcase classname="ModernIME" name="{name}" time="0.0">')
                lines.append('      <skipped message="test skipped"/>')
                lines.append("    </testcase>")
        lines.append("  </testsuite>")
        lines.append("</testsuites>")
        with open(target, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))

    def create_gtest_xml(self, suite_names, failures=0, errors=0):
        xml_path = os.path.join(self.gtest_dir, "test_detail.xml")
        lines = [
            '<?xml version="1.0" encoding="UTF-8"?>',
            f'<testsuites tests="{len(suite_names)}" failures="{failures}" errors="{errors}">',
        ]
        for suite in suite_names:
            lines.append(f'  <testsuite name="{suite}" tests="1" failures="0" errors="0">')
            lines.append(f'    <testcase name="BasicCheck" classname="{suite}" status="run"/>')
            lines.append("  </testsuite>")
        lines.append("</testsuites>")
        with open(xml_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))

    def test_valid_baseline_registration_and_execution_passes(self):
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
            execution="headless",
        )
        self.assertTrue(ok, f"Expected pass, got errors: {errors}")

    def test_missing_gtest_in_registration_fails(self):
        tests = [t for t in FULL_BASELINE_REQUIRED if t != "modernime_personal_engine_gtest"]
        self.create_registered_json(tests)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_personal_engine_gtest" in e for e in errors))

    def test_missing_benchmark_in_registration_fails(self):
        tests = [t for t in FULL_BASELINE_REQUIRED if t != "modernime_personal_engine_benchmark"]
        self.create_registered_json(tests)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_personal_engine_benchmark" in e for e in errors))

    def test_missing_install_test_in_registration_fails(self):
        tests = [t for t in FULL_BASELINE_REQUIRED if t != "modernime_install_runtime"]
        self.create_registered_json(tests)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_install_runtime" in e for e in errors))

    def test_junit_failure_fails(self):
        results = {t: "passed" for t in FULL_BASELINE_REQUIRED}
        results["modernime_pinyin_engine"] = "failed"
        self.create_junit_xml(results)

        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_pinyin_engine" in e for e in errors))

    def test_unknown_skip_in_headless_fails(self):
        results = {t: "passed" for t in FULL_BASELINE_REQUIRED}
        results["modernime_pinyin_match"] = "skipped"  # Not in whitelist
        self.create_junit_xml(results)

        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
            execution="headless",
        )
        self.assertFalse(ok)
        self.assertTrue(any("Illegal test SKIP for modernime_pinyin_match" in e for e in errors))

    def test_whitelisted_gtk_skip_in_headless_passes(self):
        results = {t: "passed" for t in FULL_BASELINE_REQUIRED}
        for whitelisted in HEADLESS_SKIP_WHITELIST:
            if whitelisted in results:
                results[whitelisted] = "skipped"
        self.create_junit_xml(results)

        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
            execution="headless",
        )
        self.assertTrue(ok, f"Expected pass in headless mode for whitelisted skips, got: {errors}")

    def test_whitelisted_gtk_skip_in_desktop_fails(self):
        results = {t: "passed" for t in FULL_BASELINE_REQUIRED}
        results["modernime_settings_focus_gtk"] = "skipped"
        self.create_junit_xml(results)

        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
            execution="desktop",
        )
        self.assertFalse(ok)
        self.assertTrue(any("Illegal test SKIP for modernime_settings_focus_gtk" in e for e in errors))

    def test_missing_execution_evidence_fails(self):
        # Register full, but only execute a subset
        subset = {t: "passed" for t in FULL_BASELINE_REQUIRED[:5]}
        self.create_junit_xml(subset)

        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
        )
        self.assertFalse(ok)
        self.assertTrue(any("missing execution evidence" in e for e in errors))

    def test_final_stage_requires_additional_tests(self):
        # Baseline registration only, but verifying with stage=final
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
        )
        self.assertFalse(ok)
        for additional in FULL_FINAL_ADDITIONAL:
            self.assertTrue(any(additional in e for e in errors), f"Expected error for {additional}")

    def test_gtest_xml_validation_passes_and_fails(self):
        # Empty GTest dir fails
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
            gtest_dir=self.gtest_dir,
        )
        self.assertFalse(ok)
        self.assertTrue(any("contains 0 executed tests" in e for e in errors))

        # With tests
        self.create_gtest_xml(["PersonalEngineTest"])
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
            junit_paths=[self.junit_xml],
            gtest_dir=self.gtest_dir,
        )
        self.assertTrue(ok, f"Expected pass, got: {errors}")

    def test_core_only_registration_and_execution_passes(self):
        self.create_registered_json(CORE_ONLY_BASELINE_REQUIRED)
        self.create_junit_xml({t: "passed" for t in CORE_ONLY_BASELINE_REQUIRED})
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="core-only",
            stage="baseline",
            junit_paths=[self.junit_xml],
        )
        self.assertTrue(ok, f"Expected core-only pass, got: {errors}")

    def test_core_only_registration_fails_under_full_profile(self):
        # When building core-only, profile=full must reject it
        self.create_registered_json(CORE_ONLY_BASELINE_REQUIRED)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="baseline",
        )
        self.assertFalse(ok)
        self.assertTrue(any("Missing required tests in registration" in e for e in errors))

    def test_missing_core_test_in_core_only_fails(self):
        tests = [t for t in CORE_ONLY_BASELINE_REQUIRED if t != "modernime_learning_store"]
        self.create_registered_json(tests)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="core-only",
            stage="baseline",
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_learning_store" in e for e in errors))

    def test_core_only_final_stage_requires_additional_tests(self):
        self.create_registered_json(CORE_ONLY_BASELINE_REQUIRED)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="core-only",
            stage="final",
        )
        self.assertFalse(ok)
        for additional in CORE_ONLY_FINAL_ADDITIONAL:
            self.assertTrue(any(additional in e for e in errors), f"Expected error for {additional}")

    def test_core_only_final_stage_passes_when_complete(self):
        complete = list(CORE_ONLY_BASELINE_REQUIRED) + list(CORE_ONLY_FINAL_ADDITIONAL)
        self.create_registered_json(complete)
        self.create_junit_xml({t: "passed" for t in complete})
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="core-only",
            stage="final",
            junit_paths=[self.junit_xml],
        )
        self.assertTrue(ok, f"Expected pass for complete core-only final, got: {errors}")

    def test_final_stage_missing_navigation_dbus_execution_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        # Execute everything except modernime_settings_navigation_dbus
        executed = {t: "passed" for t in all_final if t != "modernime_settings_navigation_dbus"}
        self.create_junit_xml(executed)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_settings_navigation_dbus" in e for e in errors))
        self.assertTrue(any("missing execution evidence" in e for e in errors))

    def test_final_stage_navigation_dbus_failure_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        results = {t: "passed" for t in all_final}
        results["modernime_settings_navigation_dbus"] = "failed"
        self.create_junit_xml(results)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
        )
        self.assertFalse(ok)
        self.assertTrue(any("Test FAILED: modernime_settings_navigation_dbus" in e for e in errors))

    def test_navigation_dbus_whitelisted_skip_in_headless_passes(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        results = {t: "passed" for t in all_final}
        results["modernime_settings_navigation_dbus"] = "skipped"
        self.create_junit_xml(results)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
            execution="headless",
        )
        self.assertTrue(ok, f"Expected pass for headless skip, got: {errors}")

    def test_navigation_dbus_skip_in_desktop_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        results = {t: "passed" for t in all_final}
        results["modernime_settings_navigation_dbus"] = "skipped"
        self.create_junit_xml(results)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
            execution="desktop",
        )
        self.assertFalse(ok)
        self.assertTrue(
            any("Illegal test SKIP for modernime_settings_navigation_dbus" in e for e in errors)
        )

    def test_final_stage_missing_runtime_controller_concurrency_execution_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        executed = {t: "passed" for t in all_final if t != "modernime_runtime_controller_concurrency"}
        self.create_junit_xml(executed)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
        )
        self.assertFalse(ok)
        self.assertTrue(any("modernime_runtime_controller_concurrency" in e for e in errors))
        self.assertTrue(any("missing execution evidence" in e for e in errors))

    def test_final_stage_runtime_controller_concurrency_failure_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        results = {t: "passed" for t in all_final}
        results["modernime_runtime_controller_concurrency"] = "failed"
        self.create_junit_xml(results)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
        )
        self.assertFalse(ok)
        self.assertTrue(any("Test FAILED: modernime_runtime_controller_concurrency" in e for e in errors))

    def test_runtime_controller_concurrency_skip_in_headless_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        results = {t: "passed" for t in all_final}
        results["modernime_runtime_controller_concurrency"] = "skipped"
        self.create_junit_xml(results)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=[self.junit_xml],
            execution="headless",
        )
        self.assertFalse(ok)
        self.assertTrue(
            any("Illegal test SKIP for modernime_runtime_controller_concurrency" in e for e in errors)
        )

    def test_nonexistent_junit_path_fails(self):
        all_final = list(FULL_BASELINE_REQUIRED) + list(FULL_FINAL_ADDITIONAL)
        self.create_registered_json(all_final)
        ok, errors = verify(
            registered_path=self.registered_json,
            profile="full",
            stage="final",
            junit_paths=["/tmp/nonexistent_junit_report.xml"],
        )
        self.assertFalse(ok)
        self.assertTrue(any("Error reading JUnit XML" in e or "not found" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
