// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets guest TLS before running; Apple callbacks also use a low stack.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);
#ifdef __APPLE__
extern "C" void runtime_thread_run_host(void (*entry)(void*), void* arg);
#endif

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
#ifdef __APPLE__
        try {
            finished = std::make_shared<std::atomic<bool>>(false);
            thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
                struct Call { const std::function<void(std::stop_token)>* function; std::stop_token token; };
                Call call{&func, stop};
                runtime_thread_run_host([](void* arg) {
                    const auto& call = *static_cast<Call*>(arg);
                    try {
                        (*call.function)(call.token);
                    } catch (...) {
                        std::fputs("STOP: exception in macOS HLE callback worker\n", stderr);
                        std::abort();
                    }
                }, &call);
                done->store(true);
            });
        } catch (...) {
            std::fputs("STOP: cannot create macOS HLE callback worker\n", stderr);
            std::abort();
        }
#else
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
            done->store(true);
        });
#endif
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // Called from guest code, through which a C++ exception cannot unwind.
    // Apple reports join failures as fatal; other platforms retain their existing recovery.
    void Join() {
        if (!thread.joinable()) return;
        try {
            if (thread.get_id() == std::this_thread::get_id()) thread.detach();
            else thread.join();
        } catch (const std::system_error& error) {
#ifdef __APPLE__
            std::fprintf(stderr, "STOP: macOS HLE worker join/detach failed: %s\n", error.what());
            std::abort();
#else
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            if (thread.get_id() != std::this_thread::get_id()) {
                for (int waited = 0; finished && !finished->load() && waited < 5000; ++waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (thread.joinable()) thread.detach();
#endif
        }
    }
    bool Joinable() const { return thread.joinable(); }
    void Stop() {
        if (thread.joinable()) {
            thread.request_stop();
            Join();
        }
    }

private:
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
