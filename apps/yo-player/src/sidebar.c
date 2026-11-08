// Sliding home navigation overlay. The grid remains underneath and is never
// resized. Trending and Live each have category tabs in the page header.
#include "sidebar.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "button-repeat.h"
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
#define SIDEBAR_SECTION_TEXT_SIZE   28
#define SIDEBAR_TITLE_TEXT_SIZE     36
#define SIDEBAR_EDGE_WIDTH         32
#define SIDEBAR_EDGE_STRIP_WIDTH   52
#define SIDEBAR_EDGE_HEIGHT        76
#define SIDEBAR_MIN_WIDTH         300
#define SIDEBAR_MAX_WIDTH         520
#define SIDEBAR_CONTENT_LEFT       48
#define SIDEBAR_CONTENT_OFFSET_PCT   2
#define SIDEBAR_TEXT_LEFT         100
#define SIDEBAR_RIGHT_PAD          14
#define SIDEBAR_TITLE_Y            34
#define SIDEBAR_ROWS_TOP          104
#define SIDEBAR_ROWS_BOTTOM        18
#define SIDEBAR_MAIN_ROWS          15
#define SIDEBAR_MAX_ROWS           SIDEBAR_MAIN_ROWS
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
   { "Live",           ICON_WIFI,      SIDEBAR_LIVE,                    0 },
   { "PS3",            0,              -1,                             ROW_SECTION },
   { "Subscriptions",  ICON_HDD,       SIDEBAR_PS3_SUBSCRIPTIONS,      0 },
   { "History",        ICON_HISTORY,   SIDEBAR_PS3_HISTORY,            0 },
   { "Watch Later",    ICON_CLOCK,     SIDEBAR_PS3_WATCH_LATER,        0 },
   { "APP",            0,              -1,                             ROW_SECTION },
   { "Account",        ICON_GOOGLE,    SIDEBAR_ACCOUNT,                0 },
   { "Settings",       ICON_COG,       SIDEBAR_SETTINGS,               0 },
   { "Exit App",        0,              SIDEBAR_EXIT,                    0 }
};

static struct {
   Font *font;
   int screenW, screenH, panelW;
   float uiScale;
   int selected;
   int scrollTop;
   int visibleRows;
   int focused;
   int targetOpen;
   float progress;
   uint64_t animationStart;
   float animationFrom;
   int animationRunning;
   ButtonRepeat navigateRepeat;
   Label title;
   Label mainNames[SIDEBAR_MAX_ROWS];
   Icon mainIcons[SIDEBAR_MAX_ROWS];
   Icon edgeOpen;
   Icon edgeClose;
   Icon exitArrow;
} side;

static float easeOut(float t)
{
   float inv = 1.0f - t;
   return 1.0f - inv * inv;
}

static int getPanelWidth(int screenW, float uiScale)
{
   if (uiScale < 1.0f) uiScale = 1.0f;
   int fontSize = (int)(SIDEBAR_TEXT_SIZE * uiScale + 0.5f);
   int textWidth = (int)(measureFontText(side.font, fontSize, "Recommended") + 0.5f);
   int twoSpaces = (int)(measureFontText(side.font, fontSize, "  ") + 0.5f);
   // End the bright panel about two spaces after its longest navigation label;
   // the fixed dark rail and arrow occupy the remaining edge strip.
   int contentOffset = screenW * SIDEBAR_CONTENT_OFFSET_PCT / 100;
   int width = SIDEBAR_TEXT_LEFT + contentOffset + textWidth + twoSpaces +
               SIDEBAR_EDGE_STRIP_WIDTH + SIDEBAR_RIGHT_PAD;
   if (width < SIDEBAR_MIN_WIDTH) width = SIDEBAR_MIN_WIDTH;
   if (width > SIDEBAR_MAX_WIDTH) width = SIDEBAR_MAX_WIDTH;
   int maxWidth = screenW * 72 / 100;
   if (width > maxWidth) width = maxWidth;
   if (width > screenW) width = screenW;
   return width;
}

