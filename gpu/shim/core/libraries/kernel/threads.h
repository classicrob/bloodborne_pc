// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets guest TLS before running; Apple callbacks also use a low stack.
#pragma once
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
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
    struct RunState {
        std::mutex mutex;
        std::condition_variable completed;
        std::jthread handle;
        std::thread::id id;
        std::stop_source stop;
        bool joining{};
        bool joined{};
    };

public:
    Thread() = default;
    ~Thread() {
        const auto run = Current();
        if (run) {
            std::lock_guard lock(run->mutex);
            if (!run->joined && run->id == std::this_thread::get_id()) {
                Fail("worker cannot destroy its own Thread owner");
            }
        }
        Stop();
    }
    void Run(std::function<void(std::stop_token)>&& func) {
        std::lock_guard start_lock(start_mutex);
        if (const auto previous = Current()) {
            std::lock_guard lock(previous->mutex);
            if (!previous->joined && previous->id == std::this_thread::get_id()) {
                Fail("worker cannot replace its own Thread");
            }
        }
        Stop();
        try {
            auto run = std::make_shared<RunState>();
            std::lock_guard owner_lock(owner_mutex);
            std::lock_guard run_lock(run->mutex);
            current = run;
            run->handle = std::jthread([func = std::move(func)](std::stop_token stop) {
#ifdef __APPLE__
                struct Call {
                    const std::function<void(std::stop_token)>* function;
                    std::stop_token token;
                };
                Call call{&func, stop};
                runtime_thread_run_host([](void* arg) {
                    const auto& call = *static_cast<Call*>(arg);
                    try {
                        (*call.function)(call.token);
                    } catch (...) {
                        Fail("exception in macOS HLE callback worker");
                    }
                }, &call);
#else
                runtime_thread_attach_host("bb:hle");
                func(stop);
#endif
            });
            run->id = run->handle.get_id();
            run->stop = run->handle.get_stop_source();
        } catch (...) {
            Fail("cannot create HLE callback worker");
        }
    }

    // One caller owns the OS join. Other callers wait for that same run's completion.
    // A worker may request its own stop, but only its external owner joins it.
    void Join() {
        const auto run = Current();
        if (!run) return;
        std::unique_lock lock(run->mutex);
        if (run->joined || run->id == std::this_thread::get_id()) return;
        if (run->joining) {
            run->completed.wait(lock, [&] { return run->joined; });
            return;
        }
        run->joining = true;
        auto handle = std::move(run->handle);
        lock.unlock();
        try {
            if (handle.joinable()) handle.join();
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "STOP: HLE worker join failed: %s\n", error.what());
            std::abort();
        }
        lock.lock();
        run->joined = true;
        lock.unlock();
        run->completed.notify_all();
    }

    bool Joinable() const {
        const auto run = Current();
        if (!run) return false;
        std::lock_guard lock(run->mutex);
        return !run->joined;
    }

    void RequestStop() {
        const auto run = Current();
        if (!run) return;
        std::stop_source stop;
        {
            std::lock_guard lock(run->mutex);
            stop = run->stop;
        }
        stop.request_stop();
    }

    void Stop() {
        RequestStop();
        Join();
    }

private:
    [[noreturn]] static void Fail(const char* reason) {
        std::fprintf(stderr, "STOP: %s\n", reason);
        std::abort();
    }
    std::shared_ptr<RunState> Current() const {
        std::lock_guard lock(owner_mutex);
        return current;
    }
    mutable std::mutex owner_mutex;
    std::mutex start_mutex;
    std::shared_ptr<RunState> current;
};
} // namespace Libraries::Kernel
