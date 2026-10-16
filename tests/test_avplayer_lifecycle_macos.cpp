// Actual AvPlayer demux/decode/Stop with real FFmpeg and low-stack HLE workers.
// Logging, path translation and GPU invalidation are isolated from the renderer.
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/libraries/avplayer/avplayer_state.h"
#include "core/memory.h"
#include <cassert>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include "test_alarm.h"

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
namespace Libraries::Kernel {
int PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(int* version) {
    *version = 0x04500000;
    return 0;
}
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
struct Poller {
    AvPlayerSource* source;
    std::atomic<bool>* stopped;
    std::atomic<unsigned>* calls;
    std::atomic<unsigned>* frames;
    bool audio;
};
static void poll_frames(void* opaque) {
    auto& p = *static_cast<Poller*>(opaque);
    while (!p.stopped->load()) {
        AvPlayerFrameInfo info{};
        bool got = p.audio ? p.source->GetAudioData(info) : p.source->GetVideoData(info);
        if (got) ++*p.frames;
        p.source->CurrentTime();
        p.source->IsActive();
        ++*p.calls;
        std::this_thread::yield();
    }
    // No frame/clock state survives a completed Stop; failure preserves output.
    for (unsigned i = 0; i < 128; ++i) {
        AvPlayerFrameInfo info{};
        info.timestamp = 0x12345678;
        assert(!(p.audio ? p.source->GetAudioData(info) : p.source->GetVideoData(info)));
        assert(info.timestamp == 0x12345678);
        AvPlayerFrameInfoEx ex{};
        ex.timestamp = 0x12345678;
        assert(!p.source->GetVideoData(ex) && ex.timestamp == 0x12345678);
        assert(p.source->CurrentTime() == 0 && !p.source->IsActive());
    }
}
static void polling_stop(void* path) {
    State state;
    AvPlayerSource source(state);
    AvPlayerInitData init{};
    init.memory_replacement = {nullptr, allocate, deallocate, allocate, deallocate};
    init.num_output_video_framebuffers = 2;
    assert(source.Init(init, static_cast<const char*>(path)));
    assert(source.FindStreams());
    for (int i = 0; i < source.GetStreamCount(); ++i) assert(source.EnableStream(i));
    assert(source.Start());
    std::atomic<bool> stopped{};
    std::atomic<unsigned> calls{}, audio_frames{}, video_frames{};
    Poller a{&source, &stopped, &calls, &audio_frames, true};
    Poller v{&source, &stopped, &calls, &video_frames, false};
    std::jthread audio([&] { runtime_thread_run_host(poll_frames, &a); });
    std::jthread video([&] { runtime_thread_run_host(poll_frames, &v); });
    while (calls < 128 || !audio_frames || !video_frames) std::this_thread::yield();
    std::this_thread::sleep_for(5ms);
    assert(source.Stop());
    stopped = true;
    audio.join(); video.join();
    assert(source.Stop());
    assert(state.errors == 0 && allocations == frees);
}

struct Events {
    std::atomic<unsigned> ready{}, play{}, stops{};
    std::atomic<bool> hold_stop{}, release_stop{};
};
static void PS4_SYSV_ABI event_callback(void* opaque, AvPlayerEvents event, s32, void*) {
    auto& events = *static_cast<Events*>(opaque);
    if (event == AvPlayerEvents::StateReady) ++events.ready;
    if (event == AvPlayerEvents::StatePlay) ++events.play;
    if (event == AvPlayerEvents::StateStop) {
        unsigned n = ++events.stops;
        // Hold the EOF notification on the controller while explicit Stop runs.
        if (n == 1 && events.hold_stop) {
            while (!events.release_stop) std::this_thread::yield();
        }
    }
}
static void state_lifecycle(void* path) {
    Events events;
    AvPlayerInitData init{};
    init.memory_replacement = {nullptr, allocate, deallocate, allocate, deallocate};
    init.num_output_video_framebuffers = 2;
    init.event_replacement = {&events, event_callback};
    {
        AvPlayerState state(init);
        assert(!state.Start()); // No source.
        assert(state.AddSource(static_cast<const char*>(path), AvPlayerSourceType::FileMp4));
        while (!events.ready) std::this_thread::yield();
        for (int i = 0; i < state.GetStreamCount(); ++i) assert(state.EnableStream(i));
        state.SetAvSyncMode(AvPlayerAvSyncMode::None);
        assert(state.Start());
        assert(state.Start()); // Play/Buffering -> Stop -> Start, no recursive lock.
        assert(events.stops == 1);
        assert(state.Stop());
        assert(events.stops == 2);
        assert(!state.Stop());
        assert(events.stops == 2);
        // A new run owns a new stop-notification claim.
        events.stops = 0;
        events.hold_stop = true;
        assert(state.Start());
        unsigned audio = 0, video = 0;
        while (!events.stops) {
            AvPlayerFrameInfo frame{};
            audio += state.GetAudioData(frame);
            video += state.GetVideoData(frame);
            std::this_thread::yield();
        }
        assert(audio && video);
        assert(state.Stop());
        assert(events.stops == 1); // EOF already claimed this run's notification.
        events.release_stop = true;
    }
    assert(allocations == frees);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    test_alarm(45);
    runtime_set_main_tls(nullptr, 0, 64, 16);
    for (unsigned i = 0; i < 12; ++i) {
        Run run{argv[1], i % 3 == 0, i % 3 == 2};
        runtime_thread_run_host(exercise, &run);
    }
    for (unsigned i = 0; i < 32; ++i)
        runtime_thread_run_host(polling_stop, argv[1]);
    for (unsigned i = 0; i < 4; ++i)
        runtime_thread_run_host(state_lifecycle, argv[1]);
    std::puts("PASS: actual controller restart, EOF callback concurrent with explicit Stop, one notification per run");
    std::puts("PASS: concurrent audio/video pollers versus Stop, stopped output/clock, atomic queues");
    std::puts("PASS: actual AvPlayer natural EOF, concurrent/repeated Stop, full-buffer cancellation, destruction, low callbacks");
}
