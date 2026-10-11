// bbport (macOS): publishing flips in flip order, each once the GPU retired its frame copy.
//
// A flip tells the guest its previous display buffer may be reused (status, events, previous
// buffer label). With the mode on, a flip is published once the GPU passed the timeline value of
// the presenter's copy of the frame (its last read of the guest framebuffer). With it off a flip
// is published at once, unless earlier ones are still queued: then it queues behind them, so flips
// are published in order across a live switch. Entries leave the queue only after they were
// published, so a flip submitted meanwhile still sees the queue non-empty.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace BbFlip {

template <typename Entry>
class Publisher {
public:
    /// `wait(tick, stop)`: until the GPU passed `tick`; false if stopped first.
    using WaitFn = std::function<bool(std::uint64_t, const std::stop_token&)>;
    using PublishFn = std::function<void(const Entry&)>;

    Publisher(WaitFn wait_, PublishFn publish_)
        : wait{std::move(wait_)}, publish{std::move(publish_)},
          thread{[this](std::stop_token stop) { Run(stop); }} {}

    ~Publisher() {
        Stop();
    }

    /// In flip order, by one thread. `tick`: 0 when there is nothing to wait for.
    void Submit(Entry entry, std::uint64_t tick, bool at_retire) {
        std::unique_lock lock{mutex};
        if (!at_retire && queue.empty()) {
            lock.unlock();
            publish(entry);
            return;
        }
        queue.push_back({std::move(entry), at_retire ? tick : 0});
        lock.unlock();
        cv.notify_one();
    }

    /// Stops and joins the publishing thread; returns how many flips were left unpublished.
    std::size_t Stop() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
        std::scoped_lock lock{mutex};
        return queue.size();
    }

    std::size_t Pending() {
        std::scoped_lock lock{mutex};
        return queue.size();
    }

private:
    struct Queued {
        Entry entry;
        std::uint64_t tick;
    };

    void Run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Queued next;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, stop, [this] { return !queue.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                next = queue.front();
            }
            if (next.tick && !wait(next.tick, stop)) {
                return;
            }
            publish(next.entry);
            std::scoped_lock lock{mutex};
            queue.pop_front();
        }
    }

    WaitFn wait;
    PublishFn publish;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Queued> queue;
    std::jthread thread; ///< last: stops before the queue goes
};

} // namespace BbFlip
