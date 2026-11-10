// Search page: persistent recent queries before the first search, then the existing VideoGrid results.
#include "app-config.h"
#include "screens/search.h"
#include "screens/home.h"
#include "sidebar.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "settings.h"
#include "screens/playlist-picker.h"
#include "downloads.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "thread.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"
#include <string.h>
#include <stdlib.h>

#define MARGIN_X          80
#define HEADER_Y          34
#define FIELD_H           56
#define FIELD_TEXT_SIZE   32
#define PROMPT_TEXT_SIZE  FIELD_TEXT_SIZE
#define SORT_TEXT_SIZE    20
#define HISTORY_ROW_H     46
#define HISTORY_ROW_GAP    4
#define HINTS_BOTTOM      88
#define HINT_GLYPH_H     28
#define HINT_TEXT        22
#define SD_HINTS_BOTTOM 100
#define HINT_ROW_GAP      8
#define GRID_BOTTOM_GAP  24
#define MAX_HINT_ROWS     3
static const char *SortNames[SORT_COUNT] = { "Relevance", "Views", "Newest" };
static const char *ChannelSortNames[CHANNEL_SORT_COUNT] = { "Latest", "Popular", "Oldest" };

static char         pendingQuery[SEARCH_QUERY_LEN];
static SearchResult pendingChannel;
static int          pendingIsChannel;
static int          pendingLanding;
static int          pendingSort;
static int          pendingSortValid;

static struct {
   VideoGrid grid;
   char      query[SEARCH_QUERY_LEN];
   char      channelId[32];
   char      channelName[48];
   char      channelSortToken[MAX_SORT_TOKEN];
   int       channelSort;
   int       sort;
   int       screenW, screenH;
   int       landing;
   int       fieldFocused;
   int       resumeGridInput;
   char      history[MAX_SEARCH_HISTORY][SEARCH_QUERY_LEN];
   int       historyCount;
   int       historySelection;   // -1 = search field, otherwise a history row
   int       historyScroll;
   int       historyVisibleRows;
   float     uiScale;
   int       fieldHeight, fieldWidth, gridTop;
   int       sortX[SORT_COUNT], sortPillWidth[SORT_COUNT];
   int       sortPillY, sortPillHeight;
   int       historyTitleY, historyTop, historyRowHeight, historyRowGap;
   int       footerBottomY, footerTopY, footerRows;
} search;

static Font        uiFont;
static Label       queryLabel, queryPromptPrefix, queryPromptSuffix;
static Label       sortLabel, sortLabels[SORT_COUNT], statusLabel, historyTitleLabel, historyEmptyLabel;
static Label       historyLabels[MAX_SEARCH_HISTORY];
static Icon        searchIcon;
static GfxTexture  searchCrossGlyph;
static ButtonHints hints;
static int crossHint = -1, squareHint = -1, triangleHint = -1;
static int selectHint = -1, downloadHint = -1, settingsHint = -1;
static int sortHint = -1, refreshHint = -1, clearHistoryHint = -1;
static SearchResult squarePendingItem;
static uint64_t squarePressedAt;
#define SQUARE_HOLD_US 550000ULL

static void reloadHistory(void)
{
   search.historyCount = getSearchHistory(search.history, MAX_SEARCH_HISTORY);
   if (search.historySelection >= search.historyCount)
      search.historySelection = search.historyCount - 1;
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++)
      setLabelText(&historyLabels[i], i < search.historyCount ? search.history[i] : "");
   if (search.historySelection < -1) search.historySelection = -1;
   if (search.historySelection >= 0) {
      if (search.historySelection < search.historyScroll) search.historyScroll = search.historySelection;
      else if (search.historySelection >= search.historyScroll + search.historyVisibleRows)
         search.historyScroll = search.historySelection - search.historyVisibleRows + 1;
   } else {
      search.historyScroll = 0;
   }
   if (search.historyScroll < 0) search.historyScroll = 0;
}

