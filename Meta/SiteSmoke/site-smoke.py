#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import base64
import contextlib
import csv
import http.client
import json
import os
import re
import runpy
import signal
import subprocess
import sys
import tempfile
import time

from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from datetime import timezone
from pathlib import Path
from threading import Lock
from urllib.parse import urlsplit

try:
    from websockets.exceptions import ConnectionClosedOK
    from websockets.exceptions import WebSocketException
    from websockets.sync.client import connect
except ImportError:
    ConnectionClosedOK = None
    WebSocketException = None
    connect = None

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
COMMAND_TIMEOUT = 10
VIEWPORT = {"width": 1280, "height": 800}
BIDI_MAX_MESSAGE_SIZE = 8 * 1024 * 1024
STATUSES = ("success", "timeout", "crash", "load-error", "harness-error")
ERROR_CATEGORIES = ("not_implemented", "javascript_exception", "console_error")


class DriverError(RuntimeError):
    pass


class TerminationRequested(KeyboardInterrupt):
    pass


def request_termination(_signum, _frame):
    raise TerminationRequested


class BiDi:
    def __init__(self, socket):
        self.socket = socket
        self.command_id = 0
        self.errors = dict.fromkeys(ERROR_CATEGORIES, 0)
        self.navigation = {}

    def read(self, deadline):
        message = json.loads(self.socket.recv(timeout=max(0, deadline - time.monotonic())))
        method = message.get("method")
        params = message.get("params", {})
        if method == "log.entryAdded" and params["level"] == "error":
            if re.search(
                r"\b(?:not[ _-]+implemented|notimplementederror|unimplemented)\b", params.get("text") or "", re.I
            ):
                category = "not_implemented"
            elif params["type"] == "javascript":
                category = "javascript_exception"
            else:
                category = "console_error"
            self.errors[category] += 1
        elif params.get("navigation") and method in ("network.responseStarted", "network.fetchError"):
            key = (params["context"], params["navigation"])
            if method == "network.responseStarted":
                self.navigation[key] = {"http_status": params["response"]["status"]}
            else:
                self.navigation.setdefault(key, {})["network_error"] = params["errorText"]
        return message

    def command(self, method, params, timeout=COMMAND_TIMEOUT):
        self.command_id += 1
        self.socket.send(json.dumps({"id": self.command_id, "method": method, "params": params}))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = self.read(deadline)
            except TimeoutError:
                break
            if message.get("id") == self.command_id:
                if message["type"] == "success":
                    return message["result"]
                raise DriverError(f"{method}: {message['error']}: {message['message']}")
        raise TimeoutError(f"{method} did not finish in {timeout}s")

    def observe(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                self.read(deadline)
            except TimeoutError:
                break


def kill_driver_group(pid):
    with contextlib.suppress(ProcessLookupError):
        os.killpg(pid, signal.SIGKILL)


class DriverProcesses:
    def __init__(self):
        self.lock = Lock()
        self.active = set()
        self.cancelled = False

    def start(self, command, log):
        with self.lock:
            if self.cancelled:
                raise DriverError("Smoke run interrupted")
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            self.active.add(process.pid)
            return process

    def cancel(self):
        with self.lock:
            self.cancelled = True
            for pid in self.active:
                kill_driver_group(pid)

    def discard(self, pid):
        with self.lock:
            self.active.discard(pid)


def open_private_file(path, flags):
    return os.open(path, flags | os.O_EXCL, 0o600)


@contextlib.contextmanager
def launch_driver(executable, directory, log_path, processes):
    with tempfile.TemporaryDirectory(prefix="profile-", dir=directory) as temporary:
        profile = Path(temporary) / "profile"
        with open(log_path, "wb", opener=open_private_file) as log:
            process = processes.start(
                [executable, "--headless", "--force-cpu-painting", "--port", "0", "--profile-path", profile],
                log,
            )
            try:
                yield process, profile
            finally:
                with contextlib.suppress(ProcessLookupError):
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    kill_driver_group(process.pid)
                    process.wait()
                kill_driver_group(process.pid)
                processes.discard(process.pid)


def webdriver_port(process, log_path):
    deadline = time.monotonic() + COMMAND_TIMEOUT
    with log_path.open("rb") as startup_log:
        while time.monotonic() < deadline and process.poll() is None:
            position = startup_log.tell()
            line = startup_log.readline()
            if not line.endswith(b"\n"):
                startup_log.seek(position)
                time.sleep(0.05)
                continue
            match = re.fullmatch(rb"Listening on 127\.0\.0\.1:(\d+)\s*", line)
            if match:
                return int(match[1])
    raise DriverError(f"WebDriver did not start (exit status: {process.poll()}); see {log_path}")


def open_session(port):
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=COMMAND_TIMEOUT)) as client:
        payload = json.dumps({"capabilities": {"alwaysMatch": {"webSocketUrl": True}}})
        client.request("POST", "/session", payload, {"Content-Type": "application/json"})
        response = client.getresponse()
        value = json.load(response)["value"]
        if response.status != 200:
            raise DriverError(f"Session creation failed: {value.get('message', response.status)}")
        return value["capabilities"]["webSocketUrl"]


