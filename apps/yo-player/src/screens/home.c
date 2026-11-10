// home - the landing screen: trending categories in a VideoGrid (see home.h).

#include "app-config.h"
#include "app.h"
#include "screens/home.h"
#include "screens/search.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "settings.h"
#include "screens/playlist-picker.h"
#include "extractor.h"
#include "vfs.h"
#include "account.h"
#include "downloads.h"
#include "sponsorblock.h"
#include "sidebar.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "thread.h"             // getTimeUs
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "button-repeat.h"
#include "osk-input.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MARGIN_X      80
#define HEADER_Y      54
#define TITLE_SIZE    30
#define GRID_TOP      100
#define HINTS_BOTTOM     88       // standard single-row hint top offset from the screen bottom
#define SD_HINTS_BOTTOM 100       // lower hint-row top offset; keeps 2.0x glyphs inside TV safe area
#define SD_HINT_ROW_GAP   8
#define GRID_BOTTOM_GAP  24
#define SD_GRID_BOTTOM_GAP 20
#define MAX_HINT_ROWS 3
#define HINT_ROW_GAP 8
#define SD_MAX_SCALE    1.5f
#define SD_HEADER_Y      54
#define SD_GRID_TOP     132
#define HINT_GLYPH_H  28
#define HINT_TEXT     22

static const char *TrendingNames[TREND_COUNT] = { "Gaming", "Sports", "Podcasts" };
static const char *LiveNames[LIVE_COUNT] = { "Popular", "Music", "Gaming", "Sports", "Podcasts", "News" };
static const char *AccountNames[ACCOUNT_FEED_COUNT] = { "Recommended", "Subscriptions", "History", "Watch Later" };

// Home categories = Trending, the PS3 queues, and - when signed in - the account's own feeds. Live is a
// separate, uncached page entered from the sidebar; L1/R1 only cycle its six subcategories once inside it.
#define HOME_SUBS        TREND_COUNT
#define HOME_WATCHLATER  (TREND_COUNT + 1)
#define HOME_ACCOUNT     (TREND_COUNT + 2)                      // first account feed; + AccountFeed
#define HOME_CAT_COUNT   (HOME_ACCOUNT + ACCOUNT_FEED_COUNT)
#define HOME_LOCAL_HISTORY (HOME_ACCOUNT + ACCOUNT_FEED_COUNT)  // persistent PS3-local playback history
#define HOME_LIVE         (HOME_LOCAL_HISTORY + 1)               // entered from the sidebar; never in the home L1/R1 ring
#define HOME_PLAYLISTS    (HOME_LIVE + 1)                        // YouTube account playlist library
#define HOME_PLAYLIST_CONTENTS (HOME_PLAYLISTS + 1)              // opened from a YouTube playlist tile
#define HOME_LOCAL_PLAYLISTS (HOME_PLAYLIST_CONTENTS + 1)        // PS3 playlist library: Watch Later / PS3
#define HOME_LOCAL_PLAYLIST_CONTENTS (HOME_LOCAL_PLAYLISTS + 1)  // opened from a local PS3 playlist
#define HOME_DOWNLOADS (HOME_LOCAL_PLAYLIST_CONTENTS + 1)
#define HOME_CACHE_COUNT (HOME_PLAYLISTS + 1)
#define SUBS_PER_CHANNEL 8      // latest videos pulled from each subscribed channel
#define LIVE_MAX_AGE_US (10ULL * 60 * 1000000)

// the L1/R1 cycle. signed out it is the original app's; signed in, the account feeds lead.
static const int SignedOutOrder[] = {
   HOME_SUBS, HOME_WATCHLATER, HOME_LOCAL_HISTORY, TREND_GAMING, TREND_SPORTS, TREND_PODCASTS
};
static const int SignedInOrder[]  = {
   HOME_ACCOUNT + ACCOUNT_FEED_HOME, HOME_ACCOUNT + ACCOUNT_FEED_SUBSCRIPTIONS, HOME_ACCOUNT + ACCOUNT_FEED_HISTORY,
   HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER, HOME_SUBS, HOME_LOCAL_HISTORY, HOME_WATCHLATER,
   TREND_GAMING, TREND_SPORTS, TREND_PODCASTS,
};

// each cached category's fully-loaded results, so returning to a tab skips the feed fetch (thumbnails still refetch).
static SearchResults categoryCache[HOME_CACHE_COUNT];
static int           categoryCached[HOME_CACHE_COUNT];
static SortOrder     categoryCacheSort[HOME_CACHE_COUNT];
static int           subsRevisionSeen;         // subscriptions revision the cached HOME_SUBS feed was built at
static int           watchLaterRevisionSeen;   // watch-later revision the cached HOME_WATCHLATER feed was built at
static int           accountRevisionSeen;      // sign-in state the category order + account caches belong to
static uint64_t      accountFeedRefreshUs[ACCOUNT_FEED_COUNT];
static uint64_t      playlistRefreshUs;
static int           localPlaylistRevisionSeen;
static int           playlistWatchLaterRevisionSeen;
static unsigned int  downloadLibraryRevisionSeen;

// Account feeds refresh periodically while visible. History and the playlist library use short TTLs
// so returning to them is immediate without keeping stale data indefinitely.
#define ACCOUNT_HOME_REFRESH_US          (15ULL * 60 * 1000000)
#define ACCOUNT_SUBSCRIPTIONS_REFRESH_US (10ULL * 60 * 1000000)
#define ACCOUNT_HISTORY_REFRESH_US       (5ULL * 60 * 1000000)
#define ACCOUNT_WATCH_LATER_REFRESH_US   (5ULL * 60 * 1000000)
#define PLAYLISTS_REFRESH_US              (5ULL * 60 * 1000000)
static const uint64_t AccountFeedRefreshIntervals[ACCOUNT_FEED_COUNT] = {
   ACCOUNT_HOME_REFRESH_US, ACCOUNT_SUBSCRIPTIONS_REFRESH_US,
   ACCOUNT_HISTORY_REFRESH_US, ACCOUNT_WATCH_LATER_REFRESH_US
};

static struct {
   VideoGrid grid;
   int       category;
   LiveCategory liveCategory;
   uint64_t  liveLoadedUs;
   int       screenW, screenH;
   int       accountOpen;   // the sign-in / account panel is showing
   int       settingsOpen;  // the settings window is showing
   int       settingsSelected;
   int       downloadSettingsOpen;
   int       downloadSettingsSelected;
   int       exitConfirmOpen;
   int       exitConfirmSelected; // 0 = Yes, 1 = No (safe default)
   int       playlistDeleteConfirmOpen;
   int       playlistDeleteConfirmSelected;
   int       playlistDeleteFromWatchLater;
   char      playlistDeleteId[64]; // YouTube playlist IDs are longer than local playlist IDs.
   int       createPlaylistOskDone;
   int       playlistNameAction; // 1 = create, 2 = rename
   char      playlistNameTargetId[LOCAL_PLAYLIST_ID_LEN];
   char      createPlaylistName[LOCAL_PLAYLIST_NAME_LEN];
   int       regionOpen;
   int       regionSelected;
   int       sponsorblockOpen;
   int       sponsorblockSelected;
   int       regionTop;
   int       regionVisible;
   int       pendingAccountCategory; // account feed selected before signing in, or -1
   int       pendingPlaylistOpen;
   volatile int playlistError;
   int       playlistReturnToSearch;
   int       playlistReturnCategory;
   int       playlistReturnSort;
   int       playlistReturnLocal;
   int       localPlaylistTab; // 0 = saved YouTube playlists, 1 = local PS3 playlists
   char      playlistReturnQuery[SEARCH_QUERY_LEN];
   SortOrder sort;
   PlaylistSortOrder playlistSort;
   char      playlistId[64];
   char      localPlaylistId[LOCAL_PLAYLIST_ID_LEN];
   char      playlistTitle[160];
   SearchResult channelLookupItem;
   sys_ppu_thread_t channelLookupTid;
   volatile int channelLookupPending;
   volatile int channelLookupActive;
   volatile int channelLookupDone;
   int channelLookupResult;
} home;

static Font        uiFont;
static Label       titleLabel, statusLabel, playlistErrorLabel, playlistToastLabel;
static uint64_t    playlistToastShownAt;
static ButtonHints hints;
static int         sdHintsTwoRows;
static int         homeFooterBottomY;
static int         homeFooterRows;
static int         homeFooterTopY;

// UI Scaling is available at every output resolution. Keep the legacy SD names
// and preference path so existing installations retain their saved selection.
#define SD_SCALE_PATH CRTUBE_DATA_DIR "/sd-ui-scale.txt"
static const float SdScaleChoices[] = { 1.00f, 1.50f };
#define SD_SCALE_COUNT ((int)(sizeof SdScaleChoices / sizeof SdScaleChoices[0]))
static int sdScaleLoaded;
static int sdScaleIndex = 1; // default 1.5x on first launch; legacy variable name retained

static void loadSdScale(void)
{
   if (sdScaleLoaded) return;
   sdScaleLoaded = 1;
   char buf[32];
   int n = readFile(SD_SCALE_PATH, buf, sizeof buf - 1);
   if (n > 0) {
      buf[n] = 0;
      float saved = 0.0f;
      if (sscanf(buf, "%f", &saved) == 1) {
         for (int i = 0; i < SD_SCALE_COUNT; i++) {
            float diff = saved - SdScaleChoices[i];
            if (diff > -0.001f && diff < 0.001f) { sdScaleIndex = i; break; }
         }
      }
   }
   setGfxSdUiScale(SdScaleChoices[sdScaleIndex]);
}

static void cycleSdScale(void)
{
   // Keep this index synchronized when another screen changes the shared scale.
   float current = getGfxUiScaleFactor();
   for (int i = 0; i < SD_SCALE_COUNT; i++) {
      float diff = current - SdScaleChoices[i];
      if (diff > -0.001f && diff < 0.001f) { sdScaleIndex = i; break; }
   }
   sdScaleIndex = (sdScaleIndex + 1) % SD_SCALE_COUNT;
   setGfxSdUiScale(SdScaleChoices[sdScaleIndex]);
   char buf[32];
   int n = snprintf(buf, sizeof buf, "%.2f\n", (double)SdScaleChoices[sdScaleIndex]);
   if (n > 0) writeFile(SD_SCALE_PATH, buf, (uint64_t)n);
}


// the account panel
static Label       panelTitle, panelLine1, panelLine2, panelCode, panelFooter;
static ButtonHints panelHints;
static int         panelConfirmHint, panelBackHint;

// Compact settings shell. The selected value is highlighted independently of
// its row label; font scaling never changes the panel's physical geometry.
#define SETTINGS_PANEL_W 680
#define SETTINGS_PANEL_H 520
#define SETTINGS_ROW_HEIGHT 38
#define SETTINGS_ROW_TOP 56
#define SETTINGS_ROW_STEP 43
#define SETTINGS_ROW_COUNT 7
static Label settingsTitle, settingsLabels[SETTINGS_ROW_COUNT], settingsValues[SETTINGS_ROW_COUNT];
static ButtonHints settingsHints;
static Label settingsAutoplayOptions[2];
#define DOWNLOAD_SETTINGS_PANEL_W 720
#define DOWNLOAD_SETTINGS_PANEL_H 500
#define DOWNLOAD_SETTINGS_ROW_COUNT 5
#define DOWNLOAD_SETTINGS_ROW_TOP 66
#define DOWNLOAD_SETTINGS_ROW_STEP 48
static Label downloadSettingsTitle, downloadSettingsLabels[DOWNLOAD_SETTINGS_ROW_COUNT];
static Label downloadSettingsValues[DOWNLOAD_SETTINGS_ROW_COUNT], downloadSettingsNotice;
static ButtonHints downloadSettingsHints;
#define SPONSOR_PANEL_W 760
#define SPONSOR_PANEL_H 520
#define SPONSOR_ROW_H 38
#define SPONSOR_FOOTER_H 52
#define SPONSOR_OPTION_W 76
#define SPONSOR_OPTION_H 30
#define SPONSOR_OPTION_GAP 4
static Label sponsorTitle, sponsorDescription, sponsorCategoryLabels[SPONSOR_CATEGORY_COUNT];
static Label sponsorActionLabels[3];
static ButtonHints sponsorHints;
static ButtonRepeat sponsorNavigateRepeat;
static ButtonRepeat settingsNavigateRepeat;
static ButtonRepeat exitConfirmNavigateRepeat;
static Label exitConfirmTitle, exitConfirmMessage, exitConfirmChoices[2];
static ButtonHints exitConfirmHints;
#define EXIT_CONFIRM_PANEL_W 560
#define EXIT_CONFIRM_PANEL_H 240
#define REGION_MAX_ROWS 40
#define REGION_PANEL_W 620
#define REGION_PANEL_H 440
#define REGION_ROW_H 34
#define REGION_LIST_TOP 78
#define REGION_FOOTER_H 52
static Label regionTitle, regionDescription, regionPosition;
static Label regionNames[REGION_MAX_ROWS], regionCodes[REGION_MAX_ROWS];
static ButtonHints regionHints;
static ButtonRepeat regionNavigateRepeat, regionPageRepeat;
static unsigned regionRevisionSeen;

// Trending's feeds remain separate pages, presented as one page with three
// persistent category chips in the header (matching TEE Vanced's layout).
#define TREND_CHIP_ICON_SIZE 18
#define TREND_CHIP_FONT_SIZE 20
#define TREND_CHIP_HEIGHT 48
#define TREND_CHIP_PAD_X 8
#define TREND_CHIP_ICON_GAP 6
#define TREND_CHIP_GAP 6
#define HOME_SORT_TEXT_SIZE 20
#define HOME_SORT_PAD_X 7
#define HOME_SORT_GAP 4
#define HOME_SORT_HEIGHT 48
static Label trendingChipLabels[TREND_COUNT];
static Icon trendingChipIcons[TREND_COUNT];
static Label liveChipLabels[LIVE_COUNT];
static Label homeSortLabels[SORT_COUNT];
static Label playlistSortLabels[PLAYLIST_SORT_COUNT];
static Label localPlaylistTabLabels[2];
static int homeSortHint = -1;
static int homeSquareHint = -1;
static int homeDownloadHint = -1;
static int homeNewPlaylistHint = -1;
static SearchResult squarePendingItem;
static uint64_t squarePressedAt;
#define SQUARE_HOLD_US 550000ULL
static const IconId TrendingIcons[TREND_COUNT] = { ICON_MICROCHIP, ICON_STAR, ICON_VOLUME_UP };

static int getSettingsPanelWidth(void)
{
   int width = home.screenW - 48;
   return width < SETTINGS_PANEL_W ? width : SETTINGS_PANEL_W;
}

static int getSettingsPanelHeight(void)
{
   int height = home.screenH - 48;
   return height < SETTINGS_PANEL_H ? height : SETTINGS_PANEL_H;
}

