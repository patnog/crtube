// home - the landing screen: trending categories in a VideoGrid (see home.h).

#include "app-config.h"
#include "localization.h"
#include "app.h"
#include "screens/home.h"

/* Exported by the PSL1GHT process module. */
extern void sysProcessExit(int status);
#include "screens/search.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "settings.h"
#include "updater.h"
#include "screens/playlist-picker.h"
#include "extractor.h"
#include "http.h"
#include "vfs.h"
#include "account.h"
#include "downloads.h"
#include "sponsorblock.h"
#include "sidebar.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"
#include "ui/loading-indicator.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "thread.h"             // getTimeUs
#include "font.h"
#include "ui/label.h"
#include "ui/scrollbar.h"
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
#define HINTS_BOTTOM     42       // standard single-row hint top offset from the screen bottom
#define SD_HINTS_BOTTOM  76       // lower hint-row top offset; keeps 2.0x glyphs inside TV safe area
#define SD_HINT_ROW_GAP   8
#define GRID_BOTTOM_GAP  24
#define SD_GRID_BOTTOM_GAP 20
#define MAX_HINT_ROWS 2
#define HINT_ROW_GAP 8
#define SD_MAX_SCALE    1.5f
#define SD_HEADER_Y      54
#define SD_GRID_TOP     132
#define HINT_GLYPH_H  28
#define HINT_TEXT     22

static const AppStringId TrendingNameIds[TREND_COUNT] = { STR_GAMING, STR_SPORTS, STR_PODCASTS };
static const AppStringId LiveNameIds[LIVE_COUNT] = { STR_POPULAR, STR_MUSIC, STR_GAMING, STR_SPORTS, STR_PODCASTS, STR_NEWS };
static const AppStringId AccountNameIds[ACCOUNT_FEED_COUNT] = { STR_RECOMMENDED, STR_SUBSCRIPTIONS, STR_HISTORY, STR_WATCH_LATER };

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
static int           downloadPlaylistRevisionSeen;
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
   int       restoreGridAfterWatchLater;
   int       restoreGridSelection;
   int       restoreGridScrollRow;
   LiveCategory liveCategory;
   uint64_t  liveLoadedUs;
   int       screenW, screenH;
   float     uiScale;
   int       accountOpen;   // the sign-in / account panel is showing
   int       settingsOpen;  // the settings window is showing
   int       settingsSelected;
   int       themeOpen;
   int       themeSelected;
   int       themeCustomOpen;
   int       themeCustomSelected;
   int       themeCustomDark;
   int       themeCustomDarkCursor;
   int       themeCustomAccentIndex;
   int       themeCustomAccentCursor;
   int       themeConfirmOpen;
   int       themeConfirmSelected;
   int       themePendingPreset;
   int       downloadSettingsOpen;
   int       downloadSettingsSelected;
   int       exitConfirmOpen;
   int       exitConfirmSelected; // 0 = Yes, 1 = No (safe default)
   int       playlistDeleteConfirmOpen;
   int       playlistDeleteConfirmSelected;
   int       playlistDeleteFromWatchLater;
   int       downloadManagerOpen;
   int       downloadManagerSelected;
   int       downloadManagerScroll;
   int       downloadManagerCategory;
   int       downloadCategory; // 0 = All, 1 = Videos, 2 = Playlists
   int       downloadManagerSelectedCount;
   char      downloadManagerSelectedIds[DOWNLOAD_MANAGEMENT_MAX_ITEMS][16];
   int       downloadManagerConfirmOpen;
   char      downloadManagerMessage[96];
   int       downloadPlaylistConfirmOpen;
   SearchResult downloadPlaylistPending;
   uint64_t  downloadPlaylistEstimatedBytes;
   int       downloadPlaylistEstimateUnknown;
   uint64_t  playlistDeleteBytes;
   int       playlistDeleteVideoCount;
   char      playlistDeleteId[64]; // YouTube playlist IDs are longer than local playlist IDs.
   int       createPlaylistOskDone;
   int       playlistNameAction; // 1 = create, 2 = rename
   char      playlistNameTargetId[LOCAL_PLAYLIST_ID_LEN];
   char      createPlaylistName[LOCAL_PLAYLIST_NAME_LEN];
   int       regionOpen;
   int       regionPickerLanguage;
   int       languageConfirmOpen;
   int       languageConfirmSelected;
   int       languagePending;
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
   char      playlistReturnChannelId[32];
   char      playlistReturnChannelName[48];
   int       playlistReturnChannelCategory;
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
   HttpRequestGroup channelLookupGroup;
   int channelLookupResult;
} home;

static Font        uiFont;
static Label       titleLabel, statusLabel, playlistErrorLabel, playlistToastLabel;
static uint64_t    homeLoadingStartedUs;
static Label       downloadSpacePrefixLabel, downloadSpaceAvailableLabel;
static int         downloadSpaceAvailableLow;
static Label       downloadManagerTitle, downloadManagerRows[10], downloadManagerStatuses[7], downloadManagerFooter;
static Label       downloadCategoryLabels[3], downloadPlaylistConfirmTitle, downloadPlaylistConfirmFooter;
static ButtonHints downloadManagerHints;
static int downloadManagerSquareHint = -1;
static uint64_t    playlistToastShownAt;
static int         playbackRestorePending;
static sys_ppu_thread_t downloadPlaylistThread;
static volatile int downloadPlaylistThreadActive, downloadPlaylistThreadDone;
static volatile int downloadPlaylistThreadResult;
static SearchResult *downloadPlaylistResults;
static int downloadPlaylistOverlayRunning; // confirmation overlay is on the screen stack
static int downloadPlaylistPreparing;
static int downloadPlaylistPreparedCount;
static int downloadPlaylistMissingCount;
static void restoreHomePlaybackSelection(void);
static ButtonHints hints;

// Search opens Settings as a lightweight overlay screen. Keeping Search on the
// screen stack avoids termSearch() joining its grid worker just to show Settings.
static void initSearchSettingsOverlay(void);
static void updateSearchSettingsOverlay(void);
static void drawSearchSettingsOverlay(void);
static void termSearchSettingsOverlay(void);
static Screen searchSettingsOverlayScreen = { initSearchSettingsOverlay, NULL,
   updateSearchSettingsOverlay, drawSearchSettingsOverlay, NULL,
   termSearchSettingsOverlay, SCREEN_TERMINATED };
static void initDownloadPlaylistConfirmOverlay(void);
static void updateDownloadPlaylistConfirmOverlay(void);
static void drawDownloadPlaylistConfirmOverlay(void);
static Screen downloadPlaylistConfirmOverlayScreen = { initDownloadPlaylistConfirmOverlay, NULL,
   updateDownloadPlaylistConfirmOverlay, drawDownloadPlaylistConfirmOverlay, NULL, NULL, SCREEN_TERMINATED };
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
#define SETTINGS_PANEL_W_LARGE 820
#define SETTINGS_PANEL_H 672
#define SETTINGS_ROW_HEIGHT 44
#define SETTINGS_ROW_TOP 62
#define SETTINGS_ROW_STEP 50
#define SETTINGS_ROW_COUNT 12
static Label settingsTitle, settingsLabels[SETTINGS_ROW_COUNT], settingsValues[SETTINGS_ROW_COUNT];
static ButtonHints settingsHints;
#define UPDATE_PANEL_W 680
#define UPDATE_PANEL_H 430
#define UPDATE_DIALOG_W 640
#define UPDATE_DIALOG_H 360
#define UPDATE_NOTES_W 760
#define UPDATE_NOTES_H 600
#define UPDATE_NOTES_MAX_LINES 256
#define UPDATE_NOTES_VISIBLE_LINES 17
#define UPDATE_NOTES_LINE_STEP 27
static int updateSettingsOpen;
static int updateSettingsSelected;
static int updateOverwritePromptOpen;
static int updateOverwriteNewRelease;
static int updateOverwriteChoice;
static int updateDialogOpen;
static int updateDialogSelected;
static int updateDialogDismissed;
static int updateNotesOpen;
static int updateNotesScroll;
static int updateNotesLineCount;
static Label updateNotesLines[UPDATE_NOTES_VISIBLE_LINES];
static Scrollbar updateNotesScrollbar;
static void buildUpdateNotesLines(void);
static char updateNotesLineText[UPDATE_NOTES_MAX_LINES][LABEL_MAX_TEXT];
static Label updatePanelTitle, updatePanelInfo[3], updatePanelRows[6];
static Label updateDialogTitle, updateDialogVersion, updateDialogRows[6];
static Label updateNotesTitle, updateNotesPageLabel;
static ButtonHints updateHints;

#define THEME_PANEL_W 860
#define THEME_PANEL_H 620
#define THEME_EDIT_CUSTOM_ROW THEME_PRESET_COUNT
#define THEME_ROW_COUNT (THEME_PRESET_COUNT + 1)
#define THEME_ROW_TOP 126
#define THEME_ROW_STEP 56
static Label themeTitle, themeRowLabels[THEME_ROW_COUNT], themeLegendLabels[2];
static Label themeSelectionLeft, themeSelectionRight;
static Label themeCustomTitle, themeCustomLabels[3], themeCustomOptions[2];
static Label themeConfirmTitle, themeConfirmMessage, themeConfirmChoices[2];
static ButtonHints themeHints;
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
static ButtonRepeat settingsOverscanRepeat, settingsCropOverscanRepeat, settingsScreensaverRepeat;
static ButtonRepeat exitConfirmNavigateRepeat;
static Label exitConfirmTitle, exitConfirmMessage, exitConfirmChoices[2];
static ButtonHints exitConfirmHints;
#define EXIT_CONFIRM_PANEL_W 560
#define EXIT_CONFIRM_PANEL_H 240
#define REGION_MAX_ROWS 40
#define REGION_PANEL_W 620
#define REGION_PANEL_H 440
#define REGION_ROW_H 34
#define REGION_LIST_TOP 96
#define REGION_FOOTER_H 52
static Label regionTitle, regionDescription, regionPosition;
static Label regionNames[REGION_MAX_ROWS], regionCodes[REGION_MAX_ROWS];
static ButtonHints regionHints;
static ButtonRepeat regionNavigateRepeat, regionPageRepeat;
static Scrollbar regionScrollbar;
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
static int homeL1Hint = -1;
static int homeR1Hint = -1;
static int homeNewPlaylistHint = -1;
static SearchResult squarePendingItem;
static uint64_t squarePressedAt;
#define SQUARE_HOLD_US 550000ULL
static const IconId TrendingIcons[TREND_COUNT] = { ICON_MICROCHIP, ICON_STAR, ICON_VOLUME_UP };

static int isLargeUiScale(void);

static int getSettingsPanelWidth(void)
{
   int preferred = isLargeUiScale() ? SETTINGS_PANEL_W_LARGE : SETTINGS_PANEL_W;
   int width = home.screenW - 48;
   return width < preferred ? width : preferred;
}

static int getSettingsPanelHeight(void)
{
   int preferred = isLargeUiScale() ? 700 : SETTINGS_PANEL_H;
   int height = home.screenH - 40;
   return height < preferred ? height : preferred;
}

static int homeThemeIsDark(void)
{
   uint32_t color = activeTheme->appBg;
   int r = (color >> 16) & 0xFF;
   int g = (color >> 8) & 0xFF;
   int b = color & 0xFF;
   return (r * 299 + g * 587 + b * 114) / 1000 < 128;
}

static uint32_t homeNotificationBackground(void)
{
   return homeThemeIsDark() ? activeTheme->badgeFill : 0xFFFFFFFF;
}

static void setHintsThemeColor(ButtonHints *bar, uint32_t color)
{
   if (!bar) return;
   bar->captionColor = color;
   for (int i = 0; i < bar->count; i++)
      setLabelColor(&bar->hints[i].caption, color);
}

static void refreshHomeThemeColors(void)
{
   setLabelColor(&titleLabel, activeTheme->textPrimary);
   setLabelColor(&statusLabel, activeTheme->textSecondary);
   setLabelColor(&playlistErrorLabel, activeTheme->textPrimary);
   setLabelColor(&playlistToastLabel, activeTheme->textPrimary);
   setLabelColor(&downloadSpacePrefixLabel, activeTheme->textSecondary);
   setLabelColor(&downloadSpaceAvailableLabel,
                 downloadSpaceAvailableLow ? 0xFFFF4444 : activeTheme->textSecondary);
   setLabelColor(&downloadManagerTitle, activeTheme->textPrimary);
   setLabelColor(&downloadPlaylistConfirmTitle, activeTheme->textPrimary);
   setLabelColor(&downloadPlaylistConfirmFooter, activeTheme->textSecondary);
   for (int i = 0; i < 3; i++) setLabelColor(&downloadCategoryLabels[i], activeTheme->textSecondary);
   for (int i = 0; i < 10; i++)
      setLabelColor(&downloadManagerRows[i], activeTheme->textPrimary);
   for (int i = 0; i < 7; i++)
      setLabelColor(&downloadManagerStatuses[i], activeTheme->textSecondary);
   setLabelColor(&downloadManagerFooter, activeTheme->textSecondary);
   setLabelColor(&panelTitle, activeTheme->textPrimary);
   setLabelColor(&panelLine1, activeTheme->textSecondary);
   setLabelColor(&panelLine2, activeTheme->textPrimary);
   setLabelColor(&panelCode, activeTheme->textPrimary);
   setLabelColor(&panelFooter, activeTheme->textSecondary);
   setLabelColor(&settingsTitle, activeTheme->textPrimary);
   setLabelColor(&updatePanelTitle, activeTheme->textPrimary);
   for (int i = 0; i < 3; i++) setLabelColor(&updatePanelInfo[i], activeTheme->textSecondary);
   for (int i = 0; i < 5; i++) setLabelColor(&updatePanelRows[i], activeTheme->textPrimary);
   setLabelColor(&updatePanelRows[5], activeTheme->textSecondary);
   setLabelColor(&updateDialogTitle, activeTheme->textPrimary);
   setLabelColor(&updateDialogVersion, activeTheme->textSecondary);
   for (int i = 0; i < 6; i++) setLabelColor(&updateDialogRows[i], activeTheme->textPrimary);
   setLabelColor(&updateNotesTitle, activeTheme->textPrimary);
   for (int i = 0; i < UPDATE_NOTES_VISIBLE_LINES; i++) setLabelColor(&updateNotesLines[i], activeTheme->textPrimary);
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      setLabelColor(&settingsLabels[i], activeTheme->textPrimary);
      setLabelColor(&settingsValues[i], activeTheme->textPrimary);
   }
   for (int i = 0; i < 2; i++) setLabelColor(&settingsAutoplayOptions[i], activeTheme->textPrimary);
   setLabelColor(&downloadSettingsTitle, activeTheme->textPrimary);
   for (int i = 0; i < DOWNLOAD_SETTINGS_ROW_COUNT; i++) {
      setLabelColor(&downloadSettingsLabels[i], activeTheme->textPrimary);
      setLabelColor(&downloadSettingsValues[i], activeTheme->textPrimary);
   }
   setLabelColor(&downloadSettingsNotice, activeTheme->textSecondary);
   setLabelColor(&sponsorTitle, activeTheme->textPrimary);
   setLabelColor(&sponsorDescription, activeTheme->textSecondary);
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++) setLabelColor(&sponsorCategoryLabels[i], activeTheme->textPrimary);
   for (int i = 0; i < 3; i++) setLabelColor(&sponsorActionLabels[i], activeTheme->textPrimary);
   setLabelColor(&exitConfirmTitle, activeTheme->textPrimary);
   setLabelColor(&exitConfirmMessage, activeTheme->textSecondary);
   for (int i = 0; i < 2; i++) setLabelColor(&exitConfirmChoices[i], activeTheme->textPrimary);
   setLabelColor(&regionTitle, activeTheme->textPrimary);
   setLabelColor(&regionDescription, activeTheme->textSecondary);
   setLabelColor(&regionPosition, activeTheme->textSecondary);
   for (int i = 0; i < REGION_MAX_ROWS; i++) {
      setLabelColor(&regionNames[i], activeTheme->textPrimary);
      setLabelColor(&regionCodes[i], activeTheme->textSecondary);
   }
   for (int i = 0; i < TREND_COUNT; i++) setLabelColor(&trendingChipLabels[i], activeTheme->textPrimary);
   for (int i = 0; i < LIVE_COUNT; i++) setLabelColor(&liveChipLabels[i], activeTheme->textPrimary);
   for (int i = 0; i < SORT_COUNT; i++) setLabelColor(&homeSortLabels[i], activeTheme->textSecondary);
   for (int i = 0; i < PLAYLIST_SORT_COUNT; i++) setLabelColor(&playlistSortLabels[i], activeTheme->textSecondary);
   for (int i = 0; i < 2; i++) setLabelColor(&localPlaylistTabLabels[i], activeTheme->textPrimary);
   setLabelColor(&themeTitle, activeTheme->textPrimary);
   for (int i = 0; i < THEME_ROW_COUNT; i++) setLabelColor(&themeRowLabels[i], activeTheme->textPrimary);
   for (int i = 0; i < 2; i++) setLabelColor(&themeLegendLabels[i], activeTheme->textSecondary);
   setLabelColor(&themeSelectionLeft, activeTheme->accent);
   setLabelColor(&themeSelectionRight, activeTheme->accent);
   setLabelColor(&themeCustomTitle, activeTheme->textPrimary);
   for (int i = 0; i < 3; i++) setLabelColor(&themeCustomLabels[i], activeTheme->textPrimary);
   for (int i = 0; i < 2; i++) setLabelColor(&themeCustomOptions[i], activeTheme->textPrimary);
   setLabelColor(&themeConfirmTitle, activeTheme->textPrimary);
   setLabelColor(&themeConfirmMessage, activeTheme->textSecondary);
   for (int i = 0; i < 2; i++) setLabelColor(&themeConfirmChoices[i], activeTheme->textPrimary);

   setHintsThemeColor(&hints, activeTheme->textSecondary);
   setHintsThemeColor(&panelHints, activeTheme->textSecondary);
   setHintsThemeColor(&settingsHints, activeTheme->textSecondary);
   setHintsThemeColor(&downloadSettingsHints, activeTheme->textSecondary);
   setHintsThemeColor(&themeHints, activeTheme->textSecondary);
   setHintsThemeColor(&exitConfirmHints, activeTheme->textSecondary);
   setHintsThemeColor(&regionHints, activeTheme->textSecondary);
   setHintsThemeColor(&sponsorHints, activeTheme->textSecondary);
   setHintsThemeColor(&downloadManagerHints, activeTheme->textSecondary);
   refreshSidebarThemeColors();
   refreshVideoGridThemeColors(&home.grid);
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
   setHttpRequestGroup(&home.channelLookupGroup);
   home.channelLookupResult = getVideoChannelId(home.channelLookupItem.videoId,
                                                 home.channelLookupItem.channelId,
                                                 sizeof home.channelLookupItem.channelId);
   if (home.channelLookupResult == 0 && home.channelLookupItem.channelId[0]) {
      char path[MAX_PATH_LEN];
      // Resolve the path instead of trusting the list's isLocalDownload flag:
      // a download can be opened from a playlist whose cached item predates it.
      if (getDownloadedVideoPath(home.channelLookupItem.videoId, path, sizeof path) == 0) {
         int saveResult = saveDownloadedChannelId(path, home.channelLookupItem.channelId);
         logInfo("[home] saved downloaded channel metadata video=%s result=%d\n",
                 home.channelLookupItem.videoId, saveResult);
      }
   }
   __sync_synchronize();
   setHttpRequestGroup(NULL);
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
   if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS) {
      int playlistRead = home.playlistReturnCategory == HOME_DOWNLOADS ?
         getDownloadPlaylistVideos(home.localPlaylistId, out) :
         getLocalPlaylistVideos(home.localPlaylistId, out);
      if (playlistRead < 0) return -1;
      if (home.playlistReturnCategory == HOME_DOWNLOADS) {
         int kept = 0;
         for (int i = 0; i < out->count; i++) {
            char path[MAX_PATH_LEN];
            if (getDownloadedVideoPath(out->items[i].videoId, path, sizeof path) == 0) {
               out->items[i].isLocalDownload = 1;
               if (!out->items[i].channelId[0])
                  loadDownloadedChannelId(path, out->items[i].channelId, sizeof out->items[i].channelId);
               out->items[kept++] = out->items[i];
            }
         }
         out->count = kept;
      }
      return 0;
   }
   if (home.category == HOME_DOWNLOADS) {
      if (home.downloadCategory == 2) return getDownloadPlaylists(out) >= 0 ? 0 : -1;
      if (getDownloadedVideos(out, home.playlistSort == PLAYLIST_SORT_AZ) < 0) return -1;
      markDownloadPlaylistMembership(out);
      if (home.downloadCategory == 1) return 0; // Videos includes both loose and playlist-owned downloads.

      // All shows only loose videos plus playlist tiles. Videos that belong to a
      // Downloads playlist remain in Videos and inside their playlist, not twice in All.
      int kept = 0;
      for (int i = 0; i < out->count; i++)
         if (!out->items[i].isDownloadPlaylistVideo) out->items[kept++] = out->items[i];
      out->count = kept;

      SearchResults *playlists = (SearchResults *)malloc(sizeof *playlists);
      if (playlists) {
         if (getDownloadPlaylists(playlists) >= 0) {
            for (int i = 0; i < playlists->count && out->count < MAX_SEARCH_RESULTS; i++)
               out->items[out->count++] = playlists->items[i];
         }
         free(playlists);
      }

      // Sort the mixed list as one timeline, so playlist downloads participate in Newest.
      for (int i = 1; i < out->count; i++) {
         SearchResult item = out->items[i];
         int j = i;
         while (j > 0) {
            int before;
            if (home.playlistSort == PLAYLIST_SORT_AZ) {
               const unsigned char *a = (const unsigned char *)item.title;
               const unsigned char *b = (const unsigned char *)out->items[j - 1].title;
               int comparison = 0;
               while (*a && *b) {
                  unsigned char ca = *a++, cb = *b++;
                  if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
                  if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
                  if (ca != cb) { comparison = ca < cb ? -1 : 1; break; }
               }
               if (!comparison) comparison = (*a != 0) - (*b != 0);
               before = comparison < 0;
            } else {
               before = item.downloadedAt > out->items[j - 1].downloadedAt;
            }
            if (!before) break;
            out->items[j] = out->items[j - 1];
            j--;
         }
         out->items[j] = item;
      }
      return 0;
   }
   if (home.category == HOME_PLAYLIST_CONTENTS) {
      int firstPage = token == NULL || token[0] == 0;
      int rc = getAccountPlaylistVideos(home.playlistId, token, out);
      // A continuation that yields no new items is the normal end-of-playlist signal.
      // Only the initial browse can make the whole playlist screen an access error.
      if (firstPage) {
         home.playlistError = rc < 0 ? rc : 0;
         if (rc == 0 && out->count == 0)
            home.playlistError = PLAYLIST_FETCH_UNAVAILABLE;
      }
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
   const char *local = isSignedIn() ? LOC(STR_THIS_PS3_SUFFIX) : "";
   char heading[128];
   if      (home.category == HOME_LOCAL_HISTORY) snprintf(heading, sizeof heading, LOC(STR_HISTORY_THIS_PS3_FORMAT), local);
   else if (home.category >= HOME_ACCOUNT && home.category < HOME_LOCAL_HISTORY)
      snprintf(heading, sizeof heading, "%s", LOC(AccountNameIds[home.category - HOME_ACCOUNT]));
   else if (home.category == HOME_SUBS)       snprintf(heading, sizeof heading, LOC(STR_SUBSCRIPTIONS_THIS_PS3_FORMAT), local);
   else if (home.category == HOME_WATCHLATER) snprintf(heading, sizeof heading, LOC(STR_WATCH_LATER_THIS_PS3_FORMAT), local);
   else if (home.category == HOME_LIVE)       snprintf(heading, sizeof heading, "%s", LOC(STR_LIVE));
   else if (home.category == HOME_PLAYLISTS || home.category == HOME_LOCAL_PLAYLISTS)
      snprintf(heading, sizeof heading, "%s", LOC(STR_PLAYLISTS));
   else if (home.category == HOME_PLAYLIST_CONTENTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS)
      snprintf(heading, sizeof heading, "%s", home.playlistTitle[0] ? home.playlistTitle : LOC(STR_PLAYLIST_FALLBACK));
   else if (home.category == HOME_DOWNLOADS) snprintf(heading, sizeof heading, "%s", LOC(STR_DOWNLOADS));
   else                                       snprintf(heading, sizeof heading, "%s", LOC(STR_TRENDING));
   setLabelText(&titleLabel, heading);
}

static int homeSupportsSort(void)
{
   return home.category == HOME_SUBS || home.category < TREND_COUNT ||
          home.category == HOME_LIVE ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_HOME ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_SUBSCRIPTIONS;
}

static int homeIsPlaylistContents(void)
{
   return home.category == HOME_PLAYLIST_CONTENTS ||
          home.category == HOME_LOCAL_PLAYLIST_CONTENTS ||
          home.category == HOME_WATCHLATER ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER;
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
   if (homeIsPlaylistContents()) {
      setButtonHintCaption(&hints, homeSortHint, LOC(STR_SHUFFLE));
      setButtonHintShown(&hints, homeSortHint, 1);
   } else {
      setButtonHintCaption(&hints, homeSortHint, LOC(STR_SORT));
      setButtonHintShown(&hints, homeSortHint, homeSupportsSort() || homeSupportsPlaylistSort());
   }
}

