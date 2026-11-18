// Search page: persistent recent queries before the first search, then the existing VideoGrid results.
#include "app-config.h"
#include "dbg.h"
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
#include "ui/loading-indicator.h"

#include "gfx.h"
#include "image-loader.h"
#include "theme.h"
#include "pad.h"
#include "thread.h"
#include "http.h"               // channel feed request-group cancellation
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"
#include "path.h"                // MAX_PATH_LEN for local download paths
#include "vfs.h"
#include "dbg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MARGIN_X          80
#define HEADER_Y          34
#define FIELD_H           56
#define FIELD_TEXT_SIZE   32
#define PROMPT_TEXT_SIZE  FIELD_TEXT_SIZE
#define SORT_TEXT_SIZE    20
#define HISTORY_ROW_H     46
#define HISTORY_ROW_GAP    4
#define HINTS_BOTTOM      42
#define HINT_GLYPH_H     28
#define HINT_TEXT        22
#define SD_HINTS_BOTTOM 76
#define HINT_ROW_GAP      8
#define GRID_BOTTOM_GAP  24
#define MAX_HINT_ROWS     2
#define CHANNEL_INFO_MARGIN 72
#define CHANNEL_INFO_PANEL_PAD 28
#define CHANNEL_AVATAR_CAP (256 * 1024)
#define CHANNEL_AVATAR_CACHE_DIR "/dev_hdd0/tmp/crtube/channel-avatar-cache"
#define CHANNEL_AVATAR_CACHE_LIMIT (5ULL * 1024ULL * 1024ULL)
#define CHANNEL_INFO_TEXT_SIZE 24
#define CHANNEL_AVATAR_SIZE 152
static const char *SortNames[SORT_COUNT] = { "Relevance", "Views", "Newest" };
static const char *ChannelSortNames[CHANNEL_SORT_COUNT] = { "Latest", "Popular", "Oldest" };

static char         pendingQuery[SEARCH_QUERY_LEN];
static SearchResult pendingChannel;
static int          pendingIsChannel;
static int          pendingChannelCategory;
static struct {
   int valid;
   int selected;
   int scrollRow;
   SortOrder sort;
   char query[SEARCH_QUERY_LEN];
   char videoId[16];
   char playlistId[64];
} channelReturn;
static int restoreChannelReturnPending;
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
   int       channelCategory;
   int       gridChannelLayout;
   int       channelPillX[4], channelPillW[4], channelPillY[4], channelPillH[4];
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

// Keep one first-page snapshot for each channel tab. Category switching can then restore the same
// result set immediately instead of waiting for another YouTube browse request. R2 bypasses this
// cache; entries expire after two minutes so this never becomes a permanent/offline feed.
#define CHANNEL_FEED_CACHE_TTL_US (120ull * 1000000ull)
typedef struct {
   volatile int valid;
   int category;
   int sort;
   char channelId[32];
   char region[8];
   uint64_t savedAtUs;
   SearchResults results;
} ChannelFeedCache;
static ChannelFeedCache channelFeedCache[4];

typedef struct {
   char channelId[32];
   HttpRequestGroup *requestGroup;
   SearchResults *results;
   int result;
} ChannelLiveFetchJob;

static Font        uiFont;
static Label       queryLabel, queryPromptPrefix, queryPromptSuffix;
static uint64_t    searchLoadingStartedUs;
static Label       sortLabel, sortLabels[SORT_COUNT], statusLabel, historyTitleLabel, historyEmptyLabel;
static Label       channelCategoryLabels[4];
static Label       historyLabels[MAX_SEARCH_HISTORY];
static Icon        searchIcon;
static GfxTexture  searchCrossGlyph;
static ButtonHints hints;
static TextTexture channelInfoTexture;
static GfxTexture channelAvatarTexture;
static Label channelAvatarNameLabel;
static char channelAvatarPath[192];
static int channelAvatarRequested;
static int channelAvatarLoaded;
static int channelAvatarDecodeAttempted;
static Label channelInfoPageLabel;
static int channelInfoVisible;
static int channelInfoPage;
static ChannelAboutInfo channelInfoData;
static ChannelAboutInfo channelInfoWorkerData;
static char channelInfoFallbackAvatarUrl[512];
static HttpRequestGroup channelInfoRequestGroup;
static sys_ppu_thread_t channelInfoWorkerTid;
static volatile int channelInfoWorkerDone;
static int channelInfoWorkerActive;
static int channelInfoWorkerResult;
static int channelInfoLoaded;
static char channelInfoWorkerChannelId[32];
static char channelInfoWorkerFallbackAvatarUrl[512];
static char channelInfoAttemptedChannelId[32];
static int crossHint = -1, squareHint = -1, triangleHint = -1;
static int selectHint = -1, downloadHint = -1, settingsHint = -1;
static int sortHint = -1, refreshHint = -1, clearHistoryHint = -1;
static int channelPrevHint = -1, channelNextHint = -1;
static SearchResult squarePendingItem;
static uint64_t squarePressedAt;
static uint64_t historySquarePressedAt;
static int historyClearConfirmOpen;
static int historySquareSelection;
static Label historyConfirmTitle, historyConfirmFooter;
static int playbackRestorePending;
static void restoreSearchPlaybackSelection(void);
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
   if (squareHint >= 0) {
      setButtonHintShown(&hints, squareHint, search.landing ? search.historySelection >= 0 : !search.fieldFocused);
      setButtonHintCaption(&hints, squareHint, search.landing ? "Delete / Clear all (hold)" : "Watch Later / Playlist (hold)");
   }
   if (triangleHint >= 0) setButtonHintShown(&hints, triangleHint, !search.landing && !search.fieldFocused);

   if (selectHint >= 0) setButtonHintShown(&hints, selectHint, !search.landing);
   if (downloadHint >= 0) setButtonHintShown(&hints, downloadHint, !search.landing && !search.fieldFocused);
   if (settingsHint >= 0) setButtonHintShown(&hints, settingsHint, !search.landing);
   if (sortHint >= 0) {
      setButtonHintShown(&hints, sortHint, !search.landing && (!search.channelId[0] || search.channelName[0]));
      setButtonHintCaption(&hints, sortHint, search.channelId[0] ? "Description" : "Sort");
   }
   if (channelPrevHint >= 0) setButtonHintShown(&hints, channelPrevHint, !search.landing && search.channelId[0]);
   if (channelNextHint >= 0) setButtonHintShown(&hints, channelNextHint, !search.landing && search.channelId[0]);
   if (refreshHint >= 0) setButtonHintShown(&hints, refreshHint, !search.landing);
   if (clearHistoryHint >= 0) setButtonHintShown(&hints, clearHistoryHint, hasHistory);
   if (search.landing) {
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

static void prependLiveChannelItems(SearchResults *live, SearchResults *regular, const char *channelId, const char *channelName)
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
      if (!item->channelId[0]) strCopy(item->channelId, sizeof item->channelId, channelId);
      if (!item->author[0])
         strCopy(item->author, sizeof item->author,
                 channelName[0] ? channelName : regular->channelName);
      if (liveCount != i) live->items[liveCount] = *item;
      liveCount++;
   }
   if (liveCount <= 0) return;
   memmove(regular->items + liveCount, regular->items, regular->count * sizeof(regular->items[0]));
   memcpy(regular->items, live->items, liveCount * sizeof(regular->items[0]));
   regular->count += liveCount;
}

static void channelLiveFetchWorker(uint64_t arg)
{
   ChannelLiveFetchJob *job = (ChannelLiveFetchJob *)(uintptr_t)arg;
   setHttpRequestGroup(job->requestGroup);
   job->result = getChannelLiveVideos(job->channelId, job->results);
   setHttpRequestGroup(NULL);
   exitThread();
}

