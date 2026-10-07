# Experimental macOS port

**Experimental; real GPU-path Milestone A and title-screen Milestone B are
pending.** The current real run stops before device creation because MoltenVK
lacks `robustBufferAccess2`. Passing runtime, surface and shader tests does not
establish that the game renders or is playable. CPU-only debugging and private
capability-bypass experiments do not count as milestone passes.

## Requirements and tested host

- Apple Silicon with Rosetta 2. Intel Macs and a native arm64 guest runtime are
  not supported by this build.
- Tested on an M4 Max, originally macOS **27.0 (26A5425a)** and now
  **27.2 (26B5101f)**. The Rosetta spike, runtime acceptance and null-image
  shader tests were rerun on 27.2. Other macOS releases and other GPUs have not
  been validated; this is not a claim of a minimum supported OS version.
- Xcode or Apple's command-line tools (`xcode-select --install`) and native ARM
  Homebrew in `/opt/homebrew`. The current build used the macOS 26.4 SDK.
  Install Rosetta through macOS when prompted before running setup.
- Your own Bloodborne dump, **CUSA03173 v1.09**, containing the base game's
  modules as well as the update. No game files are supplied.
- Space for build/dependency scratch and, separately, game extraction. SDK
  setup briefly uses several GiB; extraction retains a seekable package plus
  extracted files alongside the original archive.

## One-time setup and build

From the checkout, after installing the prerequisites:

```sh
bash packaging/macos/setup.sh
```

The script prints and installs only missing host tool/header formulas through
ARM Homebrew, with automatic updates, install upgrades, installed-dependent
checks and automatic cleanup disabled. It keeps already installed formula
versions. It then runs
`build-deps.sh`, fetches the pinned native Python/Tk runtime, builds x86_64
`bb-probe` and `libbbgpu.dylib`, runs `build.sh --test`, and verifies the actual
child runs under Rosetta. It stops at the first failure. It does not extract a
game, launch it, or create an app. `BB_BUILD_JOBS` sets build parallelism
(default 8); `BB_DEPS_PREFIX` relocates target dependencies and
`BB_PYTHON_PREFIX` relocates the standalone Python installation.

The individual steps, for contributors investigating a failure, are:

```sh
bash packaging/macos/build-deps.sh
python3 packaging/macos/fetch-python.py
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64" bash build.sh --test
```

`fetch-python.py` needs Python 3.11 or newer; `setup.sh` supplies native
Homebrew Python 3.14 for bootstrapping. Native Homebrew FFmpeg generates the
small H.264/AAC fixture for the AvPlayer acceptance test; it is separate from
the minimal x86_64 FFmpeg libraries loaded by the game. CMake, Ninja, pkg-config, glslang,
Python and Boost/robin-map headers run or are consumed on the ARM host.
Libraries loaded by the game are built for x86_64 in one shared dependency
prefix; an Intel Homebrew installation is not required.

## Make a game folder from your own FPKGs

Skip extraction if your own dump already has `eboot.bin`, `sce_sys/param.sfo`,
`sce_module/libc.prx`, the other required modules, and `dvdroot_ps4/`.
An update alone is insufficient: the tested v1.09 update has no `sce_module/`.