// Keep category navigation hints derived from the active page, never from the
// page visited before it. Playlist contents have no category navigation.
static void updateHomeCategoryHints(void)
{
   int inPlaylistContents = home.category == HOME_PLAYLIST_CONTENTS ||
                            home.category == HOME_LOCAL_PLAYLIST_CONTENTS;
   int showCategoryHints = !inPlaylistContents;
   if (homeL1Hint >= 0) setButtonHintShown(&hints, homeL1Hint, showCategoryHints);
   if (homeR1Hint >= 0) setButtonHintShown(&hints, homeR1Hint, showCategoryHints);
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
      home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER ||
      home.category == HOME_DOWNLOADS ||
      (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 0));
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
   if (home.category == HOME_DOWNLOADS ||
       (home.category == HOME_LOCAL_PLAYLIST_CONTENTS && home.playlistReturnCategory == HOME_DOWNLOADS)) setSidebarCurrent(SIDEBAR_DOWNLOADS);
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
   // Recommended is the public home feed when signed out; the other account
   // feeds still require OAuth and retain the existing sign-in prompt.
   if (isSignedIn() || feed == ACCOUNT_FEED_HOME) {
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
   updateHomeCategoryHints();
   if (category == HOME_DOWNLOADS) {
      // Only request work here; HDD enumeration and legacy-file migration happen
      // on the low-priority library worker, never on the screen transition path.
      requestDownloadManagementRefresh();
   }
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
   strCopy(home.playlistTitle, sizeof home.playlistTitle, title && title[0] ? title : LOC(STR_PLAYLIST_FALLBACK));
   home.pendingPlaylistOpen = 0;
   if (home.category == HOME_PLAYLIST_CONTENTS) {
      loadCategory();
      setSidebarForCategory();
   } else {
      switchToCategory(HOME_PLAYLIST_CONTENTS);
   }
}

void openHomePlaylistFromSearch(const char *playlistId, const char *title, const char *query, int sort,
                                const char *channelId, const char *channelName, int channelCategory)
{
   if (!playlistId || !playlistId[0]) return;
   home.playlistReturnToSearch = 1;
   home.playlistReturnLocal = 0;
   home.playlistReturnCategory = home.category;
   home.playlistReturnSort = sort;
   strCopy(home.playlistReturnQuery, sizeof home.playlistReturnQuery, query ? query : "");
   strCopy(home.playlistReturnChannelId, sizeof home.playlistReturnChannelId, channelId ? channelId : "");
   strCopy(home.playlistReturnChannelName, sizeof home.playlistReturnChannelName, channelName ? channelName : "");
   home.playlistReturnChannelCategory = channelCategory;
   home.playlistError = 0;
   strCopy(home.playlistId, sizeof home.playlistId, playlistId);
   strCopy(home.playlistTitle, sizeof home.playlistTitle, title && title[0] ? title : LOC(STR_PLAYLIST_FALLBACK));
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
   updateHomeCategoryHints();
   if (home.category == HOME_DOWNLOADS) {
      home.downloadCategory = (home.downloadCategory + 3 + delta) % 3;
      loadCategory();
      updateHomeSortHint();
      return;
   }
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
   updateHomeCategoryHints();
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
   updateHomeCategoryHints();
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

   const char *title = LOC(STR_SIGN_IN_TO_YOUTUBE), *line1 = "", *line2 = "", *code = "", *confirm = NULL;
   char url[96] = "", footer[64] = "";
   if (isSignedIn()) {
      title   = LOC(STR_SIGNED_IN_TO_YOUTUBE);
      line1   = LOC(STR_SIGNIN_ACCOUNT_FEEDS);
      line2   = LOC(STR_SIGNIN_ACCOUNT_FEEDS_LINE2);
      confirm = LOC(STR_SIGN_OUT);
   } else if (status.stage == SIGNIN_WAITING) {
      // "https://www.google.com/device" reads better without the scheme
      const char *shown = strstr(status.verificationUrl, "://");
      strCopy(url, sizeof url, shown ? shown + 3 : status.verificationUrl);
      line1 = LOC(STR_OPEN_ON_DEVICE);
      line2 = url;
      code  = status.userCode;
      snprintf(footer, sizeof footer, LOC(STR_ENTER_CODE_EXPIRES), status.secondsLeft / 60, status.secondsLeft % 60);
   } else if (status.stage == SIGNIN_DENIED)  { line1 = LOC(STR_SIGNIN_DECLINED); confirm = LOC(STR_TRY_AGAIN); }
   else if (status.stage == SIGNIN_EXPIRED)   { line1 = LOC(STR_SIGNIN_CODE_EXPIRED);    confirm = LOC(STR_TRY_AGAIN); }
   else if (status.stage == SIGNIN_FAILED)    { line1 = LOC(STR_GOOGLE_CONNECTION_FAILED); confirm = LOC(STR_TRY_AGAIN); }
   else                                       { line1 = LOC(STR_GETTING_SIGNIN_CODE); }

   setLabelIfChanged(&panelTitle, title);
   setLabelIfChanged(&panelLine1, line1);
   setLabelIfChanged(&panelLine2, line2);
   setLabelIfChanged(&panelCode, code);
   setLabelIfChanged(&panelFooter, footer);
   setButtonHintShown(&panelHints, panelConfirmHint, confirm != NULL);
   if (confirm && strcmp(panelHints.hints[panelConfirmHint].caption.text, confirm) != 0)
      setButtonHintCaption(&panelHints, panelConfirmHint, confirm);
   const char *back = (status.stage == SIGNIN_STARTING || status.stage == SIGNIN_WAITING) ? LOC(STR_CANCEL) : LOC(STR_CLOSE);
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

static int getDownloadManagerPanelWidth(void)
{
   // home.screenW/home.screenH describe the virtual canvas (1440x1080 or 1920x1080).
   int isWide = home.screenW * 3 > home.screenH * 4;
   int width = home.screenW * (isWide ? 78 : 86) / 100;
   if (width > 1500) width = 1500;

   // Keep the panel inside the virtual canvas with a consistent safe margin.
   int maxWidth = home.screenW - 48;
   if (width > maxWidth) width = maxWidth;
   return width;
}

static int isLargeUiScale(void)
{
   return getGfxUiScaleFactor() >= 1.25f;
}

static int getRegionRowHeight(void)
{
   return isLargeUiScale() ? 48 : REGION_ROW_H;
}

static int getSponsorRowHeight(void)
{
   return isLargeUiScale() ? 48 : SPONSOR_ROW_H;
}

static int getSponsorOptionWidth(void)
{
   return isLargeUiScale() ? 108 : SPONSOR_OPTION_W;
}

static int getSponsorOptionHeight(void)
{
   return isLargeUiScale() ? 42 : SPONSOR_OPTION_H;
}

static int getRegionPanelWidth(void)
{
   int preferred = isLargeUiScale() ? 760 : REGION_PANEL_W;
   int width = home.screenW - 48;
   return width < preferred ? width : preferred;
}

static int getRegionPanelHeight(void)
{
   int preferred = isLargeUiScale() ? 520 : REGION_PANEL_H;
   int height = home.screenH - 48;
   return height < preferred ? height : preferred;
}

static int getActivePickerCount(void)
{
   return home.regionPickerLanguage ? APP_LANGUAGE_COUNT : getContentRegionCount();
}

static int getActivePickerSelection(void)
{
   return home.regionPickerLanguage ? getApplicationLanguage() : getContentRegionIndex();
}

static const char *getActivePickerName(int index)
{
   if (home.regionPickerLanguage) return getAppLanguageName(index);
   const ContentRegion *region = getContentRegionAt(index);
   return region ? region->name : "";
}

static const char *getActivePickerCode(int index)
{
   if (home.regionPickerLanguage)
      return index == APP_LANGUAGE_PT_BR ? "pt-BR" : "en";
   const ContentRegion *region = getContentRegionAt(index);
   return region ? region->code : "";
}

static void clampRegionTop(void)
{
   int maxTop = getActivePickerCount() - home.regionVisible;
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
   home.regionPickerLanguage = 0;
   int count = getActivePickerCount();
   if (count <= 0) return;
   home.regionOpen = 1;
   home.regionSelected = getActivePickerSelection();
   int panelH = getRegionPanelHeight();
   int listTop = isLargeUiScale() ? 112 : REGION_LIST_TOP;
   int available = panelH - listTop - REGION_FOOTER_H;
   home.regionVisible = available / getRegionRowHeight();
   if (home.regionVisible < 1) home.regionVisible = 1;
   if (home.regionVisible > count) home.regionVisible = count;
   home.regionTop = home.regionSelected - home.regionVisible / 2;
   clampRegionTop();
   uint64_t now = sys_time_get_system_time();
   regionNavigateRepeat.timer = now; regionNavigateRepeat.repeats = 0;
   regionPageRepeat.timer = now; regionPageRepeat.repeats = 0;
}

static void openAppLanguagePicker(void)
{
   home.regionPickerLanguage = 1;
   int count = getActivePickerCount();
   if (count <= 0) return;
   home.regionOpen = 1;
   home.regionSelected = getActivePickerSelection();
   int panelH = getRegionPanelHeight();
   int listTop = isLargeUiScale() ? 112 : REGION_LIST_TOP;
   int available = panelH - listTop - REGION_FOOTER_H;
   home.regionVisible = available / getRegionRowHeight();
   if (home.regionVisible < 1) home.regionVisible = 1;
   if (home.regionVisible > count) home.regionVisible = count;
   home.regionTop = home.regionSelected - home.regionVisible / 2;
   clampRegionTop();
   uint64_t now = sys_time_get_system_time();
   regionNavigateRepeat.timer = now; regionNavigateRepeat.repeats = 0;
   regionPageRepeat.timer = now; regionPageRepeat.repeats = 0;
}

static void updateLanguageConfirm(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      home.languageConfirmOpen = 0;
      return;
   }
   if (isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_LEFT)) ||
       isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_RIGHT)) ||
       isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_UP)) ||
       isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.languageConfirmSelected = 1 - home.languageConfirmSelected;

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (home.languageConfirmSelected == 0)
         setApplicationLanguage(home.languagePending);
      home.languageConfirmOpen = 0;
   }
}

static void drawLanguageConfirm(void)
{
   int panelW = isLargeUiScale() ? 700 : 620;
   int panelH = isLargeUiScale() ? 300 : 258;
   if (panelW > home.screenW - 48) panelW = home.screenW - 48;
   if (panelH > home.screenH - 48) panelH = home.screenH - 48;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;
   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, 0x88000000);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);

   setLabelIfChanged(&themeConfirmTitle, LOC(STR_APP_LANGUAGE));
   setLabelIfChanged(&themeConfirmMessage, LOC(STR_APPLY_APP_LANGUAGE_QUESTION));
   refreshLabelScale(&themeConfirmTitle);
   refreshLabelScale(&themeConfirmMessage);
   moveLabel(&themeConfirmTitle, x + (panelW - themeConfirmTitle.tt.tex.w) / 2, y + 30);
   drawLabel(&themeConfirmTitle);
   moveLabel(&themeConfirmMessage, x + (panelW - themeConfirmMessage.tt.tex.w) / 2, y + 96);
   drawLabel(&themeConfirmMessage);

   int buttonW = 136, buttonGap = 32;
   int buttonGroupW = buttonW * 2 + buttonGap;
   int buttonStartX = x + (panelW - buttonGroupW) / 2;
   int buttonY = y + (panelH - 48) / 2 + 30;
   for (int i = 0; i < 2; i++) {
      int bx = buttonStartX + i * (buttonW + buttonGap);
      int by = buttonY;
      int selected = i == home.languageConfirmSelected;
      fillGfxRoundedRectangleFixed(bx, by, buttonW, 48, 9,
                                   selected ? activeTheme->accent : activeTheme->surface);
      Label *choice = &themeConfirmChoices[i];
      setLabelIfChanged(choice, i == 0 ? LOC(STR_YES) : LOC(STR_NO));
      refreshLabelScale(choice);
      setLabelColor(choice, selected ? activeTheme->appBg : activeTheme->textPrimary);
      moveLabel(choice, bx + (buttonW - choice->tt.tex.w) / 2,
                getCenteredLabelY(choice, by, 48));
      drawLabel(choice);
   }
}

static void updateRegionPicker(void)
{
   int count = getActivePickerCount();
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
      if (home.regionPickerLanguage) {
         home.regionOpen = 0;
         if (home.regionSelected != getApplicationLanguage()) {
            home.languagePending = home.regionSelected;
            home.languageConfirmSelected = 1;
            home.languageConfirmOpen = 1;
            exitConfirmNavigateRepeat.timer = sys_time_get_system_time();
            exitConfirmNavigateRepeat.repeats = 0;
         }
      } else if (setContentRegionIndex(home.regionSelected) == 0) {
         home.regionOpen = 0;
      }
   }
}

static void drawRegionPicker(void)
{
   int panelW = getRegionPanelWidth(), panelH = getRegionPanelHeight();
   int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2;
   int count = getActivePickerCount();
   int current = getActivePickerSelection();
   int rowHeight = getRegionRowHeight();
   char position[24];

   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);

   setLabelIfChanged(&regionTitle, home.regionPickerLanguage ? LOC(STR_APP_LANGUAGE) : LOC(STR_CONTENT_REGION));
   setLabelIfChanged(&regionDescription, home.regionPickerLanguage ? LOC(STR_APP_LANGUAGE_DESCRIPTION) : LOC(STR_CONTENT_REGION_DESCRIPTION));
   refreshLabelScale(&regionTitle);
   refreshLabelScale(&regionDescription);
   refreshLabelScale(&regionPosition);
   drawLabelCentered(&regionTitle, x + panelW / 2, y + (isLargeUiScale() ? 22 : 27));
   moveLabel(&regionDescription, x + 28, y + (isLargeUiScale() ? 66 : 52));
   setLabelColor(&regionDescription, activeTheme->textSecondary);
   drawLabel(&regionDescription);
   snprintf(position, sizeof position, "%d / %d", home.regionSelected + 1, count);
   setLabelIfChanged(&regionPosition, position);
   moveLabel(&regionPosition, x + panelW - 32 - regionPosition.tt.tex.w, y + 74);
   setLabelColor(&regionPosition, activeTheme->textSecondary);
   drawLabel(&regionPosition);

   int listTop = isLargeUiScale() ? 112 : REGION_LIST_TOP;
   int end = home.regionTop + home.regionVisible;
   if (end > count) end = count;
   for (int row = home.regionTop; row < end; row++) {
      int rowY = y + listTop + (row - home.regionTop) * rowHeight;
      int selected = row == home.regionSelected;
      int inUse = row == current;
      setLabelIfChanged(&regionNames[row], getActivePickerName(row));
      setLabelIfChanged(&regionCodes[row], getActivePickerCode(row));
      refreshLabelScale(&regionNames[row]);
      refreshLabelScale(&regionCodes[row]);
      int labelY = getCenteredLabelY(&regionNames[row], rowY, rowHeight - 3);
      if (selected)
         fillGfxRoundedRectangleFixed(x + 18, labelY - 5, panelW - 54,
                                      regionNames[row].tt.tex.h + 10, 7, activeTheme->rowHighlight);
      if (inUse)
         fillGfxRoundedRectangleFixed(x + 28, rowY + 7, 3, rowHeight - 17, 2, activeTheme->accent);
      setLabelColor(&regionNames[row], inUse ? activeTheme->accent :
                    (selected ? activeTheme->textPrimary : activeTheme->textSecondary));
      setLabelColor(&regionCodes[row], selected ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(&regionNames[row], x + 44, labelY);
      moveLabel(&regionCodes[row], x + panelW - 32 - regionCodes[row].tt.tex.w,
                getCenteredLabelY(&regionCodes[row], rowY, rowHeight - 3));
      drawLabel(&regionNames[row]);
      drawLabel(&regionCodes[row]);
   }

   int trackY = y + listTop;
   int trackH = home.regionVisible * rowHeight;
   initScrollbar(&regionScrollbar, x + panelW - 24, trackY, 8, trackH, 5, 18,
                 activeTheme->surface, activeTheme->accent);
   drawScrollbar(&regionScrollbar, count, home.regionVisible, home.regionTop);
   regionHints.y = y + panelH - 40;
   drawButtonHints(&regionHints, home.screenW);
}

static int getSponsorPanelWidth(void)
{
   int preferred = isLargeUiScale() ? 900 : SPONSOR_PANEL_W;
   int width = home.screenW - 40;
   return width < preferred ? width : preferred;
}

static int getSponsorPanelHeight(void)
{
   int preferred = isLargeUiScale() ? 600 : SPONSOR_PANEL_H;
   int height = home.screenH - 40;
   return height < preferred ? height : preferred;
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
   setLabelText(&sponsorDescription, LOC(STR_SPONSORBLOCK_DESCRIPTION));
   refreshLabelScale(&sponsorDescription);
   setLabelColor(&sponsorDescription, activeTheme->textSecondary);
   moveLabel(&sponsorDescription, x + (panelW - sponsorDescription.tt.tex.w) / 2, y + 56);
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
      int categoryDot = isLargeUiScale() ? 14 : 10;
      fillGfxRoundedRectangleFixed(x + 29, rowY + rowHeight / 2 - categoryDot / 2, categoryDot, categoryDot, categoryDot / 2,
                                    getSponsorCategoryColor((SponsorCategory)row));
      Label *name = &sponsorCategoryLabels[row];
      setLabelText(name, getSponsorCategoryName((SponsorCategory)row));
      refreshLabelScale(name);
      setLabelColor(name, focused ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(name, x + (isLargeUiScale() ? 58 : 52), getCenteredLabelY(name, rowY, rowHeight - 2));
      drawLabel(name);

      int chosen = getSponsorAction(row);
      for (int option = 0; option < 3; option++) {
         int ox = optionX + option * (optionWidth + SPONSOR_OPTION_GAP);
         int selected = chosen == actionValues[option];
         uint32_t bg = selected ? (focused ? activeTheme->accent : activeTheme->surface) : activeTheme->surface;
         int optionY = rowY + (rowHeight - optionHeight) / 2;
         fillGfxRoundedRectangleFixed(ox, optionY, optionWidth, optionHeight, 7, bg);
         Label *word = &sponsorActionLabels[option];
         setLabelText(word, option == 0 ? LOC(STR_SKIP) : option == 1 ? LOC(STR_ASK) : LOC(STR_KEEP));
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


void openHomeSettingsPanelFromSearch(void)
{
   // Do not pop Search: termSearch() tears down its grid and waits for the
   // network worker. Push a lightweight Settings overlay instead, preserving
   // the exact channel/category/query and allowing the grid worker to finish.
   home.settingsOpen = 1;
   home.settingsSelected = 0;
   home.downloadSettingsOpen = 0;
   settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
   pushScreen(&searchSettingsOverlayScreen);
}

static const char *downloadResolutionName(int value)
{
   const char *names[] = { LOC(STR_AUTO_QUALITY), LOC(STR_RESOLUTION_480P), LOC(STR_RESOLUTION_720P), LOC(STR_RESOLUTION_1080P) };
   return value >= 0 && value < 4 ? names[value] : names[0];
}

static const char *downloadLanguageName(const char *code, int originalAllowed)
{
   if (originalAllowed && strcmp(code, "original") == 0) return LOC(STR_ORIGINAL_UPLOAD);
   if (strcmp(code, "off") == 0) return LOC(STR_OFF);
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

static void loadSavedCustomThemeValues(void)
{
   home.themeCustomDark = getCustomThemeBackgroundDark();
   home.themeCustomDarkCursor = home.themeCustomDark;
   uint32_t current = getCustomThemeAccent();
   home.themeCustomAccentIndex = 0;
   int count = getThemeAccentPaletteCount();
   for (int i = 0; i < count; i++) {
      if (getThemeAccentPaletteColor(i) == current) {
         home.themeCustomAccentIndex = i;
         break;
      }
   }
   home.themeCustomAccentCursor = home.themeCustomAccentIndex;
}

static void openCustomThemeEditor(void)
{
   home.themeCustomOpen = 1;
   home.themeCustomSelected = 0;
   loadSavedCustomThemeValues();
   settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
}

static void updateThemePanel(void)
{
   if (home.themeConfirmOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
         home.themeConfirmOpen = 0;
         return;
      }
      if (isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_LEFT)) ||
          isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_RIGHT)) ||
          isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_UP)) ||
          isRepeatDue(&exitConfirmNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
         home.themeConfirmSelected = 1 - home.themeConfirmSelected;
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (home.themeConfirmSelected == 0) {
            if (home.themePendingPreset == THEME_PRESET_CUSTOM) {
               applyCustomTheme(home.themeCustomDark,
                                getThemeAccentPaletteColor(home.themeCustomAccentIndex));
            } else {
               applyThemePreset(home.themePendingPreset);
            }
            refreshHomeThemeColors();
            home.themeOpen = 0;
            home.themeCustomOpen = 0;
         }
         home.themeConfirmOpen = 0;
      }
      return;
   }

   if (home.themeCustomOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
         home.themeCustomOpen = 0;
         return;
      }
      int paletteUp = isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP));
      int paletteDown = isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN));
      int paletteLeft = isPadButtonPressed(PAD_BTN_LEFT);
      int paletteRight = isPadButtonPressed(PAD_BTN_RIGHT);
      if (home.themeCustomSelected == 1) {
         const int columns = 9;
         int row = home.themeCustomAccentCursor / columns;
         int col = home.themeCustomAccentCursor % columns;
         int count = getThemeAccentPaletteCount();
         if (paletteUp) {
            if (row > 0) home.themeCustomAccentCursor -= columns;
            else home.themeCustomSelected = 0;
         } else if (paletteDown) {
            if (row < (count - 1) / columns)
               home.themeCustomAccentCursor =
                  home.themeCustomAccentCursor + columns < count ?
                  home.themeCustomAccentCursor + columns : count - 1;
            else
               home.themeCustomSelected = 2;
         } else if (paletteLeft && col > 0) {
            home.themeCustomAccentCursor--;
         } else if (paletteRight && col < columns - 1 &&
                    home.themeCustomAccentCursor + 1 < count) {
            home.themeCustomAccentCursor++;
         }
         if (isPadButtonPressed(PAD_BTN_CROSS)) {
            home.themeCustomAccentIndex = home.themeCustomAccentCursor;
            saveCustomThemeSettings(home.themeCustomDark,
                                    getThemeAccentPaletteColor(home.themeCustomAccentIndex));
         }
      } else {
         if (home.themeCustomSelected == 0 && paletteDown)
            home.themeCustomSelected = 1;
         else if (home.themeCustomSelected == 2 && paletteUp)
            home.themeCustomSelected = 1;

         if (home.themeCustomSelected == 0) {
            if (paletteLeft || paletteRight)
               home.themeCustomDarkCursor = !home.themeCustomDarkCursor;
            if (isPadButtonPressed(PAD_BTN_CROSS)) {
               home.themeCustomDark = home.themeCustomDarkCursor;
               saveCustomThemeSettings(home.themeCustomDark,
                                       getThemeAccentPaletteColor(home.themeCustomAccentIndex));
            }
         }
      }
      if (home.themeCustomSelected == 2 && isPadButtonPressed(PAD_BTN_CROSS)) {
         home.themePendingPreset = THEME_PRESET_CUSTOM;
         home.themeConfirmOpen = 1;
         home.themeConfirmSelected = 1;
         exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
      }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
      home.themeOpen = 0;
      return;
   }
   if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.themeSelected = (home.themeSelected + 1) % THEME_ROW_COUNT;
   else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      home.themeSelected = (home.themeSelected + THEME_ROW_COUNT - 1) % THEME_ROW_COUNT;

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (home.themeSelected == THEME_EDIT_CUSTOM_ROW) {
         openCustomThemeEditor();
      } else {
         if (home.themeSelected == THEME_PRESET_CUSTOM)
            loadSavedCustomThemeValues();
         home.themePendingPreset = home.themeSelected;
         home.themeConfirmOpen = 1;
         home.themeConfirmSelected = 1;
         exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
      }
   }
}

static void drawThemeListPanel(void)
{
   int panelW = THEME_PANEL_W;
   int panelH = THEME_PANEL_H;
   if (panelW > home.screenW - 32) panelW = home.screenW - 32;
   if (panelH > home.screenH - 32) panelH = home.screenH - 32;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;

   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&themeTitle);
   drawLabelCentered(&themeTitle, home.screenW / 2, y + 28);

   int bgX = x + panelW - 206;
   int accentX = x + panelW - 112;
   refreshLabelScale(&themeLegendLabels[0]);
   refreshLabelScale(&themeLegendLabels[1]);
   drawLabelCentered(&themeLegendLabels[0], bgX + 27, y + 88);
   drawLabelCentered(&themeLegendLabels[1], accentX + 27, y + 88);

   int activeThemeRow = getActiveThemePreset();
   for (int i = 0; i < THEME_ROW_COUNT; i++) {
      int rowY = y + THEME_ROW_TOP + i * THEME_ROW_STEP;
      if (rowY + THEME_ROW_STEP > y + panelH - 48) break;
      int selected = i == home.themeSelected;
      int activeRow = i == activeThemeRow;
      if (selected)
         fillGfxRoundedRectangleFixed(x + 20, rowY, panelW - 40, 54, 8, activeTheme->rowHighlight);

      Label *name = &themeRowLabels[i];
      refreshLabelScale(name);
      setLabelColor(name, selected ? activeTheme->accent : activeTheme->textPrimary);

      if (i != THEME_EDIT_CUSTOM_ROW) {
         uint32_t bgColor, accentColor;
         getThemePresetPreview(i, &bgColor, &accentColor);
         int swatchY = rowY + 12;
         fillGfxRoundedRectangleFixed(bgX, swatchY, 54, 30, 5, activeTheme->focusBorder);
         fillGfxRoundedRectangleFixed(bgX + 2, swatchY + 2, 50, 26, 4, bgColor);
         fillGfxRoundedRectangleFixed(accentX, swatchY, 54, 30, 5, activeTheme->focusBorder);
         fillGfxRoundedRectangleFixed(accentX + 2, swatchY + 2, 50, 26, 4, accentColor);
      }
      if (activeRow && i != THEME_EDIT_CUSTOM_ROW) {
         refreshLabelScale(&themeSelectionLeft);
         int indicatorGap = 12;
         int leftIndicatorX = x + 22;
         moveLabel(&themeSelectionLeft, leftIndicatorX,
                   getCenteredLabelY(&themeSelectionLeft, rowY, 54));
         moveLabel(name, leftIndicatorX + themeSelectionLeft.tt.tex.w + indicatorGap,
                   getCenteredLabelY(name, rowY, 54));
         drawLabel(&themeSelectionLeft);

         refreshLabelScale(&themeSelectionRight);
         int rightIndicatorX = accentX + 54 + indicatorGap;
         int rightMaxX = x + panelW - 22 - themeSelectionRight.tt.tex.w;
         if (rightIndicatorX > rightMaxX) rightIndicatorX = rightMaxX;
         moveLabel(&themeSelectionRight, rightIndicatorX,
                   getCenteredLabelY(&themeSelectionRight, rowY, 54));
         drawLabel(&themeSelectionRight);
      } else {
         moveLabel(name, x + 42, getCenteredLabelY(name, rowY, 54));
      }
      drawLabel(name);
   }

   themeHints.y = y + panelH - 36;
   drawButtonHints(&themeHints, home.screenW);
}

