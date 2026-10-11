/* SDK-only checks of the actual backend's imported GPU memory lifetime order. */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static unsigned begins,ends,retires,notifications;
static int active;
typedef struct { uintptr_t address; uint64_t size; unsigned char value; int live; } Import;
static Import imports[16];
static void track(void *address,uint64_t size,unsigned char value) {
    for (unsigned i=0;i<16;++i) if (!imports[i].live) {
        imports[i]=(Import){(uintptr_t)address,size,value,1}; return;
    }
    assert(0);
}
static unsigned live(void) {
    unsigned n=0; for(unsigned i=0;i<16;++i) n+=imports[i].live; return n;
}
static void *reader(void *unused) {
    (void)unused; read_lock(); read_unlock(); return NULL;
}
static void begin(void) {
    assert(!exclusive_depth);
    assert(!pthread_mutex_lock(&gate)); assert(!active); active=1; ++begins;
    /* Joining this reader would deadlock if begin ran with the VMA writer held. */
    pthread_t worker; assert(!pthread_create(&worker,NULL,reader,NULL));
    assert(!pthread_join(worker,NULL));
}
static void retire(uintptr_t address,uint64_t size) {
    assert(active && exclusive_depth && lifetime_depth); ++retires;
    for(unsigned i=0;i<16;++i) {
        Import *p=&imports[i];
        if (!p->live || p->address>=address+size || address>=p->address+p->size) continue;
        /* The old mapping must still be readable and contain the old data. */
        assert(*(unsigned char *)p->address==p->value);
        p->live=0;
    }
}
static void notify(uintptr_t address,uint64_t size) {
    (void)address; (void)size;
    assert(active && !exclusive_depth && lifetime_depth); ++notifications;
}
static void end(void) {
    assert(active && !exclusive_depth && !lifetime_depth && !pending_count);
    active=0; ++ends; assert(begins==ends); assert(!pthread_mutex_unlock(&gate));
}
static void balanced(void) { assert(!active && begins==ends && !lifetime_depth); }
static void alias_diagnostic_test(void) {
    RuntimeMemoryAlias rows[3], saved[3];
    RuntimeMemoryAliasSnapshot snapshot;
    assert(runtime_memory_alias_snapshot(LOW_MIN,NULL,1,&snapshot)==-1);
    assert(runtime_memory_alias_snapshot(LOW_MIN,rows,3,NULL)==-1);
    assert(!runtime_memory_alias_snapshot(LOW_MIN,NULL,0,&snapshot));
    assert(!snapshot.total && !snapshot.copied && !snapshot.truncated);
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,5*PAGE,PAGE,0,&physical));
    void *a=NULL,*b=NULL,*c=NULL;
    assert(!direct_map(&a,5*PAGE,3,0,physical,PAGE));
    assert(!direct_map(&b,2*PAGE,3,0,physical+PAGE,PAGE));
    assert(!direct_map(&c,PAGE,1,0,physical+PAGE,PAGE));
    uintptr_t query=(uintptr_t)a+PAGE+17;
    assert(runtime_memory_alias_snapshot(query,rows,3,&snapshot)==1);
    assert(snapshot.start==(uintptr_t)a && snapshot.end==(uintptr_t)a+5*PAGE);
    assert(snapshot.backing_byte==(uint64_t)physical+PAGE+17 && snapshot.prot==3);
    assert(snapshot.total==2 && snapshot.copied==2 && !snapshot.truncated);
    assert(!(snapshot.generation&1));
    uint64_t generation=snapshot.generation;
    unsigned matches=0;
    for (unsigned i=0;i<2;++i) {
        assert(rows[i].backing_offset==(uint64_t)physical+PAGE);
        if (rows[i].start==(uintptr_t)b) {
            assert(rows[i].end==(uintptr_t)b+2*PAGE && rows[i].address==(uintptr_t)b+17);
            assert(rows[i].prot==3); matches|=1;
        } else if (rows[i].start==(uintptr_t)c) {
            assert(rows[i].end==(uintptr_t)c+PAGE && rows[i].address==(uintptr_t)c+17);
            assert(rows[i].prot==1); matches|=2;
        } else assert(0);
    }
    assert(matches==3 && rows[0].start<rows[1].start);
    memset(rows,0xa5,sizeof(rows)); memcpy(saved,rows,sizeof(rows));
    assert(runtime_memory_alias_snapshot(query,rows,1,&snapshot)==1);
    assert(snapshot.total==2 && snapshot.copied==1 && snapshot.truncated);
    assert(!memcmp(rows+1,saved+1,2*sizeof(*rows)));
    assert(runtime_memory_alias_snapshot(query,NULL,0,&snapshot)==1);
    assert(snapshot.total==2 && !snapshot.copied && snapshot.truncated);
    assert(snapshot.generation==generation);
    /* The adjacent physical byte beyond b must not be reported as aliased. */
    assert(runtime_memory_alias_snapshot((uintptr_t)a+3*PAGE,rows,3,&snapshot)==1);
    assert(!snapshot.total && !snapshot.copied && !snapshot.truncated);
    assert(!direct_release(physical+PAGE,PAGE));
    assert(!runtime_memory_alias_snapshot(query,rows,3,&snapshot));
    assert(snapshot.generation>generation && !snapshot.total && !snapshot.start);
    assert(runtime_memory_alias_snapshot((uintptr_t)a+2*PAGE+9,rows,3,&snapshot)==1);
    assert(snapshot.total==1 && snapshot.copied==1);
    assert(rows[0].address==(uintptr_t)b+PAGE+9);
    assert(rows[0].backing_offset==(uint64_t)physical+2*PAGE);
    assert(!direct_release(physical,PAGE));
    assert(!direct_release(physical+2*PAGE,3*PAGE));
    void *flex=NULL,*reserved=NULL;
    assert(!map_flexible(&flex,PAGE,3,0));
    assert(runtime_memory_alias_snapshot((uintptr_t)flex+7,rows,3,&snapshot)==1);
    assert(snapshot.backing_byte>=POOL_SIZE && !snapshot.total);
    assert(!reserve_range(&reserved,PAGE,0,PAGE));
    assert(!runtime_memory_alias_snapshot((uintptr_t)reserved,rows,3,&snapshot));
    assert(!snapshot.start && !snapshot.backing_byte && !snapshot.total);
    assert(!direct_unmap(flex,PAGE)); assert(!direct_unmap(reserved,PAGE));
    puts("PASS: bounded alias snapshot, physical offsets, truncation, release splits, generations, untracked vs no aliases");
}
static void *audit_address;
static int64_t audit_physical;
static void ambiguous_commit(void) {
    void *p=audit_address;
    (void)direct_map(&p,PAGE,3,MAP_FIXED_FLAG|MAP_NO_OVERWRITE,audit_physical,PAGE);
}
static void ambiguous_reserve(void) {
    void *p=audit_address;
    (void)reserve_range(&p,PAGE,MAP_FIXED_FLAG,PAGE);
}
static void overlapping_insert(void) {
    write_lock();
    size_t i=vma_index((uintptr_t)audit_address);
    (void)vma_insert(i,vmas[i]);
}
static void invalid_table(void) {
    write_lock();
    size_t i=vma_index((uintptr_t)audit_address);
    vmas[i].end=vmas[i].start;
    audit_vmas();
}
static void invalid_backing(void) {
    write_lock();
    size_t i=vma_index((uintptr_t)audit_address);
    vmas[i].phys=POOL_SIZE+FLEX_SPAN;
    unsigned byte=0;
    (void)runtime_memory_write_backing((uintptr_t)audit_address,&byte,sizeof(byte));
}
static void expect_mapping_stop(void (*run)(void),const char *reason,int tracked) {
    int pipefd[2]; assert(!pipe(pipefd));
    fflush(NULL);
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        struct rlimit no_core={0,0}; setrlimit(RLIMIT_CORE,&no_core);
        close(pipefd[0]); assert(dup2(pipefd[1],STDERR_FILENO)>=0); close(pipefd[1]);
        run(); _exit(99);
    }
    close(pipefd[1]);
    char report[8192]; size_t bytes=0; ssize_t got;
    while ((got=read(pipefd[0],report+bytes,sizeof(report)-1-bytes))>0) bytes+=(size_t)got;
    report[bytes]=0; close(pipefd[0]);
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    assert(strstr(report,reason) && strstr(report,"request=[") && strstr(report,"existing=["));
    assert(strstr(report,tracked ? "alias snapshot tracked=1" : "alias snapshot tracked=0"));
    if (strstr(reason,"insertion")) assert(strstr(report,"other=1"));
}
static void mapping_audit_test(void) {
    assert(!direct_allocate(0,POOL_SIZE,3*PAGE,PAGE,0,&audit_physical));
    void *a=NULL,*b=NULL,*alias=NULL;
    assert(!direct_map(&a,3*PAGE,3,0,audit_physical,PAGE));
    memset(a,0x6d,3*PAGE);
    b=a; assert(!map_flexible(&b,PAGE,3,0)); assert(b!=a);
    void *same=a;
    assert(map_flexible(&same,PAGE,3,MAP_FIXED_FLAG|MAP_NO_OVERWRITE)==NO_MEMORY);
    assert(same==a && *(unsigned char *)a==0x6d);
    assert(!direct_map(&alias,PAGE,3,0,audit_physical,PAGE));
    audit_address=a;
    expect_mapping_stop(overlapping_insert,"insertion overlaps",1);
    expect_mapping_stop(invalid_table,"invalid/overlapping VMA table",0);
    expect_mapping_stop(invalid_backing,"invalid live backing translation",1);
    expect_mapping_stop(ambiguous_reserve,"unqualified fixed reservation",1);
    // Bare FIXED replaces exactly the requested page, preserving neighbours.
    same=(unsigned char *)a+PAGE;
    assert(!map_flexible(&same,PAGE,3,MAP_FIXED_FLAG));
    assert(!*(unsigned char *)same && *(unsigned char *)a==0x6d);
    assert(*((unsigned char *)a+2*PAGE)==0x6d);
    unsigned word=0x12345678;
    assert(runtime_memory_write_backing((uintptr_t)a,&word,sizeof(word)));
    assert(*(unsigned *)alias==word);
    // A remap must use the current backing, not an old VA->physical cache entry.
    same=a; assert(!map_flexible(&same,PAGE,3,MAP_FIXED_FLAG));
    unsigned other=0x87654321;
    assert(runtime_memory_write_backing((uintptr_t)a,&other,sizeof(other)));
    assert(*(unsigned *)a==other && *(unsigned *)alias==word);
    assert(!direct_unmap(a,3*PAGE)); assert(!direct_unmap(b,PAGE));
    assert(!direct_release(audit_physical,3*PAGE));

    void *reserved=NULL;
    assert(!reserve_range(&reserved,2*PAGE,0,PAGE)); audit_address=reserved;
    assert(!direct_allocate(0,POOL_SIZE,PAGE,PAGE,0,&audit_physical));
    expect_mapping_stop(ambiguous_commit,"unqualified FIXED|NO_OVERWRITE",0);
    expect_mapping_stop(ambiguous_reserve,"unqualified fixed reservation",0);
    // Ordinary FIXED commitment of a reservation remains supported.
    same=reserved; assert(!direct_map(&same,PAGE,3,MAP_FIXED_FLAG,audit_physical,PAGE));
    unsigned char data[2*PAGE]; memset(data,0x7e,sizeof(data));
    assert(!runtime_memory_write_backing((uintptr_t)same,data,sizeof(data)));
    assert(!*(unsigned char *)same); // No partial write on an unbacked suffix.
    assert(!direct_release(audit_physical,PAGE));
    assert(!direct_unmap(reserved,2*PAGE));
    puts("PASS: nonfixed hints, NO_OVERWRITE rejection, exact FIXED replacement, reservation stops, VMA/backing guards, alias remap writes");
}
int main(void) {
    alarm(30);
    runtime_memory_reserve();
    alias_diagnostic_test();
    mapping_audit_test();
    runtime_memory_set_gpu_hooks(notify,notify,notify);
    runtime_memory_set_gpu_lifetime_hooks(begin,retire,end);
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,4*PAGE,PAGE,0,&physical));
    void *a=NULL,*b=NULL;
    assert(!direct_map(&a,4*PAGE,3,0,physical,PAGE));
    assert(!direct_map(&b,4*PAGE,3,0,physical,PAGE));
    uintptr_t region_start,region_end; uint64_t offset;
    assert(runtime_memory_backing_region((uintptr_t)a+PAGE,&region_start,&region_end,&offset));
    assert(region_start==(uintptr_t)a && region_end==(uintptr_t)a+4*PAGE && offset==(uint64_t)physical);
    assert(runtime_memory_backing_region((uintptr_t)b,&region_start,&region_end,&offset));
    assert(region_start==(uintptr_t)b && offset==(uint64_t)physical);
    assert(!runtime_memory_backing_region(LOW_MIN,&region_start,&region_end,&offset));
    memset(a,0x5a,4*PAGE); track(a,4*PAGE,0x5a); track(b,4*PAGE,0x5a);
    assert(!direct_release(physical+PAGE,PAGE)); assert(!live()); balanced();
    assert(!runtime_memory_backing_region((uintptr_t)a+PAGE,&region_start,&region_end,&offset));
    assert(runtime_memory_backing_region((uintptr_t)b+2*PAGE,&region_start,&region_end,&offset));
    assert(region_start==(uintptr_t)b+2*PAGE && offset==(uint64_t)physical+2*PAGE);
    assert(((unsigned char *)a)[0]==0x5a && ((unsigned char *)b)[2*PAGE]==0x5a);
    track(a,PAGE,0x5a); track(b,PAGE,0x5a);
    assert(!direct_release(physical,PAGE)); assert(!live());
    assert(!direct_release(physical+2*PAGE,2*PAGE));
    assert(!direct_allocate(physical,physical+4*PAGE,4*PAGE,PAGE,0,&physical));
    a=NULL; assert(!direct_map(&a,4*PAGE,3,0,physical,PAGE));
    for(unsigned i=0;i<4*PAGE;++i) assert(!((unsigned char *)a)[i]);
    track(a,4*PAGE,0);
    assert(!kernel_mprotect(a,PAGE,0)); assert(!live());
    assert(!kernel_mprotect(a,PAGE,3));
    track(a,4*PAGE,0);
    void *replacement=a;
    assert(!map_flexible(&replacement,4*PAGE,3,MAP_FIXED_FLAG)); assert(!live());
    assert(runtime_memory_backing_region((uintptr_t)a,&region_start,&region_end,&offset));
    assert(offset>=POOL_SIZE && region_end-region_start==4*PAGE);
    memset(a,0x6b,4*PAGE); track(a,4*PAGE,0x6b);
    assert(!direct_unmap(a,4*PAGE)); assert(!live());
    assert(!direct_release(physical,4*PAGE));
    void *flex=NULL; assert(!map_flexible(&flex,2*PAGE,3,0));
    memset(flex,0x7c,2*PAGE); track(flex,2*PAGE,0x7c);
    unsigned before=begins;
    BatchEntry batch[]={{.start=flex,.length=PAGE,.operation=1},
        {.start=(unsigned char *)flex+PAGE,.length=PAGE,.prot=0,.operation=2},
        {.operation=99}};
    int done=-1;
    assert(batch_map2(batch,3,&done,0)==INVALID && done==2);
    assert(begins==before+1 && !live()); balanced();
    assert(!direct_unmap((unsigned char *)flex+PAGE,PAGE));
    before=begins;
    assert(direct_map(NULL,PAGE,3,0,0,PAGE)==INVALID);
    assert(direct_release(physical,4*PAGE)==INVALID);
    assert(kernel_mprotect((void *)USER_MIN,PAGE,3)==INVALID);
    assert(begins==before+3); balanced();
    void *reserved=NULL; assert(!reserve_range(&reserved,PAGE,0,PAGE));
    assert(!runtime_memory_backing_region((uintptr_t)reserved,&region_start,&region_end,&offset));
    before=begins;
    void *outside=(void *)USER_MAX;
    assert(reserve_range(&outside,PAGE,MAP_FIXED_FLAG,PAGE)==NO_MEMORY);
    assert(begins==before+1); balanced();
    assert(!direct_unmap(reserved,PAGE));
    assert(retires>=6 && notifications>=10);
    runtime_memory_set_gpu_lifetime_hooks(NULL,NULL,NULL);
    printf("PASS: GPU lifetime order, alias/partial release, fixed replacement, flex, protection, batch, failures (%u transactions)\n",ends);
}
