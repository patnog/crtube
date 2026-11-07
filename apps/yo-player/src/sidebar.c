// Sliding home navigation overlay. The grid remains underneath and is never
// resized. Trending is a nested page; section captions are not selectable.
#include "sidebar.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "thread.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include <string.h>

#define SIDEBAR_ANIMATION_US       190000ULL
#define SIDEBAR_ROW_HEIGHT         46
#define SIDEBAR_ROW_GAP             6
#define SIDEBAR_ROW_STEP           (SIDEBAR_ROW_HEIGHT + SIDEBAR_ROW_GAP)
#define SIDEBAR_ICON_SIZE          24
#define SIDEBAR_TEXT_SIZE          22
#define SIDEBAR_EDGE_WIDTH         32
#define SIDEBAR_EDGE_STRIP_WIDTH   52
#define SIDEBAR_EDGE_HEIGHT        76
#define SIDEBAR_MIN_WIDTH         300
#define SIDEBAR_MAX_WIDTH         620
#define SIDEBAR_CONTENT_LEFT       28
#define SIDEBAR_TEXT_LEFT          78
#define SIDEBAR_RIGHT_PAD          14
#define SIDEBAR_TITLE_Y            34
#define SIDEBAR_ROWS_TOP          104
#define SIDEBAR_ROWS_BOTTOM        18
#define SIDEBAR_MAIN_ROWS          13
#define SIDEBAR_TREND_ROWS          4
#define SIDEBAR_MAX_ROWS           SIDEBAR_MAIN_ROWS
#define SIDEBAR_PAGE_MAIN           0
#define SIDEBAR_PAGE_TRENDING       1

#define ROW_SECTION                 1

typedef struct {
   const char *name;
   IconId icon;
   int action;
   int flags;
} SidebarRow;

static const SidebarRow mainRows[SIDEBAR_MAIN_ROWS] = {
   { "Search",         ICON_SEARCH,    SIDEBAR_SEARCH,                 0 },
   { "Recommended",    ICON_STAR,      SIDEBAR_RECOMMENDED,            0 },
   { "Subscriptions",  ICON_GOOGLE,    SIDEBAR_ACCOUNT_SUBSCRIPTIONS,  0 },
   { "History",        ICON_HISTORY,   SIDEBAR_ACCOUNT_HISTORY,        0 },
   { "Watch Later",    ICON_CLOCK,     SIDEBAR_ACCOUNT_WATCH_LATER,    0 },
   { "Trending",       ICON_UP_BIG,    SIDEBAR_TRENDING,               0 },
   { "THIS PS3",       0,              -1,                             ROW_SECTION },
   { "Subscriptions",  ICON_HDD,       SIDEBAR_PS3_SUBSCRIPTIONS,      0 },
   { "History",        ICON_HISTORY,   SIDEBAR_PS3_HISTORY,            0 },
   { "Watch Later",    ICON_CLOCK,     SIDEBAR_PS3_WATCH_LATER,        0 },
   { "APP",            0,              -1,                             ROW_SECTION },
   { "Account",        ICON_GOOGLE,    SIDEBAR_ACCOUNT,                0 },
   { "Settings",       ICON_COG,       SIDEBAR_SETTINGS,               0 }
};

static const SidebarRow trendingRows[SIDEBAR_TREND_ROWS] = {
   { "Back",           ICON_LEFT_BIG,  -1,                             0 },
   { "Gaming",         ICON_MICROCHIP, SIDEBAR_TRENDING_GAMING,         0 },
   { "Sports",         ICON_STAR,      SIDEBAR_TRENDING_SPORTS,         0 },
   { "Podcasts",       ICON_VOLUME_UP, SIDEBAR_TRENDING_PODCASTS,       0 }
};