static void drawThemeCustomPanel(void)
{
   int panelW = THEME_PANEL_W;
   int panelH = THEME_PANEL_H;
   if (panelW > home.screenW - 32) panelW = home.screenW - 32;
   if (panelH > home.screenH - 32) panelH = home.screenH - 32;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;

   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&themeCustomTitle);
   drawLabelCentered(&themeCustomTitle, home.screenW / 2, y + 28);

   int rowY = y + 132;
   refreshLabelScale(&themeCustomLabels[0]);
   setLabelColor(&themeCustomLabels[0], home.themeCustomSelected == 0 ? activeTheme->accent : activeTheme->textPrimary);
   moveLabel(&themeCustomLabels[0], x + 44, getCenteredLabelY(&themeCustomLabels[0], rowY, 52));
   drawLabel(&themeCustomLabels[0]);

   for (int i = 0; i < 2; i++) {
      int bx = x + panelW - 310 + i * 126;
      int chosen = home.themeCustomDark == (i == 0);
      int cursor = home.themeCustomDarkCursor == (i == 0);
      int focused = home.themeCustomSelected == 0;
      fillGfxRoundedRectangleFixed(bx, rowY + 4, 108, 44, 8, activeTheme->surface);
      if (chosen)
         fillGfxRoundedRectangleFixed(bx - 3, rowY + 1, 114, 50, 10, activeTheme->accent);
      if (cursor && focused)
         fillGfxRoundedRectangleFixed(bx - 6, rowY - 2, 120, 56, 12, activeTheme->focusBorder);
      fillGfxRoundedRectangleFixed(bx + 3, rowY + 7, 102, 38, 7,
                                   chosen ? activeTheme->accent : activeTheme->surface);
      refreshLabelScale(&themeCustomOptions[i]);
      setLabelColor(&themeCustomOptions[i], chosen ? activeTheme->appBg : activeTheme->textPrimary);
      moveLabel(&themeCustomOptions[i], bx + (108 - themeCustomOptions[i].tt.tex.w) / 2,
                getCenteredLabelY(&themeCustomOptions[i], rowY + 4, 44));
      drawLabel(&themeCustomOptions[i]);
   }

   rowY = y + 236;
   refreshLabelScale(&themeCustomLabels[1]);
   setLabelColor(&themeCustomLabels[1], home.themeCustomSelected == 1 ? activeTheme->accent : activeTheme->textPrimary);
   moveLabel(&themeCustomLabels[1], x + 44, rowY);
   drawLabel(&themeCustomLabels[1]);

   int paletteCount = getThemeAccentPaletteCount();
   int square = 40, gap = 10, columns = 9;
   int paletteW = columns * square + (columns - 1) * gap;
   int paletteX = x + (panelW - paletteW) / 2;
   int paletteY = y + 286;
   for (int i = 0; i < paletteCount; i++) {
      int col = i % columns, row = i / columns;
      int bx = paletteX + col * (square + gap);
      int by = paletteY + row * (square + gap);
      int chosen = i == home.themeCustomAccentIndex;
      int cursor = i == home.themeCustomAccentCursor;
      int focused = home.themeCustomSelected == 1;
      fillGfxRoundedRectangleFixed(bx - 3, by - 3, square + 6, square + 6, 8, activeTheme->focusBorder);
      if (chosen)
         fillGfxRoundedRectangleFixed(bx - 5, by - 5, square + 10, square + 10, 9, activeTheme->accent);
      if (cursor && focused)
         fillGfxRoundedRectangleFixed(bx - 7, by - 7, square + 14, square + 14, 10, activeTheme->focusBorder);
      fillGfxRoundedRectangleFixed(bx, by, square, square, 6, getThemeAccentPaletteColor(i));
   }

   int applyY = y + 440;
   int applyW = 300, applyH = 48;
   int applyX = x + (panelW - applyW) / 2;
   int applySelected = home.themeCustomSelected == 2;
   fillGfxRoundedRectangleFixed(applyX, applyY, applyW, applyH, 9,
                           applySelected ? activeTheme->accent : activeTheme->surface);
   refreshLabelScale(&themeCustomLabels[2]);
   setLabelColor(&themeCustomLabels[2], applySelected ? activeTheme->appBg : activeTheme->textPrimary);
   moveLabel(&themeCustomLabels[2], applyX + (applyW - themeCustomLabels[2].tt.tex.w) / 2,
             getCenteredLabelY(&themeCustomLabels[2], applyY, applyH));
   drawLabel(&themeCustomLabels[2]);

   themeHints.y = y + panelH - 36;
   drawButtonHints(&themeHints, home.screenW);
}

static void drawThemeConfirmPanel(void)
{
   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, 0x88000000);
   int panelW = isLargeUiScale() ? 700 : 620;
   int panelH = isLargeUiScale() ? 300 : 258;
   if (panelW > home.screenW - 48) panelW = home.screenW - 48;
   if (panelH > home.screenH - 48) panelH = home.screenH - 48;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&themeConfirmTitle);
   drawLabelCentered(&themeConfirmTitle, x + panelW / 2, y + 30);

   char message[96];
   snprintf(message, sizeof message, LOC(STR_APPLY_THEME_FORMAT),
            home.themePendingPreset == THEME_PRESET_CUSTOM ? LOC(STR_CUSTOM_LOWER) :
            (home.themePendingPreset == 0 ? LOC(STR_THEME_CRTUBE) :
             home.themePendingPreset == 1 ? LOC(STR_THEME_BLUE) :
             home.themePendingPreset == 2 ? LOC(STR_THEME_GREEN) :
             home.themePendingPreset == 3 ? LOC(STR_THEME_LIGHT) : LOC(STR_THEME_MONOCHROME)));
   setLabelIfChanged(&themeConfirmMessage, message);
   refreshLabelScale(&themeConfirmMessage);
   drawLabelCentered(&themeConfirmMessage, x + panelW / 2, y + 96);

   for (int i = 0; i < 2; i++) {
      int bx = x + (panelW - 2 * 144 - 32) / 2 + i * 176;
      int by = y + 166;
      int selected = i == home.themeConfirmSelected;
      fillGfxRoundedRectangleFixed(bx, by, 136, 48, 9,
                              selected ? activeTheme->accent : activeTheme->surface);
      Label *choice = &themeConfirmChoices[i];
      refreshLabelScale(choice);
      setLabelColor(choice, selected ? activeTheme->appBg : activeTheme->textPrimary);
      moveLabel(choice, bx + (136 - choice->tt.tex.w) / 2,
                getCenteredLabelY(choice, by, 48));
      drawLabel(choice);
   }
}

static void drawThemePanel(void)
{
   if (home.themeCustomOpen) drawThemeCustomPanel();
   else drawThemeListPanel();
   if (home.themeConfirmOpen) drawThemeConfirmPanel();
}

static void updateSettingsPanel(void)
{
   if (updateOverwritePromptOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
         updateOverwritePromptOpen = 0;
         return;
      }
      if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_RIGHT))
         updateOverwriteChoice = !updateOverwriteChoice;
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         logInfo("[update-ui] overwrite prompt choice=%s\n", updateOverwriteChoice == 0 ? LOC(STR_YES_UPPER) : LOC(STR_NO_UPPER));
         if (updateOverwriteChoice == 0) {
            int downloadRc = startUpdateDownloadOverwrite();
            logInfo("[update-ui] overwrite download start rc=%d state=%d\n", downloadRc, getUpdateDownloadState());
            if (downloadRc != 0) {
               playlistToastShownAt = getTimeUs();
               setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_RESTART_UPDATE_DOWNLOAD));
            }
         }
         updateOverwritePromptOpen = 0;
      }
      return;
   }
   if (updateSettingsOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
         updateSettingsOpen = 0;
         return;
      }
      if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
         updateSettingsSelected = (updateSettingsSelected + 1) % 5;
      else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
         updateSettingsSelected = (updateSettingsSelected + 4) % 5;
      if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_LEFT) ||
          isPadButtonPressed(PAD_BTN_RIGHT)) {
         if (updateSettingsSelected == 0) {
            int enabled = getAutomaticUpdateChecks();
            if (isPadButtonPressed(PAD_BTN_LEFT)) enabled = 0;
            else if (isPadButtonPressed(PAD_BTN_RIGHT)) enabled = 1;
            else enabled = !enabled;
            setAutomaticUpdateChecks(enabled);
         } else if (updateSettingsSelected == 1 && !isPadButtonPressed(PAD_BTN_LEFT) &&
                    !isPadButtonPressed(PAD_BTN_RIGHT)) {
            updateDialogDismissed = 1;
            startUpdateCheck(1);
         } else if (updateSettingsSelected == 2 && !isPadButtonPressed(PAD_BTN_LEFT) &&
                    !isPadButtonPressed(PAD_BTN_RIGHT)) {
            if (getUpdateCheckState() == UPDATE_CHECK_AVAILABLE || getUpdateCheckState() == UPDATE_CHECK_UP_TO_DATE) {
               updateNotesOpen = 1; updateNotesScroll = 0; buildUpdateNotesLines();
            }
         } else if (updateSettingsSelected == 3 && !isPadButtonPressed(PAD_BTN_LEFT) &&
                    !isPadButtonPressed(PAD_BTN_RIGHT)) {
            int checkState = getUpdateCheckState();
            int packageReady = isUpdateDownloaded();
            logInfo("[update-ui] settings action=DOWNLOAD check_state=%d package_ready=%d download_state=%d\n",
                    checkState, packageReady, getUpdateDownloadState());
            if (checkState == UPDATE_CHECK_AVAILABLE || checkState == UPDATE_CHECK_UP_TO_DATE) {
               if (packageReady) {
                  updateOverwriteNewRelease = strcmp(getLastDownloadedUpdateVersion(), getLatestVersion()) != 0;
                  updateOverwritePromptOpen = 1;
                  updateOverwriteChoice = 0;
                  logInfo("[update-ui] settings download confirmation new_release=%d old=%s new=%s\\n",
                          updateOverwriteNewRelease, getLastDownloadedUpdateVersion(), getLatestVersion());
               } else {
                  int downloadRc = startUpdateDownload();
                  logInfo("[update-ui] settings download start rc=%d state=%d\n", downloadRc, getUpdateDownloadState());
               }
            } else {
               logWarn("[update-ui] settings download rejected: update check state=%d\n", checkState);
            }
         } else if (updateSettingsSelected == 4 && !isPadButtonPressed(PAD_BTN_LEFT) &&
                    !isPadButtonPressed(PAD_BTN_RIGHT)) {
            updateSettingsOpen = 0;
         }
      }
      return;
   }
   if (home.themeOpen) { updateThemePanel(); return; }
   if (home.downloadSettingsOpen) { updateDownloadSettingsPanel(); return; }
   if (home.languageConfirmOpen) { updateLanguageConfirm(); return; }
   if (home.regionOpen) { updateRegionPicker(); return; }
   if (home.sponsorblockOpen) { updateSponsorBlockPanel(); return; }
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
      home.settingsOpen = 0;
      return;
   }
   if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      home.settingsSelected = (home.settingsSelected + 1) % SETTINGS_ROW_COUNT;
   else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      home.settingsSelected = (home.settingsSelected + SETTINGS_ROW_COUNT - 1) % SETTINGS_ROW_COUNT;
   int overscanDelta = 0;
   int screensaverDelta = 0;
   if (home.settingsSelected == 3) {
      if (settingsOverscanRepeat.timer == 0)
         settingsOverscanRepeat.timer = sys_time_get_system_time();
      if (isRepeatDue(&settingsOverscanRepeat, getPadButtonState(PAD_BTN_LEFT))) overscanDelta = -1;
      else if (isRepeatDue(&settingsOverscanRepeat, getPadButtonState(PAD_BTN_RIGHT))) overscanDelta = 1;
   } else {
      settingsOverscanRepeat.timer = 0;
      settingsOverscanRepeat.repeats = 0;
   }
   int cropOverscanDelta = 0;
   if (home.settingsSelected == 4) {
      if (settingsCropOverscanRepeat.timer == 0)
         settingsCropOverscanRepeat.timer = sys_time_get_system_time();
      if (isRepeatDue(&settingsCropOverscanRepeat, getPadButtonState(PAD_BTN_LEFT))) cropOverscanDelta = -1;
      else if (isRepeatDue(&settingsCropOverscanRepeat, getPadButtonState(PAD_BTN_RIGHT))) cropOverscanDelta = 1;
   } else {
      settingsCropOverscanRepeat.timer = 0;
      settingsCropOverscanRepeat.repeats = 0;
   }
   if (home.settingsSelected == 10) {
      if (settingsScreensaverRepeat.timer == 0)
         settingsScreensaverRepeat.timer = sys_time_get_system_time();
      if (isRepeatDue(&settingsScreensaverRepeat, getPadButtonState(PAD_BTN_LEFT))) screensaverDelta = -1;
      else if (isRepeatDue(&settingsScreensaverRepeat, getPadButtonState(PAD_BTN_RIGHT))) screensaverDelta = 1;
   } else {
      settingsScreensaverRepeat.timer = 0;
      settingsScreensaverRepeat.repeats = 0;
   }
   if (home.settingsSelected == 10 && isPadButtonPressed(PAD_BTN_CROSS))
      screensaverDelta = 1;

   if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_LEFT) ||
       isPadButtonPressed(PAD_BTN_RIGHT) || overscanDelta || cropOverscanDelta || screensaverDelta) {
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
         if (overscanDelta)
            setOverscanPercent(getOverscanPercent() + overscanDelta);
      } else if (home.settingsSelected == 4) {
         if (cropOverscanDelta)
            setCropToFillOverscanPercent(getCropToFillOverscanPercent() + cropOverscanDelta);
      } else if (home.settingsSelected == 5 && isPadButtonPressed(PAD_BTN_CROSS)) {
         openRegionPicker();
      } else if (home.settingsSelected == 6 && isPadButtonPressed(PAD_BTN_CROSS)) {
         openAppLanguagePicker();
      } else if (home.settingsSelected == 7 && isPadButtonPressed(PAD_BTN_CROSS)) {
         openSponsorBlockPanel();
      } else if (home.settingsSelected == 8 && isPadButtonPressed(PAD_BTN_CROSS)) {
         home.downloadSettingsOpen = 1;
         home.downloadSettingsSelected = 0;
         settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
      } else if (home.settingsSelected == 9 && isPadButtonPressed(PAD_BTN_CROSS)) {
         home.themeOpen = 1;
         home.themeSelected = getActiveThemePreset();
         if (home.themeSelected < 0 || home.themeSelected >= THEME_ROW_COUNT) home.themeSelected = 0;
         home.themeCustomOpen = 0;
         home.themeConfirmOpen = 0;
         settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
      } else if (home.settingsSelected == 11 && isPadButtonPressed(PAD_BTN_CROSS)) {
         updateSettingsOpen = 1;
         updateSettingsSelected = 0;
         settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
      } else if (home.settingsSelected == 10 && screensaverDelta) {
         int timeout = getScreensaverTimeout();
         int option = timeout == 0 ? 0 : timeout / 30;
         option = (option + (screensaverDelta > 0 ? 1 : 10)) % 11;
         setScreensaverTimeout(option * 30);
      }
   }
}

