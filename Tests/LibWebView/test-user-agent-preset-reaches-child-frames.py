#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import queue
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
DevToolsClient = devtools["DevToolsClient"]
helpers = devtools["helpers"]
stop = devtools["stop"]
wait_until = devtools["wait_until"]
TIMEOUT_SECONDS = helpers["EVENT_TIMEOUT_SECONDS"]

# The port the test server listens on, for the child frame's cross-site URL.
server_port = 0

COLLECT_USER_AGENT_DATA = """
    async function collectUserAgentData() {
        const data = navigator.userAgentData;
        const hints = await data.getHighEntropyValues([
            "architecture", "bitness", "formFactors", "fullVersionList", "model", "platformVersion", "uaFullVersion", "wow64"
        ]);
        return { lowEntropy: data.toJSON(), brands: data.brands, hints };
    }
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if self.path in ("/parent", "/child"):
            self.server.requests.put((self.path, dict(self.headers)))
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.path == "/child":
            # The child frame reports what its own process answers, and what it sends on the wire.
            body = f"""<!doctype html><title>/child</title><script>
                {COLLECT_USER_AGENT_DATA}
                parent.postMessage(JSON.stringify({{ userAgent: navigator.userAgent, languages: navigator.languages }}), "*");
                collectUserAgentData().then(data => parent.postMessage(JSON.stringify({{ userAgentData: data }}), "*"));
            </script>"""
        else:
            body = f"""<!doctype html><title>/parent</title><script>
                {COLLECT_USER_AGENT_DATA}
                window.child = null;
                window.childUserAgentData = null;
                window.parentUserAgentData = null;
                addEventListener("message", event => {{
                    const payload = JSON.parse(event.data);
                    if (payload.userAgentData)
                        window.childUserAgentData = payload.userAgentData;
                    else
                        window.child = payload;
                }});
                collectUserAgentData().then(data => window.parentUserAgentData = data);
            </script><iframe src="http://localhost:{server_port}/child"></iframe>"""
        self.wfile.write(body.encode())

    def log_message(self, format, *args):
        pass


def launch(ladybird_binary, profile, url, preset):
    deadline = time.monotonic() + TIMEOUT_SECONDS
    for _ in range(5):
        if time.monotonic() >= deadline:
            break
        devtools_port = helpers["unused_port"]()
        process = subprocess.Popen(
            [
                ladybird_binary,
                "--headless=manual",
                f"--profile-path={profile}",
                f"--devtools={devtools_port}",
                *([f"--user-agent-preset={preset}"] if preset else []),
                url,
            ],
            stderr=subprocess.PIPE,
            text=True,
        )
        bind_failed = threading.Event()

        def forward_stderr(stderr=process.stderr, bind_failed=bind_failed):
            assert stderr is not None
            for line in stderr:
                if line.startswith("Unable to launch devtools server"):
                    bind_failed.set()
                sys.stderr.write(line)

        threading.Thread(target=forward_stderr, daemon=True).start()

        while not bind_failed.is_set() and process.poll() is None and time.monotonic() < deadline:
            try:
                attempt_deadline = min(deadline, time.monotonic() + 1)
                return process, DevToolsClient(devtools_port, deadline=attempt_deadline)
            except OSError:
                pass
        stop(process)
    raise AssertionError("The DevTools server didn't start")


def test_user_agent_preset_reaches_child_frames(ladybird_binary, profile, server, preset, expected_product):
    parent_url = f"http://127.0.0.1:{server.server_port}/parent"
    process, client = launch(ladybird_binary, profile, parent_url, preset)
    try:
        wait_until("the tab shows the parent", lambda: client.tab().get("title") == "/parent")
        client.watch_frame_targets()

        parent = json.loads(
            client.evaluate("JSON.stringify({ userAgent: navigator.userAgent, languages: navigator.languages })")
        )
        if preset:
            assert "Ladybird" not in parent["userAgent"], parent
        assert expected_product in parent["userAgent"], parent

        # The child frame is cross-site, so another process hosts it: it has to answer with the same spoofed
        # User-Agent and the same languages as the page that frames it.
        wait_until("the child frame reports", lambda: client.evaluate("window.child !== null"))
        child = json.loads(client.evaluate("JSON.stringify(window.child)"))
        assert child == parent, (child, parent)

        wait_until(
            "both frames report JavaScript UA Client Hints",
            lambda: client.evaluate("window.parentUserAgentData !== null && window.childUserAgentData !== null"),
        )
        parent_data, child_data = json.loads(
            client.evaluate("JSON.stringify([window.parentUserAgentData, window.childUserAgentData])")
        )
        assert parent_data == child_data, (parent_data, child_data)
        if preset:
            expected = {"brands": [], "mobile": False, "platform": "Linux" if expected_product == "Firefox/" else "macOS"}
            assert parent_data == {"lowEntropy": expected, "brands": [], "hints": expected}, parent_data
        else:
            assert any(brand["brand"] == "Ladybird" for brand in parent_data["brands"]), parent_data
            assert {"architecture", "bitness"} <= parent_data["hints"].keys(), parent_data
        print(f"PASS: {preset or 'Default'}: JavaScript UA Client Hints agree in both frames")

        requests = dict(server.requests.get(timeout=TIMEOUT_SECONDS) for _ in range(2))
        assert requests.keys() == {"/parent", "/child"}, requests
        for path, request in requests.items():
            headers = {name.lower(): value for name, value in request.items()}
            assert headers["user-agent"] == parent["userAgent"], (path, headers, parent)
            client_hints = {name for name in headers if name.startswith("sec-ch-ua")}
            if preset:
                assert not client_hints, (path, client_hints)
            else:
                assert {"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"} <= client_hints, (path, client_hints)

        print("PASS: A child frame in another process gets the tab's User-Agent preset and languages")
        print(f"PASS: {preset or 'Default'}: UA Client Hints {'absent' if preset else 'present'} on parent and child requests")
    finally:
        client.close()
        stop(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    global server_port
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.requests = queue.Queue()
    server_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "profile"
            test_user_agent_preset_reaches_child_frames(ladybird_binary, profile, server, "Firefox Linux Desktop", "Firefox/")
            test_user_agent_preset_reaches_child_frames(ladybird_binary, profile, server, "Safari macOS Desktop", "Safari/")
            # Relaunch the same profile without a CLI preset to reset to Ladybird's default.
            test_user_agent_preset_reaches_child_frames(ladybird_binary, profile, server, None, "Ladybird/")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
