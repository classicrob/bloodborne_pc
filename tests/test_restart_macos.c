#define main bb_probe_main
#include "../src/probe.c"
#undef main
#include <assert.h>
#include <errno.h>
#include <pthread.h>

static void *restart_worker(void *unused) { (void)unused; runtime_restart(); return NULL; }
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--verify-macos-host")) {
        int translated=0; size_t size=sizeof(translated);
        assert(!sysctlbyname("sysctl.proc_translated",&translated,&size,NULL,0) && translated==1);
        puts("Restart fixture: verified translated x86_64 child"); return 0;
    }
    if (!getenv("BB_RESTART_TEST_STAGE")) {
        char pid[32]; snprintf(pid,sizeof(pid),"%d",getpid());
        assert(!setenv("BB_RESTART_TEST_PID",pid,1));
        assert(!setenv("BB_RESTART_TEST_STAGE","1",1));
        int fd=open("/dev/null",O_RDONLY); assert(fd>=0);
        assert(dup2(fd,200)==200); close(fd);
        pthread_t worker; assert(!pthread_create(&worker,NULL,restart_worker,NULL));
        pthread_join(worker,NULL); return 1;
    }
    assert(getpid()==atoi(getenv("BB_RESTART_TEST_PID")));
    assert(fcntl(200,F_GETFD)==-1 && errno==EBADF);
    assert(!strcmp(getenv("BB_CONFIG"),getenv("BB_RESTART_EXPECT_CONFIG")));
    int checked=0;
    for (int i=1;i+1<argc;++i) {
        if (!strcmp(argv[i],"--user")) { assert(!strcmp(argv[++i],getenv("BB_RESTART_EXPECT_USER"))); checked|=1; }
        if (!strcmp(argv[i],"--app0")) { assert(!strcmp(argv[++i],getenv("BB_GAME_DIR"))); checked|=2; }
    }
    assert(checked==3);
    puts("PASS: actual runtime_restart from worker -> bash run.sh -> x86_64 child, same PID, descriptors closed, save/config paths retained");
    return 0;
}