static void drawDownloadSettingsPanel(void)
{
   setLabelIfChanged(&downloadSettingsValues[0], downloadResolutionName(getDownloadResolution()));
   setLabelIfChanged(&downloadSettingsValues[1], downloadLanguageName(getDownloadAudioLanguage(), 1));
   setLabelIfChanged(&downloadSettingsValues[2], strcmp(getDownloadAudioLanguage(), "original") == 0 ?
                     LOC(STR_NOT_APPLICABLE) : (getDownloadAutoDubEnabled() ? LOC(STR_ALLOW_AUTO_GENERATED) : LOC(STR_UPLOADED_ONLY)));
   setLabelIfChanged(&downloadSettingsValues[3], getDownloadPrimarySubtitlesEnabled() ? LOC(STR_AUTO_MATCH_AUDIO) : LOC(STR_OFF));
   setLabelIfChanged(&downloadSettingsValues[4], downloadLanguageName(getDownloadSecondarySubtitleLanguage(), 0));
   int panelW = isLargeUiScale() ? 820 : DOWNLOAD_SETTINGS_PANEL_W;
   int panelH = isLargeUiScale() ? 560 : DOWNLOAD_SETTINGS_PANEL_H;
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

static void drawUpdateSettingsPanel(void)
{
   char installed[96], latest[96], stateText[96];
   snprintf(installed, sizeof installed, LOC(STR_INSTALLED_VERSION), getInstalledVersion());
   snprintf(latest, sizeof latest, LOC(STR_LATEST_AVAILABLE_VERSION), getLatestVersion());
   UpdateCheckState state = getUpdateCheckState();
   if (state == UPDATE_CHECK_AVAILABLE)
      snprintf(stateText, sizeof stateText, LOC(STR_UPDATE_AVAILABLE_STATUS), getLatestVersion());
   else if (state == UPDATE_CHECK_RUNNING)
      snprintf(stateText, sizeof stateText, LOC(STR_CHECKING_GITHUB_STATUS));
   else if (state == UPDATE_CHECK_UP_TO_DATE)
      snprintf(stateText, sizeof stateText, LOC(STR_UP_TO_DATE_STATUS));
   else if (state == UPDATE_CHECK_FAILED)
      snprintf(stateText, sizeof stateText, LOC(STR_UPDATE_CHECK_FAILED_STATUS));
   else
      snprintf(stateText, sizeof stateText, LOC(STR_NOT_CHECKED_STATUS));
   setLabelIfChanged(&updatePanelInfo[0], installed);
   setLabelIfChanged(&updatePanelInfo[1], latest);
   setLabelIfChanged(&updatePanelInfo[2], stateText);
   setLabelIfChanged(&updatePanelRows[0], getAutomaticUpdateChecks() ? LOC(STR_AUTOMATIC_CHECKS_ON) : LOC(STR_AUTOMATIC_CHECKS_OFF));
   setLabelIfChanged(&updatePanelRows[1], state == UPDATE_CHECK_RUNNING ? LOC(STR_CHECKING_GITHUB) : LOC(STR_CHECK_FOR_UPDATES_NOW));
   char releaseRow[224];
   snprintf(releaseRow, sizeof releaseRow, LOC(STR_RELEASE_NOTES_VERSION), getLatestVersion());
   setLabelIfChanged(&updatePanelRows[2], releaseRow);
   setLabelIfChanged(&updatePanelRows[3], getUpdateDownloadState() == 1 ?
                     LOC(STR_UPDATE_DOWNLOAD_STARTED) : LOC(STR_DOWNLOAD_UPDATE));
   setLabelIfChanged(&updatePanelRows[4], LOC(STR_BACK));
   setLabelIfChanged(&updatePanelRows[5],
      LOC(STR_INSTALL_FROM_XMB));

   int w = UPDATE_PANEL_W < home.screenW - 32 ? UPDATE_PANEL_W : home.screenW - 32;
   int h = UPDATE_PANEL_H < home.screenH - 32 ? UPDATE_PANEL_H : home.screenH - 32;
   int x = (home.screenW - w) / 2, y = (home.screenH - h) / 2;
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangle(x, y, w, h, 10, activeTheme->appBg);
   fillGfxRoundedRectangle(x, y, w, 4, 2, activeTheme->accent);
   refreshLabelScale(&updatePanelTitle);
   drawLabelCentered(&updatePanelTitle, home.screenW / 2, y + 28);
   for (int i = 0; i < 3; i++) {
      Label *label = &updatePanelInfo[i];
      refreshLabelScale(label);
      setLabelColor(label, activeTheme->textSecondary);
      moveLabel(label, x + 34, y + 72 + i * 28);
      drawLabel(label);
   }
   for (int i = 0; i < 5; i++) {
      Label *label = &updatePanelRows[i];
      refreshLabelScale(label);
      int rowY = y + 150 + i * 38;
      if (i == updateSettingsSelected)
         fillGfxRoundedRectangle(x + 24, rowY - 5, w - 48, 44, 7, activeTheme->rowHighlight);
      setLabelColor(label, i == updateSettingsSelected ? activeTheme->accent : activeTheme->textPrimary);
      moveLabel(label, x + 40, getCenteredLabelY(label, rowY, 36));
      drawLabel(label);
   }
   refreshLabelScale(&updatePanelRows[5]);
   setLabelColor(&updatePanelRows[5], activeTheme->textSecondary);
   moveLabel(&updatePanelRows[5], x + 40, y + h - 86);
   drawLabel(&updatePanelRows[5]);
   if (updateOverwritePromptOpen) {
      int pw = 520, ph = 190;
      int px = (home.screenW - pw) / 2, py = (home.screenH - ph) / 2;
      fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
      fillGfxRoundedRectangle(px, py, pw, ph, 10, activeTheme->appBg);
      fillGfxRoundedRectangle(px, py, pw, 4, 2, activeTheme->accent);
      setLabelIfChanged(&updateDialogTitle, updateOverwriteNewRelease ? LOC(STR_DIFFERENT_RELEASE_AVAILABLE) : LOC(STR_UPDATE_ALREADY_DOWNLOADED));
      setLabelIfChanged(&updateDialogRows[0], updateOverwriteNewRelease ? LOC(STR_DOWNLOAD_NEW_DELETE_OLD) : LOC(STR_DOWNLOAD_AGAIN_OVERWRITE));
      setLabelIfChanged(&updateDialogRows[1], LOC(STR_YES));
      setLabelIfChanged(&updateDialogRows[2], LOC(STR_NO));
      refreshLabelScale(&updateDialogTitle);
      drawLabelCentered(&updateDialogTitle, home.screenW / 2, py + 28);
      for (int n = 0; n < 3; n++) {
         Label *label = &updateDialogRows[n];
         refreshLabelScale(label);
         setLabelColor(label, n == updateOverwriteChoice + 1 ? activeTheme->accent : activeTheme->textPrimary);
         moveLabel(label, px + 28, py + 70 + n * 36);
         drawLabel(label);
      }
   }
   updateHints.y = y + h - 36;
   drawButtonHints(&updateHints, home.screenW);

}

static void drawUpdateDialog(void)
{
   setLabelIfChanged(&updateDialogTitle, LOC(STR_NEW_UPDATE_AVAILABLE_TITLE));
   char versionInfo[128];
   snprintf(versionInfo, sizeof versionInfo, LOC(STR_INSTALLED_LATEST_FORMAT),
            getInstalledVersion(), getLatestVersion());
   setLabelIfChanged(&updateDialogVersion, versionInfo);
   static const AppStringId rowIds[] = {
      STR_RELEASE_NOTES, STR_DOWNLOAD_UPDATE, STR_DISABLE_AUTOMATIC_CHECKS,
      STR_DO_NOT_NOTIFY_RELEASE, STR_CLOSE
   };
   for (int i = 0; i < 5; i++) {
      if (i == 1 && getUpdateDownloadState() == 1)
         setLabelIfChanged(&updateDialogRows[i], LOC(STR_DOWNLOADING_UPDATE));
      else
         setLabelIfChanged(&updateDialogRows[i], LOC(rowIds[i]));
   }
   int w = UPDATE_DIALOG_W < home.screenW - 32 ? UPDATE_DIALOG_W : home.screenW - 32;
   int h = UPDATE_DIALOG_H < home.screenH - 32 ? UPDATE_DIALOG_H : home.screenH - 32;
   int x = (home.screenW - w) / 2, y = (home.screenH - h) / 2;
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangle(x, y, w, h, 10, activeTheme->appBg);
   fillGfxRoundedRectangle(x, y, w, 4, 2, activeTheme->accent);
   refreshLabelScale(&updateDialogTitle);
   drawLabelCentered(&updateDialogTitle, home.screenW / 2, y + 28);
   refreshLabelScale(&updateDialogVersion);
   drawLabelCentered(&updateDialogVersion, home.screenW / 2, y + 72);
   for (int i = 0; i < 5; i++) {
      Label *label = &updateDialogRows[i];
      refreshLabelScale(label);
      int rowY = y + 108 + i * 42;
      if (i == updateDialogSelected)
         fillGfxRoundedRectangle(x + 24, rowY - 4, w - 48, 40, 7, activeTheme->rowHighlight);
      setLabelColor(label, i == updateDialogSelected ? activeTheme->accent : activeTheme->textPrimary);
      moveLabel(label, x + 40, getCenteredLabelY(label, rowY, 34));
      drawLabel(label);
   }
   updateHints.y = y + h - 34;
   drawButtonHints(&updateHints, home.screenW);
   if (updateOverwritePromptOpen) {
      int pw = 520;
      int ph = 190;
      int px = (home.screenW - pw) / 2, py = (home.screenH - ph) / 2;
      fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
      fillGfxRoundedRectangle(px, py, pw, ph, 10, activeTheme->appBg);
      fillGfxRoundedRectangle(px, py, pw, 4, 2, activeTheme->accent);
      setLabelIfChanged(&updateDialogTitle, updateOverwriteNewRelease ? LOC(STR_DIFFERENT_RELEASE_AVAILABLE) : LOC(STR_UPDATE_ALREADY_DOWNLOADED));
      setLabelIfChanged(&updateDialogRows[0], updateOverwriteNewRelease ? LOC(STR_DOWNLOAD_NEW_DELETE_OLD) : LOC(STR_DOWNLOAD_AGAIN_OVERWRITE));
      setLabelIfChanged(&updateDialogRows[1], LOC(STR_YES));
      setLabelIfChanged(&updateDialogRows[2], LOC(STR_NO));
      refreshLabelScale(&updateDialogTitle);
      drawLabelCentered(&updateDialogTitle, home.screenW / 2, py + 28);
      for (int n = 0; n < 3; n++) {
         Label *label = &updateDialogRows[n];
         refreshLabelScale(label);
         int selectedChoice = updateOverwriteChoice;
         setLabelColor(label, n == selectedChoice + 1 ? activeTheme->accent : activeTheme->textPrimary);
         moveLabel(label, px + 28, py + 70 + n * 36);
         drawLabel(label);
      }
   }
}

static void buildUpdateNotesLines(void)
{
   const char *src = getLatestReleaseNotes();
   char line[LABEL_MAX_TEXT];
   int used = 0;
   int wrapLimit = 58;
   updateNotesLineCount = 0;
   const char *releaseTitle = getLatestReleaseTitle();
   if (releaseTitle && releaseTitle[0]) {
      snprintf(updateNotesLineText[updateNotesLineCount++], LABEL_MAX_TEXT,
               "{b}%s{/b}", releaseTitle);
   }
   line[0] = 0;
   while (*src && updateNotesLineCount < UPDATE_NOTES_MAX_LINES) {
      if (*src == '\r') { src++; continue; }
      if (*src == '\n') {
         line[used] = 0;
         strCopy(updateNotesLineText[updateNotesLineCount++], LABEL_MAX_TEXT, line);
         used = 0; line[0] = 0; src++; continue;
      }
      if (used >= wrapLimit && (((unsigned char)*src & 0xC0) != 0x80)) {
         int cut = used;
         while (cut > 0 && line[cut - 1] != ' ') cut--;
         if (cut > 0) {
            line[cut - 1] = 0;
            strCopy(updateNotesLineText[updateNotesLineCount++], LABEL_MAX_TEXT, line);
            int remain = used - cut;
            memmove(line, line + cut, (size_t)remain);
            used = remain; line[used] = 0;
         } else {
            line[used] = 0;
            strCopy(updateNotesLineText[updateNotesLineCount++], LABEL_MAX_TEXT, line);
            used = 0; line[0] = 0;
         }
         if (updateNotesLineCount >= UPDATE_NOTES_MAX_LINES) break;
      }
      line[used++] = *src++;
      line[used] = 0;
   }
   if (used && updateNotesLineCount < UPDATE_NOTES_MAX_LINES) {
      line[used] = 0;
      strCopy(updateNotesLineText[updateNotesLineCount++], LABEL_MAX_TEXT, line);
   }
   if (!getLatestReleaseNotes()[0] && updateNotesLineCount < UPDATE_NOTES_MAX_LINES) {
      strCopy(updateNotesLineText[updateNotesLineCount++], LABEL_MAX_TEXT,
              LOC(STR_NO_RELEASE_NOTES));
   }
   if (!updateNotesLineCount) {
      strCopy(updateNotesLineText[0], LABEL_MAX_TEXT, LOC(STR_NO_RELEASE_NOTES));
      updateNotesLineCount = 1;
   }
   /* Convert Markdown headings and bold delimiters to native renderer tags. */
   for (int n = 0; n < updateNotesLineCount; n++) {
      char converted[LABEL_MAX_TEXT]; int out = 0;
      const char *q = updateNotesLineText[n];
      int bold = 0;
      if (q[0] == '#' && q[1] == '#') {
         q += 2;
         while (*q == ' ' || *q == '\t') q++;
         converted[out++] = '{';
         converted[out++] = 'b';
         converted[out++] = '}';
         bold = 1;
      }
      while (*q && out < LABEL_MAX_TEXT - 5) {
         if (q[0] == '*' && q[1] == '*') {
            memcpy(converted + out, bold ? "{/b}" : "{b}", bold ? 4 : 3);
            out += bold ? 4 : 3;
            bold = !bold;
            q += 2;
            continue;
         }
         converted[out++] = *q++;
      }
      if (bold && out < LABEL_MAX_TEXT - 4) {
         memcpy(converted + out, "{/b}", 4);
         out += 4;
      }
      converted[out] = 0;
      strCopy(updateNotesLineText[n], LABEL_MAX_TEXT, converted);
   }
}

static void drawUpdateNotes(void)
{
   setLabelIfChanged(&updateNotesTitle, getLatestReleaseTag());
   int w = UPDATE_NOTES_W < home.screenW - 16 ? UPDATE_NOTES_W : home.screenW - 16;
   int h = UPDATE_NOTES_H < home.screenH - 16 ? UPDATE_NOTES_H : home.screenH - 16;
   int x = (home.screenW - w) / 2, y = (home.screenH - h) / 2;
   int visible = UPDATE_NOTES_VISIBLE_LINES;
   int pageCount = (updateNotesLineCount + visible - 1) / visible;
   if (pageCount < 1) pageCount = 1;
   int page = updateNotesScroll / visible + 1;
   if (page > pageCount) page = pageCount;
   updateNotesScroll = (page - 1) * visible;
   char pageText[24];
   snprintf(pageText, sizeof pageText, "%d/%d", page, pageCount);
   setLabelIfChanged(&updateNotesPageLabel, pageText);
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangle(x, y, w, h, 10, activeTheme->appBg);
   fillGfxRoundedRectangle(x, y, w, 4, 2, activeTheme->accent);
   refreshLabelScale(&updateNotesTitle);
   drawLabelCentered(&updateNotesTitle, home.screenW / 2, y + 22);
   refreshLabelScale(&updateNotesPageLabel);
   setLabelColor(&updateNotesPageLabel, activeTheme->textSecondary);
   int pageLabelX = x + w - 24 - updateNotesPageLabel.tt.tex.w;
   if (pageLabelX < x + 24) pageLabelX = x + 24;
   moveLabel(&updateNotesPageLabel, pageLabelX, y + 18);
   drawLabel(&updateNotesPageLabel);
   for (int i = 0; i < visible; i++) {
      int index = updateNotesScroll + i;
      if (index >= updateNotesLineCount) break;
      setLabelIfChanged(&updateNotesLines[i], updateNotesLineText[index]);
      refreshLabelScale(&updateNotesLines[i]);
      setLabelColor(&updateNotesLines[i], activeTheme->textPrimary);
      moveLabel(&updateNotesLines[i], x + 24, y + 58 + i * UPDATE_NOTES_LINE_STEP);
      drawLabel(&updateNotesLines[i]);
   }
   updateHints.y = y + h - 28;
   drawButtonHints(&updateHints, home.screenW);
}

static void drawSettingsPanel(void)
{
   const char *settingNames[SETTINGS_ROW_COUNT] = {
      LOC(STR_AUTOPLAY), LOC(STR_ENABLE_1080P60), LOC(STR_UI_SCALING),
      LOC(STR_OVERSCAN), LOC(STR_OVERSCAN_CROP), LOC(STR_CONTENT_REGION), LOC(STR_APP_LANGUAGE),
      LOC(STR_SPONSORBLOCK), LOC(STR_DOWNLOAD_SETTINGS), LOC(STR_THEMES),
      LOC(STR_SCREENSAVER_TIMEOUT), LOC(STR_UPDATES_ABOUT)
   };
   setLabelIfChanged(&settingsTitle, LOC(STR_SETTINGS));
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++)
      setLabelIfChanged(&settingsLabels[i], settingNames[i]);

   // Avoid printf floating-point formatting here: the PS3 runtime may be built
   // without float printf support, which can leave only the trailing "x".
   const char *scaleValue = getGfxUiScaleFactor() >= 1.25f ? "1.5x" : "1.0x";
   char overscanValue[16], cropOverscanValue[16];
   int overscan = getOverscanPercent();
   int cropOverscan = getCropToFillOverscanPercent();
   if (overscan > 0) snprintf(overscanValue, sizeof overscanValue, "+%d%%", overscan);
   else              snprintf(overscanValue, sizeof overscanValue, "%d%%", overscan);
   if (cropOverscan > 0) snprintf(cropOverscanValue, sizeof cropOverscanValue, "+%d%%", cropOverscan);
   else                  snprintf(cropOverscanValue, sizeof cropOverscanValue, "%d%%", cropOverscan);
   setLabelIfChanged(&settingsValues[2], scaleValue);
   setLabelIfChanged(&settingsValues[3], overscanValue);
   setLabelIfChanged(&settingsValues[4], cropOverscanValue);
   const ContentRegion *region = getContentRegionAt(getContentRegionIndex());
   setLabelIfChanged(&settingsValues[5], region ? region->name : getContentRegionCode());
   setLabelIfChanged(&settingsValues[6], getAppLanguageName(getApplicationLanguage()));
   setLabelIfChanged(&settingsValues[7], LOC(STR_CONFIGURE));
   setLabelIfChanged(&settingsValues[8], LOC(STR_CONFIGURE));
   setLabelIfChanged(&settingsValues[9], LOC(STR_CONFIGURE));
   char screensaverValue[16];
   int screensaverSeconds = getScreensaverTimeout();
   if (screensaverSeconds == 0) strCopy(screensaverValue, sizeof screensaverValue, LOC(STR_OFF));
   else snprintf(screensaverValue, sizeof screensaverValue, "%ds", screensaverSeconds);
   setLabelIfChanged(&settingsValues[10], screensaverValue);
   setLabelIfChanged(&settingsValues[11], "");

   int panelW = getSettingsPanelWidth(), panelH = getSettingsPanelHeight();
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;
   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 10, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&settingsTitle);
   drawLabelCentered(&settingsTitle, home.screenW / 2, y + 28);

   int toggleW = 176;
   int toggleHalfW = toggleW / 2;
   int autoplayX = x + panelW - 40 - toggleW;
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      Label *name = &settingsLabels[i];
      int rowY = y + SETTINGS_ROW_TOP + i * SETTINGS_ROW_STEP;
      refreshLabelScale(name);
      if (i == 11 && i == home.settingsSelected)
         fillGfxRoundedRectangleFixed(x + 24, rowY - 2, panelW - 48,
                                 SETTINGS_ROW_HEIGHT + 4, 7, activeTheme->rowHighlight);
      setLabelColor(name, (i == 11 && i == home.settingsSelected) ?
                          activeTheme->accent : activeTheme->textPrimary);
      moveLabel(name, x + 40, getCenteredLabelY(name, rowY, SETTINGS_ROW_HEIGHT));
      drawLabel(name);
      if (i == 11) continue;

      if (i == 0 || i == 1) {
         // Both boolean settings use the same compact OFF/ON switch.
         int enabled = i == 0 ? getAutoplayEnabled() : getEnable1080p60();
         int selected = enabled ? 1 : 0;
         int toggleY = rowY + (SETTINGS_ROW_HEIGHT - 38) / 2;
         int selectedX = autoplayX + selected * toggleHalfW;
         fillGfxRoundedRectangleFixed(autoplayX, toggleY, toggleW, 38, 8, activeTheme->surface);
         if (i == home.settingsSelected)
            fillGfxRoundedRectangleFixed(selectedX - 4, toggleY - 4, toggleHalfW + 8, 46, 10, 0x38FFFFFF);
         fillGfxRoundedRectangleFixed(selectedX, toggleY, toggleHalfW, 38, 8, activeTheme->accent);
         for (int option = 0; option < 2; option++) {
            Label *optionLabel = &settingsAutoplayOptions[option];
            refreshLabelScale(optionLabel);
            setLabelColor(optionLabel, option == selected ? activeTheme->appBg : activeTheme->textPrimary);
            moveLabel(optionLabel,
                      autoplayX + option * toggleHalfW + (toggleHalfW - optionLabel->tt.tex.w) / 2,
                      getCenteredLabelY(optionLabel, toggleY, 38));
            drawLabel(optionLabel);
         }
         continue;
      }

      Label *value = &settingsValues[i];
      refreshLabelScale(value);
      int valuePad = 14;
      int valueW = value->tt.tex.w + valuePad * 2;
      int minValueW = 120;
      if (valueW < minValueW) valueW = minValueW;
      int valueX = x + panelW - 40 - valueW;
      int valueY = getCenteredLabelY(value, rowY, SETTINGS_ROW_HEIGHT);
      int valueH = value->tt.tex.h + 10;

      if (i == home.settingsSelected)
         fillGfxRoundedRectangleFixed(valueX, valueY - 5, valueW, valueH, 7,
                                 activeTheme->rowHighlight);

      setLabelColor(value, i == home.settingsSelected ? activeTheme->accent : activeTheme->textPrimary);
      moveLabel(value, valueX + (valueW - value->tt.tex.w) / 2, valueY);
      drawLabel(value);
   }
   settingsHints.y = y + panelH - 42;
   drawButtonHints(&settingsHints, home.screenW);
}

// Settings opened from Search is a real stacked overlay, not a pop to Home.
// Search remains allocated below it, so closing this screen restores the same
// channel/category and does not rebuild or synchronously join its grid worker.
static void initSearchSettingsOverlay(void)
{
}

static void updateSearchSettingsOverlay(void)
{
   if (updateOverwritePromptOpen && updateDialogOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
         updateOverwritePromptOpen = 0;
      } else {
         if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_RIGHT) ||
             isPadButtonPressed(PAD_BTN_UP) || isPadButtonPressed(PAD_BTN_DOWN))
            updateOverwriteChoice = !updateOverwriteChoice;
         if (isPadButtonPressed(PAD_BTN_CROSS)) {
            if (updateOverwriteChoice == 0) startUpdateDownloadOverwrite();
            updateOverwritePromptOpen = 0;
         }
      }
      return;
   }
   if (updateNotesOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) updateNotesOpen = 0;
      else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)) ||
               isPadButtonPressed(PAD_BTN_RIGHT)) {
         int pageCount = (updateNotesLineCount + UPDATE_NOTES_VISIBLE_LINES - 1) / UPDATE_NOTES_VISIBLE_LINES;
         if (updateNotesScroll / UPDATE_NOTES_VISIBLE_LINES + 1 < pageCount)
            updateNotesScroll += UPDATE_NOTES_VISIBLE_LINES;
      } else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)) ||
                 isPadButtonPressed(PAD_BTN_LEFT)) {
         if (updateNotesScroll >= UPDATE_NOTES_VISIBLE_LINES)
            updateNotesScroll -= UPDATE_NOTES_VISIBLE_LINES;
      }
      return;
   }
   if (updateDialogOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
         updateDialogOpen = 0;
         updateDialogDismissed = 1;
      } else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN))) {
         updateDialogSelected = (updateDialogSelected + 1) % 5;
      } else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP))) {
         updateDialogSelected = (updateDialogSelected + 4) % 5;
      } else if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (updateDialogSelected == 0) { updateNotesOpen = 1; updateNotesScroll = 0; buildUpdateNotesLines(); }
         else if (updateDialogSelected == 1 && !isUpdateDownloaded()) startUpdateDownload();
         else if (updateDialogSelected == 2) { disableAutomaticUpdateChecks(); updateDialogOpen = 0; updateDialogDismissed = 1; }
         else if (updateDialogSelected == 3) { ignoreCurrentUpdate(); updateDialogOpen = 0; updateDialogDismissed = 1; }
         else { updateDialogOpen = 0; updateDialogDismissed = 1; }
      }
      return;
   }

   int wasOpen = home.settingsOpen;
   updateSettingsPanel();
   if (wasOpen && !home.settingsOpen) popScreen();
}

static void drawSearchSettingsOverlay(void)
{
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->appBg);
   if (home.settingsOpen) {
      if (updateSettingsOpen) drawUpdateSettingsPanel();
      else if (home.themeOpen) drawThemePanel();
      else if (home.downloadSettingsOpen) drawDownloadSettingsPanel();
      else {
         drawSettingsPanel();
         if (home.regionOpen) drawRegionPicker();
         else if (home.sponsorblockOpen) drawSponsorBlockPanel();
      }
   }
   if (home.languageConfirmOpen) drawLanguageConfirm();
   if (updateDialogOpen) drawUpdateDialog();
   if (updateNotesOpen) drawUpdateNotes();
}

static void termSearchSettingsOverlay(void)
{
}

static void drawHomeIconFixed(Icon *icon, int x, int y, uint32_t color)
{
   if (!icon || !icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   drawGfxTextureFixedPosition(x + icon->offsetX, y + icon->offsetY,
                               icon->tex->w, icon->tex->h, *icon->tex,
                               0.0f, 0.0f, 1.0f, 1.0f, color, GFX_FILTER_LINEAR);
}

static void drawShuffleStrokePoint(int x, int y, uint32_t color, uint32_t outline)
{
   fillGfxRectangleFixed(x - 2, y - 2, 9, 9, outline);
   fillGfxRectangleFixed(x, y, 5, 5, color);
}

static void drawShuffleCurve(int x, int y, int direction, uint32_t color, uint32_t outline)
{
   // Curved S-shaped route with a clear arrowhead and padding from the badge edge.
   static const int points[2][13][2] = {
      {{0,5},{5,5},{9,6},{13,9},{17,13},{21,17},{25,20},{29,22},{33,23},{37,23},{41,23},{45,23},{49,23}},
      {{0,23},{5,23},{9,22},{13,19},{17,15},{21,11},{25,8},{29,6},{33,5},{37,5},{41,5},{45,5},{49,5}}
   };
   for (int i = 0; i < 12; i++) {
      int x1 = x + points[direction > 0 ? 0 : 1][i][0];
      int y1 = y + points[direction > 0 ? 0 : 1][i][1];
      int x2 = x + points[direction > 0 ? 0 : 1][i + 1][0];
      int y2 = y + points[direction > 0 ? 0 : 1][i + 1][1];
      int dx = x2 - x1, dy = y2 - y1;
      int steps = dx > (dy < 0 ? -dy : dy) ? dx : (dy < 0 ? -dy : dy);
      if (steps < 1) steps = 1;
      for (int n = 0; n <= steps; n++) {
         int px = x1 + dx * n / steps;
         int py = y1 + dy * n / steps;
         drawShuffleStrokePoint(px, py, color, outline);
      }
   }
   int tipX = x + 49;
   int tipY = y + (direction > 0 ? 23 : 5);
   for (int i = 0; i < 5; i++) {
      drawShuffleStrokePoint(tipX - 5 + i, tipY - 5 + i, color, outline);
      drawShuffleStrokePoint(tipX - 5 + i, tipY + 5 - i, color, outline);
   }
   // Close the arrowhead tip with foreground pixels; diagonal outlined blocks
   // otherwise leave a single contrasting pixel at the point on light themes.
   fillGfxRectangleFixed(tipX - 2, tipY - 2, 5, 5, color);
}

static void drawPlaylistShuffleBadge(void)
{
   if (!homeIsPlaylistContents()) return;
   int active = isPlaylistShuffleEnabled();
   int width = 76;
   int height = 48;
   int x = home.screenW - MARGIN_X - width;
   int y = titleLabel.y + (titleLabel.tt.tex.h - height) / 2;
   if (y < titleLabel.y) y = titleLabel.y;
   uint32_t edge = active ? activeTheme->accent : activeTheme->rowHighlight;
   uint32_t fill = active ? activeTheme->rowHighlight : activeTheme->surface;
   uint32_t ink = homeThemeIsDark() ? 0xFFFFFFFF : 0xFF202020;
   uint32_t outline = homeThemeIsDark() ? 0xFF000000 : 0xFFFFFFFF;
   fillGfxRoundedRectangleFixed(x, y, width, height, 12, edge);
   fillGfxRoundedRectangleFixed(x + 1, y + 1, width - 2, height - 2, 11, fill);
   int cx = x + 12, cy = y + 9;
   drawShuffleCurve(cx, cy, 1, ink, outline);
   drawShuffleCurve(cx, cy, -1, ink, outline);
}

#define DOWNLOAD_SPACE_LOW_BYTES  (1024ULL * 1024ULL * 1024ULL)

static void formatDownloadSpace(uint64_t bytes, char *out, int capacity)
{
   const uint64_t gb = 1024ULL * 1024ULL * 1024ULL;
   const uint64_t unit = bytes >= gb ? gb : 1024ULL * 1024ULL;
   const char *suffix = bytes >= gb ? "GB" : "MB";
   unsigned long long whole = (unsigned long long)(bytes / unit);
   unsigned long long fraction = (unsigned long long)((bytes % unit) * 100ULL / unit);
   snprintf(out, capacity, "%llu.%02llu%s", whole, fraction, suffix);
}

static void updateDownloadSpaceStats(void)
{
   // Read only the manager's in-memory snapshot here. measureTree(), getFreeSpace()
   // and all directory traversal run on the low-priority library worker.
   uint64_t usedBytes = 0, availableBytes = 0;
   int ready = 0;
   getDownloadLibraryStats(&usedBytes, &availableBytes, &ready);

   char usedText[24], availableValue[24], availableText[48], prefix[96];
   if (ready) {
      formatDownloadSpace(usedBytes, usedText, sizeof usedText);
      formatDownloadSpace(availableBytes, availableValue, sizeof availableValue);
      snprintf(availableText, sizeof availableText, LOC(STR_AVAILABLE_FORMAT), availableValue);
      downloadSpaceAvailableLow = availableBytes <= DOWNLOAD_SPACE_LOW_BYTES;
      snprintf(prefix, sizeof prefix, LOC(STR_USED_FORMAT), usedText);
   } else {
      strCopy(prefix, sizeof prefix, LOC(STR_USED_EMPTY));
      strCopy(availableText, sizeof availableText, LOC(STR_AVAILABLE_EMPTY));
      downloadSpaceAvailableLow = 0;
   }
   setLabelIfChanged(&downloadSpacePrefixLabel, prefix);
   setLabelIfChanged(&downloadSpaceAvailableLabel, availableText);
}

static int downloadSpaceStatusX, downloadSpaceStatusY;

static void setDownloadHeaderLabelScale(Label *label, float factor)
{
   float scale = getGfxUiScaleFactor() * factor;
   if (scale < 0.65f) scale = 0.65f;
   setLabelFixedRenderScale(label, scale, 0);
}

static void drawDownloadSpaceStatus(void)
{
   if (home.category != HOME_DOWNLOADS) return;
   setLabelColor(&downloadSpacePrefixLabel, activeTheme->textSecondary);
   setLabelColor(&downloadSpaceAvailableLabel,
                 downloadSpaceAvailableLow ? 0xFFFF4444 : activeTheme->textSecondary);
   drawLabelAt(&downloadSpacePrefixLabel, downloadSpaceStatusX, downloadSpaceStatusY);
   drawLabelAt(&downloadSpaceAvailableLabel,
               downloadSpaceStatusX + downloadSpacePrefixLabel.tt.tex.w, downloadSpaceStatusY);
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
      setLabelColor(label, selected ? 0xFFFFFFFF : 0xFFAAAAAA);
      drawLabelAt(label, textX, textY);
      x += width + HOME_SORT_GAP;
   }
}

static SearchResult downloadPlaylistJob;
static volatile int downloadPlaylistEstimateProgress, downloadPlaylistEstimateTotal;

static void downloadPlaylistWorker(uint64_t unused)
{
   (void)unused;
   const int capacity = 2048;
   int found = 0, failed = 0;
   uint64_t estimatedBytes = 0;
   int unknownSizes = 0;
   downloadPlaylistEstimateProgress = 0;
   downloadPlaylistEstimateTotal = 0;
   SearchResult *items = (SearchResult *)malloc(sizeof *items * capacity);
   if (!items) {
      downloadPlaylistThreadResult = -1;
      downloadPlaylistThreadDone = 1;
      return;
   }

   if (downloadPlaylistJob.isLocalPlaylist) {
      SearchResults *videos = (SearchResults *)malloc(sizeof *videos);
      int playlistRead = videos ?
         (strncmp(downloadPlaylistJob.localPlaylistId, "dp", 2) == 0 ?
            getDownloadPlaylistVideos(downloadPlaylistJob.localPlaylistId, videos) :
            getLocalPlaylistVideos(downloadPlaylistJob.localPlaylistId, videos)) : -1;
      if (!videos || playlistRead < 0) {
         failed = 1;
      } else {
         for (int i = 0; i < videos->count && found < capacity; i++) {
            SearchResult *item = &videos->items[i];
            if (!item->videoId[0] || item->isLive || item->isPlaylist) continue;
            items[found++] = *item;
         }
      }
      free(videos);
   } else if (downloadPlaylistJob.playlistId[0]) {
      char continuation[MAX_CONTINUATION] = "";
      SearchResults *page = (SearchResults *)malloc(sizeof *page);
      if (!page) failed = 1;
      else for (int pageIndex = 0; pageIndex < 64 && found < capacity; pageIndex++) {
         int rc = getAccountPlaylistVideos(downloadPlaylistJob.playlistId,
                                           continuation[0] ? continuation : NULL, page);
         if (rc != 0) { if (!found) failed = 1; break; }
         for (int i = 0; i < page->count && found < capacity; i++) {
            SearchResult *item = &page->items[i];
            if (!item->videoId[0] || item->isLive || item->isPlaylist) continue;
            items[found++] = *item;
         }
         if (!page->continuation[0] || !page->count) break;
         strCopy(continuation, sizeof continuation, page->continuation);
      }
      free(page);
   } else failed = 1;

   // Resolve each missing video's selected streams; playlist durations and
   // bitrates vary too widely for one resolution-based average to be useful.
   downloadPlaylistEstimateTotal = found;
   for (int i = 0; i < found; i++) {
      char path[MAX_PATH_LEN];
      if (getDownloadedVideoPath(items[i].videoId, path, sizeof path) != 0) {
         uint64_t itemBytes = 0;
         if (estimateDownloadSizeForVideo(&items[i], &itemBytes) == 0) estimatedBytes += itemBytes;
         else unknownSizes++;
      }
      downloadPlaylistEstimateProgress = i + 1;
   }
   home.downloadPlaylistEstimatedBytes = estimatedBytes;
   home.downloadPlaylistEstimateUnknown = unknownSizes;
   __sync_synchronize();
   downloadPlaylistResults = items;
   downloadPlaylistThreadResult = failed && !found ? -1 : found;
   downloadPlaylistThreadDone = 1;
}

