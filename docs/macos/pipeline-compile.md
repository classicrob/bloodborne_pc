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

- Startup preload (`PipelineCache::WarmUp`): store reads and shader modules on the GPU command
  thread, pipeline creation on up to 8 threads (Apple; one elsewhere).
- Runtime (planned): translation stays on the GPU command thread (it needs the shader info), the
  pipeline is created on a worker, and the draw recording thread waits for it when it records the
  draw. No draw is skipped or drawn with another pipeline.
