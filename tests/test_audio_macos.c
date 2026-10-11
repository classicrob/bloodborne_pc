#include "../src/runtime.h"
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
static int virtual_clock, inject_interrupt;
static uint64_t virtual_ns;
static unsigned virtual_sleeps;
static int audio_test_clock(clockid_t clock,struct timespec *out) {
    if (!virtual_clock) return clock_gettime(clock,out);
    assert(clock==CLOCK_MONOTONIC);
    out->tv_sec=(time_t)(virtual_ns/1000000000);out->tv_nsec=(long)(virtual_ns%1000000000);
    return 0;
}
static int audio_test_sleep(clockid_t clock,int flags,const struct timespec *at,struct timespec *remaining) {
    if (!virtual_clock) return clock_nanosleep(clock,flags,at,remaining);
    assert(clock==CLOCK_MONOTONIC && flags==TIMER_ABSTIME && !remaining);
    ++virtual_sleeps;
    if (inject_interrupt) { inject_interrupt=0;return EINTR; }
    uint64_t deadline=(uint64_t)at->tv_sec*1000000000+(uint64_t)at->tv_nsec;
    assert(deadline>=virtual_ns);virtual_ns=deadline;return 0;
}
#define clock_gettime audio_test_clock
#undef clock_nanosleep
#define clock_nanosleep audio_test_sleep
#include "../src/runtime_audio.c"
#undef clock_gettime
#undef clock_nanosleep
#include <math.h>
#include <signal.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static volatile sig_atomic_t interruptions;
static void interrupted(int signal) { (void)signal; ++interruptions; }
static void *interrupt_sleep(void *target) {
    struct timespec delay={0,1000000};
    for (unsigned i=0;i<10;++i) { nanosleep(&delay,NULL); pthread_kill(*(pthread_t *)target,SIGUSR1); }
    return NULL;
}
static void check_null_clock(void) {
    virtual_clock=1;virtual_ns=1000000000;
    setenv("BB_AUDIO_OUTPUT","null",1);unsetenv("BB_AUDIO");
    assert(!audio_init());
    int16_t samples[4096]={0};
    const unsigned lengths[]={256,512,2048};
    for (unsigned i=0;i<3;++i) {
        int h=audio_open(1,0,0,lengths[i],48000,1);assert(h>=0);
        uint64_t start=virtual_ns,period=(uint64_t)lengths[i]*1000000000/48000;
        virtual_sleeps=0;inject_interrupt=1;
        for (unsigned j=0;j<96;++j) {
            assert(audio_output(h,samples)==(int32_t)(lengths[i]*2));
            assert(virtual_ns==start+j*period);
        }
        assert(virtual_sleeps==96 && !inject_interrupt);
        assert(!audio_close(h));
    }
    int a=audio_open(1,0,0,256,48000,1),b=audio_open(1,1,0,256,48000,1);
    assert(a>=0 && b>=0);
    OutputParam outputs[]={{a,samples},{b,samples}};
    uint64_t start=virtual_ns,period=UINT64_C(256000000000)/48000;
    for (unsigned i=0;i<32;++i) {
        assert(audio_outputs(outputs,2)==512);
        assert(virtual_ns==start+i*period);
    }
    virtual_ns+=10*period;start=virtual_ns;
    assert(audio_outputs(outputs,2)==512 && virtual_ns==start);
    assert(audio_outputs(outputs,2)==512 && virtual_ns==start+period);
    assert(!audio_close(a) && !audio_close(b));
    assert(!SDL_WasInit(SDL_INIT_AUDIO));
    puts("PASS: null virtual-clock exact periods, EINTR deadline, batch pacing and stall reset");
}
int main(int argc,char **argv) {
    alarm(90);
    if (argc>1 && !strcmp(argv[1],"--null-clock")) { check_null_clock();return 0; }
    int null_sink=argc>1 && !strcmp(argv[1],"--null");
    int timer=null_sink || (argc>1 && !strcmp(argv[1],"--timer"));
    if (null_sink) { setenv("BB_AUDIO_OUTPUT","null",1); unsetenv("BB_AUDIO"); }
    else if (timer) { setenv("BB_AUDIO","none",1); unsetenv("BB_AUDIO_OUTPUT"); }
    else unsetenv("BB_AUDIO");
    assert(!audio_init());
    int handle=audio_open(1,0,0,256,48000,1);
    assert(handle>=0);
    Port *port=&ports[handle&0xff];
    if (!timer) {
        assert(port->stream && !strcmp(SDL_GetCurrentAudioDriver(),"coreaudio"));
        SDL_AudioDeviceID device=SDL_GetAudioStreamDevice(port->stream);
        assert(device && !SDL_AudioDevicePaused(device));
        printf("Real SDL output device: %s\n",SDL_GetAudioDeviceName(device));
    } else assert(!port->stream);
    if (null_sink) assert(null_output && !SDL_WasInit(SDL_INIT_AUDIO));
    FILE *dump=tmpfile(); assert(dump); port->dump=dump;
    int32_t volume[8]={16384,16384};
    assert(!audio_volume(handle,3,volume));
    int16_t samples[512];
    struct sigaction sa={0}; sa.sa_handler=interrupted; sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1,&sa,NULL));
    pthread_t self=pthread_self(),interruptor;
    assert(!pthread_create(&interruptor,NULL,interrupt_sleep,&self));
    uint64_t start=now_ns(),last=0;
    for (unsigned buffer=0;buffer<96;++buffer) {
        for (unsigned i=0;i<256;++i) {
            double t=(buffer*256+i)/48000.0;
            int16_t v=(int16_t)(1800*sin(2*3.141592653589793*440*t));
            samples[2*i]=samples[2*i+1]=v;
        }
        assert(audio_output(handle,samples)==512);
        uint64_t current=0;
        assert(!audio_last_time(handle,&current) && current>=last); last=current;
    }
    uint64_t elapsed=now_ns()-start;
    assert(!pthread_join(interruptor,NULL));
    assert(interruptions>0);
    printf("Audio pacing: 96 x 256 frames, elapsed %.3f ms (nominal 506.667 ms after first return)\n",elapsed/1e6);
    fflush(stdout);
    /* Background QoS may delay delivery; it must never permit free-running output. */
    assert(elapsed>UINT64_C(440000000));
    if (!timer) assert(elapsed<UINT64_C(900000000));
    assert(!fflush(dump) && ftell(dump)==96*1024);
    assert(!fseek(dump,-1024,SEEK_END));
    int16_t converted[512]; assert(fread(converted,1,sizeof(converted),dump)==sizeof(converted));
    for (unsigned i=0;i<512;++i) assert(converted[i]==(int16_t)(samples[i]*0.5f));
    if (!timer) {
        int queued=SDL_GetAudioStreamQueued(port->stream);
        assert(queued>=0 && queued<32*1024);
        sleep_until(now_ns()+UINT64_C(100000000));
        assert(SDL_GetAudioStreamQueued(port->stream)==0);
    }
    assert(!audio_close(handle));
    for (unsigned i=0;i<4;++i) {
        int reopened=audio_open(1,0,0,256,48000,1); assert(reopened==handle); assert(!audio_close(reopened));
    }
    if (timer) {
        int main=audio_open(1,0,0,256,48000,1),bgm=audio_open(1,1,0,256,48000,1);
        assert(main>=0 && bgm>=0);
        OutputParam outputs[]={{main,samples},{bgm,samples}};
        start=now_ns();
        for (unsigned i=0;i<32;++i) assert(audio_outputs(outputs,2)==512);
        elapsed=now_ns()-start;
        printf("Audio batch pacing: 32 x 2 ports, elapsed %.3f ms (nominal 165.333 ms)\n",elapsed/1e6);
        assert(elapsed>UINT64_C(150000000));
        assert(!audio_close(main) && !audio_close(bgm));
    }
    if (null_sink) {
        int16_t large_samples[4096]={0};
        const unsigned lengths[]={512,2048};
        for (unsigned i=0;i<2;++i) {
            int h=audio_open(1,0,0,lengths[i],48000,1);assert(h>=0);
            start=now_ns();
            for (unsigned j=0;j<8;++j) assert(audio_output(h,large_samples)==(int32_t)(2*lengths[i]));
            elapsed=now_ns()-start;
            uint64_t period=(uint64_t)lengths[i]*1000000000/48000;
            printf("Null pacing: 8 x %u frames, elapsed %.3f ms (nominal %.3f ms)\n",lengths[i],elapsed/1e6,7*period/1e6);
            assert(elapsed>=6*period);
            assert(!audio_close(h));
        }
        assert(!SDL_WasInit(SDL_INIT_AUDIO));
    }
    printf("PASS: %s audio, 96 paced buffers, volume, timestamps, EINTR and port reopen\n",null_sink ? "explicit null" : timer ? "explicit timer" : "real CoreAudio");
    runtime_audio_report();
    if (timer && !null_sink) {
        Port *health=&ports[0];
        memset(health,0,sizeof(*health));
        record_queue_health(health,100,0,256);
        record_queue_health(health,100+AUDIO_WARMUP_NS-1,0,128);
        assert(atomic_load(&warmup_starvation)==2 && atomic_load(&warmup_silence_frames)==384);
        record_queue_health(health,100+AUDIO_WARMUP_NS,0,256);
        record_queue_health(health,101+AUDIO_WARMUP_NS,512,0);
        record_queue_health(health,102+AUDIO_WARMUP_NS,128,128);
        assert(atomic_load(&input_starvation)==1 && atomic_load(&inserted_silence_frames)==384);
        assert(health->health_warned);
        runtime_audio_report();
        assert(atomic_load(&input_starvation)==1 && atomic_load(&inserted_silence_frames)==384);
        puts("PASS: cumulative audio health, warmup boundary, silence accounting, report does not reset");
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return 0;
}