static const SearchResults *findCachedChannelFeed(const char *channelId, int category, int sort)
{
   if (!channelId || !channelId[0] || category < 0 || category >= 4) return NULL;
   ChannelFeedCache *cache = &channelFeedCache[category];
   if (!cache->valid || cache->category != category || cache->sort != sort ||
       strcmp(cache->channelId, channelId) != 0 ||
       strcmp(cache->region, getContentRegionCode()) != 0) return NULL;
   uint64_t now = getTimeUs();
   if (now < cache->savedAtUs || now - cache->savedAtUs > CHANNEL_FEED_CACHE_TTL_US) {
      cache->valid = 0;
      return NULL;
   }
   return &cache->results;
}

static void cacheChannelFeed(const char *channelId, int category, int sort, const SearchResults *results)
{
   if (!channelId || !channelId[0] || !results || results->count <= 0 ||
       results->count > MAX_SEARCH_RESULTS || category < 0 || category >= 4 ||
       isHttpRequestGroupCancelled()) return;
   ChannelFeedCache *cache = &channelFeedCache[category];
   // Publish valid last: the UI never sees a partially copied SearchResults object.
   cache->valid = 0;
   __sync_synchronize();
   // Only the populated first page is copied. SearchResults has room for 500 entries, but a
   // channel's first response normally contains a few dozen; avoid a full-structure copy here.
   cache->results.count = results->count;
   strCopy(cache->results.continuation, sizeof cache->results.continuation, results->continuation);
   strCopy(cache->results.channelName, sizeof cache->results.channelName, results->channelName);
   strCopy(cache->results.channelDescription, sizeof cache->results.channelDescription, results->channelDescription);
   strCopy(cache->results.channelSubscribers, sizeof cache->results.channelSubscribers, results->channelSubscribers);
   strCopy(cache->results.channelVideoCount, sizeof cache->results.channelVideoCount, results->channelVideoCount);
   strCopy(cache->results.channelViewCount, sizeof cache->results.channelViewCount, results->channelViewCount);
   strCopy(cache->results.channelCountry, sizeof cache->results.channelCountry, results->channelCountry);
   strCopy(cache->results.channelVanityUrl, sizeof cache->results.channelVanityUrl, results->channelVanityUrl);
   strCopy(cache->results.channelJoinedDate, sizeof cache->results.channelJoinedDate, results->channelJoinedDate);
   strCopy(cache->results.channelAvatarUrl, sizeof cache->results.channelAvatarUrl, results->channelAvatarUrl);
   memcpy(cache->results.sortTokens, results->sortTokens, sizeof cache->results.sortTokens);
   memcpy(cache->results.items, results->items, (size_t)results->count * sizeof results->items[0]);
   strCopy(cache->channelId, sizeof cache->channelId, channelId);
   strCopy(cache->region, sizeof cache->region, getContentRegionCode());
   cache->category = category;
   cache->sort = sort;
   cache->savedAtUs = getTimeUs();
   __sync_synchronize();
   cache->valid = 1;
}

static int channelAvatarCachePath(const char *channelId, char *path, int capacity)
{
   if (!channelId || !channelId[0] || !path || capacity <= 0) return -1;
   int n = snprintf(path, capacity, CHANNEL_AVATAR_CACHE_DIR "/channel-%s.jpg", channelId);
   return n > 0 && n < capacity ? 0 : -1;
}

static int channelAvatarCacheValid(const char *path)
{
   VfsStat stat;
   return path && path[0] && statPath(path, &stat) == 0 && !stat.isDir && stat.size > 0;
}

// Keep the dedicated avatar cache under 5 MiB. When space is needed, remove
// the least-recently-written image first; the currently replaced channel is excluded.
static int makeChannelAvatarCacheRoom(uint64_t incomingBytes, const char *keepPath)
{
   if (incomingBytes > CHANNEL_AVATAR_CACHE_LIMIT) return 0;
   makeDir("/dev_hdd0/tmp");
   makeDir("/dev_hdd0/tmp/crtube");
   makeDir(CHANNEL_AVATAR_CACHE_DIR);
   for (;;) {
      VfsDir dir;
      char name[256], path[256], oldestPath[256];
      VfsEntryType type;
      uint64_t total = 0, oldestTime = UINT64_MAX;
      oldestPath[0] = 0;
      if (openDir(CHANNEL_AVATAR_CACHE_DIR, &dir) != 0) return 0;
      while (readDir(&dir, name, sizeof name, &type) > 0) {
         size_t length = strlen(name);
         if (type != VFS_ENTRY_FILE || length < 5 || strcmp(name + length - 4, ".jpg") != 0) continue;
         snprintf(path, sizeof path, "%s/%s", CHANNEL_AVATAR_CACHE_DIR, name);
         VfsStat stat;
         if (statPath(path, &stat) != 0 || stat.isDir) continue;
         if (keepPath && strcmp(path, keepPath) == 0) continue;
         total += stat.size;
         if (stat.mtime < oldestTime) {
            oldestTime = stat.mtime;
            strCopy(oldestPath, sizeof oldestPath, path);
         }
      }
      closeDir(&dir);
      if (total + incomingBytes <= CHANNEL_AVATAR_CACHE_LIMIT) return 1;
      if (!oldestPath[0] || deleteFile(oldestPath) != 0) return 0;
   }
}


static void migrateLegacyChannelAvatarCache(const char *channelId, const char *newPath)
{
   if (!channelId || !channelId[0] || !newPath || channelAvatarCacheValid(newPath)) return;
   char oldPath[160];
   int n = snprintf(oldPath, sizeof oldPath, "/dev_hdd0/tmp/crtube/channel-%s.jpg", channelId);
   if (n <= 0 || n >= (int)sizeof oldPath || !channelAvatarCacheValid(oldPath)) return;
   VfsStat stat;
   if (statPath(oldPath, &stat) == 0 && makeChannelAvatarCacheRoom((uint64_t)stat.size, newPath)) {
      if (renamePath(oldPath, newPath) != 0)
         logWarn("[search] could not migrate legacy channel avatar cache\n");
   }
}

static void fetchChannelAvatar(const char *url, const char *channelId)
{
   if (!url || !url[0] || !channelId || !channelId[0] || isHttpRequestGroupCancelled()) return;
   char path[192];
   if (channelAvatarCachePath(channelId, path, sizeof path) != 0 || channelAvatarCacheValid(path)) return;
   uint8_t *jpeg = (uint8_t *)malloc(CHANNEL_AVATAR_CAP);
   if (!jpeg) return;
   HttpHeader headers[] = { { "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120 Safari/537.36" },
                            { "Accept-Encoding", "identity" } };
   int length = 0, status = 0;
   int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)jpeg, CHANNEL_AVATAR_CAP, &length, &status);
   if (rc == 0 && status == 200 && length > 0 && length < CHANNEL_AVATAR_CAP - 1 &&
       !isHttpRequestGroupCancelled() && makeChannelAvatarCacheRoom((uint64_t)length, path)) {
      if (writeFile(path, (const char *)jpeg, (uint64_t)length) != 0)
         logWarn("[search] could not cache channel avatar\n");
   } else if (rc < 0 || status != 200) {
      logWarn("[search] channel avatar fetch failed rc=%d status=%d\n", rc, status);
   }
   free(jpeg);
}