static void updateSearchHints(void)
{
   int hasHistory = search.historyCount > 0;
   int fieldAction = search.landing ? (search.historySelection >= 0 ? 1 : 0) : search.fieldFocused;
   setButtonHintCaption(&hints, crossHint, fieldAction ? (search.landing ? "Search" : "Type") :
                        (search.landing ? "Type" : "Play"));
   if (squareHint >= 0) setButtonHintShown(&hints, squareHint, !search.landing && !search.fieldFocused);
   if (triangleHint >= 0) setButtonHintShown(&hints, triangleHint, !search.landing && !search.fieldFocused);

   if (selectHint >= 0) setButtonHintShown(&hints, selectHint, !search.landing);
   if (downloadHint >= 0) setButtonHintShown(&hints, downloadHint, !search.landing && !search.fieldFocused);
   if (settingsHint >= 0) setButtonHintShown(&hints, settingsHint, !search.landing);
   if (sortHint >= 0) setButtonHintShown(&hints, sortHint, !search.channelId[0]);
   if (refreshHint >= 0) setButtonHintShown(&hints, refreshHint, !search.landing);
   if (clearHistoryHint >= 0) setButtonHintShown(&hints, clearHistoryHint, hasHistory);
   if (search.landing) {
      if (squareHint >= 0) setButtonHintShown(&hints, squareHint, 0);
      if (triangleHint >= 0) setButtonHintShown(&hints, triangleHint, 0);
      if (downloadHint >= 0) setButtonHintShown(&hints, downloadHint, 0);
   }
}

static void setQueryField(void)
{
   const char *text = search.channelId[0] ? search.channelName : search.query;
   if (text[0]) {
      setLabelText(&queryLabel, text);
      setLabelColor(&queryLabel, activeTheme->textPrimary);
      setLabelText(&queryPromptPrefix, "");
      setLabelText(&queryPromptSuffix, "");
   } else {
      setLabelText(&queryLabel, "");
      setLabelText(&queryPromptPrefix, "Press ");
      setLabelText(&queryPromptSuffix, " to enter search");
      setLabelColor(&queryPromptPrefix, activeTheme->textSecondary);
      setLabelColor(&queryPromptSuffix, activeTheme->textSecondary);
   }
}

static void prependLiveChannelItems(SearchResults *live, SearchResults *regular)
{
   int liveCount = 0;
   int capacity = MAX_SEARCH_RESULTS - regular->count;
   for (int i = 0; i < live->count && liveCount < capacity; i++) {
      SearchResult *item = &live->items[i];
      int duplicate = 0;
      for (int j = 0; j < regular->count; j++) {
         if (strcmp(regular->items[j].videoId, item->videoId) == 0) { duplicate = 1; break; }
      }
      for (int j = 0; !duplicate && j < liveCount; j++)
         if (strcmp(live->items[j].videoId, item->videoId) == 0) duplicate = 1;
      if (duplicate) continue;
      if (!item->channelId[0]) strCopy(item->channelId, sizeof item->channelId, search.channelId);
      if (!item->author[0])
         strCopy(item->author, sizeof item->author,
                 search.channelName[0] ? search.channelName : regular->channelName);
      if (liveCount != i) live->items[liveCount] = *item;
      liveCount++;
   }
   if (liveCount <= 0) return;
   memmove(regular->items + liveCount, regular->items, regular->count * sizeof(regular->items[0]));
   memcpy(regular->items, live->items, liveCount * sizeof(regular->items[0]));
   regular->count += liveCount;
}

static int searchFetch(const char *token, SearchResults *out, void *user)
{
   int rc;
   (void)user;
   if (search.channelId[0]) {
      const char *first = search.channelSortToken[0] ? search.channelSortToken : NULL;
      SearchResults *live = NULL;
      int liveRc = -1;
      if (!token) {
         live = malloc(sizeof(*live));
         if (live) liveRc = getChannelLiveVideos(search.channelId, live);
      }
      rc = getChannelVideos(search.channelId, token ? token : first, out);
      if (rc == 0 && live && liveRc == 0 && live->count > 0)
         prependLiveChannelItems(live, out);
      free(live);
   } else {
      rc = searchVideos(search.query, search.sort, token, out);
   }
   return rc;
}

static void updateTriangleHint(void)
{
   const char *caption = "Channel";
   if (search.channelId[0]) caption = isSubscribed(search.channelId) ? "Unsubscribe" : "Subscribe";
   if (triangleHint >= 0) setButtonHintCaption(&hints, triangleHint, caption);
}

static void setHeading(void)
{
   setQueryField();
   if (search.channelId[0]) setLabelText(&sortLabel, ChannelSortNames[search.channelSort]);
   else setLabelText(&sortLabel, SortNames[search.sort]);
   updateTriangleHint();
   updateSearchHints();
}

static void reload(void)
{
   search.landing = 0;
   search.fieldFocused = 0;
   search.resumeGridInput = 0;
   setGridInputSuppressed(&search.grid, 0);
   setGridFocusHidden(&search.grid, 0);
   setGridSortByPublished(&search.grid, !search.channelId[0] && search.sort == SORT_NEWEST);
   setHeading();
   setGridSource(&search.grid, searchFetch, NULL);
}

