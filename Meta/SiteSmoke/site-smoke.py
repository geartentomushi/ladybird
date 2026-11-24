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
import socket
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
SITE_LISTS = {path.stem.removeprefix("tranco-"): path for path in sorted(HERE.glob("tranco-top-*.csv"))}
DEFAULT_SITE_LIST = "top-100"


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


class ChromeReference:
    """Launches headless Chrome through ChromeDriver as a reference browser for comparisons."""

    def __init__(self, chromedriver, chrome):
        self.chromedriver = chromedriver
        self.chrome = chrome
        version = subprocess.run([chrome, "--version"], capture_output=True, text=True, check=True).stdout
        match = re.search(r"\b(\d+)\.\d+\.\d+\.\d+\b", version)
        if not match:
            raise DriverError(f"Cannot read the Chrome version from: {version.strip()}")
        self.user_agent = (
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
            f"Chrome/{match[1]}.0.0.0 Safari/537.36"
        )

    def command(self, port, profile):
        return [self.chromedriver, f"--port={port}", "--allowed-ips=127.0.0.1"]

    def capabilities(self, profile):
        return {
            "browserName": "chrome",
            "goog:chromeOptions": {
                "binary": str(self.chrome),
                "args": [
                    "--headless=new",
                    "--no-first-run",
                    "--no-default-browser-check",
                    "--disable-gpu",
                    "--hide-scrollbars",
                    f"--user-data-dir={profile}",
                    f"--user-agent={self.user_agent}",
                ],
            },
        }


def unused_port():
    with contextlib.closing(socket.socket()) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


@contextlib.contextmanager
def launch_driver(executable, directory, log_path, processes):
    with tempfile.TemporaryDirectory(prefix="profile-", dir=directory) as temporary:
        profile = Path(temporary) / "profile"
        with open(log_path, "wb", opener=open_private_file) as log:
            if isinstance(executable, ChromeReference):
                port = unused_port()
                command = executable.command(port, profile)
            else:
                port = None
                command = [executable, "--headless", "--force-cpu-painting", "--port", "0", "--profile-path", profile]
            process = processes.start(command, log)
            process.fixed_port = port
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
    if process.fixed_port is not None:
        while time.monotonic() < deadline and process.poll() is None:
            with contextlib.suppress(OSError), socket.create_connection(("127.0.0.1", process.fixed_port), 0.2):
                return process.fixed_port
            time.sleep(0.05)
        raise DriverError(f"WebDriver did not start (exit status: {process.poll()}); see {log_path}")
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


def open_session(port, extra_capabilities=None):
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=COMMAND_TIMEOUT)) as client:
        capabilities = {"webSocketUrl": True, **(extra_capabilities or {})}
        payload = json.dumps({"capabilities": {"alwaysMatch": capabilities}})
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
    params["expression"] = (
        "document.documentElement ? "
        "document.documentElement.scrollWidth + 'x' + document.documentElement.scrollHeight : ''"
    )
    with contextlib.suppress(*errors, ValueError):
        result = bidi.command("script.evaluate", params)
        if result["type"] == "success" and result["result"]["type"] == "string" and result["result"]["value"]:
            width, height = map(int, result["result"]["value"].split("x"))
            row["document_size"] = {"width": width, "height": height}
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
            endpoint = open_session(
                webdriver_port(process, log_path),
                executable.capabilities(profile) if isinstance(executable, ChromeReference) else None,
            )
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


def is_large_body_drop(before, after):
    drop = before - after
    return drop >= 5 and drop * 10 >= before * 3


def compare(baseline, current):
    if any(baseline[field] != current[field] for field in ("schema_version", "settings")):
        raise ValueError("Baseline and current run use different schema versions or capture settings")
    if "chrome" in (baseline.get("browser"), current.get("browser")):
        raise ValueError("Compare Chrome reference runs with `reference`, not as a baseline or candidate")
    old_sites = {row["url"]: row for row in baseline["sites"]}
    changes = []
    for site in current["sites"]:
        url = site["url"]
        before = old_sites.pop(url, None)
        if site["status"] == "harness-error":
            changes.append({"url": url, "metric": "harness_error", "direction": "regression"})
            continue
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
            if count > 0 and is_large_body_drop(old_count, count):
                changes.append(
                    {
                        "url": url,
                        "metric": "body_element_count",
                        "direction": "informational",
                        "before": old_count,
                        "after": count,
                        "confirmation": "pending",
                    }
                )
        if before["console_errors"] is not None and site["console_errors"] is not None:
            for category in ERROR_CATEGORIES:
                metrics[category] = (before["console_errors"][category], site["console_errors"][category], False)
        for metric, (old, new, higher_is_better) in metrics.items():
            if new != old:
                direction = "improvement" if (new > old) == higher_is_better else "regression"
                changes.append({"url": url, "metric": metric, "direction": direction, "before": old, "after": new})
    changes.extend({"url": url, "metric": "coverage", "direction": "removed"} for url in old_sites)
    return changes