static const SidebarRow *getRows(int *count)
{
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
      Label *label = &side.mainNames[i];
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

   int contentOffset = screenW * SIDEBAR_CONTENT_OFFSET_PCT / 100;
   initLabel(&side.title, font, 0, 0,
             SIDEBAR_MAX_WIDTH - SIDEBAR_CONTENT_LEFT - contentOffset - SIDEBAR_EDGE_STRIP_WIDTH - SIDEBAR_RIGHT_PAD,
             56, SIDEBAR_TITLE_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "CRTube");
   setLabelScaleWithUi(&side.title, 1);

   for (int i = 0; i < SIDEBAR_MAIN_ROWS; i++) {
      initLabel(&side.mainNames[i], font, 0, 0,
                SIDEBAR_MAX_WIDTH - SIDEBAR_TEXT_LEFT - contentOffset - SIDEBAR_EDGE_STRIP_WIDTH - SIDEBAR_RIGHT_PAD,
                46, (mainRows[i].flags & ROW_SECTION) ? SIDEBAR_SECTION_TEXT_SIZE : SIDEBAR_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, mainRows[i].name);
      setLabelScaleWithUi(&side.mainNames[i], 1);
      if (mainRows[i].icon) initIcon(&side.mainIcons[i], mainRows[i].icon, SIDEBAR_ICON_SIZE);
   }
   initIcon(&side.edgeOpen, ICON_LEFT_BIG, 27);
   initIcon(&side.edgeClose, ICON_LEFT_BIG, 27);
   initIcon(&side.exitArrow, ICON_RIGHT_BIG, SIDEBAR_ICON_SIZE);
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
   freeIcon(&side.edgeOpen);
   freeIcon(&side.edgeClose);
   freeIcon(&side.exitArrow);
   memset(&side, 0, sizeof side);
}

void setSidebarCurrent(SidebarItem item)
{
   switch (item) {
      case SIDEBAR_SEARCH:                  side.selected = 0; break;
      case SIDEBAR_RECOMMENDED:             side.selected = 1; break;
      case SIDEBAR_ACCOUNT_SUBSCRIPTIONS:   side.selected = 2; break;
      case SIDEBAR_ACCOUNT_HISTORY:         side.selected = 3; break;
      case SIDEBAR_ACCOUNT_WATCH_LATER:     side.selected = 4; break;
      case SIDEBAR_TRENDING:                side.selected = 5; break;
      case SIDEBAR_LIVE:                    side.selected = 6; break;
      case SIDEBAR_PS3_SUBSCRIPTIONS:       side.selected = 8; break;
      case SIDEBAR_PS3_HISTORY:             side.selected = 9; break;
      case SIDEBAR_PS3_WATCH_LATER:         side.selected = 10; break;
      case SIDEBAR_ACCOUNT:                 side.selected = 12; break;
      case SIDEBAR_SETTINGS:                side.selected = 13; break;
      case SIDEBAR_EXIT:                    side.selected = 14; break;
      default: break;
   }
   keepSelectionVisible();
   updateSelectedColors();
}

