# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import html
import json
import os

from pathlib import Path

PASS_STATUSES = {"wpt": {"OK", "PASS"}, "test262": {"PASSED"}, "sites": {"PASS"}}
SKIP_STATUSES = {"NOTRUN", "SKIP", "SKIPPED"}
SITE_BASELINE_STATUSES = {
    "success": "PASS",
    "timeout": "FAIL",
    "crash": "FAIL",
    "load-error": "FAIL",
    "harness-error": "SKIP",
}


def wpt_statuses(reports: Path) -> dict[str, str]:
    statuses: dict[str, str] = {}
    shards = sorted(reports.glob("wpt-*.json"))
    if not shards:
        raise ValueError("No WPT shard reports")
    for shard in shards:
        with shard.open(encoding="utf-8") as stream:
            records = 0
            for line in stream:
                report = json.loads(line)
                for test in report["results"]:
                    key = [test.get("subsuite", ""), test["test"]]
                    if test["status"]:
                        statuses[json.dumps(key)] = test["status"]
                        records += 1
                    for subtest in test["subtests"]:
                        statuses[json.dumps([*key, subtest["name"]])] = subtest["status"]
            if not records:
                raise ValueError(f"No completed WPT tests in {shard.name}")
    return statuses


def read_statuses(path: Path, key: str = "results") -> dict[str, str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    statuses = data[key] if key else data
    if not isinstance(statuses, dict) or not statuses:
        raise ValueError(f"No per-test statuses in {path}")
    if not all(isinstance(status, str) and status for status in statuses.values()):
        raise ValueError(f"Invalid per-test status in {path}")
    return statuses


def regressions(suite: str, previous: dict[str, str], current: dict[str, str]) -> list[dict[str, str]]:
    passing = PASS_STATUSES[suite]
    return [
        {"test": test, "before": previous[test], "after": status}
        for test, status in sorted(current.items())
        if previous.get(test) in passing and status not in passing and status not in SKIP_STATUSES
    ]


def summarize(
    baseline: Path,
    reports: Path,
    output: Path,
    run_id: str,
    results: dict[str, str],
    revisions: dict[str, str],
    site_baseline: Path | None = None,
) -> tuple[str, int, int, int]:
    output.mkdir(parents=True, exist_ok=True)
    previous_label = f"nightly run {run_id}" if run_id else "none (first retained run)"
    lines = ["# Nightly compatibility", "", f"Comparison baseline: {previous_label}", ""]
    baseline_report = baseline / "regressions.json"
    previous_revisions = (
        json.loads(baseline_report.read_text(encoding="utf-8")).get("revisions", {})
        if baseline_report.is_file()
        else {}
    )
    for name, revision in revisions.items():
        lines.append(f"- {name} revision: <code>{html.escape(revision or 'unavailable')}</code>")
    lines.append("")
    suites = {}
    hard_count = soft_count = errors = 0
    for suite, result in results.items():
        detail: dict = {"result": result, "regressions": []}
        suites[suite] = detail
        lines.append(f"## {suite}")
        if suite == "sites" and result == "skipped":
            lines.extend(["Top-100 sites harness not available or build did not finish; no site comparison.", ""])
            continue
        if result != "success":
            errors += 1
            lines.extend([f"Suite did not complete ({result}). See the job logs.", ""])
            continue

        try:
            if suite == "wpt":
                current = wpt_statuses(reports)
            else:
                path = reports / ("sites/sites.json" if suite == "sites" else "test262.json")
                current = read_statuses(path)
            previous_path = baseline / f"{suite}-statuses.json"
            found_baseline = previous_path.is_file()
            previous = read_statuses(previous_path, key="") if found_baseline else {}
            if suite == "sites" and not found_baseline and site_baseline and site_baseline.is_file():
                rows = json.loads(site_baseline.read_text(encoding="utf-8"))["sites"]
                previous = {row["url"]: SITE_BASELINE_STATUSES[row["status"]] for row in rows}
                found_baseline = True
                detail["baseline_source"] = "committed"
                lines.extend(["Comparison baseline: committed top-sites baseline.", ""])
        except (OSError, ValueError, TypeError, KeyError) as error:
            errors += 1
            detail["result"] = "invalid-report"
            detail["error"] = str(error)
            lines.extend([f"Could not read report: <code>{html.escape(str(error))}</code>", ""])
            continue

        (output / f"{suite}-statuses.json").write_text(json.dumps(current), encoding="utf-8")
        new = regressions(suite, previous, current)
        detail.update(compared=found_baseline, tests=len(current), regressions=new)
        if not found_baseline:
            lines.extend([f"Recorded first baseline ({len(current):,} results); no comparison yet.", ""])
            continue

        soft = suite == "sites"
        if soft:
            soft_count += len(new)
        else:
            hard_count += len(new)
        label = "site regressions (advisory)" if soft else "new regressions"
        previous_revision = previous_revisions.get(suite)
        current_revision = revisions.get(suite)
        if new and previous_revision and current_revision and previous_revision != current_revision:
            detail["suite_revision_change"] = {"before": previous_revision, "after": current_revision}
            label += f" — test suite changed (rev {html.escape(previous_revision)}→{html.escape(current_revision)})"
        lines.extend([f"{len(current):,} results; **{len(new):,} {label}**.", ""])
        for entry in new[:25]:
            lines.append(
                f"- <code>{html.escape(entry['test'])}</code>: "
                f"{html.escape(entry['before'])} → {html.escape(entry['after'])}"
            )
        if len(new) > 25:
            lines.append("- See regressions.json for the complete list.")
        lines.append("")

    summary = "\n".join(lines)
    (output / "nightly-summary.md").write_text(summary, encoding="utf-8")
    (output / "regressions.json").write_text(
        json.dumps(
            {
                "baseline_run": run_id or None,
                "revisions": revisions,
                "baseline_revisions": previous_revisions,
                "suites": suites,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return summary, hard_count, soft_count, errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize nightly compatibility regressions.")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-run", default="")
    parser.add_argument("--site-baseline", type=Path)
    for suite in PASS_STATUSES:
        parser.add_argument(f"--{suite}", required=True)
    for name in ("browser", "wpt", "test262"):
        parser.add_argument(f"--{name}-revision", required=True)
    args = parser.parse_args()

    summary, hard, soft, errors = summarize(
        args.baseline,
        args.reports,
        args.output,
        args.baseline_run,
        {suite: getattr(args, suite) for suite in PASS_STATUSES},
        {name: getattr(args, f"{name}_revision") for name in ("browser", "wpt", "test262")},
        args.site_baseline,
    )
    if summary_path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary_path, "a", encoding="utf-8") as stream:
            stream.write(summary)
    else:
        print(summary)
    if hard:
        print(f"::error::{hard} compatibility regressions; see the nightly summary and regressions.json.")
    if errors:
        print(f"::error::{errors} suites did not yield valid results; see the nightly summary.")
    if soft:
        print(f"::warning::{soft} top-site regressions; live-site flips are advisory.")
    return int(bool(hard or errors))


if __name__ == "__main__":
    raise SystemExit(main())