def is_size_outlier(reference, value, factor=2):
    return reference > 0 and (value * factor < reference or value > reference * factor)


def reference_differences(reference, current):
    """List sites where Ladybird's result differs from a reference browser that loaded the site successfully."""
    if reference["settings"]["viewport"] != current["settings"]["viewport"]:
        raise ValueError("The reference and Ladybird runs use different viewports")
    reference_sites = {row["url"]: row for row in reference["sites"]}
    if set(reference_sites) != {row["url"] for row in current["sites"]}:
        raise ValueError("The reference and Ladybird runs use different targets")
    differences = []
    for site in current["sites"]:
        expected = reference_sites.get(site["url"])
        if expected is None or expected["status"] != "success" or site["status"] == "harness-error":
            continue

        def add(metric, before, after, site=site):
            differences.append(
                {"rank": site["rank"], "url": site["url"], "metric": metric, "reference": before, "ladybird": after}
            )

        if site["status"] != "success":
            add("status", "success", site["status"])
            continue
        expected_count, count = expected["body_element_count"], site["body_element_count"]
        if expected_count and count is not None and (count == 0 or is_large_body_drop(expected_count, count)):
            add("body_element_count", expected_count, count)
        expected_size, size = expected.get("document_size"), site.get("document_size")
        if expected_size and size:
            for dimension in ("width", "height"):
                if is_size_outlier(expected_size[dimension], size[dimension]):
                    add(f"document_{dimension}", expected_size[dimension], size[dimension])
        if expected["console_errors"] is not None and site["console_errors"] is not None:
            extra = sum(site["console_errors"].values()) - sum(expected["console_errors"].values())
            if extra >= 5:
                add(
                    "total_console_errors",
                    sum(expected["console_errors"].values()),
                    sum(site["console_errors"].values()),
                )
    return differences


def is_http_url(url):
    parsed = urlsplit(url)
    return parsed.scheme in ("http", "https") and parsed.hostname and parsed.username is None


def confirm_findings(changes, current, baseline_executable, executable, directory, console_threshold=5):
    pending = {}
    for change in changes:
        metric = change["metric"]
        rerunnable = metric in (
            *ERROR_CATEGORIES,
            "nonempty_body",
            "total_console_errors",
            "missing_screenshot",
            "missing_body_element_count",
        ) or (metric == "status" and change["after"] != "crash")
        if change.get("confirmation") == "pending" or (rerunnable and change["direction"] == "regression"):
            change["confirmation"] = "pending"
            pending.setdefault(change["url"], []).append(change)
    if not pending:
        return
    settings = current["settings"]
    timeout, settle = settings["navigation_timeout_s"], settings["settle_s"]
    if settings["viewport"] != VIEWPORT or not 0 < timeout < float("inf") or not 0 <= settle < float("inf"):
        raise ValueError("Invalid rerun capture settings")
    ranks = {row["url"]: row["rank"] for row in current["sites"]}
    for url in pending:
        if not is_http_url(url) or not isinstance(ranks[url], int) or ranks[url] < 1:
            raise ValueError("Invalid site in rerun")

    directory = prepare_output(directory)
    processes = DriverProcesses()
    crashed = {change["url"] for change in changes if change["metric"] == "status" and change["after"] == "crash"}
    try:
        for attempt in range(1, 3):
            attempt_directory = prepare_output(directory / f"attempt-{attempt}")
            baseline_directory = prepare_output(attempt_directory / "baseline")
            candidate_directory = prepare_output(attempt_directory / "candidate")
            for url, findings in pending.items():
                site = (ranks[url], url)
                pair = {}
                for label, driver, path in (
                    ("baseline", baseline_executable, baseline_directory),
                    ("candidate", executable, candidate_directory),
                ):
                    row = capture(driver, path, site, timeout, settle, processes)
                    pair[label] = {
                        key: row[key] for key in ("status", "body_element_count", "console_errors", "screenshot")
                    }
                if (
                    pair["candidate"]["status"] == "crash"
                    and pair["baseline"]["status"] != "crash"
                    and url not in crashed
                ):
                    changes.append(
                        {
                            "url": url,
                            "metric": "status",
                            "direction": "regression",
                            "before": pair["baseline"]["status"],
                            "after": "crash",
                            "reruns": [pair],
                        }
                    )
                    crashed.add(url)
                for change in findings:
                    change.setdefault("reruns", []).append(pair)
                    compare_rerun(change, pair, console_threshold)
            pending = {
                url: remaining
                for url, findings in pending.items()
                if (remaining := [change for change in findings if change["confirmation"] == "pending"])
            }
            if not pending:
                break
    except KeyboardInterrupt:
        processes.cancel()
        raise
    for url, findings in pending.items():
        for change in findings:
            metric = change["metric"]
            if any(rerun_value(metric, row) is None for pair in change["reruns"] for row in pair.values()):
                change["confirmation"] = "inconclusive"
                continue
            change["confirmation"] = "confirmed"
            if metric != "body_element_count":
                continue
            change["direction"] = "changed"
            if all(body_count(pair["candidate"]) == 0 for pair in change["reruns"]):
                changes.append(
                    {
                        "url": url,
                        "metric": "nonempty_body",
                        "direction": "regression",
                        "before": 1,
                        "after": 0,
                        "confirmation": "confirmed",
                    }
                )


