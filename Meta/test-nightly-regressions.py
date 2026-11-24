# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import json
import tempfile
import unittest

from pathlib import Path

from nightly_regressions import summarize


class NightlyRegressionsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.reports = self.root / "reports"
        self.results = {"wpt": "success", "test262": "success", "sites": "success"}
        self.revisions = {"browser": "browser-a", "wpt": "wpt-a", "test262": "test262-a"}

    def write_json(self, relative_path, data):
        path = self.reports / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data) + "\n", encoding="utf-8")

    def write_reports(self, wpt="PASS", test262="PASSED", sites="PASS"):
        self.write_json(
            "wpt-1.json",
            {
                "results": [
                    {
                        "test": "/example.html",
                        "subsuite": "",
                        "status": "OK",
                        "subtests": [{"name": "assertion", "status": wpt}],
                    }
                ]
            },
        )
        self.write_json("wpt-2.json", {"results": [{"test": "/ref.html", "status": "PASS", "subtests": []}]})
        self.write_json("test262.json", {"results": {"test.js": test262}})
        self.write_json("sites/sites.json", {"results": {"https://example.com/": sites}})

    def test_first_night_records_baseline_even_with_known_failures(self):
        self.write_reports(wpt="FAIL", test262="FAILED")
        output = self.root / "baseline"
        summary, hard, soft, errors = summarize(
            self.root / "none", self.reports, output, "", self.results, self.revisions
        )
        self.assertEqual((hard, soft, errors), (0, 0, 0))
        self.assertIn("Recorded first baseline", summary)
        self.assertIn("- wpt revision: <code>wpt-a</code>", summary)
        self.assertIn("- Baseline WPT revision: <code>unavailable</code>", summary)
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        self.assertEqual(data["revisions"]["wpt"], "wpt-a")
        self.assertEqual(data["baseline_revisions"]["wpt"], "")

    def test_wpt_subtests_and_test262_regressions_are_hard_but_sites_are_soft(self):
        self.write_reports()
        baseline = self.root / "baseline"
        output = self.root / "output"
        summarize(self.root / "none", self.reports, baseline, "", self.results, self.revisions)
        self.write_reports(wpt="FAIL", test262="TIMEOUT_ERROR", sites="FAIL")
        summary, hard, soft, errors = summarize(baseline, self.reports, output, "12", self.results, self.revisions)
        self.assertEqual((hard, soft, errors), (2, 1, 0))
        self.assertIn("site regressions (advisory)", summary)
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        self.assertEqual(data["baseline_run"], "12")
        [regression] = data["suites"]["wpt"]["regressions"]
        self.assertEqual(json.loads(regression["test"]), ["", "/example.html", "assertion"])
        self.assertEqual(regression["after"], "FAIL")

    def test_report_names_are_literal_markdown_code(self):
        name = "``[link](https://example.com) <b>bold</b>\n\n# Heading`"
        self.write_reports()
        self.write_json("test262.json", {"results": {name: "PASSED"}})
        baseline = self.root / "baseline"
        summarize(self.root / "none", self.reports, baseline, "", self.results, self.revisions)
        self.write_json("test262.json", {"results": {name: "FAILED"}})
        output = self.root / "output"
        summary, hard, soft, errors = summarize(baseline, self.reports, output, "12", self.results, self.revisions)
        self.assertEqual((hard, soft, errors), (1, 0, 0))
        self.assertIn(
            "- ``` ``[link](https://example.com) <b>bold</b>  # Heading` ```: PASSED → FAILED", summary.splitlines()
        )
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        self.assertEqual(data["suites"]["test262"]["regressions"][0]["test"], name)

    def test_sites_use_committed_baseline_only_when_previous_statuses_are_missing(self):
        self.write_reports(sites="FAIL")
        site_baseline = self.root / "site-baseline.json"
        site_baseline.write_text(
            json.dumps({"sites": [{"url": "https://example.com/", "status": "success"}]}), encoding="utf-8"
        )
        baseline = self.root / "baseline"
        summary, hard, soft, errors = summarize(
            self.root / "none", self.reports, baseline, "", self.results, self.revisions, site_baseline
        )
        self.assertEqual((hard, soft, errors), (0, 1, 0))
        self.assertIn("committed top-sites baseline", summary)
        summary, hard, soft, errors = summarize(
            baseline, self.reports, self.root / "output", "12", self.results, self.revisions, site_baseline
        )
        self.assertEqual((hard, soft, errors), (0, 0, 0))
        self.assertNotIn("committed top-sites baseline", summary)

    def test_suite_revision_change_labels_only_that_suites_regressions(self):
        self.write_reports()
        baseline = self.root / "baseline"
        output = self.root / "output"
        summarize(self.root / "none", self.reports, baseline, "", self.results, self.revisions)
        self.write_reports(wpt="FAIL", test262="FAILED")
        revisions = {**self.revisions, "browser": "browser-b", "wpt": "wpt-b"}
        summary, hard, soft, errors = summarize(baseline, self.reports, output, "12", self.results, revisions)
        self.assertEqual((hard, soft, errors), (2, 0, 0))
        self.assertIn("test suite changed (rev wpt-a→wpt-b)", summary)
        self.assertIn("browser revision: <code>browser-b</code>", summary)
        self.assertIn("- wpt revision: <code>wpt-b</code>", summary)
        self.assertIn("- Baseline WPT revision: <code>wpt-a</code>", summary)
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        self.assertEqual(data["revisions"], revisions)
        self.assertEqual(data["baseline_revisions"], self.revisions)
        self.assertEqual(data["suites"]["wpt"]["suite_revision_change"], {"before": "wpt-a", "after": "wpt-b"})
        self.assertNotIn("suite_revision_change", data["suites"]["test262"])

    def test_only_wpt_regressions_from_changed_test_files_are_labeled(self):
        self.write_reports()
        changed_tests = [
            "/test/direct%20file.html?mode=1#section",
            "/test/global.any.html",
            "/test/global.any.sharedworker-module.html",
            "/test/forced.https.any.serviceworker.html",
            "/test/window.window.html",
            "/test/worker.worker.html",
            "/test/visual.xhtml",
            "/test/zz-last.html",
        ]
        unchanged_tests = [f"/test/unchanged-{index:02d}.html" for index in range(26)]
        changed_files = {
            "test/direct file.html",
            "test/global.any.js",
            "test/forced.any.js",
            "test/window.window.js",
            "test/worker.worker.js",
            "test/visual-ref.xhtml",
            "test/zz-last.html",
            "elsewhere/unchanged-00.html",
            "test/unchanged-01-other.html",
        }

        def write_wpt(status):
            self.write_json(
                "wpt-1.json",
                {
                    "results": [
                        {"test": test, "status": "OK", "subtests": [{"name": "assertion", "status": status}]}
                        for test in changed_tests + unchanged_tests
                    ]
                },
            )

        baseline = self.root / "baseline"
        output = self.root / "output"
        write_wpt("PASS")
        summarize(self.root / "none", self.reports, baseline, "", self.results, self.revisions)
        write_wpt("FAIL")
        summary, hard, soft, errors = summarize(
            baseline,
            self.reports,
            output,
            "12",
            self.results,
            {**self.revisions, "wpt": "wpt-b"},
            wpt_changed_files=changed_files,
        )
        self.assertEqual((hard, soft, errors), (len(changed_tests + unchanged_tests), 0, 0))
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        labeled = {
            json.loads(entry["test"])[1]
            for entry in data["suites"]["wpt"]["regressions"]
            if entry.get("label") == "upstream test changed"
        }
        self.assertEqual(labeled, set(changed_tests))
        for test in changed_tests:
            self.assertIn(f"{json.dumps(['', test, 'assertion'])} `: PASS → FAIL — upstream test changed", summary)
        self.assertEqual(summary.count("upstream test changed"), len(changed_tests))

    def test_wpt_file_labels_require_a_baseline_revision(self):
        self.write_reports()
        baseline = self.root / "baseline"
        output = self.root / "output"
        summarize(self.root / "none", self.reports, baseline, "", self.results, self.revisions)
        (baseline / "regressions.json").unlink()
        self.write_reports(wpt="FAIL")
        summary, hard, soft, errors = summarize(
            baseline, self.reports, output, "12", self.results, self.revisions, wpt_changed_files={"example.html"}
        )
        self.assertEqual((hard, soft, errors), (1, 0, 0))
        self.assertNotIn("upstream test changed", summary)
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        self.assertNotIn("label", data["suites"]["wpt"]["regressions"][0])

    def test_skipped_site_job_and_malformed_report_are_distinct(self):
        self.write_reports()
        self.write_json("wpt-2.json", {"results": []})
        self.results["sites"] = "skipped"
        summary, hard, soft, errors = summarize(
            self.root / "none", self.reports, self.root / "baseline", "", self.results, self.revisions
        )
        self.assertEqual((hard, soft, errors), (0, 0, 1))
        self.assertIn("harness not available", summary)
        self.assertIn("No completed WPT tests in wpt-2.json", summary)


if __name__ == "__main__":
    unittest.main()