def public_final_url(url):
    parts = urlsplit(url)
    if parts.scheme not in ("http", "https"):
        return None
    return parts._replace(netloc=parts.netloc.rsplit("@", 1)[-1], query="", fragment="").geturl()


def browser_exited_abnormally(driver_pid, proc=Path("/proc")):
    try:
        children = (proc / str(driver_pid) / "task" / str(driver_pid) / "children").read_text().split()
    except OSError:
        return False
    for pid in children:
        try:
            name, _, stat = (proc / pid / "stat").read_text().rpartition(") ")
        except OSError:
            continue
        if name.partition(" (")[2] not in ("Ladybird", "Hub"):
            continue
        fields = stat.split()
        if len(fields) >= 50 and fields[0] == "Z" and fields[1] == str(driver_pid):
            # Linux proc_pid_stat field 52 is the wait status; WebDriver uses SIGTERM for normal session cleanup.
            try:
                returncode = os.waitstatus_to_exitcode(int(fields[49]))
            except ValueError:
                continue
            if returncode not in (0, -signal.SIGTERM):
                return True
    return False


def crash_evidence(process, profile, row):
    for executable in ("WebContent", "Browser", "Hub"):
        if any((profile / "data" / "CrashReports").glob(f"*-{executable}-*.txt")):
            return executable
    if process.poll() is not None:
        return "WebDriver"
    for key in ("error", "capture_error", "dom_error"):
        message = row.get(key, "")
        if "WebContent crashed" in message:
            return "WebContent"
        if "Browser connection lost" in message:
            return "Browser"
    return None


def crashed_process(process, profile, row, *, session_closed=False):
    deadline = time.monotonic() + 0.25
    while True:
        if crashed := crash_evidence(process, profile, row):
            return crashed
        if not session_closed:
            return None
        if browser_exited_abnormally(process.pid):
            return "Browser"
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.05)


def sample_render(bidi, context, row, image_path, errors):
    params = {
        "target": {"context": context},
        "expression": "document.body ? document.body.querySelectorAll('*').length : 0",
        "awaitPromise": False,
    }
    try:
        result = bidi.command("script.evaluate", params)
        if result["type"] == "success" and result["result"]["type"] == "number":
            row["body_element_count"] = result["result"]["value"]
    except errors as error:
        row["dom_error"] = str(error)
    screenshot = bidi.command("browsingContext.captureScreenshot", {"context": context})
    data = base64.b64decode(screenshot["data"], validate=True)
    with open(image_path, "wb", opener=open_private_file) as image:
        image.write(data)
    row["screenshot"] = image_path.name


