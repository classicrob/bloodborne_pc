// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#include "../src/runtime_hle_trace.h"
#include "../src/runtime_cpu_profile.h"
#include "../src/runtime_memory.h"
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
extern __thread sigjmp_buf* runtime_fault_recover;
__thread sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0; // as runtime_memory.c and bbport_toggles.h
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
#ifdef __APPLE__
int runtime_memory_backing_region(uintptr_t, uintptr_t*, uintptr_t*, uint64_t*) { std::abort(); }
void runtime_memory_set_gpu_lifetime_hooks(void (*)(void), void (*)(uintptr_t, uint64_t), void (*)(void)) { std::abort(); }
int runtime_memory_alias_snapshot(uintptr_t, RuntimeMemoryAlias*, size_t, RuntimeMemoryAliasSnapshot*) { std::abort(); }
void runtime_hle_trace_dump(void) { std::abort(); }
void runtime_hle_trace_frame(void) {}
void runtime_cpu_profile_thread(const char*) {}
#endif
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void* runtime_guest_malloc(size_t) { std::abort(); }
void runtime_guest_free(void*) { std::abort(); }
void runtime_thread_run_host(void (*)(void*), void*) { std::abort(); }
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
}

