#include "../src/runtime_audio.c"
#include <assert.h>
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
int main(int argc,char **argv) {
    int timer=argc>1 && !strcmp(argv[1],"--timer");
    if (timer) setenv("BB_AUDIO","none",1); else unsetenv("BB_AUDIO");
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
    assert(elapsed>UINT64_C(440000000) && elapsed<UINT64_C(900000000));
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
        assert(elapsed>UINT64_C(150000000) && elapsed<UINT64_C(250000000));
        assert(!audio_close(main) && !audio_close(bgm));
    }
    printf("PASS: %s audio, 96 paced buffers, volume, timestamps, EINTR and port reopen\n",timer ? "explicit timer" : "real CoreAudio");
    runtime_audio_report();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return 0;
}
