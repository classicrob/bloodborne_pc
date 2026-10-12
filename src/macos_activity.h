#ifndef BB_MACOS_ACTIVITY_H
#define BB_MACOS_ACTIVITY_H
#ifdef __APPLE__
/* Keeps macOS from napping the game: an NSProcessInfo activity (user initiated, latency
 * critical) for the whole run, said once on stdout. BB_APP_NAP=allow leaves it out (A/B); any
 * other value, or an activity the system refuses, ends the process with a message. */
void bb_macos_begin_game_activity(void);
#endif
#endif