static void enterChannel(const SearchResult *item)
{
   if (!item || !item->channelId[0]) return;
   strCopy(search.channelId, sizeof search.channelId, item->channelId);
   strCopy(search.channelName, sizeof search.channelName, item->author[0] ? item->author : "Channel");
   search.channelSort = CHANNEL_SORT_LATEST;
   search.channelSortToken[0] = 0;
   reload();
}

static void onQueryEntered(const char *text)
{
   if (!text || !text[0]) return;
   if (strstr(text, "youtu")) {
      stopVideoGrid(&search.grid);
      playVideo(text);
      return;
   }
   addSearchHistory(text);
   reloadHistory();
   search.channelId[0] = 0;
   strCopy(search.query, sizeof search.query, text);
   reload();
}

static void clearHistory(void)
{
   clearSearchHistory();
   search.historySelection = -1;
   search.historyScroll = 0;
   reloadHistory();
   updateSearchHints();
}

static void initSearch(void)
{
   search.screenW = getGfxScreenWidth();
   search.screenH = getGfxScreenHeight();
   search.uiScale = getGfxUiScaleFactor();
   if (search.uiScale < 1.0f) search.uiScale = 1.0f;
   search.fieldHeight = (int)(FIELD_H * search.uiScale + 0.5f);
   int homeGridTop = isGfxSdVideoOutput() ? 132 : 100;
   int headerGridTop = HEADER_Y + search.fieldHeight + (int)(10 * search.uiScale + 0.5f);
   search.gridTop = headerGridTop > homeGridTop ? headerGridTop : homeGridTop;
   search.historyTitleY = search.gridTop + (int)(16 * search.uiScale + 0.5f);
   search.historyTop = search.gridTop + (int)(58 * search.uiScale + 0.5f);
   search.historyRowHeight = (int)(HISTORY_ROW_H * search.uiScale + 0.5f);
   search.historyRowGap = (int)(HISTORY_ROW_GAP * search.uiScale + 0.5f);
   search.footerBottomY = search.screenH - (isGfxSdVideoOutput() ? SD_HINTS_BOTTOM : HINTS_BOTTOM);

   memset(search.channelId, 0, sizeof search.channelId);
   search.query[0] = 0;
   search.sort = pendingSortValid && pendingSort >= SORT_RELEVANCE && pendingSort < SORT_COUNT
                 ? (SortOrder)pendingSort : (SortOrder)getSortPreference(SORT_PREF_SEARCH);
   pendingSortValid = 0;
   squarePressedAt = 0;
   search.landing = pendingLanding && !pendingIsChannel;
   search.fieldFocused = 0;
   search.resumeGridInput = 0;
   search.historySelection = -1;
   search.historyScroll = 0;

   if (pendingIsChannel) {
      strCopy(search.channelId, sizeof search.channelId, pendingChannel.channelId);
      strCopy(search.channelName, sizeof search.channelName,
              pendingChannel.author[0] ? pendingChannel.author : "Channel");
      search.channelSort = CHANNEL_SORT_LATEST;
      search.channelSortToken[0] = 0;
   } else {
      strCopy(search.query, sizeof search.query, pendingQuery);
   }

   uiFont = openSystemFont(FONT_POP);
   int sortPad = (int)(4 * search.uiScale + 0.5f);
   int sortGap = (int)(3 * search.uiScale + 0.5f);
   int sortGroupWidth = 0;
   for (int i = 0; i < SORT_COUNT; i++) {
      initLabel(&sortLabels[i], &uiFont, 0, 0, AUTO, AUTO, SORT_TEXT_SIZE,
                activeTheme->textSecondary, TEXT_NOWRAP, SortNames[i]);
      setLabelScaleWithUi(&sortLabels[i], 1);
      search.sortPillWidth[i] = sortLabels[i].tt.tex.w + sortPad * 2;
      sortGroupWidth += search.sortPillWidth[i];
   }
   sortGroupWidth += sortGap * (SORT_COUNT - 1);
   search.fieldWidth = search.screenW - 2 * MARGIN_X - sortGroupWidth -
                       (int)(10 * search.uiScale + 0.5f);
   if (search.fieldWidth < (int)(200 * search.uiScale + 0.5f)) {
      int excess = (int)(200 * search.uiScale + 0.5f) - search.fieldWidth;
      int reduce = (excess + SORT_COUNT * 2 - 1) / (SORT_COUNT * 2);
      for (int i = 0; i < SORT_COUNT; i++) {
         search.sortPillWidth[i] -= reduce * 2;
         if (search.sortPillWidth[i] < sortLabels[i].tt.tex.w + 8)
            search.sortPillWidth[i] = sortLabels[i].tt.tex.w + 8;
      }
      sortGroupWidth = sortGap * (SORT_COUNT - 1);
      for (int i = 0; i < SORT_COUNT; i++) sortGroupWidth += search.sortPillWidth[i];
      search.fieldWidth = search.screenW - 2 * MARGIN_X - sortGroupWidth -
                          (int)(8 * search.uiScale + 0.5f);
   }
   search.sortPillHeight = search.fieldHeight - (int)(8 * search.uiScale + 0.5f);
   search.sortPillY = HEADER_Y + (search.fieldHeight - search.sortPillHeight) / 2;
   int sortRight = search.screenW - MARGIN_X;
   for (int i = SORT_COUNT - 1; i >= 0; i--) {
      search.sortX[i] = sortRight - search.sortPillWidth[i];
      sortRight = search.sortX[i] - sortGap;
   }
   int queryWidth = search.fieldWidth - (int)(70 * search.uiScale + 0.5f);
   if (queryWidth < 32) queryWidth = 32;
   initLabelRaw(&queryLabel, &uiFont,
                MARGIN_X + (int)(58 * search.uiScale + 0.5f),
                HEADER_Y + (int)(7 * search.uiScale + 0.5f),
                queryWidth, FIELD_H - 10, FIELD_TEXT_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   setLabelScaleWithUi(&queryLabel, 1);
   initLabelRaw(&queryPromptPrefix, &uiFont, 0, 0, 100, FIELD_H - 10,
                PROMPT_TEXT_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Press ");
   setLabelScaleWithUi(&queryPromptPrefix, 1);
   searchCrossGlyph = getConsoleGlyph(GLYPH_CROSS);
   int promptCrossH = (int)(PROMPT_TEXT_SIZE * search.uiScale * 0.92f + 0.5f);
   int promptCrossW = searchCrossGlyph.h > 0 ?
      searchCrossGlyph.w * promptCrossH / searchCrossGlyph.h : promptCrossH;
   int promptStartOffset = (int)(18 * search.uiScale + 0.5f) + queryPromptPrefix.tt.tex.w +
                           (int)(18 * search.uiScale + 0.5f) + promptCrossW;
   int promptSuffixWidth = search.fieldWidth - promptStartOffset -
                           (int)(8 * search.uiScale + 0.5f);
   if (promptSuffixWidth < 32) promptSuffixWidth = 32;
   initLabelRaw(&queryPromptSuffix, &uiFont, 0, 0, promptSuffixWidth, FIELD_H - 10,
                PROMPT_TEXT_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, " to enter search");
   setLabelScaleWithUi(&queryPromptSuffix, 1);
   initLabel(&sortLabel, &uiFont, 0, 0, AUTO, AUTO, 21,
             activeTheme->textSecondary, TEXT_NOWRAP, "");
   setLabelScaleWithUi(&sortLabel, 1);
   initLabelRaw(&statusLabel, &uiFont, MARGIN_X, 0,
                search.screenW - 2 * MARGIN_X, AUTO, 21,
                activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   setLabelScaleWithUi(&statusLabel, 1);
   initLabel(&historyTitleLabel, &uiFont, MARGIN_X, 0, AUTO, AUTO, 24,
             activeTheme->textSecondary, TEXT_NOWRAP, "Recent searches");
   setLabelScaleWithUi(&historyTitleLabel, 1);
   initLabel(&historyEmptyLabel, &uiFont, MARGIN_X, 0, AUTO, AUTO, 22,
             activeTheme->textSecondary, TEXT_NOWRAP, "No recent searches");
   setLabelScaleWithUi(&historyEmptyLabel, 1);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++) {
      initLabelRaw(&historyLabels[i], &uiFont, 0, 0,
                   search.screenW - 2 * MARGIN_X - 36, HISTORY_ROW_H - 4,
                   22, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
      setLabelScaleWithUi(&historyLabels[i], 1);
   }
   initIcon(&searchIcon, ICON_SEARCH, 28);

   initButtonHints(&hints, &uiFont, 0, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   crossHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS), "Type");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE), "Back");
   squareHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "Watch Later");
   triangleHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
   selectHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT), "Search");
   downloadHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R3), "Download");
   settingsHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_START), "Settings");
   sortHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L2), "Sort");
   refreshHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R2), "Refresh");
   clearHistoryHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L3), "Clear history");
   setButtonHintsStableLayout(&hints, 1);
   // Measure the longest dynamic captions so later mode changes cannot need an unreserved row.
   setButtonHintCaption(&hints, crossHint, "Open playlist");
   setButtonHintCaption(&hints, triangleHint, "Unsubscribe");
   search.footerRows = getButtonHintsFitRows(&hints, search.screenW, MAX_HINT_ROWS);
   if (search.footerRows < 1) search.footerRows = 1;
   float hintGlyphFinal = HINT_GLYPH_H * (isGfxSdVideoOutput() ? 1.5f : search.uiScale);
   int footerRowStep = (int)(hintGlyphFinal + HINT_ROW_GAP + 0.5f);
   search.footerTopY = search.footerBottomY - (search.footerRows - 1) * footerRowStep;

   search.historyVisibleRows = (search.footerTopY - search.historyTop - 12) /
                               (search.historyRowHeight + search.historyRowGap);
   if (search.historyVisibleRows < 1) search.historyVisibleRows = 1;
   if (search.historyVisibleRows > MAX_SEARCH_HISTORY) search.historyVisibleRows = MAX_SEARCH_HISTORY;
   reloadHistory();

   initVideoGrid(&search.grid, &uiFont, MARGIN_X, search.gridTop,
                 search.screenW - 2 * MARGIN_X,
                 search.footerTopY - search.gridTop - GRID_BOTTOM_GAP);
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchedPosition(&search.grid, getWatchedPosition);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   setGridPlaylistWatchLaterPredicate(&search.grid, isPlaylistWatchLater);
   setGridInputSuppressed(&search.grid, search.landing);
   setGridFocusHidden(&search.grid, search.landing);
   setSidebarCurrent(SIDEBAR_SEARCH);
   setHeading();
   if (!search.landing) reload();
}

