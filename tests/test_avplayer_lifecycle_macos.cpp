// Actual AvPlayer demux/decode/Stop with real FFmpeg and low-stack HLE workers.
// Logging, path translation and GPU invalidation are isolated from the renderer.
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/memory.h"
#include <cassert>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

extern "C" {
void runtime_set_main_tls(const void*, uint64_t, uint64_t, uint64_t);
void* runtime_guest_malloc(size_t);
void runtime_guest_free(void*);
int runtime_file_translate(const char* path, char* out, size_t size) {
    return std::snprintf(out, size, "%s", path) < int(size) ? 0 : -1;
}
}
namespace Common {
void SetCurrentThreadName(const char* name) { assert(!pthread_setname_np(name)); }
}
namespace Common::Log {
bool ShouldLog(Level level) { return level >= Level::err; }
void Write(std::string_view, Level, const char*, int, const char*, std::string&& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
}
}
namespace Core {
void MemoryManager::InvalidateMemory(VAddr, u64) {}
}
using namespace Libraries::AvPlayer;
using namespace std::chrono_literals;
static std::atomic<unsigned> allocations{}, frees{};
static void* PS4_SYSV_ABI allocate(void*, u32 alignment, u32 size) {
    char local;
    assert(uintptr_t(&local) < (1ULL << 40));
    auto raw = runtime_guest_malloc(size + alignment + sizeof(void*));
    assert(raw);
    auto aligned = (uintptr_t(raw) + sizeof(void*) + alignment - 1) & ~(uintptr_t(alignment) - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    ++allocations;
    return reinterpret_cast<void*>(aligned);
}
static void PS4_SYSV_ABI deallocate(void*, void* pointer) {
    ++frees;
    runtime_guest_free(static_cast<void**>(pointer)[-1]);
}
struct State : AvPlayerStateCallback {
    std::atomic<unsigned> eof{}, errors{};
    AvPlayerAvSyncMode GetSyncMode() override { return AvPlayerAvSyncMode::None; }
    void OnWarning(u32) override {}
    void OnError() override { ++errors; }
    void OnEOF() override { ++eof; }
};
struct Run { const char* path; bool natural; bool destructor_only; };
static void exercise(void* opaque) {
    auto run = *static_cast<Run*>(opaque);
    State state;
    {
        AvPlayerSource source(state);
        AvPlayerInitData init{};
        init.memory_replacement = {nullptr, allocate, deallocate, allocate, deallocate};
        init.num_output_video_framebuffers = 2;
        assert(source.Init(init, run.path));
        assert(source.FindStreams());
        for (int i = 0; i < source.GetStreamCount(); ++i) assert(source.EnableStream(i));
        assert(source.Start());
        assert(source.Start()); // Does not replace running decoder handles.
        if (run.natural) {
            unsigned audio = 0, video = 0;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!state.eof.load()) {
                AvPlayerFrameInfo frame{};
                audio += source.GetAudioData(frame);
                video += source.GetVideoData(frame);
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(1ms);
            }
            assert(audio > 0 && video > 0 && state.eof == 1);
        } else {
            // Leave both output queues full: cancellation must release decoders
            // waiting for consumer-owned buffers before the demuxer can finish.
            std::this_thread::sleep_for(15ms);
        }
        if (!run.destructor_only) {
            std::jthread first([&] { assert(source.Stop()); });
            std::jthread second([&] { assert(source.Stop()); });
            first.join(); second.join();
            assert(source.Stop());
        }
    }
    assert(state.errors == 0);
    assert(state.eof == unsigned(run.natural));
    assert(allocations == frees);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    alarm(45);
    runtime_set_main_tls(nullptr, 0, 64, 16);
    for (unsigned i = 0; i < 12; ++i) {
        Run run{argv[1], i % 3 == 0, i % 3 == 2};
        runtime_thread_run_host(exercise, &run);
    }
    std::puts("PASS: actual AvPlayer natural EOF, concurrent/repeated Stop, full-buffer cancellation, destruction, low callbacks");
}
