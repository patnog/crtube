#pragma once
#include "screen-manager.h"

// home - the landing screen: trending categories (L1/R1) in a VideoGrid. Start opens search, Triangle opens
// a channel (both push the search screen), X plays.

void openHome(void);
void openHomeSettingsPanel(void);

// Apply an action selected from the shared sidebar overlay (also used by channel view).
void handleHomeSidebarSelection(int picked);

// Called by playback after the first decoded frame; history is kept only for this app run.
void recordLocalHistory(const char *videoId, const char *title, const char *author);
extern Screen homeScreen;