static void finishDownloadPlaylistPreparation(void)
{
   if (!downloadPlaylistThreadActive || !downloadPlaylistThreadDone) return;
   joinThread(downloadPlaylistThread);
   downloadPlaylistThreadActive = 0;
   downloadPlaylistPreparing = 0;
   resumeVideoGrid(&home.grid);

   int found = (int)downloadPlaylistThreadResult;
   downloadPlaylistPreparedCount = found;
   downloadPlaylistMissingCount = 0;
   __sync_synchronize();
   if (found > 0 && downloadPlaylistResults) {
      for (int i = 0; i < found; i++) {
         char path[MAX_PATH_LEN];
         if (getDownloadedVideoPath(downloadPlaylistResults[i].videoId, path, sizeof path) != 0)
            downloadPlaylistMissingCount++;
      }
   }

   if (found < 0) {
      setLabelText(&downloadPlaylistConfirmTitle, LOC(STR_COULD_NOT_READ_PLAYLIST));
      setLabelText(&downloadPlaylistConfirmFooter, LOC(STR_CLOSE_HINT));
      return;
   }
   if (found == 0) {
      setLabelText(&downloadPlaylistConfirmTitle, LOC(STR_PLAYLIST_NO_DOWNLOADABLE_VIDEOS_TITLE));
      setLabelText(&downloadPlaylistConfirmFooter, LOC(STR_CLOSE_HINT));
      return;
   }
   if (downloadPlaylistMissingCount == 0) {
      setLabelText(&downloadPlaylistConfirmTitle, LOC(STR_ALL_PLAYLIST_VIDEOS_DOWNLOADED));
      setLabelText(&downloadPlaylistConfirmFooter, LOC(STR_SAVE_PLAYLIST_CANCEL));
      return;
   }

   char estimate[48], prompt[256];
   formatDownloadSpace(home.downloadPlaylistEstimatedBytes, estimate, sizeof estimate);
   if (home.downloadPlaylistEstimateUnknown > 0 && home.downloadPlaylistEstimatedBytes == 0)
      snprintf(prompt, sizeof prompt, LOC(STR_PLAYLIST_DOWNLOAD_PROMPT_UNKNOWN),
               downloadPlaylistJob.title, downloadPlaylistMissingCount,
               home.downloadPlaylistEstimateUnknown);
   else if (home.downloadPlaylistEstimateUnknown > 0)
      snprintf(prompt, sizeof prompt, LOC(STR_PLAYLIST_DOWNLOAD_PROMPT_ESTIMATE_UNKNOWN),
               downloadPlaylistJob.title, downloadPlaylistMissingCount, estimate,
               home.downloadPlaylistEstimateUnknown);
   else
      snprintf(prompt, sizeof prompt, LOC(STR_PLAYLIST_DOWNLOAD_PROMPT_ESTIMATE),
               downloadPlaylistJob.title, downloadPlaylistMissingCount, estimate);
   setLabelText(&downloadPlaylistConfirmTitle, prompt);
   setLabelText(&downloadPlaylistConfirmFooter, LOC(STR_DOWNLOAD_CANCEL));
}

static void openDownloadPlaylistConfirmation(const SearchResult *playlist)
{
   if (!playlist || !playlist->isPlaylist || downloadPlaylistThreadActive) return;
   free(downloadPlaylistResults);
   downloadPlaylistResults = NULL;
   home.downloadPlaylistPending = *playlist;
   downloadPlaylistJob = *playlist;
   downloadPlaylistThreadDone = 0;
   downloadPlaylistThreadResult = 0;
   downloadPlaylistPreparedCount = -1;
   downloadPlaylistMissingCount = 0;
   home.downloadPlaylistEstimatedBytes = 0;
   home.downloadPlaylistEstimateUnknown = 0;
   home.downloadPlaylistConfirmOpen = 1;
   downloadPlaylistPreparing = 1;
   downloadPlaylistOverlayRunning = 0;
   setLabelText(&downloadPlaylistConfirmTitle, LOC(STR_PREPARING_PLAYLIST_ESTIMATE));
   setLabelText(&downloadPlaylistConfirmFooter, LOC(STR_PLEASE_WAIT));
   pauseVideoGrid(&home.grid);
   if (spawnJoinableThread(&downloadPlaylistThread, downloadPlaylistWorker, 0,
                           THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                           "yt-playlist-est") == 0) {
      downloadPlaylistThreadActive = 1;
   } else {
      downloadPlaylistPreparing = 0;
      home.downloadPlaylistConfirmOpen = 0;
      resumeVideoGrid(&home.grid);
      setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_START_PLAYLIST_LOOKUP));
      playlistToastShownAt = getTimeUs();
   }
}

static void closeDownloadPlaylistConfirmation(int popOverlay)
{
   home.downloadPlaylistConfirmOpen = 0;
   downloadPlaylistPreparing = 0;
   downloadPlaylistPreparedCount = -1;
   downloadPlaylistMissingCount = 0;
   free(downloadPlaylistResults);
   downloadPlaylistResults = NULL;
   if (popOverlay && downloadPlaylistOverlayRunning) popScreen();
   downloadPlaylistOverlayRunning = 0;
}

static void submitPreparedDownloadPlaylist(int popOverlay)
{
   int found = downloadPlaylistPreparedCount;
   int playlistSaved = 0;
   if (found > 0 && downloadPlaylistResults &&
       (!downloadPlaylistJob.isLocalPlaylist || strncmp(downloadPlaylistJob.localPlaylistId, "dp", 2) != 0)) {
      char localId[LOCAL_PLAYLIST_ID_LEN];
      if (createDownloadPlaylistFromResults(downloadPlaylistJob.title, downloadPlaylistResults,
                                            found, localId, sizeof localId) == 0)
         playlistSaved = 1;
   }

   int missing = 0;
   for (int i = 0; downloadPlaylistResults && i < found; i++) {
      char path[MAX_PATH_LEN];
      if (getDownloadedVideoPath(downloadPlaylistResults[i].videoId, path, sizeof path) != 0)
         downloadPlaylistResults[missing++] = downloadPlaylistResults[i];
   }
   int queued = downloadPlaylistResults && missing > 0 ?
                enqueueDownloadBatch(downloadPlaylistResults, missing) : 0;
   free(downloadPlaylistResults);
   downloadPlaylistResults = NULL;

   if (found < 0) setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_READ_PLAYLIST));
   else if (queued == -1) setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_START_PLAYLIST_DOWNLOADS));
   else if (queued == -2) setLabelText(&playlistToastLabel, LOC(STR_DOWNLOAD_QUEUE_FULL));
   else if (queued == 0 && playlistSaved) setLabelText(&playlistToastLabel, LOC(STR_PLAYLIST_SAVED_NO_NEW_DOWNLOADS));
   else if (queued == 0) setLabelText(&playlistToastLabel, LOC(STR_NO_NEW_VIDEOS_TO_DOWNLOAD));
   else {
      char toast[96];
      snprintf(toast, sizeof toast, LOC(STR_QUEUED_PLAYLIST_VIDEOS), queued, queued == 1 ? "" : "s");
      setLabelText(&playlistToastLabel, toast);
   }
   playlistToastShownAt = getTimeUs();
   home.downloadPlaylistConfirmOpen = 0;
   downloadPlaylistPreparedCount = -1;
   downloadPlaylistMissingCount = 0;
   if (home.category == HOME_DOWNLOADS) loadCategory();
   if (popOverlay && downloadPlaylistOverlayRunning) popScreen();
   downloadPlaylistOverlayRunning = 0;
}

static void initDownloadPlaylistConfirmOverlay(void)
{
   // The Search/channel screen underneath remains on the stack and resumes unchanged.
   downloadPlaylistOverlayRunning = 1;
}

static void updateDownloadPlaylistConfirmOverlay(void)
{
   updateDownloadOverlay();
   if (downloadPlaylistPreparing && downloadPlaylistThreadActive && !downloadPlaylistThreadDone && downloadPlaylistEstimateTotal > 0) {
      char progress[128];
      snprintf(progress, sizeof progress, LOC(STR_ESTIMATING_DOWNLOAD_SIZE), downloadPlaylistEstimateProgress, downloadPlaylistEstimateTotal);
      setLabelText(&downloadPlaylistConfirmTitle, progress);
   }
   if (downloadPlaylistPreparing && downloadPlaylistThreadActive && downloadPlaylistThreadDone)
      finishDownloadPlaylistPreparation();
   if (!home.downloadPlaylistConfirmOpen || downloadPlaylistPreparing) return;
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      closeDownloadPlaylistConfirmation(1);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (downloadPlaylistPreparedCount < 0) return;
      submitPreparedDownloadPlaylist(1);
   }
}

static void drawDownloadPlaylistConfirmOverlay(void)
{
   if (!home.downloadPlaylistConfirmOpen) return;
   int screenW = getGfxScreenWidth(), screenH = getGfxScreenHeight();
   int panelW = screenW * 90 / 100;
   if (panelW > 1200) panelW = 1200;
   int panelH = (int)(260 * home.uiScale);
   int x = (screenW - panelW) / 2, y = (screenH - panelH) / 2;
   fillGfxRectangle(0, 0, screenW, screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&downloadPlaylistConfirmTitle);
   refreshLabelScale(&downloadPlaylistConfirmFooter);
   moveLabel(&downloadPlaylistConfirmTitle, x + 28, y + (int)(48 * home.uiScale));
   moveLabel(&downloadPlaylistConfirmFooter, x + 28, y + panelH - (int)(58 * home.uiScale));
   drawLabel(&downloadPlaylistConfirmTitle);
   drawLabel(&downloadPlaylistConfirmFooter);
}

void openHomeDownloadPlaylistConfirmation(const SearchResult *playlist)
{
   openDownloadPlaylistConfirmation(playlist);
   if (home.downloadPlaylistConfirmOpen) {
      downloadPlaylistOverlayRunning = 1;
      pushScreen(&downloadPlaylistConfirmOverlayScreen);
   }
}

static void drawDownloadCategoryTabs(void)
{
   int x = titleLabel.x + titleLabel.tt.tex.w + 14;
   int right = homeSortGroupLeft() - 12;
   int pad = TREND_CHIP_PAD_X, gap = TREND_CHIP_GAP;
   float statusFactor = 1.0f, categoryFactor = 1.0f;

   // Keep the three category pills beside Downloads. Shrink secondary disk text
   // first, then pill text, rather than allowing either group to overlap at 1.5x.
   for (int pass = 0; pass < 24; pass++) {
      for (int i = 0; i < 3; i++) setDownloadHeaderLabelScale(&downloadCategoryLabels[i], categoryFactor);
      setDownloadHeaderLabelScale(&downloadSpacePrefixLabel, statusFactor);
      setDownloadHeaderLabelScale(&downloadSpaceAvailableLabel, statusFactor);
      int categoryWidth = gap * 2;
      for (int i = 0; i < 3; i++) categoryWidth += downloadCategoryLabels[i].tt.tex.w + 2 * pad;
      int statusWidth = downloadSpacePrefixLabel.tt.tex.w + downloadSpaceAvailableLabel.tt.tex.w;
      if (x + categoryWidth + 12 + statusWidth <= right) break;
      if (statusFactor > 0.62f) statusFactor -= 0.04f;
      else if (categoryFactor > 0.70f) categoryFactor -= 0.04f;
      else if (pad > 3) pad--;
      else if (gap > 2) gap--;
      else break;
   }

   int statusWidth = downloadSpacePrefixLabel.tt.tex.w + downloadSpaceAvailableLabel.tt.tex.w;
   downloadSpaceStatusX = right - statusWidth;
   int textHeight = downloadSpacePrefixLabel.tt.tex.h > downloadSpaceAvailableLabel.tt.tex.h ?
                    downloadSpacePrefixLabel.tt.tex.h : downloadSpaceAvailableLabel.tt.tex.h;
   downloadSpaceStatusY = titleLabel.y + (titleLabel.tt.tex.h - textHeight) / 2;

   int y = titleLabel.y + (titleLabel.tt.tex.h - TREND_CHIP_HEIGHT) / 2;
   for (int i = 0; i < 3; i++) {
      Label *label = &downloadCategoryLabels[i];
      int width = label->tt.tex.w + 2 * pad;
      int active = i == home.downloadCategory;
      uint32_t edge = active ? activeTheme->accent : activeTheme->rowHighlight;
      uint32_t fill = active ? activeTheme->rowHighlight : activeTheme->surface;
      fillGfxRoundedRectangleFixed(x, y, width, TREND_CHIP_HEIGHT, TREND_CHIP_HEIGHT / 2, edge);
      fillGfxRoundedRectangleFixed(x + 1, y + 1, width - 2, TREND_CHIP_HEIGHT - 2,
                                   TREND_CHIP_HEIGHT / 2 - 1, fill);
      setLabelColor(label, active ? activeTheme->textPrimary : activeTheme->textSecondary);
      drawLabelAt(label, x + pad, getCenteredLabelY(label, y, TREND_CHIP_HEIGHT));
      x += width + gap;
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
      int bg = (int)((((activeTheme->appBg >> 16) & 255) * 299u +
                      ((activeTheme->appBg >> 8) & 255) * 587u +
                      (activeTheme->appBg & 255) * 114u) / 1000u);
      if (active && bg > 150) {
         int textX = x + (width - label->tt.tex.w) / 2;
         int textY = getCenteredLabelY(label, y, TREND_CHIP_HEIGHT);
         setLabelColor(label, 0xFF202020);
         drawLabelAt(label, textX - 2, textY); drawLabelAt(label, textX + 2, textY);
         drawLabelAt(label, textX - 1, textY - 1); drawLabelAt(label, textX + 1, textY - 1);
         drawLabelAt(label, textX - 1, textY + 1); drawLabelAt(label, textX + 1, textY + 1);
         setLabelColor(label, 0xFFFFFFFF);
         drawLabelAt(label, textX, textY);
      } else {
         setLabelColor(label, active ? activeTheme->textPrimary : activeTheme->textSecondary);
         moveLabel(label, x + pad, getCenteredLabelY(label, y, TREND_CHIP_HEIGHT));
         drawLabel(label);
      }
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
   home.downloadManagerOpen = 0;
   home.downloadManagerSelected = 0;
   home.downloadManagerScroll = 0;
   home.downloadManagerCategory = DOWNLOAD_MANAGEMENT_COMPLETED;
   home.downloadCategory = 0;
   home.downloadPlaylistConfirmOpen = 0;
   downloadPlaylistThreadActive = downloadPlaylistThreadDone = 0;
   downloadPlaylistThreadResult = 0;
   downloadPlaylistResults = NULL;
   home.downloadManagerSelectedCount = 0;
   home.downloadManagerConfirmOpen = 0;
   home.downloadManagerMessage[0] = 0;
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
   updateSettingsOpen = 0;
   updateSettingsSelected = 0;
   updateDialogOpen = 0;
   updateDialogSelected = 0;
   updateDialogDismissed = 0;
   updateNotesOpen = 0;
   updateNotesScroll = 0;
   home.themeOpen = 0;
   home.themeSelected = 0;
   home.themeCustomOpen = 0;
   home.themeCustomSelected = 0;
   home.themeCustomDark = 1;
   home.themeCustomDarkCursor = 1;
   home.themeCustomAccentIndex = 0;
   home.themeCustomAccentCursor = 0;
   home.themeConfirmOpen = 0;
   home.themeConfirmSelected = 1;
   home.themePendingPreset = THEME_PRESET_CRTUBE;
   home.downloadSettingsOpen = 0;
   home.downloadSettingsSelected = 0;
   home.exitConfirmOpen = 0;
   home.exitConfirmSelected = 1;
   home.regionOpen = 0;
   home.regionPickerLanguage = 0;
   home.languageConfirmOpen = 0;
   home.languageConfirmSelected = 1;
   home.languagePending = APP_LANGUAGE_EN;
   home.sponsorblockOpen = 0;
   home.sponsorblockSelected = 0;
   regionRevisionSeen = getContentRegionRevision();
   loadSdScale();
   home.screenW = getGfxScreenWidth();
   home.screenH = getGfxScreenHeight();
   home.uiScale = getGfxUiScaleFactor();
   if (home.uiScale < 1.0f) home.uiScale = 1.0f;
   sdHintsTwoRows = isGfxSdVideoOutput();
   int headerY = sdHintsTwoRows ? SD_HEADER_Y : HEADER_Y;
   int gridTop = sdHintsTwoRows ? SD_GRID_TOP : GRID_TOP;

   uiFont = openSystemFont(FONT_POP);
   initSidebar(&uiFont, home.screenW, home.screenH);
   int categoryTitleOffset = home.screenW * 3 / 100;
   initLabel(&titleLabel,  &uiFont, MARGIN_X + categoryTitleOffset, headerY,
             home.screenW - 2 * MARGIN_X - categoryTitleOffset, AUTO, TITLE_SIZE,
             activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&downloadSpacePrefixLabel, &uiFont, 0, 0, AUTO, AUTO, 18,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_USED_PREFIX));
   initLabel(&downloadSpaceAvailableLabel, &uiFont, 0, 0, AUTO, AUTO, 18,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_AVAILABLE_PREFIX));
   setLabelScaleWithUi(&downloadSpacePrefixLabel, 1);
   setLabelScaleWithUi(&downloadSpaceAvailableLabel, 1);
   downloadSpaceAvailableLow = 0;
   initLabel(&statusLabel, &uiFont, MARGIN_X, gridTop, home.screenW - 2 * MARGIN_X, AUTO, 21, activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_LOADING_ELLIPSIS));
   initLabel(&playlistErrorLabel, &uiFont, MARGIN_X, 0, home.screenW - 2 * MARGIN_X, AUTO, 18,
             activeTheme->textPrimary, TEXT_WRAP, LOC(STR_PLAYLIST_PRIVATE_UNAVAILABLE));
   initLabel(&playlistToastLabel, &uiFont, 0, 0, home.screenW - 80, AUTO, 23,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_PLAYLIST_ADDED_WATCH_LATER));
   int downloadPanelW = getDownloadManagerPanelWidth();
   initLabel(&downloadManagerTitle, &uiFont, 0, 0, downloadPanelW - 64, AUTO, 29,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_DOWNLOAD_MANAGEMENT));
   for (int i = 0; i < 10; i++) {
      initLabel(&downloadManagerRows[i], &uiFont, 0, 0, downloadPanelW - 220, AUTO, 23,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
      setLabelScaleWithUi(&downloadManagerRows[i], 1);
   }
   for (int i = 0; i < 7; i++) {
      initLabel(&downloadManagerStatuses[i], &uiFont, 0, 0, 128, AUTO, 21,
                activeTheme->textSecondary, TEXT_NOWRAP, "");
      setLabelScaleWithUi(&downloadManagerStatuses[i], 1);
   }
   initLabel(&downloadManagerFooter, &uiFont, 0, 0, downloadPanelW - 64, AUTO, 18,
             activeTheme->textSecondary, TEXT_NOWRAP, "");
   initButtonHints(&downloadManagerHints, &uiFont, 0, 20, 18, activeTheme->textSecondary);
   setButtonHintGap(&downloadManagerHints, 14);
   addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_DPAD_UP), LOC(STR_MOVE));
   addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_SELECT));
   addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_TRIANGLE), LOC(STR_ALL));
   downloadManagerSquareHint = addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_SQUARE), LOC(STR_DELETE));
   addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_L1), "/");
   addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_R1), LOC(STR_TABS));
   addButtonHint(&downloadManagerHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_BACK));

   setLabelScaleWithUi(&playlistErrorLabel, 1);
   setLabelScaleWithUi(&playlistToastLabel, 1);
   setLabelScaleWithUi(&downloadManagerTitle, 1);
   setLabelScaleWithUi(&downloadManagerFooter, 1);
   playlistToastShownAt = 0;
   // Home heading/status grow by rerasterising their font, while retaining their
   // original anchors instead of being pushed off the 4:3 safe area.
   setLabelScaleWithUi(&titleLabel, 1);
   const char *downloadCategoryNames[3] = { LOC(STR_ALL), LOC(STR_VIDEOS), LOC(STR_PLAYLISTS) };
   for (int i = 0; i < 3; i++) {
      initLabel(&downloadCategoryLabels[i], &uiFont, 0, 0, AUTO, AUTO, 18,
                activeTheme->textSecondary, TEXT_NOWRAP, downloadCategoryNames[i]);
      setLabelScaleWithUi(&downloadCategoryLabels[i], 1);
   }
   int downloadConfirmWidth = (int)((home.screenW * 0.90f - 56) / home.uiScale);
   if (downloadConfirmWidth > 760) downloadConfirmWidth = 760;
   if (downloadConfirmWidth < 320) downloadConfirmWidth = 320;
   initLabel(&downloadPlaylistConfirmTitle, &uiFont, 0, 0, downloadConfirmWidth, AUTO, 25,
             activeTheme->textPrimary, TEXT_WRAP, LOC(STR_DOWNLOAD_PLAYLIST_QUESTION));
   initLabel(&downloadPlaylistConfirmFooter, &uiFont, 0, 0, downloadConfirmWidth, AUTO, 19,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_DOWNLOAD_CANCEL));
   setLabelScaleWithUi(&downloadPlaylistConfirmTitle, 1);
   setLabelScaleWithUi(&downloadPlaylistConfirmFooter, 1);
   setLabelScaleWithUi(&statusLabel, 1);

   initButtonHints(&hints, &uiFont, 0, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_PLAY));
   homeSquareHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), LOC(STR_WATCH_LATER_PLAYLIST_HOLD));
   homeDownloadHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R3), LOC(STR_DOWNLOAD));
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), LOC(STR_CHANNEL));
   homeNewPlaylistHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L3), LOC(STR_NEW_PLAYLIST));
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT), LOC(STR_SEARCH));
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START), LOC(STR_SETTINGS));
   homeL1Hint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L1), "");
   homeR1Hint = addButtonHint(&hints, getConsoleGlyph(GLYPH_R1), LOC(STR_CATEGORY));
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R2), LOC(STR_REFRESH));
   homeSortHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_L2), LOC(STR_SORT));
   // Reserve footer space for the longer playlist-page caption as well.
   setButtonHintCaption(&hints, homeSortHint, LOC(STR_SHUFFLE));
   setButtonHintsStableLayout(&hints, 1);
   setButtonHintGap(&hints, 14);
   homeFooterBottomY = home.screenH - (sdHintsTwoRows ? SD_HINTS_BOTTOM : HINTS_BOTTOM);
   homeFooterRows = getButtonHintsFitRows(&hints, home.screenW, MAX_HINT_ROWS);
   if (homeFooterRows < 1) homeFooterRows = 1;
   float hintGlyphFinal = HINT_GLYPH_H * (sdHintsTwoRows ? SD_MAX_SCALE : 1.0f);
   int footerRowStep = (int)((hintGlyphFinal + HINT_ROW_GAP) * hints.fitScale + 0.5f);
   homeFooterTopY = homeFooterBottomY - (homeFooterRows - 1) * footerRowStep;

   int panelY = (home.screenH - getAccountPanelHeight()) / 2;
   int accountTextW = getAccountPanelWidth() - 40;
   int settingsTextW = getSettingsPanelWidth();
   initLabel(&panelTitle,  &uiFont, 0, 0, accountTextW, AUTO, 30, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelLine1,  &uiFont, 0, 0, accountTextW, AUTO, 22, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&panelLine2,  &uiFont, 0, 0, accountTextW, AUTO, 22, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&panelCode,   &uiFont, 0, 0, accountTextW, AUTO, 56, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   initLabel(&panelFooter, &uiFont, 0, 0, accountTextW, AUTO, 20, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&settingsTitle, &uiFont, 0, 0, settingsTextW - 48, AUTO, 27, activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_SETTINGS));
   initLabel(&themeTitle, &uiFont, 0, 0, THEME_PANEL_W - 48, AUTO, 28,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_THEMES));
   for (int i = 0; i < THEME_ROW_COUNT; i++) {
      const char *rowName = i == THEME_EDIT_CUSTOM_ROW ? LOC(STR_EDIT_CUSTOM) :
         (i == 0 ? LOC(STR_THEME_CRTUBE) : i == 1 ? LOC(STR_THEME_BLUE) :
          i == 2 ? LOC(STR_THEME_GREEN) : i == 3 ? LOC(STR_THEME_LIGHT) :
          i == 4 ? LOC(STR_THEME_MONOCHROME) : getThemePresetName(i));
      initLabel(&themeRowLabels[i], &uiFont, 0, 0, 420, AUTO, 21,
                activeTheme->textPrimary, TEXT_NOWRAP, rowName);
      setLabelScaleWithUi(&themeRowLabels[i], 1);
   }
   initLabel(&themeLegendLabels[0], &uiFont, 0, 0, 64, AUTO, 14,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_BACK_UPPER));
   initLabel(&themeLegendLabels[1], &uiFont, 0, 0, 54, AUTO, 14,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_ACCOUNT_ABBREVIATION));
   initLabel(&themeSelectionLeft, &uiFont, 0, 0, 64, AUTO, 24,
             activeTheme->accent, TEXT_NOWRAP, ">");
   initLabel(&themeSelectionRight, &uiFont, 0, 0, 64, AUTO, 24,
             activeTheme->accent, TEXT_NOWRAP, "<");
   initLabel(&themeCustomTitle, &uiFont, 0, 0, THEME_PANEL_W - 48, AUTO, 28,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_CUSTOM_THEME));
   initLabel(&themeCustomLabels[0], &uiFont, 0, 0, 240, AUTO, 22,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_BACKGROUND));
   initLabel(&themeCustomLabels[1], &uiFont, 0, 0, 240, AUTO, 22,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_ACCENT_COLOUR));
   initLabel(&themeCustomLabels[2], &uiFont, 0, 0, 300, AUTO, 22,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_APPLY));
   initLabel(&themeCustomOptions[0], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_DARK));
   initLabel(&themeCustomOptions[1], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_LIGHT));
   initLabel(&themeConfirmTitle, &uiFont, 0, 0, 560, AUTO, 28,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_CONFIRM_THEME_CHANGE));
   initLabel(&themeConfirmMessage, &uiFont, 0, 0, 560, AUTO, 20,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_APPLY_THEME_QUESTION));
   initLabel(&themeConfirmChoices[0], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_YES));
   initLabel(&themeConfirmChoices[1], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_NO));
   setLabelScaleWithUi(&themeConfirmTitle, 1);
   setLabelScaleWithUi(&themeConfirmMessage, 1);
   setLabelScaleWithUi(&themeConfirmChoices[0], 1);
   setLabelScaleWithUi(&themeConfirmChoices[1], 1);
   setLabelScaleWithUi(&themeTitle, 1);
   setLabelScaleWithUi(&themeCustomTitle, 1);
   setLabelScaleWithUi(&themeConfirmTitle, 1);
   setLabelScaleWithUi(&themeConfirmMessage, 1);
   for (int i = 0; i < 2; i++) {
      setLabelScaleWithUi(&themeLegendLabels[i], 1);
      setLabelScaleWithUi(&themeCustomOptions[i], 1);
      setLabelScaleWithUi(&themeConfirmChoices[i], 1);
   }
   setLabelScaleWithUi(&themeSelectionLeft, 1);
   setLabelScaleWithUi(&themeSelectionRight, 1);
   for (int i = 0; i < 3; i++) setLabelScaleWithUi(&themeCustomLabels[i], 1);
   initButtonHints(&themeHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&themeHints, getConsoleGlyph(GLYPH_DPAD_UP), NULL);
   addButtonHint(&themeHints, getConsoleGlyph(GLYPH_DPAD_DOWN), LOC(STR_MOVE));
   addButtonHint(&themeHints, getConsoleGlyph(GLYPH_DPAD_LEFT), NULL);
   addButtonHint(&themeHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), LOC(STR_CHANGE));
   addButtonHint(&themeHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_SELECT));
   addButtonHint(&themeHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_BACK));
   const char *settingNames[SETTINGS_ROW_COUNT] = {
      LOC(STR_AUTOPLAY), LOC(STR_ENABLE_1080P60), LOC(STR_UI_SCALING),
      LOC(STR_OVERSCAN), LOC(STR_OVERSCAN_CROP), LOC(STR_CONTENT_REGION), LOC(STR_APP_LANGUAGE),
      LOC(STR_SPONSORBLOCK), LOC(STR_DOWNLOAD_SETTINGS), LOC(STR_THEMES),
      LOC(STR_SCREENSAVER_TIMEOUT), LOC(STR_UPDATES_ABOUT)
   };
   setLabelScaleWithUi(&settingsTitle, 1);
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      initLabel(&settingsLabels[i], &uiFont, 0, 0, settingsTextW - 250, AUTO, 23,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, settingNames[i]);
      initLabel(&settingsValues[i], &uiFont, 0, 0, 260, AUTO, 24,
                activeTheme->textPrimary, TEXT_NOWRAP, "");
      setLabelScaleWithUi(&settingsLabels[i], 1);
      setLabelScaleWithUi(&settingsValues[i], 1);
   }
   initLabel(&updatePanelTitle, &uiFont, 0, 0, UPDATE_PANEL_W - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_UPDATES_ABOUT));
   for (int i = 0; i < 3; i++)
      initLabel(&updatePanelInfo[i], &uiFont, 0, 0, UPDATE_PANEL_W - 68, AUTO, 18,
                activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   for (int i = 0; i < 5; i++)
      initLabel(&updatePanelRows[i], &uiFont, 0, 0, UPDATE_PANEL_W - 80, AUTO, 20,
                activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&updatePanelRows[5], &uiFont, 0, 0, UPDATE_PANEL_W - 80, AUTO, 16,
             activeTheme->textSecondary, TEXT_WRAP, "");
   initLabel(&updateDialogTitle, &uiFont, 0, 0, UPDATE_DIALOG_W - 48, AUTO, 25,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_NEW_UPDATE_AVAILABLE));
   initLabel(&updateDialogVersion, &uiFont, 0, 0, UPDATE_DIALOG_W - 48, AUTO, 20,
             activeTheme->textSecondary, TEXT_NOWRAP, "");
   for (int i = 0; i < 6; i++)
      initLabel(&updateDialogRows[i], &uiFont, 0, 0, UPDATE_DIALOG_W - 80, AUTO, 19,
                activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&updateNotesTitle, &uiFont, 0, 0, UPDATE_NOTES_W - 120, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_RELEASE_NOTES));
   initLabel(&updateNotesPageLabel, &uiFont, 0, 0, 120, AUTO, 18,
             activeTheme->textSecondary, TEXT_NOWRAP, "1/1");
   for (int i = 0; i < UPDATE_NOTES_VISIBLE_LINES; i++)
      initLabel(&updateNotesLines[i], &uiFont, 0, 0, UPDATE_NOTES_W - 64, AUTO, 23,
                activeTheme->textPrimary, TEXT_NOWRAP, "");
   initButtonHints(&updateHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&updateHints, getConsoleGlyph(GLYPH_DPAD_UP), LOC(STR_PAGE));
   addButtonHint(&updateHints, getConsoleGlyph(GLYPH_DPAD_DOWN), LOC(STR_PAGE));
   addButtonHint(&updateHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_SELECT));
   addButtonHint(&updateHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_BACK));
   initLabel(&downloadSettingsTitle, &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_DOWNLOAD_SETTINGS));
   const char *downloadSettingNames[DOWNLOAD_SETTINGS_ROW_COUNT] = {
      LOC(STR_VIDEO_RESOLUTION), LOC(STR_AUDIO_LANGUAGE), LOC(STR_AUDIO_SOURCE), LOC(STR_PRIMARY_SUBTITLES), LOC(STR_SECONDARY_SUBTITLES_OPTIONAL)
   };
   for (int i = 0; i < DOWNLOAD_SETTINGS_ROW_COUNT; i++) {
      initLabel(&downloadSettingsLabels[i], &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W / 2 - 50, AUTO, 19,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, downloadSettingNames[i]);
      initLabel(&downloadSettingsValues[i], &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W / 2 - 40, AUTO, 19,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   }
   initLabel(&downloadSettingsNotice, &uiFont, 0, 0, DOWNLOAD_SETTINGS_PANEL_W - 68, AUTO, 14,
             activeTheme->textSecondary, TEXT_WRAP,
             LOC(STR_DOWNLOAD_AUDIO_SUBTITLE_NOTICE));
   setLabelScaleWithUi(&downloadSettingsTitle, 1);
   setLabelScaleWithUi(&downloadSettingsNotice, 1);
   for (int i = 0; i < DOWNLOAD_SETTINGS_ROW_COUNT; i++) { setLabelScaleWithUi(&downloadSettingsLabels[i], 1); setLabelScaleWithUi(&downloadSettingsValues[i], 1); }
   initButtonHints(&downloadSettingsHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_DPAD_DOWN), LOC(STR_MOVE));
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_DPAD_LEFT), LOC(STR_CHANGE));
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), LOC(STR_CHANGE));
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_CYCLE));
   addButtonHint(&downloadSettingsHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_BACK));

   initLabel(&settingsAutoplayOptions[0], &uiFont, 0, 0, 84, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_OFF_UPPER));
   initLabel(&settingsAutoplayOptions[1], &uiFont, 0, 0, 84, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_ON_UPPER));
   setLabelScaleWithUi(&settingsAutoplayOptions[0], 1);
   setLabelScaleWithUi(&settingsAutoplayOptions[1], 1);
   initLabel(&regionTitle, &uiFont, 0, 0, getRegionPanelWidth() - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_CONTENT_REGION));
   initLabel(&regionDescription, &uiFont, 0, 0, getRegionPanelWidth() - 56, AUTO, 17,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_CONTENT_REGION_DESCRIPTION));
   initLabel(&regionPosition, &uiFont, 0, 0, 120, AUTO, 18,
             activeTheme->textSecondary, TEXT_NOWRAP, "1 / 33");
   setLabelScaleWithUi(&regionTitle, 1);
   setLabelScaleWithUi(&regionDescription, 1);
   setLabelScaleWithUi(&regionPosition, 1);
   for (int i = 0; i < getContentRegionCount() && i < REGION_MAX_ROWS; i++) {
      const ContentRegion *region = getContentRegionAt(i);
      initLabel(&regionNames[i], &uiFont, 0, 0, getRegionPanelWidth() - 190, AUTO, 21,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, region ? region->name : "");
      initLabel(&regionCodes[i], &uiFont, 0, 0, 88, AUTO, 20,
                activeTheme->textSecondary, TEXT_NOWRAP, region ? region->code : "");
      setLabelScaleWithUi(&regionNames[i], 1);
      setLabelScaleWithUi(&regionCodes[i], 1);
   }
   initLabel(&sponsorTitle, &uiFont, 0, 0, getSponsorPanelWidth() - 48, AUTO, 24,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_SPONSORBLOCK));
   initLabel(&sponsorDescription, &uiFont, 0, 0, getSponsorPanelWidth() - 56, AUTO, 15,
             activeTheme->textSecondary, TEXT_WRAP,
             LOC(STR_SPONSORBLOCK_DESCRIPTION));
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++) {
      initLabel(&sponsorCategoryLabels[i], &uiFont, 0, 0, 300, AUTO, 22,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, getSponsorCategoryName((SponsorCategory)i));
      setLabelScaleWithUi(&sponsorCategoryLabels[i], 1);
   }
   initLabel(&sponsorActionLabels[0], &uiFont, 0, 0, SPONSOR_OPTION_W, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_SKIP));
   initLabel(&sponsorActionLabels[1], &uiFont, 0, 0, SPONSOR_OPTION_W, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_ASK));
   initLabel(&sponsorActionLabels[2], &uiFont, 0, 0, SPONSOR_OPTION_W, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_KEEP));
   for (int i = 0; i < 3; i++) setLabelScaleWithUi(&sponsorActionLabels[i], 1);
   initButtonHints(&sponsorHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_DPAD_UP), LOC(STR_MOVE));
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_DPAD_LEFT), LOC(STR_CHANGE));
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), LOC(STR_CHANGE));
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_CYCLE));
   addButtonHint(&sponsorHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_BACK));

   initButtonHints(&regionHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_CHOOSE));
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_DPAD_UP), "");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_DPAD_DOWN), LOC(STR_MOVE));
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_L2), "");
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_R2), LOC(STR_PAGE));
   addButtonHint(&regionHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_BACK));
   // Use native PS3 controller glyphs instead of textual button names.
   // The three action glyphs are grouped before their shared LOC(STR_CHANGE) caption.
   initButtonHints(&settingsHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_OPEN_SELECT));
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_DPAD_LEFT), "");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), LOC(STR_CHANGE));
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_CLOSE));

   initLabel(&exitConfirmTitle, &uiFont, 0, 0, EXIT_CONFIRM_PANEL_W - 48, AUTO, 28,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_EXIT_CRTUBE));
   initLabel(&exitConfirmMessage, &uiFont, 0, 0, EXIT_CONFIRM_PANEL_W - 48, AUTO, 20,
             activeTheme->textSecondary, TEXT_NOWRAP, LOC(STR_CONFIRM_CLOSE_APP));
   initLabel(&exitConfirmChoices[0], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_YES));
   initLabel(&exitConfirmChoices[1], &uiFont, 0, 0, 100, AUTO, 20,
             activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_NO));
   initButtonHints(&exitConfirmHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_CHOOSE));
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_DPAD_LEFT), LOC(STR_CHANGE));
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "");
   addButtonHint(&exitConfirmHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_CANCEL));

   // Settings is a compact control panel; keep its typography at a stable
   // physical size even when the rest of the UI uses the 1.5x SD font scale.
   for (int i = 0; i < TREND_COUNT; i++) {
      initLabel(&trendingChipLabels[i], &uiFont, 0, 0, 220, AUTO, TREND_CHIP_FONT_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP, LOC(TrendingNameIds[i]));
      setLabelScaleWithUi(&trendingChipLabels[i], 1);
      initIcon(&trendingChipIcons[i], TrendingIcons[i], TREND_CHIP_ICON_SIZE);
   }
   for (int i = 0; i < LIVE_COUNT; i++) {
      initLabel(&liveChipLabels[i], &uiFont, 0, 0, 180, AUTO, TREND_CHIP_FONT_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP, LOC(LiveNameIds[i]));
      setLabelScaleWithUi(&liveChipLabels[i], 1);
   }
   const char *HomeSortNames[SORT_COUNT] = { LOC(STR_RELEVANCE), LOC(STR_VIEWS), LOC(STR_NEWEST) };
   for (int i = 0; i < SORT_COUNT; i++) {
      initLabel(&homeSortLabels[i], &uiFont, 0, 0, AUTO, AUTO, HOME_SORT_TEXT_SIZE,
                activeTheme->textSecondary, TEXT_NOWRAP, HomeSortNames[i]);
      setLabelScaleWithUi(&homeSortLabels[i], 1);
   }
   const char *PlaylistSortNames[PLAYLIST_SORT_COUNT] = { LOC(STR_NEWEST), LOC(STR_A_TO_Z) };
   const char *LocalPlaylistTabNames[2] = { LOC(STR_WATCH_LATER), LOC(STR_PS3) };
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
   panelConfirmHint = addButtonHint(&panelHints, getConsoleGlyph(GLYPH_CROSS),  LOC(STR_TRY_AGAIN));
   panelBackHint    = addButtonHint(&panelHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_CLOSE));

   int gridHeight = homeFooterTopY - gridTop - (sdHintsTwoRows ? SD_GRID_BOTTOM_GAP : GRID_BOTTOM_GAP);
   if (gridHeight < 120) gridHeight = 120;
   initVideoGrid(&home.grid, &uiFont, MARGIN_X, gridTop, home.screenW - 2 * MARGIN_X, gridHeight);
   setGridWatchedPredicate(&home.grid, isWatched);
   setGridWatchedPosition(&home.grid, getWatchedPosition);
   setGridDownloadedPosition(&home.grid, getDownloadedPosition);
   setGridDownloadedWatchedPredicate(&home.grid, isDownloadedWatched);
   setGridWatchLaterPredicate(&home.grid, isWatchLater);
   setGridPlaylistWatchLaterPredicate(&home.grid, isPlaylistWatchLater);
   home.grid.suppressWatchLaterBadge = (home.category == HOME_WATCHLATER ||
      home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER ||
      home.category == HOME_DOWNLOADS ||
      (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 0));

   subsRevisionSeen = getSubscriptionsRevision();
   watchLaterRevisionSeen = getWatchLaterRevision();
   localPlaylistRevisionSeen = getLocalPlaylistRevision();
   downloadPlaylistRevisionSeen = getDownloadPlaylistRevision();
   playlistWatchLaterRevisionSeen = getPlaylistWatchLaterRevision();
   downloadLibraryRevisionSeen = getDownloadLibraryRevision();
   accountRevisionSeen = getAccountRevision();
   // signed in, start on the account's Recommended feed; otherwise on Subscriptions, as before
   home.category = isSignedIn() ? HOME_ACCOUNT + ACCOUNT_FEED_HOME : HOME_SUBS;
   updateHomeCategoryHints();
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
         if (home.category == HOME_WATCHLATER) {
            strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, squarePendingItem.videoId);
            home.playlistDeleteFromWatchLater = 3;
            home.playlistDeleteConfirmOpen = 1;
            home.playlistDeleteConfirmSelected = 1;
            exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
         } else {
            toggleWatchLater(&squarePendingItem);
            watchLaterRevisionSeen = getWatchLaterRevision();
            categoryCached[HOME_WATCHLATER] = 0;
            if (home.category == HOME_WATCHLATER) {
               home.restoreGridAfterWatchLater = 1;
               home.restoreGridSelection = home.grid.selected;
               home.restoreGridScrollRow = home.grid.scrollRow;
               loadCategory();
            }
         }
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
         setLabelText(&playlistToastLabel, LOC(STR_PLAYLIST_ADDED_WATCH_LATER));
         if (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 0) loadCategory();
      }
      return 1;
   }
   squarePendingItem = *selected;
   squarePressedAt = getTimeUs();
   return 1;
}

