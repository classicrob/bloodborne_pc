# BC4 / integer copy capability probe

`bc4-integer-copy-test` answers the narrow hardware question in the case-A alias design:
does the current device/library preserve BC4 block bytes through the production
`Runtime::CopyImage` path into a real R32G32Uint storage image and back?

The fixture uses BC4 16x16 and an integer 4x4 block grid, one mip and layer. It creates
production `VideoCore::Image` backings and calls `Runtime::CopyImage` directly, including
its current region construction, transitions and recording. It does not reproduce that
region builder in test code or change production behavior. No alternate format or
buffer-copy fallback is accepted. Unsupported creation/usage/pipeline/copy or a timeout
is a loud nonzero result, not a skip or capability pass.

The 16 input blocks are distinct eight-byte patterns. Integer and destination BC images
are first poisoned with different bytes. After BC4-to-integer, the integer raw bytes must
equal the original blocks; checking only the round trip could hide inverse mistakes.
The first integer-to-BC4 copy must also match every original byte. A one-invocation
compute shader then writes Rg32ui at interior block coordinate(1,2), changing both words.
After another production copy, all integer and BC bytes must match a CPU oracle: exactly
one changed block and fifteen unchanged blocks. The original BC source is checked too.
Every host transfer has prefix/suffix canaries. Readback waits for `Scheduler::Finish`,
then closes the scheduler before resource destruction. The test uses direct recording;
async recorder/lifetime behavior is outside this primitive.

Mutation switches `--skip-forward`, `--skip-reverse` and `--skip-store` must fail their
corresponding byte oracle. Path-call counts report the production copies and dispatch.
The ordinary run must have one forward copy, two reverse copies, one storage dispatch
and two completed readback phases. Retain the individual oracle lines, device/driver
identity, exact source/library hashes and exit status for every arm.

This does not prove decoded sampling, guest tiling or pitch identity, tail mips, layers,
CPU/GPU dirty ownership, same-command aliases, pool reuse, cache replacement or live BC4
diagnosis. One mip/layer deliberately isolates the copy primitive; adding a nonzero level
would not resolve the full-chain ownership obligation. No performance claim follows.

## Execution plan

Build owns execution after its shared baseline and control provenance. Cherry-pick the
isolated fixture commit into the existing cached tree; do not merge unrelated renderer
probe ancestry or create a cold private build. Configure normally, then build the optional
`bc4-integer-copy-test` target with the existing bounded build settings and shared heavy
wrapper. CMake applies the usual unit-test ABI options and export check. It is not yet
added to `build.sh --test` because this is an unresolved hardware capability experiment.
Use the same signed dylib/loader environment as the other device tests. Run one ordinary
arm and each mutation under a budgeted shared GPU correctness slot, saving bounded logs.
The test has a pause-aware 120-second watchdog. Any failed arm is triaged before repeating.

The CPU oracle can be compiled independently, with no Vulkan initialization or linkage:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -DBC4_COPY_ORACLE_ONLY \
  tests/test_bc4_integer_copy.cpp -o /path/to/thread-storage/bc4-copy-oracle
/path/to/thread-storage/bc4-copy-oracle
```

Run compilation through the current shared heavy wrapper and explicit Xcode toolchain.
CPU controls test distinct blocks, one changed block, detection of corruption in every
word and guard checks. Their success is not device capability evidence.

Prepared validation: the CPU-only build and controls passed with Xcode27.1,
`-Wall -Wextra -Werror`, through the shared quick-job wrapper. Device code has not yet
been compiled or run. The live BC4 descriptor and the broader cache-repair obligations
remain unresolved regardless of this fixture's future result.
