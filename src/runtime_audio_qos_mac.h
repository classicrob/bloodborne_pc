#ifndef BB_RUNTIME_AUDIO_QOS_MAC_H
#define BB_RUNTIME_AUDIO_QOS_MAC_H

#include <pthread/qos.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t audio_qos_once=PTHREAD_ONCE_INIT;
static int audio_qos_interactive;

static void audio_qos_configure(void) {
    const char *mode=getenv("BB_AUDIO_QOS");
    if (!mode || !*mode || !strcmp(mode,"inherit")) return;
    if (strcmp(mode,"interactive")) {
        fputs("STOP: BB_AUDIO_QOS must be interactive, inherit, or unset\n",stderr);
        fflush(stderr);
        _Exit(23);
    }
    audio_qos_interactive=1;
}

/* Called once by host_start, before guest entry, with its attached errno slot. */
static void audio_qos_start(const char *name,int guest_priority,int *guest_error) {
    int saved_errno=errno, saved_guest=*guest_error;
    int error=pthread_once(&audio_qos_once,audio_qos_configure);
    if (error) {
        fprintf(stderr,"STOP: audio QoS configuration failed: %d\n",error);
        fflush(stderr);
        _Exit(23);
    }
    if (audio_qos_interactive &&
        (!strcmp(name,"FMOD AudioOut thread") || !strcmp(name,"FMOD mixer thread"))) {
        int before_relative=0, after_relative=0;
        qos_class_t before=pthread_get_qos_class_np(pthread_self(),&before_relative);
        error=pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
        if (error) {
            fprintf(stderr,"STOP: audio QoS interactive for '%s' failed: %d\n",name,error);
            fflush(stderr);
            _Exit(23);
        }
        qos_class_t after=pthread_get_qos_class_np(pthread_self(),&after_relative);
        if (after!=QOS_CLASS_USER_INTERACTIVE || after_relative!=0) {
            fprintf(stderr,"STOP: audio QoS interactive for '%s' not reported (class=%u relative=%d)\n",
                    name,(unsigned)after,after_relative);
            fflush(stderr);
            _Exit(23);
        }
        fprintf(stderr,"Audio QoS: '%s' pthread=%p guest-prio=%d requested=%u/%d -> %u/%d; effective scheduling may be process-clamped\n",
                name,(void *)pthread_self(),guest_priority,(unsigned)before,before_relative,
                (unsigned)after,after_relative);
    }
    *guest_error=saved_guest;
    errno=saved_errno;
}
#endif