static void copyChannelAboutFields(ChannelAboutInfo *destination, const SearchResults *source)
{
   if (!destination || !source) return;
   if (source->channelName[0]) strCopy(destination->channelName, sizeof destination->channelName, source->channelName);
   if (source->channelDescription[0]) strCopy(destination->channelDescription, sizeof destination->channelDescription, source->channelDescription);
   if (source->channelSubscribers[0]) strCopy(destination->channelSubscribers, sizeof destination->channelSubscribers, source->channelSubscribers);
   if (source->channelVideoCount[0]) strCopy(destination->channelVideoCount, sizeof destination->channelVideoCount, source->channelVideoCount);
   if (source->channelViewCount[0]) strCopy(destination->channelViewCount, sizeof destination->channelViewCount, source->channelViewCount);
   if (source->channelCountry[0]) strCopy(destination->channelCountry, sizeof destination->channelCountry, source->channelCountry);
   if (source->channelJoinedDate[0]) strCopy(destination->channelJoinedDate, sizeof destination->channelJoinedDate, source->channelJoinedDate);
   if (source->channelAvatarUrl[0]) strCopy(destination->channelAvatarUrl, sizeof destination->channelAvatarUrl, source->channelAvatarUrl);
}

static void channelInfoFetchWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&channelInfoRequestGroup);
   channelInfoWorkerResult = getChannelAboutInfo(channelInfoWorkerChannelId, &channelInfoWorkerData);
   if (!channelInfoWorkerData.channelAvatarUrl[0])
      strCopy(channelInfoWorkerData.channelAvatarUrl, sizeof channelInfoWorkerData.channelAvatarUrl,
              channelInfoWorkerFallbackAvatarUrl);
   if (channelInfoWorkerData.channelAvatarUrl[0])
      fetchChannelAvatar(channelInfoWorkerData.channelAvatarUrl, channelInfoWorkerChannelId);
   setHttpRequestGroup(NULL);
   __sync_synchronize();
   channelInfoWorkerDone = 1;
   exitThread();
}

