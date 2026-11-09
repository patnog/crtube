#pragma once
#include "screen-manager.h"

// home - the landing screen: trending categories (L1/R1) in a VideoGrid. SELECT or Search in the sidebar
// opens the search page; Triangle opens a channel and CROSS plays.

void openHome(void);
void openHomeSettingsPanel(void);
void openHomePlaylist(const char *playlistId, const char *title);
void openHomePlaylistFromSearch(const char *playlistId, const char *title, const char *query, int sort);

// Apply an action selected from the shared sidebar overlay (also used by channel view).
void handleHomeSidebarSelection(int picked);

extern Screen homeScreen;
