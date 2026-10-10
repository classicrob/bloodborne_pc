# MoltenVK push-descriptor buffer-length repro

This standalone Vulkan test binds the same buffer through byte and dword SSBO
aliases and reads `OpArrayLength` alongside unguarded data reads. It requires
`VK_KHR_push_descriptor`, portability enumeration/subset, and storage-buffer
8-bit access. The test has no game/runtime dependency.

From the repository root, with the team's x86-64 dependency prefix:

```sh
export BB_DEPS_PREFIX=/Users/roberthaisfield/bbport-deps/x86_64
bash tests/macos_descriptor_sizes/run.sh --bench
bash tests/macos_descriptor_sizes/run.sh --push
```

The default allocated-set path must exit 0. Twelve submissions reuse one
completed descriptor set, change byte ranges and aligned buffer offsets, and
check exact byte lengths, truncated dword lengths, a second binding's length,
and both input data reads. Ranges include 5, 7, 11, 35, 36, 37 and 63 bytes.

`--push` runs exactly those checks with a push-descriptor layout and
`vkCmdPushDescriptorSetKHR`. It exits 2 if sizes or reads differ. This is a
reproduction mode, not an acceptance pass. On M4 Max, macOS 27.2 and MoltenVK
1.4.2, push-descriptor lengths are incorrect while the data reads are correct.
Allocated-set lengths and data reads are exact. A production-emitter probe also
reproduces the failure with U8/U16/U32/F32/U64 aliases: a 36-byte descriptor
reports 1 byte and 0 dwords with push descriptors, versus 36 and 9 with allocated
sets. Values on the broken path are not a stable expected result.

`--bench` additionally measures CPU command recording, not GPU execution or
whole Liverpool draw time. It alternates both paths over 21 samples of 1024
three-buffer bindings. The allocated path includes allocation in batches of
32, descriptor updates and binding; it resets and reuses the descriptor pool
between completed samples. The push path records descriptor writes directly.
The production `DescriptorHeap` already batches 32 sets, recycles completed
pools and retains unused sets; this fix does not introduce a new allocator.

First synthetic run: allocated median 223.6 ns/binding (IQR 208.0–428.7), push
609.4 ns (534.2–1574.2). At 200 bindings this is 0.045 ms versus 0.122 ms.
These measurements were collected with other team work active; they establish
that the fix does not require expensive per-draw pool creation, not a game FPS
claim. More descriptors and submission-time Metal encoding need capture-based
measurement.

Apple guest graphics and compute pipelines use allocated descriptor sets for
all resources. Shader guards and guest bounds remain enabled. Native platforms
retain their existing descriptor selection.