static void startChannelInfoWorker(void)
{
   if (channelInfoWorkerActive || !search.channelId[0] ||
       strcmp(channelInfoAttemptedChannelId, search.channelId) == 0) return;
   strCopy(channelInfoWorkerChannelId, sizeof channelInfoWorkerChannelId, search.channelId);
   strCopy(channelInfoWorkerFallbackAvatarUrl, sizeof channelInfoWorkerFallbackAvatarUrl,
           channelInfoFallbackAvatarUrl);
   strCopy(channelInfoAttemptedChannelId, sizeof channelInfoAttemptedChannelId, search.channelId);
   memset(&channelInfoWorkerData, 0, sizeof channelInfoWorkerData);
   channelInfoRequestGroup.cancelled = 0;
   channelInfoWorkerDone = 0;
   channelInfoWorkerResult = -1;
   channelInfoWorkerActive = spawnJoinableThread(&channelInfoWorkerTid, channelInfoFetchWorker, 0,
                              THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-channel-about") == 0;
}

/* Channel info is laid out against the visible screen bounds. Display-target
 * drawing applies UI scaling around the screen center, so convert the desired
 * visible panel rectangle back to logical coordinates before drawing it. */
static void getChannelInfoPanelRect(int *x, int *y, int *w, int *h)
{
   float scale = search.uiScale > 1.0f ? search.uiScale : 1.0f;
   int visibleW = search.screenW - 2 * CHANNEL_INFO_MARGIN;
   int visibleH = search.screenH - 2 * CHANNEL_INFO_MARGIN;
   int visibleX = CHANNEL_INFO_MARGIN;
   int visibleY = CHANNEL_INFO_MARGIN;
   float centerX = search.screenW * 0.5f;
   float centerY = search.screenH * 0.5f;

   *x = (int)(centerX + ((float)visibleX - centerX) / scale + 0.5f);
   *y = (int)(centerY + ((float)visibleY - centerY) / scale + 0.5f);
   *w = (int)(visibleW / scale + 0.5f);
   *h = (int)(visibleH / scale + 0.5f);
}

static void getChannelInfoTextRect(int *x, int *y, int *w, int *h)
{
   int panelX, panelY, panelW, panelH;
   getChannelInfoPanelRect(&panelX, &panelY, &panelW, &panelH);

   *x = panelX + CHANNEL_INFO_PANEL_PAD;
   *y = panelY + CHANNEL_INFO_PANEL_PAD + CHANNEL_AVATAR_SIZE + 16;
   *w = panelW - 2 * CHANNEL_INFO_PANEL_PAD;
   *h = panelH - (*y - panelY) - CHANNEL_INFO_PANEL_PAD;
   if (*w < 1) *w = 1;
   if (*h < 1) *h = 1;
}

static int channelInfoVisibleCoordX(int logicalX)
{
   float scale = search.uiScale > 1.0f ? search.uiScale : 1.0f;
   return (int)(search.screenW * 0.5f +
                (logicalX - search.screenW * 0.5f) * scale + 0.5f);
}

static int channelInfoVisibleCoordY(int logicalY)
{
   float scale = search.uiScale > 1.0f ? search.uiScale : 1.0f;
   return (int)(search.screenH * 0.5f +
                (logicalY - search.screenH * 0.5f) * scale + 0.5f);
}

static int appendChannelInfoRow(char *content, int capacity, int *used,
                                 const char *label, const char *value)
{
   if (!value || !value[0] || *used >= capacity - 1) return 0;
   int n = snprintf(content + *used, capacity - *used, "%s: %s\n\n", label, value);
   if (n > 0) *used += n < capacity - *used ? n : capacity - *used - 1;
   return n > 0;
}

static void renderChannelInfoText(void)
{
   char content[6144];
   const char *description = channelInfoData.channelDescription;
   if (!description[0])
      description = channelInfoWorkerActive ? "Loading channel information..." : "No description available.";
   int used = snprintf(content, sizeof content, "DESCRIPTION\n\n%s\n\n\nMORE INFORMATION\n\n",
                       description);
   if (used < 0) used = 0;
   if (used >= (int)sizeof content) used = sizeof content - 1;
   int infoRows = 0;
   infoRows += appendChannelInfoRow(content, sizeof content, &used, "Subscribers", channelInfoData.channelSubscribers);
   infoRows += appendChannelInfoRow(content, sizeof content, &used, "Videos", channelInfoData.channelVideoCount);
   infoRows += appendChannelInfoRow(content, sizeof content, &used, "Views", channelInfoData.channelViewCount);
   infoRows += appendChannelInfoRow(content, sizeof content, &used, "Country", channelInfoData.channelCountry);
   infoRows += appendChannelInfoRow(content, sizeof content, &used, "Joined", channelInfoData.channelJoinedDate);
   if (infoRows == 0 && used < (int)sizeof content - 1)
      snprintf(content + used, sizeof content - used, "No additional information available.");
   if (channelInfoTexture.valid) freeTextTexture(&channelInfoTexture);
   // Use the inverse-scaled logical content width. The display transform then
   // produces the intended visible width at both UI scales and aspect ratios.
   int textX, textY, textW, textH;
   getChannelInfoTextRect(&textX, &textY, &textW, &textH);
   (void)textX; (void)textY; (void)textH;
   renderFontRaw(&channelInfoTexture, &uiFont, CHANNEL_INFO_TEXT_SIZE, content,
                 activeTheme->textPrimary, textW, TEXT_WRAP);
}

static int searchFetch(const char *token, SearchResults *out, void *user)
{
   int rc;
   HttpRequestGroup *requestGroup = (HttpRequestGroup *)user;
   char channelId[sizeof search.channelId];
   char channelName[sizeof search.channelName];
   char sortToken[sizeof search.channelSortToken];
   char query[sizeof search.query];
   int category = search.channelCategory;
   int channelSort = search.channelSort;
   SortOrder sort = search.sort;
   strCopy(channelId, sizeof channelId, search.channelId);
   strCopy(channelName, sizeof channelName, search.channelName);
   strCopy(sortToken, sizeof sortToken, search.channelSortToken);
   strCopy(query, sizeof query, search.query);

   if (channelId[0]) {
      if (category != 0) {
         rc = getChannelCategoryFeed(channelId, category, token, out);
      } else {
         const char *first = sortToken[0] ? sortToken : NULL;
         SearchResults *live = NULL;
         ChannelLiveFetchJob liveJob;
         sys_ppu_thread_t liveTid;
         int liveStarted = 0;
         memset(&liveJob, 0, sizeof liveJob);
         if (!token) {
            live = malloc(sizeof(*live));
            if (live) {
               strCopy(liveJob.channelId, sizeof liveJob.channelId, channelId);
               liveJob.requestGroup = requestGroup;
               liveJob.results = live;
               liveJob.result = -1;
               // The Videos tab also promotes currently-live broadcasts. Fetch that supplementary
               // feed concurrently with Videos rather than serially adding its latency to the page.
               liveStarted = spawnJoinableThread(&liveTid, channelLiveFetchWorker,
                               (uint64_t)(uintptr_t)&liveJob, THREAD_PRIORITY_DEFAULT,
                               THREAD_STACK_SIZE_64KB, "yt-channel-live") == 0;
            }
         }
         rc = getChannelVideos(channelId, token ? token : first, out);
         if (live) {
            if (liveStarted) joinThread(liveTid);
            else if (!token && !isHttpRequestGroupCancelled())
               liveJob.result = getChannelLiveVideos(channelId, live);
            if (rc == 0 && liveJob.result == 0 && live->count > 0)
               prependLiveChannelItems(live, out, channelId, channelName);
            free(live);
         }
      }
      if (rc == 0) {
         for (int i = 0; i < out->count; i++) {
            if (!out->items[i].channelId[0])
               strCopy(out->items[i].channelId, sizeof out->items[i].channelId, channelId);
            if (!out->items[i].author[0] && channelName[0])
               strCopy(out->items[i].author, sizeof out->items[i].author, channelName);
         }
         if (!token) cacheChannelFeed(channelId, category, channelSort, out);
      }
   } else {
      rc = searchVideos(query, sort, token, out);
   }
   return isHttpRequestGroupCancelled() ? -1 : rc;
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

static void updateChannelGridLayout(void)
{
   int desired = search.channelId[0] != 0;
   if (desired == search.gridChannelLayout) return;
   stopVideoGrid(&search.grid);
   termVideoGrid(&search.grid);
   int homeGridTop = isGfxSdVideoOutput() ? 132 : 100;
   search.gridTop = homeGridTop;
   int gridBottomGap = isGfxSdVideoOutput() ? 20 : GRID_BOTTOM_GAP;
   initVideoGrid(&search.grid, &uiFont, MARGIN_X, search.gridTop,
                 search.screenW - 2 * MARGIN_X,
                 search.footerTopY - search.gridTop - gridBottomGap);
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchedPosition(&search.grid, getWatchedPosition);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   setGridPlaylistWatchLaterPredicate(&search.grid, isPlaylistWatchLater);
   search.gridChannelLayout = desired;
}

static void reload(int allowChannelCache)
{
   updateChannelGridLayout();
   search.landing = 0;
   search.fieldFocused = 0;
   search.resumeGridInput = 0;
   setGridInputSuppressed(&search.grid, 0);
   setGridFocusHidden(&search.grid, 0);
   setGridSortByPublished(&search.grid, !search.channelId[0] && search.sort == SORT_NEWEST);
   setHeading();
   if (!allowChannelCache && search.channelId[0]) {
      // An explicit refresh supersedes the previous snapshot. Invalidating before starting the worker
      // prevents a rapid category round-trip from reading the cache while this same slot is rewritten.
      channelFeedCache[search.channelCategory].valid = 0;
      __sync_synchronize();
   }
   const SearchResults *cached = allowChannelCache && search.channelId[0]
      ? findCachedChannelFeed(search.channelId, search.channelCategory, search.channelSort) : NULL;
   if (cached) setGridCached(&search.grid, cached, searchFetch, &search.grid.requestGroup);
   else setGridSource(&search.grid, searchFetch, &search.grid.requestGroup);
}

static void enterChannel(const SearchResult *item)
{
   if (!item || !item->channelId[0]) return;
   // Keep the originating query and cursor so Circle from the channel returns
   // to the same result position instead of closing Search.
   channelReturn.valid = 1;
   restoreChannelReturnPending = 0;
   channelReturn.selected = search.grid.selected;
   channelReturn.scrollRow = search.grid.scrollRow;
   channelReturn.sort = search.sort;
   strCopy(channelReturn.query, sizeof channelReturn.query, search.query);
   strCopy(channelReturn.videoId, sizeof channelReturn.videoId, item->videoId);
   strCopy(channelReturn.playlistId, sizeof channelReturn.playlistId, item->playlistId);
   strCopy(search.channelId, sizeof search.channelId, item->channelId);
   strCopy(search.channelName, sizeof search.channelName,
           item->author[0] ? item->author : "Channel");
   cancelImagePriorityAsync();
   if (channelAvatarTexture.offset) freeGfxTexture(&channelAvatarTexture);
   channelAvatarTexture.offset = 0;
   if (channelInfoTexture.valid) freeTextTexture(&channelInfoTexture);
   memset(&channelInfoData, 0, sizeof channelInfoData);
   if (!channelInfoWorkerActive) memset(&channelInfoWorkerData, 0, sizeof channelInfoWorkerData);
   strCopy(channelInfoData.channelName, sizeof channelInfoData.channelName, search.channelName);
   channelInfoFallbackAvatarUrl[0] = 0;
   if (!channelInfoWorkerActive) {
      channelInfoWorkerChannelId[0] = 0;
      channelInfoWorkerFallbackAvatarUrl[0] = 0;
   }
   channelInfoAttemptedChannelId[0] = 0;
   channelInfoLoaded = 0;
   channelAvatarRequested = 0;
   channelAvatarLoaded = 0;
   channelAvatarDecodeAttempted = 0;
   channelAvatarCachePath(search.channelId, channelAvatarPath, sizeof channelAvatarPath);
   migrateLegacyChannelAvatarCache(search.channelId, channelAvatarPath);
   search.channelSort = CHANNEL_SORT_LATEST;
   search.channelCategory = 0;
   search.channelSortToken[0] = 0;
   reload(1);
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
   reload(0);
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
   /* Keep the search header inside the same vertical band as navigation titles.
    * The previous 56px scaled field pushed the grid down at larger UI scales. */
   search.fieldHeight = (int)(40 * search.uiScale + 0.5f);
   int homeGridTop = isGfxSdVideoOutput() ? 132 : 100;
   search.gridTop = homeGridTop;
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
   historySquarePressedAt = 0;
   channelInfoVisible = 0;
   channelInfoPage = 0;
   if (channelInfoTexture.valid) freeTextTexture(&channelInfoTexture);
   if (channelAvatarTexture.offset) freeGfxTexture(&channelAvatarTexture);
   channelAvatarRequested = 0;
   channelAvatarLoaded = 0;
   channelAvatarDecodeAttempted = 0;
   channelAvatarPath[0] = 0;
   memset(&channelInfoData, 0, sizeof channelInfoData);
   memset(&channelInfoWorkerData, 0, sizeof channelInfoWorkerData);
   channelInfoFallbackAvatarUrl[0] = 0;
   channelInfoWorkerChannelId[0] = 0;
   channelInfoWorkerFallbackAvatarUrl[0] = 0;
   channelInfoAttemptedChannelId[0] = 0;
   channelInfoWorkerDone = 0;
   channelInfoWorkerActive = 0;
   channelInfoLoaded = 0;
   historyClearConfirmOpen = 0;
   search.landing = pendingLanding && !pendingIsChannel;
   search.fieldFocused = 0;
   search.resumeGridInput = 0;
   search.historySelection = -1;
   search.historyScroll = 0;

   if (pendingIsChannel) {
      strCopy(search.channelId, sizeof search.channelId, pendingChannel.channelId);
      strCopy(search.channelName, sizeof search.channelName,
              pendingChannel.author[0] ? pendingChannel.author : "Channel");
      channelAvatarCachePath(search.channelId, channelAvatarPath, sizeof channelAvatarPath);
      migrateLegacyChannelAvatarCache(search.channelId, channelAvatarPath);
      strCopy(channelInfoData.channelName, sizeof channelInfoData.channelName, search.channelName);
      search.channelSort = CHANNEL_SORT_LATEST;
      search.channelCategory = pendingChannelCategory;
      if (search.channelCategory < 0 || search.channelCategory >= 4) search.channelCategory = 0;
      search.channelSortToken[0] = 0;
   } else {
      strCopy(search.query, sizeof search.query, pendingQuery);
   }

   uiFont = openSystemFont(FONT_POP);
   initLabel(&channelInfoPageLabel, &uiFont, 0, 0, 100, AUTO, 18, activeTheme->textSecondary, TEXT_NOWRAP, "1/1");
   setLabelScaleWithUi(&channelInfoPageLabel, 1);
   int panelX, panelY, panelW, panelH;
   getChannelInfoPanelRect(&panelX, &panelY, &panelW, &panelH);
   (void)panelX; (void)panelY; (void)panelH;
   int channelNameWidth = panelW - 2 * CHANNEL_INFO_PANEL_PAD - CHANNEL_AVATAR_SIZE - 20;
   if (channelNameWidth < 1) channelNameWidth = 1;
   initLabel(&channelAvatarNameLabel, &uiFont, 0, 0, channelNameWidth, AUTO, 26,
             activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
             search.channelName[0] ? search.channelName : "Channel");
   setLabelScaleWithUi(&channelAvatarNameLabel, 1);
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
   initLabelRaw(&queryPromptPrefix, &uiFont, 0, 0, search.fieldWidth, FIELD_H - 10,
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
   static const char *const channelCategoryNames[4] = { "Videos", "LIVE", "Shorts", "Playlists" };
   int channelCategoryGap = (int)(6 * search.uiScale + 0.5f);
   int channelCategoryPad = (int)(18 * search.uiScale + 0.5f);
   int channelColW[2] = { 0, 0 };
   for (int i = 0; i < 4; i++) {
      initLabel(&channelCategoryLabels[i], &uiFont, 0, 0, AUTO, AUTO, 18,
                activeTheme->textSecondary, TEXT_NOWRAP, channelCategoryNames[i]);
      setLabelScaleWithUi(&channelCategoryLabels[i], 1);
      int col = i & 1;
      int width = channelCategoryLabels[i].tt.tex.w + channelCategoryPad;
      if (width > channelColW[col]) channelColW[col] = width;
   }
   int channelGroupW = channelColW[0] + channelColW[1] + channelCategoryGap;
   int channelGroupX = search.screenW - MARGIN_X - channelGroupW;
   int channelPillGapY = (int)(4 * search.uiScale + 0.5f);
   int channelPillH = (search.fieldHeight - channelPillGapY) / 2;
   for (int i = 0; i < 4; i++) {
      int col = i & 1;
      int row = i >> 1;
      search.channelPillX[i] = channelGroupX + (col ? channelColW[0] + channelCategoryGap : 0);
      search.channelPillW[i] = channelColW[col];
      search.channelPillY[i] = HEADER_Y + row * (channelPillH + channelPillGapY);
      search.channelPillH[i] = channelPillH;
   }
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
   int historyConfirmWidth = (int)((search.screenW * 0.90f - 40) / search.uiScale);
   if (historyConfirmWidth > 620) historyConfirmWidth = 620;
   if (historyConfirmWidth < 240) historyConfirmWidth = 240;
   initLabel(&historyConfirmTitle, &uiFont, 0, 0, historyConfirmWidth, AUTO, 27,
             activeTheme->textPrimary, TEXT_NOWRAP, "Delete all search history?");
   initLabel(&historyConfirmFooter, &uiFont, 0, 0, historyConfirmWidth, AUTO, 19,
             activeTheme->textSecondary, TEXT_NOWRAP, "X Confirm       O Cancel");
   setLabelScaleWithUi(&historyConfirmTitle, 1);
   setLabelScaleWithUi(&historyConfirmFooter, 1);
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
   channelPrevHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L1), "");
   channelNextHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R1), "Category");
   refreshHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R2), "Refresh");
   clearHistoryHint = -1;
   setButtonHintsStableLayout(&hints, 1);
   // Measure the longest dynamic captions so later mode changes cannot need an unreserved row.
   setButtonHintCaption(&hints, crossHint, "Open playlist");
   setButtonHintCaption(&hints, squareHint, "Watch Later / Playlist (hold)");
   setButtonHintCaption(&hints, triangleHint, "Unsubscribe");
   search.footerRows = getButtonHintsFitRows(&hints, search.screenW, MAX_HINT_ROWS);
   setButtonHintCaption(&hints, squareHint, "Watch Later");
   if (search.footerRows < 1) search.footerRows = 1;
   float hintGlyphFinal = HINT_GLYPH_H * (isGfxSdVideoOutput() ? 1.5f : search.uiScale);
   int footerRowStep = (int)((hintGlyphFinal + HINT_ROW_GAP) * hints.fitScale + 0.5f);
   search.footerTopY = search.footerBottomY - (search.footerRows - 1) * footerRowStep;

   search.historyVisibleRows = (search.footerTopY - search.historyTop - 12) /
                               (search.historyRowHeight + search.historyRowGap);
   if (search.historyVisibleRows < 1) search.historyVisibleRows = 1;
   if (search.historyVisibleRows > MAX_SEARCH_HISTORY) search.historyVisibleRows = MAX_SEARCH_HISTORY;
   reloadHistory();

   int gridBottomGap = isGfxSdVideoOutput() ? 20 : GRID_BOTTOM_GAP;
   initVideoGrid(&search.grid, &uiFont, MARGIN_X, search.gridTop,
                 search.screenW - 2 * MARGIN_X,
                 search.footerTopY - search.gridTop - gridBottomGap);
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchedPosition(&search.grid, getWatchedPosition);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   setGridPlaylistWatchLaterPredicate(&search.grid, isPlaylistWatchLater);
   setGridInputSuppressed(&search.grid, search.landing);
   setGridFocusHidden(&search.grid, search.landing);
   setSidebarCurrent(SIDEBAR_SEARCH);
   setHeading();
   if (!search.landing) reload(1);
}

