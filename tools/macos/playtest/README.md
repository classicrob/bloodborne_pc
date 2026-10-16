# Playtest tools (Opus-Playtest)

Run from anywhere; each script finds the repository root from its own path. Game runs go through
`tools/macos/play_route.py` (heavy.sh playtest pool); perf runs (`r0.sh`, `ab.sh`) hold the GPU
exclusively with `--cpu-quiet`. Warm cache default: `p23-clinic` (Apple shader cache 31).

- `r0.sh PROBE`: quiet R0 perf run (5 min clinic idle + 5 min walk), FPS and parse split per segment. Env `R0_LABEL`, `R0_EXTRA` (e.g. `--perf`), `R0_FLIP_AT`/`R0_FLIP_VALUE` (live toggle flip). Writes `host.txt` (thermal, memory pressure, load; before and after) into the run dir.
- `ab.sh NAME "A B B A" A=BUILD_DIR B=BUILD_DIR`: interleaved quiet R0 A/B with BB_PERF; output in `out/macos_route/ab-NAME/`.
- `perfsum.py RUN_DIR`: BB_PERF window records per R0 segment (GPU busy per flip, submissions, FPS) next to the parse-split frame time.
- `absheet.py OUT.jpg RUN_A RUN_B`: side-by-side idle/walk captures of two R0 runs and the idle pixel diffs (with within-run drift).
- `footprint.sh PROBE LABEL FIXTURE SECONDS [walk]`: `BB_PASS_FOOTPRINT_VERIFY=1` run; mutation builds must stop loudly.
- `perfcap.sh PROBE REPLAY_DIR`: the verifier's live `perf:60` replay capture at the settled clinic idle.
- `motion.sh PROBE`: camera-only motion-vector gate (pitch up/down), judged by `tools/macos/motion_agree.py`.
- `pan-explore.sh PROBE LABEL FIXTURE LEGS [TURN_S]`: rendering coverage; per leg a walk, a 4-way camera pan and a contact sheet (`<run>/sheet-NN.jpg`) to review by eye.
- `arch-route.sh PROBE LABEL`: Central Yharnam coffin-arch route from `central-yharnam-arch` (depth-bounds repro); `KEEP=1` keeps the game.
- `../play_route_goto.py`: closed-loop walk-to-XYZ on Astra-2's player-position file (`BB_PLAYER_POS_FILE`); tests in `tests/test_play_route_goto.py`. Not wired into play_route yet (waits for the publisher build).
