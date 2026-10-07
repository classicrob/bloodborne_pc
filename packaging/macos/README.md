# macOS build and app groundwork

The game process is x86_64 and requires Rosetta on Apple Silicon. The launcher
and preparation scripts run in native arm64 Python; they are separate processes
and do not load the game libraries. `run.sh` executes the actual `bb-probe` with
`--verify-macos-host` before starting the game. That child must report
`sysctl.proc_translated=1`; failure stops the launch.

For development, dependencies live in `~/bbport-deps/x86_64`, generated files
stay in the checkout's `out/`, and the game folder remains read-only:

```sh
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64" bash build.sh --test
BB_GAME_DIR="$HOME/CUSA03173" bash run.sh
python3 packaging/macos/fetch-python.py
"$HOME/bbport-deps/python-arm64/bin/python3" launcher/bbport_launcher_mac.py
```

The launcher reuses the existing settings, INI persistence and translations.
It offers game/save folders, FPS, FSR 3.1/TAA, output/preset, language, name,
fullscreen, Start/Stop and the process log. Unsupported macOS upscalers are
errors. Existing additional INI settings are preserved.

`fetch-python.py` pins CPython 3.14.8, python-build-standalone release 20261003,
aarch64 macOS install-only stripped archive, SHA-256
`ed14e9042f36c0aa18383f5d9729bcbf17847e842666a793b5a5a5d8505c0ca1`.
It verifies the archive before extraction and imports Tk using the installed
runtime. The archive is about 27 MB; the installed runtime is about 72 MiB.

`python3 packaging/macos/package.py` validates and prints the bundle plan;
it creates no app by default. The planned layout is:

```text
Bloodborne.app/Contents/
  MacOS/Bloodborne             native arm64 launcher bootstrap
  Frameworks/                 x86_64 GPU, SDL, FFmpeg, Vulkan, MoltenVK
  Resources/python/           pinned native arm64 Python and Tk
  Resources/port/bin/          x86_64 bb-probe and capability helper
  Resources/port/deps/         relative MoltenVK ICD; lib -> Frameworks
  Resources/port/{scripts,launcher}/
  Resources/port/run.sh
```

The bootstrap uses `LSArchitecturePriority=arm64`; the game child is explicitly
verified as translated x86_64. Dylib references and rpaths point inside the app,
and the builder checks GPU host exports and ad-hoc signs all Mach-O files and
the app. Packaged generated files, saves and settings use
`~/Library/Application Support/bbport`. Game data is selected externally and
is never included in the app.

Actual creation is gated on a **passing real GPU-path Milestone A**, not the
CPU-only debugging run. Supply `--build --milestone-a-report report.json`, where
the report contains `{"milestone":"A","result":"pass","gpu_path":true,
"log":"/absolute/path/to/acceptance.log"}`. The builder records report/log
hashes and refuses an existing destination. A bundle has not yet been created
or validated through a relocated launch; that acceptance follows Milestone A.