static struct {
   Font *font;
   int screenW, screenH, panelW;
   float uiScale;
   int selected;
   int trendingSelected;
   int scrollTop;
   int visibleRows;
   int page;
   int focused;
   int targetOpen;
   float progress;
   uint64_t animationStart;
   float animationFrom;
   int animationRunning;
   Label title;
   Label mainNames[SIDEBAR_MAX_ROWS];
   Icon mainIcons[SIDEBAR_MAX_ROWS];
   Label trendingNames[SIDEBAR_TREND_ROWS];
   Icon trendingIcons[SIDEBAR_TREND_ROWS];
   Icon edgeOpen;
   Icon edgeClose;
} side;

static float easeOut(float t)
{
   float inv = 1.0f - t;
   return 1.0f - inv * inv;
}

static int getPanelWidth(int screenW, float uiScale)
{
   if (uiScale < 1.0f) uiScale = 1.0f;
   int width = screenW * 42 / 100;
   if (width > SIDEBAR_MAX_WIDTH) width = SIDEBAR_MAX_WIDTH;
   width += (int)(100.0f * (uiScale - 1.0f) + 0.5f);
   if (width < SIDEBAR_MIN_WIDTH) width = SIDEBAR_MIN_WIDTH;
   int maxWidth = screenW * 72 / 100;
   if (width > maxWidth) width = maxWidth;
   if (width > screenW) width = screenW;
   return width;
}

static const SidebarRow *getRows(int *count)
{
   if (side.page == SIDEBAR_PAGE_TRENDING) {
      *count = SIDEBAR_TREND_ROWS;
      return trendingRows;
   }
   *count = SIDEBAR_MAIN_ROWS;
   return mainRows;
}

static int rowSelectable(int row)
{
   int count;
   const SidebarRow *rows = getRows(&count);
   return row >= 0 && row < count && !(rows[row].flags & ROW_SECTION);
}

static void keepSelectionVisible(void)
{
   int count;
   getRows(&count);
   if (side.page == SIDEBAR_PAGE_TRENDING) {
      side.visibleRows = SIDEBAR_TREND_ROWS;
      side.scrollTop = 0;
      return;
   }
   side.visibleRows = (side.screenH - SIDEBAR_ROWS_TOP - SIDEBAR_ROWS_BOTTOM) / SIDEBAR_ROW_STEP;
   if (side.visibleRows < 3) side.visibleRows = 3;
   if (side.visibleRows > count) side.visibleRows = count;
   if (side.selected < side.scrollTop) side.scrollTop = side.selected;
   else if (side.selected >= side.scrollTop + side.visibleRows)
      side.scrollTop = side.selected - side.visibleRows + 1;
   if (side.scrollTop < 0) side.scrollTop = 0;
   if (side.scrollTop > count - side.visibleRows) side.scrollTop = count - side.visibleRows;
}

static void animateTo(int open)
{
   float target = open ? 1.0f : 0.0f;
   if (side.targetOpen == open && side.animationRunning) return;
   if (side.progress == target) {
      side.targetOpen = open;
      side.animationRunning = 0;
      return;
   }
   side.animationFrom = side.progress;
   side.animationStart = getTimeUs();
   side.targetOpen = open;
   side.animationRunning = 1;
}

static void updateSelectedColors(void)
{
   int count;
   const SidebarRow *rows = getRows(&count);
   for (int i = 0; i < count; i++) {
      Label *label;
      if (side.page == SIDEBAR_PAGE_TRENDING) label = &side.trendingNames[i];
      else label = &side.mainNames[i];
      uint32_t color = (i == side.selected) ? activeTheme->accent : activeTheme->textPrimary;
      if (rows[i].flags & ROW_SECTION) color = activeTheme->textSecondary;
      setLabelColor(label, color);
   }
}