static void updateSearch(void)
{
   updateDownloadOverlay();
   if (isDownloadSpacePromptOpen() || oskInputActive()) return;
   if (channelInfoWorkerActive && channelInfoWorkerDone) {
      joinThread(channelInfoWorkerTid);
      channelInfoWorkerActive = 0;
      if (channelInfoWorkerResult == 0 && strcmp(channelInfoWorkerChannelId, search.channelId) == 0) {
         if (channelInfoWorkerData.channelName[0]) strCopy(channelInfoData.channelName, sizeof channelInfoData.channelName, channelInfoWorkerData.channelName);
         if (channelInfoWorkerData.channelDescription[0]) strCopy(channelInfoData.channelDescription, sizeof channelInfoData.channelDescription, channelInfoWorkerData.channelDescription);
         if (channelInfoWorkerData.channelSubscribers[0]) strCopy(channelInfoData.channelSubscribers, sizeof channelInfoData.channelSubscribers, channelInfoWorkerData.channelSubscribers);
         if (channelInfoWorkerData.channelVideoCount[0]) strCopy(channelInfoData.channelVideoCount, sizeof channelInfoData.channelVideoCount, channelInfoWorkerData.channelVideoCount);
         if (channelInfoWorkerData.channelViewCount[0]) strCopy(channelInfoData.channelViewCount, sizeof channelInfoData.channelViewCount, channelInfoWorkerData.channelViewCount);
         if (channelInfoWorkerData.channelCountry[0]) strCopy(channelInfoData.channelCountry, sizeof channelInfoData.channelCountry, channelInfoWorkerData.channelCountry);
         if (channelInfoWorkerData.channelJoinedDate[0]) strCopy(channelInfoData.channelJoinedDate, sizeof channelInfoData.channelJoinedDate, channelInfoWorkerData.channelJoinedDate);
         if (channelInfoWorkerData.channelAvatarUrl[0]) strCopy(channelInfoData.channelAvatarUrl, sizeof channelInfoData.channelAvatarUrl, channelInfoWorkerData.channelAvatarUrl);
         channelInfoLoaded = 1;
         renderChannelInfoText();
      } else if (strcmp(channelInfoWorkerChannelId, search.channelId) == 0) {
         channelInfoLoaded = 1;
      }
   }
   if (channelInfoVisible) {
      if (!channelInfoLoaded && !channelInfoWorkerActive) startChannelInfoWorker();
      if (!channelAvatarDecodeAttempted && !channelAvatarRequested && !channelAvatarLoaded &&
          channelAvatarPath[0] && channelAvatarCacheValid(channelAvatarPath)) {
         channelAvatarDecodeAttempted = 1;
         if (requestImagePriorityAsync(channelAvatarPath) == 0) channelAvatarRequested = 1;
      }
      if (channelAvatarRequested && !channelAvatarLoaded) {
         ImageBuffer decoded = { 0, 0, 0, 0 };
         int imageResult = pollImagePriorityAsync(&decoded);
         if (imageResult > 0) {
            channelAvatarTexture = uploadImageBuffer(&decoded);
            freeImageBuffer(&decoded);
            channelAvatarLoaded = channelAvatarTexture.offset != 0;
            channelAvatarRequested = 0;
         } else if (imageResult < 0) channelAvatarRequested = 0;
      }
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_L2)) {
         channelInfoVisible = 0;
         return;
      }
      int textX, textY, textW, viewH;
      getChannelInfoTextRect(&textX, &textY, &textW, &viewH);
      (void)textX; (void)textY; (void)textW;
      int pageCount = channelInfoTexture.tex.h > 0 ? (channelInfoTexture.tex.h + viewH - 1) / viewH : 1;
      if (isPadButtonPressed(PAD_BTN_DOWN) || isPadButtonPressed(PAD_BTN_RIGHT)) {
         if (channelInfoPage + 1 < pageCount) channelInfoPage++;
      } else if (isPadButtonPressed(PAD_BTN_UP) || isPadButtonPressed(PAD_BTN_LEFT)) {
         if (channelInfoPage > 0) channelInfoPage--;
      }
      return;
   }
   if (historyClearConfirmOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { historyClearConfirmOpen = 0; return; }
      if (isPadButtonPressed(PAD_BTN_CROSS)) { clearHistory(); historyClearConfirmOpen = 0; return; }
      return;
   }

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
         if (picked == SIDEBAR_SETTINGS) {
            openHomeSettingsPanelFromSearch();
         } else {
            popScreen();
            handleHomeSidebarSelection(picked);
         }
      }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START)) {
      openHomeSettingsPanelFromSearch();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      if (search.channelId[0] && channelReturn.valid) {
         strCopy(search.query, sizeof search.query, channelReturn.query);
         search.sort = channelReturn.sort;
         search.channelId[0] = 0;
         search.channelName[0] = 0;
         search.channelSortToken[0] = 0;
         restoreChannelReturnPending = 1;
         reload(0);
         return;
      }
      popScreen();
      return;
   }
   if (search.landing && search.historySelection >= 0 && search.historyCount > 0) {
      if (!historySquarePressedAt && isPadButtonPressed(PAD_BTN_SQUARE)) {
         historySquareSelection = search.historySelection;
         historySquarePressedAt = getTimeUs();
         return;
      }
      if (historySquarePressedAt) {
         if (isPadButtonDown(PAD_BTN_SQUARE) &&
             getTimeUs() - historySquarePressedAt >= SQUARE_HOLD_US) {
            historySquarePressedAt = 0;
            historyClearConfirmOpen = 1;
            return;
         }
         if (isPadButtonReleased(PAD_BTN_SQUARE)) {
            uint64_t held = getTimeUs() - historySquarePressedAt;
            historySquarePressedAt = 0;
            if (held < SQUARE_HOLD_US && historySquareSelection >= 0 &&
                historySquareSelection < search.historyCount) {
               removeSearchHistoryAt(historySquareSelection);
               search.historySelection = historySquareSelection;
               reloadHistory();
               if (search.historySelection >= search.historyCount)
                  search.historySelection = search.historyCount - 1;
               updateSearchHints();
            }
            return;
         }
         return;
      }
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
   if (isPadButtonPressed(PAD_BTN_R2)) { reload(0); return; }
   if (search.channelId[0] && isPadButtonPressed(PAD_BTN_R1)) {
      search.channelCategory = (search.channelCategory + 1) % 4;
      reload(1);
      return;
   }
   if (search.channelId[0] && isPadButtonPressed(PAD_BTN_L1)) {
      search.channelCategory = (search.channelCategory + 3) % 4;
      reload(1);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_L2)) {
      if (search.channelId[0]) {
         const SearchResults *results = gridResults(&search.grid);
         if (results) {
            memset(&channelInfoData, 0, sizeof channelInfoData);
            strCopy(channelInfoData.channelName, sizeof channelInfoData.channelName, search.channelName);
            copyChannelAboutFields(&channelInfoData, results);
            strCopy(channelInfoFallbackAvatarUrl, sizeof channelInfoFallbackAvatarUrl, results->channelAvatarUrl);
            channelInfoVisible = 1;
            channelInfoPage = 0;
            startChannelInfoWorker();
            renderChannelInfoText();
         }
      } else {
         search.sort = (SortOrder)((search.sort + 1) % SORT_COUNT);
         setSortPreference(SORT_PREF_SEARCH, (int)search.sort);
         reload(0);
      }
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

   if (restoreChannelReturnPending && gridStage(&search.grid) == GRID_READY) {
      int selected = channelReturn.selected;
      if (channelReturn.videoId[0] || channelReturn.playlistId[0]) {
         for (int i = 0; i < search.grid.results.count; i++) {
            const SearchResult *candidate = &search.grid.results.items[i];
            if ((channelReturn.videoId[0] && strcmp(candidate->videoId, channelReturn.videoId) == 0) ||
                (channelReturn.playlistId[0] && strcmp(candidate->playlistId, channelReturn.playlistId) == 0)) {
               selected = i;
               break;
            }
         }
      }
      if (selected < 0) selected = 0;
      if (selected >= search.grid.results.count) selected = search.grid.results.count - 1;
      if (selected < 0) selected = 0;
      search.grid.selected = selected;
      search.grid.scrollRow = channelReturn.scrollRow;
      int columns = search.grid.columns > 0 ? search.grid.columns : 1;
      int totalRows = (search.grid.results.count + columns - 1) / columns;
      int maxScroll = totalRows - search.grid.visibleRows;
      if (maxScroll < 0) maxScroll = 0;
      if (search.grid.scrollRow < 0) search.grid.scrollRow = 0;
      if (search.grid.scrollRow > maxScroll) search.grid.scrollRow = maxScroll;
      restoreChannelReturnPending = 0;
      channelReturn.valid = 0;
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
      if (downloadHint >= 0) { setButtonHintShown(&hints, downloadHint, 1); setButtonHintCaption(&hints, downloadHint, "Download playlist"); }
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
   if (isPadButtonPressed(PAD_BTN_R3)) {
      if (selected->isPlaylist) {
         SearchResult playlist = *selected;
         openHomeDownloadPlaylistConfirmation(&playlist);
      } else if (!selected->isLive) {
         char downloadedPath[MAX_PATH_LEN];
         if (getDownloadedVideoPath(selected->videoId, downloadedPath, sizeof downloadedPath) != 0)
            enqueueDownload(selected);
      }
      return;
   }
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
         int fromChannel = search.channelId[0] != 0;
         char returnChannelId[32], returnChannelName[48];
         strCopy(returnChannelId, sizeof returnChannelId, search.channelId);
         strCopy(returnChannelName, sizeof returnChannelName, search.channelName);
         int returnChannelCategory = search.channelCategory;
         popScreen();
         openHomePlaylistFromSearch(playlistId, playlistTitle, search.query, (int)search.sort,
                                    fromChannel ? returnChannelId : "",
                                    fromChannel ? returnChannelName : "",
                                    returnChannelCategory);
         return;
      }
      if (selected->videoId[0]) {
         markWatched(selected->videoId);
         stopVideoGrid(&search.grid);
         playVideo(selected->videoId);
      }
   }
   restoreSearchPlaybackSelection();
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
   if (search.channelId[0]) {
      int categoryGap = (int)(8 * search.uiScale + 0.5f);
      int channelFieldW = search.channelPillX[0] - fieldX - categoryGap;
      if (channelFieldW > fieldW) fieldW = channelFieldW;
   }
   int fieldFocused = search.landing ? search.historySelection < 0 : search.fieldFocused;
   int border = (int)(search.uiScale + 0.5f);
   if (border < 1) border = 1;
   int radius = fieldH / 2;
   uint32_t borderColor = fieldFocused ? activeTheme->accent : activeTheme->rowHighlight;

   fillGfxRoundedRectangleFixed(fieldX, fieldY, fieldW, fieldH, radius, borderColor);
   fillGfxRoundedRectangleFixed(fieldX + border, fieldY + border,
                                fieldW - border * 2, fieldH - border * 2,
                                radius - border, getSearchFieldColor());

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
   if (search.channelId[0] || search.query[0]) {
      refreshLabelScale(&queryLabel);
      moveLabel(&queryLabel, fieldX + (int)(58 * search.uiScale + 0.5f),
                fieldY + (fieldH - queryLabel.tt.tex.h) / 2);
      drawLabel(&queryLabel);
   } else {
      refreshLabelScale(&queryPromptPrefix);
      refreshLabelScale(&queryPromptSuffix);
      int promptX = fieldX + (int)(58 * search.uiScale + 0.5f);
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

static void drawChannelCategoryPills(void)
{
   if (!search.channelId[0]) return;
   int border = (int)(search.uiScale + 0.5f);
   if (border < 1) border = 1;
   for (int i = 0; i < 4; i++) {
      int selected = i == search.channelCategory;
      int x = search.channelPillX[i], y = search.channelPillY[i];
      int w = search.channelPillW[i], h = search.channelPillH[i];
      fillGfxRoundedRectangleFixed(x, y, w, h, h / 2,
                                   selected ? activeTheme->accent : activeTheme->surface);
      if (selected)
         fillGfxRoundedRectangleFixed(x + border, y + border, w - 2 * border, h - 2 * border,
                                      h / 2 - border, activeTheme->rowHighlight);
      refreshLabelScale(&channelCategoryLabels[i]);
      setLabelColor(&channelCategoryLabels[i], selected ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(&channelCategoryLabels[i], x + (w - channelCategoryLabels[i].tt.tex.w) / 2,
                y + (h - channelCategoryLabels[i].tt.tex.h) / 2);
      drawLabel(&channelCategoryLabels[i]);
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
   drawChannelCategoryPills();

   if (search.landing) {
      drawSearchSortPills();
      drawHistory();
   } else {
      if (!search.channelId[0]) drawSearchSortPills();
      if (gridStage(&search.grid) == GRID_READY) {
         searchLoadingStartedUs = 0;
         drawVideoGrid(&search.grid);
      } else {
         GridStage stage = gridStage(&search.grid);
         if (stage == GRID_EMPTY && search.channelId[0]) {
            static const char *const emptyCategoryText[4] = {
               "No videos available in this channel", "No LIVE streams available in this channel",
               "No Shorts available in this channel", "No playlists available in this channel"
            };
            setLabelText(&statusLabel, emptyCategoryText[search.channelCategory]);
            refreshLabelScale(&statusLabel);
            int gridAreaH = search.footerTopY - search.gridTop - GRID_BOTTOM_GAP;
            int x = (search.screenW - statusLabel.tt.tex.w) / 2;
            int y = search.gridTop + (gridAreaH - statusLabel.tt.tex.h) / 2;
            moveLabel(&statusLabel, x, y);
         } else {
            const char *message = stage == GRID_ERROR ?
               (search.channelId[0] ? "Unable to load this channel category" : "Unable to load results") :
               (stage == GRID_EMPTY ? "No results" : "Loading...");
            if (stage == GRID_RUNNING) {
               drawLoadingIndicator(search.screenW / 2, search.screenH / 2, &searchLoadingStartedUs);
            } else {
               searchLoadingStartedUs = 0;
               setLabelText(&statusLabel, message);
               refreshLabelScale(&statusLabel);
               moveLabel(&statusLabel, MARGIN_X, search.gridTop);
               drawLabel(&statusLabel);
            }
         }
      }
   }

   drawButtonHintsFitted(&hints, search.screenW, search.footerBottomY, HINT_ROW_GAP, MAX_HINT_ROWS);
   if (historyClearConfirmOpen) {
      int panelW = (int)(680 * search.uiScale), panelH = (int)(190 * search.uiScale);
      if (panelW > search.screenW - 40) panelW = search.screenW - 40;
      int x = (search.screenW - panelW) / 2, y = (search.screenH - panelH) / 2;
      fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->scrim);
      fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
      fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
      refreshLabelScale(&historyConfirmTitle);
      refreshLabelScale(&historyConfirmFooter);
      moveLabel(&historyConfirmTitle, search.screenW / 2 - historyConfirmTitle.tt.tex.w / 2,
                y + (int)(48 * search.uiScale));
      moveLabel(&historyConfirmFooter, search.screenW / 2 - historyConfirmFooter.tt.tex.w / 2,
                y + (int)(118 * search.uiScale));
      drawLabel(&historyConfirmTitle);
      drawLabel(&historyConfirmFooter);
   }
   if (channelInfoVisible) {
      fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->scrim);
      int panelX, panelY, panelW, panelH;
      getChannelInfoPanelRect(&panelX, &panelY, &panelW, &panelH);
      fillGfxRoundedRectangle(panelX, panelY, panelW, panelH, 14, activeTheme->appBg);
      fillGfxRectangle(panelX, panelY, panelW, 4, activeTheme->accent);

      const int avatarSize = CHANNEL_AVATAR_SIZE;
      int avatarX = panelX + CHANNEL_INFO_PANEL_PAD;
      int avatarY = panelY + CHANNEL_INFO_PANEL_PAD;
      if (channelAvatarLoaded) {
         drawGfxTexture(avatarX, avatarY, avatarSize, avatarSize, channelAvatarTexture,
                        0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, GFX_FILTER_LINEAR);
      } else {
         fillGfxRoundedRectangle(avatarX, avatarY, avatarSize, avatarSize, 8, activeTheme->surface);
      }

      setLabelText(&channelAvatarNameLabel, search.channelName[0] ? search.channelName : "Channel");
      setLabelColor(&channelAvatarNameLabel, activeTheme->textPrimary);
      moveLabel(&channelAvatarNameLabel,
                channelInfoVisibleCoordX(avatarX + avatarSize + 20),
                channelInfoVisibleCoordY(avatarY +
                   (avatarSize - channelAvatarNameLabel.tt.tex.h / (search.uiScale > 1.0f ? search.uiScale : 1.0f)) / 2));
      drawLabel(&channelAvatarNameLabel);

      int textX, textY, textW, viewH;
      getChannelInfoTextRect(&textX, &textY, &textW, &viewH);
      int pageCount = channelInfoTexture.tex.h > 0 ?
                      (channelInfoTexture.tex.h + viewH - 1) / viewH : 1;
      if (channelInfoPage >= pageCount) channelInfoPage = pageCount - 1;
      int pageTop = channelInfoPage * viewH;
      int visibleH = channelInfoTexture.tex.h - pageTop;
      if (visibleH > viewH) visibleH = viewH;
      if (visibleH > 0) {
         float top = (float)pageTop / channelInfoTexture.tex.h;
         float bottom = (float)(pageTop + visibleH) / channelInfoTexture.tex.h;
         int drawW = channelInfoTexture.tex.w;
         if (drawW > textW) drawW = textW;
         drawGfxTexture(textX, textY, drawW, visibleH,
                        channelInfoTexture.tex, 0.0f, top, 1.0f, bottom, 0xFFFFFFFF, GFX_FILTER_NEAREST);
      }

      char pageText[24];
      snprintf(pageText, sizeof pageText, "%d/%d", channelInfoPage + 1, pageCount);
      setLabelText(&channelInfoPageLabel, pageText);
      refreshLabelScale(&channelInfoPageLabel);
      setLabelColor(&channelInfoPageLabel, activeTheme->textSecondary);
      moveLabel(&channelInfoPageLabel,
                channelInfoVisibleCoordX(panelX + panelW - CHANNEL_INFO_PANEL_PAD) -
                   channelInfoPageLabel.tt.tex.w,
                channelInfoVisibleCoordY(panelY + 12));
      drawLabel(&channelInfoPageLabel);
   }
   drawSidebar();
}

static void suspendSearch(void)
{
   squarePressedAt = 0;
   historySquarePressedAt = 0;
   pauseVideoGrid(&search.grid);
}

static void restoreSearchPlaybackSelection(void)
{
   if (!playbackRestorePending || gridStage(&search.grid) != GRID_READY ||
       search.grid.results.count <= 0) return;
   playbackRestorePending = 0;
   char origin[16], current[16];
   getPlaybackReturnVideoIds(origin, current);
   if (!origin[0]) return;
   const char *target = (current[0] && strcmp(current, origin) != 0) ? NULL : origin;
   int selected = 0;
   if (target) {
      for (int i = 0; i < search.grid.results.count; i++) {
         if (!search.grid.results.items[i].isPlaylist &&
             strcmp(search.grid.results.items[i].videoId, target) == 0) {
            selected = i;
            break;
         }
      }
   }
   search.grid.selected = selected;
   search.grid.scrollRow = selected / (search.grid.columns > 0 ? search.grid.columns : 1);
   int columns = search.grid.columns > 0 ? search.grid.columns : 1;
   int totalRows = (search.grid.results.count + columns - 1) / columns;
   int maxScroll = totalRows - search.grid.visibleRows;
   if (maxScroll < 0) maxScroll = 0;
   if (search.grid.scrollRow > maxScroll) search.grid.scrollRow = maxScroll;
}

static void refreshSearchThemeColors(void)
{
   setLabelColor(&queryLabel, activeTheme->textPrimary);
   setLabelColor(&queryPromptPrefix, activeTheme->textSecondary);
   setLabelColor(&queryPromptSuffix, activeTheme->textSecondary);
   setLabelColor(&sortLabel, activeTheme->textSecondary);
   setLabelColor(&statusLabel, activeTheme->textSecondary);
   setLabelColor(&historyTitleLabel, activeTheme->textSecondary);
   setLabelColor(&historyEmptyLabel, activeTheme->textSecondary);
   setLabelColor(&historyConfirmTitle, activeTheme->textPrimary);
   setLabelColor(&historyConfirmFooter, activeTheme->textSecondary);
   for (int i = 0; i < SORT_COUNT; i++)
      setLabelColor(&sortLabels[i], activeTheme->textSecondary);
   for (int i = 0; i < 4; i++)
      setLabelColor(&channelCategoryLabels[i], activeTheme->textSecondary);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++)
      setLabelColor(&historyLabels[i], activeTheme->textPrimary);
   for (int i = 0; i < hints.count; i++)
      setLabelColor(&hints.hints[i].caption, activeTheme->textSecondary);
   hints.captionColor = activeTheme->textSecondary;
   refreshVideoGridThemeColors(&search.grid);
}