static int downloadManagerIsSelected(const char *videoId)
{
   for (int i = 0; i < home.downloadManagerSelectedCount; i++)
      if (strcmp(home.downloadManagerSelectedIds[i], videoId) == 0) return 1;
   return 0;
}

static void downloadManagerClearSelection(void)
{
   home.downloadManagerSelectedCount = 0;
}

static void downloadManagerToggleSelection(const char *videoId)
{
   for (int i = 0; i < home.downloadManagerSelectedCount; i++) {
      if (strcmp(home.downloadManagerSelectedIds[i], videoId) == 0) {
         for (int j = i + 1; j < home.downloadManagerSelectedCount; j++)
            strCopy(home.downloadManagerSelectedIds[j - 1], 16, home.downloadManagerSelectedIds[j]);
         home.downloadManagerSelectedCount--;
         return;
      }
   }
   if (home.downloadManagerSelectedCount < DOWNLOAD_MANAGEMENT_MAX_ITEMS) {
      strCopy(home.downloadManagerSelectedIds[home.downloadManagerSelectedCount], 16, videoId);
      home.downloadManagerSelectedCount++;
   }
}

static void downloadManagerSelectAll(void)
{
   char ids[DOWNLOAD_MANAGEMENT_MAX_ITEMS][16];
   int count = getDownloadManagementIds(home.downloadManagerCategory, ids,
                                        DOWNLOAD_MANAGEMENT_MAX_ITEMS);
   int allSelected = count == home.downloadManagerSelectedCount;
   for (int i = 0; i < count && allSelected; i++) {
      int found = 0;
      for (int j = 0; j < home.downloadManagerSelectedCount; j++)
         if (strcmp(ids[i], home.downloadManagerSelectedIds[j]) == 0) { found = 1; break; }
      if (!found) allSelected = 0;
   }
   if (allSelected) {
      downloadManagerClearSelection();
      return;
   }
   home.downloadManagerSelectedCount = count;
   for (int i = 0; i < count; i++)
      strCopy(home.downloadManagerSelectedIds[i], 16, ids[i]);
}

static void downloadManagerConfirmAction(void)
{
   int requested = home.downloadManagerSelectedCount;
   if (requested <= 0) {
      home.downloadManagerConfirmOpen = 0;
      return;
   }

   if (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_ONGOING) {
      int accepted = 0, rejected = 0;
      for (int i = 0; i < requested; i++) {
         int rc = cancelManagedDownload(home.downloadManagerSelectedIds[i]);
         if (rc == 0) accepted++;
         else rejected++;
      }
      snprintf(home.downloadManagerMessage, sizeof home.downloadManagerMessage,
               rejected ? LOC(STR_CANCEL_REQUESTED_FORMAT) : LOC(STR_CANCELLATION_REQUESTED_FORMAT),
               accepted, rejected);
   } else {
      const char *ids[DOWNLOAD_MANAGEMENT_MAX_ITEMS];
      for (int i = 0; i < requested; i++) ids[i] = home.downloadManagerSelectedIds[i];
      if (queueDeleteManagedDownloads(ids, requested) == 0) {
         for (int i = 0; i < requested; i++)
            removeVideoFromAllDownloadPlaylists(home.downloadManagerSelectedIds[i]);
         snprintf(home.downloadManagerMessage, sizeof home.downloadManagerMessage,
                  LOC(STR_DELETING_ITEMS_FORMAT), requested, requested == 1 ? "" : "s");
      } else {
         strCopy(home.downloadManagerMessage, sizeof home.downloadManagerMessage,
                 LOC(STR_COULD_NOT_QUEUE_DELETION));
      }
   }

   downloadManagerClearSelection();
   home.downloadManagerConfirmOpen = 0;
}

static void updateDownloadManagement(void)
{
   // This callback is on the UI thread: only copy bounded cache/queue snapshots,
   // mutate selection state, and enqueue operations. Never add VFS calls here.
   // A download may be paused at its disk-space confirmation while this modal is
   // open. Keep servicing that prompt so the download worker cannot wait forever.
   updateDownloadOverlay();
   if (isDownloadSpacePromptOpen()) return;

   DownloadManagementItem items[DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS];
   int count = 0, loading = 0;
   int visible = getDownloadManagementItems(home.downloadManagerCategory,
                                             home.downloadManagerScroll, items,
                                             DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS,
                                             &count, &loading);
   if (loading && home.downloadManagerCategory != DOWNLOAD_MANAGEMENT_ONGOING)
      downloadManagerClearSelection();
   if (count <= 0) home.downloadManagerSelected = home.downloadManagerScroll = 0;
   else {
      if (home.downloadManagerSelected >= count) home.downloadManagerSelected = count - 1;
      if (home.downloadManagerSelected < 0) home.downloadManagerSelected = 0;
      int maxScroll = count > DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS ?
                      count - DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS : 0;
      if (home.downloadManagerScroll > maxScroll) home.downloadManagerScroll = maxScroll;
      if (home.downloadManagerSelected < home.downloadManagerScroll)
         home.downloadManagerScroll = home.downloadManagerSelected;
      if (home.downloadManagerSelected >= home.downloadManagerScroll + DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS)
         home.downloadManagerScroll = home.downloadManagerSelected - DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS + 1;
   }

   if (downloadManagerSquareHint >= 0)
      setButtonHintCaption(&downloadManagerHints, downloadManagerSquareHint,
         home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_ONGOING ? LOC(STR_CANCEL) : LOC(STR_DELETE));

   if (home.downloadManagerConfirmOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
         home.downloadManagerConfirmOpen = 0;
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CROSS))
         downloadManagerConfirmAction();
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_R3)) {
      home.downloadManagerOpen = 0;
      return;
   }

   if (isPadButtonPressed(PAD_BTN_L1) || isPadButtonPressed(PAD_BTN_R1)) {
      int direction = isPadButtonPressed(PAD_BTN_R1) ? 1 : -1;
      home.downloadManagerCategory =
         (home.downloadManagerCategory + 3 + direction) % 3;
      home.downloadManagerSelected = home.downloadManagerScroll = 0;
      downloadManagerClearSelection();
      home.downloadManagerMessage[0] = 0;
      return;
   }

   int previousScroll = home.downloadManagerScroll;
   if (isRepeatDue(&regionNavigateRepeat, getPadButtonState(PAD_BTN_UP))) {
      if (home.downloadManagerSelected > 0) home.downloadManagerSelected--;
   } else if (isRepeatDue(&regionNavigateRepeat, getPadButtonState(PAD_BTN_DOWN))) {
      if (home.downloadManagerSelected + 1 < count) home.downloadManagerSelected++;
   }
   if (home.downloadManagerSelected < home.downloadManagerScroll)
      home.downloadManagerScroll = home.downloadManagerSelected;
   else if (home.downloadManagerSelected >= home.downloadManagerScroll + DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS)
      home.downloadManagerScroll = home.downloadManagerSelected - DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS + 1;
   if (previousScroll != home.downloadManagerScroll)
      visible = getDownloadManagementItems(home.downloadManagerCategory,
                                            home.downloadManagerScroll, items,
                                            DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS,
                                            &count, &loading);

   int canMutate = !loading || home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_ONGOING;
   if (canMutate && isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      downloadManagerSelectAll();
      home.downloadManagerMessage[0] = 0;
   }

   if (canMutate && isPadButtonPressed(PAD_BTN_CROSS) && home.downloadManagerSelected >= home.downloadManagerScroll &&
       home.downloadManagerSelected < home.downloadManagerScroll + visible) {
      int row = home.downloadManagerSelected - home.downloadManagerScroll;
      if (items[row].videoId[0]) {
         downloadManagerToggleSelection(items[row].videoId);
         home.downloadManagerMessage[0] = 0;
      }
   }

   if (canMutate && isPadButtonPressed(PAD_BTN_SQUARE)) {
      if (home.downloadManagerSelectedCount == 0 &&
          home.downloadManagerSelected >= home.downloadManagerScroll &&
          home.downloadManagerSelected < home.downloadManagerScroll + visible) {
         int row = home.downloadManagerSelected - home.downloadManagerScroll;
         if (items[row].videoId[0]) downloadManagerToggleSelection(items[row].videoId);
      }
      if (home.downloadManagerSelectedCount > 0) {
         home.downloadManagerConfirmOpen = 1;
               exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
      }
   }

   if (loading == 2)
      strCopy(home.downloadManagerMessage, sizeof home.downloadManagerMessage, LOC(STR_LIBRARY_UNAVAILABLE));
   else if (loading == 1 &&
            (!home.downloadManagerMessage[0] ||
             strcmp(home.downloadManagerMessage, LOC(STR_LIBRARY_UNAVAILABLE)) == 0))
      strCopy(home.downloadManagerMessage, sizeof home.downloadManagerMessage, LOC(STR_LOADING_LIBRARY));
   else if (!loading &&
            (strcmp(home.downloadManagerMessage, LOC(STR_LOADING_LIBRARY)) == 0 ||
             strcmp(home.downloadManagerMessage, LOC(STR_LIBRARY_UNAVAILABLE)) == 0 ||
             strncmp(home.downloadManagerMessage, LOC(STR_DELETING_PREFIX), strlen(LOC(STR_DELETING_PREFIX))) == 0))
      home.downloadManagerMessage[0] = 0;
}

static void openHomeDownloadManager(void)
{
   DownloadManagementItem firstOngoing;
   int ongoingCount = 0, managerLoading = 0;
   getDownloadManagementItems(DOWNLOAD_MANAGEMENT_ONGOING, 0, &firstOngoing, 1,
                              &ongoingCount, &managerLoading);
   home.downloadManagerOpen = 1;
   home.downloadManagerCategory = ongoingCount > 0 ?
      DOWNLOAD_MANAGEMENT_ONGOING : DOWNLOAD_MANAGEMENT_COMPLETED;
   home.downloadManagerSelected = 0;
   home.downloadManagerScroll = 0;
   home.downloadManagerSelectedCount = 0;
   home.downloadManagerMessage[0] = 0;
   requestDownloadManagementRefresh();
   regionNavigateRepeat.timer = regionNavigateRepeat.repeats = 0;
}