static void updateSearch(void)
{
   updateDownloadOverlay();
   if (oskInputActive()) return;

   if (!isSidebarActive() && isPadButtonPressed(PAD_BTN_LEFT) &&
       (search.landing || search.fieldFocused || gridStage(&search.grid) != GRID_READY ||
        search.grid.columns <= 0 || search.grid.selected < 0 ||
        (search.grid.selected % search.grid.columns) == 0)) {
      openSidebar();
      return;
   }
   if (isSidebarActive()) {
      int picked = updateSidebar();
      if (picked >= 0) {
         popScreen();
         handleHomeSidebarSelection(picked);
      }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START)) {
      popScreen();
      openHomeSettingsPanel();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }
   if (isPadButtonPressed(PAD_BTN_L3) && search.historyCount > 0) {
      clearHistory();
      return;
   }

   if (search.landing) {
      if (isPadButtonPressed(PAD_BTN_L2)) {
         search.sort = (SortOrder)((search.sort + 1) % SORT_COUNT);
         setSortPreference(SORT_PREF_SEARCH, (int)search.sort);
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (search.historySelection < 0) {
            oskInputBegin("Search YouTube", search.query, onQueryEntered);
         } else {
            char chosen[SEARCH_QUERY_LEN];
            strCopy(chosen, sizeof chosen, search.history[search.historySelection]);
            onQueryEntered(chosen);
         }
         return;
      }
      if (isRepeatDue(&search.grid.verticalRepeat, getPadButtonState(PAD_BTN_DOWN))) {
         if (search.historyCount > 0 && search.historySelection < search.historyCount - 1)
            search.historySelection++;
      } else if (isRepeatDue(&search.grid.verticalRepeat, getPadButtonState(PAD_BTN_UP))) {
         if (search.historySelection >= 0) search.historySelection--;
      }
      if (search.historySelection >= 0) {
         if (search.historySelection < search.historyScroll) search.historyScroll = search.historySelection;
         else if (search.historySelection >= search.historyScroll + search.historyVisibleRows)
            search.historyScroll = search.historySelection - search.historyVisibleRows + 1;
      } else search.historyScroll = 0;
      updateSearchHints();
      return;
   }

   if (isPadButtonPressed(PAD_BTN_SELECT) ||
       (search.fieldFocused && isPadButtonPressed(PAD_BTN_CROSS))) {
      oskInputBegin("Search YouTube", search.query, onQueryEntered);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_R2)) { reload(); return; }
   if (!search.channelId[0] && isPadButtonPressed(PAD_BTN_L2)) {
      search.sort = (SortOrder)((search.sort + 1) % SORT_COUNT);
      setSortPreference(SORT_PREF_SEARCH, (int)search.sort);
      reload();
      return;
   }

   if (!search.fieldFocused && gridStage(&search.grid) == GRID_READY &&
       isPadButtonPressed(PAD_BTN_UP) && search.grid.selected < search.grid.columns) {
      search.fieldFocused = 1;
      search.resumeGridInput = 0;
      setGridInputSuppressed(&search.grid, 1);
      setGridFocusHidden(&search.grid, 1);
      updateSearchHints();
   } else if (search.fieldFocused && isPadButtonPressed(PAD_BTN_DOWN)) {
      search.fieldFocused = 0;
      search.resumeGridInput = 1;
      updateSearchHints();
   }

   if (search.resumeGridInput && isPadButtonReleased(PAD_BTN_DOWN)) {
      search.resumeGridInput = 0;
      setGridInputSuppressed(&search.grid, 0);
      setGridFocusHidden(&search.grid, 0);
   }
   updateVideoGrid(&search.grid);
   if (search.fieldFocused || search.resumeGridInput) {
      setGridInputSuppressed(&search.grid, 1);
      setGridFocusHidden(&search.grid, 1);
   }

   if (gridStage(&search.grid) != GRID_READY || search.fieldFocused || search.resumeGridInput) return;

   if (search.channelId[0]) {
      const SearchResults *results = gridResults(&search.grid);
      if (results->channelName[0] && strcmp(search.channelName, results->channelName) != 0) {
         strCopy(search.channelName, sizeof search.channelName, results->channelName);
         setQueryField();
      }
   }

   const SearchResult *selected = gridSelected(&search.grid);
   if (!selected) return;
   if (selected->isPlaylist) {
      setButtonHintCaption(&hints, crossHint, "Open playlist");
      if (squareHint >= 0) { setButtonHintCaption(&hints, squareHint, "Save playlist"); setButtonHintShown(&hints, squareHint, 1); }
      if (triangleHint >= 0) setButtonHintShown(&hints, triangleHint, 0);
      if (downloadHint >= 0) setButtonHintShown(&hints, downloadHint, 0);
   } else {
      setButtonHintCaption(&hints, crossHint, "Play");
      if (squareHint >= 0) { setButtonHintCaption(&hints, squareHint, "Watch Later / Playlist (hold)"); setButtonHintShown(&hints, squareHint, 1); }
      if (triangleHint >= 0) setButtonHintShown(&hints, triangleHint, 1);
      if (downloadHint >= 0) setButtonHintShown(&hints, downloadHint, 1);
   }
   if (squarePressedAt) {
      if (isPadButtonDown(PAD_BTN_SQUARE) &&
          getTimeUs() - squarePressedAt >= SQUARE_HOLD_US) {
         squarePressedAt = 0;
         openLocalPlaylistPicker(&squarePendingItem);
         return;
      }
      if (isPadButtonReleased(PAD_BTN_SQUARE)) {
         toggleWatchLater(&squarePendingItem);
         squarePressedAt = 0;
         return;
      }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      if (selected->isPlaylist) addPlaylistToWatchLater(selected);
      else {
         squarePendingItem = *selected;
         squarePressedAt = getTimeUs();
      }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isPlaylist && !selected->isLive) enqueueDownload(selected); return; }
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      if (selected->isPlaylist) return;
      if (search.channelId[0]) {
         setSubscribed(search.channelId, !isSubscribed(search.channelId));
         updateTriangleHint();
      } else {
         enterChannel(selected);
      }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (selected->isPlaylist && selected->playlistId[0]) {
         char playlistId[64], playlistTitle[160];
         strCopy(playlistId, sizeof playlistId, selected->playlistId);
         strCopy(playlistTitle, sizeof playlistTitle, selected->title);
         popScreen();
         openHomePlaylistFromSearch(playlistId, playlistTitle, search.query, (int)search.sort);
         return;
      }
      if (selected->videoId[0]) {
         markWatched(selected->videoId);
         stopVideoGrid(&search.grid);
         playVideo(selected->videoId);
      }
   }
}

