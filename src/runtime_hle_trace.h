#ifndef BB_RUNTIME_HLE_TRACE_H
#define BB_RUNTIME_HLE_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uintptr_t runtime_hle_trace_wrap(const char *nid,const char *symbol,uintptr_t target);
void runtime_hle_trace_dump(void);
/* Ordinary-thread calls only; summary reporting is not signal-safe. */
void runtime_hle_trace_frame(void);
void runtime_hle_trace_summary(void);
#ifdef __cplusplus
}
#endif
#endif