static void updateHome(void)
{
   updateHomeCategoryHints();
   static int previousUpdateDownloadState;
   int currentUpdateDownloadState = getUpdateDownloadState();
   if (previousUpdateDownloadState == 1 && currentUpdateDownloadState != 1) {
      playlistToastShownAt = getTimeUs();
      setLabelText(&playlistToastLabel, currentUpdateDownloadState == 2 ?
                   LOC(STR_UPDATE_DOWNLOAD_COMPLETE) : LOC(STR_UPDATE_DOWNLOAD_FAILED));
   }
   previousUpdateDownloadState = currentUpdateDownloadState;
   if (updateOverwritePromptOpen && updateDialogOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
         logInfo("[update-ui] confirmation cancelled type=DOWNLOAD_OVERWRITE\n");
         updateOverwritePromptOpen = 0;
      } else {
         if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_RIGHT) ||
             isPadButtonPressed(PAD_BTN_UP) || isPadButtonPressed(PAD_BTN_DOWN))
            updateOverwriteChoice = !updateOverwriteChoice;
         if (isPadButtonPressed(PAD_BTN_CROSS)) {
            logInfo("[update-ui] confirmation submit type=DOWNLOAD_OVERWRITE choice=%s\n",
                    updateOverwriteChoice == 0 ? LOC(STR_YES_UPPER) : LOC(STR_NO_UPPER));
            if (updateOverwriteChoice == 0 && startUpdateDownloadOverwrite() != 0) {
               playlistToastShownAt = getTimeUs();
               setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_RESTART_UPDATE_DOWNLOAD));
            }
            updateOverwritePromptOpen = 0;
         }
      }
      return;
   }
   if (updateNotesOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) updateNotesOpen = 0;
      else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)) ||
               isPadButtonPressed(PAD_BTN_RIGHT)) {
         int pageCount = (updateNotesLineCount + UPDATE_NOTES_VISIBLE_LINES - 1) / UPDATE_NOTES_VISIBLE_LINES;
         if (updateNotesScroll / UPDATE_NOTES_VISIBLE_LINES + 1 < pageCount)
            updateNotesScroll += UPDATE_NOTES_VISIBLE_LINES;
      }
      else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)) ||
               isPadButtonPressed(PAD_BTN_LEFT)) {
         if (updateNotesScroll >= UPDATE_NOTES_VISIBLE_LINES)
            updateNotesScroll -= UPDATE_NOTES_VISIBLE_LINES;
      }
      return;
   }
   if (updateDialogOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) {
         updateDialogOpen = 0;
         updateDialogDismissed = 1;
      }
      else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
         updateDialogSelected = (updateDialogSelected + 1) % 5;
      else if (isRepeatDue(&settingsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
         updateDialogSelected = (updateDialogSelected + 4) % 5;
      else if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (updateDialogSelected == 0) { updateNotesOpen = 1; updateNotesScroll = 0; buildUpdateNotesLines(); }
         else if (updateDialogSelected == 1) {
            logInfo("[update-ui] automatic dialog action=DOWNLOAD download_state=%d\n", getUpdateDownloadState());
            if (getUpdateDownloadState() == 1) {
               playlistToastShownAt = getTimeUs();
               setLabelText(&playlistToastLabel, LOC(STR_UPDATE_DOWNLOAD_ALREADY_RUNNING));
            } else if (isUpdateDownloaded()) {
               updateOverwritePromptOpen = 1;
               updateOverwriteChoice = 0;
               logInfo("[update-ui] automatic dialog opened overwrite confirmation\n");
            } else {
               int downloadRc = startUpdateDownload();
               logInfo("[update-ui] automatic dialog download start rc=%d state=%d\n", downloadRc, getUpdateDownloadState());
               if (downloadRc != 0) {
                  playlistToastShownAt = getTimeUs();
                  setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_START_UPDATE_DOWNLOAD));
               }
            }
         }
         else if (updateDialogSelected == 2) {
            disableAutomaticUpdateChecks(); updateDialogOpen = 0; updateDialogDismissed = 1;
         } else if (updateDialogSelected == 3) {
            ignoreCurrentUpdate(); updateDialogOpen = 0; updateDialogDismissed = 1;
         } else { updateDialogOpen = 0; updateDialogDismissed = 1; }
      }
      return;
   }
   // Handle START before updateVideoGrid() can reap/decode grid work. The worker
   // continues independently while the Settings panel owns the UI.
   if (!home.settingsOpen && !home.accountOpen && !home.exitConfirmOpen &&
       !home.downloadManagerOpen && !home.playlistDeleteConfirmOpen &&
       !updateDialogOpen && !updateNotesOpen && !isDownloadSpacePromptOpen() &&
       isPadButtonPressed(PAD_BTN_START)) {
      openHomeSettingsPanel();
      return;
   }
   if (!home.settingsOpen && !home.accountOpen && !home.exitConfirmOpen &&
       !home.downloadManagerOpen && !home.playlistDeleteConfirmOpen &&
       !updateDialogDismissed && shouldNotifyUpdate()) {
      updateDialogOpen = 1;
      updateDialogSelected = 0;
      settingsNavigateRepeat.timer = settingsNavigateRepeat.repeats = 0;
   }
   setPlaylistShuffleContext(homeIsPlaylistContents());
   updateDownloadOverlay();
   if (isDownloadSpacePromptOpen()) return;
   unsigned int downloadRevision = getDownloadLibraryRevision();
   if (downloadRevision != downloadLibraryRevisionSeen) {
      downloadLibraryRevisionSeen = downloadRevision;
      if (home.category == HOME_DOWNLOADS ||
          (home.category == HOME_LOCAL_PLAYLIST_CONTENTS && home.playlistReturnCategory == HOME_DOWNLOADS)) {
         loadCategory();
         return;
      }
   }
   if (home.category == HOME_DOWNLOADS) updateDownloadSpaceStats();
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
      if (!home.settingsOpen && isPadButtonPressed(PAD_BTN_CIRCLE)) {
         cancelHttpRequestGroup(&home.channelLookupGroup);
         home.channelLookupPending = 0;
         resumeVideoGrid(&home.grid);
         logInfo("[home] channel lookup cancelled by CIRCLE\n");
         return;
      }
      // Grid and channel-ID lookup use independent request groups. Do not wait
      // for the grid worker before starting this lookup: both run off the UI thread.
      if (!home.channelLookupActive && !home.channelLookupDone) {
         if (spawnJoinableThread(&home.channelLookupTid, channelLookupWorker, 0,
                                 THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-channel-id") == 0)
            home.channelLookupActive = 1;
         else {
            home.channelLookupResult = -1;
            home.channelLookupDone = 1;
         }
      }
      if (home.settingsOpen) {
         updateSettingsPanel();
         return;
      }
      if (home.channelLookupDone) {
         if (home.channelLookupActive) {
            joinThread(home.channelLookupTid);
            home.channelLookupActive = 0;
         }
         // Only wait asynchronously for jobs that can still mutate SearchResults.
         // Thumbnail/cache jobs do not own result data and must not delay channel entry.
         if (home.grid.threadActive && !home.grid.workerDone &&
             (home.grid.job == JOB_MORE || home.grid.job == JOB_MEMBER_CHECK)) return;
         if (home.grid.threadActive && home.grid.workerDone) updateVideoGrid(&home.grid);
         home.channelLookupPending = 0;
         resumeVideoGrid(&home.grid);
         if (home.channelLookupResult == 0 && home.channelLookupItem.channelId[0]) {
            SearchResult channelItem = home.channelLookupItem;
            logInfo("[home] Triangle: async channel lookup resolved %s\n", channelItem.channelId);
            cacheCurrent();
            openChannelForCategory(&channelItem, home.playlistReturnChannelCategory);
         } else {
            logWarn("[home] Triangle: async channel lookup failed video=%s\n",
                    home.channelLookupItem.videoId);
            setLabelText(&playlistToastLabel, LOC(STR_CHANNEL_UNAVAILABLE));
            playlistToastShownAt = getTimeUs();
         }
         return;
      }
      return;
   }

   if (home.channelLookupActive && home.channelLookupDone && !home.channelLookupPending) {
      joinThread(home.channelLookupTid);
      home.channelLookupActive = 0;
   }
   if (downloadPlaylistPreparing && downloadPlaylistThreadActive && !downloadPlaylistThreadDone && downloadPlaylistEstimateTotal > 0) {
      char progress[128];
      snprintf(progress, sizeof progress, LOC(STR_ESTIMATING_DOWNLOAD_SIZE), downloadPlaylistEstimateProgress, downloadPlaylistEstimateTotal);
      setLabelText(&downloadPlaylistConfirmTitle, progress);
   }
   if (downloadPlaylistPreparing && downloadPlaylistThreadActive && downloadPlaylistThreadDone)
      finishDownloadPlaylistPreparation();
   if (home.downloadPlaylistConfirmOpen) {
      if (downloadPlaylistPreparing) return;
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
         closeDownloadPlaylistConfirmation(0);
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (downloadPlaylistPreparedCount >= 0) submitPreparedDownloadPlaylist(0);
         return;
      }
      return;
   }
   if (home.downloadManagerOpen) { updateDownloadManagement(); return; }
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
            } else if (home.playlistDeleteFromWatchLater == 2) {
               const char *deleteIds[1] = { home.playlistDeleteId };
               int deleteQueued = queueDeleteManagedDownloads(deleteIds, 1);
               if (deleteQueued == 0)
                  removeVideoFromAllDownloadPlaylists(home.playlistDeleteId);
            } else if (home.playlistDeleteFromWatchLater == 3) {
               SearchResult item; memset(&item, 0, sizeof item);
               strCopy(item.videoId, sizeof item.videoId, home.playlistDeleteId);
               toggleWatchLater(&item);
               watchLaterRevisionSeen = getWatchLaterRevision();
               categoryCached[HOME_WATCHLATER] = 0;
            } else if (home.playlistDeleteFromWatchLater == 4) {
               removeVideoFromLocalPlaylist(home.localPlaylistId, home.playlistDeleteId);
               localPlaylistRevisionSeen = getLocalPlaylistRevision();
            } else if (home.playlistDeleteFromWatchLater == 5) {
               uint64_t currentBytes = 0;
               int currentCount = 0;
               if (getDownloadPlaylistDeleteInfo(home.playlistDeleteId, &currentBytes, &currentCount) < 0) {
                  setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_MEASURE_PLAYLIST_FILES));
                  playlistToastShownAt = getTimeUs();
                  home.playlistDeleteConfirmOpen = 0;
                  return;
               }
               if (currentBytes != home.playlistDeleteBytes || currentCount != home.playlistDeleteVideoCount) {
                  home.playlistDeleteBytes = currentBytes;
                  home.playlistDeleteVideoCount = currentCount;
                  return;
               }
               if (deleteDownloadPlaylistAndVideos(home.playlistDeleteId) == 0) {
                  setLabelText(&playlistToastLabel, LOC(STR_PLAYLIST_AND_VIDEOS_DELETED));
                  playlistToastShownAt = getTimeUs();
               } else {
                  setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_DELETE_DOWNLOAD_PLAYLIST));
                  playlistToastShownAt = getTimeUs();
               }
            } else {
               deleteLocalPlaylist(home.playlistDeleteId);
               localPlaylistRevisionSeen = getLocalPlaylistRevision();
            }
            loadCategory();
         }
         home.playlistDeleteConfirmOpen = 0;
      }
      return;
   }
   if (home.category == HOME_DOWNLOADS && isPadButtonPressed(PAD_BTN_R3)) {
      openHomeDownloadManager();
      return;
   }
   if (home.createPlaylistOskDone) {
      home.createPlaylistOskDone = 0;
      if (home.createPlaylistName[0]) {
         int result = -1;
         if (home.playlistNameAction == 2) {
            result = strncmp(home.playlistNameTargetId, "dp", 2) == 0 ?
               renameDownloadPlaylist(home.playlistNameTargetId, home.createPlaylistName) :
               renameLocalPlaylist(home.playlistNameTargetId, home.createPlaylistName);
         } else if (home.playlistNameAction == 1) {
            char id[LOCAL_PLAYLIST_ID_LEN];
            result = home.category == HOME_DOWNLOADS ?
               createEmptyDownloadPlaylist(home.createPlaylistName, id, sizeof id) :
               createEmptyLocalPlaylist(home.createPlaylistName, id, sizeof id);
         }
         if (result == 0) {
            if (home.category != HOME_DOWNLOADS) {
               localPlaylistRevisionSeen = getLocalPlaylistRevision();
            } else {
            }
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
            switchToCategory(home.playlistReturnCategory == HOME_DOWNLOADS ? HOME_DOWNLOADS : HOME_LOCAL_PLAYLISTS);
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
               if (home.playlistReturnChannelId[0]) {
                  SearchResult channelItem;
                  memset(&channelItem, 0, sizeof channelItem);
                  strCopy(channelItem.channelId, sizeof channelItem.channelId, home.playlistReturnChannelId);
                  strCopy(channelItem.author, sizeof channelItem.author, home.playlistReturnChannelName);
                  home.playlistReturnChannelId[0] = 0;
                  home.playlistReturnChannelName[0] = 0;
                  if (returnCategory >= 0 && returnCategory != home.category)
                     switchToCategory(returnCategory);
                  openChannelForCategory(&channelItem, home.playlistReturnChannelCategory);
               } else {
                  if (returnCategory >= 0 && returnCategory != home.category)
                     switchToCategory(returnCategory);
                  openSearchQueryWithSort(query, returnSort);
               }
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
   if (isPadButtonPressed(PAD_BTN_R2)) { refreshCurrentCategory(); return; }
   if (isPadButtonPressed(PAD_BTN_L2) && homeIsPlaylistContents()) {
      setPlaylistShuffleEnabled(!isPlaylistShuffleEnabled());
      setPlaylistShuffleContext(1);
      playlistToastShownAt = getTimeUs();
      setLabelText(&playlistToastLabel,
                   isPlaylistShuffleEnabled() ? LOC(STR_PLAYLIST_SHUFFLE_ON) : LOC(STR_PLAYLIST_SHUFFLE_OFF));
      updateHomeSortHint();
      return;
   }
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

   updateHomeCategoryHints();

   int playlistPage = home.category == HOME_PLAYLISTS || home.category == HOME_PLAYLIST_CONTENTS ||
                      home.category == HOME_LOCAL_PLAYLISTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS ||
                      home.category == HOME_DOWNLOADS;
   if (homeDownloadHint >= 0) {
      const SearchResult *hintItem = gridSelected(&home.grid);
      int showDownload = !playlistPage || home.category == HOME_DOWNLOADS ||
                         home.category == HOME_PLAYLIST_CONTENTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS;
      if (hintItem && (home.category == HOME_PLAYLISTS || home.category == HOME_LOCAL_PLAYLISTS ||
                       home.category == HOME_DOWNLOADS)) showDownload = 1;
      setButtonHintShown(&hints, homeDownloadHint, showDownload);
      setButtonHintCaption(&hints, homeDownloadHint,
                           home.category == HOME_DOWNLOADS ? LOC(STR_MANAGE_DOWNLOADS) :
                           (hintItem && hintItem->isPlaylist ? LOC(STR_DOWNLOAD_PLAYLIST) : LOC(STR_DOWNLOAD)));
   }
   if (homeSquareHint >= 0) {
      if (home.category == HOME_WATCHLATER)
         setButtonHintCaption(&hints, homeSquareHint, LOC(STR_REMOVE_CONFIRM_PLAYLIST_HOLD));
      else if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS)
         setButtonHintCaption(&hints, homeSquareHint,
            home.playlistReturnCategory == HOME_DOWNLOADS ? LOC(STR_REMOVE_DELETE_HOLD) : LOC(STR_REMOVE_CONFIRM));
      else if (home.category == HOME_DOWNLOADS) {
         const SearchResult *downloadHintItem = gridSelected(&home.grid);
         setButtonHintCaption(&hints, homeSquareHint,
            downloadHintItem && downloadHintItem->isPlaylist ? LOC(STR_RENAME_DELETE_HOLD) : LOC(STR_ADD_PLAYLIST_DELETE_HOLD));
      }
      else if (home.category == HOME_LOCAL_PLAYLISTS)
         setButtonHintCaption(&hints, homeSquareHint,
                              home.localPlaylistTab == 0 ? LOC(STR_REMOVE_HOLD) : LOC(STR_RENAME_DELETE_HOLD));
      else if (home.category == HOME_PLAYLISTS)
         setButtonHintCaption(&hints, homeSquareHint, LOC(STR_SAVE_PLAYLIST));
      else
         setButtonHintCaption(&hints, homeSquareHint, LOC(STR_WATCH_LATER_PLAYLIST_HOLD));
   }
   if (home.category == HOME_DOWNLOADS && homeSquareHint >= 0) {
      const SearchResult *squareItem = gridSelected(&home.grid);
      setButtonHintShown(&hints, homeSquareHint, !squareItem || !squareItem->isPlaylist || squareItem->isLocalPlaylist);
   }
   if (homeNewPlaylistHint >= 0)
      setButtonHintShown(&hints, homeNewPlaylistHint,
                         (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 1) ||
                         (home.category == HOME_DOWNLOADS && home.downloadCategory != 1));
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
   if (((home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 1) ||
        (home.category == HOME_DOWNLOADS && home.downloadCategory != 1)) && isPadButtonPressed(PAD_BTN_L3)) {
      char defaultName[LOCAL_PLAYLIST_NAME_LEN]; snprintf(defaultName, sizeof defaultName, LOC(STR_PLAYLIST_NAME_FORMAT),
               (home.category == HOME_DOWNLOADS ? getDownloadPlaylistCount() : getLocalPlaylistCount()) + 1);
      home.playlistNameAction = 1;
      home.playlistNameTargetId[0] = 0;
      oskInputBegin(LOC(STR_NEW_PLAYLIST), defaultName, onHomePlaylistNameEntered); return;
   }
   if (gridStage(&home.grid) != GRID_READY) return;
   if (homeNewPlaylistHint >= 0) setButtonHintShown(&hints, homeNewPlaylistHint,
      (home.category == HOME_LOCAL_PLAYLISTS && home.localPlaylistTab == 1) ||
      (home.category == HOME_DOWNLOADS && home.downloadCategory != 1));
   const SearchResult *selected = gridSelected(&home.grid);
   if (!selected) return;

   // R3 in a downloaded playlist always opens the download manager.
   if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS &&
       home.playlistReturnCategory == HOME_DOWNLOADS &&
       isPadButtonPressed(PAD_BTN_R3)) {
      openHomeDownloadManager();
      return;
   }

   // Triangle opens the owning channel for regular videos and LIVE streams.
   // Live tiles are valid video targets too; excluding them silently disabled
   // the advertised Channel action on the Live navigation page.
   if (isPadButtonPressed(PAD_BTN_TRIANGLE) &&
       selected->videoId[0] && !selected->isPlaylist &&
       !home.channelLookupPending && !home.channelLookupActive) {
      SearchResult channelItem = *selected;
      if (channelItem.channelId[0]) {
         cacheCurrent();
         openChannelForCategory(&channelItem, home.playlistReturnChannelCategory);
      } else {
         logInfo("[home] Triangle: resolving channel for video=%s (download=%d)\n",
                 channelItem.videoId, channelItem.isLocalDownload);
         home.channelLookupItem = channelItem;
         home.channelLookupResult = -1;
         home.channelLookupGroup.cancelled = 0;
         home.channelLookupDone = 0;
         home.channelLookupActive = 0;
         home.channelLookupPending = 1;
         pauseVideoGrid(&home.grid);
      }
      return;
   }
   if (home.category == HOME_DOWNLOADS && selected->isPlaylist && selected->isLocalPlaylist) {
      if (squarePressedAt) {
         if (isPadButtonDown(PAD_BTN_SQUARE) && getTimeUs() - squarePressedAt >= SQUARE_HOLD_US) {
            squarePressedAt = 0;
            strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, selected->localPlaylistId);
            home.playlistDeleteFromWatchLater = 5;
            if (getDownloadPlaylistDeleteInfo(home.playlistDeleteId, &home.playlistDeleteBytes,
                                              &home.playlistDeleteVideoCount) < 0) {
               setLabelText(&playlistToastLabel, LOC(STR_COULD_NOT_MEASURE_PLAYLIST_FILES));
               playlistToastShownAt = getTimeUs();
               return;
            }
            home.playlistDeleteConfirmOpen = 1;
            home.playlistDeleteConfirmSelected = 1;
            exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
            return;
         }
         if (isPadButtonReleased(PAD_BTN_SQUARE)) {
            squarePressedAt = 0;
            home.playlistNameAction = 2;
            strCopy(home.playlistNameTargetId, sizeof home.playlistNameTargetId, selected->localPlaylistId);
            oskInputBegin(LOC(STR_RENAME_DOWNLOAD_PLAYLIST), selected->title, onHomePlaylistNameEntered);
            return;
         }
         return;
      }
      if (isPadButtonPressed(PAD_BTN_SQUARE)) { squarePressedAt = getTimeUs(); return; }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         home.playlistReturnToSearch = 0;
         home.playlistReturnLocal = 0;
         home.playlistReturnCategory = HOME_DOWNLOADS;
         strCopy(home.localPlaylistId, sizeof home.localPlaylistId, selected->localPlaylistId);
         strCopy(home.playlistTitle, sizeof home.playlistTitle, selected->title);
         switchToCategory(HOME_LOCAL_PLAYLIST_CONTENTS);
         return;
      }
      return;
   }
   if (home.category == HOME_LOCAL_PLAYLIST_CONTENTS && home.playlistReturnCategory == HOME_DOWNLOADS &&
       selected->isLocalDownload) {
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
         if (isPadButtonReleased(PAD_BTN_SQUARE)) {
            squarePressedAt = 0;
            removeVideoFromDownloadPlaylist(home.localPlaylistId, selected->videoId);
            loadCategory();
            return;
         }
         return;
      }
      if (isPadButtonPressed(PAD_BTN_SQUARE)) { squarePressedAt = getTimeUs(); return; }
   }
   if ((home.category == HOME_DOWNLOADS ||
        (home.category == HOME_LOCAL_PLAYLIST_CONTENTS && home.playlistReturnCategory == HOME_DOWNLOADS)) &&
       selected->isLocalDownload) {
      if (home.category == HOME_DOWNLOADS && squarePressedAt) {
         if (isPadButtonDown(PAD_BTN_SQUARE) && getTimeUs() - squarePressedAt >= SQUARE_HOLD_US) {
            squarePressedAt = 0;
            strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, selected->videoId);
            home.playlistDeleteFromWatchLater = 2;
            home.playlistDeleteConfirmOpen = 1;
            home.playlistDeleteConfirmSelected = 1;
            exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
            return;
         }
         if (isPadButtonReleased(PAD_BTN_SQUARE)) {
            squarePressedAt = 0;
            openDownloadPlaylistPicker(selected);
            return;
         }
         return;
      }
      if (home.category == HOME_DOWNLOADS && isPadButtonPressed(PAD_BTN_SQUARE)) { squarePressedAt = getTimeUs(); return; }
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
      if (home.playlistReturnCategory == HOME_DOWNLOADS) {
         removeVideoFromDownloadPlaylist(home.localPlaylistId, selected->videoId);
      } else {
         strCopy(home.playlistDeleteId, sizeof home.playlistDeleteId, selected->videoId);
         home.playlistDeleteFromWatchLater = 4;
         home.playlistDeleteConfirmOpen = 1;
         home.playlistDeleteConfirmSelected = 1;
         exitConfirmNavigateRepeat.timer = exitConfirmNavigateRepeat.repeats = 0;
         return;
      }
      loadCategory(); return;
   }
   if (home.category == HOME_PLAYLISTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 1);
      if (homeSquareHint >= 0) {
         setButtonHintCaption(&hints, homeSquareHint, LOC(STR_SAVE_PLAYLIST));
         setButtonHintShown(&hints, homeSquareHint, 1);
      }
      if (handleHomeSquareAction(selected)) return;
      if (isPadButtonPressed(PAD_BTN_R3) && selected->isPlaylist) { openDownloadPlaylistConfirmation(selected); return; }
      if (isPadButtonPressed(PAD_BTN_CROSS) && selected->isPlaylist && selected->playlistId[0]) {
         strCopy(home.playlistId, sizeof home.playlistId, selected->playlistId);
         strCopy(home.playlistTitle, sizeof home.playlistTitle, selected->title);
         openHomePlaylist(selected->playlistId, selected->title);
      }
      return;
   }
   if (home.category == HOME_LOCAL_PLAYLISTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, selected->isPlaylist);
      if (homeSquareHint >= 0) {
         setButtonHintCaption(&hints, homeSquareHint,
                              home.localPlaylistTab == 0 ? LOC(STR_REMOVE_HOLD) : LOC(STR_RENAME_DELETE_HOLD));
         setButtonHintShown(&hints, homeSquareHint, 1);
      }
      if (isPadButtonPressed(PAD_BTN_R3) && selected->isPlaylist) { openDownloadPlaylistConfirmation(selected); return; }
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
                  oskInputBegin(LOC(STR_RENAME_PLAYLIST), selected->title, onHomePlaylistNameEntered);
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
            home.playlistReturnCategory = HOME_LOCAL_PLAYLISTS;
            strCopy(home.localPlaylistId, sizeof home.localPlaylistId, selected->localPlaylistId);
            strCopy(home.playlistTitle, sizeof home.playlistTitle, selected->title);
            switchToCategory(HOME_LOCAL_PLAYLIST_CONTENTS);
         }
      }
      return;
   }
   if (home.category == HOME_WATCHLATER || home.category == HOME_LOCAL_PLAYLIST_CONTENTS) {
      if (homeDownloadHint >= 0)
         setButtonHintShown(&hints, homeDownloadHint,
                            home.category == HOME_WATCHLATER ||
                            (home.category == HOME_LOCAL_PLAYLIST_CONTENTS &&
                             home.playlistReturnCategory != HOME_DOWNLOADS));
      if (homeSquareHint >= 0) {
         setButtonHintCaption(&hints, homeSquareHint,
                              home.category == HOME_WATCHLATER ? LOC(STR_REMOVE_PLAYLIST_HOLD) :
                              (home.playlistReturnCategory == HOME_DOWNLOADS ? LOC(STR_REMOVE_DELETE_HOLD) : LOC(STR_REMOVE)));
         setButtonHintShown(&hints, homeSquareHint, 1);
      }
   } else if (home.category == HOME_PLAYLIST_CONTENTS) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 1);
      if (homeSquareHint >= 0) setButtonHintShown(&hints, homeSquareHint, 0);
   } else if (homeSquareHint >= 0) {
      if (homeDownloadHint >= 0) setButtonHintShown(&hints, homeDownloadHint, 1);
      setButtonHintCaption(&hints, homeSquareHint, LOC(STR_WATCH_LATER_PLAYLIST_HOLD));
      setButtonHintShown(&hints, homeSquareHint, 1);
   }

   int inDownloadedPlaylist = home.category == HOME_LOCAL_PLAYLIST_CONTENTS &&
                              home.playlistReturnCategory == HOME_DOWNLOADS;
   if (homeDownloadHint >= 0 && inDownloadedPlaylist) {
      setButtonHintCaption(&hints, homeDownloadHint, LOC(STR_MANAGE_DOWNLOADS));
      setButtonHintShown(&hints, homeDownloadHint, 1);
   }

   if (handleHomeSquareAction(selected)) return;
   if (isPadButtonPressed(PAD_BTN_R3)) {
      if (selected->isPlaylist) openDownloadPlaylistConfirmation(selected);
      else if (!selected->isLive && selected->videoId[0]) {
         char downloadedPath[MAX_PATH_LEN];
         if (getDownloadedVideoPath(selected->videoId, downloadedPath, sizeof downloadedPath) != 0)
            enqueueDownload(selected);
      }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatched(selected->videoId);
      if (home.category == HOME_PLAYLIST_CONTENTS ||
          home.category == HOME_LOCAL_PLAYLIST_CONTENTS ||
          home.category == HOME_WATCHLATER ||
          home.category == HOME_ACCOUNT + ACCOUNT_FEED_WATCH_LATER) {
         char queue[MAX_SEARCH_RESULTS][16];
         int queueCount = 0, queueIndex = -1;
         const SearchResults *results = gridResults(&home.grid);
         if (results) {
            for (int i = 0; i < results->count && queueCount < MAX_SEARCH_RESULTS; i++) {
               const SearchResult *item = &results->items[i];
               if (!item->videoId[0] || item->isPlaylist || item->isLive) continue;
               strCopy(queue[queueCount], sizeof queue[queueCount], item->videoId);
               if (strcmp(item->videoId, selected->videoId) == 0) queueIndex = queueCount;
               queueCount++;
            }
         }
         stopVideoGrid(&home.grid);
         setPlaylistShuffleContext(homeIsPlaylistContents());
         if (queueIndex >= 0)
            playVideoWithQueue(selected->videoId, queue, queueCount, queueIndex);
         else
            playVideo(selected->videoId);
      } else {
         stopVideoGrid(&home.grid);   // no per-call http fetch may overlap the http media stream during playback
         playVideo(selected->videoId);
      }
   }
   if (home.restoreGridAfterWatchLater && home.category == HOME_WATCHLATER &&
       gridStage(&home.grid) == GRID_READY) {
      int count = home.grid.results.count;
      home.grid.selected = count > 0 ? home.restoreGridSelection : 0;
      if (home.grid.selected >= count) home.grid.selected = count > 0 ? count - 1 : 0;
      home.grid.scrollRow = home.restoreGridScrollRow;
      int columns = home.grid.columns > 0 ? home.grid.columns : 1;
      int totalRows = (count + columns - 1) / columns;
      int maxScroll = totalRows - home.grid.visibleRows;
      if (maxScroll < 0) maxScroll = 0;
      if (home.grid.scrollRow > maxScroll) home.grid.scrollRow = maxScroll;
      if (home.grid.scrollRow < 0) home.grid.scrollRow = 0;
      home.restoreGridAfterWatchLater = 0;
   } else restoreHomePlaybackSelection();
}

