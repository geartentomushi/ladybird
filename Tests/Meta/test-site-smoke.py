#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import contextlib
import io
import json
import runpy
import signal
import sys
import tempfile
import unittest

from pathlib import Path
from types import SimpleNamespace
from typing import Optional
from unittest.mock import Mock
from unittest.mock import patch

smoke = SimpleNamespace(**runpy.run_path(str(Path(__file__).resolve().parents[2] / "Meta/SiteSmoke/site-smoke.py")))


def site(url, status="success", elements: Optional[int] = 1, **errors):
    return {
        "url": url,
        "status": status,
        "body_element_count": elements,
        "screenshot": "001.png",
        "console_errors": dict.fromkeys(smoke.ERROR_CATEGORIES, 0) | errors,
    }


def run(*sites):
    return {"schema_version": 1, "settings": {"jobs": 4}, "sites": sites}


class SiteSmokeTests(unittest.TestCase):
    def test_pinned_snapshot_preserves_unique_ranks_and_targets(self):
        source, sites = smoke.targets(None)
        self.assertIn("8PW6V (2026-11-22)", source)
        self.assertEqual([rank for rank, _ in sites], list(range(1, 101)))
        self.assertEqual(len({url for _, url in sites}), 100)
        self.assertEqual(sites[0], (1, "https://google.com/"))
        self.assertEqual(sites[-1], (100, "https://nginx.com/"))

    def test_console_errors_and_navigation_are_collected_while_waiting(self):
        messages = iter(
            [
                {
                    "method": "log.entryAdded",
                    "params": {"level": "error", "type": "javascript", "text": "Not implemented"},
                },
                {"method": "log.entryAdded", "params": {"level": "error", "type": "javascript", "text": "TypeError"}},
                {"method": "log.entryAdded", "params": {"level": "error", "type": "console", "text": "Oops"}},
                {"method": "log.entryAdded", "params": {"level": "warn", "type": "console", "text": "Ignore"}},
                {
                    "method": "network.responseStarted",
                    "params": {"context": "top", "navigation": "nav", "response": {"status": 301}},
                },
                {
                    "method": "network.responseStarted",
                    "params": {"context": "top", "navigation": "nav", "response": {"status": 404}},
                },
                {
                    "method": "network.responseStarted",
                    "params": {"context": "top", "navigation": None, "response": {"status": 500}},
                },
                {"id": 1, "type": "success", "result": {"navigation": "nav"}},
            ]
        )
        bidi = smoke.BiDi(SimpleNamespace(send=lambda _: None, recv=lambda timeout: json.dumps(next(messages))))
        self.assertEqual(bidi.command("browsingContext.navigate", {}), {"navigation": "nav"})
        self.assertEqual(bidi.errors, dict.fromkeys(smoke.ERROR_CATEGORIES, 1))
        self.assertEqual(bidi.navigation, {("top", "nav"): {"http_status": 404}})

    def test_navigation_deadline_does_not_consume_later_command_response(self):
        messages = iter([None, {"id": 1, "type": "success"}, {"id": 2, "type": "success", "result": "screenshot"}])

        def receive(timeout):
            message = next(messages)
            if message is None:
                raise TimeoutError
            return json.dumps(message)

        bidi = smoke.BiDi(SimpleNamespace(send=lambda _: None, recv=receive))
        with self.assertRaisesRegex(TimeoutError, "navigate"):
            bidi.command("browsingContext.navigate", {})
        self.assertEqual(bidi.command("browsingContext.captureScreenshot", {}), "screenshot")

    def test_diffs_status_render_health_categories_and_coverage_by_url(self):
        baseline = run(
            site("now-timed-out"),
            site("now-loads", "crash"),
            site("now-empty", not_implemented=1),
            site("removed"),
        )
        current = run(
            site("now-empty", elements=0, console_error=2),
            site("now-loads"),
            site("now-timed-out", "timeout"),
            site("added"),
        )
        actual = {(row["url"], row["metric"], row["direction"]) for row in smoke.compare(baseline, current)}
        self.assertEqual(
            actual,
            {
                ("now-timed-out", "status", "regression"),
                ("now-loads", "status", "improvement"),
                ("now-empty", "nonempty_body", "regression"),
                ("now-empty", "not_implemented", "improvement"),
                ("now-empty", "console_error", "regression"),
                ("removed", "coverage", "removed"),
                ("added", "coverage", "added"),
            },
        )

    def test_rejects_incomparable_settings(self):
        baseline, current = run(site("example")), run(site("example"))
        current["settings"]["jobs"] = 8
        with self.assertRaisesRegex(ValueError, "different.*settings"):
            smoke.compare(baseline, current)

    def test_compare_cli_exits_nonzero_only_when_changes_include_regressions(self):
        with tempfile.TemporaryDirectory() as temporary:
            old = Path(temporary) / "old.json"
            new = Path(temporary) / "new.json"
            crashed = Path(temporary) / "crashed.json"
            old.write_text(json.dumps(run(site("example"))))
            new.write_text(json.dumps(run(site("example", "timeout"))))
            crashed.write_text(json.dumps(run(site("example", "crash"))))
            for before, after, status in ((old, new, 1), (new, old, 0), (old, old, 0), (new, crashed, 1)):
                with patch.object(sys, "argv", ["site-smoke.py", "compare", str(before), str(after), "--json"]):
                    with contextlib.redirect_stdout(io.StringIO()) as output:
                        self.assertEqual(smoke.main(), status)
                    self.assertEqual(
                        any(row["direction"] == "regression" for row in json.loads(output.getvalue())),
                        bool(status),
                    )

    def test_gate_blocks_partial_loads_new_crashes_and_large_console_increases(self):
        baseline = run(
            site("crashed", "load-error"),
            site("timeout"),
            site("empty"),
            site("no-capture"),
            site("no-dom"),
            site("noisy", console_error=2),
            site("small-increase"),
        )
        no_capture = site("no-capture")
        no_capture["screenshot"] = None
        current = run(
            site("crashed", "crash"),
            site("timeout", "timeout"),
            site("empty", elements=0),
            no_capture,
            site("no-dom", elements=None),
            site("noisy", console_error=5, not_implemented=2),
            site("small-increase", console_error=4),
        )
        changes, blocking = smoke.gate_regressions(baseline, current, 5)
        self.assertTrue(any(change["url"] == "small-increase" for change in changes))
        self.assertEqual(
            {(change["url"], change["metric"]) for change in blocking},
            {
                ("crashed", "status"),
                ("timeout", "status"),
                ("empty", "nonempty_body"),
                ("no-capture", "missing_screenshot"),
                ("no-dom", "missing_body_element_count"),
                ("noisy", "total_console_errors"),
            },
        )

    def test_compare_and_gate_agree_on_new_crashes_after_failed_loads(self):
        for status in ("timeout", "load-error", "harness-error"):
            with self.subTest(status=status):
                before = run(site("example", status))
                after = run(site("example", "crash"))
                changes, blocking = smoke.gate_regressions(before, after, 5)
                self.assertEqual(changes[0]["direction"], "regression")
                self.assertEqual(changes, blocking)

    def test_gate_writes_summary_and_returns_nonzero_after_suite_and_site_failures(self):
        settings = {"jobs": 4, "navigation_timeout_s": 25, "settle_s": 2}
        baseline = dict(run(site("example")), revision="baseline", settings=settings)
        current = dict(run(site("example", "timeout")), settings=settings, summary={"sites": 1})
        args = SimpleNamespace(cc=None, cxx=None, build_jobs=None, test_jobs=6, console_error_increase=5)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            commands = []

            def execute(command, **kwargs):
                commands.append(command)
                if "--results-dir" in command:
                    directory = command[command.index("--results-dir") + 1]
                    directory.mkdir()
                    counts = {
                        "total": 2,
                        "fail": int(directory.name == "libweb-text"),
                        "timeout": 0,
                        "crashed": 0,
                        "skipped": 0,
                    }
                    (directory / "results.js").write_text(f"const RESULTS_DATA = {json.dumps({'summary': counts})};")
                if "--webdriver" in command:
                    directory = command[command.index("--output") + 1]
                    directory.mkdir()
                    (directory / "results.json").write_text(json.dumps(current))
                return 0

            with contextlib.ExitStack() as stack:
                stack.enter_context(patch.object(smoke.subprocess, "check_output", return_value="candidate\n"))
                stack.enter_context(patch.dict(smoke.run_gate.__globals__, {"run_stage": execute}))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                self.assertEqual(smoke.run_gate(args, output, baseline), 1)
            report = json.loads((output / "gate-summary.json").read_text())
            self.assertFalse(report["passed"])
            self.assertEqual(report["stages"]["libweb-text"]["summary"]["fail"], 1)
            self.assertEqual(report["stages"]["libweb-layout"]["summary"]["pass"], 2)
            self.assertEqual(report["site_regressions"][0]["after"], "timeout")
            self.assertIn("test-js", commands[0])
            self.assertEqual(commands[-2][0], "ctest")

    def test_gate_counts_evidence_loss_on_unchanged_failed_loads(self):
        before = site("lost", "load-error")
        missing = dict(before, screenshot=None, body_element_count=None)
        absent = dict(missing, url="absent")
        _, blocking = smoke.gate_regressions(run(before, absent), run(missing, absent), 5)
        self.assertEqual(
            {(change["url"], change["metric"]) for change in blocking},
            {("lost", "missing_screenshot"), ("lost", "missing_body_element_count")},
        )

    def test_ci_status_mapping_and_infrastructure_failure(self):
        rows = [site(status, status) for status in smoke.STATUSES]
        self.assertEqual(
            smoke.ci_results(rows),
            {
                "results": {
                    "success": "PASS",
                    "timeout": "FAIL",
                    "crash": "FAIL",
                    "load-error": "FAIL",
                    "harness-error": "SKIP",
                }
            },
        )
        for sites in ([], [site("unavailable", "harness-error")]):
            with self.assertRaisesRegex(ValueError, "No site was tested"):
                smoke.ci_results(sites)

    def test_ci_requires_a_sibling_webdriver(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            ladybird = directory / "Ladybird"
            ladybird.touch(mode=0o755)
            with self.assertRaisesRegex(ValueError, "WebDriver next to Ladybird"):
                smoke.run_ci(ladybird, directory / "results.json")

    def test_ci_preserves_details_and_accepts_partial_harness_errors(self):
        def fake_run(command, **_):
            self.assertEqual(command[2], "run")
            self.assertNotIn("--url", command)
            details = Path(command[command.index("--output") + 1])
            _, targets = smoke.targets(None)
            rows = [
                {"rank": rank, **site(url, smoke.STATUSES[(rank - 1) % len(smoke.STATUSES)])} for rank, url in targets
            ]
            (details / "results.json").write_text(json.dumps({"sites": rows}))
            return 1

        with tempfile.TemporaryDirectory() as temporary, patch.dict(smoke.run_ci.__globals__, {"run_stage": fake_run}):
            directory = Path(temporary)
            ladybird = directory / "Ladybird"
            ladybird.touch(mode=0o755)
            (directory / "WebDriver").touch(mode=0o755)
            output = directory / "ci.json"
            self.assertEqual(smoke.run_ci(ladybird, output), 0)
            results = json.loads(output.read_text())["results"]
            self.assertEqual(len(results), 100)
            self.assertEqual(results["https://google.com/"], "PASS")
            self.assertEqual(results["https://nginx.com/"], "SKIP")
            self.assertTrue((directory / "ci.json.details" / "results.json").is_file())

    def test_final_url_does_not_include_credentials_query_or_fragment(self):
        self.assertEqual(
            smoke.public_final_url("https://user:pass@host:8443/path?token=abc#state"), "https://host:8443/path"
        )
        self.assertIsNone(smoke.public_final_url("data:text/html,secret"))

    def test_cancel_kills_active_driver_groups_and_blocks_new_launches(self):
        processes = smoke.DriverProcesses()
        with (
            patch.object(smoke.subprocess, "Popen", return_value=SimpleNamespace(pid=123)) as spawn,
            patch.object(smoke.os, "killpg") as kill,
        ):
            processes.start(["WebDriver"], None)
            processes.cancel()
            kill.assert_called_once_with(123, signal.SIGKILL)
            with self.assertRaisesRegex(smoke.DriverError, "interrupted"):
                processes.start(["WebDriver"], None)
            spawn.assert_called_once()

    def test_stage_forwards_the_interrupt_signal_and_waits_for_exit(self):
        for error, signum in (
            (KeyboardInterrupt, signal.SIGINT),
            (smoke.TerminationRequested, signal.SIGTERM),
        ):
            with self.subTest(signum=signum):
                stage = SimpleNamespace(pid=123, wait=Mock(side_effect=[error, 128 + signum]))
                with (
                    patch.object(smoke.subprocess, "Popen", return_value=contextlib.nullcontext(stage)),
                    patch.object(smoke.os, "killpg") as kill,
                ):
                    with self.assertRaises(error):
                        smoke.run_stage(["stage"])
                    kill.assert_called_once_with(123, signum)
                    self.assertEqual(stage.wait.call_count, 2)

    def test_recovers_crash_evidence_from_profile_and_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary)
            driver = SimpleNamespace(poll=lambda: None)
            self.assertIsNone(smoke.crashed_process(driver, profile, {}))
            self.assertEqual(
                smoke.crashed_process(driver, profile, {"error": "navigate: unknown error: Browser connection lost"}),
                "Browser",
            )
            directory = profile / "data" / "CrashReports"
            directory.mkdir(parents=True)
            (directory / "2026-11-22T00-00-00Z-WebContent-123456.txt").touch()
            self.assertEqual(smoke.crashed_process(driver, profile, {}), "WebContent")

    def test_browser_wait_status_distinguishes_a_crash_from_driver_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            proc = Path(temporary)
            driver = proc / "123" / "task" / "123"
            driver.mkdir(parents=True)
            (driver / "children").write_text("456")
            child = proc / "456"
            child.mkdir()
            for state, status, crashed in (
                ("R", 0, False),
                ("Z", 0, False),
                ("Z", signal.SIGTERM, False),
                ("Z", signal.SIGKILL, True),
                ("Z", 1 << 8, True),
            ):
                with self.subTest(state=state, status=status):
                    fields = [state, "123"] + ["0"] * 47 + [str(status)]
                    (child / "stat").write_text("456 (Hub) " + " ".join(fields))
                    self.assertEqual(smoke.browser_exited_abnormally(123, proc), crashed)

    @unittest.skipIf(smoke.ConnectionClosedOK is None, "requires websockets")
    def test_session_close_during_settle_needs_crash_evidence(self):
        from websockets.frames import Close

        close = Close(1000, "")
        bidi = SimpleNamespace(errors={}, navigation={})
        bidi.command = lambda method, *_: (
            {"contexts": [{"context": "tab"}]}
            if method == "browsingContext.getTree"
            else {"url": "https://example.com/", "navigation": "navigation"}
        )

        def observe(_):
            raise smoke.ConnectionClosedOK(close, close, True)

        bidi.observe = observe
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            driver = SimpleNamespace(pid=123, poll=lambda: None)
            connect = Mock(return_value=contextlib.nullcontext(None))
            browser_exited = Mock(return_value=False)
            with patch.dict(
                smoke.capture.__globals__,
                {
                    "launch_driver": lambda *_: contextlib.nullcontext((driver, path)),
                    "webdriver_port": lambda *_: 1234,
                    "open_session": lambda *_: "ws://localhost:1234",
                    "connect": connect,
                    "BiDi": lambda _: bidi,
                    "browser_exited_abnormally": browser_exited,
                },
            ):
                row = smoke.capture(path / "WebDriver", path, (1, "https://example.com/"), 5, 2)
                self.assertEqual(row["status"], "load-error")
                self.assertNotIn("crashed_process", row)
                self.assertEqual(smoke.ci_results([row]), {"results": {"https://example.com/": "FAIL"}})

                browser_exited.return_value = True
                row = smoke.capture(path / "WebDriver", path, (1, "https://example.com/"), 5, 2)
                self.assertEqual(row["status"], "crash")
                self.assertEqual(row["crashed_process"], "Browser")

                browser_exited.return_value = False
                reports = path / "data" / "CrashReports"
                reports.mkdir(parents=True)
                (reports / "2026-11-22T00-00-00Z-Browser-123456.txt").touch()
                row = smoke.capture(path / "WebDriver", path, (1, "https://example.com/"), 5, 2)
        viewport = smoke.VIEWPORT
        self.assertGreater(connect.call_args.kwargs["max_size"], viewport["width"] * viewport["height"] * 4 * 4 / 3)
        self.assertEqual(row["status"], "crash")
        self.assertEqual(row["crashed_process"], "Browser")
        self.assertEqual(smoke.ci_results([row]), {"results": {"https://example.com/": "FAIL"}})


if __name__ == "__main__":
    unittest.main()
