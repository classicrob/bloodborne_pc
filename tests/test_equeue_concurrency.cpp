// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include "core/libraries/kernel/equeue.h"

using namespace Libraries::Kernel;
using namespace std::chrono_literals;

namespace Libraries::Kernel {
struct EqueueConcurrencyTest {
    static std::mutex& Mutex(EqueueInternal& q) { return q.m_mutex; }
};
}

static void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static void Add(EqueueInternal& q, u64 id, OrbisKernelEvent::Filter filter, u16 flags = 0) {
    EqueueEvent e{};
    e.event.ident = id;
    e.event.filter = filter;
    e.event.flags = OrbisKernelEvent::Add | flags;
    e.event.udata = reinterpret_cast<void*>(0x1234);
    // Timer scheduling is dormant: this fixture starts no kernel service thread.
    if (filter == OrbisKernelEvent::Timer) e.event.data = 86400000;
    Check(q.AddEvent(e), "register event");
}

static int Poll(EqueueInternal& q, OrbisKernelEvent& e) {
    const OrbisKernelUseconds zero = 0;
    return q.WaitForEvents(&e, 1, &zero);
}

static void PollMustTakeLock() {
    EqueueInternal q{1, "poll-lock"};
    Add(q, 1, OrbisKernelEvent::GraphicsCore);
    Check(q.TriggerEvent(1, OrbisKernelEvent::GraphicsCore, reinterpret_cast<void*>(7)), "trigger");
    std::unique_lock held{EqueueConcurrencyTest::Mutex(q)};
    std::mutex progress_mutex;
    std::condition_variable progress;
    bool entered = false, done = false;
    OrbisKernelEvent e{};
    int count = -1;
    std::thread poller([&] {
        {
            std::lock_guard guard{progress_mutex};
            entered = true;
            progress.notify_one();
        }
        count = Poll(q, e);
        std::lock_guard guard{progress_mutex};
        done = true;
        progress.notify_one();
    });
    bool consumed_while_locked;
    {
        std::unique_lock guard{progress_mutex};
        progress.wait(guard, [&] { return entered; });
        consumed_while_locked = progress.wait_for(guard, 100ms, [&] { return done; });
    }
    held.unlock();
    poller.join();
    Check(!consumed_while_locked, "zero-timeout poll consumed an event while queue mutex was held");
    Check(count == 1 && e.data == 7 && Poll(q, e) == 0, "locked poll consumes once after release");
    std::puts("PASS: zero-timeout consumption honors queue mutex");
}

static void FilterSemantics() {
    OrbisKernelEvent e{};
    EqueueInternal q{2, "filters"};
    Add(q, 1, OrbisKernelEvent::User);
    q.TriggerEvent(1, OrbisKernelEvent::User, reinterpret_cast<void*>(3));
    Check(Poll(q, e) == 1 && Poll(q, e) == 1 && e.udata == reinterpret_cast<void*>(3),
          "level user event stays readable");
    Check(q.RemoveEvent(1, OrbisKernelEvent::User), "remove level event");
    Add(q, 1, OrbisKernelEvent::User, OrbisKernelEvent::Clear);
    q.TriggerEvent(1, OrbisKernelEvent::User, reinterpret_cast<void*>(4));
    q.TriggerEvent(1, OrbisKernelEvent::User, reinterpret_cast<void*>(5));
    Check(Poll(q, e) == 1 && e.udata == reinterpret_cast<void*>(5) && Poll(q, e) == 0,
          "clear user event coalesces, resets, keeps latest udata");
    q.RemoveEvent(1, OrbisKernelEvent::User);
    Add(q, 1, OrbisKernelEvent::User, OrbisKernelEvent::OneShot);
    q.TriggerEvent(1, OrbisKernelEvent::User, nullptr);
    Check(Poll(q, e) == 1 && Poll(q, e) == 0 && !q.EventExists(1, OrbisKernelEvent::User),
          "one-shot event removed after delivery");

    Add(q, 2, OrbisKernelEvent::GraphicsCore);
    q.TriggerEvent(2, OrbisKernelEvent::GraphicsCore, reinterpret_cast<void*>(8));
    q.TriggerEvent(2, OrbisKernelEvent::GraphicsCore, reinterpret_cast<void*>(9));
    Check(Poll(q, e) == 1 && e.data == 9 && e.fflags == 0 &&
          e.udata == reinterpret_cast<void*>(0x1234) && (e.flags & OrbisKernelEvent::Clear) &&
          Poll(q, e) == 0, "GraphicsCore coalesces latest data; no fabricated occurrence count");

    Add(q, 3, OrbisKernelEvent::VideoOut);
    for (unsigned batch : {1u, 3u, 15u, 20u}) {
        for (u64 i = 1; i <= batch; ++i) {
            const u64 hint = (i << 16) | (1u << 8) | 3;
            q.TriggerEvent(3, OrbisKernelEvent::VideoOut, reinterpret_cast<void*>(hint));
        }
        Check(Poll(q, e) == 1 && ((e.data >> 12) & 15) == std::min(batch, 15u) &&
              (e.data >> 16) == batch && Poll(q, e) == 0,
              "VideoOut count saturates at 15, latest flip arg retained, clears after read");
    }
    Add(q, 4, OrbisKernelEvent::Timer);
    for (unsigned i = 0; i < 19; ++i) q.TriggerEvent(4, OrbisKernelEvent::Timer, nullptr);
    Check(Poll(q, e) == 1 && e.data == 19 && Poll(q, e) == 0, "timer occurrences accumulate then clear");
    q.RemoveEvent(4, OrbisKernelEvent::Timer);
    std::puts("PASS: level/Clear/OneShot, EOP coalescing, VideoOut packed counts, timer accumulation");
}