static void drawDownloadManagement(void)
{
   DownloadManagementItem items[DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS];
   int totalCount = 0, loading = 0;
   int visible = getDownloadManagementItems(home.downloadManagerCategory,
                                             home.downloadManagerScroll, items,
                                             DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS,
                                             &totalCount, &loading);
   int counts[3] = {0, 0, 0};
   DownloadManagementItem countItem;
   for (int category = 0; category < 3; category++) {
      int ignoredLoading = 0;
      getDownloadManagementItems(category, 0, &countItem, 1,
                                 &counts[category], &ignoredLoading);
   }

   int panelW = getDownloadManagerPanelWidth();
   int panelH = home.screenH - 32;
   if (panelH > 500) panelH = 500;
   if (panelH < 360) panelH = home.screenH - 16;
   int rowH = (panelH - 202) / DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS;
   if (rowH < 30) rowH = 30;
   int listTop = 132;
   int listH = rowH * DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS;
   int x = (home.screenW - panelW) / 2;
   int y = (home.screenH - panelH) / 2;

   fillGfxRectangleFixed(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   setLabelText(&downloadManagerTitle, LOC(STR_DOWNLOAD_MANAGEMENT));
   refreshLabelScale(&downloadManagerTitle);
   drawLabelAt(&downloadManagerTitle, x + 30, y + 14);

   const char *categoryNames[3] = { LOC(STR_ONGOING), LOC(STR_COMPLETED), LOC(STR_FAILED) };
   int tabGap = 8;
   int tabW = (panelW - 48 - tabGap * 2) / 3;
   int tabY = y + 66;
   for (int i = 0; i < 3; i++) {
      int tabX = x + 24 + i * (tabW + tabGap);
      int selected = i == home.downloadManagerCategory;
      char tabText[48];
      snprintf(tabText, sizeof tabText, "%s (%d)", categoryNames[i], counts[i]);
      fillGfxRoundedRectangleFixed(tabX, tabY, tabW, 42, 7,
                                   selected ? activeTheme->accent : activeTheme->surface);
      setLabelText(&downloadManagerRows[i], tabText);
      setLabelColor(&downloadManagerRows[i], selected ? activeTheme->appBg : activeTheme->textSecondary);
      refreshLabelScale(&downloadManagerRows[i]);
      moveLabel(&downloadManagerRows[i],
                tabX + (tabW - downloadManagerRows[i].tt.tex.w) / 2,
                getCenteredLabelY(&downloadManagerRows[i], tabY, 42));
      drawLabel(&downloadManagerRows[i]);
   }

   for (int row = 0; row < DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS; row++) {
      int index = home.downloadManagerScroll + row;
      int rowY = y + listTop + row * rowH;
      if (row < visible) {
         DownloadManagementItem *item = &items[row];
         int selected = index == home.downloadManagerSelected;
         int checked = downloadManagerIsSelected(item->videoId);
         char status[32];
         if (item->active) {
            int percent = item->percent;
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            snprintf(status, sizeof status, "%s%d%%",
                     item->cancelling ? LOC(STR_CANCELLING) : "", percent);
         } else if (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_ONGOING) {
            strCopy(status, sizeof status, LOC(STR_QUEUED));
         } else if (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_COMPLETED) {
            status[0] = 0;
         } else {
            strCopy(status, sizeof status, LOC(STR_FAILED));
         }

         if (selected)
            fillGfxRoundedRectangleFixed(x + 14, rowY + 1, panelW - 28, rowH - 2,
                                         6, activeTheme->rowHighlight);

         // Selection checkbox is a filled square only when selected. No filesystem
         // work or thumbnail decode occurs while drawing this list.
         fillGfxRoundedRectangleFixed(x + 25, rowY + (rowH - 16) / 2, 16, 16, 3,
                                      checked ? activeTheme->accent : activeTheme->surface);
         if (checked)
            fillGfxRoundedRectangleFixed(x + 30, rowY + (rowH - 6) / 2, 6, 6, 2,
                                         activeTheme->appBg);

         setLabelColor(&downloadManagerRows[row + 3], selected ?
                       activeTheme->textPrimary : activeTheme->textSecondary);
         setLabelColor(&downloadManagerStatuses[row], selected ?
                       activeTheme->textPrimary : activeTheme->textSecondary);
         setLabelText(&downloadManagerRows[row + 3], item->title);
         setLabelText(&downloadManagerStatuses[row], status);
         refreshLabelScale(&downloadManagerRows[row + 3]);
         refreshLabelScale(&downloadManagerStatuses[row]);

         int titleX = x + 52;
         int titleY = getCenteredLabelY(&downloadManagerRows[row + 3], rowY, rowH);
         int statusX = x + panelW - 34 - downloadManagerStatuses[row].tt.tex.w;
         if (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_COMPLETED) {
            drawLabelAt(&downloadManagerRows[row + 3], titleX,
                        getCenteredLabelY(&downloadManagerRows[row + 3], rowY, rowH));
         } else {
            drawLabelAt(&downloadManagerRows[row + 3], titleX, titleY);
            drawLabelAt(&downloadManagerStatuses[row], statusX,
                        getCenteredLabelY(&downloadManagerStatuses[row], rowY, rowH));
         }
      } else {
         setLabelText(&downloadManagerRows[row + 3], "");
         setLabelText(&downloadManagerStatuses[row], "");
      }
   }

   if (totalCount > DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS) {
      int trackX = x + panelW - 12;
      int trackY = y + listTop;
      fillGfxRoundedRectangleFixed(trackX, trackY, 4, listH, 2, activeTheme->surface);
      int thumbH = listH * DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS / totalCount;
      if (thumbH < 18) thumbH = 18;
      int maxScroll = totalCount - DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS;
      int thumbY = trackY + (listH - thumbH) * home.downloadManagerScroll / maxScroll;
      fillGfxRoundedRectangleFixed(trackX, thumbY, 4, thumbH, 2, activeTheme->accent);
   }

   if (home.downloadManagerMessage[0] || loading || totalCount == 0) {
      const char *message = home.downloadManagerMessage[0] ? home.downloadManagerMessage :
         (loading ? LOC(STR_LOADING_LIBRARY) :
          (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_ONGOING ? LOC(STR_NO_ONGOING_DOWNLOADS) :
           (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_COMPLETED ?
            LOC(STR_NO_COMPLETED_DOWNLOADS) : LOC(STR_NO_FAILED_DOWNLOADS))));
      setLabelText(&downloadManagerFooter, message);
      refreshLabelScale(&downloadManagerFooter);
      drawLabelAt(&downloadManagerFooter, x + 28, y + panelH - 54);
      downloadManagerHints.y = y + panelH - 26;
      int hintsWidth = 0;
      for (int i = 0; i < downloadManagerHints.count; i++) {
         ButtonHint *hint = &downloadManagerHints.hints[i];
         hintsWidth += hint->drawWidth;
         if (!hint->pairNext) hintsWidth += 8 + hint->caption.tt.tex.w;
         if (i + 1 < downloadManagerHints.count)
            hintsWidth += hint->pairNext ? 8 : downloadManagerHints.gap;
      }
      drawButtonHintsAt(&downloadManagerHints, x + (panelW - hintsWidth) / 2);
   } else {
      downloadManagerHints.y = y + panelH - 30;
      int hintsWidth = 0;
      for (int i = 0; i < downloadManagerHints.count; i++) {
         ButtonHint *hint = &downloadManagerHints.hints[i];
         hintsWidth += hint->drawWidth;
         if (!hint->pairNext) hintsWidth += 8 + hint->caption.tt.tex.w;
         if (i + 1 < downloadManagerHints.count)
            hintsWidth += hint->pairNext ? 8 : downloadManagerHints.gap;
      }
      drawButtonHintsAt(&downloadManagerHints, x + (panelW - hintsWidth) / 2);
   }

   if (home.downloadManagerConfirmOpen) {
      float uiScale = getGfxUiScaleFactor();
      if (uiScale < 1.0f) uiScale = 1.0f;
      fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, 0x50000000);
      int confirmW = (int)(430 * uiScale + 0.5f);
      int confirmH = (int)(148 * uiScale + 0.5f);
      if (confirmW > home.screenW - 48) confirmW = home.screenW - 48;
      if (confirmH > home.screenH - 48) confirmH = home.screenH - 48;
      int cx = (home.screenW - confirmW) / 2;
      int cy = (home.screenH - confirmH) / 2;
      fillGfxRoundedRectangleFixed(cx, cy, confirmW, confirmH, 10, activeTheme->appBg);
      fillGfxRoundedRectangleFixed(cx, cy, confirmW, 4, 2, activeTheme->accent);

      char prompt[DOWNLOAD_MANAGEMENT_TITLE_LEN];
      if (home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_ONGOING)
         snprintf(prompt, sizeof prompt, LOC(STR_CANCEL_DOWNLOAD_COUNT), home.downloadManagerSelectedCount,
                  home.downloadManagerSelectedCount == 1 ? "" : "s");
      else
         snprintf(prompt, sizeof prompt, LOC(STR_DELETE_DOWNLOAD_ITEMS), home.downloadManagerSelectedCount,
                  home.downloadManagerCategory == DOWNLOAD_MANAGEMENT_FAILED ? LOC(STR_FAILED_LOWER) : LOC(STR_DOWNLOADED_LOWER),
                  home.downloadManagerSelectedCount == 1 ? "" : "s");
      setLabelText(&downloadManagerTitle, prompt);
      refreshLabelScale(&downloadManagerTitle);
      moveLabel(&downloadManagerTitle, home.screenW / 2 - downloadManagerTitle.tt.tex.w / 2,
                cy + (int)(34 * uiScale + 0.5f));
      drawLabel(&downloadManagerTitle);

      setLabelText(&downloadManagerFooter, LOC(STR_CONFIRM_CANCEL_FOOTER));
      refreshLabelScale(&downloadManagerFooter);
      moveLabel(&downloadManagerFooter, home.screenW / 2 - downloadManagerFooter.tt.tex.w / 2,
                cy + (int)(94 * uiScale + 0.5f));
      drawLabel(&downloadManagerFooter);
      setLabelText(&downloadManagerTitle, LOC(STR_DOWNLOAD_MANAGEMENT));
   }
}

static void refreshHomeHintCaptions(void)
{
   // Button-hint labels are allocated once; refresh their text after an
   // application-language change instead of retaining the startup language.
   setButtonHintCaption(&hints, 0, LOC(STR_PLAY));
   setButtonHintCaption(&hints, 1, LOC(STR_WATCH_LATER_PLAYLIST_HOLD));
   setButtonHintCaption(&hints, 2, LOC(STR_DOWNLOAD));
   setButtonHintCaption(&hints, 3, LOC(STR_CHANNEL));
   setButtonHintCaption(&hints, 4, LOC(STR_NEW_PLAYLIST));
   setButtonHintCaption(&hints, 5, LOC(STR_SEARCH));
   setButtonHintCaption(&hints, 6, LOC(STR_SETTINGS));
   setButtonHintCaption(&hints, 8, LOC(STR_CATEGORY));
   setButtonHintCaption(&hints, 9, LOC(STR_REFRESH));
   setButtonHintCaption(&hints, 10, LOC(STR_SORT));
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
      homeLoadingStartedUs = 0;
      drawVideoGrid(&home.grid);
   } else {
      GridStage stage = gridStage(&home.grid);
      if (stage == GRID_RUNNING) {
         drawLoadingIndicator(home.screenW / 2, home.screenH / 2, &homeLoadingStartedUs);
      } else {
         homeLoadingStartedUs = 0;
         setLabelText(&statusLabel, stage == GRID_ERROR ? LOC(STR_UNABLE_TO_LOAD_CONTENT) : LOC(STR_NO_RESULTS));
         drawLabel(&statusLabel);
      }
   }
   // Draw the header after the grid so the category chips remain above any
   // selection glow that reaches into the header band.
   drawLabel(&titleLabel);
   if (home.category < TREND_COUNT) drawTrendingTabs();
   else if (home.category == HOME_LIVE) drawLiveTabs();
   else if (home.category == HOME_LOCAL_PLAYLISTS) drawLocalPlaylistTabs();
   else if (home.category == HOME_DOWNLOADS) drawDownloadCategoryTabs();
   if (homeSupportsSort() || homeSupportsPlaylistSort()) drawHomeSortPills();
   drawPlaylistShuffleBadge();
   if (home.category == HOME_DOWNLOADS) drawDownloadSpaceStatus();
   refreshHomeHintCaptions();
   drawButtonHintsFitted(&hints, home.screenW, homeFooterBottomY, HINT_ROW_GAP, MAX_HINT_ROWS);
   if (!home.accountOpen && !home.settingsOpen && !home.exitConfirmOpen) drawSidebar();
   if (home.downloadPlaylistConfirmOpen) {
      int panelW = home.screenW * 90 / 100;
      if (panelW > 1200) panelW = 1200;
      int panelH = (int)(260 * home.uiScale);
      int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2;
      fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
      fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
      fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
      refreshLabelScale(&downloadPlaylistConfirmTitle);
      refreshLabelScale(&downloadPlaylistConfirmFooter);
      moveLabel(&downloadPlaylistConfirmTitle, x + 28, y + (int)(48 * home.uiScale));
      moveLabel(&downloadPlaylistConfirmFooter, x + 28, y + panelH - (int)(58 * home.uiScale));
      drawLabel(&downloadPlaylistConfirmTitle);
      drawLabel(&downloadPlaylistConfirmFooter);
   }
   if (home.accountOpen) drawAccountPanel();
   if (home.settingsOpen) {
      if (updateSettingsOpen) drawUpdateSettingsPanel();
      else if (home.themeOpen) drawThemePanel();
      else if (home.downloadSettingsOpen) drawDownloadSettingsPanel();
      else {
         drawSettingsPanel();
         if (home.regionOpen) drawRegionPicker();
         else if (home.sponsorblockOpen) drawSponsorBlockPanel();
      }
      if (home.languageConfirmOpen) drawLanguageConfirm();
   }
   if (updateDialogOpen) drawUpdateDialog();
   if (updateNotesOpen) drawUpdateNotes();
   if (home.exitConfirmOpen) drawExitConfirmation();
   if (playlistToastShownAt && getTimeUs() - playlistToastShownAt < 1000000ULL &&
       !home.accountOpen && !home.settingsOpen && !home.exitConfirmOpen && !home.playlistDeleteConfirmOpen) {
      refreshLabelScale(&playlistToastLabel);
      int toastX = (home.screenW - playlistToastLabel.tt.tex.w) / 2;
      int toastY = (home.screenH - playlistToastLabel.tt.tex.h) / 2;
      setLabelColor(&playlistToastLabel, activeTheme->textPrimary);
      fillGfxRoundedRectangleFixed(toastX - 20, toastY - 12,
                                   playlistToastLabel.tt.tex.w + 40, playlistToastLabel.tt.tex.h + 24,
                                   10, homeNotificationBackground());
      drawLabelAt(&playlistToastLabel, toastX, toastY);
   }
   if (home.playlistDeleteConfirmOpen) {
      setLabelText(&exitConfirmTitle,
                   home.playlistDeleteFromWatchLater == 1 ? LOC(STR_REMOVE_PLAYLIST) :
                   home.playlistDeleteFromWatchLater == 2 ? LOC(STR_DELETE_DOWNLOAD) :
                   (home.playlistDeleteFromWatchLater == 3 || home.playlistDeleteFromWatchLater == 4) ? LOC(STR_REMOVE_VIDEO) :
                   home.playlistDeleteFromWatchLater == 5 ? LOC(STR_DELETE_DOWNLOAD_PLAYLIST) : LOC(STR_DELETE_PLAYLIST));
      char deleteMessage[192];
      if (home.playlistDeleteFromWatchLater == 5) {
         char freed[48]; formatDownloadSpace(home.playlistDeleteBytes, freed, sizeof freed);
         snprintf(deleteMessage, sizeof deleteMessage, LOC(STR_DELETE_PLAYLIST_AND_VIDEOS),
                  home.playlistDeleteVideoCount, freed);
      } else {
         strCopy(deleteMessage, sizeof deleteMessage,
                 home.playlistDeleteFromWatchLater == 1 ? LOC(STR_REMOVE_FROM_WATCH_LATER) :
                 home.playlistDeleteFromWatchLater == 2 ? LOC(STR_DELETE_DOWNLOADED_VIDEO) :
                 home.playlistDeleteFromWatchLater == 3 ? LOC(STR_REMOVE_VIDEO_WATCH_LATER) :
                 home.playlistDeleteFromWatchLater == 4 ? LOC(STR_REMOVE_VIDEO_PS3_PLAYLIST) :
                 LOC(STR_DELETE_PLAYLIST_AND_VIDEOS_QUESTION));
      }
      setLabelText(&exitConfirmMessage, deleteMessage);
      setLabelText(&exitConfirmChoices[0], LOC(STR_YES)); setLabelText(&exitConfirmChoices[1], LOC(STR_NO));
      int saved = home.exitConfirmSelected; home.exitConfirmSelected = home.playlistDeleteConfirmSelected;
      drawExitConfirmation(); home.playlistDeleteConfirmSelected = home.exitConfirmSelected; home.exitConfirmSelected = saved;
      setLabelText(&exitConfirmTitle, LOC(STR_EXIT_CRTUBE));
      setLabelText(&exitConfirmMessage, LOC(STR_CONFIRM_CLOSE_APP));
   }
   if (home.downloadManagerOpen) {
      drawDownloadManagement();
   }
}

static void suspendHome(void)
{
   squarePressedAt = 0;
   // Do not join a feed/thumbnail worker when opening Search or another overlay.
   // The active job is allowed to finish in parallel while this grid is hidden.
   pauseVideoGrid(&home.grid);
}

static void restoreHomePlaybackSelection(void)
{
   if (!playbackRestorePending || gridStage(&home.grid) != GRID_READY ||
       home.grid.results.count <= 0) return;
   playbackRestorePending = 0;
   char origin[16], current[16];
   getPlaybackReturnVideoIds(origin, current);
   if (!origin[0]) return;

   if (!homeIsPlaylistContents() && current[0] && strcmp(current, origin) != 0) {
      home.grid.selected = 0;
      home.grid.scrollRow = 0;
      return;
   }

   const char *target = homeIsPlaylistContents() && current[0] ? current : origin;
   for (int i = 0; i < home.grid.results.count; i++) {
      if (!home.grid.results.items[i].isPlaylist &&
          strcmp(home.grid.results.items[i].videoId, target) == 0) {
         home.grid.selected = i;
         home.grid.scrollRow = i / (home.grid.columns > 0 ? home.grid.columns : 1);
         int columns = home.grid.columns > 0 ? home.grid.columns : 1;
         int totalRows = (home.grid.results.count + columns - 1) / columns;
         int maxScroll = totalRows - home.grid.visibleRows;
         if (maxScroll < 0) maxScroll = 0;
         if (home.grid.scrollRow > maxScroll) home.grid.scrollRow = maxScroll;
         return;
      }
   }
   home.grid.selected = 0;
   home.grid.scrollRow = 0;
}

// returning from search/play: if the load was interrupted mid-flight (worker stopped), restart it; an
// already-loaded grid is kept as-is (instant, thumbnails intact).
static void resumeHome(void)
{
   playbackRestorePending = 1;

   // The grid is paused while another screen/overlay owns input. Resume it
   // before any early return, but never wait for its worker here.
   resumeVideoGrid(&home.grid);

   if (home.category == HOME_DOWNLOADS) {
      requestDownloadManagementRefresh();
      if (!home.grid.threadActive) loadCategory();
      return;
   }
   if (home.category == HOME_LIVE) {
      uint64_t age = getTimeUs() - home.liveLoadedUs;
      if (!home.grid.threadActive &&
          (gridStage(&home.grid) != GRID_READY || age >= LIVE_MAX_AGE_US))
         loadCategory();
      return;
   }
   if (home.category == HOME_LOCAL_HISTORY) {
      if (!home.grid.threadActive) loadCategory();
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
   if (getDownloadPlaylistRevision() != downloadPlaylistRevisionSeen) {
      downloadPlaylistRevisionSeen = getDownloadPlaylistRevision();
      if (home.category == HOME_DOWNLOADS ||
          (home.category == HOME_LOCAL_PLAYLIST_CONTENTS && home.playlistReturnCategory == HOME_DOWNLOADS)) {
         loadCategory();
         return;
      }
   }
   if (getLocalPlaylistRevision() != localPlaylistRevisionSeen ||
       getPlaylistWatchLaterRevision() != playlistWatchLaterRevisionSeen) {
      localPlaylistRevisionSeen = getLocalPlaylistRevision();
      playlistWatchLaterRevisionSeen = getPlaylistWatchLaterRevision();
      if (home.category == HOME_LOCAL_PLAYLISTS || home.category == HOME_LOCAL_PLAYLIST_CONTENTS ||
          home.category == HOME_DOWNLOADS) {
         loadCategory();
         return;
      }
   }
   // A still-running worker is already making progress. Never cancel/restart it
   // merely because the screen became visible again. Reap it from updateHome().
   if (home.grid.threadActive) return;
   if (gridStage(&home.grid) != GRID_READY && gridStage(&home.grid) != GRID_EMPTY) loadCategory();
   else restoreHomePlaybackSelection();
}

static void termHome(void)
{
   if (downloadPlaylistThreadActive) { joinThread(downloadPlaylistThread); downloadPlaylistThreadActive = 0; }
   free(downloadPlaylistResults);
   downloadPlaylistResults = NULL;
   if (home.channelLookupActive) {
      cancelHttpRequestGroup(&home.channelLookupGroup);
      joinThread(home.channelLookupTid);
      home.channelLookupActive = 0;
   }
   termVideoGrid(&home.grid);
   termSidebar();
   termButtonHints(&hints);
   termButtonHints(&panelHints);
   termButtonHints(&downloadManagerHints);
   freeLabel(&titleLabel);
   freeLabel(&downloadSpacePrefixLabel);
   freeLabel(&downloadSpaceAvailableLabel);
   freeLabel(&statusLabel);
   freeLabel(&playlistErrorLabel);
   freeLabel(&playlistToastLabel);
   freeLabel(&downloadManagerTitle);
   freeLabel(&downloadPlaylistConfirmTitle);
   freeLabel(&downloadPlaylistConfirmFooter);
   for (int i = 0; i < 3; i++) freeLabel(&downloadCategoryLabels[i]);
   for (int i = 0; i < 10; i++) freeLabel(&downloadManagerRows[i]);
   for (int i = 0; i < 7; i++) freeLabel(&downloadManagerStatuses[i]);
   freeLabel(&downloadManagerFooter);
   freeLabel(&panelTitle);
   freeLabel(&panelLine1);
   freeLabel(&panelLine2);
   freeLabel(&panelCode);
   freeLabel(&panelFooter);
   freeLabel(&settingsTitle);
   freeLabel(&updatePanelTitle);
   for (int i = 0; i < 3; i++) freeLabel(&updatePanelInfo[i]);
   for (int i = 0; i < 6; i++) freeLabel(&updatePanelRows[i]);
   freeLabel(&updateDialogTitle);
   freeLabel(&updateDialogVersion);
   for (int i = 0; i < 6; i++) freeLabel(&updateDialogRows[i]);
   freeLabel(&updateNotesTitle);
   for (int i = 0; i < UPDATE_NOTES_VISIBLE_LINES; i++) freeLabel(&updateNotesLines[i]);
   termButtonHints(&updateHints);
   freeLabel(&themeTitle);
   for (int i = 0; i < THEME_ROW_COUNT; i++) freeLabel(&themeRowLabels[i]);
   for (int i = 0; i < 2; i++) freeLabel(&themeLegendLabels[i]);
   freeLabel(&themeSelectionLeft);
   freeLabel(&themeSelectionRight);
   freeLabel(&themeCustomTitle);
   for (int i = 0; i < 3; i++) freeLabel(&themeCustomLabels[i]);
   for (int i = 0; i < 2; i++) freeLabel(&themeCustomOptions[i]);
   freeLabel(&themeConfirmTitle);
   freeLabel(&themeConfirmMessage);
   for (int i = 0; i < 2; i++) freeLabel(&themeConfirmChoices[i]);
   termButtonHints(&themeHints);
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
