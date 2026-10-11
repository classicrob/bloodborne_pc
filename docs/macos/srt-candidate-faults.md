# Unreadable SRT permutation candidates

`GetProgram` walks a cached permutation's shader resource table before it can compare the full
specialization against the selecting draw. A candidate's layout can require a table that the draw
does not supply. Rejecting its stage or fetch program first helps, but does not make every remaining
table read valid.

On Apple, candidate walking now uses the existing thread-local fault recovery point. Flat-buffer
allocation and copying the user-data registers happen before recovery is armed. Only the walker
call is inside the recovery region, so a jump crosses no C++ objects requiring destruction. A read
fault rejects that candidate, restores any enclosing recovery point, and returns through the normal
snapshot cleanup. Partly written scratch data is never published. The next candidate is evaluated
normally. Rejections are logged for the first eight on each thread, then every 1024, including the
shader hash and walker address.

The selected module still requires a full specialization match. The generated walker is never
patched to return zero. If no candidate matches, the existing fresh-translation path runs; this
change does not turn an invalid live SRT into a valid descriptor or make guest-memory reuse safe.
Native platforms retain their existing walker behavior. Shader output and cache versions do not
change.

## Run C evidence

The PREVIEW-11 run `20261011-075756-1080p-PM4-C-P11` used labels at GPU completion with the unsafe
submit throttle disabled. In its pinned library, the frame at `0xca6d6` is the return from
`GetProgram::matches_data` calling `Info::RefreshFlatBuf`. The selecting stage is an ordinary VS
(hardware Vertex, software Vertex).

The function pointer was valid: `rax=0x10eea801c`, the generated walker's first instruction. Its
first operations push the root pointer, load the pointer at root +8 (user-data s[2:3]), and mask it
to 48 bits. The fault at `0x10eea802e` is `mov r10d,[rdi]`, with `rdi=0`. This was a null table load,
not a call through a null function pointer. The bytes printed around registers were fault-dump
memory samples, not evidence of code rewriting.

The crash identifies the candidate-walk call site. It does not establish why s[2:3] was null: an
incompatible candidate layout and invalid or prematurely reused live user data remain distinct
possibilities. The regression below qualifies candidate rejection; a live replay is needed to
determine whether it resolves this run.

## Regression

The existing `shader-permutation-info-test` installs two different SRT root layouts in two real
compiled permutations of one program and role. The layouts produce the same descriptor slot from
different root pointers. Alternating draws supplies one table and leaves the other pointer null.
The wrong candidate writes to scratch before faulting. Sixteen lookups must reuse the original
module and key, retain the rejected module's flat data and user-data binding, and preserve the
outer snapshot and recovery point. These installed layouts test candidate evaluation independently
of how the compiler derives a layout; they are not a reconstruction of run C's unknown shader.

The unguarded candidate-walk negative control must escape to the fixture's outer recovery point
and exit 2 with `Candidate SRT fault escaped to the caller's recovery point`.