void initSidebar(Font *font, int screenW, int screenH)
{
   memset(&side, 0, sizeof side);
   side.font = font;
   side.screenW = screenW;
   side.screenH = screenH;
   side.uiScale = getGfxUiScaleFactor();
   if (side.uiScale < 1.0f) side.uiScale = 1.0f;
   side.panelW = getPanelWidth(screenW, side.uiScale);
   side.selected = 5; // Trending is the initial active section.
   side.trendingSelected = 1;

   initLabel(&side.title, font, 0, 0,
             SIDEBAR_MAX_WIDTH - SIDEBAR_CONTENT_LEFT - SIDEBAR_EDGE_STRIP_WIDTH - SIDEBAR_RIGHT_PAD,
             48, 28, activeTheme->textPrimary, TEXT_NOWRAP, "CRTube");
   setLabelScaleWithUi(&side.title, 1);

   for (int i = 0; i < SIDEBAR_MAIN_ROWS; i++) {
      initLabel(&side.mainNames[i], font, 0, 0,
                SIDEBAR_MAX_WIDTH - SIDEBAR_TEXT_LEFT - SIDEBAR_EDGE_STRIP_WIDTH - SIDEBAR_RIGHT_PAD,
                42, SIDEBAR_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, mainRows[i].name);
      setLabelScaleWithUi(&side.mainNames[i], 1);
      if (mainRows[i].icon) initIcon(&side.mainIcons[i], mainRows[i].icon, SIDEBAR_ICON_SIZE);
   }
   for (int i = 0; i < SIDEBAR_TREND_ROWS; i++) {
      initLabel(&side.trendingNames[i], font, 0, 0,
                SIDEBAR_MAX_WIDTH - SIDEBAR_TEXT_LEFT - SIDEBAR_EDGE_STRIP_WIDTH - SIDEBAR_RIGHT_PAD,
                42, SIDEBAR_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, trendingRows[i].name);
      setLabelScaleWithUi(&side.trendingNames[i], 1);
      initIcon(&side.trendingIcons[i], trendingRows[i].icon, SIDEBAR_ICON_SIZE);
   }
   initIcon(&side.edgeOpen, ICON_RIGHT_BIG, 20);
   initIcon(&side.edgeClose, ICON_LEFT_BIG, 20);
   keepSelectionVisible();
   updateSelectedColors();
}

void termSidebar(void)
{
   freeLabel(&side.title);
   for (int i = 0; i < SIDEBAR_MAIN_ROWS; i++) {
      freeLabel(&side.mainNames[i]);
      if (mainRows[i].icon) freeIcon(&side.mainIcons[i]);
   }
   for (int i = 0; i < SIDEBAR_TREND_ROWS; i++) {
      freeLabel(&side.trendingNames[i]);
      freeIcon(&side.trendingIcons[i]);
   }
   freeIcon(&side.edgeOpen);
   freeIcon(&side.edgeClose);
   memset(&side, 0, sizeof side);
}

void setSidebarCurrent(SidebarItem item)
{
   side.page = SIDEBAR_PAGE_MAIN;
   switch (item) {
      case SIDEBAR_SEARCH:                  side.selected = 0; break;
      case SIDEBAR_RECOMMENDED:             side.selected = 1; break;
      case SIDEBAR_ACCOUNT_SUBSCRIPTIONS:   side.selected = 2; break;
      case SIDEBAR_ACCOUNT_HISTORY:         side.selected = 3; break;
      case SIDEBAR_ACCOUNT_WATCH_LATER:     side.selected = 4; break;
      case SIDEBAR_TRENDING:                side.selected = 5; break;
      case SIDEBAR_TRENDING_GAMING:         side.selected = 5; side.trendingSelected = 1; break;
      case SIDEBAR_TRENDING_SPORTS:         side.selected = 5; side.trendingSelected = 2; break;
      case SIDEBAR_TRENDING_PODCASTS:       side.selected = 5; side.trendingSelected = 3; break;
      case SIDEBAR_PS3_SUBSCRIPTIONS:       side.selected = 7; break;
      case SIDEBAR_PS3_HISTORY:             side.selected = 8; break;
      case SIDEBAR_PS3_WATCH_LATER:         side.selected = 9; break;
      case SIDEBAR_ACCOUNT:                 side.selected = 11; break;
      case SIDEBAR_SETTINGS:                side.selected = 12; break;
      default: break;
   }
   keepSelectionVisible();
   updateSelectedColors();
}

