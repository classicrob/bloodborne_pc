/* BB_TOGGLE_FILE: one or two decimal masks, re-read live into both toggle words. */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include <unistd.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static char path[64];
static void put(const char *text) {
    char staged[80];
    snprintf(staged,sizeof staged,"%s.new",path);
    FILE *f=fopen(staged,"w");
    assert(f && fputs(text,f)>=0 && !fclose(f));
    assert(!rename(staged,path));
}
/* Waits up to 2 s (the watcher reads every 250 ms) for both words. */
static void expect(const char *what, uint64_t mask, uint64_t mask2) {
    for (int i=0;i<200;++i) {
        if (__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_ACQUIRE)==mask &&
            __atomic_load_n(&runtime_disabled_optimizations2,__ATOMIC_ACQUIRE)==mask2) {
            printf("PASS %s\n",what); fflush(stdout); return;
        }
        struct timespec t={0,10000000}; nanosleep(&t,NULL);
    }
    fprintf(stderr,"FAIL %s: mask=%llu mask2=%llu, expected %llu %llu\n",what,
        (unsigned long long)runtime_disabled_optimizations,(unsigned long long)runtime_disabled_optimizations2,
        (unsigned long long)mask,(unsigned long long)mask2);
    exit(1);
}
int main(void) {
    alarm(30);
    uint64_t m[2];
    assert(!toggle_parse("",m) && m[0]==0 && m[1]==0);
    assert(!toggle_parse("12\n",m) && m[0]==12 && m[1]==0);
    assert(!toggle_parse(" 9223372036854775808 \t 3\r\n",m) && m[0]==1ull<<63 && m[1]==3);
    assert(!toggle_parse("18446744073709551615 18446744073709551615",m) && m[0]==UINT64_MAX && m[1]==UINT64_MAX);
    assert(!toggle_parse("1\n2\n",m) && m[0]==1 && m[1]==2);
    const char *bad[]={"x","12x","-1","+1","1 2 3","0x10","18446744073709551616","1 -2","1,2"};
    for (size_t i=0;i<sizeof bad/sizeof bad[0];++i) assert(toggle_parse(bad[i],m)==-1);
    puts("PASS parse"); fflush(stdout);
    char bits[256];
    toggle_bits(bits,sizeof bits,"bits",0); assert(!strcmp(bits,""));
    toggle_bits(bits,sizeof bits,"bits",(1ull<<59)|9); assert(!strcmp(bits," (bits 0 3 59)"));
    toggle_bits(bits,sizeof bits,"word 2 bits",UINT64_MAX); assert(strlen(bits)<sizeof bits && bits[strlen(bits)-1]==')');
    toggle_bits(bits,24,"bits",UINT64_MAX); assert(strlen(bits)==23);
    puts("PASS bit list"); fflush(stdout);

    snprintf(path,sizeof path,"/tmp/bb-toggle-test.%d",(int)getpid());
    put("5 7\n");
    pthread_t watcher;
    assert(!pthread_create(&watcher,NULL,toggle_watcher,path));
    expect("two masks at start",5,7);
    put("1152921504606846976\n");
    expect("one mask: word 2 cleared",1ull<<60,0);
    put("0 1\n");
    expect("word 2 only",0,1);
    put("3 garbage\n");
    struct timespec t={0,600000000}; nanosleep(&t,NULL);
    expect("unparsed file keeps both words",0,1);
    put("");
    expect("empty file: both 0",0,0);
    put("8 16");
    expect("recovers after the empty file",8,16);
    assert(!unlink(path));
    expect("missing file: both 0",0,0);
    return 0;
}