def capture(executable, directory, site, timeout, settle, processes=None):
    if connect is None or WebSocketException is None or ConnectionClosedOK is None:
        raise DriverError("Install `websockets` as pinned in requirements.txt")
    rank, url = site
    log_path = directory / f"{rank:03d}.log"
    image_path = directory / f"{rank:03d}.png"
    row = {
        "rank": rank,
        "url": url,
        "status": "harness-error",
        "load_ms": None,
        "final_url": None,
        "http_status": None,
        "body_element_count": None,
        "console_errors": None,
        "screenshot": None,
    }
    errors = (DriverError, OSError, WebSocketException, http.client.HTTPException)
    try:
        with launch_driver(executable, directory, log_path, processes or DriverProcesses()) as (process, profile):
            endpoint = open_session(webdriver_port(process, log_path))
            with connect(
                endpoint,
                proxy=None,
                compression=None,
                max_size=BIDI_MAX_MESSAGE_SIZE,
                open_timeout=COMMAND_TIMEOUT,
                close_timeout=1,
            ) as socket:
                bidi = BiDi(socket)
                context = bidi.command("browsingContext.getTree", {"maxDepth": 0})["contexts"][0]["context"]
                bidi.command("browsingContext.setViewport", {"context": context, "viewport": VIEWPORT})
                bidi.command(
                    "session.subscribe",
                    {"events": ["log.entryAdded", "network.responseStarted", "network.fetchError"]},
                )
                row["console_errors"] = bidi.errors
                started = time.monotonic()
                session_closed = False
                try:
                    nav = bidi.command(
                        "browsingContext.navigate", {"context": context, "url": url, "wait": "complete"}, timeout
                    )
                    row["final_url"] = public_final_url(nav["url"])
                    info = bidi.navigation.get((context, nav["navigation"]), {})
                    row["http_status"] = info.get("http_status")
                    if "network_error" in info or (row["http_status"] or 0) >= 400:
                        row["status"] = "load-error"
                        row["error"] = info.get("network_error", f"HTTP {row['http_status']}")
                    else:
                        row["status"] = "success"
                except TimeoutError:
                    row["status"] = "timeout"
                    row["error"] = f"Navigation exceeded {timeout:g}s"
                except errors as error:
                    row["status"] = "load-error"
                    row["error"] = str(error)
                    session_closed = isinstance(error, ConnectionClosedOK)
                row["load_ms"] = round((time.monotonic() - started) * 1000)
                try:
                    bidi.observe(settle)
                    sample_render(bidi, context, row, image_path, errors)
                except errors + (ValueError,) as error:
                    row["capture_error"] = str(error)
                    session_closed |= isinstance(error, ConnectionClosedOK)
                if crashed := crashed_process(process, profile, row, session_closed=session_closed):
                    row["status"] = "crash"
                    row["crashed_process"] = crashed
                elif session_closed and row["status"] == "success":
                    row["status"] = "load-error"
    except errors as error:
        row["error"] = str(error)
    return row


def summarize(sites):
    statuses = Counter(site["status"] for site in sites)
    return {
        "sites": len(sites),
        "statuses": {status: statuses[status] for status in STATUSES},
        "screenshots": sum(bool(site["screenshot"]) for site in sites),
        "nonempty_bodies": sum((site["body_element_count"] or 0) > 0 for site in sites),
        "console_errors": {
            category: sum((site["console_errors"] or {}).get(category, 0) for site in sites)
            for category in ERROR_CATEGORIES
        },
    }