void openSidebar(void)
{
   side.page = SIDEBAR_PAGE_MAIN;
   side.focused = 1;
   keepSelectionVisible();
   updateSelectedColors();
   animateTo(1);
}

int isSidebarActive(void)
{
   return side.progress > 0.0f || side.targetOpen || side.animationRunning;
}

int isSidebarFocused(void) { return side.focused; }

static void moveSelection(int direction)
{
   int count;
   getRows(&count);
   int next = side.selected;
   do {
      next += direction;
      if (next < 0) next = count - 1;
      if (next >= count) next = 0;
   } while (!rowSelectable(next));
   side.selected = next;
   keepSelectionVisible();
   updateSelectedColors();
}

int updateSidebar(void)
{
   if (side.animationRunning) {
      uint64_t elapsed = getTimeUs() - side.animationStart;
      float t = (float)elapsed / (float)SIDEBAR_ANIMATION_US;
      if (t >= 1.0f) {
         t = 1.0f;
         side.animationRunning = 0;
      }
      float eased = easeOut(t);
      float target = side.targetOpen ? 1.0f : 0.0f;
      side.progress = side.animationFrom + (target - side.animationFrom) * eased;
   }

   if (!side.focused) return -1;
   if (isPadButtonPressed(PAD_BTN_UP)) moveSelection(-1);
   if (isPadButtonPressed(PAD_BTN_DOWN)) moveSelection(1);

   // LEFT and CIRCLE both close the sidebar. RIGHT is intentionally left
   // unhandled so it does not act as a second close shortcut.
   if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      side.focused = 0;
      animateTo(0);
      return -1;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      int count;
      const SidebarRow *rows = getRows(&count);
      if (side.page == SIDEBAR_PAGE_TRENDING && side.selected == 0) {
         side.page = SIDEBAR_PAGE_MAIN;
         side.selected = 5;
         keepSelectionVisible();
         updateSelectedColors();
         return -1;
      }
      int action = rows[side.selected].action;
      if (action == SIDEBAR_TRENDING) {
         side.page = SIDEBAR_PAGE_TRENDING;
         side.selected = side.trendingSelected;
         keepSelectionVisible();
         updateSelectedColors();
         return -1;
      }
      side.focused = 0;
      animateTo(0);
      return action;
   }
   return -1;
}

