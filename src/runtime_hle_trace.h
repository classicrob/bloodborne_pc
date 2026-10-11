#ifndef BB_RUNTIME_HLE_TRACE_H
#define BB_RUNTIME_HLE_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uintptr_t runtime_hle_trace_wrap(const char *nid,const char *symbol,uintptr_t target);
void runtime_hle_trace_dump(void);
#ifdef __cplusplus
}
#endif
#endif