void openSidebar(void)
{
   side.focused = 1;
   side.navigateRepeat.timer = side.navigateRepeat.repeats = 0;
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
   if (isRepeatDue(&side.navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) moveSelection(1);
   else if (isRepeatDue(&side.navigateRepeat, getPadButtonState(PAD_BTN_UP))) moveSelection(-1);

   if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      side.focused = 0;
      animateTo(0);
      return -1;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      int count;
      const SidebarRow *rows = getRows(&count);
      int action = rows[side.selected].action;
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

static void drawSidebarRowIcon(Icon *icon, int x, int y, uint32_t color)
{
   if (!icon || !icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   // Give the navigation glyphs a little more visual weight at 1.5x without
   // scaling the sidebar geometry or the edge arrow.
   float iconScale = side.uiScale >= 1.25f ? 1.25f : 1.0f;
   int width = (int)(icon->tex->w * iconScale + 0.5f);
   int height = (int)(icon->tex->h * iconScale + 0.5f);
   int offsetX = (int)(icon->offsetX * iconScale + 0.5f);
   int offsetY = (int)(icon->offsetY * iconScale + 0.5f);
   drawGfxTextureFixedPosition(x + offsetX, y + offsetY, width, height, *icon->tex,
                               0.0f, 0.0f, 1.0f, 1.0f, color, GFX_FILTER_LINEAR);
}

static void drawExitDoorIcon(int x, int y, uint32_t color)
{
   int iconSize = (int)(SIDEBAR_ICON_SIZE * (side.uiScale >= 1.25f ? 1.25f : 1.0f) + 0.5f);
   int line = iconSize >= 30 ? 3 : 2;
   int width = iconSize * 70 / 100;
   fillGfxRectangleFixed(x + 2, y, line, iconSize, color);
   fillGfxRectangleFixed(x + 2, y, width, line, color);
   fillGfxRectangleFixed(x + 2, y + iconSize - line, width, line, color);
   drawSidebarRowIcon(&side.exitArrow, x + 5, y, color);
}

static void drawEdge(int panelX)
{
   int edgeX = panelX + side.panelW - SIDEBAR_EDGE_WIDTH;
   int edgeY = (side.screenH - SIDEBAR_EDGE_HEIGHT) / 2;
   Icon *glyph = &side.edgeOpen; // The indicator always points left.
   int glyphX;

   if (side.progress > 0.0f) {
      int railX = panelX + side.panelW - SIDEBAR_EDGE_STRIP_WIDTH;
      fillGfxRectangleFixed(railX, 0, SIDEBAR_EDGE_STRIP_WIDTH, side.screenH, activeTheme->appBg);
      // Keep the arrow centered in the rail, clear of its light inner edge.
      fillGfxRectangleFixed(railX, 0, 2, side.screenH, activeTheme->textSecondary);
      glyphX = railX + (SIDEBAR_EDGE_STRIP_WIDTH - glyph->size) / 2;
   } else {
      // Move only the closed-state indicator and arrow 1% left of the
      // previous position. The open rail and its arrow remain unchanged.
      int closedOffset = -(int)(side.screenW * 1.0f / 100.0f + 0.5f);
      int indicatorX = edgeX + closedOffset;
      fillGfxRoundedRectangleFixed(indicatorX, edgeY,
                                   SIDEBAR_EDGE_WIDTH, SIDEBAR_EDGE_HEIGHT,
                                   10, activeTheme->surface);
      glyphX = indicatorX + (SIDEBAR_EDGE_WIDTH - glyph->size) / 2;
   }

   drawSidebarIconFixed(glyph, glyphX,
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
         refreshLabelScale(&side.mainNames[i]);
         int contentOffset = side.screenW * SIDEBAR_CONTENT_OFFSET_PCT / 100;
         moveLabel(&side.mainNames[i], panelX + SIDEBAR_CONTENT_LEFT + contentOffset,
                   getCenteredLabelY(&side.mainNames[i], rowY, SIDEBAR_ROW_HEIGHT));
         drawLabel(&side.mainNames[i]);
         continue;
      }
      if (i == side.selected)
         fillGfxRectangleFixed(panelX + 36, rowY,
                               side.panelW - 36 - SIDEBAR_EDGE_STRIP_WIDTH,
                               SIDEBAR_ROW_HEIGHT, activeTheme->rowHighlight);
      int iconSize = (int)(SIDEBAR_ICON_SIZE * (side.uiScale >= 1.25f ? 1.25f : 1.0f) + 0.5f);
      int iconY = rowY + (SIDEBAR_ROW_HEIGHT - iconSize) / 2;
      int contentOffset = side.screenW * SIDEBAR_CONTENT_OFFSET_PCT / 100;
      uint32_t iconColor = i == side.selected ? activeTheme->accent : activeTheme->textSecondary;
      if (i == 14)
         drawExitDoorIcon(panelX + SIDEBAR_CONTENT_LEFT + contentOffset, iconY, iconColor);
      else
         drawSidebarRowIcon(&side.mainIcons[i], panelX + SIDEBAR_CONTENT_LEFT + contentOffset, iconY, iconColor);
      refreshLabelScale(&side.mainNames[i]);
      moveLabel(&side.mainNames[i], panelX + SIDEBAR_TEXT_LEFT + contentOffset,
                getCenteredLabelY(&side.mainNames[i], rowY, SIDEBAR_ROW_HEIGHT));
      drawLabel(&side.mainNames[i]);
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
   // Keep the collapsed indicator slightly inset from the physical left edge.
   // The panel still finishes at x=0 when fully open, so the indicator travels
   // with the panel edge throughout the same animation.
   int closedOffset = side.screenW * 5 / 100;
   int travel = side.panelW - SIDEBAR_EDGE_WIDTH - closedOffset;
   if (travel < 0) travel = 0;
   int panelX = -(int)((1.0f - side.progress) * travel + 0.5f);

   if (side.progress > 0.0f) {
      int panelRight = panelX + side.panelW;
      if (panelRight < side.screenW)
         fillGfxRectangleFixed(panelRight, 0, side.screenW - panelRight, side.screenH, activeTheme->scrim);
      fillGfxRectangleFixed(panelX, 0, side.panelW, side.screenH, activeTheme->surface);

      int contentOffset = side.screenW * SIDEBAR_CONTENT_OFFSET_PCT / 100;
      moveLabel(&side.title, panelX + SIDEBAR_CONTENT_LEFT + contentOffset, SIDEBAR_TITLE_Y);
      refreshLabelScale(&side.title);
      drawLabel(&side.title);

      drawMainRows(panelX);
   }
   drawEdge(panelX);
}
