# Shader-input corrections from the corpus sweep

The 8cea904 corpus sweep found 132 vertex modules with illegal `Flat` decorations and one malformed fragment module (`0xf9c8eb4a`, `GXFlver_SOPos_FwdDbg`). Both defects were in shared recompiler code and affect native platforms as well as Apple.

## IMAGE_GET_LOD sampler tracking

The malformed module was not truncated by the dump writer. Its backend emitted an `OpLoad` with three words: the result type was absent because no sampler type had been declared, and the pointer came from indexing an empty sampler list. `IMAGE_GET_LOD` supplied only an image and coordinates to the IR, although the SPIR-V LOD query consumes a sampled image. Discovery registered samplers only for ordinary sampling instructions.

The translator now carries the instruction's four S# dwords as a temporary IR operand. Discovery resolves them through the existing sampler resource path; patching adds the sampler and packs its binding beside the image binding, then clears the temporary operand. The emitter checks the patched state and sampler binding before loading it. Null images retain the existing zero-result path.

This preserves the guest-selected sampler, including its LOD settings, rather than inserting a synthetic default sampler. The regression encodes IMAGE_GET_LOD with S# at s[8:11], checks that the selected sampler's nonzero maximum LOD reaches resource metadata, and validates null and non-null emitted modules. It checks compilation/resource tracking, not a pixel-level LOD measurement.

## Subgroup input decorations

`SubgroupLocalInvocationId` and `SubgroupLtMask` previously received `Flat` in every hardware stage. The vertex execution model forbids this decoration on inputs. Both inputs now receive it only in the fragment stage. A regression emits vertex, fragment and compute modules, checks the two decorations explicitly, and validates each module.

## Qualified raw P0 inputs on Apple

Metal supports flat varyings but cannot translate `PerVertexKHR` arrays. The corpus contains 28 fragment shaders with raw parameter inputs; every captured access selects P0. The first world-entry failure, `0xb6e87124`, executes `v_interp_mov_f32 v8, P0, attr2.x` (word `0xc8220802`, byte offset `0x258`). Its live input maps to parameter 5 with `FLAT_SHADE=true` and `DEFAULT_VAL=false`, so it is not passthrough.

On devices without per-vertex fragment inputs, a live raw P0 read becomes a `Flat` input only when `FLAT_SHADE` is set and the input is not passthrough. Ordinary default P0 reads remain constants. Live non-flat P0, passthrough, P10 and P20 reads fail with the shader hash and input flags. Individual raw reads retain an IR flag until final dead-code elimination: a later P0 read cannot hide a live unsupported read of the same parameter, while dead reads do not force a rejection or declaration. The native per-vertex path is unchanged.

The qualification follows the AMD register description and Mesa's flat-input lowering: flat inputs read P0 with `FLAT_SHADE` enabled. It deliberately makes no claim that raw hardware P0 in non-flat or passthrough mode is the API provoking vertex. MoltenVK on M4 was separately tested with indexed triangle lists, odd/even strips with restart, and fans: Metal flat inputs select Vulkan's FIRST provoking vertex in all four controls, while the LAST reference fails all four. The renderer rejects LAST mode and auxiliary RectList/QuadList pipelines with live flat inputs, whose tessellation corner selection is not qualified.

The regression translates actual encoded interpolation instructions. It checks qualified Flat, default constants, dead non-flat/passthrough reads, dead P10 followed by live P0, and unchanged native P0/P10 modules. Five negative controls require a named rejection, including live P10 followed by P0 on the same parameter.

## Validation

`shader-inputs-test` is wired into Darwin `build.sh --test`, including signing/export checks and `spirv-val` for its twelve modules. The pre-existing null-descriptor emitter test uses the updated emitter signature. Shader binary cache versions are Apple 26 and native 15. The full corpus sweep results are recorded with the merge handoff; representative synthesized descriptors remain a limitation of that sweep.
