#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import contextlib
import io
import json
import os
import runpy
import select
import signal
import stat
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path
from types import SimpleNamespace
from typing import Optional
from unittest.mock import Mock
from unittest.mock import patch

smoke = SimpleNamespace(**runpy.run_path(str(Path(__file__).resolve().parents[2] / "Meta/SiteSmoke/site-smoke.py")))
tranco = SimpleNamespace(**runpy.run_path(str(Path(smoke.__file__).with_name("filter-tranco.py"))))


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
        self.assertEqual(len(sites), 100)
        ranks = [rank for rank, _ in sites]
        self.assertEqual(ranks, sorted(set(ranks)))
        self.assertEqual(len({url for _, url in sites}), 100)
        self.assertEqual(sites[0], (1, "https://google.com/"))

    def test_tranco_filter_requires_a_successful_html_response(self):
        for returncode, response, expected in (
            (0, "200\ttext/html; charset=utf-8", True),
            (0, "204\t", False),
            (0, "200\tapplication/json", False),
            (0, "404\ttext/html", False),
            (60, "000\t", False),
        ):
            with (
                self.subTest(response=response, returncode=returncode),
                patch.object(
                    tranco.subprocess, "run", return_value=SimpleNamespace(returncode=returncode, stdout=response)
                ),
            ):
                self.assertEqual(tranco.probe(("2", "example.com")), expected)

    def test_tranco_filter_bounds_the_entire_probe_and_skips_a_timeout(self):
        def fake_curl(command, *, timeout, **_kwargs):
            self.assertEqual(timeout, 40)
            if command[-1] == "https://slow.example/":
                raise subprocess.TimeoutExpired(command, timeout)
            return SimpleNamespace(returncode=0, stdout="200\ttext/html")

        with patch.object(tranco.subprocess, "run", side_effect=fake_curl) as curl:
            rows = [("1", "slow.example"), ("2", "browsable.example")]
            self.assertEqual(tranco.select_domains(iter(rows), count=1), rows[1:])
        self.assertEqual(curl.call_count, 2)

    def test_tranco_filter_preserves_original_rank_numbers(self):
        rows = [("1", "unreachable.example"), ("2", "first.example"), ("4", "second.example"), ("8", "later.example")]
        with patch.dict(tranco.select_domains.__globals__, {"probe": lambda row: row != rows[0]}):
            self.assertEqual(tranco.select_domains(iter(rows), count=2), rows[1:3])

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

    def test_large_nonempty_body_count_drops_are_reported_but_do_not_block(self):
        baseline = run(
            site("large-drop", elements=22),
            site("exact-percentage", elements=20),
            site("exact-count", elements=10),
            site("small-count-drop", elements=10),
            site("small-percentage-drop", elements=20),
        )
        current = run(
            site("large-drop", elements=15),
            site("exact-percentage", elements=14),
            site("exact-count", elements=5),
            site("small-count-drop", elements=6),
            site("small-percentage-drop", elements=15),
        )
        changes, blocking = smoke.gate_regressions(baseline, current, 5)
        self.assertEqual(
            [(row["url"], row["metric"], row["direction"], row["before"], row["after"]) for row in changes],
            [
                ("large-drop", "body_element_count", "changed", 22, 15),
                ("exact-percentage", "body_element_count", "changed", 20, 14),
                ("exact-count", "body_element_count", "changed", 10, 5),
            ],
        )
        self.assertTrue(all(row["confirmation"] == "pending" for row in changes))
        self.assertEqual(blocking, [])

    def test_only_persistent_body_count_drops_are_confirmed_by_reruns(self):
        names = ("persistent", "cleared", "inconclusive", "unchanged")
        urls = [f"https://{name}.example/" for name in names]
        baseline = run(*(site(url, elements=22) for url in urls))
        current = run(
            *(dict(site(url, elements=14 if rank < 4 else 22), rank=rank) for rank, url in enumerate(urls, 1))
        )
        settings = {"navigation_timeout_s": 25, "settle_s": 2, "viewport": smoke.VIEWPORT}
        baseline["settings"].update(settings)
        current["settings"].update(settings)
        changes = smoke.compare(baseline, current)
        attempts = [
            site(urls[0], elements=14),
            site(urls[1], elements=22),
            site(urls[2], status="timeout", elements=None),
            site(urls[0], elements=15),
            site(urls[2], elements=14),
        ]
        capture = Mock(side_effect=attempts)
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.dict(smoke.confirm_body_drops.__globals__, {"capture": capture}),
        ):
            smoke.confirm_body_drops(changes, current, Path("WebDriver"), Path(temporary) / "reruns")
        self.assertEqual(
            [(row["url"], row["confirmation"]) for row in changes],
            list(zip(urls, ("confirmed", "cleared-on-rerun", "inconclusive"))),
        )
        self.assertEqual(
            [call.args[2] for call in capture.call_args_list],
            [(1, urls[0]), (2, urls[1]), (3, urls[2]), (1, urls[0]), (3, urls[2])],
        )

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

    @unittest.skipUnless(os.name == "posix" and smoke.connect is not None, "requires POSIX and websockets")
    def test_run_keeps_output_and_driver_logs_private_under_a_permissive_umask(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "results"
            output.mkdir()
            output.chmod(0o755)
            command = [
                sys.executable,
                smoke.__file__,
                "run",
                "--webdriver",
                sys.executable,
                "--revision",
                "fixture",
                "--output",
                str(output),
                "--url",
                "https://example.com/",
            ]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10, umask=0)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
            for name in ("001.log", "results.json"):
                with self.subTest(name=name):
                    self.assertEqual(stat.S_IMODE((output / name).stat().st_mode), 0o600)

    def test_ci_preserves_details_and_accepts_partial_harness_errors(self):
        def fake_run(command, **_):
            self.assertEqual(command[2], "run")
            self.assertNotIn("--url", command)
            details = Path(command[command.index("--output") + 1])
            _, targets = smoke.targets(None)
            rows = [
                {"rank": rank, **site(url, smoke.STATUSES[index % len(smoke.STATUSES)])}
                for index, (rank, url) in enumerate(targets)
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
            self.assertEqual(results[smoke.targets(None)[1][4][1]], "SKIP")
            self.assertTrue((directory / "ci.json.details" / "results.json").is_file())

            unrelated = directory / "unrelated.json"
            unrelated.write_text("untouched")
            for target in (unrelated, directory):
                with self.subTest(target=target):
                    link = directory / ("link-to-" + target.name + ".json")
                    link.symlink_to(target, target_is_directory=target.is_dir())
                    self.assertEqual(smoke.run_ci(ladybird, link), 0)
                    self.assertFalse(link.is_symlink())
                    self.assertEqual(json.loads(link.read_text()), {"results": results})
                    self.assertEqual(stat.S_IMODE(link.stat().st_mode), 0o600)
                    self.assertEqual(unrelated.read_text(), "untouched")

    def test_atomic_output_cleans_up_when_replace_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            output = directory / "output"
            output.mkdir()
            with self.assertRaises(OSError):
                smoke.write_json_atomically(output, {"results": {}})
            self.assertEqual(list(directory.iterdir()), [output])

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

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_sigterm_cleans_up_a_detached_stage(self):
        launcher = """
import runpy
import signal
import sys

smoke = runpy.run_path(sys.argv[1])
signal.signal(signal.SIGTERM, smoke["request_termination"])
try:
    smoke["run_stage"]([sys.executable, "-c", sys.argv[2]])
except smoke["TerminationRequested"]:
    raise SystemExit(128 + signal.SIGTERM) from None
"""
        child = "import os, signal; print(os.getpid(), flush=True); signal.pause()"
        command = [sys.executable, "-c", launcher, str(Path(smoke.__file__)), child]
        with subprocess.Popen(command, stdout=subprocess.PIPE, start_new_session=True, text=True) as wrapper:
            assert wrapper.stdout is not None
            child_pid = None
            try:
                ready, _, _ = select.select([wrapper.stdout], [], [], 10)
                self.assertTrue(ready, "the detached stage did not report its PID")
                child_pid = int(wrapper.stdout.readline())
                self.assertEqual(os.getpgid(child_pid), child_pid)
                self.assertNotEqual(child_pid, wrapper.pid)
                os.killpg(wrapper.pid, signal.SIGTERM)
                self.assertEqual(wrapper.wait(timeout=10), 128 + signal.SIGTERM)
                with self.assertRaises(ProcessLookupError):
                    os.killpg(child_pid, 0)
            finally:
                if child_pid is not None:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(child_pid, signal.SIGKILL)
                if wrapper.poll() is None:
                    wrapper.kill()

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
                    fields = [state, "123"] + ["0"] * 47 + [str(int(status))]
                    (child / "stat").write_text("456 (Hub) " + " ".join(fields))
                    self.assertEqual(smoke.browser_exited_abnormally(123, proc), crashed)
            (child / "stat").write_text("456 (Hub) Z 123 " + "0 " * 47 + "invalid")
            self.assertFalse(smoke.browser_exited_abnormally(123, proc))

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