static uint32_t getSearchFieldColor(void)
{
   uint32_t color = activeTheme->surface;
   int r = (color >> 16) & 0xFF;
   int g = (color >> 8) & 0xFF;
   int b = color & 0xFF;
   r += (255 - r) / 10;
   g += (255 - g) / 10;
   b += (255 - b) / 10;
   return (color & 0xFF000000) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void drawSearchBar(void)
{
   int fieldX = MARGIN_X;
   int fieldY = HEADER_Y;
   int fieldH = search.fieldHeight;
   int fieldW = search.fieldWidth;
   int fieldFocused = search.landing ? search.historySelection < 0 : search.fieldFocused;
   int border = (int)(search.uiScale + 0.5f);
   if (border < 1) border = 1;
   int radius = fieldH / 2;
   uint32_t borderColor = fieldFocused ? activeTheme->accent : activeTheme->rowHighlight;

   fillGfxRoundedRectangleFixed(fieldX, fieldY, fieldW, fieldH, radius, borderColor);
   fillGfxRoundedRectangleFixed(fieldX + border, fieldY + border,
                                fieldW - border * 2, fieldH - border * 2,
                                radius - border, getSearchFieldColor());

   if (search.channelId[0] || search.query[0]) {
      if (searchIcon.tex && searchIcon.tex->w > 0 && searchIcon.tex->h > 0) {
         int iconH = (int)(searchIcon.tex->h * search.uiScale + 0.5f);
         int iconW = (int)(searchIcon.tex->w * search.uiScale + 0.5f);
         int iconX = fieldX + (int)(16 * search.uiScale + 0.5f);
         int iconY = fieldY + (fieldH - iconH) / 2;
         drawGfxTextureFixedPosition(iconX + (int)(searchIcon.offsetX * search.uiScale),
                                     iconY + (int)(searchIcon.offsetY * search.uiScale),
                                     iconW, iconH, *searchIcon.tex, 0.0f, 0.0f, 1.0f, 1.0f,
                                     fieldFocused ? activeTheme->accent : activeTheme->textSecondary,
                                     GFX_FILTER_LINEAR);
      }
      refreshLabelScale(&queryLabel);
      moveLabel(&queryLabel,
                fieldX + (int)(58 * search.uiScale + 0.5f),
                fieldY + (fieldH - queryLabel.tt.tex.h) / 2);
      drawLabel(&queryLabel);
   } else {
      refreshLabelScale(&queryPromptPrefix);
      refreshLabelScale(&queryPromptSuffix);
      int promptX = fieldX + (int)(18 * search.uiScale + 0.5f);
      int promptY = fieldY + (fieldH - queryPromptPrefix.tt.tex.h) / 2;
      moveLabel(&queryPromptPrefix, promptX, promptY);
      drawLabel(&queryPromptPrefix);
      int crossH = (int)(PROMPT_TEXT_SIZE * search.uiScale * 0.92f + 0.5f);
      int crossW = searchCrossGlyph.h > 0 ? searchCrossGlyph.w * crossH / searchCrossGlyph.h : crossH;
      int crossX = promptX + queryPromptPrefix.tt.tex.w + (int)(18 * search.uiScale + 0.5f);
      int crossY = fieldY + (fieldH - crossH) / 2;
      if (searchCrossGlyph.offset)
         drawGfxTextureFixedPosition(crossX, crossY, crossW, crossH, searchCrossGlyph,
                                     0.0f, 0.0f, 1.0f, 1.0f,
                                     activeTheme->textSecondary, GFX_FILTER_LINEAR);
      int suffixX = crossX + crossW + (int)(14 * search.uiScale + 0.5f);
      moveLabel(&queryPromptSuffix, suffixX, promptY);
      drawLabel(&queryPromptSuffix);
   }
}

static void drawSearchSortPills(void)
{
   int border = (int)(search.uiScale + 0.5f);
   if (border < 1) border = 1;
   int radius = search.sortPillHeight / 2;
   for (int i = 0; i < SORT_COUNT; i++) {
      int selected = i == search.sort;
      uint32_t edge = selected ? activeTheme->accent : activeTheme->rowHighlight;
      uint32_t fill = selected ? activeTheme->rowHighlight : activeTheme->surface;
      int x = search.sortX[i];
      int y = search.sortPillY;
      int w = search.sortPillWidth[i];
      int h = search.sortPillHeight;
      fillGfxRoundedRectangleFixed(x, y, w, h, radius, edge);
      fillGfxRoundedRectangleFixed(x + border, y + border, w - border * 2, h - border * 2,
                                   radius - border, fill);
      refreshLabelScale(&sortLabels[i]);
      int textX = x + (w - sortLabels[i].tt.tex.w) / 2;
      int textY = y + (h - sortLabels[i].tt.tex.h) / 2;
      // A compact horizontal outline improves contrast without increasing the pill height.
      setLabelColor(&sortLabels[i], 0xFF202020);
      drawLabelAt(&sortLabels[i], textX - 2, textY);
      drawLabelAt(&sortLabels[i], textX + 2, textY);
      drawLabelAt(&sortLabels[i], textX - 1, textY - 1);
      drawLabelAt(&sortLabels[i], textX + 1, textY - 1);
      drawLabelAt(&sortLabels[i], textX - 1, textY + 1);
      drawLabelAt(&sortLabels[i], textX + 1, textY + 1);
      setLabelColor(&sortLabels[i], selected ? activeTheme->textPrimary : activeTheme->textSecondary);
      drawLabelAt(&sortLabels[i], textX, textY);
   }
}

static void drawHistory(void)
{
   refreshLabelScale(&historyTitleLabel);
   moveLabel(&historyTitleLabel, MARGIN_X, search.historyTitleY);
   drawLabel(&historyTitleLabel);
   if (!search.historyCount) {
      refreshLabelScale(&historyEmptyLabel);
      moveLabel(&historyEmptyLabel, MARGIN_X + (int)(18 * search.uiScale + 0.5f), search.historyTop);
      drawLabel(&historyEmptyLabel);
      return;
   }
   int end = search.historyScroll + search.historyVisibleRows;
   if (end > search.historyCount) end = search.historyCount;
   int width = search.screenW - 2 * MARGIN_X;
   int inset = (int)(18 * search.uiScale + 0.5f);
   for (int i = search.historyScroll; i < end; i++) {
      int y = search.historyTop + (i - search.historyScroll) *
              (search.historyRowHeight + search.historyRowGap);
      int selected = i == search.historySelection;
      if (selected) {
         int radius = (int)(9 * search.uiScale + 0.5f);
         fillGfxRoundedRectangleFixed(MARGIN_X, y, width, search.historyRowHeight,
                                      radius, activeTheme->rowHighlight);
      } else {
         fillGfxRectangleFixed(MARGIN_X, y, width, search.historyRowHeight, activeTheme->surface);
      }
      setLabelColor(&historyLabels[i], selected ? activeTheme->accent : activeTheme->textPrimary);
      refreshLabelScale(&historyLabels[i]);
      moveLabel(&historyLabels[i], MARGIN_X + inset,
                y + (search.historyRowHeight - historyLabels[i].tt.tex.h) / 2);
      drawLabel(&historyLabels[i]);
   }
}

static void drawSearch(void)
{
   fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->appBg);
   drawSearchBar();

   if (search.landing) {
      drawSearchSortPills();
      drawHistory();
   } else {
      if (search.channelId[0]) {
         refreshLabelScale(&sortLabel);
         moveLabel(&sortLabel, search.screenW - MARGIN_X - sortLabel.tt.tex.w,
                   HEADER_Y + (search.fieldHeight - sortLabel.tt.tex.h) / 2);
         drawLabel(&sortLabel);
      } else {
         drawSearchSortPills();
      }
      if (gridStage(&search.grid) == GRID_READY) {
         drawVideoGrid(&search.grid);
      } else {
         setLabelText(&statusLabel, gridStage(&search.grid) == GRID_EMPTY ? "No results" : "Loading...");
         refreshLabelScale(&statusLabel);
         moveLabel(&statusLabel, MARGIN_X, search.gridTop);
         drawLabel(&statusLabel);
      }
   }

   drawButtonHintsFitted(&hints, search.screenW, search.footerBottomY, HINT_ROW_GAP, MAX_HINT_ROWS);
   drawDownloadOverlay(search.screenW);
   drawSidebar();
}