static void drawSidebarIconFixed(Icon *icon, int x, int y, uint32_t color)
{
   if (!icon || !icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   drawGfxTextureFixedPosition(x + icon->offsetX, y + icon->offsetY,
                               icon->tex->w, icon->tex->h, *icon->tex,
                               0.0f, 0.0f, 1.0f, 1.0f, color, GFX_FILTER_LINEAR);
}

static void drawEdge(int panelX)
{
   int edgeX = panelX + side.panelW - SIDEBAR_EDGE_WIDTH;
   int edgeY = (side.screenH - SIDEBAR_EDGE_HEIGHT) / 2;

   if (side.progress > 0.0f) {
      int railX = panelX + side.panelW - SIDEBAR_EDGE_STRIP_WIDTH;
      fillGfxRectangleFixed(railX, 0, SIDEBAR_EDGE_STRIP_WIDTH, side.screenH, activeTheme->appBg);
      // A subtle inner rule makes the rail read as the panel's actual edge.
      fillGfxRectangleFixed(railX, 0, 2, side.screenH, activeTheme->textSecondary);
   } else {
      fillGfxRectangleFixed(edgeX, edgeY, SIDEBAR_EDGE_WIDTH, SIDEBAR_EDGE_HEIGHT, activeTheme->surface);
   }

   Icon *glyph = side.targetOpen ? &side.edgeClose : &side.edgeOpen;
   drawSidebarIconFixed(glyph,
                        edgeX + (SIDEBAR_EDGE_WIDTH - glyph->size) / 2,
                        edgeY + (SIDEBAR_EDGE_HEIGHT - glyph->size) / 2,
                        activeTheme->textPrimary);
}

static void drawMainRows(int panelX)
{
   int end = side.scrollTop + side.visibleRows;
   if (end > SIDEBAR_MAIN_ROWS) end = SIDEBAR_MAIN_ROWS;
   for (int i = side.scrollTop; i < end; i++) {
      int rowY = SIDEBAR_ROWS_TOP + (i - side.scrollTop) * SIDEBAR_ROW_STEP;
      if (mainRows[i].flags & ROW_SECTION) {
         moveLabel(&side.mainNames[i], panelX + SIDEBAR_CONTENT_LEFT, rowY + 12);
         drawLabel(&side.mainNames[i]);
         continue;
      }
      if (i == side.selected)
         fillGfxRectangleFixed(panelX + 20, rowY,
                               side.panelW - 20 - SIDEBAR_EDGE_STRIP_WIDTH,
                               SIDEBAR_ROW_HEIGHT, activeTheme->rowHighlight);
      int iconY = rowY + (SIDEBAR_ROW_HEIGHT - SIDEBAR_ICON_SIZE) / 2;
      drawSidebarIconFixed(&side.mainIcons[i], panelX + SIDEBAR_CONTENT_LEFT, iconY,
                           i == side.selected ? activeTheme->accent : activeTheme->textSecondary);
      refreshLabelScale(&side.mainNames[i]);
      moveLabel(&side.mainNames[i], panelX + SIDEBAR_TEXT_LEFT,
                getCenteredLabelY(&side.mainNames[i], rowY, SIDEBAR_ROW_HEIGHT));
      drawLabel(&side.mainNames[i]);
   }
}

static void drawTrendingRows(int panelX)
{
   for (int i = 0; i < SIDEBAR_TREND_ROWS; i++) {
      int rowY = SIDEBAR_ROWS_TOP + i * SIDEBAR_ROW_STEP;
      if (i == side.selected)
         fillGfxRectangleFixed(panelX + 20, rowY,
                               side.panelW - 20 - SIDEBAR_EDGE_STRIP_WIDTH,
                               SIDEBAR_ROW_HEIGHT, activeTheme->rowHighlight);
      int iconY = rowY + (SIDEBAR_ROW_HEIGHT - SIDEBAR_ICON_SIZE) / 2;
      drawSidebarIconFixed(&side.trendingIcons[i], panelX + SIDEBAR_CONTENT_LEFT, iconY,
                           i == side.selected ? activeTheme->accent : activeTheme->textSecondary);
      refreshLabelScale(&side.trendingNames[i]);
      moveLabel(&side.trendingNames[i], panelX + SIDEBAR_TEXT_LEFT,
                getCenteredLabelY(&side.trendingNames[i], rowY, SIDEBAR_ROW_HEIGHT));
      drawLabel(&side.trendingNames[i]);
   }
}

void drawSidebar(void)
{
   float currentScale = getGfxUiScaleFactor();
   if (currentScale < 1.0f) currentScale = 1.0f;
   if (currentScale != side.uiScale) {
      side.uiScale = currentScale;
      side.panelW = getPanelWidth(side.screenW, side.uiScale);
   }

   keepSelectionVisible();
   int travel = side.panelW - SIDEBAR_EDGE_WIDTH;
   int panelX = -(int)((1.0f - side.progress) * travel + 0.5f);

   if (side.progress > 0.0f) {
      int panelRight = panelX + side.panelW;
      if (panelRight < side.screenW)
         fillGfxRectangleFixed(panelRight, 0, side.screenW - panelRight, side.screenH, activeTheme->scrim);
      fillGfxRectangleFixed(panelX, 0, side.panelW, side.screenH, activeTheme->surface);

      moveLabel(&side.title, panelX + SIDEBAR_CONTENT_LEFT, SIDEBAR_TITLE_Y);
      refreshLabelScale(&side.title);
      drawLabel(&side.title);

      if (side.page == SIDEBAR_PAGE_TRENDING) drawTrendingRows(panelX);
      else drawMainRows(panelX);
   }
   drawEdge(panelX);
}