static void MixedConsumers() {
    constexpr unsigned slots = 32, rounds = 128, total = slots * rounds, consumers = 8;
    EqueueInternal q{3, "mixed"};
    std::array<std::atomic<unsigned>, total + 1> seen{};
    std::atomic<unsigned> completed{}, ready{};
    std::atomic<bool> start{}, finish{};
    const auto filter = [](unsigned id) {
        return id % 3 == 0 ? OrbisKernelEvent::GraphicsCore :
               id % 3 == 1 ? OrbisKernelEvent::VideoOut : OrbisKernelEvent::User;
    };
    for (unsigned id = 1; id <= slots; ++id) Add(q, id, filter(id), OrbisKernelEvent::Clear);
    std::array<std::thread, consumers> workers;
    for (unsigned i = 0; i < consumers; ++i) {
        workers[i] = std::thread([&, i] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            while (!finish.load()) {
                OrbisKernelEvent e{};
                const OrbisKernelUseconds timeout = i % 2 ? 2000 : 0;
                const int n = q.WaitForEvents(&e, 1, &timeout);
                Check(n == 0 || n == 1, "wait result count");
                if (!n) { std::this_thread::yield(); continue; }
                const u64 token = e.filter == OrbisKernelEvent::User ? reinterpret_cast<u64>(e.udata) :
                                  e.filter == OrbisKernelEvent::VideoOut ? e.data >> 16 : e.data;
                Check(token && token <= total && e.ident == (token - 1) % slots + 1,
                      "trigger payload and event identity stay paired");
                Check(e.filter == filter(e.ident), "filter identity");
                if (e.filter == OrbisKernelEvent::VideoOut)
                    Check(((e.data >> 12) & 15) == 1, "one outstanding flip per identity");
                Check(seen[token].fetch_add(1) == 0, "duplicate clear-event consumption");
                completed.fetch_add(1);
            }
        });
    }
    std::array<std::thread, 2> triggers;
    for (unsigned p = 0; p < triggers.size(); ++p) {
        triggers[p] = std::thread([&, p] {
            while (!start.load()) std::this_thread::yield();
            for (unsigned r = 0; r < rounds; ++r) {
                for (unsigned id = 1 + p; id <= slots; id += 2) {
                    const unsigned token = r * slots + id;
                    if (r) while (!seen[token - slots].load()) std::this_thread::yield();
                    const u64 data = filter(id) == OrbisKernelEvent::VideoOut ?
                                     (u64(token) << 16) | id : token;
                    Check(q.TriggerEvent(id, filter(id), reinterpret_cast<void*>(data)), "concurrent trigger");
                }
            }
        });
    }
    while (ready.load() != consumers) std::this_thread::yield();
    start.store(true);
    for (auto& t : triggers) t.join();
    while (completed.load() != total) std::this_thread::yield();
    finish.store(true);
    for (auto& t : workers) t.join();
    for (unsigned i = 1; i <= total; ++i) Check(seen[i].load() == 1, "missing clear-event consumption");
    OrbisKernelEvent e{};
    Check(Poll(q, e) == 0, "no stale event after all acknowledgements");
    std::printf("PASS: %u exact-once acknowledged triggers, %u mixed poll/timedwait consumers, 2 producers\n",
                total, consumers);
}

int main() {
    alarm(30);
    std::puts("Equeue concurrency fixture starting");
    std::fflush(stdout);
    PollMustTakeLock();
    FilterSemantics();
    MixedConsumers();
}
