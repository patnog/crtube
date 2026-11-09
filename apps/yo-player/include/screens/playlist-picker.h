#pragma once

#include "extractor.h"
#include "screen-manager.h"

// Open the PS3 playlist chooser for a video. The selected entry is copied so
// the underlying grid may remain suspended while the chooser is active.
void openLocalPlaylistPicker(const SearchResult *item);

extern Screen playlistPickerScreen;
