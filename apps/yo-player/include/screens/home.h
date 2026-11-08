#pragma once
#include "screen-manager.h"

// home - the landing screen: trending categories (L1/R1) in a VideoGrid. Start opens search, Triangle opens
// a channel (both push the search screen), X plays.

void openHome(void);
void openHomeSettingsPanel(void);

// Apply an action selected from the shared sidebar overlay (also used by channel view).
void handleHomeSidebarSelection(int picked);

extern Screen homeScreen;
