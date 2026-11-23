#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import csv
import subprocess
import sys

from concurrent.futures import ThreadPoolExecutor
from itertools import islice
from pathlib import Path

SNAPSHOT = "# Tranco list 8PW6V (2026-11-22): https://tranco-list.eu/list/8PW6V"
JOBS = 16
PROBE_TIMEOUT_SECONDS = 40


def probe(rank_and_domain):
    _, domain = rank_and_domain
    try:
        result = subprocess.run(
            [
                "curl",
                "--disable",
                "--silent",
                "--location",
                "--proto",
                "=https",
                "--proto-redir",
                "=http,https",
                "--max-redirs",
                "10",
                "--connect-timeout",
                "5",
                "--max-time",
                "12",
                "--retry",
                "2",
                "--retry-all-errors",
                "--output",
                "/dev/null",
                "--write-out",
                "%{http_code}\t%{content_type}",
                f"https://{domain}/",
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=PROBE_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return False
    status, _, content_type = result.stdout.partition("\t")
    return (
        result.returncode == 0 and status.startswith("2") and content_type.split(";")[0].strip().lower() == "text/html"
    )


def select_domains(rows, count=100):
    accepted = []
    with ThreadPoolExecutor(max_workers=JOBS) as executor:
        while batch := list(islice(rows, JOBS)):
            for row, browsable in zip(batch, executor.map(probe, batch)):
                if browsable:
                    accepted.append(row)
                    if len(accepted) == count:
                        return accepted
    raise ValueError(f"Only {len(accepted)} of {count} required domains qualified")


def main():
    parser = argparse.ArgumentParser(description="Select the first 100 browsable domains from Tranco snapshot 8PW6V")
    parser.add_argument("snapshot", type=Path, help="Unmodified CSV downloaded from Tranco's snapshot URL")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        with args.snapshot.open(newline="") as source:
            domains = select_domains(csv.reader(source))
        with args.output.open("w", newline="") as output:
            output.write(SNAPSHOT + "\n")
            writer = csv.writer(output, lineterminator="\n")
            writer.writerow(("rank", "domain"))
            writer.writerows(domains)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print(f"Selected {len(domains)} domains through Tranco rank {domains[-1][0]}: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