For a RAR archive, use [official UnRAR](https://www.rarlab.com/rar_add.htm) to
list and extract the named base/update package members to a scratch folder:

```sh
unrar lb "/path/to/your-own-dump.rar"
mkdir -p "$HOME/bbport-game/pkg"
unrar x "/path/to/your-own-dump.rar" "base.pkg" "$HOME/bbport-game/pkg/"
unrar x "/path/to/your-own-dump.rar" "update-1.09.pkg" "$HOME/bbport-game/pkg/"
```

Replace those member names with the exact names shown by the listing. The
team's UnRAR source build handled the tested RAR5 archive; 7zz 24.09 rejected
its compression method. PKG extraction requires a **complete seekable local
PKG file**. Piping a RAR member into the extractor is not supported.

The tested macOS `pkg_extractor` is the vendored shadPS4 package reader from
[Ashref-dev/ps4pkg](https://github.com/Ashref-dev/ps4pkg), pinned at
`522a35b6d68cbce725704d261d2238bc8a097300`. To build that source tool separately:

```sh
git clone https://github.com/Ashref-dev/ps4pkg out/pkg-tools
git -C out/pkg-tools checkout 522a35b6d68cbce725704d261d2238bc8a097300
brew install zlib
(cd out/pkg-tools/vendor/cryptopp && make -j8 static)
cmake -S out/pkg-tools/vendor -B out/pkg-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/zlib
cmake --build out/pkg-build --parallel 8
out/pkg-build/pkg_extractor "$HOME/bbport-game/pkg/base.pkg" "$HOME/bbport-game/base"
out/pkg-build/pkg_extractor "$HOME/bbport-game/pkg/update-1.09.pkg" "$HOME/bbport-game/update"
ditto "$HOME/bbport-game/update/CUSA03173-patch" "$HOME/bbport-game/base/CUSA03173"
```

The tool appends `CUSA03173` for the base and `CUSA03173-patch` for the update.
`ditto` overlays the update into the extracted base copy; keep your original
packages intact. These commands describe the extractor/overlay approach used
for the verified dump. They do not obtain a game or repair an unreadable dump.
Check the complete output before reclaiming package scratch:

```sh
BB_GAME_DIR="$HOME/bbport-game/base/CUSA03173" \
  "$HOME/bbport-deps/python-arm64/bin/python3" scripts/prepare.py \
  "$HOME/bbport-game/base/CUSA03173" --out out
```

For the tested dump, prepare/link validation found 42 required modules and the
runtime linked native libc/Fios2. A successful extraction alone does not prove
runtime compatibility. Keep the selected game folder read-only during use;
all generated files go into the checkout or the selected data directory.

## Start and locate data

```sh
"$HOME/bbport-deps/python-arm64/bin/python3" launcher/bbport_launcher_mac.py
```

Select the complete game folder, edit settings, and press Start. For direct
Terminal use with the same preparation/link/content/patch path:

```sh
BB_GAME_DIR="$HOME/bbport-game/base/CUSA03173" \
  PYTHON="$HOME/bbport-deps/python-arm64/bin/python3" bash run.sh
```

`run.sh` builds incrementally by default. To use an existing validated binary:

```sh
BB_GAME_DIR="$HOME/bbport-game/base/CUSA03173" BB_PREBUILT=1 \
  BB_PROBE="$PWD/out/bb-probe" \
  PYTHON="$HOME/bbport-deps/python-arm64/bin/python3" bash run.sh
```

Development data lives next to `run.sh`: generated images in `out/`, saves in
`user/`, settings in `bbport.ini` and `launcher-settings.json`. `BB_DATA_DIR`
relocates the writable data; `BB_USER_DIR` and `BB_CONFIG` independently
relocate saves and the INI. Target libraries live in `~/bbport-deps/x86_64`;
launcher Python lives in `~/bbport-deps/python-arm64`. The future app uses
`~/Library/Application Support/bbport` for writable data. The app is still a
validated plan; no relocated app launch has passed yet.

Both launch paths select the one prefix-relative MoltenVK ICD manifest.
`run.sh` verifies the real `bb-probe` child reports translated x86_64 before
launching the guest. Missing dependencies or a failed preflight stop the run.
`--cpu-only` is a diagnostic mode and does not supply a GPU submission shim.

## Output and performance target

The target is steady **60 FPS with temporal upscaling to 4K output** on the
M4 Max, rendering around 1440p or lower. Native 4K scene rendering is not
required. This is a target, not a measured production result.

- External 16:9 display: choose **3840 × 2160**, FSR 3.1 and Quality to render
  at 2560 × 1440, or a lower-resolution preset when needed.
- Built-in 3456 × 2234 panel: choose **3456 × 1944 (MacBook panel fit)**, the
  largest fitting 16:9 image. Full-panel presentation centers it with 145 pixels
  of letterboxing above and below. FSR 3.1 Quality renders at 2304 × 1296.

The launcher and in-game output menu offer both sizes. The presenter already
centers the game image while preserving its aspect ratio. Adding the panel
choice introduces no extra per-frame copy or shader pass. A 4K60 gameplay
acceptance measurement remains pending.

## Unsupported paths and feature gaps

Choose **FSR 3.1, TAA or off**. FSR 4 and 4.1.1 are deliberately disabled on
Apple: their INT8/dot-product/compute-derivative shader paths have not been
validated for this Metal backend, and 4.1.1 additionally needs
`VK_VALVE_shader_mixed_float_dot_product`. This is not a claim that Apple
hardware lacks all INT8 operations. The Mac launcher rejects saved FSR4/411
settings; the renderer also aborts on an FSR 4/4.1.1 selection and names the
selected version. The AMD shader feature requirements above are why these
versions are disabled for this backend.

The measured M4 Max/MoltenVK capability audit is a snapshot of SDK 1.4.363
with MoltenVK 1.4.2, not a statement about every future driver:

| Capability | Measured result and current status |
| --- | --- |
| `robustBufferAccess2` | False. Current real device creation fails loudly; exact shader bounds semantics are pending. |
| `nullDescriptor` | False natively. Exact zero-read/discard-write image, buffer and vertex semantics are implemented and tested under Rosetta; this gate is cleared. |
| Geometry shaders | False. Guest ES/GS modes abort with an emulation diagnostic rather than skipping draws. Actual game requirements remain under investigation. |
| Tessellation | Supported, but portability `tessellationIsolines` and `tessellationPointMode` are false; mode checks/emulation remain audit work. |
| `shaderFloat64` | False. Exact FP64 implementation or hard rejection of actual guest use is pending. |
| Sparse binding/residency | False. A private, incomplete-capability experiment exposed an unbound sparse buffer returning device address zero; this is a blocker investigation, not acceptance evidence. |
| Logic operations, depth bounds, indirect draw count, wide lines | False. Renderer use and loud checks/emulation remain under review. |
| Multisample storage images, buffer/shared 64-bit atomics | False. Shader use and loud checks/emulation remain under review. |
| Float denorm and signed-zero modes | Unsupported-mode warnings remain in shader tests; exact semantics/hard rejection are pending review. |
| Robust image access, tessellation basics, barycentrics, vertex divisor, external host memory | Reported supported; reporting a feature does not establish full game correctness. |

Missing required behavior on Apple must fail with a named error or use a
verified exact semantic implementation. Pending audit entries above are
limitations, not permission to assume the behavior works. A private experiment
with incomplete robustness and a temporary arena reached the title screen at
1080p and reported 60 FPS. That result is discovery evidence only; no production
title-screen, frame-presentation or playability pass is claimed.

## App packaging groundwork

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
