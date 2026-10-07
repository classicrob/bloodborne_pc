#pragma once
#ifdef __APPLE__
#include "../../src/compat_mac.h"

static inline uint64_t gettid(void) { return compat_thread_id(); }
#endif