def compare_rerun(change, pair, console_threshold):
    metric = change["metric"]
    before = 1 if metric == "status" or metric.startswith("missing_") else change["before"]
    baseline = rerun_value(metric, pair["baseline"])
    candidate = rerun_value(metric, pair["candidate"])

    def worse(old, new):
        if metric == "body_element_count":
            return is_large_body_drop(old, new)
        if metric in ("status", "nonempty_body") or metric.startswith("missing_"):
            return new < old
        return new - old >= (console_threshold if metric == "total_console_errors" else 1)

    if baseline is not None and worse(before, baseline):
        change["clear_reason"] = (
            "baseline-also-dropped" if metric == "body_element_count" else "baseline-also-regressed"
        )
    elif candidate is None or (worse(before, candidate) and (baseline is None or worse(baseline, candidate))):
        return
    change["confirmation"] = "cleared-on-rerun"
    change["direction"] = "informational"


def rerun_value(metric, row):
    if row["status"] == "harness-error":
        return None
    if metric == "status":
        return int(row["status"] == "success")
    if metric.startswith("missing_"):
        return int(row[metric.removeprefix("missing_")] is not None)
    if metric == "body_element_count":
        return body_count(row)
    if metric == "nonempty_body":
        count = body_count(row)
        return int(count > 0) if count is not None else None
    errors = row["console_errors"]
    if row["status"] != "success" or errors is None:
        return None
    return sum(errors.values()) if metric == "total_console_errors" else errors[metric]


def body_count(row):
    return row["body_element_count"] if row["status"] == "success" else None


def resolve_webdriver(path):
    path = path.resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"Not an executable: {path}")
    return path


def linked_ladybird_libraries(executable, environment):
    result = subprocess.run(
        ["ldd", executable], check=False, env=environment, capture_output=True, text=True, timeout=30
    )
    if result.returncode and result.stderr.strip() == "not a dynamic executable":
        with executable.open("rb") as file:
            if file.read(4) == b"\x7fELF":
                return {}
    if result.returncode or "=> not found" in result.stdout:
        raise ValueError(f"Cannot resolve libraries for {executable}: {(result.stderr or result.stdout).strip()}")
    libraries = {}
    for line in result.stdout.splitlines():
        match = re.fullmatch(r"\s*(liblagom-\S+\.so(?:\.\S+)?)\s+=>\s+(.+?)\s+\(0x[0-9a-fA-F]+\)", line)
        if match:
            libraries[match[1]] = Path(match[2]).resolve(strict=True)
    return libraries


