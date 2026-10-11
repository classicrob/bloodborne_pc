# Pipeline compile time on Apple

Cold sessions spent about 190 ms per shader/pipeline compile at world load, all on the GPU command
thread (efce883, Finalise load: 55 compiles, 10.6 s). This note records where that time goes and
how it is spread over cores.

## Where the time goes

Microbenchmark `out/mvk-concurrency/harness` (not tracked; x86_64 under Rosetta, MoltenVK 1.4.2,
unique ~60 KB SPIR-V compute shaders so the system Metal cache cannot hit), with
`MVK_CONFIG_PERFORMANCE_TRACKING=1 MVK_CONFIG_ACTIVITY_PERFORMANCE_LOGGING_STYLE=1`, per pipeline:

| step | time |
|---|---|
| SPIR-V to MSL (SPIRV-Cross, in process, Rosetta) | 9-18 ms |
| MSL to MTLLibrary (Metal compiler service, native) | 45-55 ms |
| MTLFunction to pipeline state (Metal back end, native) | 250-305 ms |

The native Metal compiler is ~95% of pipeline creation. Our own GCN to SPIR-V translation is
measured separately in game ("Compiles:" line after Frame stats, "Slow compile" lines).

## Concurrency

Same harness, 8 threads (another build was running on the host, so absolute numbers are high):

| mode | pipelines/s |
|---|---|
| serial | 2.5 |
| 8 threads, one shared VkPipelineCache | 8.1 |
| 8 threads, a VkPipelineCache per thread | 10.2 |
| 8 threads, no VkPipelineCache | 8.7 |

A shared VkPipelineCache does not serialize creation. 16 threads were not faster than 8.
`MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION` is still to be measured on a quiet host.

## Use

- Startup preload (`PipelineCache::WarmUp`): serial. Creating the preloaded pipelines on 8 threads
  (5f858bd, reverted) made a warm start fail on ca56783: MoltenVK refused to link a pipeline whose
  fragment shader reads locations 0-4 ("Fragment input(s) user(locn0-4) mismatching vertex shader
  output type(s) or not written by vertex shader"). The shaders MoltenVK named belong to different
  cache records: fragment shader a89e01e9 permutation 1 (record g_0x7af64b7767ecc424, an ordinary
  VS/FS pipeline) and vertex shader 89cda9e4 permutation 2 (record g_0x9733905e9ce37805, a QuadList
  pipeline with auxiliary tessellation stages). Every stored record is consistent on its own
  (interface locations and types checked offline). The failure is deterministic with a 13-record
  subset that mixes QuadList and ordinary records, and needs several of them together. Concurrent
  creation is the trigger; the mechanism (inside MoltenVK or ours) is being isolated with a harness.
- Runtime creation on workers (Opus-Renderer branch bb/opus-renderer-async-pipelines, 6caadad) is
  held for the same reason.
