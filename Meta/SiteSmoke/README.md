# Real-site smoke tests

Build Ladybird with WebDriver, then run from the checkout root on Linux or macOS
with Python 3.9+ and the `websockets` version pinned in `requirements.txt`.
Use a new output directory outside the checkout for each run:

```sh
python3 Meta/SiteSmoke/site-smoke.py run \
    --webdriver Build/release/bin/WebDriver --revision "$(git rev-parse HEAD)" \
    --output "$HOME/ladybird-site-smoke"
python3 Meta/SiteSmoke/site-smoke.py compare \
    Meta/SiteSmoke/baseline.json "$HOME/ladybird-site-smoke/results.json"
```

On macOS, use `--webdriver Build/release/bin/Ladybird.app/Contents/MacOS/WebDriver`
instead. The `Ladybird` executable is in the same bundle directory.

The default targets are the first 100 browsable domains from Tranco list
[8PW6V][tranco], dated 2026-11-22, in original rank order. The filter uses
`curl` (not Ladybird) to fetch each HTTPS homepage with TLS verification and up
to two retries for transfer failures. It follows redirects and keeps only final
2xx responses with a `text/html` content type. Regenerate the list with:

```sh
curl --fail --location https://tranco-list.eu/download/8PW6V/1000000 -o /tmp/tranco-8PW6V.csv
python3 Meta/SiteSmoke/filter-tranco.py /tmp/tranco-8PW6V.csv --output Meta/SiteSmoke/tranco-top-100.csv
```

Live DNS, TLS, and HTTP responses can change; use the committed list for
comparisons. After regenerating the list, rerun the smoke test from the source
build on the new list, replace `baseline.json` with that run's `results.json`,
and commit the list and baseline together. The gate rejects a baseline whose
targets do not match the pinned list.

For a smaller run, repeat `--url https://example.com/` with the URLs you want.
`--jobs`, `--timeout`, and `--settle` control concurrency, the navigation
deadline and the wait after navigation, respectively. Use the same settings for
comparisons.

Each site gets a fresh temporary profile with CPU painting and a 1280×800
viewport. `results.json` includes a summary and one row per target. Load time
measures the BiDi navigation to the load event (or elapsed time to failure),
not browser startup or screenshot capture. HTTP errors observed on the main
navigation and failed navigations are `load-error`; driver startup and session
setup failures are `harness-error`. The run exits nonzero for harness errors.
Crashes are detected from browser crash reports, WebDriver crash messages, or
driver exit during capture. Redirect URLs are recorded without queries or fragments.
The tool attempts to save a numbered PNG and count elements under the document
body even on unsuccessful loads when the page still responds.

Console counts cover BiDi error-level events observed through screenshot capture,
separated into not implemented, other JavaScript exceptions, and other console
errors. Native engine diagnostics are not console events; inspect the numbered
driver logs for those. A successful load or nonempty body does not prove the
site is usable: a consent prompt, verification challenge or error page can also
render content.
The harness keeps output directories and files private to the owner (0700 and 0600).

`compare` reports per-site load-status changes and, for successful loads,
changes in console-error counts and whether the body is nonempty. If the body
remains nonempty but loses at least five elements and at least 30% of its
baseline count, the gate can take up to two pairs of fresh captures, alternating
baseline and candidate builds. Both pairs must still show a drop of that size;
if the baseline also drops by that threshold from its original count, the site
changed and the finding is cleared. The drop does not block the gate unless
both candidate reruns show an empty body.

The same paired captures check new timeouts and load errors, empty bodies,
lost screenshots or DOM evidence, and console increases. The candidate must
still fail the original check in both pairs while the baseline remains good.
For console counts, the increase must also meet the same threshold against
each paired baseline. A recovered candidate or a baseline that also fails the
check clears that finding; it stays in the report as informational. Raw
improvements and coverage changes are not rerun. An observed new crash still
fails the gate. A missing rerun measurement is marked inconclusive unless
another pair clears the finding; an inconclusive blocking finding still fails
the gate.

For the gate, pass `--baseline-webdriver /path/to/baseline/WebDriver`. For
standalone comparisons, also pass `--webdriver /path/to/candidate/WebDriver`
and `--rerun-output /path/to/new-directory`. Use the binaries that produced the
original results, kept in separate build directories. Without the binaries,
the original changes are reported as measured and suspected body-count drops
are informational and marked pending.
The comparison exits 1 if any reported change is a regression. Use `--json` to get the same
differences as JSON. Timings are saved but not graded because
network latency and server behavior vary. Inspect screenshots and repeat a run
before attributing changes to the engine. Commit only the baseline JSON, not the
screenshots or logs.

## Gate a batch of fixes

On Linux, use the same build prerequisites as `Meta/ladybird.py`, then run:

```sh
python3 Meta/SiteSmoke/site-smoke.py gate --output "$HOME/ladybird-fix-gate"
```

The gate builds the Release browser and test targets, checks LibWeb Text, Layout
and Ref and the configured LibJS CTest suite, then runs the pinned list with the
baseline's settings. It writes `gate-summary.json` and stage logs in the output
directory. Use `--baseline` to compare with a different complete Tranco baseline,
or `--cc`, `--cxx`, `--build-jobs`, and `--test-jobs` to control the build and
test environment. It runs standalone, not as a CI workflow.

On Linux, use a system FFmpeg with H.264/AAC support and a libavcodec major
supported by Ladybird (currently 60–63). The bundled decoders alone do not
satisfy the media expectations in the Text suite.

The gate exits nonzero on build or test failure, missing results, or a site
regression: a new crash, a previously successful load that no longer succeeds,
lost screenshot or DOM evidence, a previously nonempty body becoming empty,
or a per-site rise of at least five console errors on an otherwise successful
load (configurable with `--console-error-increase`). Smaller changes still appear
in the report. These are objective load/render signals; the gate does not
automatically recognize a visually broken page or a challenge page that returns
HTTP 200. Review its screenshots and rerun transient internet failures.

## Build-free interface for CI

```sh
Meta/run-top-sites-smoke.sh --ladybird "$PWD/Build/release/bin/Ladybird" \
    --output "$HOME/site-smoke-results.json"
```

Both paths must be absolute; the built `WebDriver` must be next to `Ladybird`.
The wrapper does not build, download dependencies, or use recorded pages. It
needs Python 3.9+, the pinned `websockets` package, and outbound access to the
live sites. Four sites run concurrently, with a 25-second navigation deadline
and two-second settle time per site. Allow several minutes; widespread
timeouts take longer.

The output is `{"results":{"https://example.com/":"PASS"}}`. A successful load
is `PASS`; a timeout, crash, or load error is `FAIL`; an isolated setup error
is `SKIP`. Site failures do not change the exit status. Invalid inputs,
incomplete output, or failure to set up any site exit nonzero. The full
`results.json`, screenshots and logs go in the sibling `<output>.details/`
directory; use a new output filename or an empty details directory for each run.

[tranco]: https://tranco-list.eu/list/8PW6V
