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
        summary, hard, soft, errors = summarize(
            self.root / "none", self.reports, self.root / "baseline", "", self.results, self.revisions
        )
        self.assertEqual((hard, soft, errors), (0, 0, 0))
        self.assertIn("Recorded first baseline", summary)

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
        data = json.loads((output / "regressions.json").read_text(encoding="utf-8"))
        self.assertEqual(data["revisions"], revisions)
        self.assertEqual(data["baseline_revisions"], self.revisions)
        self.assertEqual(data["suites"]["wpt"]["suite_revision_change"], {"before": "wpt-a", "after": "wpt-b"})
        self.assertNotIn("suite_revision_change", data["suites"]["test262"])

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