def build_library_paths(driver):
    driver = resolve_webdriver(driver)
    environment = os.environ | {"LC_ALL": "C"}
    # LibWebView's helper-process environment does not forward dynamic-loader overrides.
    helper_environment = {name: value for name, value in environment.items() if not name.startswith("LD_")}
    libexec = driver.parent.parent / "libexec"
    for name in ("Ladybird", "WebContent"):
        for directory in (libexec, driver.parent):
            executable = directory / name
            if executable.is_file() and os.access(executable, os.X_OK):
                break
        else:
            raise ValueError(f"Cannot find {name} alongside {driver} or in {libexec}")
        libraries = linked_ladybird_libraries(
            executable.resolve(), helper_environment if name == "WebContent" else environment
        )
        yield from libraries.items()
    yield from linked_ladybird_libraries(driver, environment).items()


def validate_build_pair(baseline, candidate):
    if sys.platform != "linux":
        raise ValueError("Shared-library checks for paired builds currently support Linux only")

    def libraries_by_identity(driver):
        libraries = {}
        for name, path in build_library_paths(driver):
            info = path.stat()
            libraries[(info.st_dev, info.st_ino)] = (name, path)
        return libraries

    guidance = "See Meta/SiteSmoke/README.md#prepare-a-standalone-linux-baseline."
    try:
        baseline, candidate = resolve_webdriver(baseline), resolve_webdriver(candidate)
        same_driver = baseline.samefile(candidate)
        before, after = ({}, {}) if same_driver else (libraries_by_identity(baseline), libraries_by_identity(candidate))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        raise ValueError(
            f"Cannot verify separate Ladybird builds: {error}. Use built WebDriver binaries, not wrappers. {guidance}"
        ) from error
    if same_driver:
        raise ValueError(
            f"Contaminated SiteSmoke baseline: both WebDriver paths identify the same executable "
            f"(baseline: {baseline}; candidate: {candidate}). {guidance}"
        )
    for identity, (name, path) in before.items():
        if identity in after:
            candidate_path = after[identity][1]
            raise ValueError(
                f"Contaminated SiteSmoke baseline: both builds resolve {name} to the same file "
                f"(baseline: {path}; candidate: {candidate_path}). "
                f"A copied build can retain an absolute RUNPATH; WebContent does not inherit LD_LIBRARY_PATH. {guidance}"
            )


def targets(urls, site_list=DEFAULT_SITE_LIST):
    if urls:
        return "custom", list(enumerate(urls, start=1))
    with SITE_LISTS[site_list].open(newline="") as snapshot:
        source = snapshot.readline().removeprefix("# ").strip()
        return source, [(int(row["rank"]), f"https://{row['domain']}/") for row in csv.DictReader(snapshot)]


def print_comparison(changes):
    counts = Counter(change["direction"] for change in changes)
    print(
        "Changes:",
        ", ".join(
            f"{key}={counts[key]}"
            for key in ("regression", "improvement", "changed", "added", "removed", "informational")
        ),
    )
    for change in changes:
        delta = f": {change['before']} → {change['after']}" if "before" in change else ""
        confirmation = f" ({change['confirmation']})" if "confirmation" in change else ""
        print(f"{change['direction']:13} {change['url']} {change['metric']}{delta}{confirmation}")


def gate_regressions(baseline, current, console_threshold, *, changes=None):
    if changes is None:
        changes = compare(baseline, current) + gate_evidence_findings(baseline, current, console_threshold)
    regressions = [
        change
        for change in changes
        if (
            change["direction"] == "regression"
            and change["metric"]
            in (
                "status",
                "harness_error",
                "nonempty_body",
                "missing_screenshot",
                "missing_body_element_count",
                "total_console_errors",
            )
        )
        or change["direction"] in ("added", "removed")
    ]
    return changes, regressions


def gate_evidence_findings(baseline, current, console_threshold):
    findings = []
    old_sites = {row["url"]: row for row in baseline["sites"]}
    for site in current["sites"]:
        before = old_sites.get(site["url"])
        if before is None or site["status"] == "harness-error":
            continue
        for metric in ("screenshot", "body_element_count"):
            if before[metric] is not None and site[metric] is None:
                findings.append({"url": site["url"], "metric": f"missing_{metric}", "direction": "regression"})
        if before["status"] != "success" or site["status"] != "success":
            continue
        if before["console_errors"] is not None and site["console_errors"] is not None:
            old_count = sum(before["console_errors"].values())
            count = sum(site["console_errors"].values())
            if count - old_count >= console_threshold:
                findings.append(
                    {
                        "url": site["url"],
                        "metric": "total_console_errors",
                        "direction": "regression",
                        "before": old_count,
                        "after": count,
                    }
                )
    return findings


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