// approximate seconds-ago from a "N unit ago" string. approximate is fine here (unlike a precise date sort):
// it only needs to group recent above old across channels. unknown/live sorts last.
#define AGE_UNKNOWN 2000000000L   // larger than any real age, still inside a 32-bit long

static long ageSeconds(const char *published)
{
   const char *digit = published;
   while (*digit && (*digit < '0' || *digit > '9')) digit++;
   if (!*digit) return AGE_UNKNOWN;
   long value = atol(digit);
   if      (strstr(published, "second")) return value;
   else if (strstr(published, "minute")) return value * 60;
   else if (strstr(published, "hour"))   return value * 3600;
   else if (strstr(published, "day"))    return value * 86400;
   else if (strstr(published, "week"))   return value * 604800;
   else if (strstr(published, "month"))  return value * 2592000;
   else if (strstr(published, "year"))   return value * 31536000;
   return AGE_UNKNOWN;
}

static int byRecency(const void *a, const void *b)
{
   long ageA = ageSeconds(((const SearchResult *)a)->published), ageB = ageSeconds(((const SearchResult *)b)->published);
   return (ageA > ageB) - (ageA < ageB);
}

// aggregate the latest videos from each subscribed channel into one feed, newest first. bails if the source
// was swapped away mid-fetch (each channel is a separate blocking request), so switching off stays snappy.
static int fetchSubscriptions(SearchResults *out)
{
   char channels[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int channelCount = getSubscriptions(channels, MAX_SUBSCRIPTIONS);
   memset(out, 0, sizeof *out);
   if (channelCount == 0) return -1;

   SearchResults *page = (SearchResults *)malloc(sizeof *page);
   if (!page) return -1;
   for (int c = 0; c < channelCount && out->count < MAX_SEARCH_RESULTS; c++) {
      if (gridStopRequested(&home.grid)) break;
      if (getChannelVideos(channels[c], NULL, page) != 0) continue;   // NULL token = the channel's latest page
      for (int i = 0, added = 0; i < page->count && added < SUBS_PER_CHANNEL && out->count < MAX_SEARCH_RESULTS; i++) {
         int duplicate = 0;
         for (int j = 0; j < out->count; j++)
            if (strcmp(out->items[j].videoId, page->items[i].videoId) == 0) { duplicate = 1; break; }
         if (duplicate) continue;
         // channel-page items omit their own channel id/name (the channel is implied) - stamp them so the
         // tile shows the channel and Triangle can open it.
         SearchResult *item = &out->items[out->count++];
         *item = page->items[i];
         strCopy(item->channelId, sizeof item->channelId, channels[c]);
         if (!item->author[0]) strCopy(item->author, sizeof item->author, page->channelName);
         added++;
      }
   }
   free(page);
   qsort(out->items, out->count, sizeof out->items[0], byRecency);   // mixed recency across all channels
   out->continuation[0] = 0;   // merged feed - no single continuation to page (for now)
   return out->count > 0 ? 0 : -1;
}

static int fetchLocalHistory(SearchResults *out)
{
   // History stores IDs locally, so current LIVE state must be refreshed from YouTube.
   // VideoGrid resolves it asynchronously for visible items, without delaying the page load.
   int count = getLocalHistory(out);
   for (int i = 0; i < count; i++) out->items[i].checkLiveStatus = 1;
   return count > 0 ? 0 : -1;
}

static void channelLookupWorker(uint64_t arg)
{
   (void)arg;
   home.channelLookupResult = getVideoChannelId(home.channelLookupItem.videoId,
                                                 home.channelLookupItem.channelId,
                                                 sizeof home.channelLookupItem.channelId);
   __sync_synchronize();
   home.channelLookupDone = 1;
   exitThread();
}

static int homeLiveFetchStopped(void)
{
   return gridStopRequested(&home.grid);
}

static int homeFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   if (home.category == HOME_LOCAL_HISTORY) return fetchLocalHistory(out);
   if (home.category == HOME_LIVE) return getLiveFeed(home.liveCategory, token, out, homeLiveFetchStopped);
   if (home.category == HOME_PLAYLISTS) return getAccountPlaylists(token, out);
   if (home.category == HOME_LOCAL_PLAYLISTS)
      return (home.localPlaylistTab == 0 ? getPlaylistWatchLater(out) : getLocalPlaylists(out)) >= 0 ? 0 : -1;
   if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS)
      return getLocalPlaylistVideos(home.localPlaylistId, out) >= 0 ? 0 : -1;
   if (home.category == HOME_DOWNLOADS)
      return getDownloadedVideos(out, home.playlistSort == PLAYLIST_SORT_AZ) >= 0 ? 0 : -1;
   if (home.category == HOME_PLAYLIST_CONTENTS) {
      home.playlistError = 0;
      int rc = getAccountPlaylistVideos(home.playlistId, token, out);
      home.playlistError = rc < 0 ? rc : 0;
      if (rc == 0 && out->count == 0)
         home.playlistError = PLAYLIST_FETCH_UNAVAILABLE;
      return rc;
   }
   if (home.category >= HOME_ACCOUNT && home.category < HOME_LOCAL_HISTORY)
      return getAccountFeed((AccountFeed)(home.category - HOME_ACCOUNT), token, out);
   if (home.category == HOME_SUBS)       return fetchSubscriptions(out);
   if (home.category == HOME_WATCHLATER) return getWatchLater(out) > 0 ? 0 : -1;
   return getTrending(home.category, token, out);
}

static void setHeading(void)
{
   // signed in, the account has its own Subscriptions and Watch Later, so name the PS3's copies apart
   const char *local = isSignedIn() ? " \xe2\x80\xa2 this PS3" : "";
   char heading[128];
   if      (home.category == HOME_LOCAL_HISTORY) snprintf(heading, sizeof heading, "History%s", local);
   else if (home.category >= HOME_ACCOUNT && home.category < HOME_LOCAL_HISTORY)
      snprintf(heading, sizeof heading, "%s", AccountNames[home.category - HOME_ACCOUNT]);
   else if (home.category == HOME_SUBS)       snprintf(heading, sizeof heading, "Subscriptions%s", local);
   else if (home.category == HOME_WATCHLATER) snprintf(heading, sizeof heading, "Watch Later%s", local);
   else if (home.category == HOME_LIVE)       snprintf(heading, sizeof heading, "Live");
   else if (home.category == HOME_PLAYLISTS || home.category == HOME_LOCAL_PLAYLISTS)
      snprintf(heading, sizeof heading, "Playlists");
   else if (home.category == HOME_PLAYLIST_CONTENTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS)
      snprintf(heading, sizeof heading, "%s", home.playlistTitle[0] ? home.playlistTitle : "Playlist");
   else if (home.category == HOME_DOWNLOADS) snprintf(heading, sizeof heading, "Downloads");
   else                                       snprintf(heading, sizeof heading, "Trending");
   setLabelText(&titleLabel, heading);
}

static int homeSupportsSort(void)
{
   return home.category == HOME_SUBS || home.category < TREND_COUNT ||
          home.category == HOME_LIVE ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_HOME ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_SUBSCRIPTIONS;
}

static int homeSupportsPlaylistSort(void)
{
   return home.category == HOME_PLAYLISTS || home.category == HOME_LOCAL_PLAYLISTS || home.category == HOME_DOWNLOADS;
}

static SortPreference currentFeedSortPreference(void)
{
   if (home.category < TREND_COUNT) return (SortPreference)(SORT_PREF_TRENDING_GAMING + home.category);
   if (home.category == HOME_SUBS) return SORT_PREF_PS3_SUBSCRIPTIONS;
   if (home.category == HOME_LIVE) return (SortPreference)(SORT_PREF_LIVE_POPULAR + home.liveCategory);
   if (home.category == HOME_ACCOUNT + ACCOUNT_FEED_HOME) return SORT_PREF_RECOMMENDED;
   return SORT_PREF_ACCOUNT_SUBSCRIPTIONS;
}

static SortPreference currentPlaylistSortPreference(void)
{
   if (home.category == HOME_PLAYLISTS) return SORT_PREF_ACCOUNT_PLAYLISTS;
   return home.localPlaylistTab == 0 ? SORT_PREF_LOCAL_PLAYLISTS_WATCH_LATER : SORT_PREF_LOCAL_PLAYLISTS_PS3;
}

static void updateHomeSortHint(void)
{
   if (homeSortHint < 0) return;
   setButtonHintShown(&hints, homeSortHint, homeSupportsSort() || homeSupportsPlaylistSort());
}

static int homeSortGroupLeft(void)
{
   int count = homeSupportsPlaylistSort() ? PLAYLIST_SORT_COUNT : SORT_COUNT;
   int total = HOME_SORT_GAP * (count - 1);
   for (int i = 0; i < count; i++) {
      Label *label = homeSupportsPlaylistSort() ? &playlistSortLabels[i] : &homeSortLabels[i];
      refreshLabelScale(label);
      total += label->tt.tex.w + HOME_SORT_PAD_X * 2;
   }
   return home.screenW - MARGIN_X - total;
}

static void refreshCurrentCategory(void)
{
   if (home.category < HOME_CACHE_COUNT) categoryCached[home.category] = 0;
   setGridSortMode(&home.grid, homeSupportsSort() ? home.sort : SORT_RELEVANCE);
   setGridPlaylistSortMode(&home.grid, homeSupportsPlaylistSort() ? home.playlistSort : PLAYLIST_SORT_NEWEST);
   if (home.category == HOME_PLAYLISTS) playlistRefreshUs = getTimeUs();
   if (home.category >= HOME_ACCOUNT && home.category < HOME_LOCAL_HISTORY) {
      int feed = home.category - HOME_ACCOUNT;
      accountFeedRefreshUs[feed] = getTimeUs();
   }
   logInfo("[home] manual/periodic refresh category=%d\n", home.category);
   if (home.category == HOME_LIVE) home.liveLoadedUs = getTimeUs();
   setGridSource(&home.grid, homeFetch, NULL);
}

static void loadCategory(void)
{
   home.grid.suppressWatchLaterBadge = (home.category == HOME_WATCHLATER ||
      home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER);
   if (homeSupportsSort()) home.sort = (SortOrder)getSortPreference(currentFeedSortPreference());
   if (homeSupportsPlaylistSort() && home.category != HOME_DOWNLOADS)
      home.playlistSort = (PlaylistSortOrder)getSortPreference(currentPlaylistSortPreference());
   setGridSortMode(&home.grid, homeSupportsSort() ? home.sort : SORT_RELEVANCE);
   setGridPlaylistSortMode(&home.grid, homeSupportsPlaylistSort() ? home.playlistSort : PLAYLIST_SORT_NEWEST);
   if (home.category < HOME_CACHE_COUNT && homeSupportsSort() && categoryCached[home.category] &&
       categoryCacheSort[home.category] != home.sort)
      categoryCached[home.category] = 0;
   setHeading();
   if (home.category == HOME_LOCAL_HISTORY || home.category == HOME_LIVE ||
       home.category == HOME_PLAYLIST_CONTENTS || home.category == HOME_LOCAL_PLAYLISTS ||
       home.category == HOME_LOCAL_PLAYLIST_CONTENTS || home.category == HOME_DOWNLOADS) {
      if (home.category == HOME_LIVE) home.liveLoadedUs = getTimeUs();
      setGridSource(&home.grid, homeFetch, NULL);
      return;
   }
   if (home.category == HOME_PLAYLISTS) {
      uint64_t now = getTimeUs();
      if (!playlistRefreshUs || now - playlistRefreshUs >= PLAYLISTS_REFRESH_US ||
          !categoryCached[HOME_PLAYLISTS]) {
         categoryCached[HOME_PLAYLISTS] = 0;
         playlistRefreshUs = now;
         setGridSource(&home.grid, homeFetch, NULL);
      } else {
         setGridCached(&home.grid, &categoryCache[HOME_PLAYLISTS], homeFetch, NULL);
      }
      return;
   }
   if (home.category >= HOME_ACCOUNT && home.category < HOME_LOCAL_HISTORY) {
      int feed = home.category - HOME_ACCOUNT;
      uint64_t now = getTimeUs();
      uint64_t last = accountFeedRefreshUs[feed];
      int expired = !last || now - last >= AccountFeedRefreshIntervals[feed];
      if (expired || !categoryCached[home.category]) {
         categoryCached[home.category] = 0;
         accountFeedRefreshUs[feed] = now;
         setGridSource(&home.grid, homeFetch, NULL);
      } else {
         setGridCached(&home.grid, &categoryCache[home.category], homeFetch, NULL);
      }
      return;
   }
   if (categoryCached[home.category]) setGridCached(&home.grid, &categoryCache[home.category], homeFetch, NULL);
   else                               setGridSource(&home.grid, homeFetch, NULL);
}

// cache the current category (only when it's fully loaded) so returning to it is instant.
static void cacheCurrent(void)
{
   if (home.category == HOME_LOCAL_HISTORY || home.category == HOME_LIVE ||
       home.category == HOME_PLAYLIST_CONTENTS || home.category == HOME_LOCAL_PLAYLISTS ||
       home.category == HOME_LOCAL_PLAYLIST_CONTENTS || home.category == HOME_DOWNLOADS) return;
   if (gridStage(&home.grid) != GRID_READY || gridBusy(&home.grid)) return;   // don't copy mid-append
   if (home.grid.threadActive) {
      const SearchResults *results = gridResults(&home.grid);
      for (int i = 0; i < results->count; i++)
         if (results->items[i].isPlaylist) return;   // playlist thumbnail workers resolve IDs in-place
   }
   categoryCache[home.category] = *gridResults(&home.grid);
   categoryCacheSort[home.category] = homeSupportsSort() ? home.sort : SORT_RELEVANCE;
   categoryCached[home.category] = 1;
}

static void switchToCategory(int category);
static void openAccountPanel(void);

static void setSidebarForCategory(void)
{
   if (home.category == HOME_DOWNLOADS) setSidebarCurrent(SIDEBAR_DOWNLOADS);
   else if (home.category == HOME_LOCAL_PLAYLISTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS) setSidebarCurrent(SIDEBAR_PS3_PLAYLISTS);
   else if (home.category == HOME_PLAYLISTS || home.category == HOME_PLAYLIST_CONTENTS) setSidebarCurrent(SIDEBAR_PLAYLISTS);
   else if (home.category == HOME_LIVE) setSidebarCurrent(SIDEBAR_LIVE);
   else if (home.category == HOME_WATCHLATER) setSidebarCurrent(SIDEBAR_PS3_WATCH_LATER);
   else if (home.category == HOME_LOCAL_HISTORY) setSidebarCurrent(SIDEBAR_PS3_HISTORY);
   else if (home.category == HOME_SUBS) setSidebarCurrent(SIDEBAR_PS3_SUBSCRIPTIONS);
   else if (home.category == HOME_ACCOUNT + ACCOUNT_FEED_HOME) setSidebarCurrent(SIDEBAR_RECOMMENDED);
   else if (home.category == HOME_ACCOUNT + ACCOUNT_FEED_SUBSCRIPTIONS) setSidebarCurrent(SIDEBAR_ACCOUNT_SUBSCRIPTIONS);
   else if (home.category == HOME_ACCOUNT + ACCOUNT_FEED_HISTORY) setSidebarCurrent(SIDEBAR_ACCOUNT_HISTORY);
   else if (home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER) setSidebarCurrent(SIDEBAR_ACCOUNT_WATCH_LATER);
   else setSidebarCurrent(SIDEBAR_TRENDING);
}

