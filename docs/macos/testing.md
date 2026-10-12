# Test gates

| Command | When | Result |
|---|---|---|
| `bash build.sh --test-keep-going` | **The gate** for every preview and merge request, on the exact tree to be shipped or merged | Every test runs, failures included. Prints `test-keep-going: N run, P passed, F failed, S skipped` and the failures. Exit 1 if any test failed; each exit is in `out/test-results.txt`. |
| `bash build.sh --test-dry` | Before merging a change to `build.sh` or a test target | Builds, links and signs everything `--test` runs, plus the export gate and wiring check. Runs no test, so it is not a gate on its own. |
| `bash build.sh --test-one <target or script>` | Iterating on one test | Builds only that target and runs it exactly as `--test` does. Exit is its first failure. |
| `bash build.sh --test` | Local runs | Stops at the first failure. |

- **What to report:** the command, the tree (commit or merge hash), the exit, the summary line and each failure. Never report only a rerun.
- **Paused runs:** if heavy.sh says the job was `PAUSED` for a perf run and a test failed, rerun that test once with `--test-one` and report both results.
- **Skips:** `BB_TEST_SKIP=<substring>` skips matching tests under `--test-keep-going` only. They are listed in the summary and in `out/test-results.txt`; say why in the report. A skip is not a pass.
- **What still stops a run:** a failing build or non-test step (compile, link, sign, the export gate, the wiring check) stops even `--test-keep-going`, with a non-zero exit and no summary.
- **Where it runs:** in a heavy.sh build slot (build.sh takes one itself). Run it as one background job with a timeout of at least 2 h, and don't edit the tree while it runs.
