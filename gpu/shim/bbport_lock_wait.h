// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): lock-wait timing for our own mutexes (diagnostic, always on). A Timed lock
// tries first; only a contended acquisition opens a parse-split scope ("lock wait: <name>") on
// the calling thread for the time it blocks, so the uncontended cost is one try_lock. The hold
// time is not measured. Other platforms use the plain mutex (BB_TIMED_LOCK).
#pragma once

#ifdef __APPLE__
#include "bbport_parse_split.h"

namespace BbLockWait {

template <typename Mutex, ::BbSplit::K Kind>
class Timed {
public:
    void lock() {
        if (mutex.try_lock()) {
            return;
        }
        ::BbSplit::Scope scope{Kind};
        mutex.lock();
    }
    bool try_lock() {
        return mutex.try_lock();
    }
    void unlock() {
        mutex.unlock();
    }

private:
    Mutex mutex;
};

/// The same for a shared mutex: shared and exclusive waits charge the same kind.
template <typename Mutex, ::BbSplit::K Kind>
class TimedShared {
public:
    void lock() {
        if (mutex.try_lock()) {
            return;
        }
        ::BbSplit::Scope scope{Kind};
        mutex.lock();
    }
    bool try_lock() {
        return mutex.try_lock();
    }
    void unlock() {
        mutex.unlock();
    }
    void lock_shared() {
        if (mutex.try_lock_shared()) {
            return;
        }
        ::BbSplit::Scope scope{Kind};
        mutex.lock_shared();
    }
    bool try_lock_shared() {
        return mutex.try_lock_shared();
    }
    void unlock_shared() {
        mutex.unlock_shared();
    }

private:
    Mutex mutex;
};

} // namespace BbLockWait

#define BB_TIMED_LOCK(mutex_type, kind) ::BbLockWait::Timed<mutex_type, ::BbSplit::kind>
#define BB_TIMED_SHARED_LOCK(mutex_type, kind)                                                     \
    ::BbLockWait::TimedShared<mutex_type, ::BbSplit::kind>
#else
#define BB_TIMED_LOCK(mutex_type, kind) mutex_type
#define BB_TIMED_SHARED_LOCK(mutex_type, kind) mutex_type
#endif