static void selectAccountFeed(AccountFeed feed)
{
   int category = HOME_ACCOUNT + (int)feed;
   if (isSignedIn()) {
      switchToCategory(category);
      return;
   }
   home.pendingAccountCategory = category;
   openAccountPanel();
}

static void switchToCategory(int category)
{
   if (category != HOME_PLAYLIST_CONTENTS && home.category == HOME_PLAYLIST_CONTENTS &&
       (home.playlistReturnToSearch || home.playlistReturnLocal)) {
      home.playlistReturnToSearch = 0;
      home.playlistReturnLocal = 0;
      home.playlistReturnCategory = -1;
      home.playlistReturnSort = SORT_RELEVANCE;
      home.playlistReturnQuery[0] = 0;
   }
   if (category < 0 ||
       (category >= HOME_CAT_COUNT && category != HOME_LOCAL_HISTORY && category != HOME_LIVE &&
        category != HOME_PLAYLISTS && category != HOME_PLAYLIST_CONTENTS &&
        category != HOME_LOCAL_PLAYLISTS && category != HOME_LOCAL_PLAYLIST_CONTENTS &&
        category != HOME_DOWNLOADS) ||
       category == home.category) return;
   cacheCurrent();
   home.category = category;
   loadCategory();
   setSidebarForCategory();
   updateHomeSortHint();
}

void openHomePlaylist(const char *playlistId, const char *title)
{
   if (!playlistId || !playlistId[0]) return;
   home.playlistReturnToSearch = 0;
   home.playlistReturnLocal = 0;
   home.playlistReturnCategory = -1;
   home.playlistReturnSort = SORT_RELEVANCE;
   home.playlistReturnQuery[0] = 0;
   home.playlistError = 0;
   strCopy(home.playlistId, sizeof home.playlistId, playlistId);
   strCopy(home.playlistTitle, sizeof home.playlistTitle, title && title[0] ? title : "Playlist");
   home.pendingPlaylistOpen = 0;
   if (home.category == HOME_PLAYLIST_CONTENTS) {
      loadCategory();
      setSidebarForCategory();
   } else {
      switchToCategory(HOME_PLAYLIST_CONTENTS);
   }
}

void openHomePlaylistFromSearch(const char *playlistId, const char *title, const char *query, int sort)
{
   if (!playlistId || !playlistId[0]) return;
   home.playlistReturnToSearch = 1;
   home.playlistReturnLocal = 0;
   home.playlistReturnCategory = home.category;
   home.playlistReturnSort = sort;
   strCopy(home.playlistReturnQuery, sizeof home.playlistReturnQuery, query ? query : "");
   home.playlistError = 0;
   strCopy(home.playlistId, sizeof home.playlistId, playlistId);
   strCopy(home.playlistTitle, sizeof home.playlistTitle, title && title[0] ? title : "Playlist");
   home.pendingPlaylistOpen = 0;
   if (home.category == HOME_PLAYLIST_CONTENTS) {
      loadCategory();
      setSidebarForCategory();
   } else {
      switchToCategory(HOME_PLAYLIST_CONTENTS);
   }
}

static void switchCategory(int delta)
{
   if (home.category == HOME_LOCAL_PLAYLISTS) {
      home.localPlaylistTab = 1 - home.localPlaylistTab;
      loadCategory();
      updateHomeSortHint();
      return;
   }
   if (home.category == HOME_PLAYLISTS || home.category == HOME_PLAYLIST_CONTENTS ||
       home.category == HOME_LOCAL_PLAYLIST_CONTENTS || home.category == HOME_DOWNLOADS) return;
   if (home.category == HOME_LIVE) {
      home.liveCategory = (LiveCategory)((home.liveCategory + delta + LIVE_COUNT) % LIVE_COUNT);
      loadCategory();
      return;
   }
   cacheCurrent();
   const int *order = isSignedIn() ? SignedInOrder : SignedOutOrder;
   int count = isSignedIn() ? (int)(sizeof SignedInOrder / sizeof SignedInOrder[0])
                            : (int)(sizeof SignedOutOrder / sizeof SignedOutOrder[0]);
   int position = 0;
   while (position < count && order[position] != home.category) position++;
   if (position == count) position = 0;   // the category left the cycle (signed out): restart it
   home.category = order[(position + delta + count) % count];
   loadCategory();
   setSidebarForCategory();
   updateHomeSortHint();
}

// signed in or out (here, or a revoked token found by a feed request): the account caches belong to the
// old state, so drop them and land on the first tab of the new cycle.
static void applyAccountChange(void)
{
   accountRevisionSeen = getAccountRevision();
   for (int feed = 0; feed < ACCOUNT_FEED_COUNT; feed++) {
      categoryCached[HOME_ACCOUNT + feed] = 0;
      accountFeedRefreshUs[feed] = 0;
   }
   categoryCached[HOME_PLAYLISTS] = 0;
   playlistRefreshUs = 0;
   home.category = isSignedIn() ? HOME_ACCOUNT + ACCOUNT_FEED_HOME : HOME_SUBS;
   loadCategory();
   setSidebarForCategory();
   updateHomeSortHint();
}

// ---- account panel (Select) ----

#define PANEL_W 820
#define PANEL_H 360

static int getAccountPanelWidth(void)
{
   int width = home.screenW - 48;
   return width < PANEL_W ? width : PANEL_W;
}

static int getAccountPanelHeight(void)
{
   int height = home.screenH - 48;
   return height < PANEL_H ? height : PANEL_H;
}

// setLabelText re-rasterises, so the countdown only pays for it once a second
static void setLabelIfChanged(Label *label, const char *text)
{
   if (strcmp(label->text, text) != 0) setLabelText(label, text);
}

static void openAccountPanel(void)
{
   home.accountOpen = 1;
   if (!isSignedIn()) beginSignIn();
}

static void closeAccountPanel(void)
{
   cancelSignIn();
   home.accountOpen = 0;
   home.pendingAccountCategory = -1;
   home.pendingPlaylistOpen = 0;
}

static void updateAccountPanel(void)
{
   SignInStatus status;
   getSignInStatus(&status);
   int retryable = status.stage == SIGNIN_DENIED || status.stage == SIGNIN_EXPIRED || status.stage == SIGNIN_FAILED;

   if (status.stage == SIGNIN_DONE) {
      home.accountOpen = 0;
      if (home.pendingPlaylistOpen) {
         home.pendingPlaylistOpen = 0;
         home.pendingAccountCategory = -1;
         if (home.category == HOME_PLAYLIST_CONTENTS) loadCategory();
         else switchToCategory(HOME_PLAYLIST_CONTENTS);
      } else if ((home.pendingAccountCategory >= HOME_ACCOUNT && home.pendingAccountCategory < HOME_LOCAL_HISTORY) ||
                 home.pendingAccountCategory == HOME_PLAYLISTS) {
         int category = home.pendingAccountCategory;
         home.pendingAccountCategory = -1;
         switchToCategory(category);
      }
      return;
   }   // applyAccountChange shows the new feeds
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { closeAccountPanel(); return; }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (isSignedIn())   { signOut(); home.accountOpen = 0; }
      else if (retryable) beginSignIn();
   }
}

static void drawAccountPanel(void)
{
   SignInStatus status;
   getSignInStatus(&status);

   const char *title = "Sign in to YouTube", *line1 = "", *line2 = "", *code = "", *confirm = NULL;
   char url[96] = "", footer[64] = "";
   if (isSignedIn()) {
      title   = "Signed in to YouTube";
      line1   = "Recommended, Subscriptions, History and Watch Later";
      line2   = "come from your account. Videos still play signed out.";
      confirm = "Sign out";
   } else if (status.stage == SIGNIN_WAITING) {
      // "https://www.google.com/device" reads better without the scheme
      const char *shown = strstr(status.verificationUrl, "://");
      strCopy(url, sizeof url, shown ? shown + 3 : status.verificationUrl);
      line1 = "On your phone or computer, go to";
      line2 = url;
      code  = status.userCode;
      snprintf(footer, sizeof footer, "and enter this code. It expires in %d:%02d.", status.secondsLeft / 60, status.secondsLeft % 60);
   } else if (status.stage == SIGNIN_DENIED)  { line1 = "Sign-in was declined on the consent screen."; confirm = "Try again"; }
   else if (status.stage == SIGNIN_EXPIRED)   { line1 = "The code expired before it was entered.";    confirm = "Try again"; }
   else if (status.stage == SIGNIN_FAILED)    { line1 = "Couldn't reach Google. Check the connection."; confirm = "Try again"; }
   else                                       { line1 = "Getting a sign-in code..."; }

   setLabelIfChanged(&panelTitle, title);
   setLabelIfChanged(&panelLine1, line1);
   setLabelIfChanged(&panelLine2, line2);
   setLabelIfChanged(&panelCode, code);
   setLabelIfChanged(&panelFooter, footer);
   setButtonHintShown(&panelHints, panelConfirmHint, confirm != NULL);
   if (confirm && strcmp(panelHints.hints[panelConfirmHint].caption.text, confirm) != 0)
      setButtonHintCaption(&panelHints, panelConfirmHint, confirm);
   const char *back = (status.stage == SIGNIN_STARTING || status.stage == SIGNIN_WAITING) ? "Cancel" : "Close";
   if (strcmp(panelHints.hints[panelBackHint].caption.text, back) != 0) setButtonHintCaption(&panelHints, panelBackHint, back);

   int panelW = getAccountPanelWidth(), panelH = getAccountPanelHeight();
   int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2, centerX = home.screenW / 2;
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRectangle(x, y, panelW, panelH, activeTheme->appBg);
   fillGfxRectangle(x, y, panelW, 4, activeTheme->accent);
   drawLabelCentered(&panelTitle, centerX, y + 28);
   drawLabelCentered(&panelLine1, centerX, y + panelH / 4);
   if (line2[0])  drawLabelCentered(&panelLine2, centerX, y + panelH / 4 + 36);
   if (code[0])   drawLabelCentered(&panelCode, centerX, y + panelH / 2 - 4);
   if (footer[0]) drawLabelCentered(&panelFooter, centerX, y + panelH - 92);
   drawButtonHints(&panelHints, home.screenW);
}

static int isLargeUiScale(void)
{
   return getGfxUiScaleFactor() >= 1.25f;
}

static int getRegionRowHeight(void)
{
   return isLargeUiScale() ? 42 : REGION_ROW_H;
}

static int getSponsorRowHeight(void)
{
   return isLargeUiScale() ? 40 : SPONSOR_ROW_H;
}

static int getSponsorOptionWidth(void)
{
   return isLargeUiScale() ? 84 : SPONSOR_OPTION_W;
}

static int getSponsorOptionHeight(void)
{
   return isLargeUiScale() ? 34 : SPONSOR_OPTION_H;
}

static int getRegionPanelWidth(void)
{
   int width = home.screenW - 48;
   return width < REGION_PANEL_W ? width : REGION_PANEL_W;
}

static int getRegionPanelHeight(void)
{
   int height = home.screenH - 48;
   return height < REGION_PANEL_H ? height : REGION_PANEL_H;
}

static void clampRegionTop(void)
{
   int maxTop = getContentRegionCount() - home.regionVisible;
   if (maxTop < 0) maxTop = 0;
   if (home.regionTop > maxTop) home.regionTop = maxTop;
   if (home.regionTop < 0) home.regionTop = 0;
}

static void followRegionSelection(void)
{
   if (home.regionSelected < home.regionTop) home.regionTop = home.regionSelected;
   if (home.regionSelected >= home.regionTop + home.regionVisible)
      home.regionTop = home.regionSelected - home.regionVisible + 1;
   clampRegionTop();
}

static void openRegionPicker(void)
{
   int count = getContentRegionCount();
   if (count <= 0) return;
   home.regionOpen = 1;
   home.regionSelected = getContentRegionIndex();
   int panelH = getRegionPanelHeight();
   int available = panelH - REGION_LIST_TOP - REGION_FOOTER_H;
   home.regionVisible = available / getRegionRowHeight();
   if (home.regionVisible < 1) home.regionVisible = 1;
   if (home.regionVisible > count) home.regionVisible = count;
   home.regionTop = home.regionSelected - home.regionVisible / 2;
   clampRegionTop();
   uint64_t now = sys_time_get_system_time();
   regionNavigateRepeat.timer = now; regionNavigateRepeat.repeats = 0;
   regionPageRepeat.timer = now; regionPageRepeat.repeats = 0;
}

static void updateRegionPicker(void)
{
   int count = getContentRegionCount();
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { home.regionOpen = 0; return; }
   if (count <= 0) return;

   if      (isRepeatDue(&regionNavigateRepeat, getPadButtonState(PAD_BTN_DOWN))) {
      home.regionSelected = (home.regionSelected + 1) % count;
      followRegionSelection();
   } else if (isRepeatDue(&regionNavigateRepeat, getPadButtonState(PAD_BTN_UP))) {
      home.regionSelected = (home.regionSelected + count - 1) % count;
      followRegionSelection();
   }

   if (isRepeatDue(&regionPageRepeat, getPadButtonState(PAD_BTN_R2))) {
      home.regionSelected += home.regionVisible;
      if (home.regionSelected >= count) home.regionSelected = count - 1;
      followRegionSelection();
   } else if (isRepeatDue(&regionPageRepeat, getPadButtonState(PAD_BTN_L2))) {
      home.regionSelected -= home.regionVisible;
      if (home.regionSelected < 0) home.regionSelected = 0;
      followRegionSelection();
   }

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      setContentRegionIndex(home.regionSelected);
      home.regionOpen = 0;
   }
}

