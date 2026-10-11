#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <string.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    unsigned char *stack=runtime_low_map_named(32768,PROT_READ|PROT_WRITE,"guest-stack");
    assert(stack); stack[32767]=7;
    void *object=runtime_guest_malloc(80); assert(object);
    errno=EDOM; runtime_guest_free(object); assert(errno==EDOM);
    void *again=runtime_guest_malloc(80); assert(again==object);
    runtime_guest_free(again);
    assert(stack[32767]==7);
    return 0;
}
