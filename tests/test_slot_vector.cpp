// SPDX-License-Identifier: GPL-2.0-or-later
// Common::SlotVector::is_allocated: the invalid id (a default SlotId) is not allocated, an index
// beyond the vector's capacity stops (exit 23, a forked child), and allocation follows insert and
// erase. It read the storage bitset out of bounds for the invalid id: the replay capture crashed
// on the first image without a linked depth image (2026-10-11).
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "common/slot_vector.h"

namespace {
u32 failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-62s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

/// Runs `body` in a child; whether it exited 23 with `message` on stderr.
template <typename Body>
bool StopsWith(Body body, const char* message) {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        body();
        std::_Exit(0); // not reached when the body stops
    }
    close(fds[1]);
    std::string text;
    char chunk[4096];
    for (ssize_t n; (n = read(fds[0], chunk, sizeof(chunk))) > 0;) {
        text.append(chunk, size_t(n));
    }
    close(fds[0]);
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 23 && text.find(message) != std::string::npos;
}
} // namespace

int main() {
    Common::SlotVector<int> slots;
    Expect("the invalid id is not allocated (empty vector)", !slots.is_allocated(Common::SlotId{}));
    const Common::SlotId a = slots.insert(1);
    const Common::SlotId b = slots.insert(2);
    Expect("the invalid id is not allocated (after inserts)",
           !slots.is_allocated(Common::SlotId{}));
    Expect("inserted ids are allocated", slots.is_allocated(a) && slots.is_allocated(b));
    slots.erase(a);
    Expect("an erased id is not allocated, the other still is",
           !slots.is_allocated(a) && slots.is_allocated(b));
    Expect("an index beyond the capacity stops", StopsWith([] {
               Common::SlotVector<int> small;
               small.insert(7);
               (void)small.is_allocated(Common::SlotId{1u << 20});
           }, "beyond this vector's"));
    std::printf("slot-vector-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