static void drawRegionPicker(void)
{
   int panelW = getRegionPanelWidth(), panelH = getRegionPanelHeight();
   int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2;
   int count = getContentRegionCount();
   int current = getContentRegionIndex();
   int rowHeight = getRegionRowHeight();
   char position[24];

   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);

   refreshLabelScale(&regionTitle);
   refreshLabelScale(&regionDescription);
   refreshLabelScale(&regionPosition);
   drawLabelCentered(&regionTitle, home.screenW / 2, y + 27);
   moveLabel(&regionDescription, x + 28, y + 52);
   setLabelColor(&regionDescription, activeTheme->textSecondary);
   drawLabel(&regionDescription);
   snprintf(position, sizeof position, "%d / %d", home.regionSelected + 1, count);
   setLabelIfChanged(&regionPosition, position);
   moveLabel(&regionPosition, x + panelW - 28 - regionPosition.tt.tex.w, y + 52);
   setLabelColor(&regionPosition, activeTheme->textSecondary);
   drawLabel(&regionPosition);

   int end = home.regionTop + home.regionVisible;
   if (end > count) end = count;
   for (int row = home.regionTop; row < end; row++) {
      int rowY = y + REGION_LIST_TOP + (row - home.regionTop) * rowHeight;
      int selected = row == home.regionSelected;
      int inUse = row == current;
      refreshLabelScale(&regionNames[row]);
      refreshLabelScale(&regionCodes[row]);
      int labelY = getCenteredLabelY(&regionNames[row], rowY, rowHeight - 3);
      if (selected)
         fillGfxRoundedRectangleFixed(x + 18, labelY - 5, panelW - 36,
                                      regionNames[row].tt.tex.h + 10, 7, activeTheme->rowHighlight);
      if (inUse)
         fillGfxRoundedRectangleFixed(x + 28, rowY + 7, 3, rowHeight - 17, 2, activeTheme->accent);
      setLabelColor(&regionNames[row], inUse ? activeTheme->accent :
                    (selected ? activeTheme->textPrimary : activeTheme->textSecondary));
      setLabelColor(&regionCodes[row], selected ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(&regionNames[row], x + 44, labelY);
      moveLabel(&regionCodes[row], x + panelW - 42 - regionCodes[row].tt.tex.w,
                getCenteredLabelY(&regionCodes[row], rowY, rowHeight - 3));
      drawLabel(&regionNames[row]);
      drawLabel(&regionCodes[row]);
   }
   regionHints.y = y + panelH - 40;
   drawButtonHints(&regionHints, home.screenW);
}

static int getSponsorPanelWidth(void)
{
   int width = home.screenW - 48;
   return width < SPONSOR_PANEL_W ? width : SPONSOR_PANEL_W;
}

static int getSponsorPanelHeight(void)
{
   int height = home.screenH - 48;
   return height < SPONSOR_PANEL_H ? height : SPONSOR_PANEL_H;
}

static void openSponsorBlockPanel(void)
{
   home.sponsorblockOpen = 1;
   home.sponsorblockSelected = 0;
   uint64_t now = sys_time_get_system_time();
   sponsorNavigateRepeat.timer = now;
   sponsorNavigateRepeat.repeats = 0;
}

static void updateSponsorBlockPanel(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { home.sponsorblockOpen = 0; return; }
   if (isRepeatDue(&sponsorNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.sponsorblockSelected = (home.sponsorblockSelected + 1) % SPONSOR_CATEGORY_COUNT;
   else if (isRepeatDue(&sponsorNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      home.sponsorblockSelected = (home.sponsorblockSelected + SPONSOR_CATEGORY_COUNT - 1) % SPONSOR_CATEGORY_COUNT;

   int action = getSponsorAction(home.sponsorblockSelected);
   int option = action == SPONSOR_ACTION_SKIP ? 0 : action == SPONSOR_ACTION_ASK ? 1 : 2;
   if (isPadButtonPressed(PAD_BTN_LEFT)) option = (option + 2) % 3;
   else if (isPadButtonPressed(PAD_BTN_RIGHT) || isPadButtonPressed(PAD_BTN_CROSS)) option = (option + 1) % 3;
   else return;
   action = option == 0 ? SPONSOR_ACTION_SKIP : option == 1 ? SPONSOR_ACTION_ASK : SPONSOR_ACTION_KEEP;
   setSponsorAction(home.sponsorblockSelected, action);
}

static void drawSponsorBlockPanel(void)
{
   int panelW = getSponsorPanelWidth(), panelH = getSponsorPanelHeight();
   int rowHeight = getSponsorRowHeight();
   int optionWidth = getSponsorOptionWidth();
   int optionHeight = getSponsorOptionHeight();
   int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2;
   static const int actionValues[3] = { SPONSOR_ACTION_SKIP, SPONSOR_ACTION_ASK, SPONSOR_ACTION_KEEP };
   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);

   refreshLabelScale(&sponsorTitle);
   drawLabelCentered(&sponsorTitle, home.screenW / 2, y + 27);
   refreshLabelScale(&sponsorDescription);
   setLabelColor(&sponsorDescription, activeTheme->textSecondary);
   moveLabel(&sponsorDescription, x + 28, y + 56);
   drawLabel(&sponsorDescription);

   int rowTop = y + (panelH >= 480 ? (isLargeUiScale() ? 148 : 160) :
                     panelH - SPONSOR_FOOTER_H - SPONSOR_CATEGORY_COUNT * rowHeight - 12);
   int optionGroupW = 3 * optionWidth + 2 * SPONSOR_OPTION_GAP;
   int optionX = x + panelW - 28 - optionGroupW;
   for (int row = 0; row < SPONSOR_CATEGORY_COUNT; row++) {
      int rowY = rowTop + row * rowHeight;
      int focused = row == home.sponsorblockSelected;
      if (focused)
         fillGfxRoundedRectangleFixed(x + 18, rowY, panelW - 36, rowHeight - 2, 7, activeTheme->rowHighlight);
      fillGfxRoundedRectangleFixed(x + 29, rowY + rowHeight / 2 - 6, 10, 10, 5,
                                    getSponsorCategoryColor((SponsorCategory)row));
      Label *name = &sponsorCategoryLabels[row];
      refreshLabelScale(name);
      setLabelColor(name, focused ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(name, x + 52, getCenteredLabelY(name, rowY, rowHeight - 2));
      drawLabel(name);

      int chosen = getSponsorAction(row);
      for (int option = 0; option < 3; option++) {
         int ox = optionX + option * (optionWidth + SPONSOR_OPTION_GAP);
         int selected = chosen == actionValues[option];
         uint32_t bg = selected ? (focused ? activeTheme->accent : activeTheme->surface) : activeTheme->surface;
         int optionY = rowY + (rowHeight - optionHeight) / 2;
         fillGfxRoundedRectangleFixed(ox, optionY, optionWidth, optionHeight, 7, bg);
         Label *word = &sponsorActionLabels[option];
         refreshLabelScale(word);
         setLabelColor(word, selected ? (focused ? activeTheme->appBg : activeTheme->textPrimary) : 0xFF888888);
         moveLabel(word, ox + (optionWidth - word->tt.tex.w) / 2,
                   getCenteredLabelY(word, optionY, optionHeight));
         drawLabel(word);
      }
   }
   sponsorHints.y = y + panelH - 40;
   drawButtonHints(&sponsorHints, home.screenW);
}

static void openExitConfirmation(void)
{
   home.exitConfirmOpen = 1;
   home.exitConfirmSelected = 1; // No is selected by default.
   exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
}

static void updateExitConfirmation(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      home.exitConfirmOpen = 0;
      return;
   }
   if (isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_LEFT)) ||
       isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      home.exitConfirmSelected = (home.exitConfirmSelected + 1) % 2;
   else if (isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_RIGHT)) ||
            isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.exitConfirmSelected = (home.exitConfirmSelected + 1) % 2;

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (home.exitConfirmSelected == 0) requestAppExit();
      else home.exitConfirmOpen = 0;
   }
}

static void drawExitConfirmation(void)
{
   int panelW = home.screenW - 48;
   if (panelW > EXIT_CONFIRM_PANEL_W) panelW = EXIT_CONFIRM_PANEL_W;
   int panelH = EXIT_CONFIRM_PANEL_H;
   if (panelH > home.screenH - 48) panelH = home.screenH - 48;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;

   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangle(x, y, panelW, panelH, 10, activeTheme->appBg);
   fillGfxRoundedRectangle(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&exitConfirmTitle);
   refreshLabelScale(&exitConfirmMessage);
   drawLabelCentered(&exitConfirmTitle, home.screenW / 2, y + 34);
   drawLabelCentered(&exitConfirmMessage, home.screenW / 2, y + 94);

   int choiceY = y + 148;
   int choiceW = 112, choiceH = 40, gap = 24;
   int choicesX = home.screenW / 2 - choiceW - gap / 2;
   for (int i = 0; i < 2; i++) {
      Label *label = &exitConfirmChoices[i];
      refreshLabelScale(label);
      int bx = choicesX + i * (choiceW + gap);
      fillGfxRoundedRectangle(bx, choiceY, choiceW, choiceH, 8,
                              i == home.exitConfirmSelected ? activeTheme->accent : activeTheme->surface);
      setLabelColor(label, i == home.exitConfirmSelected ? activeTheme->appBg : activeTheme->textPrimary);
      moveLabel(label, bx + (choiceW - label->tt.tex.w) / 2,
                getCenteredLabelY(label, choiceY, choiceH));
      drawLabel(label);
   }
   exitConfirmHints.y = y + panelH - 34;
   drawButtonHints(&exitConfirmHints, home.screenW);
}

static void onHomePlaylistNameEntered(const char *name)
{
   home.createPlaylistName[0] = 0;
   if (name && name[0]) strCopy(home.createPlaylistName, sizeof home.createPlaylistName, name);
   home.createPlaylistOskDone = 1;
}

void openHomeSettingsPanel(void)
{
   home.settingsOpen = 1;
   home.settingsSelected = 0;
   home.downloadSettingsOpen = 0;
   settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
}

static const char *downloadResolutionName(int value)
{
   static const char *names[] = { "Auto", "480p", "720p", "1080p" };
   return value >= 0 && value < 4 ? names[value] : names[0];
}

static const char *downloadLanguageName(const char *code, int originalAllowed)
{
   if (originalAllowed && strcmp(code, "original") == 0) return "Original upload";
   if (strcmp(code, "off") == 0) return "Off";
   for (int i = 0; i < getDownloadLanguageCount(); i++) {
      const DownloadLanguage *language = getDownloadLanguageAt(i);
      if (language && strcmp(language->code, code) == 0) return language->name;
   }
   return code;
}

static void cycleDownloadSetting(int row, int direction)
{
   if (row == 0) {
      int value = (getDownloadResolution() + direction + 4) % 4;
      setDownloadResolution(value);
   } else if (row == 1) {
      int count = getDownloadLanguageCount() + 1;
      int current = 0;
      const char *code = getDownloadAudioLanguage();
      if (strcmp(code, "original") != 0) {
         for (int i = 0; i < getDownloadLanguageCount(); i++) {
            const DownloadLanguage *language = getDownloadLanguageAt(i);
            if (language && strcmp(language->code, code) == 0) { current = i + 1; break; }
         }
      }
      current = (current + direction + count) % count;
      const DownloadLanguage *language = current > 0 ? getDownloadLanguageAt(current - 1) : NULL;
      setDownloadAudioLanguage(language ? language->code : "original");
   } else if (row == 2) {
      if (strcmp(getDownloadAudioLanguage(), "original") != 0)
         setDownloadAutoDubEnabled(!getDownloadAutoDubEnabled());
   } else if (row == 3) {
      setDownloadPrimarySubtitlesEnabled(!getDownloadPrimarySubtitlesEnabled());
   } else if (row == 4) {
      int count = getDownloadLanguageCount() + 1;
      int current = 0;
      const char *code = getDownloadSecondarySubtitleLanguage();
      if (strcmp(code, "off") != 0) {
         for (int i = 0; i < getDownloadLanguageCount(); i++) {
            const DownloadLanguage *language = getDownloadLanguageAt(i);
            if (language && strcmp(language->code, code) == 0) { current = i + 1; break; }
         }
      }
      current = (current + direction + count) % count;
      const DownloadLanguage *language = current > 0 ? getDownloadLanguageAt(current - 1) : NULL;
      setDownloadSecondarySubtitleLanguage(language ? language->code : "off");
   }
}

static void updateDownloadSettingsPanel(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
      home.downloadSettingsOpen = 0;
      return;
   }
   if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.downloadSettingsSelected = (home.downloadSettingsSelected + 1) % DOWNLOAD_SETTINGS_ROW_COUNT;
   else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      home.downloadSettingsSelected = (home.downloadSettingsSelected + DOWNLOAD_SETTINGS_ROW_COUNT - 1) % DOWNLOAD_SETTINGS_ROW_COUNT;
   int direction = 0;
   if (isPadButtonPressed(PAD_BTN_LEFT)) direction = -1;
   else if (isPadButtonPressed(PAD_BTN_RIGHT) || isPadButtonPressed(PAD_BTN_CROSS)) direction = 1;
   if (direction) cycleDownloadSetting(home.downloadSettingsSelected, direction);
}

static void updateSettingsPanel(void)
{
   if (home.downloadSettingsOpen) { updateDownloadSettingsPanel(); return; }
   if (home.regionOpen) { updateRegionPicker(); return; }
   if (home.sponsorblockOpen) { updateSponsorBlockPanel(); return; }
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) { home.settingsOpen = 0; return; }
   if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.settingsSelected = (home.settingsSelected + 1) % SETTINGS_ROW_COUNT;
   else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      home.settingsSelected = (home.settingsSelected + SETTINGS_ROW_COUNT - 1) % SETTINGS_ROW_COUNT;
   if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_LEFT) ||
       isPadButtonPressed(PAD_BTN_RIGHT)) {
      if (home.settingsSelected == 0 || home.settingsSelected == 1) {
         int enabled = home.settingsSelected == 0 ? getAutoplayEnabled() : getEnable1080p60();
         if (isPadButtonPressed(PAD_BTN_LEFT)) enabled = 0;
         else if (isPadButtonPressed(PAD_BTN_RIGHT)) enabled = 1;
         else enabled = !enabled;
         if (home.settingsSelected == 0) setAutoplayEnabled(enabled);
         else setEnable1080p60(enabled);
      } else if (home.settingsSelected == 2) {
         cycleSdScale();
      } else if (home.settingsSelected == 3) {
         int delta = isPadButtonPressed(PAD_BTN_LEFT) ? -1 : 1;
         setOverscanPercent(getOverscanPercent() + delta);
      } else if (home.settingsSelected == 4 && isPadButtonPressed(PAD_BTN_CROSS)) {
         openRegionPicker();
      } else if (home.settingsSelected == 5 && isPadButtonPressed(PAD_BTN_CROSS)) {
         openSponsorBlockPanel();
      } else if (home.settingsSelected == 6 && isPadButtonPressed(PAD_BTN_CROSS)) {
         home.downloadSettingsOpen = 1;
         home.downloadSettingsSelected = 0;
         settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
      }
   }
}

