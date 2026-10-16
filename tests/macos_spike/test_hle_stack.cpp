#include "core/libraries/kernel/threads.h"
#include <cassert>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../test_alarm.h"
extern "C" {
void *runtime_low_map(size_t,int);
void runtime_set_main_tls(const void *,uint64_t,uint64_t,uint64_t);
uint32_t runtime_thread_tcb_offset(void);
int32_t *runtime_errno(void);
}
static uintptr_t (*load_tcb)();
static uintptr_t observed_stack;
static void check_stack(void *) {
    char local;
    observed_stack=reinterpret_cast<uintptr_t>(&local);
    assert(observed_stack<(UINT64_C(1)<<40));
    auto tcb=reinterpret_cast<uint64_t *>(load_tcb());
    assert(tcb && tcb[0]==reinterpret_cast<uintptr_t>(tcb));
    assert(tcb[0]<(UINT64_C(1)<<40));
    assert(reinterpret_cast<uintptr_t>(runtime_errno())<(UINT64_C(1)<<40));
}
int main() {
    test_alarm(30);
    rlimit lim{0,0}; assert(!setrlimit(RLIMIT_CORE,&lim));
    const unsigned char op[]{0x65,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
    auto code=static_cast<unsigned char *>(runtime_low_map(4096,PROT_READ|PROT_WRITE));
    assert(code); std::copy(std::begin(op),std::end(op),code);
    uint32_t offset=runtime_thread_tcb_offset();
    for(unsigned i=0;i<4;i++) code[5+i]=static_cast<unsigned char>(offset>>(8*i));
    assert(!mprotect(code,4096,PROT_READ|PROT_EXEC));
    load_tcb=reinterpret_cast<uintptr_t (*)()>(code);
    runtime_set_main_tls(nullptr,0,64,16);
    char native_stack;
    auto native_address=reinterpret_cast<uintptr_t>(&native_stack);
    runtime_thread_run_host(check_stack,nullptr);
    auto first_stack=observed_stack;
    for(unsigned i=0;i<100;i++) {
        runtime_thread_run_host(check_stack,nullptr);
        assert(observed_stack==first_stack);
    }
    assert(reinterpret_cast<uintptr_t>(&native_stack)==native_address && first_stack!=native_address);
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        Libraries::Kernel::Thread bad;
        bad.Run([](std::stop_token) { throw std::runtime_error("test worker failure"); });
        bad.Join(); _exit(99);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    std::atomic<unsigned> ready{},stopped{};
    uintptr_t tcbs[3]{};
    Libraries::Kernel::Thread workers[3];
    for(unsigned i=0;i<3;i++) workers[i].Run([&,i](std::stop_token stop) {
        char local;
        assert(reinterpret_cast<uintptr_t>(&local)<(UINT64_C(1)<<40));
        tcbs[i]=load_tcb();
        *runtime_errno()=static_cast<int32_t>(i+1);
        ready.fetch_add(1);
        while(!stop.stop_requested()) std::this_thread::yield();
        assert(*runtime_errno()==static_cast<int32_t>(i+1));
        stopped.fetch_add(1);
    });
    while(ready.load()!=3) std::this_thread::yield();
    for(auto& worker:workers) { assert(worker.Joinable()); worker.Stop(); assert(!worker.Joinable()); }
    assert(stopped.load()==3 && tcbs[0]!=tcbs[1] && tcbs[0]!=tcbs[2] && tcbs[1]!=tcbs[2]);
    std::atomic<bool> start{},done{};
    Libraries::Kernel::Thread self;
    self.Run([&](std::stop_token stop) {
        while(!start.load()) std::this_thread::yield();
        self.Stop(); assert(stop.stop_requested()); check_stack(nullptr); done.store(true);
    });
    start.store(true);
    while(!done.load()) std::this_thread::yield();
    self.Join();
    assert(!self.Joinable());
    for (unsigned trial=0; trial<64; ++trial) {
        Libraries::Kernel::Thread target;
        std::atomic<unsigned> joining{},joined{};
        std::atomic<bool> release{},returned{};
        target.Run([&](std::stop_token stop) {
            while(!release.load()) std::this_thread::yield();
            target.Stop();
            assert(stop.stop_requested());
            target.Join();
            returned.store(true);
        });
        auto joiner=[&] {
            joining.fetch_add(1);
            target.Join();
            assert(returned.load());
            joined.fetch_add(1);
        };
        std::thread first(joiner),second(joiner);
        while(joining.load()!=2) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(joined.load()==0);
        target.RequestStop();
        release.store(true);
        first.join();second.join();
        target.Join();target.Stop();
        assert(joined.load()==2 && !target.Joinable());
        target.Run([](std::stop_token) {});
        target.Join();
    }
    std::atomic<bool> destructor_ready{},callback_destroyed{};
    {
        Libraries::Kernel::Thread owned;
        owned.Run([&](std::stop_token stop) {
            struct Guard {
                std::atomic<bool>& done;
                ~Guard() { done.store(true); }
            } guard{callback_destroyed};
            destructor_ready.store(true);
            while(!stop.stop_requested()) std::this_thread::yield();
        });
        while(!destructor_ready.load()) std::this_thread::yield();
    }
    assert(callback_destroyed.load());
    test_alarm(0);
    std::puts("macOS HLE stacks: real low allocator/TLS, reuse, 3 workers, stop/join, self-stop, concurrent/repeated joins, destruction order, loud exception passed");
}
