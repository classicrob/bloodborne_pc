# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
if (APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "bbport guest execution architecture")
endif()
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        if (APPLE)
            message(FATAL_ERROR "Requested macOS LTO unavailable: ${bb_ipo_error}")
        else()
            message(WARNING "LTO unavailable: ${bb_ipo_error}")
        endif()
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# bbport (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32 AND NOT APPLE)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(gs_indirect_setup.comp gs_indirect_setup_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for bb-probe.exe to resolve, so the
# library is static there and linked into the executable.
if (WIN32)
    set(BBGPU_KIND STATIC)
else()
    set(BBGPU_KIND SHARED)
endif()
add_library(bbgpu ${BBGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
if (APPLE)
    # MetalFX temporal upscaling module (docs/macos/metalfx.md, metalfx-integration.md unit 2):
    # Objective-C++ with ARC; Vulkan is reached only through the vkGetDeviceProcAddr it is given.
    enable_language(OBJCXX)
    target_sources(bbgpu PRIVATE shim/macos_metalfx.mm)
    set_source_files_properties(shim/macos_metalfx.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(bbgpu PRIVATE "-framework Foundation" "-framework Metal"
        "-framework MetalFX" "-framework Security")
endif()
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
if (APPLE)
    target_include_directories(bbgpu PRIVATE third_party/xbyak)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_mac_compat.h)
    target_link_options(bbgpu PRIVATE -Wl,-undefined,dynamic_lookup)
    target_sources(bbgpu INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../src/macos_ui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/compat_mac.c)
    target_link_options(bbgpu INTERFACE -Wl,-export_dynamic)
    set_target_properties(bbgpu PROPERTIES INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR ON BUILD_RPATH "@loader_path"
        INSTALL_RPATH "@loader_path")
elseif (NOT WIN32)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(bbgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h)
    target_include_directories(bbgpu PRIVATE third_party/xbyak)
    target_link_libraries(bbgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm)
elseif (NOT APPLE)
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
endif()

# bbport (Windows): bbgpu is a static library there, so the loader and the HLE runtime are
# linked here, where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32)
    enable_language(RC)
    file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
    file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${BB_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(bb-probe ../src/probe.c ${BB_RUNTIME} ../src/compat_win.c ../src/vulkan_smoke.c
        ../packaging/windows/bb-probe.rc)
    set_target_properties(bb-probe PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(bb-probe PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
    target_include_directories(bb-probe PRIVATE .. ../src)
    target_link_libraries(bb-probe PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(bb-probe PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    add_executable(bb-gpu-capabilities ../tools/gpu_capabilities.c ../packaging/windows/bb-gpu-capabilities.rc)
    target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan)
    # "Play Bloodborne.exe": the game with the launcher's saved settings, no launcher window.
    add_executable(bb-play WIN32 ../packaging/windows/play.c ../packaging/windows/play.rc)
    target_compile_options(bb-play PRIVATE -municode)
    target_link_options(bb-play PRIVATE -municode)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

if (APPLE)
    add_executable(null-descriptor-test EXCLUDE_FROM_ALL ../tests/test_null_descriptors.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    target_include_directories(null-descriptor-test PRIVATE shim shadps4 third_party/gcn/include)
    target_compile_options(null-descriptor-test PRIVATE -UNDEBUG -fPIC)
    set_source_files_properties(../tests/test_null_descriptors.cpp PROPERTIES
        COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
    target_link_libraries(null-descriptor-test PRIVATE bbgpu)
    add_executable(buffer-bounds-test EXCLUDE_FROM_ALL ../tests/test_buffer_bounds.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(buffer-bounds-benchmark EXCLUDE_FROM_ALL ../tests/test_buffer_bounds_benchmark.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(buffer-dead-resources-test EXCLUDE_FROM_ALL ../tests/test_buffer_dead_resources.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(host-import-arena-test EXCLUDE_FROM_ALL ../tests/test_host_import_arena.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(vertex-pulling-pipeline-test EXCLUDE_FROM_ALL ../tests/test_vertex_pulling_pipeline.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(shader-permutation-info-test EXCLUDE_FROM_ALL ../tests/test_shader_permutation_info.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(gpu-user-dir-test EXCLUDE_FROM_ALL ../tests/test_gpu_user_dir.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(push-userdata-test EXCLUDE_FROM_ALL ../tests/test_push_userdata.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(macos-perf-test EXCLUDE_FROM_ALL ../tests/test_macos_perf.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(flatten-base-shape-test EXCLUDE_FROM_ALL ../tests/test_flatten_base_shape.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(depth-bounds-shader-test EXCLUDE_FROM_ALL ../tests/test_depth_bounds_shader.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(shader-inputs-test EXCLUDE_FROM_ALL ../tests/test_shader_inputs.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    foreach(bounds_target buffer-bounds-test buffer-bounds-benchmark buffer-dead-resources-test host-import-arena-test vertex-pulling-pipeline-test shader-permutation-info-test gpu-user-dir-test push-userdata-test macos-perf-test depth-bounds-shader-test shader-inputs-test flatten-base-shape-test)
        target_include_directories(${bounds_target} PRIVATE shim shadps4 third_party/gcn/include)
        target_compile_options(${bounds_target} PRIVATE -UNDEBUG -fPIC)
        target_link_libraries(${bounds_target} PRIVATE bbgpu)
    endforeach()
    set_source_files_properties(../tests/test_buffer_bounds.cpp ../tests/test_buffer_bounds_benchmark.cpp
        ../tests/test_buffer_dead_resources.cpp
        PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
    target_compile_definitions(host-import-arena-test PRIVATE VK_USE_PLATFORM_METAL_EXT)
    # gcn_buffer_conformance/gcn_asm.h assembles the test's GCN fetch shaders.
    target_include_directories(shader-permutation-info-test PRIVATE ../tests)
    target_include_directories(flatten-base-shape-test PRIVATE ../tests)
    # Offline sweep of the game's shader corpus through the production pipeline cache
    # (tools/macos/shader_sweep, run by sweep.py; not a --test target).
    add_executable(shader-sweep EXCLUDE_FROM_ALL ../tools/macos/shader_sweep/sweep.cpp
        ../tests/test_gpu_runtime_stubs.cpp)
    target_include_directories(shader-sweep PRIVATE shim shadps4 third_party/gcn/include)
    # Production NDEBUG: the driver shares Vulkan-Hpp and renderer types with libbbgpu.
    target_compile_options(shader-sweep PRIVATE -fPIC)
    target_link_libraries(shader-sweep PRIVATE bbgpu)
    target_include_directories(depth-bounds-shader-test PRIVATE third_party/sirit/externals/SPIRV-Headers/include)
    # Unit tests of the GPU library's Apple paths (build.sh --test runs each).
    add_executable(cmd-snapshot-test EXCLUDE_FROM_ALL ../tests/test_cmd_snapshot.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(cond-exec-test EXCLUDE_FROM_ALL ../tests/test_cond_exec.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(strmout-update-test EXCLUDE_FROM_ALL ../tests/test_strmout_update.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(equeue-concurrency-test EXCLUDE_FROM_ALL ../tests/test_equeue_concurrency.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(feature-checks-test EXCLUDE_FROM_ALL ../tests/test_feature_checks.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(image-view-storage-test EXCLUDE_FROM_ALL ../tests/test_image_view_storage.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(submit-info-bounds-test EXCLUDE_FROM_ALL ../tests/test_submit_info_bounds.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(vertex-fetch-translation-test EXCLUDE_FROM_ALL ../tests/test_vertex_fetch_translation.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(buffer-null-test EXCLUDE_FROM_ALL ../tests/test_buffer_null.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(rect-interface-test EXCLUDE_FROM_ALL ../tests/test_rect_interface.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(fence-address-test EXCLUDE_FROM_ALL ../tests/test_fence_address.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(overlay-concurrency-test EXCLUDE_FROM_ALL ../tests/test_overlay_concurrency.cpp ../tests/test_gpu_runtime_stubs.cpp)
    add_executable(depth-bounds-probe-test EXCLUDE_FROM_ALL ../tests/macos_depth_bounds/probe.cpp ../tests/test_gpu_runtime_stubs.cpp)
    set(apple_unit_test_targets cmd-snapshot-test cond-exec-test strmout-update-test equeue-concurrency-test feature-checks-test image-view-storage-test submit-info-bounds-test vertex-fetch-translation-test buffer-null-test rect-interface-test fence-address-test overlay-concurrency-test depth-bounds-probe-test)
    foreach(unit_target IN LISTS apple_unit_test_targets)
        target_include_directories(${unit_target} PRIVATE shim shadps4 third_party/gcn/include)
        target_compile_options(${unit_target} PRIVATE -UNDEBUG -fPIC)
        target_link_libraries(${unit_target} PRIVATE bbgpu)
    endforeach()
    # gcn_buffer_conformance/gcn_asm.h; sirit and Vulkan for the rect interface test.
    target_include_directories(vertex-fetch-translation-test PRIVATE ../tests)
    target_include_directories(rect-interface-test PRIVATE third_party/sirit/include
        third_party/sirit/externals/SPIRV-Headers/include)
    target_link_libraries(rect-interface-test PRIVATE Vulkan::Vulkan)
    target_include_directories(depth-bounds-probe-test PRIVATE third_party/sirit/include
        third_party/sirit/externals/SPIRV-Headers/include)
    target_link_libraries(depth-bounds-probe-test PRIVATE Vulkan::Vulkan)
    target_include_directories(overlay-concurrency-test PRIVATE third_party/imgui third_party/imgui/backends)
    # The test compiles the overlay itself, which embeds the overlay font.
    target_compile_definitions(overlay-concurrency-test PRIVATE
        BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(test_target motion-shader-test scene-resolution-test taa-shader-test camera-motion-test null-descriptor-test buffer-bounds-test buffer-bounds-benchmark buffer-dead-resources-test host-import-arena-test vertex-pulling-pipeline-test shader-permutation-info-test gpu-user-dir-test push-userdata-test macos-perf-test depth-bounds-shader-test shader-inputs-test flatten-base-shape-test shader-sweep ${apple_unit_test_targets})
        add_custom_command(TARGET ${test_target} POST_BUILD
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/macos/check-exports.py
                $<TARGET_FILE:bbgpu> $<TARGET_FILE:${test_target}> VERBATIM)
    endforeach()
endif()

if (APPLE)
    get_property(darwin_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(darwin_target IN LISTS darwin_targets)
        get_target_property(darwin_kind ${darwin_target} TYPE)
        if (darwin_kind STREQUAL "EXECUTABLE" OR darwin_kind STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET ${darwin_target} POST_BUILD
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/macos/sign.py
                    $<TARGET_FILE:${darwin_target}> VERBATIM)
        endif()
    endforeach()
endif()