static void drawDownloadSettingsPanel(void)
{
   setLabelIfChanged(&downloadSettingsValues[0], downloadResolutionName(getDownloadResolution()));
   setLabelIfChanged(&downloadSettingsValues[1], downloadLanguageName(getDownloadAudioLanguage(), 1));
   setLabelIfChanged(&downloadSettingsValues[2], strcmp(getDownloadAudioLanguage(), "original") == 0 ?
                     "Not applicable" : (getDownloadAutoDubEnabled() ? "Allow auto-generated" : "Uploaded only"));
   setLabelIfChanged(&downloadSettingsValues[3], getDownloadPrimarySubtitlesEnabled() ? "Auto (match audio)" : "Off");
   setLabelIfChanged(&downloadSettingsValues[4], downloadLanguageName(getDownloadSecondarySubtitleLanguage(), 0));
   int panelW = DOWNLOAD_SETTINGS_PANEL_W;
   int panelH = DOWNLOAD_SETTINGS_PANEL_H;
   if (panelW > home.screenW - 32) panelW = home.screenW - 32;
   if (panelH > home.screenH - 32) panelH = home.screenH - 32;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangle(x, y, panelW, panelH, 10, activeTheme->appBg);
   fillGfxRoundedRectangle(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&downloadSettingsTitle);
   drawLabelCentered(&downloadSettingsTitle, home.screenW / 2, y + 30);

   for (int i = 0; i < DOWNLOAD_SETTINGS_ROW_COUNT; i++) {
      Label *name = &downloadSettingsLabels[i];
      Label *settingValue = &downloadSettingsValues[i];
      int rowY = y + DOWNLOAD_SETTINGS_ROW_TOP + i * DOWNLOAD_SETTINGS_ROW_STEP;
      refreshLabelScale(name);
      setLabelColor(name, activeTheme->textPrimary);
      moveLabel(name, x + 34, getCenteredLabelY(name, rowY, 40));
      drawLabel(name);
      refreshLabelScale(settingValue);
      int valueW = settingValue->tt.tex.w + 24;
      if (valueW < 140) valueW = 140;
      if (valueW > panelW / 2) valueW = panelW / 2;
      int valueX = x + panelW - 34 - valueW;
      int valueY = getCenteredLabelY(settingValue, rowY, 40);
      if (i == home.downloadSettingsSelected)
         fillGfxRoundedRectangle(valueX, valueY - 5, valueW, settingValue->tt.tex.h + 10, 7,
                                 activeTheme->rowHighlight);
      int disabled = i == 2 && strcmp(getDownloadAudioLanguage(), "original") == 0;
      setLabelColor(settingValue, disabled ? activeTheme->textSecondary :
                    (i == home.downloadSettingsSelected ? activeTheme->accent : activeTheme->textPrimary));
      moveLabel(settingValue, valueX + (valueW - settingValue->tt.tex.w) / 2, valueY);
      drawLabel(settingValue);
   }
   refreshLabelScale(&downloadSettingsNotice);
   moveLabel(&downloadSettingsNotice, x + 34, y + panelH - 74);
   drawLabel(&downloadSettingsNotice);
   downloadSettingsHints.y = y + panelH - 36;
   drawButtonHints(&downloadSettingsHints, home.screenW);
}

static void drawSettingsPanel(void)
{
   // Avoid printf floating-point formatting here: the PS3 runtime may be built
   // without float printf support, which can leave only the trailing "x".
   const char *scaleValue = getGfxUiScaleFactor() >= 1.25f ? "1.5x" : "1.0x";
   char overscanValue[16];
   int overscan = getOverscanPercent();
   if (overscan > 0) snprintf(overscanValue, sizeof overscanValue, "+%d%%", overscan);
   else              snprintf(overscanValue, sizeof overscanValue, "%d%%", overscan);
   setLabelIfChanged(&settingsValues[2], scaleValue);
   setLabelIfChanged(&settingsValues[3], overscanValue);
   const ContentRegion *region = getContentRegionAt(getContentRegionIndex());
   setLabelIfChanged(&settingsValues[4], region ? region->name : getContentRegionCode());
   setLabelIfChanged(&settingsValues[5], "Configure");
   setLabelIfChanged(&settingsValues[6], "Configure");

   int panelW = getSettingsPanelWidth(), panelH = getSettingsPanelHeight();
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangle(x, y, panelW, panelH, 10, activeTheme->appBg);
   fillGfxRoundedRectangle(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&settingsTitle);
   drawLabelCentered(&settingsTitle, home.screenW / 2, y + 28);

   int autoplayX = x + panelW - 40 - 132;
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      Label *name = &settingsLabels[i];
      int rowY = y + SETTINGS_ROW_TOP + i * SETTINGS_ROW_STEP;
      refreshLabelScale(name);
      setLabelColor(name, activeTheme->textPrimary);
      moveLabel(name, x + 40, getCenteredLabelY(name, rowY, SETTINGS_ROW_HEIGHT));
      drawLabel(name);

      if (i == 0 || i == 1) {
         // Both boolean settings use the same compact OFF/ON switch.
         int enabled = i == 0 ? getAutoplayEnabled() : getEnable1080p60();
         int selected = enabled ? 1 : 0;
         int toggleY = rowY + (SETTINGS_ROW_HEIGHT - 36) / 2;
         int selectedX = autoplayX + selected * 66;
         fillGfxRoundedRectangle(autoplayX, toggleY, 132, 36, 8, activeTheme->surface);
         if (i == home.settingsSelected)
            fillGfxRoundedRectangle(selectedX - 4, toggleY - 4, 74, 44, 10, 0x38FFFFFF);
         fillGfxRoundedRectangle(selectedX, toggleY, 66, 36, 8, activeTheme->accent);
         for (int option = 0; option < 2; option++) {
            Label *optionLabel = &settingsAutoplayOptions[option];
            refreshLabelScale(optionLabel);
            setLabelColor(optionLabel, option == selected ? activeTheme->appBg : activeTheme->textPrimary);
            moveLabel(optionLabel,
                      autoplayX + option * 66 + (66 - optionLabel->tt.tex.w) / 2,
                      getCenteredLabelY(optionLabel, toggleY, 36));
            drawLabel(optionLabel);
         }
         continue;
      }

      Label *value = &settingsValues[i];
      refreshLabelScale(value);
      int valuePad = 12;
      int valueW = value->tt.tex.w + valuePad * 2;
      int minValueW = 88;
      if (valueW < minValueW) valueW = minValueW;
      int valueX = x + panelW - 40 - valueW;
      int valueY = getCenteredLabelY(value, rowY, SETTINGS_ROW_HEIGHT);
      int valueH = value->tt.tex.h + 10;

      if (i == home.settingsSelected)
         fillGfxRoundedRectangle(valueX, valueY - 5, valueW, valueH, 7,
                                 activeTheme->rowHighlight);

      setLabelColor(value, i == home.settingsSelected ? activeTheme->accent : activeTheme->textPrimary);
      moveLabel(value, valueX + (valueW - value->tt.tex.w) / 2, valueY);
      drawLabel(value);
   }
   settingsHints.y = y + panelH - 42;
   drawButtonHints(&settingsHints, home.screenW);
}

