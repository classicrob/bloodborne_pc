#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "test_alarm.h"

typedef int32_t (ABI *Op)(void **);
typedef int32_t (ABI *Init)(void **, void **, const char *);
typedef int32_t (ABI *Pair)(void **, void **);
#define MUTEX(type, nid) ((type)runtime_mutex_resolve(nid))
#define RW(type, nid) ((type)runtime_rwlock_resolve(nid))
#define INVALID UINT32_C(0x80020016)
/* runtime_hle_trace.c (linked for the cond wake check's caller) reads the guest errno. */
static _Thread_local int32_t guest_error;
int32_t *runtime_errno(void) { return &guest_error; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
static void contains(const char *text, const char *needle) {
    if (!strstr(text, needle)) {
        fprintf(stderr, "Missing diagnostic: %s\n%s", needle, text);
        abort();
    }
}
int main(void) {
    test_alarm(20);
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("Testing runtime sync EINVAL diagnostics");
    FILE *capture = tmpfile();
    assert(capture);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    Init init = MUTEX(Init, "cmo1RIYva9o#p#J");
    Op lock = MUTEX(Op, "9UK1vLZQft4#p#J");
    Op unlock = MUTEX(Op, "tn3VlD0hG60#p#J");
    Op destroy = MUTEX(Op, "2Of0f+3mhhE#p#J");
    void *mutex = NULL;
    assert(init(&mutex, NULL, "diagnostic") == 0);
    assert((uintptr_t)mutex < (UINT64_C(1) << 40));
    assert(lock(&mutex) == 0 && unlock(&mutex) == 0);
    assert(ftell(capture) == 0);
    assert(destroy(&mutex) == 0 && mutex == (void *)2);
    assert((uint32_t)lock(&mutex) == INVALID);
    assert((uint32_t)unlock(&mutex) == INVALID);
    assert((uint32_t)destroy(&mutex) == INVALID);
    assert((uint32_t)lock(NULL) == INVALID);
    assert(MUTEX(Op, "7H0iTOciTLo#I#J")(&mutex) == 22);

    /* A caller in low executable guest memory must survive the HLE wrapper. */
    unsigned char code[] = {
        0x48,0xbf,0,0,0,0,0,0,0,0, 0x48,0xb8,0,0,0,0,0,0,0,0,
        0x48,0x83,0xec,0x08, 0xff,0xd0, 0x48,0x83,0xc4,0x08,0xc3
    };
    void *slot = &mutex;
    memcpy(code + 2, &slot, 8);
    memcpy(code + 12, &lock, 8);
    unsigned char *guest = runtime_low_map(4096, PROT_READ | PROT_WRITE);
    assert(guest);
    memcpy(guest, code, sizeof(code));
    assert(mprotect(guest, 4096, PROT_READ | PROT_EXEC) == 0);
    assert((uint32_t)((int32_t (ABI *)(void))guest)() == INVALID);

    /* GuestMutex begins with the actual native mutex. Bypass the guest sentinel
       to distinguish Darwin's destroyed-native error from handle validation. */
    mutex = NULL;
    assert(init(&mutex, NULL, "native-destroyed") == 0);
    assert(pthread_mutex_destroy((pthread_mutex_t *)mutex) == 0);
    assert((uint32_t)lock(&mutex) == INVALID);
    runtime_guest_free(mutex);
    mutex = (void *)2;
    assert((uint32_t)lock(&mutex) == INVALID); /* Must not inherit native_errno. */

    void *cond = NULL;
    assert(MUTEX(Init, "2Tb92quprl0#p#J")(&cond, NULL, NULL) == 0);
    assert((uint32_t)MUTEX(Pair, "WKAXJ4XBPQ4#p#J")(&cond, &mutex) == INVALID);
    assert(pthread_cond_destroy((pthread_cond_t *)cond) == 0);
    assert((uint32_t)MUTEX(Op, "kDh-NfxgMtE#p#J")(&cond) == INVALID);
    runtime_guest_free(cond);
    assert((uint32_t)MUTEX(Op, "F8bUHwAG284#p#J")(NULL) == INVALID);

    void *rw = NULL;
    assert(RW(Init, "6ULAa0fq4jA#p#J")(&rw, NULL, NULL) == 0);
    assert(RW(Op, "Ox9i0c7L5w0#p#J")(&rw) == 0);
    assert(RW(Op, "+L98PIbGttk#p#J")(&rw) == 0);
    assert(RW(Op, "BB+kb08Tl9A#p#J")(&rw) == 0);
    assert((uint32_t)RW(Op, "Ox9i0c7L5w0#p#J")(&rw) == INVALID);
    assert((uint32_t)RW(Op, "BB+kb08Tl9A#p#J")(NULL) == INVALID);

    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, capture);
    assert(!ferror(capture) && feof(capture));
    text[n] = 0;
    fclose(capture);
    contains(text, "op=mutex_lock kind=mutex");
    contains(text, "handle=0x2 state=destroyed");
    contains(text, "state=null-slot");
    contains(text, "op=posix_mutex_lock");
    contains(text, "native_errno=22 native_call=pthread_mutex_lock");
    contains(text, "native_errno=0 native_call=none(validation)");
    contains(text, "op=cond_wait kind=condition");
    contains(text, "native_errno=22 native_call=pthread_cond_signal");
    contains(text, "op=attr_init kind=attribute");
    contains(text, "op=rw_read kind=rwlock");
    contains(text, "handle=0x1 state=destroyed");
    char caller[64];
    snprintf(caller, sizeof(caller), "caller=%p", guest + 26);
    contains(text, caller);
    fputs(text, stdout);
    puts("PASS: sentinel/native/condition/rwlock EINVAL, POSIX errno and exact low guest caller");
    return 0;
}
