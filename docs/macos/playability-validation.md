# macOS playability validation

## Audio

Run `sh tests/test_audio_macos.sh` with the shared x86_64 SDL3 dependency prefix
(`BB_DEPS_PREFIX`, default `~/bbport-deps/x86_64`). The test emits a short,
low-volume 440 Hz tone through the actual `runtime_audio.c` API and CoreAudio.
It also checks explicit timer mode, multiport pacing, half-volume conversion,
monotonic output timestamps, EINTR during absolute sleeps, queue drainage, port
close/reopen, and a named fatal error when the selected SDL driver is absent.
On the development host the real device was **MacBook Pro Speakers**; 96
256-frame stereo buffers were submitted, and the queue drained. This verifies
device submission, not subjective listening or acoustic loopback.

Apple SDL initialization, stream open/resume, queue query and submission errors
now stop explicitly. Absolute audio sleeps retry EINTR only. `BB_AUDIO=none`
is an explicit disabled-output configuration, never an automatic device-failure
fallback. Other platforms retain their audio error policy.

## ATRAC9

Run `python3 tests/test_ajm_macos.py` after building `out/libatrac9.a`.
For a shared build, set `BB_ATRAC9_LIBRARY` to that archive and
`BB_ATRAC9_SOURCE_ROOT` to the source repository containing its headers.
No copyrighted audio fixture, game data, GPU library, or network download is used.

The test synthesizes a valid 48 kHz mono ATRAC9 sequence: one nonzero spectral
coefficient, direct scale factors, explicit gradient, and eight padded
256-byte superframes. It invokes the actual AJM batch/control APIs and verifies
S16, S32, float, input/output consumption, decoder history, and gapless trimming.
All 2048 S16 samples match an independently computed DCT-IV plus overlap/window
reference exactly on Rosetta (test tolerance: one PCM unit). Signed PCM expansion
uses multiplication, avoiding undefined left shifts of negative samples.

A cross-check with host FFmpeg 7.1.1 accepted the generated RIFF/ATRAC9 stream but
produced reversed polarity at one-quarter amplitude (within rounding). It was
therefore not used as an unquestioned oracle; the direct mathematical reference
matches LibAtrac9. The real-stream comparison below checks whether this discrepancy generalizes.

## Saves and case-sensitive guest paths

`tests/test_savedata_macos.sh` exercises the actual save and file implementations:
create/write/close/reopen, metadata and icon persistence, SaveDataMemory across setup,
search, read-only mounts (file and metadata writes rejected), error injection for a
failed metadata write, and recursive deletion with a symlink to an external file.
`nftw(FTW_DEPTH|FTW_PHYS)` removes the link while preserving its target. Mounted saves
cannot be deleted. Apple save write/close/delete errors now reach the caller instead
of reporting success. Linux and Windows error behavior is unchanged.

On this case-insensitive APFS volume, guest lookups now require the exact directory
entry spelling, matching Linux. Case-only rename works. Creating a second name
that differs only by case cannot be represented on that volume and aborts with a
message naming the path; it must never overwrite an existing save accidentally.
Save mount/delete also reject wrong-case directory names. Mod discovery deliberately
matches filenames case-insensitively and uses the original game's spelling in the
merged view; all 13 `test_mods.py` tests pass. The no-mod test resolves the `/var` to
`/private/var` host alias before comparing paths.

Validation: strict x86_64 syntax checks pass, as do the full save/file test under
Rosetta and a native ARM build of the same C sources. The Rosetta run completed
after the temporary system-wide translation outage recovered.

`run.sh` changes to its own directory, then defaults `BB_DATA_DIR` to `.`. Thus
`out/`, `user/`, and `bbport.ini` live next to `run.sh`. `BB_DATA_DIR` relocates all
three, while `BB_USER_DIR` and `BB_CONFIG` independently override saves and settings.
Save paths are `user/savedata/<user>/<title>/<save>/`; metadata sits beside the save
in `<save>.sce_sys/`, and SaveDataMemory in `<title>.memory/memory.dat`.


## Real game ATRAC9 comparison

`tests/test_ajm_asset.c` decodes a supplied configuration and frame sequence through
actual AJM control/run/wait calls. Compile it against the normal LibAtrac9 archive,
then use `tests/compare_atrac9_asset.py BANK --decoder EXECUTABLE --ffmpeg FFMPEG`.
The script extracts one local FSB5 stream into `out/`, wraps identical encoded bytes
in RIFF for FFmpeg, and reports PCM differences. Its FSB5 field layout follows the
[vgmstream parser](https://github.com/vgmstream/vgmstream/blob/master/src/meta/fsb5.c).
It supports single-layer ATRAC9 and retains decoder priming/padding samples for an
identical comparison. No game bytes or decoded audio are committed.

Measured v1.09 EU asset: `dvdroot_ps4/sound/sprj_c1212.fsb`, stream 0; bank SHA256
`7b3f701463b12fb235b7eb53a80794ef7e145e4531af061e924d10e2aa163ecb`.
Configuration `fe7004f0`, mono 48 kHz, 160-byte superframes with four 256-sample
frames each. The 55 superframes decode to 56,320 PCM samples (FSB nominal length
55,296). Actual AJM under Rosetta and native ARM emit byte-identical S16 PCM.
FFmpeg 7.1.1 emits exactly the negation: fitted AJM/FFmpeg gain -1.0, residual RMS
0.0, maximum direct difference 5,280. Thus the synthesized stream's quarter-scale
difference does not generalize to this real stream. The polarity disagreement is a
platform-independent decoder reference question; this result does not establish
which decoder matches the console. No decoder scaling or sign change was made.