static void drawHomeIconFixed(Icon *icon, int x, int y, uint32_t color)
{
   if (!icon || !icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   drawGfxTextureFixedPosition(x + icon->offsetX, y + icon->offsetY,
                               icon->tex->w, icon->tex->h, *icon->tex,
                               0.0f, 0.0f, 1.0f, 1.0f, color, GFX_FILTER_LINEAR);
}

static void drawHomeSortPills(void)
{
   int playlistMode = homeSupportsPlaylistSort();
   int count = playlistMode ? PLAYLIST_SORT_COUNT : SORT_COUNT;
   int x = homeSortGroupLeft();
   int y = titleLabel.y + (titleLabel.tt.tex.h - HOME_SORT_HEIGHT) / 2;
   if (y < titleLabel.y) y = titleLabel.y;
   for (int i = 0; i < count; i++) {
      Label *label = playlistMode ? &playlistSortLabels[i] : &homeSortLabels[i];
      int width = label->tt.tex.w + HOME_SORT_PAD_X * 2;
      int selected = playlistMode ? i == (int)home.playlistSort : i == (int)home.sort;
      uint32_t edge = selected ? activeTheme->accent : activeTheme->rowHighlight;
      uint32_t fill = selected ? activeTheme->rowHighlight : activeTheme->surface;
      fillGfxRoundedRectangleFixed(x, y, width, HOME_SORT_HEIGHT, HOME_SORT_HEIGHT / 2, edge);
      fillGfxRoundedRectangleFixed(x + 1, y + 1, width - 2, HOME_SORT_HEIGHT - 2,
                                   HOME_SORT_HEIGHT / 2 - 1, fill);
      setLabelColor(label, 0xFF202020);
      int textX = x + (width - label->tt.tex.w) / 2;
      int textY = y + (HOME_SORT_HEIGHT - label->tt.tex.h) / 2;
      drawLabelAt(label, textX - 2, textY); drawLabelAt(label, textX + 2, textY);
      drawLabelAt(label, textX - 1, textY - 1); drawLabelAt(label, textX + 1, textY - 1);
      drawLabelAt(label, textX - 1, textY + 1); drawLabelAt(label, textX + 1, textY + 1);
      setLabelColor(label, selected ? activeTheme->textPrimary : activeTheme->textSecondary);
      drawLabelAt(label, textX, textY);
      x += width + HOME_SORT_GAP;
   }
}

static void drawLocalPlaylistTabs(void)
{
   int x = titleLabel.x + titleLabel.tt.tex.w + 18;
   int right = homeSortGroupLeft() - 14;
   int pad = TREND_CHIP_PAD_X, gap = TREND_CHIP_GAP;
   for (int i = 0; i < 2; i++) refreshLabelScale(&localPlaylistTabLabels[i]);
   int total = localPlaylistTabLabels[0].tt.tex.w + localPlaylistTabLabels[1].tt.tex.w + 4 * pad + gap;
   while (x + total > right && pad > 3) { pad--; total -= 4; }
   while (x + total > right && gap > 2) { gap--; total--; }
   int y = titleLabel.y + (titleLabel.tt.tex.h - TREND_CHIP_HEIGHT) / 2;
   for (int i = 0; i < 2; i++) {
      Label *label = &localPlaylistTabLabels[i];
      int width = label->tt.tex.w + 2 * pad;
      int active = i == home.localPlaylistTab;
      uint32_t edge = active ? activeTheme->accent : activeTheme->rowHighlight;
      uint32_t fill = active ? activeTheme->rowHighlight : activeTheme->surface;
      fillGfxRoundedRectangleFixed(x, y, width, TREND_CHIP_HEIGHT, TREND_CHIP_HEIGHT / 2, edge);
      fillGfxRoundedRectangleFixed(x + 1, y + 1, width - 2, TREND_CHIP_HEIGHT - 2, TREND_CHIP_HEIGHT / 2 - 1, fill);
      setLabelColor(label, active ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(label, x + pad, getCenteredLabelY(label, y, TREND_CHIP_HEIGHT));
      drawLabel(label);
      x += width + gap;
   }
}

static void drawTrendingTabs(void)
{
   int x = titleLabel.x + titleLabel.tt.tex.w + 18;
   int right = homeSortGroupLeft() - 14;
   int gap = TREND_CHIP_GAP;
   int pad = TREND_CHIP_PAD_X;
   int iconSize = TREND_CHIP_ICON_SIZE;
   for (int i = 0; i < TREND_COUNT; i++) refreshLabelScale(&trendingChipLabels[i]);

   int active = home.category;
   int first = 0, last = TREND_COUNT - 1;
   int total = 0;
   for (int i = first; i <= last; i++)
      total += iconSize + TREND_CHIP_ICON_GAP + trendingChipLabels[i].tt.tex.w + 2 * pad +
               (i > first ? gap : 0);
   while (x + total > right && pad > 3) {
      pad--;
      total = 0;
      for (int i = first; i <= last; i++)
         total += iconSize + TREND_CHIP_ICON_GAP + trendingChipLabels[i].tt.tex.w + 2 * pad +
                  (i > first ? gap : 0);
   }
   while (x + total > right && gap > 2) {
      gap--;
      total = 0;
      for (int i = first; i <= last; i++)
         total += iconSize + TREND_CHIP_ICON_GAP + trendingChipLabels[i].tt.tex.w + 2 * pad +
                  (i > first ? gap : 0);
   }
   if (x + total > right) {
      first = last = active;
      pad = 3;
      gap = 0;
      iconSize = 0;
      if (x + trendingChipLabels[active].tt.tex.w + 2 * pad > right) return;
   }

   int chipY = titleLabel.y + (titleLabel.tt.tex.h - TREND_CHIP_HEIGHT) / 2;
   if (chipY < titleLabel.y - 4) chipY = titleLabel.y - 4;
   for (int i = first; i <= last; i++) {
      Label *label = &trendingChipLabels[i];
      int chipW = label->tt.tex.w + 2 * pad;
      if (iconSize > 0) chipW += iconSize + TREND_CHIP_ICON_GAP;
      int selected = home.category == i;
      fillGfxRectangleFixed(x, chipY, chipW, TREND_CHIP_HEIGHT,
                            selected ? activeTheme->rowHighlight : activeTheme->surface);
      if (iconSize > 0)
         drawHomeIconFixed(&trendingChipIcons[i], x + pad,
                           chipY + (TREND_CHIP_HEIGHT - TREND_CHIP_ICON_SIZE) / 2,
                           selected ? activeTheme->accent : activeTheme->textSecondary);
      moveLabel(label, x + pad + (iconSize > 0 ? iconSize + TREND_CHIP_ICON_GAP : 0),
                getCenteredLabelY(label, chipY, TREND_CHIP_HEIGHT));
      setLabelColor(label, selected ? activeTheme->accent : activeTheme->textPrimary);
      drawLabel(label);
      x += chipW + gap;
   }
}

static int liveTabsWidth(int first, int last, int pad, int gap)
{
   int width = 0;
   for (int i = first; i <= last; i++)
      width += liveChipLabels[i].tt.tex.w + 2 * pad + (i > first ? gap : 0);
   return width;
}

static void drawLiveTabs(void)
{
   int active = (int)home.liveCategory;
   int x = titleLabel.x + titleLabel.tt.tex.w + 18;
   int right = homeSupportsSort() ? homeSortGroupLeft() - 14 : home.screenW - MARGIN_X;
   int pad = TREND_CHIP_PAD_X, gap = TREND_CHIP_GAP;
   for (int i = 0; i < LIVE_COUNT; i++) refreshLabelScale(&liveChipLabels[i]);

   while (x + liveTabsWidth(0, LIVE_COUNT - 1, pad, gap) > right && pad > 4) pad--;
   while (x + liveTabsWidth(0, LIVE_COUNT - 1, pad, gap) > right && gap > 3) gap--;

   int first = 0, last = LIVE_COUNT - 1;
   if (x + liveTabsWidth(first, last, pad, gap) > right) {
      int bestCount = 0;
      first = last = active;
      for (int from = 0; from <= active; from++) {
         for (int to = active; to < LIVE_COUNT; to++) {
            int count = to - from + 1;
            if (count > bestCount && x + liveTabsWidth(from, to, pad, gap) <= right) {
               first = from;
               last = to;
               bestCount = count;
            }
         }
      }
      if (!bestCount && x + liveChipLabels[active].tt.tex.w <= right) {
         pad = 0;
         gap = 0;
      } else if (!bestCount) {
         return;
      }
   }

   int y = titleLabel.y + (titleLabel.tt.tex.h - TREND_CHIP_HEIGHT) / 2;
   for (int i = first; i <= last; i++) {
      Label *label = &liveChipLabels[i];
      int width = label->tt.tex.w + 2 * pad;
      int selected = i == active;
      fillGfxRectangleFixed(x, y, width, TREND_CHIP_HEIGHT,
                            selected ? activeTheme->rowHighlight : activeTheme->surface);
      setLabelColor(label, selected ? activeTheme->accent : activeTheme->textPrimary);
      moveLabel(label, x + pad, getCenteredLabelY(label, y, TREND_CHIP_HEIGHT));
      drawLabel(label);
      x += width + gap;
   }
}

void handleHomeSidebarSelection(int picked)
{
   switch (picked) {
      case SIDEBAR_SEARCH:
         cacheCurrent();
         openSearchPage();
         break;
      case SIDEBAR_RECOMMENDED:
         selectAccountFeed(ACCOUNT_FEED_HOME);
         break;
      case SIDEBAR_ACCOUNT_SUBSCRIPTIONS:
         selectAccountFeed(ACCOUNT_FEED_SUBSCRIPTIONS);
         break;
      case SIDEBAR_ACCOUNT_HISTORY:
         selectAccountFeed(ACCOUNT_FEED_HISTORY);
         break;
      case SIDEBAR_ACCOUNT_WATCH_LATER:
         selectAccountFeed(ACCOUNT_FEED_WATCH_LATER);
         break;
      case SIDEBAR_PLAYLISTS:
         if (isSignedIn()) switchToCategory(HOME_PLAYLISTS);
         else {
            home.pendingAccountCategory = HOME_PLAYLISTS;
            openAccountPanel();
         }
         break;
      case SIDEBAR_TRENDING:
         if (home.category >= TREND_COUNT) switchToCategory(TREND_GAMING);
         break;
      case SIDEBAR_LIVE:
         if (home.category != HOME_LIVE) {
            home.liveCategory = LIVE_POPULAR;
            switchToCategory(HOME_LIVE);
         }
         break;
      case SIDEBAR_PS3_SUBSCRIPTIONS:
         switchToCategory(HOME_SUBS);
         break;
      case SIDEBAR_PS3_HISTORY:
         switchToCategory(HOME_LOCAL_HISTORY);
         break;
      case SIDEBAR_PS3_WATCH_LATER:
         switchToCategory(HOME_WATCHLATER);
         break;
      case SIDEBAR_PS3_PLAYLISTS:
         switchToCategory(HOME_LOCAL_PLAYLISTS);
         break;
      case SIDEBAR_DOWNLOADS:
         switchToCategory(HOME_DOWNLOADS);
         break;
      case SIDEBAR_ACCOUNT:
         openAccountPanel();
         break;
      case SIDEBAR_SETTINGS:
         openHomeSettingsPanel();
         break;
      case SIDEBAR_EXIT:
         openExitConfirmation();
         break;
      default:
         break;
   }
}

static void initHome(void)
{
   home.pendingAccountCategory = -1;
   home.pendingPlaylistOpen = 0;
   home.playlistError = 0;
   home.createPlaylistOskDone = 0;
   home.playlistNameAction = 0;
   home.playlistNameTargetId[0] = 0;
   home.playlistDeleteConfirmOpen = 0;
   home.playlistDeleteFromWatchLater = 0;
   home.playlistDeleteId[0] = 0;
   home.playlistReturnToSearch = 0;
   home.playlistReturnLocal = 0;
   home.playlistReturnCategory = -1;
   home.playlistReturnSort = SORT_RELEVANCE;
   home.playlistReturnQuery[0] = 0;
   home.localPlaylistTab = 0;
   home.sort = SORT_RELEVANCE;
   home.playlistSort = PLAYLIST_SORT_NEWEST;
   squarePressedAt = 0;
   home.liveCategory = LIVE_POPULAR;
   home.settingsOpen = 0;
   home.settingsSelected = 0;
   home.downloadSettingsOpen = 0;
   home.downloadSettingsSelected = 0;
   home.exitConfirmOpen = 0;
   home.exitConfirmSelected = 1;
   home.regionOpen = 0;
   home.sponsorblockOpen = 0;
   home.sponsorblockSelected = 0;
   regionRevisionSeen = getContentRegionRevision();
   loadSdScale();
   home.screenW = getGfxScreenWidth();
   home.screenH = getGfxScreenHeight();
   sdHintsTwoRows = isGfxSdVideoOutput();
   int headerY = sdHintsTwoRows ? SD_HEADER_Y : HEADER_Y;
   int gridTop = sdHintsTwoRows ? SD_GRID_TOP : GRID_TOP;

   uiFont = openSystemFont(FONT_POP);
   initSidebar(&uiFont, home.screenW, home.screenH);
   int categoryTitleOffset = home.screenW * 3 / 100;
   initLabel(&titleLabel,  &uiFont, MARGIN_X + categoryTitleOffset, headerY,
             home.screenW - 2 * MARGIN_X - categoryTitleOffset, AUTO, TITLE_SIZE,
             activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statusLabel, &uiFont, MARGIN_X, gridTop, home.screenW - 2 * MARGIN_X, AUTO, 21, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   initLabel(&playlistErrorLabel, &uiFont, MARGIN_X, 0, home.screenW - 2 * MARGIN_X, AUTO, 18,
             activeTheme->textPrimary, TEXT_WRAP, "This playlist is private, restricted or unavailable.");
   initLabel(&playlistToastLabel, &uiFont, 0, 0, home.screenW - 80, AUTO, 23,
             activeTheme->textPrimary, TEXT_NOWRAP, "Playlist added to Watch Later");
   setLabelScaleWithUi(&playlistErrorLabel, 1);
   setLabelScaleWithUi(&playlistToastLabel, 1);
   playlistToastShownAt = 0;
   // Home heading/status grow by rerasterising their font, while retaining their
   // original anchors instead of being pushed off the 4:3 safe area.
   setLabelScaleWithUi(&titleLabel, 1);
   setLabelScaleWithUi(&statusLabel, 1);

   initButtonHints(&hints, &uiFont, 0, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS), "Play");
   homeSquareHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "Watch Later / Playlist (hold)");
   homeDownloadHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R3), "Download");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
   homeNewPlaylistHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L3), "New playlist");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT), "Search");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START), "Settings");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_L1), "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R1), "Category");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R2), "Refresh");
   homeSortHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L2), "Sort");
   setButtonHintsStableLayout(&hints, 1);
   setButtonHintGap(&hints, 14);
   homeFooterBottomY = home.screenH - (sdHintsTwoRows ? SD_HINTS_BOTTOM : HINTS_BOTTOM);
   homeFooterRows = getButtonHintsFitRows(&hints, home.screenW, MAX_HINT_ROWS);
   if (homeFooterRows < 1) homeFooterRows = 1;
   float hintGlyphFinal = HINT_GLYPH_H * (sdHintsTwoRows ? SD_MAX_SCALE : 1.0f);
   int footerRowStep = (int)(hintGlyphFinal + HINT_ROW_GAP + 0.5f);
   homeFooterTopY = homeFooterBottomY - (homeFooterRows - 1) * footerRowStep;

   int panelY = (home.screenH - getAccountPanelHeight()) / 2;
   int accountTextW = getAccountPanelWidth() - 40;
   int settingsTextW = getSettingsPanelWidth();
   initLabel(&panelTitle,  &uiFont, 0, 0, accountTextW, AUTO, 30, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelLine1,  &uiFont, 0, 0, accountTextW, AUTO, 22, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&panelLine2,  &uiFont, 0, 0, accountTextW, AUTO, 22, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&panelCode,   &uiFont, 0, 0, accountTextW, AUTO, 56, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelFooter, &uiFont, 0, 0, accountTextW, AUTO, 20, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&settingsTitle, &uiFont, 0, 0, settingsTextW - 48, AUTO, 24, activeTheme->textPrimary, TEXT_NOWRAP, "Settings");
   static const char *settingNames[SETTINGS_ROW_COUNT] = {
      "Autoplay", "Enable 1080p60 (experimental)", "UI Scaling",
      "Overscan", "Content Region", "SponsorBlock", "Download settings"
   };
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      initLabel(&settingsLabels[i], &uiFont, 0, 0, settingsTextW - 220, AUTO, 20,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, settingNames[i]);
      initLabel(&settingsValues[i], &uiFont, 0, 0, 160, AUTO, 22,
                activeTheme->textPrimary, TEXT_NOWRAP, "");
   }
   initLabel(&downloadSettingsTitle, &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, "Download settings");
   static const char *downloadSettingNames[DOWNLOAD_SETTINGS_ROW_COUNT] = {
      "Video resolution", "Audio language", "Audio source", "Primary subtitles", "Secondary subtitles (optional)"
   };
   for (int i = 0; i < DOWNLOAD_SETTINGS_ROW_COUNT; i++) {
      initLabel(&downloadSettingsLabels[i], &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W / 2 - 50, AUTO, 19,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, downloadSettingNames[i]);
      initLabel(&downloadSettingsValues[i], &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W / 2 - 40, AUTO, 19,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   }
   initLabel(&downloadSettingsNotice, &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W - 68, AUTO, 14,
             activeTheme->textSecondary, TEXT_WRAP,
             "If requested audio is unavailable, original upload audio is used; primary subtitles follow the audio actually selected.");
   initButtonHints(&downloadSettingsHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_DPAD_DOWN), "Move");
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_DPAD_LEFT), "Change");
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "Change");
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_CROSS), "Cycle");
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_CIRCLE), "Back");

   initLabel(&settingsAutoplayOptions[0], &uiFont, 0, 0, 60, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, "OFF");
   initLabel(&settingsAutoplayOptions[1], &uiFont, 0, 0, 60, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, "ON");
   initLabel(&regionTitle, &uiFont, 0, 0, getRegionPanelWidth() - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, "Content Region");
   initLabel(&regionDescription, &uiFont, 0, 0, getRegionPanelWidth() - 150, AUTO, 15,
             activeTheme->textSecondary, TEXT_NOWRAP, "Country used for YouTube content lists");
   initLabel(&regionPosition, &uiFont, 0, 0, 80, AUTO, 15,
             activeTheme->textSecondary, TEXT_NOWRAP, "1 / 33");
   for (int i = 0; i < getContentRegionCount() && i < REGION_MAX_ROWS; i++) {
      const ContentRegion *region = getContentRegionAt(i);
      initLabel(&regionNames[i], &uiFont, 0, 0, getRegionPanelWidth() - 150, AUTO, 18,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, region ? region->name : "");
      initLabel(&regionCodes[i], &uiFont, 0, 0, 48, AUTO, 18,
                activeTheme->textSecondary, TEXT_NOWRAP, region ? region->code : "");
      setLabelScaleWithUi(&regionNames[i], 1);
      setLabelScaleWithUi(&regionCodes[i], 1);
   }
   initLabel(&sponsorTitle, &uiFont, 0, 0, getSponsorPanelWidth() - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, "SponsorBlock");
   initLabel(&sponsorDescription, &uiFont, 0, 0, getSponsorPanelWidth() - 56, AUTO, 15,
             activeTheme->textSecondary, TEXT_WRAP,
             "Skip: automatically skip the segment.  Ask: press Triangle to skip it.  Keep: play it normally.");
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++) {
      initLabel(&sponsorCategoryLabels[i], &uiFont, 0, 0, 250, AUTO, 18,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, getSponsorCategoryName((SponsorCategory)i));
      setLabelScaleWithUi(&sponsorCategoryLabels[i], 1);
   }
   initLabel(&sponsorActionLabels[0], &uiFont, 0, 0, SPONSOR_OPTION_W, AUTO, 16,
             activeTheme->textPrimary, TEXT_NOWRAP, "Skip");
   initLabel(&sponsorActionLabels[1], &uiFont, 0, 0, SPONSOR_OPTION_W, AUTO, 16,
             activeTheme->textPrimary, TEXT_NOWRAP, "Ask");
   initLabel(&sponsorActionLabels[2], &uiFont, 0, 0, SPONSOR_OPTION_W, AUTO, 16,
             activeTheme->textPrimary, TEXT_NOWRAP, "Keep");
   for (int i = 0; i < 3; i++) setLabelScaleWithUi(&sponsorActionLabels[i], 1);
   initButtonHints(&sponsorHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_DPAD_UP), "Move");
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_DPAD_LEFT), "Change");
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "Change");
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_CROSS), "Cycle");
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_CIRCLE), "Back");

   initButtonHints(&regionHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_CROSS), "Choose");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_DPAD_UP), "");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_DPAD_DOWN), "Move");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_L2), "");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_R2), "Page");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_CIRCLE), "Back");
   // Use native PS3 controller glyphs instead of textual button names.
   // The three action glyphs are grouped before their shared "Change" caption.
   initButtonHints(&settingsHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_CROSS), "Open / Select");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_DPAD_LEFT), "");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "Change");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_CIRCLE), "Close");

   initLabel(&exitConfirmTitle, &uiFont, 0, 0, EXIT_CONFIRM_PANEL_W - 48, AUTO, 28,
             activeTheme->textPrimary, TEXT_NOWRAP, "Exit CRTube");
   initLabel(&exitConfirmMessage, &uiFont, 0, 0, EXIT_CONFIRM_PANEL_W - 48, AUTO, 20,
             activeTheme->textSecondary, TEXT_NOWRAP, "Are you sure you want to close the app?");
   initLabel(&exitConfirmChoices[0], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, "Yes");
   initLabel(&exitConfirmChoices[1], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, "No");
   initButtonHints(&exitConfirmHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_CROSS), "Choose");
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_DPAD_LEFT), "Change");
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "");
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_CIRCLE), "Cancel");

   // Settings is a compact control panel; keep its typography at a stable
   // physical size even when the rest of the UI uses the 1.5x SD font scale.
   for (int i = 0; i < TREND_COUNT; i++) {
      initLabel(&trendingChipLabels[i], &uiFont, 0, 0, 220, AUTO, TREND_CHIP_FONT_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP, TrendingNames[i]);
      setLabelScaleWithUi(&trendingChipLabels[i], 1);
      initIcon(&trendingChipIcons[i], TrendingIcons[i], TREND_CHIP_ICON_SIZE);
   }
   for (int i = 0; i < LIVE_COUNT; i++) {
      initLabel(&liveChipLabels[i], &uiFont, 0, 0, 180, AUTO, TREND_CHIP_FONT_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP, LiveNames[i]);
      setLabelScaleWithUi(&liveChipLabels[i], 1);
   }
   static const char *HomeSortNames[SORT_COUNT] = { "Relevance", "Views", "Newest" };
   for (int i = 0; i < SORT_COUNT; i++) {
      initLabel(&homeSortLabels[i], &uiFont, 0, 0, AUTO, AUTO, HOME_SORT_TEXT_SIZE,
                activeTheme->textSecondary, TEXT_NOWRAP, HomeSortNames[i]);
      setLabelScaleWithUi(&homeSortLabels[i], 1);
   }
   static const char *PlaylistSortNames[PLAYLIST_SORT_COUNT] = { "Newest", "A-Z" };
   static const char *LocalPlaylistTabNames[2] = { "Watch Later", "PS3" };
   for (int i = 0; i < PLAYLIST_SORT_COUNT; i++) {
      initLabel(&playlistSortLabels[i], &uiFont, 0, 0, AUTO, AUTO, HOME_SORT_TEXT_SIZE,
                activeTheme->textSecondary, TEXT_NOWRAP, PlaylistSortNames[i]);
      setLabelScaleWithUi(&playlistSortLabels[i], 1);
   }
   for (int i = 0; i < 2; i++) {
      initLabel(&localPlaylistTabLabels[i], &uiFont, 0, 0, AUTO, AUTO, TREND_CHIP_FONT_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP, LocalPlaylistTabNames[i]);
      setLabelScaleWithUi(&localPlaylistTabLabels[i], 1);
   }
   updateHomeSortHint();
   initButtonHints(&panelHints, &uiFont, panelY + getAccountPanelHeight() - 48, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   panelConfirmHint = addButtonHint(&panelHints, getConsoleGlyph(GLYPH_CROSS),  "Try again");
   panelBackHint    = addButtonHint(&panelHints, getConsoleGlyph(GLYPH_CIRCLE), "Close");

   int gridHeight = homeFooterTopY - gridTop - (sdHintsTwoRows ? SD_GRID_BOTTOM_GAP : GRID_BOTTOM_GAP);
   if (gridHeight < 120) gridHeight = 120;
   initVideoGrid(&home.grid, &uiFont, MARGIN_X, gridTop, home.screenW - 2 * MARGIN_X, gridHeight);
   setGridWatchedPredicate(&home.grid, isWatched);
   setGridWatchedPosition(&home.grid, getWatchedPosition);
   setGridDownloadedPosition(&home.grid, getDownloadedPosition);
   setGridWatchLaterPredicate(&home.grid, isWatchLater);
   home.grid.suppressWatchLaterBadge = (home.category == HOME_WATCHLATER ||
      home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER);

   subsRevisionSeen = getSubscriptionsRevision();
   watchLaterRevisionSeen = getWatchLaterRevision();
   localPlaylistRevisionSeen = getLocalPlaylistRevision();
   playlistWatchLaterRevisionSeen = getPlaylistWatchLaterRevision();
   downloadLibraryRevisionSeen = getDownloadLibraryRevision();
   accountRevisionSeen = getAccountRevision();
   // signed in, start on the account's Recommended feed; otherwise on Subscriptions, as before
   home.category = isSignedIn() ? HOME_ACCOUNT + ACCOUNT_FEED_HOME : HOME_SUBS;
   loadCategory();
   setSidebarForCategory();
   updateHomeSortHint();
}