static void resumeSearch(void)
{
   playbackRestorePending = 1;
   refreshSearchThemeColors();
   resumeVideoGrid(&search.grid);
   restoreSearchPlaybackSelection();
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   if (channelInfoWorkerActive) {
      cancelHttpRequestGroup(&channelInfoRequestGroup);
      joinThread(channelInfoWorkerTid);
      channelInfoWorkerActive = 0;
   }
   cancelImagePriorityAsync();
   if (channelInfoTexture.valid) freeTextTexture(&channelInfoTexture);
   if (channelAvatarTexture.offset) freeGfxTexture(&channelAvatarTexture);
   channelAvatarTexture.offset = 0;
   freeLabel(&channelAvatarNameLabel);
   channelAvatarRequested = 0;
   channelAvatarLoaded = 0;
   channelAvatarDecodeAttempted = 0;
   channelInfoVisible = 0;
   freeLabel(&channelInfoPageLabel);
   termButtonHints(&hints);
   freeLabel(&queryLabel);
   freeLabel(&queryPromptPrefix);
   freeLabel(&queryPromptSuffix);
   freeLabel(&sortLabel);
   for (int i = 0; i < 4; i++) freeLabel(&channelCategoryLabels[i]);
   for (int i = 0; i < SORT_COUNT; i++) freeLabel(&sortLabels[i]);
   freeLabel(&statusLabel);
   freeLabel(&historyTitleLabel);
   freeLabel(&historyEmptyLabel);
   freeLabel(&historyConfirmTitle);
   freeLabel(&historyConfirmFooter);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++) freeLabel(&historyLabels[i]);
   freeIcon(&searchIcon);
   closeFont(&uiFont);
}

void openSearchPage(void)
{
   channelReturn.valid = 0;
   restoreChannelReturnPending = 0;
   pendingQuery[0] = 0;
   pendingIsChannel = 0;
   pendingLanding = 1;
   pendingSortValid = 0;
   pushScreen(&searchScreen);
}

void openSearchQuery(const char *query)
{
   channelReturn.valid = 0;
   restoreChannelReturnPending = 0;
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
   channelReturn.valid = 0;
   restoreChannelReturnPending = 0;
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
   openChannelForCategory(item, 0);
}

void openChannelForCategory(const SearchResult *item, int category)
{
   if (!item || !item->channelId[0]) return;
   channelReturn.valid = 0;
   restoreChannelReturnPending = 0;
   pendingChannel = *item;
   pendingChannelCategory = category;
   pendingIsChannel = 1;
   pendingLanding = 0;
   pendingSortValid = 0;
   pendingQuery[0] = 0;
   pushScreen(&searchScreen);
}

Screen searchScreen = { initSearch, resumeSearch, updateSearch, drawSearch, suspendSearch, termSearch, SCREEN_TERMINATED };
