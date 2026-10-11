// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
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

static void BulkOutput() {
    EqueueInternal q{5, "bulk-output"};
    std::array<OrbisKernelEvent, 33> output;
    for (u64 id = 1; id <= output.size(); ++id) {
        Add(q, id, OrbisKernelEvent::GraphicsCore);
        q.TriggerEvent(id, OrbisKernelEvent::GraphicsCore, reinterpret_cast<void*>(id + 100));
    }
    const OrbisKernelUseconds zero = 0;
    Check(q.WaitForEvents(output.data(), output.size(), &zero) == static_cast<int>(output.size()),
          "large staged request is not truncated");
    for (unsigned i = 0; i < output.size(); ++i)
        Check(output[i].ident == i + 1 && output[i].data == i + 101, "large staged payload");
    Check(Poll(q, output[0]) == 0, "bulk consumption cleared all events");
    std::puts("PASS: 33-event request retains full capacity and payloads");
}

static void* protected_output;
static size_t output_page_size;
static int fault_notify, fault_resume;
static void OutputFault(int, siginfo_t* info, void*) {
    const auto at = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto first = reinterpret_cast<uintptr_t>(protected_output);
    if (at < first || at - first >= output_page_size) _exit(90);
    char token = 'f';
    // The producer must acquire the queue mutex before allowing this copy to resume.
    if (write(fault_notify, &token, 1) != 1 || read(fault_resume, &token, 1) != 1 ||
        mprotect(protected_output, output_page_size, PROT_READ | PROT_WRITE)) _exit(91);
}

static void ProtectedOutput(bool poll) {
    const pid_t child = fork();
    Check(child >= 0, "fork protected output control");
    if (!child) {
        alarm(5);
        EqueueInternal q{4, "protected-output"};
        Add(q, 1, OrbisKernelEvent::GraphicsCore);
        Add(q, 2, OrbisKernelEvent::GraphicsCore);
        q.TriggerEvent(1, OrbisKernelEvent::GraphicsCore, reinterpret_cast<void*>(7));
        output_page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        protected_output = mmap(nullptr, output_page_size, PROT_READ,
                                MAP_PRIVATE | MAP_ANON, -1, 0);
        Check(protected_output != MAP_FAILED, "protected output mapping");
        int notify[2], resume[2];
        Check(!pipe(notify) && !pipe(resume), "fault coordination pipes");
        fault_notify = notify[1]; fault_resume = resume[0];
        struct sigaction sa{};
        sa.sa_sigaction = OutputFault; sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        Check(!sigaction(SIGBUS, &sa, nullptr) && !sigaction(SIGSEGV, &sa, nullptr), "output fault handler");
        std::thread trigger([&] {
            char token;
            Check(read(notify[0], &token, 1) == 1, "wait for output fault");
            Check(q.TriggerEvent(2, OrbisKernelEvent::GraphicsCore, reinterpret_cast<void*>(8)),
                  "trigger while guest output copy is faulted");
            Check(write(resume[1], &token, 1) == 1, "resume faulted output copy");
        });
        const OrbisKernelUseconds zero = 0;
        auto* output = static_cast<OrbisKernelEvent*>(protected_output);
        Check(q.WaitForEvents(output, 1, poll ? &zero : nullptr) == 1 && output->data == 7,
              "protected output copy resumed");
        trigger.join();
        OrbisKernelEvent next{};
        Check(Poll(q, next) == 1 && next.ident == 2 && next.data == 8 && Poll(q, next) == 0,
              "concurrent trigger retained exactly once");
        _exit(0);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child, "join protected output child");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          poll ? "poll output fault blocked concurrent trigger" : "blocking output fault blocked concurrent trigger");
    std::printf("PASS: %s guest output fault allows concurrent trigger and resumes\n", poll ? "poll" : "blocking");
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
    BulkOutput();
    ProtectedOutput(true);
    ProtectedOutput(false);
    MixedConsumers();
}