static int handleHomeSquareAction(const SearchResult *selected)
{
   if (squarePressedAt) {
      if (isPadButtonDown(PAD_BTN_SQUARE) &&
          getTimeUs() - squarePressedAt >= SQUARE_HOLD_US) {
         squarePressedAt = 0;
         openLocalPlaylistPicker(&squarePendingItem);
         return 1;
      }
      if (isPadButtonReleased(PAD_BTN_SQUARE)) {
         toggleWatchLater(&squarePendingItem);
         watchLaterRevisionSeen = getWatchLaterRevision();
         categoryCached[HOME_WATCHLATER] = 0;
         if (home.category == HOME_WATCHLATER) loadCategory();
         squarePressedAt = 0;
         return 1;
      }
      return 1;
   }
   if (!isPadButtonPressed(PAD_BTN_SQUARE)) return 0;
   if (selected->isPlaylist) {
      if (!selected->isLocalPlaylist && addPlaylistToWatchLater(selected) == 0) {
         playlistWatchLaterRevisionSeen = getPlaylistWatchLaterRevision();
         playlistToastShownAt = getTimeUs();
         setLabelText(&playlistToastLabel, "Playlist added to Watch Later");
         if (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 0) loadCategory();
      }
      return 1;
   }
   squarePendingItem = *selected;
   squarePressedAt = getTimeUs();
   return 1;
}

static void updateHome(void)
{
   updateDownloadOverlay();
   unsigned int downloadRevision = getDownloadLibraryRevision();
   if (downloadRevision != downloadLibraryRevisionSeen) {
      downloadLibraryRevisionSeen = downloadRevision;
      if (home.category == HOME_DOWNLOADS) { loadCategory(); return; }
   }
   updateSignIn();
   if (getAccountRevision() != accountRevisionSeen) applyAccountChange();
   if (getContentRegionRevision() != regionRevisionSeen) {
      regionRevisionSeen = getContentRegionRevision();
      for (int c = 0; c < TREND_COUNT; c++) categoryCached[c] = 0;
      categoryCached[HOME_ACCOUNT + ACCOUNT_FEED_HOME] = 0;
      logInfo("[home] content region changed to %s; regional feeds invalidated\n", getContentRegionCode());
      if (home.category < TREND_COUNT || home.category == HOME_LIVE ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_HOME)
         refreshCurrentCategory();
   }
   if (home.channelLookupPending) {
      updateVideoGrid(&home.grid);   // reap the previous grid job; pauseVideoGrid prevents another one
      if (!home.channelLookupActive && !home.channelLookupDone && !home.grid.threadActive) {
         if (spawnJoinableThread(&home.channelLookupTid, channelLookupWorker, 0,
                                 THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-channel-id") == 0)
            home.channelLookupActive = 1;
         else {
            home.channelLookupResult = -1;
            home.channelLookupDone = 1;
         }
      }
      if (home.channelLookupDone) {
         if (home.channelLookupActive) {
            joinThread(home.channelLookupTid);
            home.channelLookupActive = 0;
         }
         home.channelLookupPending = 0;
         resumeVideoGrid(&home.grid);
         if (home.channelLookupResult == 0 && home.channelLookupItem.channelId[0]) {
            SearchResult channelItem = home.channelLookupItem;
            logInfo("[home] Triangle: async channel lookup resolved %s\n", channelItem.channelId);
            cacheCurrent();
            openChannelFor(&channelItem);
         } else {
            logWarn("[home] Triangle: async channel lookup failed video=%s\n",
                    home.channelLookupItem.videoId);
         }
         return;
      }
      return;
   }

   if (home.accountOpen) { updateAccountPanel(); return; }
   if (home.settingsOpen) { updateSettingsPanel(); return; }
   if (home.exitConfirmOpen) { updateExitConfirmation(); return; }
   if (home.playlistDeleteConfirmOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { home.playlistDeleteConfirmOpen = 0; return; }
      if (isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_LEFT)) || isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_RIGHT)) ||
          isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_UP)) || isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
         home.playlistDeleteConfirmSelected = 1 - home.playlistDeleteConfirmSelected;
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (home.playlistDeleteConfirmSelected == 0) {
            if (home.playlistDeleteFromWatchLater == 1) {
               removePlaylistFromWatchLater(home.playlistDeleteId);
               playlistWatchLaterRevisionSeen = getPlaylistWatchLaterRevision();
               categoryCached[HOME_LOCAL_PLAYLISTS] = 0;
            } else if (home.playlistDeleteFromWatchLater == 2) {
               deleteDownloadedVideo(home.playlistDeleteId);
            } else {
               deleteLocalPlaylist(home.playlistDeleteId);
               localPlaylistRevisionSeen = getLocalPlaylistRevision();
               categoryCached[HOME_LOCAL_PLAYLISTS] = 0;
            }
            loadCategory();
         }
         home.playlistDeleteConfirmOpen = 0;
      }
      return;
   }
   if (home.createPlaylistOskDone) {
      home.createPlaylistOskDone = 0;
      if (home.createPlaylistName[0]) {
         int result = -1;
         if (home.playlistNameAction == 2) {
            result = renameLocalPlaylist(home.playlistNameTargetId, home.createPlaylistName);
         } else if (home.playlistNameAction == 1) {
            char id[LOCAL_PLAYLIST_ID_LEN];
            result = createEmptyLocalPlaylist(home.createPlaylistName, id, sizeof id);
         }
         if (result == 0) {
            localPlaylistRevisionSeen = getLocalPlaylistRevision();
            categoryCached[HOME_LOCAL_PLAYLISTS] = 0;
            loadCategory();
         }
      }
      home.playlistNameAction = 0;
      home.playlistNameTargetId[0] = 0;
      return;
   }
   if (oskInputActive()) return;

   // Circle remains a global shortcut for the menu. LEFT only opens it when
   // the selected thumbnail is in the first column; elsewhere LEFT belongs to
   // the grid and must move the selection normally.
   if (!isSidebarActive()) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
         if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS) {
            switchToCategory(HOME_LOCAL_PLAYLISTS);
            return;
         }
         if (home.category == HOME_PLAYLIST_CONTENTS) {
            if (home.playlistReturnToSearch) {
               char query[SEARCH_QUERY_LEN];
               strCopy(query, sizeof query, home.playlistReturnQuery);
               int returnCategory = home.playlistReturnCategory;
               int returnSort = home.playlistReturnSort;
               home.playlistReturnToSearch = 0;
               home.playlistReturnLocal = 0;
               home.playlistReturnCategory = -1;
               home.playlistReturnSort = SORT_RELEVANCE;
               home.playlistReturnQuery[0] = 0;
               if (returnCategory >= 0 && returnCategory != home.category)
                  switchToCategory(returnCategory);
               openSearchQueryWithSort(query, returnSort);
            } else if (home.playlistReturnLocal) {
               switchToCategory(HOME_LOCAL_PLAYLISTS);
            } else {
               switchToCategory(HOME_PLAYLISTS);
            }
            return;
         }
         openSidebar();
         return;
      }
      if (isPadButtonPressed(PAD_BTN_LEFT) &&
          home.grid.columns > 0 && home.grid.selected >= 0 &&
          (home.grid.selected % home.grid.columns) == 0) {
         openSidebar();
         return;
      }
   }
   if (isSidebarActive()) {
      int picked = updateSidebar();
      if (picked >= 0) handleHomeSidebarSelection(picked);
      return;
   }

   if (isPadButtonPressed(PAD_BTN_SELECT)) { cacheCurrent(); openSearchPage(); return; }
   if (isPadButtonPressed(PAD_BTN_START)) { openHomeSettingsPanel(); return; }
   if (isPadButtonPressed(PAD_BTN_R2)) { refreshCurrentCategory(); return; }
   if (isPadButtonPressed(PAD_BTN_L2) && homeSupportsSort()) {
      home.sort = (SortOrder)((home.sort + 1) % SORT_COUNT);
      setSortPreference(currentFeedSortPreference(), (int)home.sort);
      refreshCurrentCategory();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_L2) && homeSupportsPlaylistSort()) {
      home.playlistSort = (PlaylistSortOrder)((home.playlistSort + 1) % PLAYLIST_SORT_COUNT);
      if (home.category != HOME_DOWNLOADS)
         setSortPreference(currentPlaylistSortPreference(), (int)home.playlistSort);
      refreshCurrentCategory();
      return;
   }

   // L1/R1 stay responsive even mid-load (the source swap is non-blocking)
   if      (isPadButtonPressed(PAD_BTN_R1)) { switchCategory(1);  return; }
   else if (isPadButtonPressed(PAD_BTN_L1)) { switchCategory(-1); return; }

   updateVideoGrid(&home.grid);
   int playlistPage = home.category == HOME_PLAYLISTS || home.category == HOME_PLAYLIST_CONTENTS ||
                      home.category == HOME_LOCAL_PLAYLISTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS ||
                      home.category == HOME_DOWNLOADS;
   if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, !playlistPage);
   if (homeSquareHint >= 0) {
      if (home.category == HOME_WATCHLATER)
         setButtonHintCaption(&hints, homeSquareHint, "Remove / Playlist (hold)");
      else if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS)
         setButtonHintCaption(&hints, homeSquareHint, "Remove");
      else if (home.category == HOME_DOWNLOADS)
         setButtonHintCaption(&hints, homeSquareHint, "Delete (hold)");
      else if (home.category == HOME_LOCAL_PLAYLISTS)
         setButtonHintCaption(&hints, homeSquareHint,
                              home.localPlaylistTab == 0 ? "Remove (hold)" : "Rename / Delete (hold)");
      else if (home.category == HOME_PLAYLISTS)
         setButtonHintCaption(&hints, homeSquareHint, "Save playlist");
      else
         setButtonHintCaption(&hints, homeSquareHint, "Watch Later / Playlist (hold)");
   }
   if (homeNewPlaylistHint >= 0)
      setButtonHintShown(&hints, homeNewPlaylistHint,
                         home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 1);
   if (home.category >= HOME_ACCOUNT && home.category < HOME_LOCAL_HISTORY &&
       gridStage(&home.grid) == GRID_READY && !home.grid.threadActive) {
      int feed = home.category - HOME_ACCOUNT;
      uint64_t now = getTimeUs();
      if (accountFeedRefreshUs[feed] &&
          now - accountFeedRefreshUs[feed] >= AccountFeedRefreshIntervals[feed]) {
         refreshCurrentCategory();
         return;
      }
   }
   if (gridStage(&home.grid) != GRID_READY) return;
   if (homeNewPlaylistHint >= 0) setButtonHintShown(&hints, homeNewPlaylistHint, home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 1);
   if (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 1 && isPadButtonPressed(PAD_BTN_L3)) {
      char defaultName[LOCAL_PLAYLIST_NAME_LEN]; snprintf(defaultName, sizeof defaultName, "Playlist %d", getLocalPlaylistCount() + 1);
      home.playlistNameAction = 1;
      home.playlistNameTargetId[0] = 0;
      oskInputBegin("New playlist", defaultName, onHomePlaylistNameEntered); return;
   }
   const SearchResult *selected = gridSelected(&home.grid);
   if (!selected) return;
   if (home.category == HOME_DOWNLOADS && selected->isLocalDownload) {
      if (squarePressedAt) {
         if (isPadButtonDown(PAD_BTN_SQUARE) && getTimeUs() - squarePressedAt >= SQUARE_HOLD_US) {
            squarePressedAt = 0;
            strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, selected->videoId);
            home.playlistDeleteFromWatchLater = 2;
            home.playlistDeleteConfirmOpen = 1;
            home.playlistDeleteConfirmSelected = 1;
            exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
            return;
         }
         if (isPadButtonReleased(PAD_BTN_SQUARE)) squarePressedAt = 0;
         return;
      }
      if (isPadButtonPressed(PAD_BTN_SQUARE)) { squarePressedAt = getTimeUs(); return; }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         char path[MAX_PATH_LEN];
         if (getDownloadedVideoPath(selected->videoId, path, sizeof path) == 0) {
            char queue[MAX_SEARCH_RESULTS][16];
            int queueCount = 0, queueIndex = -1;
            const SearchResults *results = gridResults(&home.grid);
            if (results) {
               for (int i = 0; i < results->count && queueCount < MAX_SEARCH_RESULTS; i++) {
                  const SearchResult *item = &results->items[i];
                  if (!item->isLocalDownload || !item->videoId[0]) continue;
                  strCopy(queue[queueCount], sizeof queue[queueCount], item->videoId);
                  if (strcmp(item->videoId, selected->videoId) == 0) queueIndex = queueCount;
                  queueCount++;
               }
            }
            stopVideoGrid(&home.grid);
            if (queueIndex >= 0)
               playLocalVideoWithQueue(path, queue, queueCount, queueIndex);
            else
               playLocalVideo(path);
         }
         return;
      }
      return;
   }
   if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS && isPadButtonPressed(PAD_BTN_SQUARE) && selected->videoId[0]) {
      removeVideoFromLocalPlaylist(home.localPlaylistId, selected->videoId);
      localPlaylistRevisionSeen = getLocalPlaylistRevision(); categoryCached[HOME_LOCAL_PLAYLISTS] = 0; loadCategory(); return;
   }
   if (home.category == HOME_PLAYLISTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 0);
      if (homeSquareHint >= 0) {
         setButtonHintCaption(&hints, homeSquareHint, "Save playlist");
         setButtonHintShown(&hints, homeSquareHint, 1);
      }
      if (handleHomeSquareAction(selected)) return;
      if (isPadButtonPressed(PAD_BTN_CROSS) && selected->isPlaylist && selected->playlistId[0]) {
         strCopy(home.playlistId, sizeof home.playlistId, selected->playlistId);
         strCopy(home.playlistTitle, sizeof home.playlistTitle, selected->title);
         openHomePlaylist(selected->playlistId, selected->title);
      }
      return;
   }
   if (home.category == HOME_LOCAL_PLAYLISTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 0);
      if (homeSquareHint >= 0) {
         setButtonHintCaption(&hints, homeSquareHint,
                              home.localPlaylistTab == 0 ? "Remove (hold)" : "Rename / Delete (hold)");
         setButtonHintShown(&hints, homeSquareHint, 1);
      }
      if ((home.localPlaylistTab == 0 && selected->isPlaylist && selected->playlistId[0]) ||
          (home.localPlaylistTab == 1 && selected->isLocalPlaylist && selected->localPlaylistId[0])) {
         if (squarePressedAt) {
            if (isPadButtonDown(PAD_BTN_SQUARE) && getTimeUs() - squarePressedAt >= SQUARE_HOLD_US) {
               squarePressedAt = 0;
               home.playlistDeleteFromWatchLater = (home.localPlaylistTab == 0);
               if (home.playlistDeleteFromWatchLater)
                  strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, selected->playlistId);
               else
                  strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, selected->localPlaylistId);
               home.playlistDeleteConfirmOpen = 1; home.playlistDeleteConfirmSelected = 1;
               exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0; return;
            }
            if (isPadButtonReleased(PAD_BTN_SQUARE)) {
               squarePressedAt = 0;
               if (home.localPlaylistTab == 1 && selected->isLocalPlaylist && selected->localPlaylistId[0]) {
                  home.playlistNameAction = 2;
                  strCopy(home.playlistNameTargetId, sizeof home.playlistNameTargetId, selected->localPlaylistId);
                  oskInputBegin("Rename playlist", selected->title, onHomePlaylistNameEntered);
               }
               return;
            }
            return;
         }
         if (isPadButtonPressed(PAD_BTN_SQUARE)) { squarePressedAt = getTimeUs(); return; }
      }
      if (isPadButtonPressed(PAD_BTN_CROSS) && selected->isPlaylist) {
         if (home.localPlaylistTab == 0 && selected->playlistId[0]) {
            home.playlistReturnToSearch = 0;
            home.playlistReturnLocal = 1;
            home.playlistReturnCategory = HOME_LOCAL_PLAYLISTS;
            home.localPlaylistTab = 0;
            home.playlistError = 0;
            strCopy(home.playlistId, sizeof home.playlistId, selected->playlistId);
            strCopy(home.playlistTitle, sizeof home.playlistTitle, selected->title);
            switchToCategory(HOME_PLAYLIST_CONTENTS);
         } else if (home.localPlaylistTab == 1 && selected->isLocalPlaylist && selected->localPlaylistId[0]) {
            home.playlistReturnToSearch = 0;
            home.playlistReturnLocal = 0;
            strCopy(home.localPlaylistId, sizeof home.localPlaylistId, selected->localPlaylistId);
            strCopy(home.playlistTitle, sizeof home.playlistTitle, selected->title);
            switchToCategory(HOME_LOCAL_PLAYLIST_CONTENTS);
         }
      }
      return;
   }
   if (home.category == HOME_WATCHLATER || home.category == HOME_LOCAL_PLAYLIST_CONTENTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 0);
      if (homeSquareHint >= 0) {
         setButtonHintCaption(&hints, homeSquareHint,
                              home.category == HOME_WATCHLATER ? "Remove / Playlist (hold)" : "Remove");
         setButtonHintShown(&hints, homeSquareHint, 1);
      }
   } else if (home.category == HOME_PLAYLIST_CONTENTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 0);
      if (homeSquareHint >= 0) setButtonHintShown(&hints, homeSquareHint, 0);
   } else if (homeSquareHint >= 0) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 1);
      setButtonHintCaption(&hints, homeSquareHint, "Watch Later / Playlist (hold)");
      setButtonHintShown(&hints, homeSquareHint, 1);
   }
   if (handleHomeSquareAction(selected)) return;
   if (home.category != HOME_PLAYLIST_CONTENTS && home.category != HOME_LOCAL_PLAYLIST_CONTENTS &&
       isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) enqueueDownload(selected); return; }
   // Recommended tiles don't say which channel they are from
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      logInfo("[diag-triangle] home page=%d selected=%d videoId=%s channelIdLen=%d authorLen=%d\n",
         home.category, home.grid.selected, selected->videoId,
         (int)strlen(selected->channelId), (int)strlen(selected->author));
      SearchResult channelItem = *selected;
      if (!channelItem.channelId[0]) {
         logInfo("[home] Triangle: scheduling async channel lookup for video=%s\n", channelItem.videoId);
         home.channelLookupItem = channelItem;
         home.channelLookupResult = -1;
         home.channelLookupDone = 0;
         home.channelLookupActive = 0;
         home.channelLookupPending = 1;
         pauseVideoGrid(&home.grid);
         return;
      }

      logInfo("[diag-triangle] home cacheCurrent begin\n");
      cacheCurrent();
      logInfo("[diag-triangle] home cacheCurrent done; openChannelFor begin\n");
      openChannelFor(&channelItem);
      logInfo("[diag-triangle] home openChannelFor returned\n");
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatched(selected->videoId);
      stopVideoGrid(&home.grid);   // no per-call http fetch may overlap the http media stream during playback
      playVideo(selected->videoId);
   }
}