def run_ci(ladybird, output, site_list=DEFAULT_SITE_LIST, baseline_webdriver=None):
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
    if baseline_webdriver is not None:
        validate_build_pair(baseline_webdriver, driver)
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
            "--list",
            site_list,
        ],
    )
    if status not in (0, 1):
        raise ValueError(f"Site smoke harness exited {status}; see {details}")
    _, sites = targets(None, site_list)
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
    if args.baseline_webdriver is not None:
        try:
            validate_build_pair(args.baseline_webdriver, build / "bin/WebDriver")
        except ValueError as error:
            print(error, file=sys.stderr)
            report["stages"]["build-pair"] = {"exit_status": 1, "error": str(error)}
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
            "--list",
            args.list,
        ],
    )
    try:
        current = json.loads((sites_dir / "results.json").read_text())
        changes = compare(baseline, current) + gate_evidence_findings(baseline, current, args.console_error_increase)
        if args.baseline_webdriver is not None:
            confirm_findings(
                changes,
                current,
                args.baseline_webdriver,
                build / "bin/WebDriver",
                output / "site-reruns",
                args.console_error_increase,
            )
        changes, regressions = gate_regressions(baseline, current, args.console_error_increase, changes=changes)
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
    driver = run.add_mutually_exclusive_group(required=True)
    driver.add_argument("--webdriver", type=Path, help="Built Ladybird WebDriver executable")
    driver.add_argument("--chromedriver", type=Path, help="ChromeDriver for a headless Chrome reference run")
    run.add_argument("--chrome", type=Path, help="Chrome executable matching --chromedriver")
    run.add_argument("--revision", required=True, help="Revision or build label for the supplied executable")
    run.add_argument(
        "--output", type=Path, required=True, help="New directory for results, screenshots and driver logs"
    )
    run.add_argument("--url", action="append", help="Use these URLs instead of the pinned list (repeatable)")
    run.add_argument("--list", choices=SITE_LISTS, default=DEFAULT_SITE_LIST, help="Pinned site list to run")
    run.add_argument("--jobs", type=int, default=4)
    run.add_argument("--timeout", type=float, default=25, help="Per-site navigation deadline in seconds")
    run.add_argument("--settle", type=float, default=2, help="Seconds to observe before taking each screenshot")
    comparison = commands.add_parser("compare", help="Compare two results files")
    comparison.add_argument("baseline", type=Path)
    comparison.add_argument("current", type=Path)
    comparison.add_argument("--json", action="store_true", help="Print the comparison as JSON")
    comparison.add_argument("--webdriver", type=Path, help="Candidate WebDriver for paired reruns")
    comparison.add_argument("--baseline-webdriver", type=Path, help="Baseline WebDriver for paired reruns")
    comparison.add_argument("--rerun-output", type=Path, help="New directory for paired rerun artifacts")
    gate = commands.add_parser(
        "gate", help="Build and run the LibWeb, LibJS, and pinned site regression checks (Linux)"
    )
    gate.add_argument("--output", type=Path, required=True)
    gate.add_argument("--list", choices=SITE_LISTS, default=DEFAULT_SITE_LIST, help="Pinned site list to run")
    gate.add_argument("--baseline", type=Path, help="Baseline results (default: the pinned list's baseline)")
    gate.add_argument("--baseline-webdriver", type=Path, help="Baseline WebDriver for paired reruns")
    gate.add_argument("--cc", help="C compiler passed to Meta/ladybird.py")
    gate.add_argument("--cxx", help="C++ compiler passed to Meta/ladybird.py")
    gate.add_argument("--build-jobs", type=int)
    gate.add_argument("--test-jobs", type=int, default=6)
    gate.add_argument("--console-error-increase", type=int, default=5, help="Per-site increase that fails the gate")
    ci = commands.add_parser("ci", help="Run without building and emit the compact CI results")
    ci.add_argument("--ladybird", type=Path, required=True, help="Absolute path to Ladybird, next to WebDriver")
    ci.add_argument(
        "--baseline-webdriver", type=Path, help="Verify independent libraries from this baseline build (Linux)"
    )
    ci.add_argument("--output", type=Path, required=True, help="Absolute path to the compact results JSON")
    ci.add_argument("--list", choices=SITE_LISTS, default=DEFAULT_SITE_LIST, help="Pinned site list to run")
    reference = commands.add_parser("reference", help="List differences from a reference browser run")
    reference.add_argument("reference", type=Path, help="Results from a --chromedriver run")
    reference.add_argument("current", type=Path, help="Ladybird results for the same sites")
    reference.add_argument("--json", action="store_true", help="Print the differences as JSON")
    args = parser.parse_args()
    if args.command == "reference":
        try:
            differences = reference_differences(
                json.loads(args.reference.read_text()), json.loads(args.current.read_text())
            )
        except (KeyError, OSError, ValueError) as error:
            reference.error(str(error))
        if args.json:
            print(json.dumps(differences, indent=2))
        else:
            print(f"Differences: {len(differences)}")
            for difference in differences:
                print(
                    f"{difference['rank']:3d} {difference['url']} {difference['metric']}: "
                    f"{difference['reference']} → {difference['ladybird']}"
                )
        return 0
    if args.command == "ci":
        try:
            return run_ci(args.ladybird, args.output, args.list, args.baseline_webdriver)
        except (OSError, ValueError) as error:
            ci.error(str(error))
    if args.command == "compare":
        rerun_options = (args.webdriver, args.baseline_webdriver, args.rerun_output)
        if any(rerun_options) and not all(rerun_options):
            comparison.error("--webdriver, --baseline-webdriver and --rerun-output must be supplied together")
        try:
            current = json.loads(args.current.read_text())
            baseline = json.loads(args.baseline.read_text())
            changes = compare(baseline, current) + gate_evidence_findings(baseline, current, 5)
            if args.webdriver is not None:
                if sys.platform == "linux":
                    validate_build_pair(args.baseline_webdriver, args.webdriver)
                confirm_findings(
                    changes,
                    current,
                    resolve_webdriver(args.baseline_webdriver),
                    resolve_webdriver(args.webdriver),
                    args.rerun_output,
                )
        except (DriverError, KeyError, OSError, ValueError) as error:
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
            if args.baseline_webdriver is not None:
                args.baseline_webdriver = resolve_webdriver(args.baseline_webdriver)
                candidate = ROOT / "Build/release/bin/WebDriver"
                if candidate.exists():
                    validate_build_pair(args.baseline_webdriver, candidate)
            if args.baseline is None:
                name = "baseline.json" if args.list == DEFAULT_SITE_LIST else f"baseline-{args.list}.json"
                args.baseline = HERE / name
            baseline = json.loads(args.baseline.read_text())
            source, sites = targets(None, args.list)
            if (
                baseline["schema_version"] != 1
                or baseline["site_list"] != source
                or [(row["rank"], row["url"]) for row in baseline["sites"]] != sites
                or baseline["summary"]["statuses"]["harness-error"]
                or baseline["settings"]["viewport"] != VIEWPORT
            ):
                raise ValueError(
                    f"The gate needs a complete baseline for the pinned {args.list} list without harness errors"
                )
            output = prepare_output(args.output)
        except (KeyError, OSError, ValueError) as error:
            gate.error(str(error))
        return run_gate(args, output, baseline)

    if args.jobs < 1 or not 0 < args.timeout < float("inf") or not 0 <= args.settle < float("inf"):
        run.error("Require positive --jobs and --timeout, and nonnegative --settle")
    for url in args.url or []:
        if not is_http_url(url):
            run.error(f"Require an HTTP(S) URL without credentials: {url}")
    if args.url and len(set(args.url)) != len(args.url):
        run.error("Each --url must be unique")
    if (args.chrome is None) != (args.chromedriver is None):
        run.error("--chrome and --chromedriver must be supplied together")
    try:
        if args.chromedriver is not None:
            executable = ChromeReference(resolve_webdriver(args.chromedriver), resolve_webdriver(args.chrome))
        else:
            executable = resolve_webdriver(args.webdriver)
    except (DriverError, OSError, ValueError, subprocess.CalledProcessError) as error:
        run.error(str(error))
    if connect is None:
        run.error("Install `websockets` as pinned in requirements.txt")
    try:
        output = prepare_output(args.output)
    except (OSError, ValueError) as error:
        run.error(str(error))
    source, sites = targets(args.url, args.list)
    result = {
        "schema_version": 1,
        "revision": args.revision,
        "browser": "chrome" if isinstance(executable, ChromeReference) else "ladybird",
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
