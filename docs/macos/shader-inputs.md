# Shader-input corrections from the corpus sweep

The 8cea904 corpus sweep found 132 vertex modules with illegal `Flat` decorations and one malformed fragment module (`0xf9c8eb4a`, `GXFlver_SOPos_FwdDbg`). Both defects were in shared recompiler code and affect native platforms as well as Apple.

## IMAGE_GET_LOD sampler tracking

The malformed module was not truncated by the dump writer. Its backend emitted an `OpLoad` with three words: the result type was absent because no sampler type had been declared, and the pointer came from indexing an empty sampler list. `IMAGE_GET_LOD` supplied only an image and coordinates to the IR, although the SPIR-V LOD query consumes a sampled image. Discovery registered samplers only for ordinary sampling instructions.

The translator now carries the instruction's four S# dwords as a temporary IR operand. Discovery resolves them through the existing sampler resource path; patching adds the sampler and packs its binding beside the image binding, then clears the temporary operand. The emitter checks the patched state and sampler binding before loading it. Null images retain the existing zero-result path.

This preserves the guest-selected sampler, including its LOD settings, rather than inserting a synthetic default sampler. The regression encodes IMAGE_GET_LOD with S# at s[8:11], checks that the selected sampler's nonzero maximum LOD reaches resource metadata, and validates null and non-null emitted modules. It checks compilation/resource tracking, not a pixel-level LOD measurement.

## Subgroup input decorations

`SubgroupLocalInvocationId` and `SubgroupLtMask` previously received `Flat` in every hardware stage. The vertex execution model forbids this decoration on inputs. Both inputs now receive it only in the fragment stage. A regression emits vertex, fragment and compute modules, checks the two decorations explicitly, and validates each module.

## Validation

`shader-inputs-test` is wired into Darwin `build.sh --test`, including signing/export checks and `spirv-val` for its five modules. The pre-existing null-descriptor emitter test uses the updated emitter signature. Shader binary cache versions are Apple 26 and native 15. The full corpus sweep results are recorded with the merge handoff; representative synthesized descriptors remain a limitation of that sweep.
