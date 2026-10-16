# Test gates

| Command | When | Result |
|---|---|---|
| `bash build.sh --test-keep-going` | **The gate** for every preview and merge request, on the exact tree to be shipped or merged | Every test runs, failures included. Prints `test-keep-going: N run, P passed, F failed, S skipped` and the failures. Exit 1 if any test failed, 3 if any was skipped (`NOT A FULL GATE`), 0 only when all ran and passed; each exit is in `out/test-results.txt`. |
| `bash build.sh --test-dry` | Before merging a change to `build.sh` or a test target | Builds, links and signs everything `--test` runs, plus the export gate and wiring check. Runs no test, so it is not a gate on its own. |
| `bash build.sh --test-one <target or script>` | Iterating on one test | Builds only that target and runs it exactly as `--test` does. Exit is its first failure. |
| `bash build.sh --test` | Local runs | Stops at the first failure. |

- **What to report:** the command, the tree (commit or merge hash), the exit, the summary line and each failure. Never report only a rerun.
- **Paused runs:** if heavy.sh says the job was `PAUSED` for a perf run and a test failed, rerun that test once with `--test-one` and report both results.
- **Skips:** `BB_TEST_SKIP=<substring>` skips matching tests under `--test-keep-going` only. They are listed in the summary and in `out/test-results.txt`. A run with skips exits 3 and prints `NOT A FULL GATE`: it never passes as the gate. A merge request or preview must list each skip and say why.
- **What still stops a run:** a failing build or non-test step (compile, link, sign, the export gate, the wiring check) stops even `--test-keep-going`, with a non-zero exit and no summary.
- **Where it runs:** in a heavy.sh build slot (build.sh takes one itself). Run it as one background job with a timeout of at least 2 h, and don't edit the tree while it runs.

## Previews

`tools/macos/preview.sh --stage DIR [--build-only] [--priority] [--skip SUBSTRING] NAME`, from a clean
checkout of the commit to preview: builds, stages `bb-probe` and `gpu/libbbgpu.dylib` to
`DIR/<short HEAD>-NAME/` with `SHA256SUMS` (signatures checked; never overwrites), smoke-tests in a
heavy.sh quick slot, then runs the gate (`build.sh --test-keep-going`; a failure in a run paused by
a perf run is rerun once with `--test-one`, both results kept). `PREVIEW.txt` in the stage
directory holds every fact for the post. Exit 0 passed, 1 failed, 3 skips, 2 usage. A preview that
fails a step keeps its directory (with `FAILED` in `PREVIEW.txt`): a new attempt needs a new NAME.
Each staged directory also has `manifest.json`: `source_tree.head` is the full commit hash;
`artifacts.bb-probe` and `artifacts.libbbgpu.dylib` each give the absolute staged `path` and
`sha256`. `qualification.test_gate` is `PASS` only after the full keep-going gate passes; it is
`NOT_RUN` for a build-only preview or an interrupted gate, and `FAILED` or `SKIPPED` otherwise.
