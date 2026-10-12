#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdint.h>
static _Thread_local int32_t guest_error;
static int32_t *runtime_errno(void) { return &guest_error; }
#include "../src/runtime_player_position_mac.h"

#define IMAGE UINT64_C(0x800000000)
static unsigned char memory[0x5000];
static uint64_t singleton;
static int read_count, change_at;
static bool fake_read(uintptr_t address,void *out,size_t n,void *ctx) {
    (void)ctx;
    if (++read_count==change_at) memory[0x41e0]^=1;
    if (address==IMAGE+0x553e878 && n==8) { memcpy(out,&singleton,8); return true; }
    if (address<0x1000 || address>0x5000 || n>0x5000-address) return false;
    memcpy(out,memory+address,n);
    return true;
}
static void put64(size_t address,uint64_t value) { memcpy(memory+address,&value,8); }
static void setup(void) {
    memset(memory,0,sizeof(memory)); read_count=0; change_at=0;
    singleton=0x1000;
    put64(0x1060,0x2000); put64(0x23b0,0x3000); put64(0x3068,0x4000);
    uint32_t map=0x18010000; memcpy(memory+0x23f8,&map,4);
    float v[8]={0,1.25f,0,0,-193.4f,-28.646f,68.5f,1}; memcpy(memory+0x41d0,v,32);
}
static char output[PATH_MAX];
static char *read_output(void) {
    static char text[2048];
    FILE *f=fopen(output,"r"); assert(f);
    size_t n=fread(text,1,sizeof(text)-1,f); text[n]=0;
    assert(!ferror(f)); assert(fclose(f)==0); return text;
}
static void expect_stop(void (*fn)(void),const char *message) {
    int fds[2]; assert(pipe(fds)==0);
    pid_t p=fork(); assert(p>=0);
    if (!p) { close(fds[0]); assert(dup2(fds[1],STDERR_FILENO)>=0); close(fds[1]); fn(); _Exit(0); }
    close(fds[1]);
    char text[4096]; size_t used=0; ssize_t n;
    while ((n=read(fds[0],text+used,sizeof(text)-1-used))>0) used+=(size_t)n;
    text[used]=0; close(fds[0]);
    int status; assert(waitpid(p,&status,0)==p);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==23);
    assert(strstr(text,message));
    printf("PASS: refusal reason %s\n",message);
}
static void bad_guard(void) { setenv("BB_PLAYER_POS_FILE",output,1); runtime_player_position_init(0,0); }
static void bad_path(void) { setenv("BB_PLAYER_POS_FILE","relative",1); runtime_player_position_init(0,0); }
static void bad_file(void) { strcpy(player_path,"/no-such-bb-player-dir/file"); PlayerObservation o={0}; player_publish(PLAYER_ABSENT,&o,player_now()); }
static void bad_sample(void) { player_image=1; player_last=0; atomic_store(&player_enabled,true); player_position_sample(); }
static bool guard_file_read(uintptr_t at,void *out,size_t n,void *ctx) {
    FILE *f=ctx;
    return at>=IMAGE && fseek(f,(long)(at-IMAGE),SEEK_SET)==0 && fread(out,1,n,f)==n;
}
int main(int argc,char **argv) {
    PlayerObservation o;
    setup(); assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_OK);
    assert(o.player==0x2000 && o.map==0x18010000 && o.position[2]==68.5f && o.rotation[1]==1.25f);
    for (int level=0;level<4;++level) {
        setup(); if (!level) singleton=0; else put64((size_t[]){0,0x1060,0x23b0,0x3068}[level],0);
        assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_ABSENT);
    }
    setup(); put64(0x23b0,UINT64_MAX-16); assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_UNREADABLE);
    setup(); singleton=0x6000; assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_UNREADABLE);
    setup(); change_at=7; assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_CHANGED);
    setup(); float nan=NAN; memcpy(memory+0x41e4,&nan,4); assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_NONFINITE);
    setup(); memcpy(memory+0x41d4,&nan,4); assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_NONFINITE);
    setup(); memset(memory+0x23f8,255,4); assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_NO_MAP);
    assert(!player_code_guard(IMAGE,1,fake_read,NULL));
    assert(!player_code_guard(IMAGE,0x553e880,fake_read,NULL));
    char path[]="/tmp/bb-player-pos-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); close(fd);
    strcpy(output,path); strcpy(player_path,path);
    player_started=player_now(); player_token=123;
    unsetenv("BB_PLAYER_POS_FILE"); runtime_player_position_init(0,0); assert(!atomic_load(&player_enabled));
    setup(); assert(player_observe(fake_read,NULL,IMAGE,&o)==PLAYER_OK);
    player_publish(PLAYER_OK,&o,player_now());
    char *text=read_output(); assert(strstr(text,"\"valid\":true") && strstr(text,"\"sequence\":1") && strstr(text,"\"xyz\":["));
    puts(text);
    player_publish(PLAYER_ABSENT,&o,player_now());
    text=read_output(); assert(strstr(text,"\"valid\":false") && strstr(text,"\"sequence\":2") && !strstr(text,"\"xyz\""));
    assert(strstr(text,"sample_monotonic_ns") && strstr(text,"run_id") && strstr(text,"pad_reads"));
    puts(text);
    expect_stop(bad_guard,"unsupported image: player position code guard failed");
    expect_stop(bad_path,"requires an absolute bounded path");
    expect_stop(bad_file,"cannot create output temporary file");
    expect_stop(bad_sample,"PLAYER-POS STOP: unreadable-player-chain");
    text=read_output(); assert(strstr(text,"unreadable-player-chain") && !strstr(text,"\"xyz\""));
    /* Real self-read over a sparse fake image, exercising the production sampler. */
    size_t n=0x553f000;
    unsigned char *image=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); assert(image!=MAP_FAILED);
    uint64_t pointers[4]={(uintptr_t)image+0x1000,(uintptr_t)image+0x2000,(uintptr_t)image+0x3000,(uintptr_t)image+0x4000};
    memcpy(image+0x553e878,&pointers[0],8); memcpy(image+0x1060,&pointers[1],8);
    memcpy(image+0x23b0,&pointers[2],8); memcpy(image+0x3068,&pointers[3],8);
    setup(); memcpy(image+0x41d0,memory+0x41d0,32); memcpy(image+0x23f8,memory+0x23f8,4);
    player_image=(uintptr_t)image; player_last=0; atomic_store(&player_enabled,true);
    errno=17; guest_error=23; player_position_sample(); assert(errno==17 && guest_error==23);
    text=read_output(); assert(strstr(text,"\"valid\":true"));
    uint64_t sequence=player_sequence; player_position_sample(); assert(player_sequence==sequence);
    assert(atomic_load(&player_pad_reads)==2);
    assert(mprotect(image+0x4000,0x1000,PROT_NONE)==0);
    assert(player_observe(player_mach_read,NULL,(uintptr_t)image,&o)==PLAYER_UNREADABLE);
    munmap(image,n);
    assert(!player_mach_read((uintptr_t)image,&o,8,NULL));
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        assert(player_code_guard(IMAGE,0x553e880,guard_file_read,f)); fclose(f);
        puts("PASS: local 1.09 extracted-image code guards");
    }
    unlink(output);
    puts("PASS: player chain, lifecycle, strict errors, file publication, real Mach reads, rate cap and errno");
    return 0;
}