def compare(baseline, current):
    if any(baseline[field] != current[field] for field in ("schema_version", "settings")):
        raise ValueError("Baseline and current run use different schema versions or capture settings")
    old_sites = {row["url"]: row for row in baseline["sites"]}
    changes = []
    for site in current["sites"]:
        url = site["url"]
        before = old_sites.pop(url, None)
        if before is None:
            changes.append({"url": url, "metric": "coverage", "direction": "added"})
            continue
        old_status, status = before["status"], site["status"]
        if old_status != status:
            direction = "changed"
            if status == "crash":
                direction = "regression"
            elif "harness-error" not in (old_status, status):
                if old_status == "success":
                    direction = "regression"
                elif status == "success":
                    direction = "improvement"
            changes.append(
                {"url": url, "metric": "status", "direction": direction, "before": old_status, "after": status}
            )
        if old_status != "success" or status != "success":
            continue
        metrics = {}
        old_count, count = before["body_element_count"], site["body_element_count"]
        if old_count is not None and count is not None:
            metrics["nonempty_body"] = (int(old_count > 0), int(count > 0), True)
        if before["console_errors"] is not None and site["console_errors"] is not None:
            for category in ERROR_CATEGORIES:
                metrics[category] = (before["console_errors"][category], site["console_errors"][category], False)
        for metric, (old, new, higher_is_better) in metrics.items():
            if new != old:
                direction = "improvement" if (new > old) == higher_is_better else "regression"
                changes.append({"url": url, "metric": metric, "direction": direction, "before": old, "after": new})
    changes.extend({"url": url, "metric": "coverage", "direction": "removed"} for url in old_sites)
    return changes


def targets(urls):
    if urls:
        return "custom", list(enumerate(urls, start=1))
    with (HERE / "tranco-top-100.csv").open(newline="") as snapshot:
        source = snapshot.readline().removeprefix("# ").strip()
        return source, [(int(row["rank"]), f"https://{row['domain']}/") for row in csv.DictReader(snapshot)]


def print_comparison(changes):
    counts = Counter(change["direction"] for change in changes)
    print(
        "Changes:",
        ", ".join(f"{key}={counts[key]}" for key in ("regression", "improvement", "changed", "added", "removed")),
    )
    for change in changes:
        delta = f": {change['before']} → {change['after']}" if "before" in change else ""
        print(f"{change['direction']:11} {change['url']} {change['metric']}{delta}")


def gate_regressions(baseline, current, console_threshold):
    changes = compare(baseline, current)
    regressions = [
        change
        for change in changes
        if (change["direction"] == "regression" and change["metric"] in ("status", "nonempty_body"))
        or change["direction"] in ("added", "removed")
    ]
    old_sites = {row["url"]: row for row in baseline["sites"]}
    for site in current["sites"]:
        before = old_sites.get(site["url"])
        if before is None:
            continue
        for metric in ("screenshot", "body_element_count"):
            if before[metric] is not None and site[metric] is None:
                regressions.append({"url": site["url"], "metric": f"missing_{metric}", "direction": "regression"})
        if before["status"] != "success" or site["status"] != "success":
            continue
        if before["console_errors"] is not None and site["console_errors"] is not None:
            old_count = sum(before["console_errors"].values())
            count = sum(site["console_errors"].values())
            if count - old_count >= console_threshold:
                regressions.append(
                    {
                        "url": site["url"],
                        "metric": "total_console_errors",
                        "direction": "regression",
                        "before": old_count,
                        "after": count,
                    }
                )
    return changes, regressions


def prepare_output(path):
    if path.is_symlink():
        raise ValueError(f"Output directory must not be a symbolic link: {path}")
    path = path.resolve()
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if any(path.iterdir()):
        raise ValueError(f"Output directory is not empty: {path}")
    path.chmod(0o700)
    return path


def ci_results(sites):
    status = {
        "success": "PASS",
        "timeout": "FAIL",
        "crash": "FAIL",
        "load-error": "FAIL",
        "harness-error": "SKIP",
    }
    if not sites or all(row["status"] == "harness-error" for row in sites):
        raise ValueError("No site was tested; check the detailed WebDriver logs")
    return {"results": {row["url"]: status[row["status"]] for row in sites}}


def run_stage(command, **kwargs):
    with subprocess.Popen(command, start_new_session=True, **kwargs) as process:
        try:
            return process.wait()
        except KeyboardInterrupt as error:
            signum = signal.SIGTERM if isinstance(error, TerminationRequested) else signal.SIGINT
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signum)
            process.wait()
            raise


