// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: worker threads that build pipelines handed out before they are built (Apple). The
// Metal compile dominates pipeline creation and runs in parallel (docs/macos/pipeline-compile.md);
// whoever needs a pipeline's handle waits for it (Pipeline::WaitBuilt), so no draw is skipped.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class PipelineBuildQueue {
public:
    explicit PipelineBuildQueue(u32 num_threads) {
        for (u32 i = 0; i < num_threads; ++i) {
            workers.emplace_back([this, i] {
                // Named for profiles and crash reports (BB_CPU_PROFILE caches the name).
                Common::SetCurrentThreadName(fmt::format("PipelineBuild {}", i).c_str());
                Run();
            });
        }
    }

    /// Finishes every queued build, then stops the workers.
    ~PipelineBuildQueue() {
        {
            std::scoped_lock lk{mutex};
            stopping = true;
        }
        work_cv.notify_all();
        workers.clear();
    }

    void Post(std::function<void()> build) {
        {
            std::scoped_lock lk{mutex};
            queue.push_back(std::move(build));
            ++pending;
        }
        work_cv.notify_one();
    }

    /// Waits until every build posted so far has finished.
    void WaitIdle() {
        std::unique_lock lk{mutex};
        idle_cv.wait(lk, [this] { return pending == 0; });
    }

private:
    void Run() {
        std::unique_lock lk{mutex};
        while (true) {
            work_cv.wait(lk, [this] { return stopping || !queue.empty(); });
            if (queue.empty()) {
                return; // stopping, nothing left
            }
            auto build = std::move(queue.front());
            queue.pop_front();
            lk.unlock();
            build();
            lk.lock();
            if (--pending == 0) {
                idle_cv.notify_all();
            }
        }
    }

    std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::deque<std::function<void()>> queue;
    u64 pending = 0;
    bool stopping = false;
    std::vector<std::jthread> workers; ///< last: joined first
};

} // namespace Vulkan
