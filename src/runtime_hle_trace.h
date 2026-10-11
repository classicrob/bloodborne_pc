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
/* The guest return address of the call whose return slot is `slot`: the original when the trace
 * replaced it with its return bridge, else the slot's value. */
uintptr_t runtime_hle_trace_original_return(const uintptr_t *slot);
/* The import a trace thunk stands for (`pointer` itself if it is none). */
uintptr_t runtime_hle_trace_unwrap(uintptr_t pointer);
#ifdef __cplusplus
}
#endif
#endif
