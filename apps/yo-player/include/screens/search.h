#pragma once

// search screen - recent searches before a query, then a VideoGrid of search results or channel videos.
// Circle returns to home; L2 cycles search ordering.

#include "screen.h"
#include "extractor.h"

extern Screen searchScreen;

// open the search landing page with the persistent recent-search list.
void openSearchPage(void);

// push the search screen showing a text-search for `query`.
void openSearchQuery(const char *query);
void openSearchQueryWithSort(const char *query, int sort);

// push the search screen showing the given result's channel (its videos only).
void openChannelFor(const SearchResult *item);
void openChannelForCategory(const SearchResult *item, int category);
