#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local int guest_error;
#ifndef BB_QOS_REAL
static atomic_int sets, gets;
static int inject_error, inject_mismatch;
static _Thread_local qos_class_t requested=QOS_CLASS_UTILITY;
static qos_class_t test_get(pthread_t thread,int *relative) {
    assert(pthread_equal(thread,pthread_self()));
    atomic_fetch_add(&gets,1);
    errno=99;guest_error=77;
    *relative=0;
    return requested;
}
static int test_set(qos_class_t qos,int relative) {
    assert(qos==QOS_CLASS_USER_INTERACTIVE && relative==0);
    atomic_fetch_add(&sets,1);
    errno=98;guest_error=76;
    if (inject_error) return EPERM;
    if (!inject_mismatch) requested=qos;
    return 0;
}
#define pthread_get_qos_class_np test_get
#define pthread_set_qos_class_self_np test_set
#endif
#include "../src/runtime_audio_qos_mac.h"
#ifndef BB_QOS_REAL
#undef pthread_get_qos_class_np
#undef pthread_set_qos_class_self_np
#endif

static void *worker(void *arg) {
    const char *name=arg;
    errno=17;guest_error=23;
    audio_qos_start(name,256,&guest_error);
    assert(errno==17 && guest_error==23);
#ifdef BB_QOS_REAL
    int relative=0,policy=0;
    struct sched_param param={0};
    qos_class_t qos=pthread_get_qos_class_np(pthread_self(),&relative);
    assert(!pthread_getschedparam(pthread_self(),&policy,&param));
    printf("OBSERVED %s requested-qos=%u relative=%d sched=%d priority=%d\n",
           name,(unsigned)qos,relative,policy,param.sched_priority);
#endif
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
#ifndef BB_QOS_REAL
    inject_error=!strcmp(argv[1],"error");
    inject_mismatch=!strcmp(argv[1],"mismatch");
#endif
    const char *names[]={"FMOD AudioOut thread","FMOD mixer thread","FMOD stream thread",
        "FMOD file thread","FMOD AudioOut thread extra","GXRenderThread","bb:DrawRec"};
    pthread_t threads[7];
    for (unsigned i=0;i<7;++i) assert(!pthread_create(&threads[i],NULL,worker,(void *)names[i]));
    for (unsigned i=0;i<7;++i) assert(!pthread_join(threads[i],NULL));
#ifndef BB_QOS_REAL
    int active=!strcmp(argv[1],"interactive");
    assert(atomic_load(&sets)==(active ? 2 : 0));
    assert(atomic_load(&gets)==(active ? 4 : 0));
    /* Configuration remains latched; changing the environment doesn't opt in. */
    if (!active) {
        assert(!setenv("BB_AUDIO_QOS","interactive",1));
        worker((void *)names[0]);
        assert(!atomic_load(&sets));
    }
#endif
    puts("PASS: audio QoS selection, concurrent startup, errno and configuration");
    return 0;
}
