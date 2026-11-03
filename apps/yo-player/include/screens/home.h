#pragma once
#include "screen-manager.h"

// home - the landing screen: trending categories (L1/R1) in a VideoGrid. Start opens search, Triangle opens
// a channel (both push the search screen), X plays.

void openHome(void);

// Called by playback after the first decoded frame; history is kept only for this app run.
void recordLocalHistory(const char *videoId, const char *title, const char *author);
extern Screen homeScreen;