def write_json_atomically(output, data):
    fd, name = tempfile.mkstemp(dir=output.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(json.dumps(data, indent=2) + "\n")
        os.replace(name, output)
    finally:
        Path(name).unlink(missing_ok=True)


def run_ci(ladybird, output):
    if not ladybird.is_absolute() or not output.is_absolute():
        raise ValueError("--ladybird and --output must be absolute paths")
    if not ladybird.is_file() or not os.access(ladybird, os.X_OK):
        raise ValueError(f"Not a Ladybird executable: {ladybird}")
    driver = ladybird.with_name("WebDriver")
    if not driver.is_file() or not os.access(driver, os.X_OK):
        raise ValueError(f"Expected an executable WebDriver next to Ladybird: {driver}")
    browser = driver.with_name("Ladybird")
    if not browser.is_file() or not ladybird.samefile(browser):
        raise ValueError(f"--ladybird must identify the adjacent browser WebDriver launches: {browser}")
    if not output.is_symlink() and output.is_dir():
        raise ValueError(f"--output must be a JSON file, not a directory: {output}")
    details = prepare_output(output.with_name(output.name + ".details"))
    status = run_stage(
        [
            sys.executable,
            __file__,
            "run",
            "--webdriver",
            str(driver),
            "--revision",
            "unreported",
            "--output",
            str(details),
        ],
    )
    if status not in (0, 1):
        raise ValueError(f"Site smoke harness exited {status}; see {details}")
    _, sites = targets(None)
    results_file = details / "results.json"
    try:
        rows = json.loads(results_file.read_text())["sites"]
        if [(row["rank"], row["url"]) for row in rows] != sites:
            raise ValueError("Results do not cover the pinned site list")
        result = ci_results(rows)
    except (OSError, TypeError, KeyError, ValueError) as error:
        raise ValueError(f"Invalid or unavailable site results in {results_file}: {error}") from error
    write_json_atomically(output, result)
    print(f"CI results: {output}")
    return 0


def run_gate(args, output, baseline):
    build = ROOT / "Build/release"
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    environment = os.environ.copy()
    environment.pop("BASH_ENV", None)
    report = {"revision": revision, "baseline_revision": baseline["revision"], "stages": {}}

    def stage(name, command):
        log = output / f"{name}.log"
        with open(log, "w", opener=open_private_file) as stream:
            try:
                status = run_stage(command, cwd=ROOT, env=environment, stdout=stream, stderr=subprocess.STDOUT)
            except OSError as error:
                print(error, file=stream)
                status = 1
        print(f"{name}: exit {status} ({log})", flush=True)
        report["stages"][name] = {"exit_status": status, "log": log.name}
        return report["stages"][name]

    def finish():
        report["passed"] = not report.get("site_regressions") and all(
            details["exit_status"] == 0 and "error" not in details for details in report["stages"].values()
        )
        with open(output / "gate-summary.json", "w", opener=open_private_file) as stream:
            stream.write(json.dumps(report, indent=2) + "\n")
        print(f"Gate {'passed' if report['passed'] else 'failed'}: {output / 'gate-summary.json'}")
        return int(not report["passed"])

    command = [sys.executable, ROOT / "Meta/ladybird.py", "build", "--preset", "Release"]
    for option in ("cc", "cxx", "build_jobs"):
        if value := getattr(args, option):
            command.extend(["--jobs" if option == "build_jobs" else f"--{option}", str(value)])
    command.extend(
        ["ladybird", "WebDriver", "test-web", "test-js", "test-js-debugger", "test262-runner", "TestVMThreads"]
    )
    if stage("build", command)["exit_status"]:
        return finish()

    read_test_results = runpy.run_path(str(ROOT / "Meta/test-results.py"))["read_results"]
    for mode in ("Text", "Layout", "Ref"):
        results_dir = output / f"libweb-{mode.lower()}"
        details = stage(
            results_dir.name,
            [
                build / "bin/test-web",
                "--test-path",
                ROOT / "Tests/LibWeb",
                "--results-dir",
                results_dir,
                "--filter",
                f"{mode}/input/",
                "--test-concurrency",
                str(args.test_jobs),
                "--per-test-timeout",
                "120",
            ],
        )
        try:
            summary = read_test_results(results_dir / "results.js")["summary"]
            details["summary"] = summary
            summary["pass"] = summary["total"] - sum(summary[key] for key in ("fail", "timeout", "crashed", "skipped"))
            if summary["pass"] <= 0 or any(summary[key] for key in ("fail", "timeout", "crashed")):
                details["error"] = "The suite had failures or ran no passing tests"
        except (KeyError, OSError, ValueError) as error:
            details["error"] = str(error)

    stage(
        "libjs",
        ["ctest", "--test-dir", build / "Tests/LibJS", "--output-on-failure", "--no-tests=error", "--timeout", "1800"],
    )
    settings = baseline["settings"]
    sites_dir = output / "sites"
    details = stage(
        "site-smoke",
        [
            sys.executable,
            __file__,
            "run",
            "--webdriver",
            build / "bin/WebDriver",
            "--revision",
            revision,
            "--output",
            sites_dir,
            "--jobs",
            str(settings["jobs"]),
            "--timeout",
            str(settings["navigation_timeout_s"]),
            "--settle",
            str(settings["settle_s"]),
        ],
    )
    try:
        current = json.loads((sites_dir / "results.json").read_text())
        changes, regressions = gate_regressions(baseline, current, args.console_error_increase)
        details["summary"] = current["summary"]
        report["site_changes"] = changes
        report["site_regressions"] = regressions
        report["console_error_increase"] = args.console_error_increase
        print_comparison(changes)
        if regressions:
            print("Blocking findings:")
            for finding in regressions:
                print(f"  {finding['url']} {finding['metric']}")
    except (KeyError, OSError, ValueError) as error:
        details["error"] = str(error)
    return finish()


def main():
    parser = argparse.ArgumentParser(
        description="Measure Ladybird headless navigation and compare real-site smoke runs."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="Capture a smoke run")
    run.add_argument("--webdriver", type=Path, required=True, help="Built Ladybird WebDriver executable")
    run.add_argument("--revision", required=True, help="Revision or build label for the supplied executable")
    run.add_argument(
        "--output", type=Path, required=True, help="New directory for results, screenshots and driver logs"
    )
    run.add_argument("--url", action="append", help="Use these URLs instead of the pinned list (repeatable)")
    run.add_argument("--jobs", type=int, default=4)
    run.add_argument("--timeout", type=float, default=25, help="Per-site navigation deadline in seconds")
    run.add_argument("--settle", type=float, default=2, help="Seconds to observe before taking each screenshot")
    comparison = commands.add_parser("compare", help="Compare two results files")
    comparison.add_argument("baseline", type=Path)
    comparison.add_argument("current", type=Path)
    comparison.add_argument("--json", action="store_true", help="Print the comparison as JSON")
    gate = commands.add_parser(
        "gate", help="Build and run the LibWeb, LibJS, and pinned site regression checks (Linux)"
    )
    gate.add_argument("--output", type=Path, required=True)
    gate.add_argument("--baseline", type=Path, default=HERE / "baseline.json")
    gate.add_argument("--cc", help="C compiler passed to Meta/ladybird.py")
    gate.add_argument("--cxx", help="C++ compiler passed to Meta/ladybird.py")
    gate.add_argument("--build-jobs", type=int)
    gate.add_argument("--test-jobs", type=int, default=6)
    gate.add_argument("--console-error-increase", type=int, default=5, help="Per-site increase that fails the gate")
    ci = commands.add_parser("ci", help="Run without building and emit the compact CI results")
    ci.add_argument("--ladybird", type=Path, required=True, help="Absolute path to Ladybird, next to WebDriver")
    ci.add_argument("--output", type=Path, required=True, help="Absolute path to the compact results JSON")
    args = parser.parse_args()
    if args.command == "ci":
        try:
            return run_ci(args.ladybird, args.output)
        except (OSError, ValueError) as error:
            ci.error(str(error))
    if args.command == "compare":
        try:
            changes = compare(json.loads(args.baseline.read_text()), json.loads(args.current.read_text()))
        except (KeyError, OSError, ValueError) as error:
            comparison.error(str(error))
        if args.json:
            print(json.dumps(changes, indent=2))
        else:
            print_comparison(changes)
        return int(any(change["direction"] == "regression" for change in changes))

    if args.command == "gate":
        if sys.platform != "linux":
            gate.error("The fix-batch gate currently supports Linux")
        if (
            args.test_jobs < 1
            or args.console_error_increase < 1
            or (args.build_jobs is not None and args.build_jobs < 1)
        ):
            gate.error("Require positive job counts and console error threshold")
        if connect is None:
            gate.error("Install `websockets` as pinned in requirements.txt")
        try:
            baseline = json.loads(args.baseline.read_text())
            source, sites = targets(None)
            if (
                baseline["schema_version"] != 1
                or baseline["site_list"] != source
                or [(row["rank"], row["url"]) for row in baseline["sites"]] != sites
                or baseline["summary"]["statuses"]["harness-error"]
                or baseline["settings"]["viewport"] != VIEWPORT
            ):
                raise ValueError(
                    "The gate needs a complete baseline for the pinned top-100 list without harness errors"
                )
            output = prepare_output(args.output)
        except (KeyError, OSError, ValueError) as error:
            gate.error(str(error))
        return run_gate(args, output, baseline)

    if args.jobs < 1 or not 0 < args.timeout < float("inf") or not 0 <= args.settle < float("inf"):
        run.error("Require positive --jobs and --timeout, and nonnegative --settle")
    for url in args.url or []:
        parsed = urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username is not None:
            run.error(f"Require an HTTP(S) URL without credentials: {url}")
    if args.url and len(set(args.url)) != len(args.url):
        run.error("Each --url must be unique")
    executable = args.webdriver.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        run.error(f"Not an executable: {executable}")
    if connect is None:
        run.error("Install `websockets` as pinned in requirements.txt")
    try:
        output = prepare_output(args.output)
    except (OSError, ValueError) as error:
        run.error(str(error))
    source, sites = targets(args.url)
    result = {
        "schema_version": 1,
        "revision": args.revision,
        "started_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "site_list": source,
        "settings": {
            "jobs": args.jobs,
            "navigation_timeout_s": args.timeout,
            "settle_s": args.settle,
            "viewport": VIEWPORT,
        },
        "sites": [],
    }
    processes = DriverProcesses()
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        try:
            for row in executor.map(
                lambda site: capture(executable, output, site, args.timeout, args.settle, processes), sites
            ):
                result["sites"].append(row)
                print(f"{row['rank']:3d} {row['status']:13} {str(row['load_ms']):>5} ms {row['url']}", flush=True)
        except KeyboardInterrupt:
            processes.cancel()
            executor.shutdown(cancel_futures=True)
            raise
    result["summary"] = summarize(result["sites"])
    results_file = output / "results.json"
    with open(results_file, "w", opener=open_private_file) as stream:
        stream.write(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], indent=2))
    print(f"Results: {results_file}")
    return int(result["summary"]["statuses"]["harness-error"] > 0)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, request_termination)
    try:
        raise SystemExit(main())
    except TerminationRequested:
        print("Site smoke terminated", file=sys.stderr)
        raise SystemExit(128 + signal.SIGTERM) from None
    except KeyboardInterrupt:
        print("Site smoke interrupted", file=sys.stderr)
        raise SystemExit(130) from None