static void drawHome(void)
{
   // Explicit, change-only refresh: never allocate/rebuild text textures from
   // inside drawLabel itself.
   refreshLabelScale(&titleLabel);
   refreshLabelScale(&statusLabel);
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->appBg);
   if (home.category == HOME_PLAYLIST_CONTENTS && home.playlistError) {
      refreshLabelScale(&playlistErrorLabel);
      int statusX = (home.screenW - playlistErrorLabel.tt.tex.w) / 2;
      int statusY = (home.screenH - playlistErrorLabel.tt.tex.h) / 2;
      fillGfxRoundedRectangleFixed(statusX - 18, statusY - 9,
                                   playlistErrorLabel.tt.tex.w + 36, playlistErrorLabel.tt.tex.h + 18,
                                   9, activeTheme->badgeFill);
      drawLabelAt(&playlistErrorLabel, statusX, statusY);
   } else if (gridStage(&home.grid) == GRID_READY) {
      drawVideoGrid(&home.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&home.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }
   // Draw the header after the grid so the category chips remain above any
   // selection glow that reaches into the header band.
   drawLabel(&titleLabel);
   if (home.category < TREND_COUNT) drawTrendingTabs();
   else if (home.category == HOME_LIVE) drawLiveTabs();
   else if (home.category == HOME_LOCAL_PLAYLISTS) drawLocalPlaylistTabs();
   if (homeSupportsSort() || homeSupportsPlaylistSort()) drawHomeSortPills();
   drawButtonHintsFitted(&hints, home.screenW, homeFooterBottomY, HINT_ROW_GAP, MAX_HINT_ROWS);
   drawDownloadOverlay(home.screenW);
   if (!home.accountOpen && !home.settingsOpen && !home.exitConfirmOpen) drawSidebar();
   if (home.accountOpen) drawAccountPanel();
   if (home.settingsOpen) {
      if (home.downloadSettingsOpen) drawDownloadSettingsPanel();
      else {
         drawSettingsPanel();
         if (home.regionOpen) drawRegionPicker();
         else if (home.sponsorblockOpen) drawSponsorBlockPanel();
      }
   }
   if (home.exitConfirmOpen) drawExitConfirmation();
   if (playlistToastShownAt && getTimeUs() - playlistToastShownAt < 1000000ULL &&
       !home.accountOpen && !home.settingsOpen && !home.exitConfirmOpen && !home.playlistDeleteConfirmOpen) {
      refreshLabelScale(&playlistToastLabel);
      int toastX = (home.screenW - playlistToastLabel.tt.tex.w) / 2;
      int toastY = (home.screenH - playlistToastLabel.tt.tex.h) / 2;
      fillGfxRoundedRectangleFixed(toastX - 20, toastY - 12,
                                   playlistToastLabel.tt.tex.w + 40, playlistToastLabel.tt.tex.h + 24,
                                   10, activeTheme->badgeFill);
      drawLabelAt(&playlistToastLabel, toastX, toastY);
   }
   if (home.playlistDeleteConfirmOpen) {
      setLabelText(&exitConfirmTitle, home.playlistDeleteFromWatchLater == 1 ? "Remove playlist" :
                   home.playlistDeleteFromWatchLater == 2 ? "Delete download" : "Delete playlist");
      setLabelText(&exitConfirmMessage, home.playlistDeleteFromWatchLater == 1 ? "Remove this playlist from Watch Later?" :
                   home.playlistDeleteFromWatchLater == 2 ? "Delete this downloaded video from the HDD?" :
                   "Delete this playlist and its videos?");
      setLabelText(&exitConfirmChoices[0], "Yes"); setLabelText(&exitConfirmChoices[1], "No");
      int saved = home.exitConfirmSelected; home.exitConfirmSelected = home.playlistDeleteConfirmSelected;
      drawExitConfirmation(); home.playlistDeleteConfirmSelected = home.exitConfirmSelected; home.exitConfirmSelected = saved;
      setLabelText(&exitConfirmTitle, "Exit CRTube");
      setLabelText(&exitConfirmMessage, "Are you sure you want to close the app?");
   }
}

static void suspendHome(void)
{
   squarePressedAt = 0;
   stopVideoGrid(&home.grid);   // free the http pools for the pushed screen
}

// returning from search/play: if the load was interrupted mid-flight (worker stopped), restart it; an
// already-loaded grid is kept as-is (instant, thumbnails intact).
static void resumeHome(void)
{
   if (home.category == HOME_DOWNLOADS) {
      loadCategory();
      return;
   }
   if (home.category == HOME_LIVE) {
      uint64_t age = getTimeUs() - home.liveLoadedUs;
      if (gridStage(&home.grid) != GRID_READY || age >= LIVE_MAX_AGE_US) loadCategory();
      return;
   }
   if (home.category == HOME_LOCAL_HISTORY) {
      loadCategory();
      return;
   }
   // a subscribe/unsubscribe while we were away makes the cached Subscriptions feed stale - drop it and,
   // if it's the current tab, refetch now.
   if (getSubscriptionsRevision() != subsRevisionSeen) {
      subsRevisionSeen = getSubscriptionsRevision();
      categoryCached[HOME_SUBS] = 0;
      if (home.category == HOME_SUBS) { loadCategory(); return; }
   }
   if (getWatchLaterRevision() != watchLaterRevisionSeen) {
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) { loadCategory(); return; }
   }
   if (getLocalPlaylistRevision() != localPlaylistRevisionSeen ||
       getPlaylistWatchLaterRevision() != playlistWatchLaterRevisionSeen) {
      localPlaylistRevisionSeen = getLocalPlaylistRevision();
      playlistWatchLaterRevisionSeen = getPlaylistWatchLaterRevision();
      if (home.category == HOME_LOCAL_PLAYLISTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS) {
         loadCategory();
         return;
      }
   }
   if (gridStage(&home.grid) != GRID_READY) loadCategory();
}

static void termHome(void)
{
   if (home.channelLookupActive) {
      joinThread(home.channelLookupTid);
      home.channelLookupActive = 0;
   }
   termVideoGrid(&home.grid);
   termSidebar();
   termButtonHints(&hints);
   termButtonHints(&panelHints);
   freeLabel(&titleLabel);
   freeLabel(&statusLabel);
   freeLabel(&playlistErrorLabel);
   freeLabel(&playlistToastLabel);
   freeLabel(&panelTitle);
   freeLabel(&panelLine1);
   freeLabel(&panelLine2);
   freeLabel(&panelCode);
   freeLabel(&panelFooter);
   freeLabel(&settingsTitle);
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      freeLabel(&settingsLabels[i]);
      freeLabel(&settingsValues[i]);
   }
   freeLabel(&downloadSettingsTitle);
   for (int i = 0; i < DOWNLOAD_SETTINGS_ROW_COUNT; i++) {
      freeLabel(&downloadSettingsLabels[i]);
      freeLabel(&downloadSettingsValues[i]);
   }
   freeLabel(&downloadSettingsNotice);
   termButtonHints(&downloadSettingsHints);
   freeLabel(&settingsAutoplayOptions[0]);
   freeLabel(&settingsAutoplayOptions[1]);
   termButtonHints(&settingsHints);
   freeLabel(&exitConfirmTitle);
   freeLabel(&exitConfirmMessage);
   freeLabel(&exitConfirmChoices[0]);
   freeLabel(&exitConfirmChoices[1]);
   termButtonHints(&exitConfirmHints);
   freeLabel(&regionTitle);
   freeLabel(&regionDescription);
   freeLabel(&regionPosition);
   for (int i = 0; i < REGION_MAX_ROWS; i++) { freeLabel(&regionNames[i]); freeLabel(&regionCodes[i]); }
   termButtonHints(&regionHints);
   for (int i = 0; i < TREND_COUNT; i++) {
      freeLabel(&trendingChipLabels[i]);
      freeIcon(&trendingChipIcons[i]);
   }
   for (int i = 0; i < LIVE_COUNT; i++) freeLabel(&liveChipLabels[i]);
   for (int i = 0; i < SORT_COUNT; i++) freeLabel(&homeSortLabels[i]);
   for (int i = 0; i < PLAYLIST_SORT_COUNT; i++) freeLabel(&playlistSortLabels[i]);
   for (int i = 0; i < 2; i++) freeLabel(&localPlaylistTabLabels[i]);
   closeFont(&uiFont);
}

void openHome(void) { changeScreen(&homeScreen); }

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
