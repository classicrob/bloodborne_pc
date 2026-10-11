// bbport (macOS, labels at GPU completion, the default): GPU-visible labels written strictly in the order
// they were issued in the command stream. A label is issued when the stream reaches it, but becomes
// ready (its guest-memory copies done) later and from other threads, possibly after a later label:
// deferred labels go through the copy threads' completion, labels in direct recording mode don't.
// The writer takes them in issue order only, so no label is published ahead of an earlier one.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>

namespace BbLabels {

class Sequencer {
public:
    struct Label {
        std::uint64_t tick;          ///< the GPU timeline value covering the work before it
        std::function<void()> write; ///< writes the label (and raises its interrupt)
    };

    /// In command stream order, by the one thread recording at a time (stage A or B).
    std::uint64_t Issue() {
        std::scoped_lock lk{mutex};
        return next_issue++;
    }

    /// From any thread, once the label's prerequisites (guest copies) are done.
    void Ready(std::uint64_t sequence, std::uint64_t tick, std::function<void()> write) {
        {
            std::scoped_lock lk{mutex};
            ready.emplace(sequence, Label{tick, std::move(write)});
        }
        cv.notify_all();
    }

    /// The writer: blocks until the next label in issue order is ready; empty once stopped.
    std::optional<Label> Next(std::stop_token stop) {
        std::unique_lock lk{mutex};
        cv.wait(lk, stop, [&] { return ready.contains(next_write); });
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        auto node = ready.extract(next_write);
        ++next_write;
        return std::move(node.mapped());
    }

    /// Issued and not taken by Next (ready or not): what a stopped writer leaves unwritten.
    std::uint64_t Outstanding() {
        std::scoped_lock lk{mutex};
        return next_issue - next_write;
    }

private:
    std::mutex mutex;
    std::condition_variable_any cv;
    std::map<std::uint64_t, Label> ready;
    std::uint64_t next_issue = 0;
    std::uint64_t next_write = 0;
};

} // namespace BbLabels