static void suspendSearch(void)
{
   squarePressedAt = 0;
   pauseVideoGrid(&search.grid);
}

static void resumeSearch(void)
{
   resumeVideoGrid(&search.grid);
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   termButtonHints(&hints);
   freeLabel(&queryLabel);
   freeLabel(&queryPromptPrefix);
   freeLabel(&queryPromptSuffix);
   freeLabel(&sortLabel);
   for (int i = 0; i < SORT_COUNT; i++) freeLabel(&sortLabels[i]);
   freeLabel(&statusLabel);
   freeLabel(&historyTitleLabel);
   freeLabel(&historyEmptyLabel);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++) freeLabel(&historyLabels[i]);
   freeIcon(&searchIcon);
   closeFont(&uiFont);
}

void openSearchPage(void)
{
   pendingQuery[0] = 0;
   pendingIsChannel = 0;
   pendingLanding = 1;
   pendingSortValid = 0;
   pushScreen(&searchScreen);
}

void openSearchQuery(const char *query)
{
   if (!query || !query[0]) { openSearchPage(); return; }
   strCopy(pendingQuery, sizeof pendingQuery, query);
   pendingIsChannel = 0;
   pendingLanding = 0;
   pendingSortValid = 0;
   addSearchHistory(query);
   pushScreen(&searchScreen);
}

void openSearchQueryWithSort(const char *query, int sort)
{
   if (!query || !query[0]) { openSearchPage(); return; }
   strCopy(pendingQuery, sizeof pendingQuery, query);
   pendingIsChannel = 0;
   pendingLanding = 0;
   pendingSort = sort;
   pendingSortValid = 1;
   addSearchHistory(query);
   pushScreen(&searchScreen);
}

void openChannelFor(const SearchResult *item)
{
   if (!item || !item->channelId[0]) return;
   pendingChannel = *item;
   pendingIsChannel = 1;
   pendingLanding = 0;
   pendingSortValid = 0;
   pendingQuery[0] = 0;
   pushScreen(&searchScreen);
}

Screen searchScreen = { initSearch, resumeSearch, updateSearch, drawSearch, suspendSearch, termSearch, SCREEN_TERMINATED };
