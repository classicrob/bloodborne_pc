#ifndef BB_RUNTIME_CPU_PROFILE_H
#define BB_RUNTIME_CPU_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef __APPLE__
void runtime_cpu_profile_init(void);
void runtime_cpu_profile_stop(void);
void runtime_cpu_profile_thread(const char *name);
void runtime_cpu_profile_stack(void *base,size_t size);
void runtime_cpu_profile_guest(uintptr_t base,size_t size,int module);
int runtime_cpu_profile_fault(int signal,void *context);
#endif
#ifdef __cplusplus
}
#endif
#endif
