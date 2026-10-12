/* bb_macos_begin_game_activity (src/macos_activity.c), as bb-probe calls it at startup. */
#include "macos_activity.h"
#include <stdio.h>
#include <unistd.h>
int main(void) {
    bb_macos_begin_game_activity();
    printf("pid %d\n", (int)getpid());
    fflush(stdout);
    /* Held until the script has read the power assertions and closes stdin. */
    while (getchar() != EOF) {}
    return 0;
}
