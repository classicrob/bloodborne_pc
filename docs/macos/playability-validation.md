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
matches LibAtrac9. More codec modes and real game streams remain integration
coverage beyond this synthesized-frame test.
