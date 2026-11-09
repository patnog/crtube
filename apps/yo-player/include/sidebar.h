#pragma once

#include "font.h"

// Actions returned by the home navigation overlay. The account feeds and the
// local PS3 feeds are intentionally separate entries even when their names match.
typedef enum {
   SIDEBAR_SEARCH,
   SIDEBAR_RECOMMENDED,
   SIDEBAR_ACCOUNT_SUBSCRIPTIONS,
   SIDEBAR_ACCOUNT_HISTORY,
   SIDEBAR_ACCOUNT_WATCH_LATER,
   SIDEBAR_TRENDING,
   SIDEBAR_LIVE,
   SIDEBAR_PS3_SUBSCRIPTIONS,
   SIDEBAR_PS3_HISTORY,
   SIDEBAR_PS3_WATCH_LATER,
   SIDEBAR_ACCOUNT,
   SIDEBAR_SETTINGS,
   SIDEBAR_EXIT
} SidebarItem;

void initSidebar(Font *font, int screenW, int screenH);
void termSidebar(void);
void openSidebar(void);
void setSidebarCurrent(SidebarItem item);
int  isSidebarActive(void);
int  isSidebarFocused(void);
// LEFT opens from the first grid column; UP/DOWN navigate; CROSS commits;
// LEFT, RIGHT or CIRCLE closes the menu.
int  updateSidebar(void);
void drawSidebar(void);
