/* BB_TOGGLE_FILE: one or two decimal masks, re-read live into both toggle words. */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static char path[64];
static void put_bytes(const char *text, size_t length) {
    char staged[80];
    snprintf(staged,sizeof staged,"%s.new",path);
    FILE *f=fopen(staged,"w");
    assert(f && fwrite(text,1,length,f)==length && !fclose(f));
    assert(!rename(staged,path));
}
static void put(const char *text) { put_bytes(text,strlen(text)); }
/* toggle_read fails with a reason containing want, and leaves both masks 0. */
static void read_fails(const char *what, const char *want) {
    uint64_t m[2]={1,1};
    char why[160]="";
    assert(toggle_read(path,m,why,sizeof why)==-1 && m[0]==0 && m[1]==0);
    if (!strstr(why,want)) { fprintf(stderr,"FAIL %s: reason '%s', expected '%s'\n",what,why,want); exit(1); }
    printf("PASS %s: %s\n",what,why); fflush(stdout);
}
/* Points path at target with one rename (a symlink), so the file is never missing. */
static void point(const char *target) {
    char staged[80];
    snprintf(staged,sizeof staged,"%s.new",path);
    assert(!symlink(target,staged) && !rename(staged,path));
}
/* After two whole reads of the bad file, both words are as they were. */
static void kept(const char *what, uint64_t mask, uint64_t mask2) {
    const unsigned long start=__atomic_load_n(&toggle_reads,__ATOMIC_ACQUIRE);
    for (int i=0;i<300 && __atomic_load_n(&toggle_reads,__ATOMIC_ACQUIRE)<start+2;++i) {
        struct timespec t={0,10000000}; nanosleep(&t,NULL);
    }
    const uint64_t got=__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_ACQUIRE);
    const uint64_t got2=__atomic_load_n(&runtime_disabled_optimizations2,__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&toggle_reads,__ATOMIC_ACQUIRE)<start+2 || got!=mask || got2!=mask2) {
        fprintf(stderr,"FAIL %s: reads %lu after %lu, mask=%llu mask2=%llu, expected %llu %llu kept\n",what,
            __atomic_load_n(&toggle_reads,__ATOMIC_ACQUIRE),start,(unsigned long long)got,
            (unsigned long long)got2,(unsigned long long)mask,(unsigned long long)mask2);
        exit(1);
    }
    printf("PASS %s keeps both words\n",what); fflush(stdout);
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
    /* The reader: only a missing file reads as no toggles. */
    uint64_t r[2]={1,1}; char why[160];
    assert(!toggle_read(path,r,why,sizeof why) && r[0]==0 && r[1]==0);
    put("4 5\n");
    assert(!toggle_read(path,r,why,sizeof why) && r[0]==4 && r[1]==5);
    put_bytes("1\0garbage",10); read_fails("embedded NUL","NUL byte");
    put_bytes("1\0",2); read_fails("trailing NUL","NUL byte");
    char longer[300]; memset(longer,' ',sizeof longer); longer[sizeof longer-1]='1';
    put_bytes(longer,sizeof longer); read_fails("file over 255 bytes","longer than 255");
    put("1 2\n"); assert(!chmod(path,0)); read_fails("unreadable file (EACCES)","cannot open");
    assert(!unlink(path) && !mkdir(path,0700)); read_fails("directory (read error)","read failed");
    assert(!rmdir(path));
    puts("PASS reader"); fflush(stdout);

    put("5 7\n");
    pthread_t watcher;
    assert(!pthread_create(&watcher,NULL,toggle_watcher,path));
    expect("two masks at start",5,7);
    put("1152921504606846976\n");
    expect("one mask: word 2 cleared",1ull<<60,0);
    put("0 1\n");
    expect("word 2 only",0,1);
    put("3 garbage\n");
    kept("unparsed text",0,1);
    put("");
    expect("empty file: both 0",0,0);
    put("8 16");
    expect("recovers after the empty file",8,16);
    put_bytes("1\0garbage",10);
    kept("binary file with a NUL",8,16);
    put("8 16"); assert(!chmod(path,0));
    kept("unreadable file",8,16);
    char directory[80];
    snprintf(directory,sizeof directory,"%s.dir",path);
    assert(!mkdir(directory,0700));
    point(directory);
    kept("directory",8,16);
    assert(!unlink(path) && !rmdir(directory));
    expect("missing file: both 0",0,0);
    put("2 3");
    expect("readable again",2,3);
    assert(!unlink(path));
    return 0;
}
