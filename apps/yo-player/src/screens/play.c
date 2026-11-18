// play screen - resolve a video, then stream it with the simple-lib-av engine.
//
// Both stream urls are handed straight to the player: simple-lib-av opens each through
// openHttpStream (the http module), so each demuxer reads the moov and then each sample
// on demand by HTTP range - nothing is downloaded in full. Adaptive picks are split: a
// video-only stream plus an audio-only stream, each its own independent http stream.
//
// Follows file-manager's video overlay: the open work runs on a worker
// (createVideoPlayer does blocking network I/O and must stay off the UI thread),
// the UI thread adopts the built player and pulls frames each render frame.

#include "app-config.h"
#include "localization.h"
#include "../../BUILD_CONFIG.h"
#include "screens/play.h"
#include "screens/home.h"
#include "downloads.h"
#include "extractor.h"
#include "stream-select.h"   // pickBestVideo / pickBestAudio (shared with download)

#include "gfx.h"
#include "theme.h"
#include "colors.h"   // COLOR_BLACK: the letterbox bars are black whatever the theme
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/loading-indicator.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "ui/volume-meter.h"
#include "button-repeat.h"     // left/right scrub auto-repeat
#include "thread.h"
#include "screen-manager.h"
#include "string-utilities.h"   // strCopy

#include "video-player.h"
#include "http.h"               // cancel network I/O before joining playback workers
#include "image-loader.h"       // decode the autoplay thumbnail without blocking the render thread
#include "storage.h"            // resume position (get/setWatchedPosition)
#include "vfs.h"                // persist the shared SD UI scale preference
#include "settings.h"           // per-category SponsorBlock actions
#include "sponsorblock.h"       // skip segments (auto-skip + seek-bar marks)
#include "subtitles.h"          // fetch + parse a caption track; cue lookup during playback
#include "subtitle-translate.h" // background translation for synthetic language tracks
#include "chapters.h"           // description timestamp lines -> chapter list
#include "dbg.h"                // logInfo/logError (bridge)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>              // snprintf (stats overlay)
#include <sys/sys_time.h>       // sys_time_get_system_time (fps measure)
#include <math.h>                // autoplay countdown ring

#if BUILD_DIAG_SUBTITLE_RENDER
typedef struct {
   uint64_t totalUs;
   uint64_t maxUs;
   uint64_t calls;
} SubtitleRenderDiagStage;

enum {
   SUB_DIAG_CLOCK = 0,
   SUB_DIAG_CUE,
   SUB_DIAG_LABEL,
   SUB_DIAG_DRAW,
   SUB_DIAG_STAGE_COUNT
};

static SubtitleRenderDiagStage subtitleRenderDiag[SUB_DIAG_STAGE_COUNT];
static uint64_t subtitleRenderDiagWindowUs;

static void recordSubtitleRenderDiag(int stage, uint64_t elapsedUs)
{
   SubtitleRenderDiagStage *diag = &subtitleRenderDiag[stage];
   diag->calls++;
   diag->totalUs += elapsedUs;
   if (elapsedUs > diag->maxUs) diag->maxUs = elapsedUs;
}

static void flushSubtitleRenderDiag(uint64_t nowUs)
{
   if (!subtitleRenderDiagWindowUs) subtitleRenderDiagWindowUs = nowUs;
   if (nowUs - subtitleRenderDiagWindowUs < 5000000ull) return;

   logInfo("[subtitle-render-diag] spanMs=%llu clock=%llu/%llu/%llu cue=%llu/%llu/%llu label=%llu/%llu/%llu draw=%llu/%llu/%llu\n",
           (unsigned long long)((nowUs - subtitleRenderDiagWindowUs) / 1000ull),
           (unsigned long long)(subtitleRenderDiag[SUB_DIAG_CLOCK].calls ? subtitleRenderDiag[SUB_DIAG_CLOCK].totalUs / subtitleRenderDiag[SUB_DIAG_CLOCK].calls : 0),
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_CLOCK].maxUs,
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_CLOCK].calls,
           (unsigned long long)(subtitleRenderDiag[SUB_DIAG_CUE].calls ? subtitleRenderDiag[SUB_DIAG_CUE].totalUs / subtitleRenderDiag[SUB_DIAG_CUE].calls : 0),
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_CUE].maxUs,
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_CUE].calls,
           (unsigned long long)(subtitleRenderDiag[SUB_DIAG_LABEL].calls ? subtitleRenderDiag[SUB_DIAG_LABEL].totalUs / subtitleRenderDiag[SUB_DIAG_LABEL].calls : 0),
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_LABEL].maxUs,
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_LABEL].calls,
           (unsigned long long)(subtitleRenderDiag[SUB_DIAG_DRAW].calls ? subtitleRenderDiag[SUB_DIAG_DRAW].totalUs / subtitleRenderDiag[SUB_DIAG_DRAW].calls : 0),
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_DRAW].maxUs,
           (unsigned long long)subtitleRenderDiag[SUB_DIAG_DRAW].calls);
   memset(subtitleRenderDiag, 0, sizeof subtitleRenderDiag);
   subtitleRenderDiagWindowUs = nowUs;
}
#endif

// volume: up/down step the shared pill meter (ui/volume-meter.h), pills in the theme accent.
#define VOLUME_DEFAULT    VOLUME_METER_PILLS

// seek controls (bar + time) show on activity / pause, then auto-hide. left/right scrub the target
// instantly; VOD seeks by keyframe, LIVE DVR seeks by rebuilding both DASH streams at the target sq.
#define SEEK_STEP_SECONDS   5.0f
#define LIVE_SEGMENT_SEC_DEFAULT 5
#define LIVE_WINDOW_SEC_DEFAULT 43200
#define LIVE_START_BEHIND 3
#define LIVE_WINDOW_MARGIN 6
#define LIVE_EDGE_SLACK_SEC 3.0f
#define LIVE_STALL_TIMEOUT_US 20000000ULL
#define CROSS_RESTART_HOLD_US 3000000ULL
#define SEEK_ACCEL_1_US     1500000ULL
#define SEEK_ACCEL_2_US     3000000ULL
#define SEEK_ACCEL_3_US     5000000ULL
#define SEEK_APPLY_IDLE_US  400000ULL
#define TOAST_VISIBLE_US    2000000ULL   // how long a toast ("Skipped ...", "Subtitles: ...") stays up
#define ASK_PROMPT_VISIBLE_US 3000000ULL
#define AUTOPLAY_NOTICE_US  3000000ULL
#define AUTOPLAY_THUMB_CAP  (128 * 1024)

// subtitles: Triangle toggles captions on/off; language selection lives in Options.
#define SUBTITLE_TEXT_SIZE 30
#define MAX_AUDIO_TRACKS 128
#define MAX_INITIAL_FALLBACK_ATTEMPTS MAX_STREAM_FORMATS

// description overlay: SELECT toggles the description full-screen; up/down scroll it.
#define DESCRIPTION_MARGIN_X    120
#define DESCRIPTION_MARGIN_Y    80
#define DESCRIPTION_TEXT_SIZE   24
#define DESCRIPTION_PAGE_GAP 24

// chapter picker: START toggles a centred list of the video's chapters; up/down select
// (the window follows the selection), cross jumps there, circle/START close without jumping.
#define CHAPTER_TEXT_SIZE    24
#define CHAPTER_ROW_HEIGHT   46
#define CHAPTER_VISIBLE_ROWS 12
#define CHAPTER_TIME_GAP     24   // between the right-aligned time column and the title column

#define OPTIONS_COUNT 9
#define MAX_PLAYBACK_QUALITIES 12
#define OPTIONS_DROPDOWN_MAX MAX_AUDIO_TRACKS
#define OPTIONS_DROPDOWN_VISIBLE_MAX 10
static const AppStringId optionsValueIds[OPTIONS_COUNT][OPTIONS_DROPDOWN_MAX] = {
   {STR_QUALITY_AUTO_LABEL},
   {STR_DEFAULT},
   {STR_OFF, STR_ON},
   {STR_ENGLISH},
   {STR_OPEN_CHAPTERS},
   {STR_OFF, STR_ON},
   {STR_OFF, STR_ON},
   {STR_START},
   {STR_OPEN_SETTINGS}
};
static int optionsValueCounts[OPTIONS_COUNT] = {1, 1, 2, 1, 1, 2, 2, 1, 1};
static char optionsQualityLabels[OPTIONS_DROPDOWN_MAX][32];
static char optionsAudioLabels[MAX_AUDIO_TRACKS][64];
static int optionsQualityTargets[OPTIONS_DROPDOWN_MAX];
static int preferredQualityHeight, preferredQualityFps; // 0/0 means Auto; persists across videos in RAM
static int optionsVisible, optionsDropdownOpen, optionsSelected, optionsDropdownSelected;
static int lastSubtitlePick = -1;     // preserve the selected track while captions are switched OFF
static int suppressPlaybackCrossUntilRelease;
static uint64_t l1HoldStartUs, r1HoldStartUs;
static int l1HoldTriggered, r1HoldTriggered;
#define PLAYBACK_NAV_HOLD_US 2000000ULL
static VideoPlayer *chapterNavPlayer;
static int chapterNavIndex = -1;
static int chapterNavIndexValid, chapterNavQueued, chapterNavInFlight;
static ButtonRepeat optionsNavigateRepeat, optionsDropdownRepeat, chapterNavigateRepeat;
static Label optionsLabels[OPTIONS_COUNT];
static Label optionsValueLabels[OPTIONS_DROPDOWN_MAX];
static Label optionsDownloadLabel;
static uint64_t optionsDownloadPressedUntil;
static Label optionsCurrentValueLabels[3]; // Quality, Audio, Subs language

// Playback-owned settings panel. It deliberately shares the persisted settings APIs
// with navigation, but owns its input state, labels, layout and SponsorBlock submenu.
#define PLAYBACK_SETTINGS_COUNT 9
#define PLAYBACK_SETTINGS_ROW_H 44
#define PLAYBACK_SETTINGS_PANEL_W 760
#define PLAYBACK_SETTINGS_PANEL_H 450
#define PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS 8
#define PLAYBACK_AUTO_TRANSLATE_ROW_H 38
#define PLAYBACK_AUTO_TRANSLATE_PANEL_H 410
#define PLAYBACK_SUBTITLE_STYLE_COUNT 5
#define PLAYBACK_SUBTITLE_STYLE_PANEL_H 490
#define PLAYBACK_SPONSOR_PANEL_W 900
#define PLAYBACK_SPONSOR_PANEL_H 470
#define PLAYBACK_SPONSOR_ROW_H 50
#define PLAYBACK_SPONSOR_OPTION_W 104
#define PLAYBACK_SPONSOR_OPTION_GAP 8
#define PLAYBACK_SD_SCALE_PATH CRTUBE_DATA_DIR "/sd-ui-scale.txt"
static int playbackSettingsOpen, playbackSettingsSelected;
static int playbackAutoTranslateOpen, playbackAutoTranslateSelected;
static int autoTranslateRefreshPending;
static ButtonRepeat playbackAutoTranslateRepeat;
static Label playbackAutoTranslateTitle, playbackAutoTranslateRows[PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS];
static int playbackSubtitleStyleOpen, playbackSubtitleStyleSelected;
static ButtonRepeat playbackSubtitleStyleRepeat;
static Label playbackSubtitleStyleTitle, playbackSubtitleStyleLabels[PLAYBACK_SUBTITLE_STYLE_COUNT], playbackSubtitlePreview;
static int playbackSponsorOpen, playbackSponsorSelected;
static ButtonRepeat playbackSettingsRepeat, playbackSponsorRepeat, playbackSubtitleOffsetRepeat;
static ButtonRepeat playbackOverscanRepeat, playbackOsdTimeoutRepeat;
static Label playbackSettingsTitle, playbackSettingsLabels[PLAYBACK_SETTINGS_COUNT];
static Label playbackSettingsValues[PLAYBACK_SETTINGS_COUNT];
static Label playbackSettingsToggleLabels[2];
static Label playbackSponsorTitle, playbackSponsorCategoryLabels[SPONSOR_CATEGORY_COUNT];
static Label playbackSponsorActionLabels[3];

// Playback settings are persisted globally, while adaptive decoder recovery is session-local:
// a failed 1080p60 rendition is skipped for this video without rewriting the user's preference.
// Android-fallback LIVE sessions retain their origin separately from the currently active
// client, so a temporary VISIONOS seek fallback does not change the next seek's priority.
//
// the video to play, set by playVideo() before the screen is pushed. defaults to
// a short public clip so pushing the screen directly still does something.
static char requestedInput[256] = "jNQXAC9IVRw";   // "Me at the zoo"
static char playbackOriginVideoId[16];
static char requestedLocalPath[MAX_PATH_LEN];
static int requestedIsLocal;

// Snapshot of the Downloads grid order at playback start. It deliberately does not consult
// the live grid while the player is open, so changing sort later cannot reorder navigation.
#define DOWNLOAD_QUEUE_CAP MAX_SEARCH_RESULTS
static char playlistQueue[DOWNLOAD_QUEUE_CAP][16];
static int playlistQueueCount;
static int playlistQueueCursor;
static int playlistShuffleEnabled;
static int playlistShuffleContext;
static uint32_t playlistShuffleRandomState;
static char downloadedQueue[DOWNLOAD_QUEUE_CAP][16];
static int downloadedQueueCount;
static int downloadedQueueCursor;

// Playback navigation: the current video plus the two most recent predecessors.
// The cursor allows L1 to move backward; a new video after going back discards the forward branch.
#define PLAYBACK_NAV_CAP 3
static char playbackNavIds[PLAYBACK_NAV_CAP][12];
static int playbackNavCount;
static int playbackNavCursor = -1;

static int playbackIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int extractPlaybackId(const char *input, char out[12])
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   if (!input || !input[0]) return 0;
   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id && strlen(input) == 11) id = input;
   if (!id) return 0;
   for (int i = 0; i < 11; i++) {
      if (!id[i] || !playbackIdChar(id[i])) return 0;
      out[i] = id[i];
   }
   out[11] = 0;
   return 1;
}

static void appendPlaybackNavigation(const char *input)
{
   char id[12];
   if (!extractPlaybackId(input, id)) return;

   // If the user went back, a newly selected video starts a new forward branch.
   if (playbackNavCursor >= 0 && playbackNavCursor < playbackNavCount - 1)
      playbackNavCount = playbackNavCursor + 1;
   if (playbackNavCount == PLAYBACK_NAV_CAP) {
      memmove(playbackNavIds, playbackNavIds + 1,
              (PLAYBACK_NAV_CAP - 1) * sizeof playbackNavIds[0]);
      playbackNavCount--;
      if (playbackNavCursor > 0) playbackNavCursor--;
   }
   if (playbackNavCount > 0 && strcmp(playbackNavIds[playbackNavCount - 1], id) == 0) {
      playbackNavCursor = playbackNavCount - 1;
      return;
   }
   strCopy(playbackNavIds[playbackNavCount++], sizeof playbackNavIds[0], id);
   playbackNavCursor = playbackNavCount - 1;
}

static int previousPlaybackId(char out[12])
{
   if (playbackNavCursor <= 0) return 0;
   playbackNavCursor--;
   strCopy(out, 12, playbackNavIds[playbackNavCursor]);
   return 1;
}

static int nextPlaybackId(char out[12])
{
   if (playbackNavCursor < 0 || playbackNavCursor >= playbackNavCount - 1) return 0;
   playbackNavCursor++;
   strCopy(out, 12, playbackNavIds[playbackNavCursor]);
   return 1;
}

// Session-only autoplay history: 32 YouTube IDs (11 chars + terminator), reset on app restart.
#define AUTOPLAY_HISTORY_CAP 32
static char autoplayWatchedIds[AUTOPLAY_HISTORY_CAP][12];
static int autoplayWatchedCount;

static int autoplayIsIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void rememberAutoplayVideo(const char *input)
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   if (!input || !input[0]) return;

   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id && strlen(input) == 11) id = input;
   if (!id) return;

   char videoId[12];
   for (int i = 0; i < 11; i++) {
      if (!id[i] || !autoplayIsIdChar(id[i])) return;
      videoId[i] = id[i];
   }
   videoId[11] = 0;

   for (int i = 0; i < autoplayWatchedCount; i++)
      if (strcmp(autoplayWatchedIds[i], videoId) == 0) return;

   if (autoplayWatchedCount == AUTOPLAY_HISTORY_CAP) {
      memmove(autoplayWatchedIds, autoplayWatchedIds + 1,
              (AUTOPLAY_HISTORY_CAP - 1) * sizeof autoplayWatchedIds[0]);
      autoplayWatchedCount--;
   }
   strCopy(autoplayWatchedIds[autoplayWatchedCount++], sizeof autoplayWatchedIds[0], videoId);
}

typedef enum { STAGE_LOADING, STAGE_FAILED, STAGE_PLAYING } Stage;

static struct {
   volatile Stage   stage;
   char             message[128];     // failure reason (worker writes, UI reads)
   VideoPlayer     *pendingPlayer;    // built by the worker, adopted by the UI thread
   volatile int     workerDone;
   volatile int     cancelRequested; // cooperative cancellation for playback-owned work
   HttpRequestGroup requestGroup;   // isolates playback network I/O from downloads and navigation
   int              pauseWhenReady; // preserve Cross presses made before the first frame/player adoption
   int              threadActive;
   sys_ppu_thread_t workerTid;
   volatile int     nextWorkerDone;
   int              nextThreadActive, nextAttempted;
   sys_ppu_thread_t nextWorkerTid;
   char             nextVideoId[16];
   char             nextVideoTitle[256];
   uint8_t         *nextThumbJpeg;
   int              nextThumbJpegLen;
   GfxTexture       nextThumbTexture;
   int              nextNoticeVisible, nextImageRequested;
   uint64_t         nextNoticeStartedUs;
   VideoPlayer     *player;
   int              screenW, screenH;
   int              firstFrameSeen;   // a real frame has been presented (loading status ends, save is safe)
   int              diagFirstYuvDraw;
   int              historyRecorded;  // prevents duplicates after resolution switches
   int              showStats;        // debug overlay visibility; no controller binding
   char             title[256];        // resolved video title (worker sets, UI reads after adoption)
   char             author[128];       // resolved channel name
   char            *description;       // dynamically sized resolved description
   char             viewCount[32];
   char             publishedDate[24];
   char             likeCount[32];
   volatile int     metadataWorkerDone;
   int              metadataThreadActive, metadataFetchTried;
   sys_ppu_thread_t metadataWorkerTid;

   // description overlay: the text is rasterised once, on first open, into one tall texture that
   // up/down scroll. playback carries on untouched underneath.
   int              descriptionVisible;
   int              descriptionRendered;
   int              descriptionPage;    // current fixed-height description page

   // chapter picker: chapters come out of the description at resolve time; when that finds none, a
   // side worker asks the /next endpoint (YouTube's own chapter list). the row labels are rasterised
   // on first open, and the widest row sizes the panel.
   ChapterList      chapters;            // parsed by the worker (UI reads after adoption)
   ChapterList      fetchedChapters;     // /next result (worker writes, copied over on reap)
   volatile int     chWorkerDone;
   int              chThreadActive, chFetchTried;
   sys_ppu_thread_t chWorkerTid;
   int              chaptersVisible;
   int              chaptersRendered;
   int              chapterSelected;
   int              chapterTimeWidth;    // widest time label (the right-aligned time column)
   int              chapterPanelWidth;
   int              vidItag, vidW, vidH, vidFps, audItag;   // picked formats (worker sets, UI reads)
   char             videoUrls[MAX_PLAYBACK_QUALITIES][MAX_STREAM_URL];
   char             fallbackAudioUrl[MAX_STREAM_URL];
   int              videoItags[MAX_PLAYBACK_QUALITIES], videoWidths[MAX_PLAYBACK_QUALITIES];
   int              videoHeights[MAX_PLAYBACK_QUALITIES], videoFps[MAX_PLAYBACK_QUALITIES];
   int              videoHasAudio[MAX_PLAYBACK_QUALITIES];
   int              fallbackAudioItag;
   int              videoAvailable[MAX_PLAYBACK_QUALITIES], videoCount;
   int              resolutionIndex, autoResolutionIndex;
   VideoPlayer     *pendingResolutionPlayer;
   VideoPlayer     *retiringResolutionPlayer;
   volatile int     resolutionSwitchDone;
   int              resolutionSwitchThreadActive, resolutionSwitchWasPaused, resolutionSwitchWasFirstFrameSeen;
   int              resolutionSwitchTeeRecovery, teeRecoveryAttempted;
   int              resolutionSwitchFpsFallback;
   long             resolutionSwitchLiveEdgeSq, resolutionSwitchLiveFirstSq, resolutionSwitchLivePlaySq;
   float            resolutionSwitchLiveLatencySec;
   int              decoderStopWarningShown;
   int              resolutionSwitchTarget, resolutionSwitchOldIndex, resolutionSwitchRestored;
   volatile int     resolutionSwitchAborting;
   float            resolutionSwitchPosition;
   char             resolutionSwitchVideoUrl[MAX_STREAM_URL];
   char             resolutionSwitchAudioUrl[MAX_STREAM_URL];
   char             resolutionSwitchOldVideoUrl[MAX_STREAM_URL];
   char             resolutionSwitchOldAudioUrl[MAX_STREAM_URL];
   sys_ppu_thread_t resolutionSwitchTid;
   char             audioUrls[MAX_AUDIO_TRACKS][MAX_STREAM_URL];
   char             audioNames[MAX_AUDIO_TRACKS][64];
   char             audioIds[MAX_AUDIO_TRACKS][40];
   int              audioItags[MAX_AUDIO_TRACKS];
   int              audioCount, audioPick;
   volatile int     audioSwitchDone;
   int              audioSwitchThreadActive, audioSwitchWasPaused;
   int              audioFallbackAttempted;
   sys_ppu_thread_t audioSwitchTid;
   float            audioSwitchPosition;
   int              audioSwitchTarget, audioSwitchFallback;
   int              isLive;           // active or recently-ended DASH LIVE stream
   int              liveEnded;        // post-live recording: edge is fixed and playback ends there
   int              liveDvr;          // rewind enabled by the broadcaster (always enabled for post-live)
   int              liveAndroidClient; // active formats came from ANDROID; enables signed-base renewal
   int              liveAndroidFallbackSession; // session origin; keeps ANDROID first on every seek
   int              liveSegmentSec;   // target duration of each numbered DASH segment
   int              liveWindowSegs;   // advertised DVR window in segments
   long             liveFloorSq;      // earliest segment available in the manifest
   long             liveEdgeSq;       // newest segment at resolve time
   uint64_t         liveEdgeUs;       // when that edge was observed
   long             liveFirstSq;      // segment from which the current player was opened
   uint64_t         liveFirstPtsNs;   // PTS of its first presented frame
   int              liveAnchored;     // first frame has been presented
   float            liveLatencySec;   // normal startup distance behind the live edge
   char             liveVideoOpen[MAX_STREAM_URL + 48];
   char             liveAudioOpen[MAX_STREAM_URL + 48];
   VideoPlayer     *liveRetiringPlayer;
   int              liveReopenThreadActive;
   int              liveRefreshActive;
   int              liveRefreshAttempts;
   uint64_t         liveRefreshStableSinceUs;
   int              liveBaseRefreshActive;
   volatile int     liveBaseRefreshDone;
   int              liveBaseRefreshSuccess;
   int              liveBaseRefreshAttempts;
   uint64_t         liveBaseRefreshNextTryUs;
   sys_ppu_thread_t liveBaseRefreshTid;
   VideoPlayer     *liveBaseRefreshPlayer;
   StreamInfo      *liveBaseRefreshInfo;
   int              liveBaseRefreshVideoItag, liveBaseRefreshAudioItag;
   int              liveBaseRefreshNeedAudio;
   char             liveBaseRefreshAudioTrackId[40];
   char             liveBaseRefreshVideoBase[MAX_STREAM_URL];
   char             liveBaseRefreshAudioBase[MAX_STREAM_URL];
   long             liveBaseRefreshVideoStartSq, liveBaseRefreshVideoEdgeSq, liveBaseRefreshAudioEdgeSq;
   int              liveBaseRefreshWindowSec;
   uint64_t         liveLastFrameUs;  // last newly presented frame; detects a stalled LIVE transport
   int              liveEndCheckActive;
   volatile int     liveEndCheckDone;
   int              liveEndCheckResult;
   sys_ppu_thread_t liveEndCheckTid;
   int              liveEndDetected;
   int              liveFailureVisible;
   VideoPlayer     *vodRetiringPlayer;
   int              vodRefreshActive;
   int              vodRefreshAttempts;
   int              vodRefreshResumePending;
   float            vodRefreshPosition;
   uint64_t         vodRefreshStableSinceUs;
   const uint8_t   *lastFrame;        // fps measure: a changed pointer means a newly presented frame
   int              framesThisSecond, measuredFps;
   uint64_t         fpsTickUs;
   uint64_t         fpsWindowStartUs, fpsWindowFrames, fpsWindowMaxGapUs;
   uint64_t         fpsAdaptiveLastSwitchUs;
   int              fpsAdaptiveSwitches;
   int              fpsAdaptiveBadWindows;
   int              fpsAdaptiveLowBufferWindows;
   int              fpsFallbackRequested;

   // seek: the UI owns the scrub target so the bar moves instantly; the engine seek is deferred until
   // the input goes quiet (each apply costs a decoder flush + fragment reload)
   int              seeking;
   float            seekTarget;
   uint64_t         lastSeekInputUs;
   uint64_t         controlsShownUs;   // last seek-bar activity
   uint64_t         hudShownUs;        // last L2 full-HUD request
   int              controlsDismissed; // Circle explicitly hid the playback HUD, including while paused

   // sponsorblock: fetched on a side thread (in parallel with resolve/open), then auto-skipped during
   // playback and drawn on the seek bar. sbReady goes up once the thread is reaped and segments are safe.
   SponsorSegments  sb;
   volatile int     sbWorkerDone;
   int              sbThreadActive, sbReady;
   int              askPromptShown;  // current Ask segment already announced; -1 when outside one
   uint64_t         askPromptStartedUs;
   sys_ppu_thread_t sbWorkerTid;
   uint64_t         toastShownUs;      // brief toast timer (0 = hidden)
   uint64_t         toastDurationUs;   // per-toast lifetime; defaults to TOAST_VISIBLE_US
   uint64_t         liveReturnNoticeUs; // one-second "Back to LIVE" notice
   int              liveReturnNoticePending; // show only after the replacement LIVE frame arrives

   // subtitles: Options selects a track; Triangle toggles it on/off. A worker fetches it in the background
   // (updatePlay respawns the fetch whenever the pick and the fetched track disagree), and once ready
   // the cue for the current position drives subtitleLabel each frame.
   CaptionTrack    *captionTracks;     // dynamically sized to include every source and translated track
   int              captionCount;
   int              subtitlePick;        // chosen track index, -1 = off
   int              subtitleFetched;     // track index the fetch (in flight or done) belongs to, -1 = none
   int              subtitlesReady;      // fetched track is safe to read
   int              subtitleScanFrom;    // cue lookup cache (getSubtitleCueText)
   int              subtitleDisplayedCue;
   int              subtitleDisplayedPending;
   float            subtitleLastPosition;
   SubtitleTrack   *subtitleTrack;       // heap; ~800 KB, allocated on first enable, freed on exit
   volatile int     subWorkerDone;
   int              subtitleFetchResult; // fetch / translation result
   SubtitleTranslation subtitleTranslation;
   int              subtitleTranslationStoppedShown;
   int              subThreadActive;
   sys_ppu_thread_t subWorkerTid;
} state;

#define STAT_LINES 4

static Font  uiFont;
static Label statusLabel;
static uint64_t loadingAnimationStartedUs;
static Label statLabels[STAT_LINES];
static Label timeLeftLabel, timeRightLabel;   // seek bar: current time (left) / total (right)
static Label liveBadgeLabel, backToLabel;
static int liveBadgeLanguage = -1;
static Label liveFailureTitleLabel, liveFailureBodyLabel;
static ButtonHints liveFailureHints, playbackHints;
static Label toastLabel;                      // brief centred toast ("Skipped ...", "Subtitles: ...")
static Label askPromptPrefixLabel, askPromptSuffixLabel;
static Label titleLabel;                      // video title, top-left while the HUD is visible
static Label authorLabel;                     // channel name below the title
static Label nextNoticeHeadingLabel;           // "next video" notice heading
static Label nextNoticeTitleLabel;             // selected autoplay video title
static Label subtitleLabel;                   // active subtitle cue, above the seek bar
static Label playbackInfoLabel;               // resolution, current audio and active subtitles, top-right
static Label chapterLabels[MAX_CHAPTERS];     // chapter titles (picker overlay rows)
static Label chapterTimeLabels[MAX_CHAPTERS]; // chapter start times, right-aligned in their own column
static TextTexture descriptionTexture;        // only the currently visible description page
static Label descriptionPageLabel;
static const char *descriptionBody;
static size_t descriptionBodyLength;
static char descriptionMetadata[256];
static size_t descriptionMetadataLength;
static size_t descriptionContentLength;
static int descriptionPageCount;
static size_t descriptionPageMaxBytes = 64;
static int descriptionPageMaxNewlines = 3;
static void clearDescriptionPages(void);
static void renderDescriptionPage(int page);
static void recountDescriptionPages(void);
static void collapseDescriptionBlankLines(char *text);
static int statsTextPadding;

// the shared pill meter; its level is kept in `volumeLevel` outside `state` so it survives across videos.
static VolumeMeter volumeMeter;
static int volumeLevel = VOLUME_DEFAULT;
// Playback display preference: deliberately outside `state` so it survives between videos.
// It starts OFF on application launch and is toggled with R2 during playback.
static int cropToFillEnabled;
// Normalized pan position for the excess crop area; zero keeps the image centred.
static float cropPanX;
static float cropPanY;
static uint64_t cropPanLastUpdateUs;
// Subtitle enablement is session-only; language preference is persisted in settings.txt.
static int subtitlesEnabled;

static const StreamFormat *pickAudioForTrack(const StreamInfo *info, const char *trackId)
{
   const StreamFormat *best = NULL;
   int originalOnly = trackId && strcmp(trackId, "__original_upload__") == 0;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *f = &info->formats[i];
      if (!f->hasAudio || f->hasVideo || f->needsCipher || !f->url[0] || strcmp(f->container, "mp4") != 0) continue;
      if (originalOnly) {
         // YouTube commonly omits audioTrack metadata on the uploaded/original AAC rendition.
         if (f->audioTrackId[0] || f->audioIsAutoDubbed) continue;
      } else if (trackId && trackId[0] && strcmp(f->audioTrackId, trackId) != 0) continue;
      if (!best || f->itag == 140) best = f;
   }
   return best;
}

static int playbackCancelRequested(void)
{
   return __sync_fetch_and_add(&state.cancelRequested, 0) != 0;
}

static void requestPlaybackCancel(void)
{
   __sync_lock_test_and_set(&state.cancelRequested, 1);
   __sync_synchronize();
   // Interrupt playback-owned resolver, stream and prefetch requests before joining workers.
   cancelHttpRequestGroup(&state.requestGroup);
}

static void fail(const char *reason)
{
   if (playbackCancelRequested()) return;
   logError("[yt] play failed: %s\n", reason);
   strCopy(state.message, sizeof state.message, reason);
   __sync_synchronize();   // publish message before the UI thread sees STAGE_FAILED
   state.stage = STAGE_FAILED;
}

static void writeLiveDescriptor(char *out, int cap, const char *descriptor, long edgeSq, long playSq)
{
   out[0] = 0;
   if (strncmp(descriptor, "dashseg|", 8) != 0 && strncmp(descriptor, "dashvod|", 8) != 0 &&
       strncmp(descriptor, "dashand|", 8) != 0) return;
   long firstSq = atol(descriptor + 8);
   const char *edge = strchr(descriptor + 8, '|');
   if (!edge) return;
   edge = strchr(edge + 1, '|');
   if (!edge) return;
   snprintf(out, cap, "%.7s|%ld|%ld|%ld|%s", descriptor, firstSq, edgeSq, playSq, edge + 1);
}

static uint64_t liveBaseRefreshIntervalUs(void)
{
   uint64_t segmentSec = state.liveSegmentSec > 0 ? (uint64_t)state.liveSegmentSec : 5ull;
   uint64_t intervalSec = segmentSec * 4ull;
   if (intervalSec < 4ull) intervalSec = 4ull;
   if (intervalSec > 20ull) intervalSec = 20ull;
   return intervalSec * 1000000ull;
}

static uint64_t liveBaseRefreshRetryDelayUs(void)
{
   unsigned shift = state.liveBaseRefreshAttempts > 0 ? (unsigned)(state.liveBaseRefreshAttempts - 1) : 0;
   if (shift > 3) shift = 3;
   return (2000000ull << shift); // 2, 4, 8, then 16 seconds; no fixed attempt limit
}

static const StreamFormat *findLiveFormatByItag(const StreamInfo *info, int itag)
{
   if (!info || itag <= 0) return NULL;
   for (int i = 0; i < info->formatCount; i++)
      if (info->formats[i].itag == itag && info->formats[i].isLiveSegmented &&
          info->formats[i].hasVideo) return &info->formats[i];
   return NULL;
}

static const StreamFormat *findLiveAudioFormatByItag(const StreamInfo *info, int itag, const char *trackId)
{
   if (!info || itag <= 0) return NULL;
   int original = trackId && strcmp(trackId, "__original_upload__") == 0;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (format->itag != itag || !format->isLiveSegmented || !format->hasAudio || format->hasVideo) continue;
      if (original) {
         if (!format->audioTrackId[0] && !format->audioIsAutoDubbed) return format;
      } else if (!trackId || !trackId[0] || strcmp(format->audioTrackId, trackId) == 0) {
         return format;
      }
   }
   return NULL;
}

static int extractLiveFormatBase(const StreamFormat *format, char *out, int cap)
{
   if (!format || !out || cap <= 0 || strncmp(format->url, "dashand|", 8) != 0) return -1;
   const char *first = strchr(format->url + 8, '|');
   const char *second = first ? strchr(first + 1, '|') : NULL;
   if (!second || !second[1]) return -1;
   int length = (int)strlen(second + 1);
   if (length >= cap) return -1;
   memcpy(out, second + 1, (size_t)length + 1);
   return 0;
}

// Refresh every stored rendition from the selected client. This also keeps later
// manual quality/audio switches coherent if a seek had to move from ANDROID to VISIONOS.
static void updateStoredLiveUrls(const StreamInfo *info)
{
   for (int i = 0; i < state.videoCount; i++) {
      const StreamFormat *format = findLiveFormatByItag(info, state.videoItags[i]);
      if (format)
         strCopy(state.videoUrls[i], sizeof state.videoUrls[i], format->url);
   }
   for (int i = 0; i < state.audioCount; i++) {
      const StreamFormat *format = findLiveAudioFormatByItag(info, state.audioItags[i], state.audioIds[i]);
      if (format)
         strCopy(state.audioUrls[i], sizeof state.audioUrls[i], format->url);
   }
   if (state.fallbackAudioItag > 0) {
      const StreamFormat *format = findLiveAudioFormatByItag(info, state.fallbackAudioItag, NULL);
      if (format)
         strCopy(state.fallbackAudioUrl, sizeof state.fallbackAudioUrl, format->url);
   }
}

// Resolve a fresh manifest using ANDROID only. The active VideoPlayer keeps decoding while
// this worker prepares the matching itag BaseURLs; no VISIONOS request or player rebuild occurs.
static void liveBaseRefreshWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   StreamInfo *info = (StreamInfo *)calloc(1, sizeof *info);
   int rc = -1;
   if (info && !playbackCancelRequested()) {
      const Extractor *extractor = findExtractor(requestedInput);
      if (extractor && extractor->refreshLiveAndroid)
         rc = extractor->refreshLiveAndroid(requestedInput, state.liveBaseRefreshVideoItag,
                                             state.liveBaseRefreshNeedAudio ? state.liveBaseRefreshAudioItag : 0,
                                             info);
   }
   state.liveBaseRefreshSuccess = 0;
   state.liveBaseRefreshVideoBase[0] = 0;
   state.liveBaseRefreshAudioBase[0] = 0;
   if (rc == 0 && !playbackCancelRequested()) {
      const StreamFormat *video = findLiveFormatByItag(info, state.liveBaseRefreshVideoItag);
      const StreamFormat *audio = state.liveBaseRefreshNeedAudio
                                ? findLiveAudioFormatByItag(info, state.liveBaseRefreshAudioItag,
                                                           state.liveBaseRefreshAudioTrackId) : NULL;
      if (video && (!state.liveBaseRefreshNeedAudio || audio) &&
          extractLiveFormatBase(video, state.liveBaseRefreshVideoBase,
                                sizeof state.liveBaseRefreshVideoBase) == 0 &&
          (!audio || extractLiveFormatBase(audio, state.liveBaseRefreshAudioBase,
                                           sizeof state.liveBaseRefreshAudioBase) == 0)) {
         state.liveBaseRefreshVideoStartSq = video->liveStartSq;
         state.liveBaseRefreshVideoEdgeSq = video->liveEdgeSq;
         state.liveBaseRefreshWindowSec = info->liveWindowSec;
         state.liveBaseRefreshAudioEdgeSq = audio ? audio->liveEdgeSq : 0;
         state.liveBaseRefreshSuccess = 1;
      }
   }
   if (!state.liveBaseRefreshSuccess && !playbackCancelRequested())
      logWarn("[yt-live-renew] Android manifest refresh failed rc=%d videoItag=%d audioItag=%d\n",
              rc, state.liveBaseRefreshVideoItag, state.liveBaseRefreshNeedAudio ? state.liveBaseRefreshAudioItag : 0);
   if (state.liveBaseRefreshSuccess && !playbackCancelRequested()) {
      state.liveBaseRefreshInfo = info;
      info = NULL;
   }
   freeStreamInfo(info);
   __sync_synchronize();
   state.liveBaseRefreshDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static void scheduleLiveBaseRefresh(int urgent)
{
   if (!state.isLive || !state.liveAndroidClient || state.liveEnded ||
       state.stage != STAGE_PLAYING || !state.player || state.liveBaseRefreshActive ||
       state.liveRefreshActive || state.threadActive || state.resolutionSwitchThreadActive ||
       state.audioSwitchThreadActive || state.liveReopenThreadActive || state.subThreadActive)
      return;
   uint64_t now = sys_time_get_system_time();
   if (state.liveBaseRefreshNextTryUs && now < state.liveBaseRefreshNextTryUs &&
       (!urgent || state.liveBaseRefreshAttempts > 0)) return;

   state.liveBaseRefreshPlayer = state.player;
   state.liveBaseRefreshVideoItag = state.vidItag;
   state.liveBaseRefreshAudioItag = state.audItag;
   state.liveBaseRefreshNeedAudio = isVideoPlayerUsingSeparateAudio(state.player);
   state.liveBaseRefreshAudioTrackId[0] = 0;
   if (state.liveBaseRefreshNeedAudio && state.audioPick >= 0 && state.audioPick < state.audioCount)
      strCopy(state.liveBaseRefreshAudioTrackId, sizeof state.liveBaseRefreshAudioTrackId,
              state.audioIds[state.audioPick]);
   state.liveBaseRefreshSuccess = 0;
   state.liveBaseRefreshInfo = NULL;
   state.liveBaseRefreshDone = 0;
   state.liveBaseRefreshActive = 1;
   state.liveBaseRefreshAttempts++;
   logInfo("[yt-live-renew] starting Android BaseURL refresh attempt=%d urgent=%d itags=%d/%d separateAudio=%d\n",
           state.liveBaseRefreshAttempts, urgent, state.liveBaseRefreshVideoItag,
           state.liveBaseRefreshNeedAudio ? state.liveBaseRefreshAudioItag : 0,
           state.liveBaseRefreshNeedAudio);
   if (spawnJoinableThread(&state.liveBaseRefreshTid, liveBaseRefreshWorker, 0,
                           THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-live-base-refresh") != 0) {
      state.liveBaseRefreshActive = 0;
      state.liveBaseRefreshNextTryUs = now + liveBaseRefreshRetryDelayUs();
      logError("[yt-live-renew] could not start Android BaseURL refresh worker\n");
   }
}

static void reapLiveBaseRefresh(void)
{
   if (!state.liveBaseRefreshActive || !state.liveBaseRefreshDone) return;
   joinThread(state.liveBaseRefreshTid);
   __sync_synchronize();
   state.liveBaseRefreshActive = 0;
   state.liveBaseRefreshDone = 0;
   uint64_t now = sys_time_get_system_time();
   int applied = 0;
   StreamInfo *info = state.liveBaseRefreshInfo;
   int audioTrackMatches = !state.liveBaseRefreshNeedAudio ||
      (state.audioPick >= 0 && state.audioPick < state.audioCount &&
       strcmp(state.audioIds[state.audioPick], state.liveBaseRefreshAudioTrackId) == 0) ||
      (state.audioPick < 0 && !state.liveBaseRefreshAudioTrackId[0]);
   if (state.liveBaseRefreshSuccess && info && state.player == state.liveBaseRefreshPlayer &&
       state.isLive && state.liveAndroidClient && !state.resolutionSwitchThreadActive &&
       !state.audioSwitchThreadActive && !state.liveReopenThreadActive && audioTrackMatches &&
       state.vidItag == state.liveBaseRefreshVideoItag && state.audItag == state.liveBaseRefreshAudioItag) {
      applied = refreshVideoPlayerLiveBases(state.player,
                    state.liveBaseRefreshVideoBase, state.liveBaseRefreshVideoEdgeSq,
                    state.liveBaseRefreshNeedAudio ? state.liveBaseRefreshAudioBase : NULL,
                    state.liveBaseRefreshAudioEdgeSq) == 0;
   }
   if (applied) {
      updateStoredLiveUrls(info);
      state.liveEdgeSq = state.liveBaseRefreshVideoEdgeSq;
      state.liveFloorSq = state.liveBaseRefreshVideoStartSq;
      if (state.liveBaseRefreshWindowSec > 0 && state.liveSegmentSec > 0) {
         state.liveWindowSegs = state.liveBaseRefreshWindowSec / state.liveSegmentSec;
         if (state.liveWindowSegs < 1) state.liveWindowSegs = 1;
      }
      state.liveEdgeUs = now;
      state.liveBaseRefreshAttempts = 0;
      state.liveRefreshAttempts = 0;
      state.liveBaseRefreshNextTryUs = now + liveBaseRefreshIntervalUs();
      logInfo("[yt-live-renew] Android BaseURL pair applied without restarting playback; edgeSq=%ld next refresh in %llums\n",
              state.liveEdgeSq, (unsigned long long)(liveBaseRefreshIntervalUs()/1000ull));
   } else {
      state.liveBaseRefreshNextTryUs = now + liveBaseRefreshRetryDelayUs();
      logWarn("[yt-live-renew] Android BaseURL refresh not applied; retry in %llums\n",
              (unsigned long long)(liveBaseRefreshRetryDelayUs()/1000ull));
   }
   freeStreamInfo(info);
   state.liveBaseRefreshInfo = NULL;
}

// Wait for the decoder/audio pipeline to produce a usable pre-roll. This runs on the
// playback worker, never on the UI thread. A timeout is treated as a failed rendition
// and allows the diagnostic fallback to try another format.
static int waitForInitialPlayerReady(VideoPlayer *player, unsigned timeoutMs)
{
   uint64_t started = sys_time_get_system_time();
   while (player && !playbackCancelRequested() && !isVideoPlayerReady(player) &&
          !isVideoPlayerFailed(player) &&
          sys_time_get_system_time() - started < (uint64_t)timeoutMs * 1000ull)
      sleepMs(50);
   return player && !playbackCancelRequested() && isVideoPlayerReady(player);
}

static int fallbackFormatUsable(const StreamFormat *f)
{
   if (!f || !f->hasVideo || f->needsCipher || !f->url[0]) return 0;
   if (strcmp(f->container, "mp4") != 0) return 0;
   if (f->height < 144 || f->height > 1080 || f->fps <= 0 || f->fps > 60) return 0;
   return isPlaybackVideoFormatDecodable(f);
}

// Lower values have higher fallback priority. The order is deliberately shared
// by initial LIVE and VOD recovery: 1080p60, 1080p30, 720p60, 720p30,
// 480p, 360p, 240p, then 144p.
static int fallbackQualityRank(const StreamFormat *f)
{
   if (f->height > 720) return f->fps >= 50 ? 0 : 1;
   if (f->height > 480) return f->fps >= 50 ? 2 : 3;
   if (f->height > 360) return 4;
   if (f->height > 240) return 5;
   if (f->height > 144) return 6;
   return 7;
}

static int fallbackFormatBefore(const StreamFormat *a, const StreamFormat *b,
                                int preferSeparateAudio)
{
   int rankA = fallbackQualityRank(a);
   int rankB = fallbackQualityRank(b);
   if (rankA != rankB) return rankA < rankB;
   if (a->height != b->height) return a->height > b->height;
   if (a->fps != b->fps) return a->fps > b->fps;
   if (preferSeparateAudio && a->hasAudio != b->hasAudio) return !a->hasAudio;
   return a->width > b->width;
}

// Try the normal resolver pick first. If it cannot produce a decoded pre-roll,
// test every distinct compatible MP4 quality in the requested priority order.
// This path is used for both LIVE and VOD, including segmented LIVE URLs.
static VideoPlayer *openInitialWithFallback(const StreamInfo *info,
                                             const StreamFormat **selectedVideo,
                                             const StreamFormat **selectedAudio,
                                             int allowFallback)
{
   const StreamFormat *video = *selectedVideo;
   const StreamFormat *audio = *selectedAudio;
   const char *audioUrl = (video->hasAudio || !audio) ? NULL : audio->url;
   VideoPlayer *player;
   int primaryReady;

   logInfo("[yt-fallback] primary attempt itag=%d audio=%d urlLen=%d audioUrlLen=%d\n",
           video->itag, video->hasAudio ? video->itag : (audio ? audio->itag : 0),
           (int)strlen(video->url), audioUrl ? (int)strlen(audioUrl) : 0);
   if (playbackCancelRequested()) return NULL;
   player = createVideoPlayerSplitCancelable(video->url, audioUrl, allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);
   if (playbackCancelRequested()) {
      if (player) destroyVideoPlayer(player);
      return NULL;
   }
   primaryReady = player && (!allowFallback || waitForInitialPlayerReady(player, 10000));
   if (playbackCancelRequested()) {
      if (player) destroyVideoPlayer(player);
      return NULL;
   }
   if (primaryReady) {
      if (allowFallback) logInfo("[yt-fallback] primary ready itag=%d\n", video->itag);
      return player;
   }
   if (player) {
      logWarn("[yt-fallback] primary not ready after 10000ms itag=%d\n", video->itag);
      destroyVideoPlayer(player);
   } else {
      logWarn("[yt-fallback] primary open failed itag=%d\n", video->itag);
   }
   if (playbackCancelRequested() || !allowFallback) return NULL;

   const StreamFormat *attempts[MAX_INITIAL_FALLBACK_ATTEMPTS];
   int count = 0;
   for (int i = 0; i < info->formatCount && count < MAX_INITIAL_FALLBACK_ATTEMPTS; i++) {
      const StreamFormat *f = &info->formats[i];
      if (!fallbackFormatUsable(f) || f == video || strcmp(f->url, video->url) == 0) continue;

      int duplicate = 0;
      for (int j = 0; j < count; j++) {
         if (attempts[j]->height == f->height && attempts[j]->fps == f->fps) {
            // Keep one candidate per quality. Prefer video-only when a separate audio
            // stream is available, then prefer the wider rendition within the same tier.
            if (fallbackFormatBefore(f, attempts[j], audio != NULL)) attempts[j] = f;
            duplicate = 1;
            break;
         }
         if (strcmp(attempts[j]->url, f->url) == 0) {
            duplicate = 1;
            break;
         }
      }
      if (duplicate) continue;

      int insert = count;
      while (insert > 0 && fallbackFormatBefore(f, attempts[insert - 1], audio != NULL)) {
         attempts[insert] = attempts[insert - 1];
         insert--;
      }
      attempts[insert] = f;
      count++;
   }

   for (int i = 0; i < count; i++) {
      if (playbackCancelRequested()) return NULL;
      const StreamFormat *candidate = attempts[i];
      const char *candidateAudio = (candidate->hasAudio || !audio) ? NULL : audio->url;
      logInfo("[yt-fallback] attempt=%d/%d videoItag=%d %dx%d fps=%d muxed=%d audioItag=%d\n",
              i + 1, count, candidate->itag, candidate->width, candidate->height,
              candidate->fps, candidate->hasAudio,
              candidate->hasAudio ? candidate->itag : (audio ? audio->itag : 0));
      player = createVideoPlayerSplitCancelable(candidate->url, candidateAudio,
                                                allocGfxVideoBuffer, freeGfxVideoBuffer,
                                                &state.cancelRequested);
      if (playbackCancelRequested()) {
         if (player) destroyVideoPlayer(player);
         return NULL;
      }
      if (!player) {
         logWarn("[yt-fallback] open failed itag=%d\n", candidate->itag);
         continue;
      }
      if (!waitForInitialPlayerReady(player, 8000)) {
         destroyVideoPlayer(player);
         if (playbackCancelRequested()) return NULL;
         logWarn("[yt-fallback] pre-roll timeout itag=%d after 8000ms\n", candidate->itag);
         continue;
      }
      if (playbackCancelRequested()) {
         destroyVideoPlayer(player);
         return NULL;
      }
      *selectedVideo = candidate;
      *selectedAudio = candidate->hasAudio ? NULL : audio;
      logInfo("[yt-fallback] SUCCESS videoItag=%d audioItag=%d\n",
              candidate->itag, candidate->hasAudio ? candidate->itag : (audio ? audio->itag : 0));
      return player;
   }
   logError("[yt-fallback] exhausted: no candidate reached decoded pre-roll\n");
   return NULL;
}

static int addPlaybackQuality(const StreamFormat *format,
                              const StreamFormat *qualityFormats[MAX_PLAYBACK_QUALITIES],
                              int preferVideoOnly)
{
   if (!isPlaybackVideoFormatDecodable(format)) return 0;
   int insert = 0;
   while (insert < state.videoCount &&
          (state.videoHeights[insert] < format->height ||
           (state.videoHeights[insert] == format->height && state.videoFps[insert] < format->fps)))
      insert++;

   if (insert < state.videoCount && state.videoHeights[insert] == format->height &&
       state.videoFps[insert] == format->fps) {
      // Keep one rendition per resolution/frame-rate pair. Prefer video-only when an AAC
      // track can be paired, otherwise retain the muxed rendition.
      if (format->hasAudio && !qualityFormats[insert]->hasAudio) return 0;
      if (!format->hasAudio && qualityFormats[insert]->hasAudio && !preferVideoOnly) return 0;
      qualityFormats[insert] = format;
      strCopy(state.videoUrls[insert], sizeof state.videoUrls[insert], format->url);
      state.videoItags[insert] = format->itag;
      state.videoWidths[insert] = format->width;
      state.videoHeights[insert] = format->height;
      state.videoFps[insert] = format->fps;
      state.videoHasAudio[insert] = format->hasAudio;
      return 1;
   }
   if (state.videoCount >= MAX_PLAYBACK_QUALITIES) {
      if (insert == 0) return 0;
      for (int i = 0; i < state.videoCount - 1; i++) {
         qualityFormats[i] = qualityFormats[i + 1];
         strCopy(state.videoUrls[i], sizeof state.videoUrls[i], state.videoUrls[i + 1]);
         state.videoItags[i] = state.videoItags[i + 1];
         state.videoWidths[i] = state.videoWidths[i + 1];
         state.videoHeights[i] = state.videoHeights[i + 1];
         state.videoFps[i] = state.videoFps[i + 1];
         state.videoHasAudio[i] = state.videoHasAudio[i + 1];
         state.videoAvailable[i] = state.videoAvailable[i + 1];
      }
      state.videoCount--;
      insert--;
   }
   for (int i = state.videoCount; i > insert; i--) {
      qualityFormats[i] = qualityFormats[i - 1];
      strCopy(state.videoUrls[i], sizeof state.videoUrls[i], state.videoUrls[i - 1]);
      state.videoItags[i] = state.videoItags[i - 1];
      state.videoWidths[i] = state.videoWidths[i - 1];
      state.videoHeights[i] = state.videoHeights[i - 1];
      state.videoFps[i] = state.videoFps[i - 1];
      state.videoHasAudio[i] = state.videoHasAudio[i - 1];
      state.videoAvailable[i] = state.videoAvailable[i - 1];
   }
   qualityFormats[insert] = format;
   strCopy(state.videoUrls[insert], sizeof state.videoUrls[insert], format->url);
   state.videoItags[insert] = format->itag;
   state.videoWidths[insert] = format->width;
   state.videoHeights[insert] = format->height;
   state.videoFps[insert] = format->fps;
   state.videoHasAudio[insert] = format->hasAudio;
   state.videoAvailable[insert] = 1;
   state.videoCount++;
   return 1;
}

static int findPreferredQuality(void)
{
   if (state.videoCount <= 0) return -1;
   if (preferredQualityHeight <= 0) return state.videoCount - 1;

   int exact = -1, sameHeightLowerFps = -1, sameHeightHigherFps = -1;
   int lower = -1, higher = -1;
   for (int i = 0; i < state.videoCount; i++) {
      int height = state.videoHeights[i], fps = state.videoFps[i];
      if (height == preferredQualityHeight && fps == preferredQualityFps) exact = i;
      else if (height == preferredQualityHeight) {
         if (fps <= preferredQualityFps &&
             (sameHeightLowerFps < 0 || fps > state.videoFps[sameHeightLowerFps]))
            sameHeightLowerFps = i;
         if (fps > preferredQualityFps &&
             (sameHeightHigherFps < 0 || fps < state.videoFps[sameHeightHigherFps]))
            sameHeightHigherFps = i;
      } else if (height < preferredQualityHeight) {
         if (lower < 0 || height > state.videoHeights[lower] ||
             (height == state.videoHeights[lower] && fps > state.videoFps[lower])) lower = i;
      } else {
         if (higher < 0 || height < state.videoHeights[higher] ||
             (height == state.videoHeights[higher] && fps > state.videoFps[higher])) higher = i;
      }
   }
   if (exact >= 0) return exact;
   if (sameHeightLowerFps >= 0) return sameHeightLowerFps;
   if (sameHeightHigherFps >= 0) return sameHeightHigherFps;
   if (lower >= 0) return lower;
   return higher >= 0 ? higher : state.videoCount - 1;
}

static void prepareQualityOptions(void)
{
   int count = (state.isLive ? 1 : state.videoCount + 1);
   if (count > OPTIONS_DROPDOWN_MAX) count = OPTIONS_DROPDOWN_MAX;
   optionsValueCounts[0] = count;
   optionsQualityTargets[0] = -1;
   if (state.autoResolutionIndex >= 0) {
      snprintf(optionsQualityLabels[0], sizeof optionsQualityLabels[0], LOC(STR_AUTO_QUALITY_FORMAT),
               state.videoHeights[state.autoResolutionIndex], state.videoFps[state.autoResolutionIndex]);
   } else {
      strCopy(optionsQualityLabels[0], sizeof optionsQualityLabels[0], LOC(STR_QUALITY_AUTO_LABEL));
   }
   for (int item = 1; item < count; item++) {
      int target = state.videoCount - item;
      optionsQualityTargets[item] = target;
      snprintf(optionsQualityLabels[item], sizeof optionsQualityLabels[item], "%dp%d",
               state.videoHeights[target], state.videoFps[target]);
   }
}

static int qualityDropdownIndexForCurrent(void)
{
   for (int item = 1; item < optionsValueCounts[0]; item++)
      if (optionsQualityTargets[item] == state.resolutionIndex) return item;
   return 0;
}

static void prepareAudioOptions(void)
{
   int count = state.audioCount;
   if (count > MAX_AUDIO_TRACKS) count = MAX_AUDIO_TRACKS;
   if (count <= 0) {
      optionsValueCounts[1] = 1;
      if (state.resolutionIndex >= 0 && state.resolutionIndex < state.videoCount &&
          state.videoHasAudio[state.resolutionIndex])
         strCopy(optionsAudioLabels[0], sizeof optionsAudioLabels[0], LOC(STR_ORIGINAL_UPLOAD_PLAYBACK));
      else
         strCopy(optionsAudioLabels[0], sizeof optionsAudioLabels[0], LOC(STR_UNAVAILABLE));
      return;
   }

   optionsValueCounts[1] = count;
   for (int i = 0; i < count; i++)
      strCopy(optionsAudioLabels[i], sizeof optionsAudioLabels[i], state.audioNames[i]);
}

static int subtitleLanguageBaseMatches(const char *a, const char *b);
static int subtitleNamesMatch(const char *a, const char *b);
static int subtitleIsAutoGenerated(const CaptionTrack *track);
static int subtitleIsAutoTranslated(const CaptionTrack *track);
static int subtitleMainTrackCount(void);

static void prepareSubtitleOptions(void)
{
   if (state.isLive || state.captionCount <= 0) {
      optionsValueCounts[3] = 1;
      return;
   }
   optionsValueCounts[3] = subtitleMainTrackCount();
}

static int subtitleLanguageBaseMatches(const char *a, const char *b)
{
   int i = 0;
   while (a[i] && b[i] && a[i] != '-' && b[i] != '-') {
      char ca = a[i], cb = b[i];
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb) return 0;
      i++;
   }
   return i > 0 && (a[i] == 0 || a[i] == '-') && (b[i] == 0 || b[i] == '-');
}


// Compare YouTube display names after removing the optional generated-caption suffix.
// This makes "French" and "French (auto-generated)" the same language without
// depending on the exact capitalization or on a locale-specific regex engine.
static int subtitleNamesMatch(const char *a, const char *b)
{
   static const char *suffixes[] = { " (auto-generated)", " (auto-translated)" };
   int lengthA = (int)strlen(a), lengthB = (int)strlen(b);
   for (int suffixIndex = 0; suffixIndex < 2; suffixIndex++) {
      int suffixLength = (int)strlen(suffixes[suffixIndex]);
      if (lengthA >= suffixLength) {
         int match = 1;
         for (int j = 0; j < suffixLength; j++) {
            char c = a[lengthA - suffixLength + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != suffixes[suffixIndex][j]) { match = 0; break; }
         }
         if (match) lengthA -= suffixLength;
      }
      if (lengthB >= suffixLength) {
         int match = 1;
         for (int j = 0; j < suffixLength; j++) {
            char c = b[lengthB - suffixLength + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != suffixes[suffixIndex][j]) { match = 0; break; }
         }
         if (match) lengthB -= suffixLength;
      }
   }
   if (lengthA <= 0 || lengthA != lengthB) return 0;
   for (int i = 0; i < lengthA; i++) {
      char ca = a[i], cb = b[i];
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb) return 0;
   }
   return 1;
}


static int subtitleIsAutoTranslated(const CaptionTrack *track)
{
   return track->isAutoTranslated || strstr(track->name, "auto-translated") != NULL;
}

static int subtitleIsAutoGenerated(const CaptionTrack *track)
{
   return !subtitleIsAutoTranslated(track) &&
          (track->isAutoGenerated || strstr(track->name, "auto-generated") != NULL);
}

static int subtitleMainTrackCount(void)
{
   int count = 0;
   for (int i = 0; i < state.captionCount; i++)
      if (!subtitleIsAutoGenerated(&state.captionTracks[i]) &&
          !subtitleIsAutoTranslated(&state.captionTracks[i])) count++;
   for (int i = 0; i < state.captionCount; i++)
      if (subtitleIsAutoGenerated(&state.captionTracks[i])) count++;
   for (int i = 0; i < state.captionCount; i++)
      if (subtitleIsAutoTranslated(&state.captionTracks[i])) count++;
   return count;
}

static int subtitleMenuCount(void)
{
   int count = subtitleMainTrackCount();
   return count > 0 ? count : 1;
}

// Resolve a visible row to its actual CaptionTrack index. Translations follow source tracks.
static int subtitleTrackAtMenuIndex(int menuIndex)
{
   int visible = 0;
   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (subtitleIsAutoGenerated(track) || subtitleIsAutoTranslated(track)) continue;
      if (visible++ == menuIndex) return i;
   }
   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (!subtitleIsAutoGenerated(track)) continue;
      if (visible++ == menuIndex) return i;
   }
   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (!subtitleIsAutoTranslated(track)) continue;
      if (visible++ == menuIndex) return i;
   }
   return -1;
}

static int subtitleMainMenuIndexForTrack(int trackIndex)
{
   if (trackIndex < 0 || trackIndex >= state.captionCount) return 0;

   int visible = 0;
   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (subtitleIsAutoGenerated(track) || subtitleIsAutoTranslated(track)) continue;
      if (i == trackIndex) return visible;
      visible++;
   }
   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (!subtitleIsAutoGenerated(track)) continue;
      if (i == trackIndex) return visible;
      visible++;
   }
   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (!subtitleIsAutoTranslated(track)) continue;
      if (i == trackIndex) return visible;
      visible++;
   }
   return 0;
}

// Creator-provided captions win, followed by auto-generated and then auto-translated
// captions in the requested language. English ASR remains the final fallback.
static int findPreferredSubtitleTrack(const char *languageCode)
{
   if (!languageCode || !languageCode[0]) languageCode = "en";

   const char *selectedName = NULL;
   for (int i = 0; i < state.captionCount; i++) {
      if (subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, languageCode)) {
         selectedName = state.captionTracks[i].name;
         break;
      }
   }

   for (int i = 0; i < state.captionCount; i++)
      if (!subtitleIsAutoGenerated(&state.captionTracks[i]) &&
          !subtitleIsAutoTranslated(&state.captionTracks[i]) &&
          (subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, languageCode) ||
           (selectedName && subtitleNamesMatch(state.captionTracks[i].name, selectedName)))) return i;

   for (int i = 0; i < state.captionCount; i++)
      if (subtitleIsAutoGenerated(&state.captionTracks[i]) &&
          (subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, languageCode) ||
           (selectedName && subtitleNamesMatch(state.captionTracks[i].name, selectedName)))) return i;

   for (int i = 0; i < state.captionCount; i++)
      if (subtitleIsAutoTranslated(&state.captionTracks[i]) &&
          subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, languageCode)) return i;

   const char *autoTarget = getAutoTranslateLanguage();
   if (autoTarget && autoTarget[0] && strcmp(autoTarget, "off") != 0 &&
       !subtitleLanguageBaseMatches(languageCode, autoTarget)) {
      for (int i = 0; i < state.captionCount; i++)
         if (subtitleIsAutoTranslated(&state.captionTracks[i]) &&
             subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, autoTarget)) return i;
   }

   for (int i = 0; i < state.captionCount; i++)
      if (subtitleIsAutoGenerated(&state.captionTracks[i]) &&
          subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, "en")) return i;
   return -1;
}

static int getRequestedDownloadedId(char out[12])
{
   if (!requestedIsLocal || !requestedLocalPath[0]) return 0;
   const char *base = strrchr(requestedLocalPath, '/');
   base = base ? base + 1 : requestedLocalPath;
   size_t length = strlen(base);
   if (length != 15 || strcmp(base + 11, ".mp4") != 0) return 0;
   memcpy(out, base, 11);
   out[11] = 0;
   return 1;
}

static int appendAutoTranslatedCaptionTrack(const char *targetLanguage, int localOnly)
{
   if (!targetLanguage || !targetLanguage[0] || strcmp(targetLanguage, "off") == 0 ||
       state.captionCount <= 0 || !state.captionTracks) return 0;

   for (int i = 0; i < state.captionCount; i++) {
      CaptionTrack *track = &state.captionTracks[i];
      if (!subtitleIsAutoTranslated(track) &&
          subtitleLanguageBaseMatches(track->languageCode, targetLanguage)) return 0;
      if (subtitleIsAutoTranslated(track) &&
          subtitleLanguageBaseMatches(track->languageCode, targetLanguage)) return 0;
   }

   int sourceIndex = -1;
   for (int pass = 0; pass < 2 && sourceIndex < 0; pass++) {
      for (int i = 0; i < state.captionCount; i++) {
         CaptionTrack *track = &state.captionTracks[i];
         if (!track->languageCode[0] || subtitleIsAutoTranslated(track) ||
             subtitleLanguageBaseMatches(track->languageCode, targetLanguage)) continue;
         if (localOnly ? !track->isLocalFile : !track->isTranslatable) continue;
         if (pass == 0 && track->isAutoGenerated) continue;
         sourceIndex = i;
         break;
      }
   }
   if (sourceIndex < 0) return 0;

   CaptionTrack source = state.captionTracks[sourceIndex];
   CaptionTrack *expanded = realloc(state.captionTracks,
                                    (size_t)(state.captionCount + 1) * sizeof *expanded);
   if (!expanded) {
      logWarn("[subs] unable to allocate automatic translation track\n");
      return -1;
   }
   state.captionTracks = expanded;
   CaptionTrack *translated = &state.captionTracks[state.captionCount];
   memset(translated, 0, sizeof *translated);
   const char *languageName = getAutoTranslateLanguageName(targetLanguage);
   if (!languageName || !languageName[0]) languageName = targetLanguage;
   const char *translatedSuffix = LOC(STR_AUTO_TRANSLATED_SUFFIX);
   size_t suffixLength = strlen(translatedSuffix);
   size_t maxLanguageLength = sizeof translated->name - suffixLength - 1;
   snprintf(translated->name, sizeof translated->name, "%.*s%s",
            (int)maxLanguageLength, languageName, translatedSuffix);
   strCopy(translated->languageCode, sizeof translated->languageCode, targetLanguage);
   strCopy(translated->sourceLanguageCode, sizeof translated->sourceLanguageCode, source.languageCode);
   strCopy(translated->url, sizeof translated->url, source.url);
   translated->isAutoTranslated = 1;
   translated->isLocalFile = source.isLocalFile;
   translated->isTranslatable = 0;
   translated->isAutoGenerated = 0;
   state.captionCount++;
   logInfo("[subs] added %s translation from %s source (%s)\n",
           targetLanguage, source.languageCode, localOnly ? "downloaded" : "online");
   return 1;
}

#if BUILD_DIAG_LIVE_RECOVERY
static unsigned liveUrlFingerprint(const char *url)
{
   unsigned hash = 2166136261u;
   if (!url) return 0;
   while (*url) { hash ^= (unsigned char)*url++; hash *= 16777619u; }
   return hash;
}

static long long liveUrlExpiry(const char *url)
{
   const char *p = url ? strstr(url, "expire=") : NULL;
   if (p) return strtoll(p + 7, NULL, 10);
   p = url ? strstr(url, "/expire/") : NULL;
   return p ? strtoll(p + 8, NULL, 10) : 0;
}
#endif

static void worker(uint64_t arg)
{
   (void)arg;
#if BUILD_DIAG_LIVE_RECOVERY
   int liveRecoveryWorker = state.liveRefreshActive;
   uint64_t recoveryWorkerStartUs = sys_time_get_system_time();
   uint64_t recoveryResolveStartUs = 0;
#endif
   setHttpRequestGroup(&state.requestGroup);
   StreamInfo *info = NULL;
   if (requestedIsLocal) {
      char fallback[256];
      strCopy(fallback, sizeof fallback, LOC(STR_DOWNLOADED_VIDEO));
      const char *base = strrchr(requestedLocalPath, '/');
      if (base) {
         strCopy(fallback, sizeof fallback, base + 1);
         char *extension = strrchr(fallback, '.');
         if (extension) *extension = 0;
      }
      strCopy(state.title, sizeof state.title, fallback);
      strCopy(state.author, sizeof state.author, LOC(STR_DOWNLOADED_AUTHOR));
      char localDescription[16384] = "";
      loadDownloadedMetadata(requestedLocalPath, state.title, sizeof state.title,
                             state.author, sizeof state.author,
                             localDescription, sizeof localDescription,
                             state.publishedDate, sizeof state.publishedDate,
                             state.viewCount, sizeof state.viewCount,
                             state.likeCount, sizeof state.likeCount);
      if (localDescription[0]) {
         state.description = (char *)malloc(strlen(localDescription) + 1);
         if (state.description) strcpy(state.description, localDescription);
         if (state.description) collapseDescriptionBlankLines(state.description);
      }
      CaptionTrack localTracks[3];
      int localTrackCount = getDownloadedSubtitleTracks(requestedLocalPath, localTracks, 3);
      if (localTrackCount > 0) {
         state.captionTracks = (CaptionTrack *)malloc((size_t)localTrackCount * sizeof *state.captionTracks);
         if (state.captionTracks) {
            memcpy(state.captionTracks, localTracks, (size_t)localTrackCount * sizeof *state.captionTracks);
            state.captionCount = localTrackCount;
            appendAutoTranslatedCaptionTrack(getAutoTranslateLanguage(), 1);
            state.subtitlePick = subtitlesEnabled ?
               findPreferredSubtitleTrack(getPlaybackSubtitleLanguage()) : -1;
            if (state.subtitlePick < 0 && subtitlesEnabled) {
               for (int i = 0; i < state.captionCount; i++)
                  if (!subtitleIsAutoTranslated(&state.captionTracks[i])) {
                     state.subtitlePick = i;
                     break;
                  }
            }
            lastSubtitlePick = state.subtitlePick;
         }
      }
      parseChapters(state.description ? state.description : "", &state.chapters);
      { ChapterList downloadedChapters; if (loadDownloadedChapters(requestedLocalPath, &downloadedChapters) > 0) state.chapters = downloadedChapters; }
      if (!state.likeCount[0]) strCopy(state.likeCount, sizeof state.likeCount, LOC(STR_OFFLINE));
      if (!state.viewCount[0]) strCopy(state.viewCount, sizeof state.viewCount, LOC(STR_OFFLINE));
      if (!state.publishedDate[0]) strCopy(state.publishedDate, sizeof state.publishedDate, LOC(STR_OFFLINE));
      state.isLive = 0;
      state.videoCount = 0;
      state.resolutionIndex = -1;
      state.autoResolutionIndex = -1;
      state.pendingPlayer = createVideoPlayerSplitCancelable(requestedLocalPath, NULL,
                               allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);
      if (!state.pendingPlayer || !waitForInitialPlayerReady(state.pendingPlayer, 10000)) {
         if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
         if (!playbackCancelRequested()) fail(LOC(STR_COULD_NOT_PLAY_DOWNLOADED_FILE));
         goto done;
      }
      char downloadedId[12];
      if (getRequestedDownloadedId(downloadedId)) {
         int resumeAt = getDownloadedPosition(downloadedId);
         float duration = getVideoDurationSeconds(state.pendingPlayer);
         if (resumeAt > 3 && (duration <= 0 || resumeAt < duration - 10))
            seekVideoPlayer(state.pendingPlayer, (float)resumeAt);
      }
      state.vidItag = state.audItag = state.vidW = state.vidH = state.vidFps = 0;
      goto done;
   }
   info = calloc(1, sizeof *info);   // large format/URL table; keep it off the stack
   const Extractor *extractor = findExtractor(requestedInput);
   if (playbackCancelRequested()) goto done;
   if (!info || !extractor) { fail(LOC(STR_UNRECOGNIZED_LINK_ID)); goto done; }

   // resolve (stage stays STAGE_LOADING, set by initPlay's memset, until we adopt or fail)
#if BUILD_DIAG_LIVE_RECOVERY
   if (liveRecoveryWorker) recoveryResolveStartUs = sys_time_get_system_time();
#endif
   int extractResult = extractor->extract(requestedInput, info, getAutoTranslateLanguage());
#if BUILD_DIAG_LIVE_RECOVERY
   if (liveRecoveryWorker)
      logInfo("[yt-live-renew-diag] resolve rc=%d elapsedMs=%llu formats=%d live=%d postLive=%d segmentSec=%d windowSec=%d\n",
              extractResult,
              (unsigned long long)((sys_time_get_system_time()-recoveryResolveStartUs)/1000ull),
              info->formatCount, info->isLive, info->isPostLive,
              info->liveSegmentSec, info->liveWindowSec);
#endif
   if (playbackCancelRequested()) goto done;
   if (extractResult != 0 || info->formatCount == 0) {
      fail(extractResult == -2 ? LOC(STR_CHANNEL_MEMBERS_ONLY_VIDEO) : LOC(STR_RESOLVE_FAILED));
      goto done;
   }
   strCopy(state.title, sizeof state.title, info->title);
   strCopy(state.author, sizeof state.author, info->author);
   if (info->description) {
      size_t descriptionLen = strlen(info->description);
      state.description = (char *)malloc(descriptionLen + 1);
      if (state.description) memcpy(state.description, info->description, descriptionLen + 1);
      if (state.description) collapseDescriptionBlankLines(state.description);
      else logError("[yt-description] unable to allocate %u bytes for playback description\n",
                    (unsigned)(descriptionLen + 1));
   }
   strCopy(state.viewCount, sizeof state.viewCount, info->viewCount);
   strCopy(state.publishedDate, sizeof state.publishedDate, info->publishedDate);
   parseChapters(state.description ? state.description : "", &state.chapters);
   if (info->captionCount > 0) {
      state.captionTracks = malloc((size_t)info->captionCount * sizeof *state.captionTracks);
      if (state.captionTracks) {
         for (int i = 0; i < info->captionCount; i++)
            state.captionTracks[state.captionCount++] = info->captions[i];
      } else {
         logError("[subtitles] unable to allocate playback caption list\n");
      }
   }
   appendAutoTranslatedCaptionTrack(getAutoTranslateLanguage(), 0);

   const StreamFormat *video = pickBestPlaybackVideo(info);
   if (!video) { fail(LOC(STR_NO_PLAYABLE_MP4_VIDEO)); goto done; }
   int isLivePlayback = info->isLive || info->isPostLive || video->isLiveSegmented;
   state.liveAndroidClient = isLivePlayback && info->liveAndroidClient;
   state.liveAndroidFallbackSession = state.liveAndroidClient;
   const StreamFormat *availableAudio = pickBestAudio(info);
   const StreamFormat *qualityFormats[MAX_PLAYBACK_QUALITIES] = {0};
   state.videoCount = 0;
   state.autoResolutionIndex = -1;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *candidate = &info->formats[i];
      if (!isPlaybackVideoFormatDecodable(candidate)) continue;
      if (!candidate->hasAudio && !availableAudio && video->hasAudio) continue;
      addPlaybackQuality(candidate, qualityFormats, availableAudio != NULL);
   }
   if (state.videoCount == 0) { fail(LOC(STR_NO_PLAYABLE_MP4_VIDEO)); goto done; }
   // Keep the selector's exact default rendition as the Auto choice when it shares a
   // resolution/FPS pair with another duplicate format.
   for (int i = 0; i < state.videoCount; i++) {
      if (state.videoHeights[i] == video->height && state.videoFps[i] == video->fps) {
         qualityFormats[i] = video;
         strCopy(state.videoUrls[i], sizeof state.videoUrls[i], video->url);
         state.videoItags[i] = video->itag;
         state.videoWidths[i] = video->width;
         state.videoHasAudio[i] = video->hasAudio;
         break;
      }
   }
   state.autoResolutionIndex = state.videoCount - 1;
   int selectedQuality = (isLivePlayback || preferredQualityHeight <= 0) ?
                         state.videoCount - 1 : findPreferredQuality();
   if (selectedQuality < 0) { fail(LOC(STR_NO_PLAYABLE_MP4_VIDEO)); goto done; }
   video = qualityFormats[selectedQuality];
   state.resolutionIndex = selectedQuality;
   // A muxed rendition carries its own audio. Video-only choices use the original/default AAC track.
   const StreamFormat *audio = video->hasAudio ? NULL : availableAudio;
#if BUILD_DIAG_LIVE_RECOVERY
   if (liveRecoveryWorker) {
      logInfo("[yt-live-renew-diag] formats oldVideo=%d oldAudio=%d newVideo=%d newAudio=%d muxed=%d separateAudio=%d videoLive=%d videoSq=%ld..%ld videoBaseHash=%08x videoExpire=%lld audioLive=%d audioSq=%ld..%ld audioBaseHash=%08x audioExpire=%lld\n",
              state.vidItag, state.audItag, video->itag,
              audio ? audio->itag : (video->hasAudio ? video->itag : 0),
              video->hasAudio, audio != NULL, video->isLiveSegmented,
              video->liveStartSq, video->liveEdgeSq, liveUrlFingerprint(video->url),
              liveUrlExpiry(video->url), audio ? audio->isLiveSegmented : 0,
              audio ? audio->liveStartSq : 0, audio ? audio->liveEdgeSq : 0,
              liveUrlFingerprint(audio ? audio->url : NULL),
              liveUrlExpiry(audio ? audio->url : NULL));
   }
#endif
   if (!audio && video->hasAudio) {
      const StreamFormat *fallbackAudio = pickBestAudio(info);
      if (fallbackAudio) {
         strCopy(state.fallbackAudioUrl, sizeof state.fallbackAudioUrl, fallbackAudio->url);
         state.fallbackAudioItag = fallbackAudio->itag;
      }
   }
   state.audioPick = -1;
   state.audioCount = 0;
   if (!video->hasAudio) {
      // Put the uploaded/original AAC rendition first so it cannot be crowded out by
      // the language-ordered auto-dub formats in YouTube's response.
      {
         const StreamFormat *original = pickAudioForTrack(info, "__original_upload__");
         if (original) {
            int n = state.audioCount++;
            strCopy(state.audioIds[n], sizeof state.audioIds[n], "__original_upload__");
            strCopy(state.audioNames[n], sizeof state.audioNames[n], LOC(STR_ORIGINAL_UPLOAD_PLAYBACK));
            strCopy(state.audioUrls[n], sizeof state.audioUrls[n], original->url);
            state.audioItags[n] = original->itag;
            logInfo("[audio-diag] preserved original AAC itag=%d urlLen=%d\n",
                    original->itag, (int)strlen(original->url));
         }
      }
      // Add explicitly original/non-dubbed language tracks before auto-dubs, regardless
      // of YouTube's format ordering. Bitrate/codec variants remain internal.
      for (int pass = 0; pass < 2 && state.audioCount < MAX_AUDIO_TRACKS; pass++) {
         for (int i = 0; i < info->formatCount && state.audioCount < MAX_AUDIO_TRACKS; i++) {
            const StreamFormat *f = &info->formats[i];
            if (!f->hasAudio || f->hasVideo || !f->audioTrackId[0]) continue;
            if ((f->audioIsAutoDubbed ? 1 : 0) != pass) continue;
            int found = 0;
            for (int j = 0; j < state.audioCount; j++)
               if (strcmp(state.audioIds[j], f->audioTrackId) == 0) found = 1;
            if (found) continue;
            const StreamFormat *best = pickAudioForTrack(info, f->audioTrackId);
            if (!best) continue;
            int n = state.audioCount++;
            strCopy(state.audioIds[n], sizeof state.audioIds[n], best->audioTrackId);
            if (best->audioIsAutoDubbed)
               snprintf(state.audioNames[n], sizeof state.audioNames[n], "%s (auto-dubbed)", best->audioTrackName[0] ? best->audioTrackName : best->audioTrackId);
            else
               strCopy(state.audioNames[n], sizeof state.audioNames[n], best->audioTrackName[0] ? best->audioTrackName : best->audioTrackId);
            strCopy(state.audioUrls[n], sizeof state.audioUrls[n], best->url);
            state.audioItags[n] = best->itag;
         }
      }
      int defaultPick = -1;
      // 1) Prefer an explicitly identified upload-original track, when available.
      for (int j = 0; j < state.audioCount; j++) {
         if (strcmp(state.audioIds[j], "__original_upload__") == 0) {
            defaultPick = j;
            break;
         }
      }
      // 2) Respect YouTube's declared default track, provided it is not an auto-dub.
      if (defaultPick < 0 && info->defaultAudioTrackId[0]) {
         for (int j = 0; j < state.audioCount; j++) {
            if (strcmp(state.audioIds[j], info->defaultAudioTrackId) != 0) continue;
            const StreamFormat *candidate = pickAudioForTrack(info, state.audioIds[j]);
            if (candidate && !candidate->audioIsAutoDubbed) defaultPick = j;
            break;
         }
      }
      // 3) Fall back to an adaptive format explicitly marked as the default.
      if (defaultPick < 0)
         for (int i = 0; i < info->formatCount && defaultPick < 0; i++) {
            const StreamFormat *f = &info->formats[i];
            if (!f->hasAudio || f->audioIsAutoDubbed || !f->audioIsDefault || !f->audioTrackId[0]) continue;
            for (int j = 0; j < state.audioCount; j++)
               if (strcmp(state.audioIds[j], f->audioTrackId) == 0) { defaultPick = j; break; }
         }
      // 4) Last resort: choose the first available non-auto-dubbed track.
      if (defaultPick < 0) {
         for (int j = 0; j < state.audioCount; j++) {
            const StreamFormat *candidate = pickAudioForTrack(info, state.audioIds[j]);
            if (candidate && !candidate->audioIsAutoDubbed) {
               defaultPick = j;
               break;
            }
         }
      }
      logInfo("[audio-diag] video defaultAudioTrackId='%s' defaultPick=%d tracks=%d\n",
              info->defaultAudioTrackId, defaultPick, state.audioCount);
      for (int i = 0; i < state.audioCount; i++) {
         const StreamFormat *trackFormat = pickAudioForTrack(info, state.audioIds[i]);
         logInfo("[audio-diag] track[%d] id='%s' name='%s' itag=%d default=%d autoDubbed=%d url=%d\n",
                 i, state.audioIds[i], state.audioNames[i], state.audioItags[i],
                 trackFormat ? trackFormat->audioIsDefault : 0,
                 trackFormat ? trackFormat->audioIsAutoDubbed : 0,
                 state.audioUrls[i][0] != 0);
      }
      if (state.audioCount > 0) {
         state.audioPick = defaultPick;
         if (defaultPick >= 0) {
            audio = pickAudioForTrack(info, state.audioIds[defaultPick]);
            logInfo("[audio-diag] selected default index=%d id='%s' name='%s' itag=%d\n",
                    defaultPick, state.audioIds[defaultPick], state.audioNames[defaultPick], audio ? audio->itag : 0);
         } else {
            audio = NULL;
            logWarn("[audio-diag] declared default audio track is not available as a selectable stream; starting without an arbitrary dubbed track\n");
         }
      } else {
         logWarn("[audio-diag] no selectable AAC audio tracks parsed\n");
      }
   }
   logInfo("[yt] play video itag %d %dx%d %s, audio itag %d\n", video->itag, video->width, video->height,
           video->hasAudio ? "muxed" : "video-only", audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   state.vidItag = video->itag; state.vidW = video->width; state.vidH = video->height; state.vidFps = video->fps;
   state.audItag = audio ? audio->itag : (video->hasAudio ? video->itag : 0);
   state.isLive = isLivePlayback;
   state.liveEnded = info->isPostLive;
   if (video->isLiveSegmented) {
      state.liveDvr = info->liveDvr || state.liveEnded;
      state.liveSegmentSec = info->liveSegmentSec > 0 ? info->liveSegmentSec : LIVE_SEGMENT_SEC_DEFAULT;
      int windowSec = info->liveWindowSec > 0 ? info->liveWindowSec : LIVE_WINDOW_SEC_DEFAULT;
      state.liveWindowSegs = windowSec / state.liveSegmentSec;
      if (state.liveWindowSegs < 1) state.liveWindowSegs = 1;
      state.liveFloorSq = video->liveStartSq > 0 ? video->liveStartSq : 0;
      state.liveEdgeSq = video->liveEdgeSq;
      state.liveEdgeUs = sys_time_get_system_time();
      state.liveFirstSq = state.liveEnded ? state.liveFloorSq : state.liveEdgeSq - LIVE_START_BEHIND;
      if (state.liveFirstSq < state.liveFloorSq) state.liveFirstSq = state.liveFloorSq;
      state.liveLatencySec = state.liveEnded ? 0.0f : (float)((LIVE_START_BEHIND + 1) * state.liveSegmentSec);
      logInfo("[yt] LIVE rewind=%s ended=%d sq=%ld..%ld segment=%ds window=%d segments\n",
              state.liveDvr ? "on" : "off", state.liveEnded, state.liveFloorSq, state.liveEdgeSq,
              state.liveSegmentSec, state.liveWindowSegs);
   }

   // Use the same decoded pre-roll validation and ordered quality recovery for
   // segmented LIVE and VOD. A LIVE rendition that cannot initialize must not be
   // accepted as a successful player merely because its player object was created.
   if (playbackCancelRequested()) goto done;
#if BUILD_DIAG_LIVE_RECOVERY
   uint64_t liveOpenStartUs = liveRecoveryWorker ? sys_time_get_system_time() : 0;
#endif
   state.pendingPlayer = openInitialWithFallback(info, &video, &audio, 1);
#if BUILD_DIAG_LIVE_RECOVERY
   if (liveRecoveryWorker)
      logInfo("[yt-live-renew-diag] player-open success=%d elapsedMs=%llu selectedVideo=%d selectedAudio=%d separateAudio=%d liveEdge=%ld firstSq=%ld\n",
              state.pendingPlayer != NULL,
              (unsigned long long)((sys_time_get_system_time()-liveOpenStartUs)/1000ull),
              video ? video->itag : 0, audio ? audio->itag : 0,
              audio != NULL, video ? video->liveEdgeSq : 0,
              video ? video->liveStartSq : 0);
#endif
   if (playbackCancelRequested()) {
      if (state.pendingPlayer) {
         destroyVideoPlayer(state.pendingPlayer);
         state.pendingPlayer = NULL;
      }
      goto done;
   }
   if (!state.pendingPlayer) { fail(LOC(STR_PRIMARY_FALLBACK_STREAMS_FAILED)); goto done; }

   // The initial pre-roll fallback may have selected a different format. Keep the live quality
   // index and the HUD aligned with the rendition that actually opened.
   state.resolutionIndex = -1;
   for (int i = 0; i < state.videoCount; i++) {
      if (strcmp(state.videoUrls[i], video->url) == 0) {
         state.resolutionIndex = i;
         break;
      }
   }
   if (state.resolutionIndex < 0 && isPlaybackVideoFormatDecodable(video) &&
       state.videoCount < MAX_PLAYBACK_QUALITIES) {
      addPlaybackQuality(video, qualityFormats, availableAudio != NULL);
      for (int i = 0; i < state.videoCount; i++)
         if (strcmp(state.videoUrls[i], video->url) == 0) { state.resolutionIndex = i; break; }
      state.autoResolutionIndex = state.videoCount - 1;
   }
   if (state.resolutionIndex < 0) state.resolutionIndex = selectedQuality;
   if (preferredQualityHeight == 0) state.autoResolutionIndex = state.resolutionIndex;
   state.vidItag = video->itag; state.vidW = video->width;
   state.vidH = video->height; state.vidFps = video->fps;
   state.audItag = audio ? audio->itag : (video->hasAudio ? video->itag : 0);
   state.isLive = isLivePlayback;
   state.liveBaseRefreshNextTryUs = state.liveAndroidClient
      ? sys_time_get_system_time() + liveBaseRefreshIntervalUs() : 0;
   state.subtitlePick = (subtitlesEnabled && !state.isLive) ?
                        findPreferredSubtitleTrack(getPlaybackSubtitleLanguage()) : -1;
   lastSubtitlePick = state.subtitlePick;

   // Resume by the canonical video ID, including when playback was started from a pasted YouTube URL.
   // The same ID is used by local history and by the saved position table.
   {
      char resumeId[12];
      int savedResumeAt = (!state.isLive && extractPlaybackId(requestedInput, resumeId))
                        ? getWatchedPosition(resumeId) : 0;
      float resumeAt = state.vodRefreshResumePending ? state.vodRefreshPosition : (float)savedResumeAt;
      float duration = getVideoDurationSeconds(state.pendingPlayer);
      if (resumeAt > 3.0f && (duration <= 0 || resumeAt < duration - 10.0f))
         seekVideoPlayer(state.pendingPlayer, resumeAt);
      if (state.vodRefreshResumePending) state.vodRefreshResumePending = 0;
   }

done:
#if BUILD_DIAG_LIVE_RECOVERY
   if (liveRecoveryWorker)
      logInfo("[yt-live-renew-diag] worker-done totalMs=%llu pendingPlayer=%d cancel=%d\n",
              (unsigned long long)((sys_time_get_system_time()-recoveryWorkerStartUs)/1000ull),
              state.pendingPlayer != NULL, playbackCancelRequested());
#endif
   freeStreamInfo(info);
   __sync_synchronize();
   state.workerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static void liveRefreshWorker(uint64_t delayMs)
{
#if BUILD_DIAG_LIVE_RECOVERY
   uint64_t refreshStartUs = sys_time_get_system_time();
#endif
   VideoPlayer *old = state.liveRetiringPlayer;
   state.liveRetiringPlayer = NULL;
   if (old) {
#if BUILD_DIAG_LIVE_RECOVERY
      uint64_t destroyStartUs = sys_time_get_system_time();
#endif
      destroyVideoPlayer(old);
#if BUILD_DIAG_LIVE_RECOVERY
      logInfo("[yt-live-renew-diag] old-player-destroyMs=%llu\n",
              (unsigned long long)((sys_time_get_system_time()-destroyStartUs)/1000ull));
#endif
   }

   // Short, cancellable backoff: leaving playback must not wait for the full delay.
   uint64_t remaining = delayMs;
   while (remaining > 0 && !playbackCancelRequested()) {
      unsigned step = remaining > 50 ? 50 : (unsigned)remaining;
      sleepMs(step);
      remaining -= step;
   }
   if (playbackCancelRequested()) {
      __sync_synchronize();
      state.workerDone = 1;
      exitThread();
   }
   // Reuse the complete resolver/open path. It fetches fresh YouTube playback data and
   // creates a new VideoPlayer, demuxers, segment sources, decoder and audio pipeline.
#if BUILD_DIAG_LIVE_RECOVERY
   logInfo("[yt-live-renew-diag] refresh-worker-start delayMs=%llu elapsedMs=%llu oldItags=%d/%d oldSeparateAudio=%d\n",
           (unsigned long long)delayMs,
           (unsigned long long)((sys_time_get_system_time()-refreshStartUs)/1000ull),
           state.vidItag, state.audItag,
           state.player ? isVideoPlayerUsingSeparateAudio(state.player) : 0);
#endif
   worker(0);
}

static void vodRefreshWorker(uint64_t delayMs)
{
   VideoPlayer *old = state.vodRetiringPlayer;
   state.vodRetiringPlayer = NULL;
   if (old) destroyVideoPlayer(old);

   uint64_t remaining = delayMs;
   while (remaining > 0 && !playbackCancelRequested()) {
      unsigned step = remaining > 50 ? 50 : (unsigned)remaining;
      sleepMs(step);
      remaining -= step;
   }
   if (playbackCancelRequested()) {
      __sync_synchronize();
      state.workerDone = 1;
      exitThread();
   }

   // Re-resolve the VOD and build a completely new player/session. The resume point
   // was captured before teardown and is applied by worker() after the new player opens.
   worker(0);
}

static void showVodRefreshFailure(void)
{
   if (state.player) {
      finishGfx();
      destroyVideoPlayer(state.player);
      state.player = NULL;
   }
   if (state.pendingPlayer) {
      finishGfx();
      destroyVideoPlayer(state.pendingPlayer);
      state.pendingPlayer = NULL;
   }
   if (state.vodRetiringPlayer) {
      finishGfx();
      destroyVideoPlayer(state.vodRetiringPlayer);
      state.vodRetiringPlayer = NULL;
   }
   state.vodRefreshActive = 0;
   state.liveFailureVisible = 1;
   state.stage = STAGE_FAILED;
   strCopy(state.message, sizeof state.message, LOC(STR_UNABLE_TO_RENEW_VIDEO));
   setLabelText(&liveFailureTitleLabel, LOC(STR_PLAYBACK_FAILED));
   setLabelText(&liveFailureBodyLabel, LOC(STR_VIDEO_RENEW_FAILED));
   logError("[yt-vod-refresh] exhausted 5 recovery attempts; playback stopped\n");
}

static void scheduleVodHttpRefresh(void)
{
   static const unsigned delaysMs[5] = { 500, 1000, 2000, 3000, 5000 };
   if (state.isLive || state.liveFailureVisible || state.vodRefreshActive || state.threadActive ||
       state.resolutionSwitchThreadActive || state.audioSwitchThreadActive ||
       state.liveReopenThreadActive || state.subThreadActive) return;

   if (state.vodRefreshAttempts >= 5) {
      showVodRefreshFailure();
      return;
   }

   if (state.vodRefreshAttempts == 0) {
      if (!state.player) return;
      state.vodRefreshPosition = getVideoPositionSeconds(state.player);
      if (state.vodRefreshPosition < 0.0f) state.vodRefreshPosition = 0.0f;
      state.vodRefreshResumePending = 1;
      state.pauseWhenReady = isVideoPaused(state.player);
   }

   unsigned delayMs = delaysMs[state.vodRefreshAttempts++];
   logWarn("[yt-vod-refresh] recovery attempt %d/5 in %u ms at %.2fs\n",
           state.vodRefreshAttempts, delayMs, state.vodRefreshPosition);
   strCopy(state.message, sizeof state.message, LOC(STR_REFRESHING_VIDEO_PLAYBACK));
   setLabelText(&statusLabel, state.message);
   state.stage = STAGE_LOADING;
   state.firstFrameSeen = 0;
   state.seeking = 0;
   state.controlsDismissed = 1;
   state.lastFrame = NULL;
   state.vodRefreshStableSinceUs = 0;
   state.vodRetiringPlayer = state.player;
   state.player = NULL;
   finishGfx();

   free(state.description);
   state.description = NULL;
   state.descriptionRendered = 0;
   clearDescriptionPages();
   free(state.subtitleTrack);
   state.subtitleTrack = NULL;
   free(state.captionTracks);
   state.captionTracks = NULL;
   state.captionCount = 0;
   state.subtitleFetched = -1;
   state.subtitlePick = -1;
   lastSubtitlePick = -1;
   state.subtitlesReady = 0;
   state.subtitleScanFrom = -1;
   state.subtitleDisplayedCue = -1;
   state.subtitleDisplayedPending = 0;
   state.subtitleLastPosition = 0.0f;
   state.workerDone = 0;
   state.vodRefreshActive = 1;
   state.threadActive = (spawnJoinableThread(&state.workerTid, vodRefreshWorker, delayMs,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-vod-refresh") == 0);
   if (!state.threadActive) {
      state.vodRefreshActive = 0;
      showVodRefreshFailure();
   }
}

static void showLiveRefreshFailure(void)
{
   if (state.player) {
      finishGfx();
      destroyVideoPlayer(state.player);
      state.player = NULL;
   }
   if (state.pendingPlayer) {
      finishGfx();
      destroyVideoPlayer(state.pendingPlayer);
      state.pendingPlayer = NULL;
   }
   if (state.liveRetiringPlayer) {
      finishGfx();
      destroyVideoPlayer(state.liveRetiringPlayer);
      state.liveRetiringPlayer = NULL;
   }
   state.liveRefreshActive = 0;
   state.liveFailureVisible = 1;
   state.stage = STAGE_FAILED;
   strCopy(state.message, sizeof state.message, LOC(STR_UNABLE_TO_RECONNECT_LIVE));
   setLabelText(&liveFailureTitleLabel, LOC(STR_LIVE_PLAYBACK_FAILED));
   setLabelText(&liveFailureBodyLabel, LOC(STR_LIVE_RENEW_FAILED));
   logError("[yt-live-refresh] exhausted 5 renewal attempts; playback stopped\n");
}

static void scheduleLiveRefresh(void)
{
   static const unsigned delaysMs[5] = { 500, 1000, 2000, 3000, 5000 };
   if (state.liveAndroidClient && state.player) {
      if (state.liveBaseRefreshActive) return;
      if (getVideoPlayerLiveRenewalNeeded(state.player)) {
         scheduleLiveBaseRefresh(1);
         return;
      }
   }
   if (!state.isLive || state.liveFailureVisible || state.liveRefreshActive || state.liveBaseRefreshActive || state.threadActive ||
       state.resolutionSwitchThreadActive || state.audioSwitchThreadActive ||
       state.liveReopenThreadActive || state.subThreadActive) return;
   if (state.liveRefreshAttempts >= 5) {
      showLiveRefreshFailure();
      return;
   }

   unsigned delayMs = delaysMs[state.liveRefreshAttempts];
   state.liveRefreshAttempts++;
   logWarn("[yt-live-refresh] renewal attempt %d/5 in %u ms\n", state.liveRefreshAttempts, delayMs);
#if BUILD_DIAG_LIVE_RECOVERY
   logInfo("[yt-live-renew-diag] schedule attempt=%d delayMs=%u live=%d ended=%d dvr=%d itags=%d/%d separateAudio=%d segmentSec=%d windowSegs=%d edgeSq=%ld floorSq=%ld\n",
           state.liveRefreshAttempts, delayMs, state.isLive, state.liveEnded, state.liveDvr,
           state.vidItag, state.audItag,
           state.player ? isVideoPlayerUsingSeparateAudio(state.player) : 0,
           state.liveSegmentSec, state.liveWindowSegs, state.liveEdgeSq, state.liveFloorSq);
#endif
   strCopy(state.message, sizeof state.message, LOC(STR_RECONNECTING_LIVE));
   state.stage = STAGE_LOADING;
   state.firstFrameSeen = 0;
   state.liveAnchored = 0;
   state.liveReturnNoticePending = 1;
   state.liveReturnNoticeUs = 0;
   state.liveRefreshStableSinceUs = 0;
   state.seeking = 0;
   state.controlsDismissed = 1;
   state.lastFrame = NULL;
   state.liveLastFrameUs = 0;
   state.liveRetiringPlayer = state.player;
   state.player = NULL;
   finishGfx();

   // The old player is retired before resolving. Remove session-owned subtitle data so the
   // replacement worker can build a clean set from the new extractor response.
   free(state.description);
   state.description = NULL;
   state.descriptionRendered = 0;
   clearDescriptionPages();
   free(state.subtitleTrack);
   state.subtitleTrack = NULL;
   free(state.captionTracks);
   state.captionTracks = NULL;
   state.captionCount = 0;
   state.subtitleFetched = -1;
   state.subtitlePick = -1;
   lastSubtitlePick = -1;
   state.subtitlesReady = 0;
   state.subtitleScanFrom = -1;
   state.subtitleDisplayedCue = -1;
   state.subtitleDisplayedPending = 0;
   state.subtitleLastPosition = 0.0f;
   state.workerDone = 0;
   state.liveRefreshActive = 1;
   state.threadActive = (spawnJoinableThread(&state.workerTid, liveRefreshWorker, delayMs,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-live-refresh") == 0);
   if (!state.threadActive) {
      state.liveRefreshActive = 0;
      showLiveRefreshFailure();
   }
}

// a bare 11-char videoId (not a typed url) is what SponsorBlock and history key on; typed urls no-op.
static int isBareVideoId(const char *input)
{
   return input[0] && !strchr(input, '/') && !strchr(input, ':');
}

// fetch skip segments in parallel with resolve/open, so they cost no startup latency.
static void sponsorWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   fetchSponsorSegments(requestedInput, &state.sb);
   __sync_synchronize();
   state.sbWorkerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

// asks /next for youtube's own chapter list; spawned only when the description parse found none.
static void chapterWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   fetchChapters(requestedInput, &state.fetchedChapters);
   __sync_synchronize();
   state.chWorkerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

// WEB metadata is deferred until SELECT opens the panel; it never delays playback startup.
static void metadataWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   char videoId[12] = "";
   char views[32] = "";
   char published[24] = "";
   char likes[32] = "";
   const Extractor *extractor = findExtractor(requestedInput);

   if (extractPlaybackId(requestedInput, videoId) && extractor && extractor->getVideoMetadata &&
       extractor->getVideoMetadata(videoId, views, sizeof views, published, sizeof published,
                                   likes, sizeof likes) == 0) {
      if (views[0]) strCopy(state.viewCount, sizeof state.viewCount, views);
      if (published[0]) strCopy(state.publishedDate, sizeof state.publishedDate, published);
      if (likes[0]) {
         // WEB microformat returns an integer; keep the compact display used by the old /next UI.
         char *end = NULL;
         long count = strtol(likes, &end, 10);
         if (end != likes && count >= 1000) {
            if (count >= 1000000000L)
               snprintf(state.likeCount, sizeof state.likeCount, "%ldb", count / 1000000000L);
            else if (count >= 1000000L)
               snprintf(state.likeCount, sizeof state.likeCount, "%ldm", count / 1000000L);
            else
               snprintf(state.likeCount, sizeof state.likeCount, "%ldk", count / 1000L);
         } else {
            strCopy(state.likeCount, sizeof state.likeCount, likes);
         }
      } else {
         strCopy(state.likeCount, sizeof state.likeCount, LOC(STR_UNAVAILABLE));
      }
   } else {
      strCopy(state.likeCount, sizeof state.likeCount, LOC(STR_UNAVAILABLE));
   }
   __sync_synchronize();
   state.metadataWorkerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

// Resolve YouTube's next recommendation off the UI thread; playback is untouched if this fails.
static void autoplayWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   if (getNextVideoId(requestedInput, state.nextVideoId, sizeof state.nextVideoId,
                      state.nextVideoTitle, sizeof state.nextVideoTitle,
                      autoplayWatchedIds, autoplayWatchedCount) == 0) {
      uint8_t *jpeg = (uint8_t *)malloc(AUTOPLAY_THUMB_CAP);
      if (jpeg) {
         char url[128];
         snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", state.nextVideoId);
         HttpHeader headers[] = {
            { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/125.0.0.0 Safari/537.36" },
            { "Accept-Encoding", "identity" }
         };
         int length = 0, status = 0;
         int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)jpeg,
                            AUTOPLAY_THUMB_CAP, &length, &status);
         if (rc == 0 && status == 200 && length > 0) {
            state.nextThumbJpeg = jpeg;
            state.nextThumbJpegLen = length;
         } else {
            logWarn("[yt] autoplay thumbnail failed rc=%d status=%d\n", rc, status);
            free(jpeg);
         }
      }
   }
   __sync_synchronize();
   state.nextWorkerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static void launchVideo(const char *input, int addToNavigation);

// A stalled LIVE can mean either a temporary transport failure or a broadcast that ended.
// Ask YouTube's player endpoint before entering the normal LIVE renewal loop.
static void liveEndCheckWorker(uint64_t arg)
{
   (void)arg;
   char videoId[12] = "";
   const Extractor *extractor = findExtractor(requestedInput);
   state.liveEndCheckResult = -1;
   setHttpRequestGroup(&state.requestGroup);
   if (!playbackCancelRequested() && extractor && extractor->getLiveEndedStatus &&
       extractPlaybackId(requestedInput, videoId))
      state.liveEndCheckResult = extractor->getLiveEndedStatus(videoId);
   __sync_synchronize();
   state.liveEndCheckDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static void scheduleLiveEndCheck(void)
{
   if (!state.isLive || state.liveEnded || state.liveEndDetected || state.liveFailureVisible ||
       state.liveEndCheckActive || state.liveRefreshActive || state.liveBaseRefreshActive ||
       state.threadActive || state.resolutionSwitchThreadActive || state.audioSwitchThreadActive ||
       state.liveReopenThreadActive || state.subThreadActive) return;

   state.liveEndCheckDone = 0;
   state.liveEndCheckResult = -1;
   state.liveEndCheckActive = (spawnJoinableThread(&state.liveEndCheckTid, liveEndCheckWorker, 0,
                               THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB,
                               "yt-live-end-check") == 0);
   if (!state.liveEndCheckActive) {
      logWarn("[yt-live-end] could not start status check; falling back to LIVE renewal\n");
      scheduleLiveRefresh();
      return;
   }
   logInfo("[yt-live-end] checking YouTube status after stalled LIVE transport\n");
}

void setPlaylistShuffleEnabled(int enabled)
{
   playlistShuffleEnabled = enabled != 0;
}

void setPlaylistShuffleContext(int enabled)
{
   playlistShuffleContext = enabled != 0;
}

int isPlaylistShuffleEnabled(void)
{
   return playlistShuffleEnabled;
}

static uint32_t nextPlaylistShuffleRandom(void)
{
   if (!playlistShuffleRandomState) {
      uint64_t seed = getTimeUs();
      playlistShuffleRandomState = (uint32_t)(seed ^ (seed >> 32) ^ 0x9E3779B9u);
      if (!playlistShuffleRandomState) playlistShuffleRandomState = 0xA341316Cu;
   }
   uint32_t x = playlistShuffleRandomState;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   playlistShuffleRandomState = x;
   return x;
}

static void shufflePlaylistQueueFromCurrent(void)
{
   if (!playlistShuffleEnabled || !playlistShuffleContext || playlistQueueCount < 2 || playlistQueueCursor < 0)
      return;

   char ordered[DOWNLOAD_QUEUE_CAP][16];
   int count = 0;
   strCopy(ordered[count++], sizeof ordered[0], playlistQueue[playlistQueueCursor]);
   for (int i = 0; i < playlistQueueCount; i++) {
      if (i == playlistQueueCursor) continue;
      strCopy(ordered[count++], sizeof ordered[0], playlistQueue[i]);
   }

   // Keep the selected video first; randomize every remaining item without replacement.
   for (int i = count - 1; i > 1; i--) {
      int j = 1 + (int)(nextPlaylistShuffleRandom() % (uint32_t)i);
      char temp[16];
      strCopy(temp, sizeof temp, ordered[i]);
      strCopy(ordered[i], sizeof ordered[i], ordered[j]);
      strCopy(ordered[j], sizeof ordered[j], temp);
   }
   for (int i = 0; i < count; i++)
      strCopy(playlistQueue[i], sizeof playlistQueue[0], ordered[i]);
   playlistQueueCount = count;
   playlistQueueCursor = 0;
}

static int launchPlaylistQueueIndex(int index)
{
   if (index < 0 || index >= playlistQueueCount) return 0;
   playlistQueueCursor = index;
   char videoId[16];
   strCopy(videoId, sizeof videoId, playlistQueue[index]);
   requestPlaybackCancel();
   popScreen();
   launchVideo(videoId, 0);
   return 1;
}

static int launchDownloadedQueueIndex(int index)
{
   if (index < 0 || index >= downloadedQueueCount) return 0;
   char path[MAX_PATH_LEN];
   if (getDownloadedVideoPath(downloadedQueue[index], path, sizeof path) != 0) {
      logInfo("[play] queued download unavailable: %s\n", downloadedQueue[index]);
      return 0;
   }
   requestPlaybackCancel();
   popScreen();
   downloadedQueueCursor = index;
   requestedIsLocal = 1;
   strCopy(requestedLocalPath, sizeof requestedLocalPath, path);
   requestedInput[0] = 0;
   pushScreen(&playScreen);
   return 1;
}

static void launchVideo(const char *input, int addToNavigation)
{
   requestedIsLocal = 0;
   requestedLocalPath[0] = 0;
   strCopy(requestedInput, sizeof requestedInput, input);
   if (addToNavigation) appendPlaybackNavigation(input);
   pushScreen(&playScreen);
}

void playVideo(const char *input)
{
   playbackOriginVideoId[0] = 0;
   extractPlaybackId(input, playbackOriginVideoId);
   playlistQueueCount = 0;
   playlistQueueCursor = -1;
   launchVideo(input, 1);
}

void playVideoWithQueue(const char *input, const char videoIds[][16], int count, int index)
{
   playbackOriginVideoId[0] = 0;
   extractPlaybackId(input, playbackOriginVideoId);
   if (!input || !input[0] || !videoIds || count <= 0 || index < 0 || index >= count) {
      playVideo(input);
      return;
   }
   if (count > DOWNLOAD_QUEUE_CAP) count = DOWNLOAD_QUEUE_CAP;
   playlistQueueCount = 0;
   playlistQueueCursor = -1;
   for (int i = 0; i < count; i++) {
      if (!videoIds[i][0]) continue;
      strCopy(playlistQueue[playlistQueueCount], sizeof playlistQueue[0], videoIds[i]);
      if (i == index) playlistQueueCursor = playlistQueueCount;
      playlistQueueCount++;
   }
   if (playlistQueueCount == 0 || playlistQueueCursor < 0) {
      playlistQueueCount = 0;
      playlistQueueCursor = -1;
   } else {
      shufflePlaylistQueueFromCurrent();
   }
   launchVideo(input, 1);
}

void playLocalVideo(const char *path)
{
   if (!path || !path[0]) return;
   downloadedQueueCount = 0;
   downloadedQueueCursor = -1;
   requestedIsLocal = 1;
   strCopy(requestedLocalPath, sizeof requestedLocalPath, path);
   requestedInput[0] = 0;
   pushScreen(&playScreen);
}

void playLocalVideoWithQueue(const char *path, const char videoIds[][16], int count, int index)
{
   if (!path || !path[0] || !videoIds || count <= 0 || index < 0 || index >= count) return;
   if (count > DOWNLOAD_QUEUE_CAP) count = DOWNLOAD_QUEUE_CAP;
   downloadedQueueCount = 0;
   downloadedQueueCursor = -1;
   for (int i = 0; i < count; i++) {
      if (!videoIds[i][0]) continue;
      strCopy(downloadedQueue[downloadedQueueCount], sizeof downloadedQueue[0], videoIds[i]);
      if (i == index) downloadedQueueCursor = downloadedQueueCount;
      downloadedQueueCount++;
   }
   if (downloadedQueueCount == 0 || downloadedQueueCursor < 0) {
      playLocalVideo(path);
      return;
   }
   requestedIsLocal = 1;
   strCopy(requestedLocalPath, sizeof requestedLocalPath, path);
   requestedInput[0] = 0;
   pushScreen(&playScreen);
}

// Playback uses the scale selected in the main menu for text rasterisation only.
// drawPlay() disables the global SD transform so video geometry and HUD placement
// remain unchanged.
static float playbackFontScale = 1.0f;

static int playbackFontSize(int baseSize)
{
   return baseSize > 0 ? baseSize : 1;
}

static int playbackSubtitleFontSize(int baseSize)
{
   // At UI scale 1.5x, subtitles should be only 1.25x the default size,
   // not 1.25x on top of the general 1.5x playback font scaling.
   int size = playbackFontScale >= 1.5f ? (baseSize * 5 + 2) / 4 : baseSize;
   return size > 0 ? size : 1;
}

static int playbackChapterRowHeight(void)
{
   int scaled = (int)(playbackFontSize(CHAPTER_TEXT_SIZE) * playbackFontScale * 1.5f + 0.5f);
   return scaled > CHAPTER_ROW_HEIGHT ? scaled : CHAPTER_ROW_HEIGHT;
}

static void renderDescription(void);

static void applyPlaybackUiLabelScale(float scale)
{
   if (scale < 1.0f) scale = 1.0f;
   if (scale > 1.5f) scale = 1.5f;
   playbackFontScale = scale;

#define SCALE_PLAYBACK_LABEL(label) setLabelFixedRenderScale(&(label), scale, 0)
   SCALE_PLAYBACK_LABEL(descriptionPageLabel);
   SCALE_PLAYBACK_LABEL(statusLabel);
   for (int i = 0; i < STAT_LINES; i++) {
      statLabels[i].size = playbackSubtitleFontSize(22);
      statLabels[i].fixedRenderScale = 0.0f;
      setLabelFixedRenderScale(&statLabels[i], 1.0f, 0);
   }
   SCALE_PLAYBACK_LABEL(timeLeftLabel);
   SCALE_PLAYBACK_LABEL(timeRightLabel);
   SCALE_PLAYBACK_LABEL(liveBadgeLabel);
   SCALE_PLAYBACK_LABEL(backToLabel);
   SCALE_PLAYBACK_LABEL(liveFailureTitleLabel);
   SCALE_PLAYBACK_LABEL(liveFailureBodyLabel);
   SCALE_PLAYBACK_LABEL(toastLabel);
   SCALE_PLAYBACK_LABEL(askPromptPrefixLabel);
   SCALE_PLAYBACK_LABEL(askPromptSuffixLabel);
   SCALE_PLAYBACK_LABEL(titleLabel);
   SCALE_PLAYBACK_LABEL(authorLabel);
   SCALE_PLAYBACK_LABEL(nextNoticeHeadingLabel);
   SCALE_PLAYBACK_LABEL(nextNoticeTitleLabel);
   subtitleLabel.size = playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4);
   subtitleLabel.fixedRenderScale = 0.0f;
   setLabelFixedRenderScale(&subtitleLabel, 1.0f, 0);
   SCALE_PLAYBACK_LABEL(playbackInfoLabel);
   for (int i = 0; i < MAX_CHAPTERS; i++) {
      SCALE_PLAYBACK_LABEL(chapterLabels[i]);
      SCALE_PLAYBACK_LABEL(chapterTimeLabels[i]);
   }
   for (int i = 0; i < OPTIONS_COUNT; i++) SCALE_PLAYBACK_LABEL(optionsLabels[i]);
   for (int i = 0; i < OPTIONS_DROPDOWN_MAX; i++) SCALE_PLAYBACK_LABEL(optionsValueLabels[i]);
   SCALE_PLAYBACK_LABEL(optionsDownloadLabel);
   for (int i = 0; i < 3; i++) SCALE_PLAYBACK_LABEL(optionsCurrentValueLabels[i]);
   SCALE_PLAYBACK_LABEL(playbackSettingsTitle);
   SCALE_PLAYBACK_LABEL(playbackAutoTranslateTitle);
   for (int i = 0; i < PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS; i++)
      SCALE_PLAYBACK_LABEL(playbackAutoTranslateRows[i]);
   SCALE_PLAYBACK_LABEL(playbackSubtitleStyleTitle);
   for (int i = 0; i < PLAYBACK_SUBTITLE_STYLE_COUNT; i++) {
      // The label's raster font grows with playbackFontScale. Grow its wrapping
      // width by the same factor; otherwise long entries such as "Background
      // Opacity" are clipped at UI scale 1.5x by the old 300px fixed width.
      playbackSubtitleStyleLabels[i].width = (int)(300.0f * scale + 0.5f);
      SCALE_PLAYBACK_LABEL(playbackSubtitleStyleLabels[i]);
   }
   playbackSubtitlePreview.size = playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4);
   playbackSubtitlePreview.fixedRenderScale = 0.0f;
   setLabelFixedRenderScale(&playbackSubtitlePreview, 1.0f, 0);
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      SCALE_PLAYBACK_LABEL(playbackSettingsLabels[i]);
      SCALE_PLAYBACK_LABEL(playbackSettingsValues[i]);
   }
   for (int i = 0; i < 2; i++) SCALE_PLAYBACK_LABEL(playbackSettingsToggleLabels[i]);
   SCALE_PLAYBACK_LABEL(playbackSponsorTitle);
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++) SCALE_PLAYBACK_LABEL(playbackSponsorCategoryLabels[i]);
   for (int i = 0; i < 3; i++) SCALE_PLAYBACK_LABEL(playbackSponsorActionLabels[i]);
   setLabelFixedRenderScale(&volumeMeter.numberLabel, scale, 0);
#undef SCALE_PLAYBACK_LABEL

   if (state.descriptionRendered) renderDescription();
}

static void initPlay(void)
{
   loadingAnimationStartedUs = 0;
   memset(&state, 0, sizeof state);
   state.requestGroup.cancelled = 0;
   state.askPromptShown = -1;
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   playbackFontScale = getGfxSdUiScale();
   cropPanX = cropPanY = 0.0f;
   cropPanLastUpdateUs = sys_time_get_system_time();
   if (playbackFontScale < 1.0f) playbackFontScale = 1.0f;

   uiFont = openSystemFont(FONT_POP);
   initLabel(&descriptionPageLabel, &uiFont, 0, 0, 180, AUTO, playbackFontSize(DESCRIPTION_TEXT_SIZE), activeTheme->textSecondary, TEXT_NOWRAP, "1/1");
   initLabel(&statusLabel, &uiFont, 0, 0, state.screenW - 80, AUTO, playbackFontSize(28), activeTheme->textPrimary, TEXT_WRAP, "");
   statsTextPadding = (int)(measureFontText(&uiFont, playbackSubtitleFontSize(22), "   ") + 0.5f);
   if (statsTextPadding < 3) statsTextPadding = 3;
   int statsTextMaxWidth = state.screenW - 80 - 2 * statsTextPadding;
   for (int i = 0; i < STAT_LINES; i++)
      initLabel(&statLabels[i], &uiFont, 0, 0, statsTextMaxWidth, AUTO, playbackSubtitleFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");
   initLabel(&timeLeftLabel,  &uiFont, 0, 0, 200, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&timeRightLabel, &uiFont, 0, 0, 200, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabelRaw(&liveBadgeLabel, &uiFont, 0, 0, 120, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_LIVE_BADGE));
   liveBadgeLanguage = getAppLanguage();
   initLabelRaw(&backToLabel, &uiFont, 0, 0, 180, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_BACK_TO));
   initLabelRaw(&liveFailureTitleLabel, &uiFont, 0, 0, 760, AUTO, playbackFontSize(30), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_LIVE_PLAYBACK_FAILED));
   initLabelRaw(&liveFailureBodyLabel, &uiFont, 0, 0, 760, AUTO, playbackFontSize(22), activeTheme->textSecondary, TEXT_WRAP, LOC(STR_LIVE_RENEW_FAILED));
   initButtonHints(&liveFailureHints, &uiFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&liveFailureHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_RETURN));
   addButtonHint(&liveFailureHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_RETURN));
   setButtonHintsStableLayout(&liveFailureHints, 1);
   // Use two rows so playback controls remain readable at 1.5x UI scale.
   // drawButtonHintsFitted() only scales down when the available width requires it.
   initButtonHints(&playbackHints, &uiFont, 0, 30, 20, activeTheme->textSecondary);
   setButtonHintGap(&playbackHints, 8);
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_CROSS), LOC(STR_PLAY_PAUSE));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_DPAD_LEFT), NULL);
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), LOC(STR_SEEK));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_L1), NULL);
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_R1), LOC(STR_CHAPTER_VIDEO_HOLD));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_DPAD_UP), NULL);
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_DPAD_DOWN), LOC(STR_VOLUME));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_TRIANGLE), LOC(STR_SUBTITLES));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_L2), LOC(STR_OSD));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_R2), LOC(STR_CROP));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_R3), LOC(STR_STATS));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_CIRCLE), LOC(STR_HIDE_OSD));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_START), LOC(STR_OPTIONS));
   addButtonHint(&playbackHints, getConsoleGlyph(GLYPH_SELECT), LOC(STR_DESCRIPTION));
   initLabelRaw(&toastLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");   // raw: shows track names
   initLabelRaw(&askPromptPrefixLabel, &uiFont, 0, 0, 180, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_PRESS));
   initLabelRaw(&askPromptSuffixLabel, &uiFont, 0, 0, 260, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_TO_SKIP_SPONSOR));
   initLabelRaw(&titleLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");
   initLabelRaw(&authorLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(20), activeTheme->textSecondary, TEXT_WRAP, "");
   initLabelRaw(&nextNoticeHeadingLabel, &uiFont, 0, 0, 500, AUTO, playbackFontSize(22), activeTheme->accent, TEXT_NOWRAP, LOC(STR_NEXT_VIDEO));
   initLabelRaw(&nextNoticeTitleLabel, &uiFont, 0, 0, 1040, AUTO, playbackFontSize(30), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabelRaw(&subtitleLabel, &uiFont, 0, 0, state.screenW - 160, AUTO, playbackSubtitleFontSize(SUBTITLE_TEXT_SIZE), activeTheme->textPrimary, TEXT_WRAP, "");
   initLabelRaw(&playbackInfoLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");
   for (int i = 0; i < MAX_CHAPTERS; i++) {
      initLabelRaw(&chapterLabels[i], &uiFont, 0, 0, 990, AUTO, playbackFontSize(CHAPTER_TEXT_SIZE), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&chapterTimeLabels[i], &uiFont, 0, 0, 150, AUTO, playbackFontSize(CHAPTER_TEXT_SIZE), activeTheme->textSecondary, TEXT_NOWRAP, "");
   }
   optionsVisible = optionsDropdownOpen = optionsSelected = optionsDropdownSelected = 0;
   lastSubtitlePick = -1;
   suppressPlaybackCrossUntilRelease = 0;
   const char *localizedOptionsNames[OPTIONS_COUNT] = { LOC(STR_QUALITY), LOC(STR_AUDIO), LOC(STR_SUBTITLES), LOC(STR_SUBS_LANGUAGE), LOC(STR_CHAPTERS), LOC(STR_CROP_TO_FILL), LOC(STR_AUTOPLAY), LOC(STR_DOWNLOAD), LOC(STR_PLAYBACK_SETTINGS) };
   for (int i = 0; i < OPTIONS_COUNT; i++)
      initLabelRaw(&optionsLabels[i], &uiFont, 0, 0, state.screenW - 96, AUTO, playbackFontSize(27), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, localizedOptionsNames[i]);
   for (int i = 0; i < OPTIONS_DROPDOWN_MAX; i++)
      initLabelRaw(&optionsValueLabels[i], &uiFont, 0, 0, 360, AUTO, playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabelRaw(&optionsDownloadLabel, &uiFont, 0, 0, 150, AUTO, playbackFontSize(23), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_START));
   optionsDownloadPressedUntil = 0;
   initLabelRaw(&optionsCurrentValueLabels[0], &uiFont, 0, 0, 0, AUTO,
                playbackFontSize(27), activeTheme->textSecondary, TEXT_NOWRAP, "");
   initLabelRaw(&optionsCurrentValueLabels[1], &uiFont, 0, 0, 400, AUTO,
                playbackFontSize(27), activeTheme->textSecondary, TEXT_NOWRAP, "");
   initLabelRaw(&optionsCurrentValueLabels[2], &uiFont, 0, 0, 400, AUTO,
                playbackFontSize(27), activeTheme->textSecondary, TEXT_NOWRAP, "");

   playbackSettingsOpen = playbackSponsorOpen = playbackSubtitleStyleOpen = playbackAutoTranslateOpen = 0;
   playbackSettingsSelected = playbackSponsorSelected = playbackAutoTranslateSelected = 0;
   autoTranslateRefreshPending = 0;
   initLabelRaw(&playbackSettingsTitle, &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_PLAYBACK_SETTINGS));
   const char *playbackSettingNames[PLAYBACK_SETTINGS_COUNT] = {
      LOC(STR_ENABLE_1080P60), LOC(STR_UI_SCALING), LOC(STR_OVERSCAN), LOC(STR_OVERSCAN_CROP),
      LOC(STR_SUBTITLE_STYLE), LOC(STR_SUBTITLE_OFFSET), LOC(STR_AUTO_TRANSLATE_SUBTITLES), LOC(STR_SPONSORBLOCK), LOC(STR_OSD_TIMEOUT)
   };
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      initLabelRaw(&playbackSettingsLabels[i], &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 290, AUTO,
                   playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
                   playbackSettingNames[i]);
      initLabelRaw(&playbackSettingsValues[i], &uiFont, 0, 0, 210, AUTO,
                   playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, "");
   }
   initLabelRaw(&playbackSettingsToggleLabels[0], &uiFont, 0, 0, 60, AUTO,
                playbackFontSize(18), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_OFF_UPPER));
   initLabelRaw(&playbackSettingsToggleLabels[1], &uiFont, 0, 0, 60, AUTO,
                playbackFontSize(18), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_ON_UPPER));
   initLabelRaw(&playbackAutoTranslateTitle, &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_AUTO_TRANSLATE_SUBTITLES));
   for (int i = 0; i < PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS; i++)
      initLabelRaw(&playbackAutoTranslateRows[i], &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 100, AUTO,
                   playbackFontSize(18), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabelRaw(&playbackSubtitleStyleTitle, &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_SUBTITLE_STYLE));
   const char *subtitleStyleNames[PLAYBACK_SUBTITLE_STYLE_COUNT] = {
      LOC(STR_TEXT_COLOR), LOC(STR_BACKGROUND_COLOR), LOC(STR_BACKGROUND_OPACITY), LOC(STR_TEXT_HIGHLIGHT), LOC(STR_FONT_SIZE)
   };
   for (int i = 0; i < PLAYBACK_SUBTITLE_STYLE_COUNT; i++)
      initLabelRaw(&playbackSubtitleStyleLabels[i], &uiFont, 0, 0, 300, AUTO,
                   playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, subtitleStyleNames[i]);
   initLabelRaw(&playbackSubtitlePreview, &uiFont, 0, 0, 520, AUTO,
                playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4), 0xFFFFFFFF, TEXT_WRAP, LOC(STR_SUBTITLE_PREVIEW));
   initLabelRaw(&playbackSponsorTitle, &uiFont, 0, 0, PLAYBACK_SPONSOR_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_SPONSORBLOCK));
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++)
      initLabelRaw(&playbackSponsorCategoryLabels[i], &uiFont, 0, 0, 420, AUTO,
                   playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
                   getSponsorCategoryName((SponsorCategory)i));
   initLabelRaw(&playbackSponsorActionLabels[0], &uiFont, 0, 0, PLAYBACK_SPONSOR_OPTION_W, AUTO,
                playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_SKIP));
   initLabelRaw(&playbackSponsorActionLabels[1], &uiFont, 0, 0, PLAYBACK_SPONSOR_OPTION_W, AUTO,
                playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_ASK));
   initLabelRaw(&playbackSponsorActionLabels[2], &uiFont, 0, 0, PLAYBACK_SPONSOR_OPTION_W, AUTO,
                playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, LOC(STR_KEEP));

   state.subtitlePick = state.subtitleFetched = -1;   // default off (memset left them 0)
   state.subtitleScanFrom = -1;
   state.subtitleDisplayedCue = -1;
   state.subtitleDisplayedPending = 0;
   state.subtitleLastPosition = 0.0f;

   initVolumeMeter(&volumeMeter, &uiFont, activeTheme->accent, volumeLevel);
   applyPlaybackUiLabelScale(playbackFontScale);
   layoutVolumeMeter(&volumeMeter, state.screenW, state.screenH);

   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-play") == 0);
   if (!state.threadActive) fail(LOC(STR_COULD_NOT_START_PLAYBACK_WORKER));

   if (hasSponsorBlockActions() && isBareVideoId(requestedInput))
      state.sbThreadActive = (spawnJoinableThread(&state.sbWorkerTid, sponsorWorker, 0,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-sponsor") == 0);
}

static void showControls(void)
{
   state.controlsDismissed = 0;
   state.controlsShownUs = sys_time_get_system_time();
}

static void showFullPlaybackHud(void)
{
   uint64_t now = sys_time_get_system_time();
   state.controlsDismissed = 0;
   state.controlsShownUs = now;
   state.hudShownUs = now;
}

int isPlaybackPausedForScreenSaver(void)
{
   return state.stage == STAGE_PLAYING && state.player && isVideoPaused(state.player);
}

// The Cross press that dismisses the screensaver must not be interpreted again
// as the release half of the normal playback pause toggle.
static int suppressNextCrossRelease;

int resumePlaybackFromScreenSaver(void)
{
   if (!isPlaybackPausedForScreenSaver()) return 0;
   setVideoPaused(state.player, 0);
   if (!state.isLive) suppressNextCrossRelease = 1;
   showFullPlaybackHud();
   return 1;
}

// Rebuild the right-side playback HUD only on input/state changes, never in the render loop.
static void refreshPlaybackInfoHud(void)
{
   char info[256];
   int used = 0;
   if (state.vidW > 0 && state.vidH > 0)
      used = snprintf(info, sizeof info, "%dx%d", state.vidW, state.vidH);
   else
      used = snprintf(info, sizeof info, LOC(STR_RESOLUTION_UNAVAILABLE));

   if (state.audioPick >= 0 && state.audioPick < state.audioCount) {
      used += snprintf(info + used, sizeof info - used, "\n");
      used += snprintf(info + used, sizeof info - used, LOC(STR_PLAYBACK_AUDIO_FORMAT),
                       state.audioNames[state.audioPick]);
   } else if (state.audItag && state.audItag == state.vidItag) {
      used += snprintf(info + used, sizeof info - used, "\n");
      used += snprintf(info + used, sizeof info - used, LOC(STR_PLAYBACK_AUDIO_FORMAT), LOC(STR_AUDIO_EMBEDDED));
   } else if (state.audItag) {
      used += snprintf(info + used, sizeof info - used, "\n");
      used += snprintf(info + used, sizeof info - used, LOC(STR_PLAYBACK_AUDIO_FORMAT), LOC(STR_AUDIO_AAC));
   } else {
      used += snprintf(info + used, sizeof info - used, "\n");
      used += snprintf(info + used, sizeof info - used, LOC(STR_PLAYBACK_AUDIO_FORMAT),
                       LOC(STR_UNAVAILABLE));
   }

   if (state.subtitlePick >= 0 && state.subtitlePick < state.captionCount) {
      used += snprintf(info + used, sizeof info - used, "\n");
      snprintf(info + used, sizeof info - used, LOC(STR_PLAYBACK_SUBTITLES_FORMAT),
               state.captionTracks[state.subtitlePick].name);
   }

   setLabelText(&playbackInfoLabel, info);
}

static void formatTime(char *buffer, int cap, int seconds)
{
   if (seconds < 0) seconds = 0;
   if (seconds >= 3600) snprintf(buffer, cap, "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
   else                 snprintf(buffer, cap, "%d:%02d", seconds / 60, seconds % 60);
}

static void showToastFor(const char *text, uint64_t durationUs)
{
   setLabelText(&toastLabel, text);
   state.toastShownUs = sys_time_get_system_time();
   state.toastDurationUs = durationUs;
}

static void showToast(const char *text)
{
   showToastFor(text, TOAST_VISIBLE_US);
}

static void setSubtitleTrack(int trackIndex)
{
   if (trackIndex < -1 || trackIndex >= state.captionCount) trackIndex = -1;
   if (trackIndex == state.subtitlePick) {
      refreshPlaybackInfoHud();
      return;
   }
   // Stop network/translation work immediately when subtitles are turned off or switched.
   // The worker is joined by updatePlay before another track is fetched.
   if (state.subThreadActive && trackIndex != state.subtitleFetched)
      state.subtitleTranslation.cancel = 1;
   state.subtitlePick = trackIndex;
   if (trackIndex >= 0) lastSubtitlePick = trackIndex;
   state.subtitlesReady = 0;
   state.subtitleScanFrom = -1;
   state.subtitleDisplayedCue = -1;
   state.subtitleDisplayedPending = 0;
   state.subtitleLastPosition = 0.0f;
   if (!state.subThreadActive && trackIndex >= 0) {
      if (state.subtitleFetched == trackIndex && state.subtitleTrack && state.subtitleTrack->count > 0)
         state.subtitlesReady = 1;
      else if (state.subtitleFetched != trackIndex)
         state.subtitleFetched = -1;
   }
   setLabelText(&subtitleLabel, "");
   refreshPlaybackInfoHud();
}

static void showSelectedSubtitleToast(void)
{
   char notice[112];
   int trackIndex = state.subtitlePick;

   if (trackIndex < 0 || trackIndex >= state.captionCount) {
      showToast(LOC(STR_SUBTITLES_OFF));
      return;
   }

   const CaptionTrack *track = &state.captionTracks[trackIndex];
   if (track->isAutoGenerated && !strstr(track->name, "auto-generated") &&
       !strstr(track->name, "auto-translated")) {
      snprintf(notice, sizeof notice, LOC(STR_SUBTITLES_AUTO_GENERATED_FORMAT), track->name);
   } else {
      snprintf(notice, sizeof notice, LOC(STR_SUBTITLES_FORMAT), track->name);
   }
   showToast(notice);
}

static void applyPreferredSubtitleTrack(void)
{
   int track = -1;
   if (!state.isLive && subtitlesEnabled) {
      if (lastSubtitlePick >= 0 && lastSubtitlePick < state.captionCount)
         track = lastSubtitlePick;
      else
         track = findPreferredSubtitleTrack(getPlaybackSubtitleLanguage());
      if (track < 0) {
         for (int i = 0; i < state.captionCount; i++)
            if (!subtitleIsAutoTranslated(&state.captionTracks[i])) { track = i; break; }
      }
   }
   setSubtitleTrack(track);
}

// fetches the picked caption track in the background; updatePlay reaps it and respawns on a new pick.
static int subtitleCachePath(const CaptionTrack *track, char *out, int cap)
{
   char videoId[12];
   if (!track || !track->isAutoTranslated || !track->sourceLanguageCode[0]) return 0;
   if (requestedIsLocal) {
      if (!getRequestedDownloadedId(videoId)) return 0;
   } else if (!extractPlaybackId(requestedInput, videoId)) return 0;
   for (const char *p = track->sourceLanguageCode; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-')) return 0;
   for (const char *p = track->languageCode; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-')) return 0;
   int length = snprintf(out, cap, CRTUBE_DATA_DIR "/subtitle-cache/%s_%s_%s.srt",
                         videoId, track->sourceLanguageCode, track->languageCode);
   return length > 0 && length < cap;
}

static void saveSubtitleCache(const SubtitleTrack *track, const char *path)
{
   char temporary[MAX_PATH_LEN];
   int length = snprintf(temporary, sizeof temporary, "%s.tmp", path);
   if (length <= 0 || length >= (int)sizeof temporary) return;
   if (saveSubtitleTrackSrt(track, temporary) != 0) {
      deleteFile(temporary);
      return;
   }
   // Publish only a complete SRT. A power loss during write leaves a .tmp file, never a valid-looking cache.
   VfsStat temporaryStat;
   if (statPath(temporary, &temporaryStat) != 0 ||
       reserveSubtitleTranslationCacheSpace(path, temporaryStat.size) != 0) {
      deleteFile(temporary);
      logWarn("[subs] translation cache full; keeping translation for this session only\n");
      return;
   }
   if (fileExists(path)) deleteFile(path);
   if (renamePath(temporary, path) != 0) deleteFile(temporary);
   else touchSubtitleTranslationCache(path);
}

static void subtitleWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   CaptionTrack *track = &state.captionTracks[state.subtitleFetched];
   char cachePath[MAX_PATH_LEN];
   int hasCachePath = subtitleCachePath(track, cachePath, sizeof cachePath);

   if (hasCachePath && fileExists(cachePath) &&
       loadSubtitlesSrt(cachePath, state.subtitleTrack) == 0) {
      state.subtitleFetchResult = 0;
      touchSubtitleTranslationCache(cachePath);
      logInfo("[subs] translation cache hit: %s\n", cachePath);
   } else {
      state.subtitleFetchResult = track->isLocalFile ?
         loadSubtitlesSrt(track->url, state.subtitleTrack) :
         fetchSubtitles(track->url, state.subtitleTrack);
      if (state.subtitleFetchResult == 0 && track->isAutoTranslated &&
          state.subtitleTrack->count > 0 && !state.subtitleTranslation.cancel)
         state.subtitleFetchResult = translateSubtitles(state.subtitleTrack, track->languageCode,
                                                         &state.subtitleTranslation);
      // Only complete translations are cached. A cancelled or rate-limited translation may contain
      // source-language cues and must be retried on the next selection.
      if (hasCachePath && state.subtitleFetchResult == 0 &&
          !state.subtitleTranslation.cancel && !state.subtitleTranslation.stopped &&
          state.subtitleTrack->count > 0) {
         makeDir(CRTUBE_DATA_DIR "/subtitle-cache");
         saveSubtitleCache(state.subtitleTrack, cachePath);
         if (fileExists(cachePath)) logInfo("[subs] translation cached: %s\n", cachePath);
      }
   }
   __sync_synchronize();
   state.subWorkerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

// Triangle only toggles subtitles. The selected track is retained while OFF.
static void toggleSubtitles(void)
{
   if (state.captionCount == 0 || state.isLive) { showToast(LOC(STR_NO_SUBTITLES)); return; }

   if (subtitlesEnabled) {
      subtitlesEnabled = 0;
      setSubtitleTrack(-1);
      showToast(LOC(STR_SUBTITLES_OFF));
      return;
   }

   subtitlesEnabled = 1;
   applyPreferredSubtitleTrack();
   if (state.subtitlePick < 0) {
      subtitlesEnabled = 0;
      showToast(LOC(STR_NO_SUBTITLES_AVAILABLE));
      return;
   }
   showSelectedSubtitleToast();
}

static void audioSwitchWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   int target = state.audioSwitchTarget;
   logInfo("[audio-diag] L3 switch begin old=%d target=%d id='%s' posMs=%d wasPaused=%d urlLen=%d\n",
           state.audioPick, target, state.audioIds[target],
           (int)(state.audioSwitchPosition * 1000.0f), state.audioSwitchWasPaused,
           (int)strlen(state.audioUrls[target]));
   int rc = replaceVideoPlayerAudio(state.player, state.audioUrls[target], state.audioSwitchPosition);
   state.audioSwitchFallback = rc == 0;
   if (rc == 0)
      logInfo("[audio-diag] L3 switch accepted target=%d itag=%d\n", target, state.audioItags[target]);
   else
      logWarn("[audio-diag] L3 switch failed target=%d; previous video remains active\n", target);
   __sync_synchronize();
   state.audioSwitchDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static double liveEdgeNow(void);
static double livePositionSq(void);

static VideoPlayer *openReadyResolutionPlayer(const char *videoUrl, const char *audioUrl, float position)
{
   if (playbackCancelRequested()) return NULL;
   VideoPlayer *replacement = createVideoPlayerSplitCancelable(
      videoUrl, audioUrl && audioUrl[0] ? audioUrl : NULL,
      allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);
   if (!replacement || playbackCancelRequested()) {
      if (replacement) destroyVideoPlayer(replacement);
      return NULL;
   }
   if (audioUrl && audioUrl[0] && !isVideoPlayerUsingSeparateAudio(replacement)) {
      logWarn("[video-diag] replacement video did not open the requested separate audio source\n");
      destroyVideoPlayer(replacement);
      return NULL;
   }
   if (position > 0.5f) seekVideoPlayer(replacement, position);
   setVideoPaused(replacement, 1);

   // Only one VDEC instance is kept alive during a resolution change. The previous player is
   // retired before this function is called, avoiding a second decoder's SPU/work-memory demand.
   uint64_t started = sys_time_get_system_time();
   while (!playbackCancelRequested() && !isVideoPlayerReady(replacement) &&
          !isVideoPlayerFailed(replacement) &&
          sys_time_get_system_time() - started < 15000000ull) sleepMs(50);
   if (playbackCancelRequested() || !isVideoPlayerReady(replacement)) {
      if (!playbackCancelRequested())
         logWarn("[video-diag] resolution stream did not reach decoded pre-roll\n");
      destroyVideoPlayer(replacement);
      return NULL;
   }
   return replacement;
}

static void resolutionSwitchWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   VideoPlayer *oldPlayer = state.retiringResolutionPlayer;
   state.retiringResolutionPlayer = NULL;
   if (oldPlayer) destroyVideoPlayer(oldPlayer);

   VideoPlayer *replacement = NULL;
   if (!state.resolutionSwitchAborting && state.resolutionSwitchTeeRecovery) {
      // The old TEE session has already failed and was retired above. Never reopen it.
      // Try each available lower profile until the original Sony decoder reaches pre-roll.
      for (int profile = state.resolutionSwitchTarget; profile >= 0 && !replacement; profile--) {
         if (!state.videoAvailable[profile] ||
             strcmp(state.videoUrls[profile], state.resolutionSwitchOldVideoUrl) == 0)
            continue;
         state.resolutionSwitchTarget = profile;
         strCopy(state.resolutionSwitchVideoUrl, sizeof state.resolutionSwitchVideoUrl,
                 state.videoUrls[profile]);
         logInfo("[tee-recovery] trying Sony fallback profile=%d %dp@%d itag=%d\n",
                 profile, state.videoHeights[profile], state.videoFps[profile],
                 state.videoItags[profile]);
         replacement = openReadyResolutionPlayer(state.resolutionSwitchVideoUrl,
                                                   state.resolutionSwitchAudioUrl,
                                                   state.resolutionSwitchPosition);
      }
   } else if (!state.resolutionSwitchAborting) {
      replacement = openReadyResolutionPlayer(state.resolutionSwitchVideoUrl,
                                                state.resolutionSwitchAudioUrl,
                                                state.resolutionSwitchPosition);
   }

   // Manual resolution changes restore the previous rendition on failure. TEE recovery must
   // never reopen the failed 1080p60 source.
   if (!replacement && !state.resolutionSwitchAborting && !state.resolutionSwitchTeeRecovery) {
      logWarn("[video-diag] requested resolution failed; reopening previous rendition\n");
      replacement = openReadyResolutionPlayer(state.resolutionSwitchOldVideoUrl,
                                                state.resolutionSwitchOldAudioUrl,
                                                state.resolutionSwitchPosition);
      state.resolutionSwitchRestored = replacement != NULL;
   }
   if (!replacement && state.resolutionSwitchTeeRecovery) {
      strCopy(state.message, sizeof state.message, LOC(STR_VIDEO_DECODER_RECOVERY_FAILED));
      state.stage = STAGE_FAILED;
   }
   if (!replacement && !state.resolutionSwitchAborting && !state.resolutionSwitchTeeRecovery) {
      strCopy(state.message, sizeof state.message, LOC(STR_RESOLUTION_SWITCH_RECOVERY_FAILED));
      state.stage = STAGE_FAILED;
   }

   state.pendingResolutionPlayer = replacement;
   __sync_synchronize();
   state.resolutionSwitchDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static int startResolutionSwitch(int target, int automaticFallback)
{
   if (target < 0 || target >= state.videoCount || !state.videoAvailable[target] ||
       !state.player || state.resolutionSwitchThreadActive || state.audioSwitchThreadActive ||
       state.seeking || (state.isLive && automaticFallback != 2 && automaticFallback != 3)) return 0;
   if (strcmp(state.videoUrls[target], state.videoUrls[state.resolutionIndex]) == 0) return 0;

   state.resolutionSwitchTarget = target;
   state.resolutionSwitchOldIndex = state.resolutionIndex;
   state.resolutionSwitchRestored = 0;
   state.resolutionSwitchAborting = 0;
   state.resolutionSwitchTeeRecovery = (automaticFallback == 2);
   state.resolutionSwitchFpsFallback = (automaticFallback == 3);
   state.resolutionSwitchWasPaused = isVideoPaused(state.player);
   state.resolutionSwitchWasFirstFrameSeen = state.firstFrameSeen;
   state.resolutionSwitchPosition = state.isLive ? 0.0f : getVideoPositionSeconds(state.player);
   strCopy(state.resolutionSwitchVideoUrl, sizeof state.resolutionSwitchVideoUrl, state.videoUrls[target]);
   strCopy(state.resolutionSwitchOldVideoUrl, sizeof state.resolutionSwitchOldVideoUrl,
           state.videoUrls[state.resolutionIndex]);
   if (state.isLive) {
      long edge = (long)liveEdgeNow();
      long playSq = state.liveDvr ? (long)livePositionSq() : -1;
      char descriptor[MAX_STREAM_URL];
      state.resolutionSwitchLiveEdgeSq = edge;
      state.resolutionSwitchLivePlaySq = playSq;
      state.resolutionSwitchLiveFirstSq = playSq < 0 ? edge - LIVE_START_BEHIND : playSq;
      if (state.resolutionSwitchLiveFirstSq < state.liveFloorSq)
         state.resolutionSwitchLiveFirstSq = state.liveFloorSq;
      state.resolutionSwitchLiveLatencySec = playSq < 0 && !state.liveEnded
         ? (float)((LIVE_START_BEHIND + 1) * state.liveSegmentSec) : 0.0f;
      strCopy(descriptor, sizeof descriptor, state.resolutionSwitchVideoUrl);
      writeLiveDescriptor(state.resolutionSwitchVideoUrl, sizeof state.resolutionSwitchVideoUrl,
                          descriptor, edge, playSq);
      if (!state.resolutionSwitchVideoUrl[0])
         strCopy(state.resolutionSwitchVideoUrl, sizeof state.resolutionSwitchVideoUrl, descriptor);
      strCopy(descriptor, sizeof descriptor, state.resolutionSwitchOldVideoUrl);
      writeLiveDescriptor(state.resolutionSwitchOldVideoUrl, sizeof state.resolutionSwitchOldVideoUrl,
                          descriptor, edge, playSq);
      if (!state.resolutionSwitchOldVideoUrl[0])
         strCopy(state.resolutionSwitchOldVideoUrl, sizeof state.resolutionSwitchOldVideoUrl, descriptor);
   }
   state.resolutionSwitchAudioUrl[0] = 0;
   state.resolutionSwitchOldAudioUrl[0] = 0;
   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      strCopy(state.resolutionSwitchAudioUrl, sizeof state.resolutionSwitchAudioUrl, state.audioUrls[state.audioPick]);
   else if (state.fallbackAudioUrl[0])
      strCopy(state.resolutionSwitchAudioUrl, sizeof state.resolutionSwitchAudioUrl, state.fallbackAudioUrl);
   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      strCopy(state.resolutionSwitchOldAudioUrl, sizeof state.resolutionSwitchOldAudioUrl, state.audioUrls[state.audioPick]);
   else if (state.fallbackAudioUrl[0] && isVideoPlayerUsingSeparateAudio(state.player))
      strCopy(state.resolutionSwitchOldAudioUrl, sizeof state.resolutionSwitchOldAudioUrl, state.fallbackAudioUrl);
   if (state.isLive) {
      char descriptor[MAX_STREAM_URL];
      if (state.resolutionSwitchAudioUrl[0]) {
         strCopy(descriptor, sizeof descriptor, state.resolutionSwitchAudioUrl);
         writeLiveDescriptor(state.resolutionSwitchAudioUrl, sizeof state.resolutionSwitchAudioUrl,
                             descriptor, state.resolutionSwitchLiveEdgeSq,
                             state.resolutionSwitchLivePlaySq);
         if (!state.resolutionSwitchAudioUrl[0])
            strCopy(state.resolutionSwitchAudioUrl, sizeof state.resolutionSwitchAudioUrl, descriptor);
      }
      if (state.resolutionSwitchOldAudioUrl[0]) {
         strCopy(descriptor, sizeof descriptor, state.resolutionSwitchOldAudioUrl);
         writeLiveDescriptor(state.resolutionSwitchOldAudioUrl, sizeof state.resolutionSwitchOldAudioUrl,
                             descriptor, state.resolutionSwitchLiveEdgeSq,
                             state.resolutionSwitchLivePlaySq);
         if (!state.resolutionSwitchOldAudioUrl[0])
            strCopy(state.resolutionSwitchOldAudioUrl, sizeof state.resolutionSwitchOldAudioUrl, descriptor);
      }
   }

   setVideoPaused(state.player, 1);
   if (state.firstFrameSeen && (!state.isLive || state.liveEnded)) {
      char historyId[12];
      if (extractPlaybackId(requestedInput, historyId)) {
         setWatchedDuration(historyId, (int)(getVideoDurationSeconds(state.player) + 0.5f));
         setWatchedPosition(historyId, (int)state.resolutionSwitchPosition);
      }
   }
   finishGfx();   // retire the displayed frame before the worker frees its RSX-visible buffers
   state.retiringResolutionPlayer = state.player;
   state.player = NULL;
   state.firstFrameSeen = 0;
   state.resolutionSwitchDone = 0;
   state.resolutionSwitchThreadActive = (spawnJoinableThread(&state.resolutionSwitchTid, resolutionSwitchWorker, 0,
                                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-resolution") == 0);
   if (!state.resolutionSwitchThreadActive) {
      state.player = state.retiringResolutionPlayer;
      state.retiringResolutionPlayer = NULL;
      if (!state.resolutionSwitchWasPaused && state.player) setVideoPaused(state.player, 0);
      state.firstFrameSeen = state.resolutionSwitchWasFirstFrameSeen;
      state.resolutionSwitchFpsFallback = 0;
      if (automaticFallback != 2) showToast(LOC(STR_RESOLUTION_SWITCH_COULD_NOT_START));
      return 0;
   }
   if (state.isLive) {
      state.liveEdgeSq = state.resolutionSwitchLiveEdgeSq;
      state.liveEdgeUs = sys_time_get_system_time();
      state.liveFirstSq = state.resolutionSwitchLiveFirstSq;
      state.liveLatencySec = state.resolutionSwitchLiveLatencySec;
      state.liveAnchored = 0;
      state.liveFirstPtsNs = 0;
   }
   if (automaticFallback != 2 && automaticFallback != 3) {
      char notice[96];
      if (state.videoFps[target] > 0) {
         if (automaticFallback)
            snprintf(notice, sizeof notice, LOC(STR_REDUCING_RESOLUTION_AUDIO_ISSUE_FPS),
                     state.videoHeights[target], state.videoFps[target]);
         else
            snprintf(notice, sizeof notice, LOC(STR_CHANGING_TO_RESOLUTION_FPS),
                     state.videoHeights[target], state.videoFps[target]);
      } else {
         if (automaticFallback)
            snprintf(notice, sizeof notice, LOC(STR_REDUCING_RESOLUTION_AUDIO_ISSUE), state.videoHeights[target]);
         else
            snprintf(notice, sizeof notice, LOC(STR_CHANGING_TO_RESOLUTION), state.videoHeights[target]);
      }
      showToast(notice);
   }
   return 1;
}

static int findLowerResolutionProfile(void)
{
   for (int profile = state.resolutionIndex - 1; profile >= 0; profile--) {
      if (!state.videoAvailable[profile]) continue;
      if (strcmp(state.videoUrls[profile], state.videoUrls[state.resolutionIndex]) == 0) continue;
      return profile;
   }
   return -1;
}

// Prefer 30 FPS at the current resolution; otherwise use the highest 30 FPS
// rendition that does not increase the current resolution.
// Keep the current resolution when moving from 50/60 to 30 FPS. If the current
// rendition is already <=30 FPS, step down in resolution instead of retrying it.
static int findAdaptiveFallbackProfile(void)
{
   int currentHeight, sameHeight30 = -1, lower30 = -1, lowerHighFps = -1;
   if (state.resolutionIndex < 0 || state.resolutionIndex >= state.videoCount) return -1;
   currentHeight = state.videoHeights[state.resolutionIndex];

   // First preserve the current resolution at 30 FPS (e.g. 1080p60 -> 1080p30).
   if (state.vidFps >= 50) {
      for (int i = 0; i < state.videoCount; i++) {
         if (i == state.resolutionIndex || !state.videoAvailable[i] || state.videoFps[i] != 30 ||
             state.videoHeights[i] != currentHeight ||
             strcmp(state.videoUrls[i], state.videoUrls[state.resolutionIndex]) == 0) continue;
         if (sameHeight30 < 0 || state.videoHasAudio[i] < state.videoHasAudio[sameHeight30]) sameHeight30 = i;
      }
      if (sameHeight30 >= 0) return sameHeight30;
   }

   // Prefer a lower-resolution 30 FPS stream when available. If the only 30 FPS
   // option is much smaller (e.g. 480p30), compare it with higher-resolution 50/60 FPS.
   for (int i = 0; i < state.videoCount; i++) {
      if (i == state.resolutionIndex || !state.videoAvailable[i] || state.videoFps[i] != 30 ||
          state.videoHeights[i] >= currentHeight ||
          strcmp(state.videoUrls[i], state.videoUrls[state.resolutionIndex]) == 0) continue;
      if (lower30 < 0 || state.videoHeights[i] > state.videoHeights[lower30]) lower30 = i;
   }
   for (int i = 0; i < state.videoCount; i++) {
      if (i == state.resolutionIndex || !state.videoAvailable[i] || state.videoFps[i] < 50 ||
          state.videoHeights[i] >= currentHeight ||
          strcmp(state.videoUrls[i], state.videoUrls[state.resolutionIndex]) == 0) continue;
      if (lowerHighFps < 0 || state.videoHeights[i] > state.videoHeights[lowerHighFps] ||
          (state.videoHeights[i] == state.videoHeights[lowerHighFps] &&
           state.videoFps[i] > state.videoFps[lowerHighFps])) lowerHighFps = i;
   }

   if (lowerHighFps >= 0 &&
       (lower30 < 0 || state.videoHeights[lowerHighFps] > state.videoHeights[lower30]))
      return lowerHighFps;
   if (lower30 >= 0) return lower30;
   return lowerHighFps;
}

static void startAudioTrackSwitch(int target)
{
   if (state.seeking) {
      logWarn("[audio-diag] audio selection ignored while a seek transaction is pending\n");
      showToast(LOC(STR_WAIT_SEEK_BEFORE_AUDIO));
      return;
   }
   if (target < 0 || target >= state.audioCount) {
      showToast(LOC(STR_AUDIO_TRACK_UNAVAILABLE));
      return;
   }
   if (target == state.audioPick) return;
   if (state.isLive || !state.player || state.audioSwitchThreadActive) {
      showToast(LOC(STR_AUDIO_TRACK_BUSY_UNAVAILABLE));
      return;
   }

   state.audioSwitchPosition = getVideoPositionSeconds(state.player);
   state.audioSwitchWasPaused = isVideoPaused(state.player);
   state.audioSwitchTarget = target;
   state.audioFallbackAttempted = 0;
   state.audioSwitchFallback = 0;
   setVideoPaused(state.player, 1);
   snprintf(state.message, sizeof state.message, LOC(STR_LOADING_AUDIO_FORMAT), state.audioNames[target]);
   refreshPlaybackInfoHud();
   showControls();
   state.audioSwitchDone = 0;
   state.audioSwitchThreadActive = (spawnJoinableThread(&state.audioSwitchTid, audioSwitchWorker, 0,
                                    THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-audio") == 0);
   if (!state.audioSwitchThreadActive) {
      state.message[0] = 0;
      if (!state.audioSwitchWasPaused) setVideoPaused(state.player, 0);
      showToast(LOC(STR_AUDIO_SWITCH_FAILED));
   }
}

// accumulates a scrub target while left/right is held; the engine seek fires once input goes quiet
static void nudgeSeek(float deltaSeconds, uint64_t nowUs)
{
   if (!state.seeking) {
      state.seeking = 1;
      state.seekTarget = getVideoPositionSeconds(state.player);
      chapterNavIndexValid = chapterNavQueued = chapterNavInFlight = 0;
   }
   state.seekTarget += deltaSeconds;
   float duration = getVideoDurationSeconds(state.player);
   if (state.seekTarget < 0.0f) state.seekTarget = 0.0f;
   if (duration > 0.0f && state.seekTarget > duration) state.seekTarget = duration;
   state.lastSeekInputUs = nowUs;
   showControls();
}

// left/right scrub, cross toggles pause (restarts if the video ended). only once playing.
static void updateCropPan(void);
static int findAskSponsorSegment(void);
static int controlsVisible(void);
static int fullPlaybackHudVisible(void);

static double liveEdgeNow(void)
{
   if (state.liveEnded || state.liveSegmentSec <= 0) return (double)state.liveEdgeSq;
   double elapsed = (double)(sys_time_get_system_time() - state.liveEdgeUs) / 1000000.0;
   return (double)state.liveEdgeSq + elapsed / (double)state.liveSegmentSec;
}

static double livePositionSq(void)
{
   if (!state.player || !state.liveAnchored || state.liveSegmentSec <= 0)
      return (double)state.liveFirstSq;
   uint64_t pts = getVideoPositionNs(state.player);
   double played = pts > state.liveFirstPtsNs ? (double)(pts - state.liveFirstPtsNs) / 1000000000.0 : 0.0;
   return (double)state.liveFirstSq + played / (double)state.liveSegmentSec;
}

static double liveWindowStartSq(void)
{
   double start = liveEdgeNow() - (double)state.liveWindowSegs +
                  (state.liveEnded ? 0.0 : (double)LIVE_WINDOW_MARGIN);
   return start > (double)state.liveFloorSq ? start : (double)state.liveFloorSq;
}

static float liveTimelineLength(void)
{
   double segments = liveEdgeNow() + 1.0 - liveWindowStartSq();
   float length = (float)(segments * state.liveSegmentSec) - state.liveLatencySec;
   return length > 0.0f ? length : 0.0f;
}

static float liveTimelinePosition(void)
{
   float position = (float)((livePositionSq() - liveWindowStartSq()) * state.liveSegmentSec);
   if (position < 0.0f) position = 0.0f;
   float length = liveTimelineLength();
   if (position > length) position = length;
   return position;
}

// Normal segment startup latency is not considered a user rewind.
static int isLiveBehindAtEdge(float behindSeconds)
{
   return behindSeconds <= LIVE_EDGE_SLACK_SEC + 0.5f * (float)state.liveSegmentSec;
}

static void liveReopenWorker(uint64_t arg)
{
   (void)arg;
   setHttpRequestGroup(&state.requestGroup);
   VideoPlayer *old = state.liveRetiringPlayer;
   state.liveRetiringPlayer = NULL;
   if (old) destroyVideoPlayer(old);

   VideoPlayer *replacement = NULL;

   // Android-fallback LIVE sessions re-resolve the selected itags for every seek/reconnect.
   // The extractor tries ANDROID first, then VISIONOS and the remaining LIVE clients.
   if (!playbackCancelRequested() && state.liveAndroidFallbackSession) {
      const Extractor *extractor = findExtractor(requestedInput);
      StreamInfo *info = (StreamInfo *)calloc(1, sizeof *info);
      int separateAudio = state.audItag > 0 && !state.videoHasAudio[state.resolutionIndex];
      int rc = -1;
      if (info && extractor && extractor->refreshLiveForSeek)
         rc = extractor->refreshLiveForSeek(requestedInput, state.vidItag,
                                             separateAudio ? state.audItag : 0, info);
      const StreamFormat *video = rc == 0 ? findLiveFormatByItag(info, state.vidItag) : NULL;
      const char *trackId = state.audioPick >= 0 && state.audioPick < state.audioCount
                          ? state.audioIds[state.audioPick] : NULL;
      const StreamFormat *audio = separateAudio
         ? findLiveAudioFormatByItag(info, state.audItag, trackId) : NULL;
      char refreshedVideo[MAX_STREAM_URL] = "";
      char refreshedAudio[MAX_STREAM_URL] = "";
      if (video && (!separateAudio || audio)) {
         long targetSq = state.liveLatencySec > 0.0f
                       ? video->liveEdgeSq - LIVE_START_BEHIND : state.liveFirstSq;
         if (targetSq < video->liveStartSq) targetSq = video->liveStartSq;
         if (targetSq > video->liveEdgeSq) targetSq = video->liveEdgeSq;
         writeLiveDescriptor(refreshedVideo, sizeof refreshedVideo, video->url,
                             video->liveEdgeSq, targetSq);
         if (audio)
            writeLiveDescriptor(refreshedAudio, sizeof refreshedAudio, audio->url,
                                audio->liveEdgeSq, targetSq);
         if (refreshedVideo[0])
            replacement = createVideoPlayerSplitCancelable(refreshedVideo,
                               refreshedAudio[0] ? refreshedAudio : NULL,
                               allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);
         if (replacement) {
            strCopy(state.liveVideoOpen, sizeof state.liveVideoOpen, refreshedVideo);
            strCopy(state.liveAudioOpen, sizeof state.liveAudioOpen, refreshedAudio);
            updateStoredLiveUrls(info);
            state.liveAndroidClient = info->liveAndroidClient;
            state.liveDvr = info->liveDvr;
            if (info->liveSegmentSec > 0) state.liveSegmentSec = info->liveSegmentSec;
            if (info->liveWindowSec > 0 && state.liveSegmentSec > 0) {
               state.liveWindowSegs = info->liveWindowSec / state.liveSegmentSec;
               if (state.liveWindowSegs < 1) state.liveWindowSegs = 1;
            }
            state.liveEdgeSq = video->liveEdgeSq;
            state.liveFloorSq = video->liveStartSq;
            state.liveFirstSq = targetSq;
            state.liveEdgeUs = sys_time_get_system_time();
            logInfo("[yt-live-seek] reopened selected itags after Android-first refresh client=%s edgeSq=%ld\n",
                    state.liveAndroidClient ? "ANDROID" : "VISIONOS", state.liveEdgeSq);
         }
      }
      freeStreamInfo(info);
   }

   // Keep the previous signed descriptor as a final recovery attempt if re-resolution
   // could not produce a usable replacement. Non-Android LIVE sessions retain their
   // original reopen path unchanged.
   if (!replacement && !playbackCancelRequested())
      replacement = createVideoPlayerSplitCancelable(state.liveVideoOpen,
                         state.liveAudioOpen[0] ? state.liveAudioOpen : NULL,
                         allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);

   if (playbackCancelRequested() && replacement) {
      destroyVideoPlayer(replacement);
      replacement = NULL;
   }
   if (!replacement && !playbackCancelRequested()) fail(LOC(STR_LIVE_SEEK_RECONNECT_FAILED));
   state.pendingPlayer = replacement;
   __sync_synchronize();
   state.workerDone = 1;
   setHttpRequestGroup(NULL);
   exitThread();
}

static int reopenLiveAt(long playSq)
{
   if (!state.isLive || !state.player || state.threadActive || state.liveReopenThreadActive ||
       state.resolutionSwitchThreadActive || state.audioSwitchThreadActive || state.resolutionIndex < 0)
      return 0;

   long edge = (long)liveEdgeNow();
   if (state.liveEnded) {
      if (playSq < state.liveFloorSq) playSq = state.liveFloorSq;
      if (playSq > edge) playSq = edge;
   } else if (playSq > edge - LIVE_START_BEHIND) {
      playSq = -1;
   }

   const char *videoUrl = state.videoUrls[state.resolutionIndex];
   const char *audioUrl = NULL;
   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      audioUrl = state.audioUrls[state.audioPick];
   else if (state.fallbackAudioUrl[0] && !state.videoHasAudio[state.resolutionIndex])
      audioUrl = state.fallbackAudioUrl;

   writeLiveDescriptor(state.liveVideoOpen, sizeof state.liveVideoOpen, videoUrl, edge, playSq);
   state.liveAudioOpen[0] = 0;
   if (audioUrl && audioUrl[0]) {
      writeLiveDescriptor(state.liveAudioOpen, sizeof state.liveAudioOpen, audioUrl, edge, playSq);
      if (!state.liveAudioOpen[0]) strCopy(state.liveAudioOpen, sizeof state.liveAudioOpen, audioUrl);
   }
   if (!state.liveVideoOpen[0]) return 0;

   state.liveEdgeSq = edge;
   state.liveEdgeUs = sys_time_get_system_time();
   state.liveFirstSq = playSq < 0 ? edge - LIVE_START_BEHIND : playSq;
   if (state.liveFirstSq < state.liveFloorSq) state.liveFirstSq = state.liveFloorSq;
   state.liveLatencySec = playSq < 0 && !state.liveEnded
                        ? (float)((LIVE_START_BEHIND + 1) * state.liveSegmentSec) : 0.0f;
   state.liveAnchored = 0;
   state.seeking = 0;
   state.pauseWhenReady = playSq < 0 ? 0 : isVideoPaused(state.player);
   state.lastFrame = NULL;
   state.liveLastFrameUs = 0;

   logInfo("[yt] LIVE rebuild at sq=%ld edge=%ld%s\\n",
           state.liveFirstSq, edge, playSq < 0 ? " (near live edge)" : "");
   finishGfx();
   state.liveRetiringPlayer = state.player;
   state.player = NULL;
   state.firstFrameSeen = 0;
   state.stage = STAGE_LOADING;
   state.workerDone = 0;
   state.threadActive = (spawnJoinableThread(&state.workerTid, liveReopenWorker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-live-seek") == 0);
   state.liveReopenThreadActive = state.threadActive;
   if (!state.threadActive) {
      state.player = state.liveRetiringPlayer;
      state.liveRetiringPlayer = NULL;
      state.stage = STAGE_PLAYING;
      state.firstFrameSeen = 1;
      state.liveReopenThreadActive = 0;
      return 0;
   }
   return 1;
}

static void nudgeLiveSeek(float deltaSeconds, uint64_t nowUs)
{
   if (!state.liveDvr) {
      showToast(LOC(STR_LIVE_REWIND_DISABLED));
      return;
   }
   if (!state.seeking) {
      state.seeking = 1;
      state.seekTarget = liveTimelinePosition();
   }
   state.seekTarget += deltaSeconds;
   float length = liveTimelineLength();
   if (state.seekTarget < 0.0f) state.seekTarget = 0.0f;
   if (state.seekTarget > length) state.seekTarget = length;
   state.lastSeekInputUs = nowUs;
   showControls();
}

static void handleLiveSeekInput(uint64_t nowUs)
{
   static ButtonRepeat seekRepeat;
   if (!state.liveDvr) {
      if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_RIGHT))
         showToast(LOC(STR_LIVE_REWIND_DISABLED));
      return;
   }

   if      (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_RIGHT))) nudgeLiveSeek(+SEEK_STEP_SECONDS, nowUs);
   else if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_LEFT)))  nudgeLiveSeek(-SEEK_STEP_SECONDS, nowUs);

   if (state.seeking && nowUs - state.lastSeekInputUs >= SEEK_APPLY_IDLE_US) {
      float targetSeconds = state.seekTarget;
      state.seeking = 0;
      float behind = liveTimelineLength() - targetSeconds;
      if (!state.liveEnded && isLiveBehindAtEdge(behind)) {
         if (!isLiveBehindAtEdge(liveTimelineLength() - liveTimelinePosition())) reopenLiveAt(-1);
      } else {
         long targetSq = (long)(liveWindowStartSq() + targetSeconds / (float)state.liveSegmentSec);
         if (targetSq < state.liveFloorSq) targetSq = state.liveFloorSq;
         if (targetSq > (long)liveEdgeNow()) targetSq = (long)liveEdgeNow();
         if (targetSq != (long)livePositionSq()) reopenLiveAt(targetSq);
      }
   }
}

static void cycleRemoteAudioTrack(void)
{
   if (state.audioCount <= 1) {
      showToast(state.audioCount == 1 ? LOC(STR_ONLY_ONE_AUDIO_TRACK) : LOC(STR_NO_ALTERNATE_AUDIO_TRACKS));
      return;
   }
   int target = state.audioPick < 0 ? 0 : (state.audioPick + 1) % state.audioCount;
   startAudioTrackSwitch(target);
}

static void cycleRemoteSubtitleTrack(void)
{
   if (state.isLive || state.captionCount <= 0) {
      showToast(LOC(STR_NO_SUBTITLES));
      return;
   }

   if (!subtitlesEnabled || state.subtitlePick < 0) {
      subtitlesEnabled = 1;
      setSubtitleTrack(0);
      showSelectedSubtitleToast();
      return;
   }

   if (state.subtitlePick + 1 < state.captionCount) {
      setSubtitleTrack(state.subtitlePick + 1);
      showSelectedSubtitleToast();
      return;
   }

   subtitlesEnabled = 0;
   setSubtitleTrack(-1);
   showToast(LOC(STR_SUBTITLES_OFF));
}

static void navigatePlaybackChapter(int direction)
{
   if (state.chapters.count == 0) {
      showToast(LOC(STR_NO_CHAPTERS));
      return;
   }

   float position = getVideoPositionSeconds(state.player);
   int actual = 0;
   for (int i = 0; i < state.chapters.count; i++)
      if (state.chapters.chapters[i].start <= position) actual = i;
   int current = chapterNavIndexValid ? chapterNavIndex : actual;
   int target = current + direction;
   if (target < 0 || target >= state.chapters.count) {
      showToast(direction < 0 ? LOC(STR_NO_PREVIOUS_CHAPTER) : LOC(STR_NO_NEXT_CHAPTER));
      return;
   }
   chapterNavIndex = target;
   chapterNavIndexValid = 1;
   chapterNavQueued = 1;
   state.seeking = 0;
   showControls();
}

static void handlePlaybackInput(void)
{
   if (state.audioSwitchThreadActive || state.resolutionSwitchThreadActive || state.liveReopenThreadActive)
      return; // keep the source stable until the staged handoff commits
   uint64_t nowUs = sys_time_get_system_time();

   // Chapter navigation coalesces rapid taps into one destination. A new decoder seek is
   // posted only after the previous seek has produced its first presented frame.
   if (chapterNavPlayer != state.player) {
      chapterNavPlayer = state.player;
      chapterNavIndex = -1;
      chapterNavIndexValid = chapterNavQueued = chapterNavInFlight = 0;
   }
   if (chapterNavInFlight && !isVideoSeekPending(state.player)) chapterNavInFlight = 0;

   // Keep the logical chapter selected until playback actually reaches its start.
   // Normal seeks land on the preceding keyframe, so the reported position can
   // remain in the previous chapter for a short time after the seek has completed.
   if (chapterNavIndexValid && !chapterNavQueued && !chapterNavInFlight &&
       !isVideoSeekPending(state.player) && state.chapters.count > 0) {
      float settledPosition = getVideoPositionSeconds(state.player);
      int settledChapter = 0;
      for (int i = 0; i < state.chapters.count; i++)
         if (state.chapters.chapters[i].start <= settledPosition) settledChapter = i;
      if (settledChapter >= chapterNavIndex) chapterNavIndexValid = 0;
   }

   if (chapterNavQueued && !chapterNavInFlight && !isVideoSeekPending(state.player) &&
       state.chapters.count > 0 && chapterNavIndex >= 0 && chapterNavIndex < state.chapters.count) {
      seekVideoPlayer(state.player, state.chapters.chapters[chapterNavIndex].start);
      chapterNavQueued = 0;
      chapterNavInFlight = 1;
   }

   if (isPadButtonPressed(PAD_BTN_L2)) {
      refreshPlaybackInfoHud();
      showFullPlaybackHud();
   }

   updateCropPan();

   // PLAY and PAUSE keep their legacy CROSS alias for every other screen and
   // playback overlay. Here, while normal playback owns input, treat them as
   // independent transport commands and suppress the CROSS toggle they alias.
   PadButtonState remotePlayState = getPadRemoteButtonState(PAD_REMOTE_PLAY);
   PadButtonState remotePauseState = getPadRemoteButtonState(PAD_REMOTE_PAUSE);
   int remoteTransportActive = remotePlayState != PAD_BUTTON_STATE_UP ||
                               remotePauseState != PAD_BUTTON_STATE_UP;

   if (remotePlayState == PAD_BUTTON_STATE_PRESSED && isVideoPaused(state.player)) {
      setVideoPaused(state.player, 0);
      showControls();
   } else if (remotePauseState == PAD_BUTTON_STATE_PRESSED && !isVideoPaused(state.player)) {
      setVideoPaused(state.player, 1);
      showFullPlaybackHud();
   }

   if (state.isLive) {
      if (!remoteTransportActive && isPadButtonPressed(PAD_BTN_CROSS)) {
         setVideoPaused(state.player, !isVideoPaused(state.player));
         if (isVideoPaused(state.player)) showFullPlaybackHud();
         else showControls();
      }
   } else {
      static uint64_t crossHoldStartUs;
      static int crossRestartTriggered;
      PadButtonState crossState = remoteTransportActive ? PAD_BUTTON_STATE_UP :
                                  getPadButtonState(PAD_BTN_CROSS);

      if (crossState == PAD_BUTTON_STATE_PRESSED) {
         crossHoldStartUs = nowUs;
         crossRestartTriggered = 0;
      } else if (crossState == PAD_BUTTON_STATE_HELD && crossHoldStartUs &&
                 !crossRestartTriggered && nowUs - crossHoldStartUs >= CROSS_RESTART_HOLD_US) {
         crossRestartTriggered = 1;
         state.seeking = 0;
         seekVideoPlayer(state.player, 0.0f);
         showControls();
      } else if (crossState == PAD_BUTTON_STATE_RELEASED) {
         if (suppressNextCrossRelease) {
            // The screensaver handler already resumed playback on the press edge.
            suppressNextCrossRelease = 0;
         } else if (!crossRestartTriggered) {
            if (isVideoEnded(state.player)) {
               seekVideoPlayer(state.player, 0.0f);
               showControls();
            } else {
               setVideoPaused(state.player, !isVideoPaused(state.player));
               if (isVideoPaused(state.player)) showFullPlaybackHud();
               else showControls();
            }
         }
         crossHoldStartUs = 0;
         crossRestartTriggered = 0;
      }
   }

   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      int askIndex = findAskSponsorSegment();
      if (askIndex >= 0) {
         SponsorSegment *segment = &state.sb.segments[askIndex];
         segment->skipped = 1;
         state.askPromptShown = -1;
         state.askPromptStartedUs = 0;
         seekVideoPlayerPast(state.player, segment->end);
         char notice[64];
         snprintf(notice, sizeof notice, LOC(STR_SKIPPED_FORMAT), getSponsorCategoryName(segment->category));
         showToast(notice);
      }
   }

   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) toggleSubtitles();
   if (isPadRemoteButtonPressed(PAD_REMOTE_AUDIO)) cycleRemoteAudioTrack();
   if (isPadRemoteButtonPressed(PAD_REMOTE_SUBTITLE)) cycleRemoteSubtitleTrack();
   if (getPadRemoteButtonState(PAD_REMOTE_SLOW_REV) == PAD_BUTTON_STATE_RELEASED)
      navigatePlaybackChapter(-1);
   if (getPadRemoteButtonState(PAD_REMOTE_SLOW_FWD) == PAD_BUTTON_STATE_RELEASED)
      navigatePlaybackChapter(1);

   // DualShock L1/R1: short press changes chapter; two-second hold changes video.
   // BD Remote PREV/NEXT trigger the same video-navigation action immediately.
   PadButtonState l1State = getPadButtonState(PAD_BTN_L1);
   PadButtonState r1State = getPadButtonState(PAD_BTN_R1);
   PadButtonState remotePrevState = getPadRemoteButtonState(PAD_REMOTE_PREV);
   PadButtonState remoteNextState = getPadRemoteButtonState(PAD_REMOTE_NEXT);
   if (remotePrevState == PAD_BUTTON_STATE_PRESSED) {
      l1State = PAD_BUTTON_STATE_HELD;
      l1HoldStartUs = nowUs - PLAYBACK_NAV_HOLD_US;
      l1HoldTriggered = 0;
   } else if (remotePrevState == PAD_BUTTON_STATE_HELD) {
      l1State = PAD_BUTTON_STATE_HELD;
   } else if (remotePrevState == PAD_BUTTON_STATE_RELEASED) {
      l1State = PAD_BUTTON_STATE_RELEASED;
   }
   if (remoteNextState == PAD_BUTTON_STATE_PRESSED) {
      r1State = PAD_BUTTON_STATE_HELD;
      r1HoldStartUs = nowUs - PLAYBACK_NAV_HOLD_US;
      r1HoldTriggered = 0;
   } else if (remoteNextState == PAD_BUTTON_STATE_HELD) {
      r1State = PAD_BUTTON_STATE_HELD;
   } else if (remoteNextState == PAD_BUTTON_STATE_RELEASED) {
      r1State = PAD_BUTTON_STATE_RELEASED;
   }
   if (l1State == PAD_BUTTON_STATE_PRESSED) { l1HoldStartUs = nowUs; l1HoldTriggered = 0; }
   if (r1State == PAD_BUTTON_STATE_PRESSED) { r1HoldStartUs = nowUs; r1HoldTriggered = 0; }

   if (l1State == PAD_BUTTON_STATE_HELD && l1HoldStartUs && !l1HoldTriggered && nowUs - l1HoldStartUs >= PLAYBACK_NAV_HOLD_US) {
      l1HoldTriggered = 1;
      if (requestedIsLocal) {
         if (downloadedQueueCount > 0 && downloadedQueueCursor > 0) { launchDownloadedQueueIndex(downloadedQueueCursor - 1); return; }
         else showToast(LOC(STR_NO_PREVIOUS_DOWNLOAD));
      } else if (playlistQueueCount > 0 && playlistQueueCursor > 0) {
         launchPlaylistQueueIndex(playlistQueueCursor - 1);
         return;
      } else {
         if (playlistQueueCount > 0) {
            playlistQueueCount = 0;
            playlistQueueCursor = -1;
         }
         char previousId[12];
         if (previousPlaybackId(previousId)) { requestPlaybackCancel(); popScreen(); launchVideo(previousId, 0); return; }
         showToast(LOC(STR_NO_PREVIOUS_VIDEO));
      }
   }
   if (r1State == PAD_BUTTON_STATE_HELD && r1HoldStartUs && !r1HoldTriggered && nowUs - r1HoldStartUs >= PLAYBACK_NAV_HOLD_US) {
      r1HoldTriggered = 1;
      if (requestedIsLocal) {
         if (downloadedQueueCount > 0 && downloadedQueueCursor + 1 < downloadedQueueCount) { launchDownloadedQueueIndex(downloadedQueueCursor + 1); return; }
         else showToast(LOC(STR_NO_NEXT_DOWNLOAD));
      } else if (playlistQueueCount > 0 && playlistQueueCursor + 1 < playlistQueueCount) {
         launchPlaylistQueueIndex(playlistQueueCursor + 1);
         return;
      } else {
         char nextId[12];
         if (state.nextThreadActive || state.nextNoticeVisible) showToast(LOC(STR_NEXT_VIDEO_ALREADY_LOADING));
         else if (playlistQueueCount == 0 && nextPlaybackId(nextId)) { requestPlaybackCancel(); popScreen(); launchVideo(nextId, 0); return; }
         else {
            state.nextAttempted = 1;
            state.nextWorkerDone = 0;
            state.nextVideoId[0] = 0;
            state.nextVideoTitle[0] = 0;
            state.nextThreadActive = (spawnJoinableThread(&state.nextWorkerTid, autoplayWorker, 0,
                                      THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-next") == 0);
            if (!state.nextThreadActive) showToast(LOC(STR_NEXT_VIDEO_UNAVAILABLE));
            else showToast(LOC(STR_LOADING_NEXT_RECOMMENDATION));
         }
      }
   }

   if (l1State == PAD_BUTTON_STATE_RELEASED) {
      if (!l1HoldTriggered) {
         if (state.chapters.count == 0) showToast(LOC(STR_NO_CHAPTERS));
         else {
            float position = getVideoPositionSeconds(state.player);
            int actual = 0;
            for (int i = 0; i < state.chapters.count; i++) if (state.chapters.chapters[i].start <= position) actual = i;
            int current = chapterNavIndexValid ? chapterNavIndex : actual;
            if (current > 0) {
               chapterNavIndex = current - 1;
               chapterNavIndexValid = 1;
               chapterNavQueued = 1;
               state.seeking = 0;
               showControls();
            } else showToast(LOC(STR_NO_PREVIOUS_CHAPTER));
         }
      }
      l1HoldStartUs = 0; l1HoldTriggered = 0;
   }
   if (r1State == PAD_BUTTON_STATE_RELEASED) {
      if (!r1HoldTriggered) {
         if (state.isLive) {
            // A short R1 press on a DVR live stream returns to the moving live edge;
            // chapter navigation is reserved for VOD. Discard any pending timeline scrub.
            state.seeking = 0;
            float behind = liveTimelineLength() - liveTimelinePosition();
            if (!isLiveBehindAtEdge(behind)) reopenLiveAt(-1);
            else showControls();
         } else if (state.chapters.count == 0) showToast(LOC(STR_NO_CHAPTERS));
         else {
            float position = getVideoPositionSeconds(state.player);
            int actual = 0;
            for (int i = 0; i < state.chapters.count; i++) if (state.chapters.chapters[i].start <= position) actual = i;
            int current = chapterNavIndexValid ? chapterNavIndex : actual;
            if (current + 1 < state.chapters.count) {
               chapterNavIndex = current + 1;
               chapterNavIndexValid = 1;
               chapterNavQueued = 1;
               state.seeking = 0;
               showControls();
            } else showToast(LOC(STR_NO_NEXT_CHAPTER));
         }
      }
      r1HoldStartUs = 0; r1HoldTriggered = 0;
   }

   // volume: the widget steps itself on up/down; apply + persist the new level (works on live streams too)
   if (handleVolumeMeterInput(&volumeMeter)) {
      volumeLevel = volumeMeter.level;
      setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
   }

   if (state.isLive) { handleLiveSeekInput(nowUs); return; }

   static ButtonRepeat seekRepeat;
   static uint64_t seekHoldStartUs;
   static int seekDirection;
   PadButtonState rightState = getPadButtonState(PAD_BTN_RIGHT);
   PadButtonState leftState = getPadButtonState(PAD_BTN_LEFT);
   PadButtonState activeState = PAD_BUTTON_STATE_UP;
   int direction = 0;

   if (rightState == PAD_BUTTON_STATE_PRESSED || rightState == PAD_BUTTON_STATE_HELD) {
      direction = 1;
      activeState = rightState;
   } else if (leftState == PAD_BUTTON_STATE_PRESSED || leftState == PAD_BUTTON_STATE_HELD) {
      direction = -1;
      activeState = leftState;
   }

   if (!direction) {
      seekDirection = 0;
      seekHoldStartUs = 0;
   } else {
      int directionChanged = (direction != seekDirection);
      if (directionChanged) {
         seekDirection = direction;
         seekHoldStartUs = nowUs;
         if (activeState == PAD_BUTTON_STATE_HELD) {
            // Switching direction while held starts a fresh acceleration ramp and nudges immediately.
            seekRepeat.timer = nowUs;
            seekRepeat.repeats = 0;
            nudgeSeek(direction * SEEK_STEP_SECONDS, nowUs);
         }
      }

      int repeatDue = 0;
      if (activeState == PAD_BUTTON_STATE_PRESSED || activeState == PAD_BUTTON_STATE_HELD)
         repeatDue = isRepeatDue(&seekRepeat, activeState);

      if (repeatDue) {
         uint64_t heldUs = nowUs - seekHoldStartUs;
         float step = SEEK_STEP_SECONDS;
         if (heldUs >= SEEK_ACCEL_3_US)      step = 60.0f;
         else if (heldUs >= SEEK_ACCEL_2_US) step = 30.0f;
         else if (heldUs >= SEEK_ACCEL_1_US) step = 15.0f;
         nudgeSeek(direction * step, nowUs);
      }
   }

   if (state.seeking && nowUs - state.lastSeekInputUs >= SEEK_APPLY_IDLE_US) {
      seekVideoPlayer(state.player, state.seekTarget);
      state.seeking = 0;
   }
}

// Each category has its own action. Ask segments can be skipped manually with Square.
static int shouldAutoSkipSponsor(SponsorCategory category)
{
   return getSponsorAction((int)category) == SPONSOR_ACTION_SKIP;
}

static int findAskSponsorSegment(void)
{
   if (!state.sbReady || !state.player) return -1;
   float position = getVideoPositionSeconds(state.player);
   for (int i = 0; i < state.sb.count; i++) {
      SponsorSegment *segment = &state.sb.segments[i];
      if (!segment->skipped && getSponsorAction((int)segment->category) == SPONSOR_ACTION_ASK &&
          position >= segment->start && position < segment->end - 0.2f) return i;
   }
   return -1;
}

// jump past the first not-yet-skipped sponsor segment the playhead has entered. one-shot per segment so a
// manual rewind into it doesn't fight the user.
static void autoSkipSponsor(void)
{
   float pos = getVideoPositionSeconds(state.player);
   for (int i = 0; i < state.sb.count; i++) {
      SponsorSegment *segment = &state.sb.segments[i];
      if (!shouldAutoSkipSponsor(segment->category)) continue;
      if (segment->skipped || pos < segment->start || pos >= segment->end - 0.2f) continue;
      segment->skipped = 1;
      seekVideoPlayerPast(state.player, segment->end);   // land on the next keyframe after the segment, not before it
      char notice[64];
      snprintf(notice, sizeof notice, LOC(STR_SKIPPED_FORMAT), getSponsorCategoryName(segment->category));
      showToast(notice);
      return;
   }
}

// Compose both sections into one texture so metadata and description share the same font scale,
// width and scrolling behaviour while remaining visually distinct through section headings.

// Collapse runs of whitespace-only description lines to at most two empty lines.
// This keeps YouTube descriptions with pathological vertical whitespace renderable.
static void collapseDescriptionBlankLines(char *text)
{
   if (!text || !*text) return;

   char *read = text;
   char *write = text;
   int blankLines = 0;

   while (*read) {
      char *line = read;
      while (*read && *read != '\r' && *read != '\n') read++;

      char *end = read;
      int hasNewline = (*read != 0);
      int blank = 1;
      for (char *p = line; p < end; p++) {
         if (*p != ' ' && *p != '\t') {
            blank = 0;
            break;
         }
      }

      if (blank) {
         if (blankLines < 2 && hasNewline) {
            *write++ = '\n';
            blankLines++;
         }
      } else {
         size_t length = (size_t)(end - line);
         memmove(write, line, length);
         write += length;
         blankLines = 0;
         if (hasNewline) *write++ = '\n';
      }

      if (*read == '\r') read++;
      if (*read == '\n') read++;
   }

   *write = 0;
}

// The full description remains in its existing owner (state.description). Only
// a small metadata header and one bounded page buffer are added here.
static char descriptionContentCharAt(size_t offset)
{
   if (offset < descriptionMetadataLength) return descriptionMetadata[offset];
   offset -= descriptionMetadataLength;
   return offset < descriptionBodyLength ? descriptionBody[offset] : 0;
}

static void copyDescriptionContent(size_t offset, char *destination, size_t length)
{
   size_t copied = 0;
   if (offset < descriptionMetadataLength) {
      size_t part = descriptionMetadataLength - offset;
      if (part > length) part = length;
      memcpy(destination, descriptionMetadata + offset, part);
      copied += part;
      offset = 0;
   } else {
      offset -= descriptionMetadataLength;
   }

   if (copied < length && offset < descriptionBodyLength) {
      size_t part = descriptionBodyLength - offset;
      if (part > length - copied) part = length - copied;
      memcpy(destination + copied, descriptionBody + offset, part);
   }
}

// Return the next bounded page length. A page is limited by both byte count and
// explicit line breaks, so long unbroken lines and many short lines stay bounded.
static size_t descriptionNextPageLength(size_t offset, size_t remaining)
{
   if (remaining == 0) return 0;

   size_t limit = remaining < descriptionPageMaxBytes
                ? remaining : descriptionPageMaxBytes;
   size_t end = 0;
   int newlines = 0;

   while (end < limit) {
      char c = descriptionContentCharAt(offset + end++);
      if (c == '\n' && ++newlines >= descriptionPageMaxNewlines)
         break;
   }

   if (end < remaining && end == limit) {
      // Prefer a word boundary near the end. Keep the whitespace in this page.
      size_t minimum = limit / 2;
      for (size_t i = end; i > minimum; i--) {
         char c = descriptionContentCharAt(offset + i - 1);
         if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            end = i;
            break;
         }
      }
   }

   // Never split a UTF-8 code point across pages.
   if (end < remaining) {
      while (end > 0 &&
             (((unsigned char)descriptionContentCharAt(offset + end)) & 0xC0) == 0x80)
         end--;
   }
   if (end == 0) end = limit < remaining ? limit : remaining;
   return end;
}

static void clearDescriptionPages(void)
{
   descriptionBody = NULL;
   descriptionBodyLength = 0;
   descriptionMetadata[0] = 0;
   descriptionMetadataLength = 0;
   descriptionContentLength = 0;
   descriptionPageCount = 0;
   freeTextTexture(&descriptionTexture);
}

static void renderDescriptionPage(int page)
{
   if (descriptionContentLength == 0 || descriptionPageCount <= 0) return;
   if (page < 0) page = 0;
   if (page >= descriptionPageCount) page = descriptionPageCount - 1;

   size_t offset = 0;
   for (int i = 0; i < page && offset < descriptionContentLength; i++)
      offset += descriptionNextPageLength(offset, descriptionContentLength - offset);

   if (offset >= descriptionContentLength) return;
   size_t pageLength = descriptionNextPageLength(offset, descriptionContentLength - offset);
   char *pageText = (char *)malloc(pageLength + 1);
   if (!pageText) {
      logError("[yt-description] unable to allocate page buffer bytes=%u\n",
               (unsigned)(pageLength + 1));
      freeTextTexture(&descriptionTexture);
      renderFontRaw(&descriptionTexture, &uiFont,
                    (int)(playbackFontSize(DESCRIPTION_TEXT_SIZE) * playbackFontScale + 0.5f),
                    LOC(STR_DESCRIPTION_PAGE_UNAVAILABLE),
                    activeTheme->textPrimary,
                    state.screenW - 2 * DESCRIPTION_MARGIN_X, TEXT_WRAP);
      return;
   }
   copyDescriptionContent(offset, pageText, pageLength);
   pageText[pageLength] = 0;

   // Only one description page owns a GPU texture at a time.
   freeTextTexture(&descriptionTexture);
   renderFontRaw(&descriptionTexture, &uiFont,
                 (int)(playbackFontSize(DESCRIPTION_TEXT_SIZE) * playbackFontScale + 0.5f),
                 pageText, activeTheme->textPrimary,
                 state.screenW - 2 * DESCRIPTION_MARGIN_X, TEXT_WRAP);
   free(pageText);

   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   if (descriptionTexture.tex.h > viewHeight && descriptionPageMaxBytes > 16) {
      // If the actual font metrics exceed the estimate, tighten the page budget
      // and rebuild the index. Keep the current text position visible.
      size_t targetOffset = offset;
      // Tighten only by the measured overflow ratio. Halving the budget on
      // even a small overflow left most pages using only half the screen.
      float fitRatio = (float)viewHeight / (float)descriptionTexture.tex.h;
      fitRatio *= 0.92f; // leave a small margin for rasterizer rounding
      if (fitRatio < 0.25f) fitRatio = 0.25f;
      if (fitRatio > 0.90f) fitRatio = 0.90f;
      size_t adjustedBytes = (size_t)(descriptionPageMaxBytes * fitRatio);
      if (adjustedBytes < 16) adjustedBytes = 16;
      descriptionPageMaxBytes = adjustedBytes;
      int adjustedNewlines = (int)(descriptionPageMaxNewlines * fitRatio);
      if (adjustedNewlines < 1) adjustedNewlines = 1;
      if (adjustedNewlines >= descriptionPageMaxNewlines &&
          descriptionPageMaxNewlines > 1)
         adjustedNewlines = descriptionPageMaxNewlines - 1;
      descriptionPageMaxNewlines = adjustedNewlines;
      recountDescriptionPages();

      size_t scanOffset = 0;
      int targetPage = 0;
      while (scanOffset < targetOffset && targetPage + 1 < descriptionPageCount) {
         size_t length = descriptionNextPageLength(scanOffset,
                                                    descriptionContentLength - scanOffset);
         if (!length) break;
         scanOffset += length;
         targetPage++;
      }
      state.descriptionPage = targetPage;
      renderDescriptionPage(targetPage);
      return;
   }

   if (!descriptionTexture.tex.w || !descriptionTexture.tex.h) {
      logError("[yt-description] page rasterization failed page=%d/%d bytes=%u\n",
               page + 1, descriptionPageCount, (unsigned)pageLength);
      renderFontRaw(&descriptionTexture, &uiFont,
                    (int)(playbackFontSize(DESCRIPTION_TEXT_SIZE) * playbackFontScale + 0.5f),
                    LOC(STR_DESCRIPTION_PAGE_RENDER_FAILED),
                    activeTheme->textPrimary,
                    state.screenW - 2 * DESCRIPTION_MARGIN_X, TEXT_WRAP);
   }

   logInfo("[yt-description] page=%d/%d bytes=%u texture=%dx%d\n",
           page + 1, descriptionPageCount, (unsigned)pageLength,
           descriptionTexture.tex.w, descriptionTexture.tex.h);
}

static void recountDescriptionPages(void)
{
   descriptionPageCount = 0;
   size_t offset = 0;
   while (offset < descriptionContentLength) {
      size_t pageLength = descriptionNextPageLength(offset, descriptionContentLength - offset);
      if (!pageLength) break;
      offset += pageLength;
      descriptionPageCount++;
   }
   if (descriptionPageCount <= 0) descriptionPageCount = 1;
}

static void renderDescription(void)
{
   char views[48];
   const char *rawViews = state.viewCount[0] ? state.viewCount : LOC(STR_UNAVAILABLE);
   // Add thousands separators to the numeric view count without changing the source value.
   int digits = (int)strlen(rawViews), commas = digits > 3 ? (digits - 1) / 3 : 0;
   int out = 0;
   for (int i = 0; i < digits && out < (int)sizeof views - 1; i++) {
      views[out++] = rawViews[i];
      if (rawViews[i] >= '0' && rawViews[i] <= '9' && i + 1 < digits &&
          (digits - i - 1) % 3 == 0 && rawViews[i + 1] >= '0' && rawViews[i + 1] <= '9' && commas > 0) {
         views[out++] = ',';
         commas--;
      }
   }
   views[out] = 0;

   const char *description = state.description && state.description[0]
                           ? state.description : LOC(STR_NO_DESCRIPTION);
   clearDescriptionPages();
   const char *likesText = state.likeCount[0] ? state.likeCount : LOC(STR_LOADING_ELLIPSIS);
   if (strcmp(likesText, "Offline") == 0) likesText = LOC(STR_OFFLINE);
   int headerLength = snprintf(descriptionMetadata, sizeof descriptionMetadata,
                               LOC(STR_DESCRIPTION_METADATA_FORMAT),
                               views, state.publishedDate[0] ? state.publishedDate : LOC(STR_UNAVAILABLE),
                               likesText);
   if (headerLength < 0) headerLength = 0;
   if (headerLength >= (int)sizeof descriptionMetadata)
      headerLength = (int)sizeof descriptionMetadata - 1;

   descriptionMetadataLength = (size_t)headerLength;
   descriptionBody = description;
   descriptionBodyLength = strlen(descriptionBody);
   descriptionContentLength = descriptionMetadataLength + descriptionBodyLength;

   int fontSize = (int)(playbackFontSize(DESCRIPTION_TEXT_SIZE) * playbackFontScale + 0.5f);
   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   int maxWidth = state.screenW - 2 * DESCRIPTION_MARGIN_X;
   int safeLines = fontSize > 0 ? (int)(viewHeight / (fontSize * 1.35f)) - 1 : 4;
   if (safeLines < 3) safeLines = 3;

   // Use almost the full available vertical space. The previous half-line
   // budget and 256-byte ceiling forced ordinary descriptions into tiny pages.
   descriptionPageMaxNewlines = safeLines - 1;
   if (descriptionPageMaxNewlines < 2) descriptionPageMaxNewlines = 2;

   // Average glyph width is smaller than the font height. This is an initial
   // estimate; the rasterized texture height is checked and corrected below.
   int charsPerLine = fontSize > 0 ? (maxWidth * 2) / fontSize : 32;
   if (charsPerLine < 8) charsPerLine = 8;
   int pageBytes = charsPerLine * (safeLines - 1);
   if (pageBytes < 512) pageBytes = 512;
   if (pageBytes > 4096) pageBytes = 4096;
   descriptionPageMaxBytes = (size_t)pageBytes;

   recountDescriptionPages();

   if (state.descriptionPage >= descriptionPageCount) state.descriptionPage = descriptionPageCount - 1;
   renderDescriptionPage(state.descriptionPage);
   state.descriptionRendered = 1;
}

// SELECT opens the metadata + description panel. Likes are fetched asynchronously on first open;
// the request never delays playback startup or blocks drawing/input.
static void openDescription(void)
{
   if (!state.metadataFetchTried) {
      state.metadataFetchTried = 1;
      if (requestedIsLocal) {
         strCopy(state.likeCount, sizeof state.likeCount, LOC(STR_OFFLINE));
      } else {
      state.likeCount[0] = 0;
      state.metadataWorkerDone = 0;
      state.metadataThreadActive = (spawnJoinableThread(&state.metadataWorkerTid, metadataWorker, 0,
                                     THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-metadata") == 0);
      if (!state.metadataThreadActive)
         strCopy(state.likeCount, sizeof state.likeCount, LOC(STR_UNAVAILABLE));
      }
   }
   if (!state.descriptionRendered) {
      state.descriptionPage = 0;
      renderDescription();
   } else if (state.descriptionPage != 0) {
      state.descriptionPage = 0;
      renderDescriptionPage(0);
   }
   state.descriptionPage = 0;
   state.descriptionVisible = 1;
}

// while the description is up it owns the pad: SELECT/circle close it, up/down scroll. playback
// input (seek, pause, back-out) is blocked underneath.
static void handleDescriptionInput(void)
{
   if (isPadButtonPressed(PAD_BTN_SELECT) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      state.descriptionVisible = 0;
      return;
   }

   int nextPage = state.descriptionPage;
   if (isPadButtonPressed(PAD_BTN_DOWN) || isPadButtonPressed(PAD_BTN_RIGHT)) {
      if (nextPage + 1 < descriptionPageCount) nextPage++;
   } else if (isPadButtonPressed(PAD_BTN_UP) || isPadButtonPressed(PAD_BTN_LEFT)) {
      if (nextPage > 0) nextPage--;
   }
   if (nextPage != state.descriptionPage) {
      state.descriptionPage = nextPage;
      renderDescriptionPage(state.descriptionPage);
   }
}

// START opens the chapter picker: the rows are rasterised once (off the draw path), the widest time
// and title size the two columns, and the selection starts on the chapter the playhead is in.
static void openChapters(void)
{
   // Avoid fetching YouTube's large /next response on every playback. Fetch only when the user asks
   // for chapters; the request stays off the draw path and can be cancelled cleanly on exit.
   if (state.isLive) { showToast(LOC(STR_NO_CHAPTERS)); return; }
   if (state.chapters.count == 0) {
      if (!state.chFetchTried && isBareVideoId(requestedInput)) {
         state.chFetchTried = 1;
         state.chWorkerDone = 0;
         state.chThreadActive = (spawnJoinableThread(&state.chWorkerTid, chapterWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-chapters") == 0);
      }
      showToast(state.chThreadActive ? LOC(STR_LOADING_CHAPTERS) : LOC(STR_NO_CHAPTERS));
      return;
   }

   if (!state.chaptersRendered) {
      int widestTitle = 0;
      for (int i = 0; i < state.chapters.count; i++) {
         char timeText[16];
         formatTime(timeText, sizeof timeText, (int)state.chapters.chapters[i].start);
         setLabelText(&chapterTimeLabels[i], timeText);
         setLabelText(&chapterLabels[i], state.chapters.chapters[i].title);
         if (chapterTimeLabels[i].tt.tex.w > state.chapterTimeWidth) state.chapterTimeWidth = chapterTimeLabels[i].tt.tex.w;
         if (chapterLabels[i].tt.tex.w > widestTitle) widestTitle = chapterLabels[i].tt.tex.w;
      }
      state.chapterPanelWidth = state.chapterTimeWidth + CHAPTER_TIME_GAP + widestTitle;
      state.chaptersRendered = 1;
   }

   float position = getVideoPositionSeconds(state.player);
   state.chapterSelected = 0;
   for (int i = 0; i < state.chapters.count; i++)
      if (state.chapters.chapters[i].start <= position) state.chapterSelected = i;
   state.chaptersVisible = 1;
   chapterNavigateRepeat.timer = chapterNavigateRepeat.repeats = 0;
}

// while the picker is up it owns the pad: up/down move the selection, cross jumps to the chapter
// (playback carries on paused or playing, only the position moves), circle/START close without
// jumping. playback input is blocked underneath.
static void handleChaptersInput(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) { state.chaptersVisible = 0; return; }

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      state.chaptersVisible = 0;
      suppressPlaybackCrossUntilRelease = 1; // consume the matching RELEASED event too
      state.seeking = 0;   // a pending scrub must not overwrite the jump
      chapterNavIndexValid = chapterNavQueued = chapterNavInFlight = 0;
      seekVideoPlayer(state.player, state.chapters.chapters[state.chapterSelected].start);
      showControls();
      return;
   }

   if (isRepeatDue(&chapterNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))    state.chapterSelected++;
   else if (isRepeatDue(&chapterNavigateRepeat, getPadButtonState(PAD_BTN_UP))) state.chapterSelected--;
   if (state.chapters.count > 0) {
      if (state.chapterSelected < 0) state.chapterSelected = state.chapters.count - 1;
      if (state.chapterSelected >= state.chapters.count) state.chapterSelected = 0;
   }
}

static void openPlaybackOptions(void)
{
   optionsVisible = 1;
   optionsDropdownOpen = 0;
   optionsSelected = 0;
   optionsDropdownSelected = 0;
   optionsNavigateRepeat.timer = optionsNavigateRepeat.repeats = 0;
   optionsDropdownRepeat.timer = optionsDropdownRepeat.repeats = 0;

   // Prefetch chapters in the background; opening Options never opens the chapter picker.
   if (!state.isLive && state.chapters.count == 0 && !state.chFetchTried &&
       isBareVideoId(requestedInput)) {
      state.chFetchTried = 1;
      state.chWorkerDone = 0;
      state.chThreadActive = (spawnJoinableThread(&state.chWorkerTid, chapterWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-chapters") == 0);
   }
}

static int autoTranslateMenuCount(void)
{
   return getAutoTranslateLanguageCount() + 2; // Off and Auto (Content Region)
}

static int autoTranslateMenuIndexForPreference(const char *preference)
{
   if (!preference || strcmp(preference, "off") == 0) return 0;
   if (strcmp(preference, "auto") == 0) return 1;
   for (int i = 0; i < getAutoTranslateLanguageCount(); i++) {
      const AutoTranslateLanguage *language = getAutoTranslateLanguageAt(i);
      if (language && strcmp(language->code, preference) == 0) return i + 2;
   }
   return 1;
}

static void refreshAutoTranslatedCaptionTrack(void)
{
   if (state.subThreadActive) {
      autoTranslateRefreshPending = 1;
      if (state.subtitlePick >= 0 && state.subtitlePick < state.captionCount &&
          subtitleIsAutoTranslated(&state.captionTracks[state.subtitlePick]))
         state.subtitleTranslation.cancel = 1;
      return;
   }

   int selected = state.subtitlePick;
   int selectedWasTranslated = selected >= 0 && selected < state.captionCount &&
                               subtitleIsAutoTranslated(&state.captionTracks[selected]);
   int rememberedWasTranslated = lastSubtitlePick >= 0 && lastSubtitlePick < state.captionCount &&
                                 subtitleIsAutoTranslated(&state.captionTracks[lastSubtitlePick]);
   if (selectedWasTranslated) {
      setSubtitleTrack(-1);
      state.subtitleFetched = -1;
      state.subtitlesReady = 0;
      free(state.subtitleTrack);
      state.subtitleTrack = NULL;
   }
   if (selectedWasTranslated || rememberedWasTranslated) lastSubtitlePick = -1;

   for (int i = 0; i < state.captionCount; ) {
      if (subtitleIsAutoTranslated(&state.captionTracks[i])) {
         memmove(&state.captionTracks[i], &state.captionTracks[i + 1],
                 (size_t)(state.captionCount - i - 1) * sizeof *state.captionTracks);
         state.captionCount--;
      } else {
         i++;
      }
   }

   appendAutoTranslatedCaptionTrack(getAutoTranslateLanguage(), requestedIsLocal);
   autoTranslateRefreshPending = 0;

   if (selectedWasTranslated && subtitlesEnabled) {
      int next = findPreferredSubtitleTrack(getPlaybackSubtitleLanguage());
      if (next >= 0) setSubtitleTrack(next);
   }
}

static void openPlaybackAutoTranslateSettings(void)
{
   playbackAutoTranslateOpen = 1;
   playbackAutoTranslateSelected = autoTranslateMenuIndexForPreference(getAutoTranslateLanguagePreference());
   playbackAutoTranslateRepeat.timer = playbackAutoTranslateRepeat.repeats = 0;
}

static void updatePlaybackAutoTranslateSettings(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_LEFT)) {
      playbackAutoTranslateOpen = 0;
      return;
   }
   int count = autoTranslateMenuCount();
   if (isRepeatDue(&playbackAutoTranslateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      playbackAutoTranslateSelected = (playbackAutoTranslateSelected + 1) % count;
   else if (isRepeatDue(&playbackAutoTranslateRepeat, getPadButtonState(PAD_BTN_UP)))
      playbackAutoTranslateSelected = (playbackAutoTranslateSelected + count - 1) % count;

   if (!isPadButtonPressed(PAD_BTN_CROSS)) return;
   const char *languageCode = NULL;
   if (playbackAutoTranslateSelected == 0) languageCode = "off";
   else if (playbackAutoTranslateSelected == 1) languageCode = "auto";
   else {
      const AutoTranslateLanguage *language =
         getAutoTranslateLanguageAt(playbackAutoTranslateSelected - 2);
      if (language) languageCode = language->code;
   }
   const char *previousLanguage = getAutoTranslateLanguagePreference();
   int languageChanged = !previousLanguage || strcmp(previousLanguage, languageCode ? languageCode : "") != 0;
   if (!languageCode || setAutoTranslateLanguage(languageCode) != 0) {
      showToast(LOC(STR_COULD_NOT_SAVE_AUTO_TRANSLATE));
   } else {
      if (languageChanged) {
         autoTranslateRefreshPending = 1;
         refreshAutoTranslatedCaptionTrack();
      }
      if (strcmp(languageCode, "off") == 0 || strcmp(languageCode, "auto") == 0) {
         char notice[128];
         snprintf(notice, sizeof notice, LOC(STR_AUTO_TRANSLATE_FORMAT),
                  strcmp(languageCode, "off") == 0 ? LOC(STR_OFF) : LOC(STR_AUTO_CONTENT_REGION));
         showToast(notice);
      }
      else {
         const char *name = getAutoTranslateLanguageName(languageCode);
         char notice[96];
         snprintf(notice, sizeof notice, LOC(STR_AUTO_TRANSLATE_FORMAT), name ? name : languageCode);
         showToast(notice);
      }
   }
   playbackAutoTranslateOpen = 0;
}

static void openPlaybackSettings(void)
{
   playbackSettingsOpen = 1;
   playbackSponsorOpen = 0;
   playbackSubtitleStyleOpen = 0;
   playbackAutoTranslateOpen = 0;
   playbackSettingsSelected = 0;
   playbackSettingsRepeat.timer = playbackSettingsRepeat.repeats = 0;
   playbackOverscanRepeat.timer = playbackOverscanRepeat.repeats = 0;
   playbackSubtitleOffsetRepeat.timer = playbackSubtitleOffsetRepeat.repeats = 0;
}

static void openPlaybackSponsorSettings(void)
{
   playbackSponsorOpen = 1;
   playbackSponsorSelected = 0;
   playbackSponsorRepeat.timer = playbackSponsorRepeat.repeats = 0;
}

static void updatePlaybackSponsorSettings(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { playbackSponsorOpen = 0; return; }
   if (isRepeatDue(&playbackSponsorRepeat, getPadButtonState(PAD_BTN_DOWN)))
      playbackSponsorSelected = (playbackSponsorSelected + 1) % SPONSOR_CATEGORY_COUNT;
   else if (isRepeatDue(&playbackSponsorRepeat, getPadButtonState(PAD_BTN_UP)))
      playbackSponsorSelected = (playbackSponsorSelected + SPONSOR_CATEGORY_COUNT - 1) % SPONSOR_CATEGORY_COUNT;

   int action = getSponsorAction(playbackSponsorSelected);
   int option = action == SPONSOR_ACTION_SKIP ? 0 : action == SPONSOR_ACTION_ASK ? 1 : 2;
   if (isPadButtonPressed(PAD_BTN_LEFT)) option = (option + 2) % 3;
   else if (isPadButtonPressed(PAD_BTN_RIGHT) || isPadButtonPressed(PAD_BTN_CROSS)) option = (option + 1) % 3;
   else return;
   action = option == 0 ? SPONSOR_ACTION_SKIP : option == 1 ? SPONSOR_ACTION_ASK : SPONSOR_ACTION_KEEP;
   if (setSponsorAction(playbackSponsorSelected, action) != 0) {
      showToast(LOC(STR_COULD_NOT_SAVE_SPONSORBLOCK));
      return;
   }
   // If every category was Keep when playback began, no SponsorBlock request was
   // started. Fetch now when the user enables an action from this playback submenu.
   if (hasSponsorBlockActions() && !state.sbReady && !state.sbThreadActive &&
       isBareVideoId(requestedInput)) {
      state.sbWorkerDone = 0;
      state.sbThreadActive = (spawnJoinableThread(&state.sbWorkerTid, sponsorWorker, 0,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-sponsor") == 0);
   }
}

static void setPlaybackUiScale(float scale)
{
   scale = scale >= 1.25f ? 1.5f : 1.0f;
   setGfxSdUiScale(scale);
   char value[16];
   int length = snprintf(value, sizeof value, "%.2f\n", (double)scale);
   if (length > 0 && writeFile(PLAYBACK_SD_SCALE_PATH, value, (uint64_t)length) != 0)
      logWarn("[play] could not persist SD UI scale\n");
   applyPlaybackUiLabelScale(scale);
}

static void updatePlaybackSubtitleStyle(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { playbackSubtitleStyleOpen = 0; return; }
   if (isRepeatDue(&playbackSubtitleStyleRepeat, getPadButtonState(PAD_BTN_DOWN)))
      playbackSubtitleStyleSelected = (playbackSubtitleStyleSelected + 1) % PLAYBACK_SUBTITLE_STYLE_COUNT;
   else if (isRepeatDue(&playbackSubtitleStyleRepeat, getPadButtonState(PAD_BTN_UP)))
      playbackSubtitleStyleSelected = (playbackSubtitleStyleSelected + PLAYBACK_SUBTITLE_STYLE_COUNT - 1) % PLAYBACK_SUBTITLE_STYLE_COUNT;
   int delta = isPadButtonPressed(PAD_BTN_LEFT) ? -1 :
               (isPadButtonPressed(PAD_BTN_RIGHT) || isPadButtonPressed(PAD_BTN_CROSS)) ? 1 : 0;
   if (delta) {
      static const int limits[5] = { 6, 5, 5, 3, 6 };
      int setting = playbackSubtitleStyleSelected;
      int value = (getSubtitleStyle(setting) + delta + limits[setting]) % limits[setting];
      if (setSubtitleStyle(setting, value) != 0) showToast(LOC(STR_COULD_NOT_SAVE_SUBTITLE_STYLE));
   }
}

static void updatePlaybackSettings(void)
{
   if (playbackSubtitleStyleOpen) { updatePlaybackSubtitleStyle(); return; }
   if (playbackAutoTranslateOpen) { updatePlaybackAutoTranslateSettings(); return; }
   if (playbackSponsorOpen) { updatePlaybackSponsorSettings(); return; }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { playbackSettingsOpen = 0; return; }
   if (isRepeatDue(&playbackSettingsRepeat, getPadButtonState(PAD_BTN_DOWN)))
      playbackSettingsSelected = (playbackSettingsSelected + 1) % PLAYBACK_SETTINGS_COUNT;
   else if (isRepeatDue(&playbackSettingsRepeat, getPadButtonState(PAD_BTN_UP)))
      playbackSettingsSelected = (playbackSettingsSelected + PLAYBACK_SETTINGS_COUNT - 1) % PLAYBACK_SETTINGS_COUNT;

   if (playbackSettingsSelected != 2 && playbackSettingsSelected != 3) {
      playbackOverscanRepeat.timer = 0;
      playbackOverscanRepeat.repeats = 0;
   }
   if (playbackSettingsSelected != 8) {
      playbackOsdTimeoutRepeat.timer = 0;
      playbackOsdTimeoutRepeat.repeats = 0;
   }
   int left = isPadButtonPressed(PAD_BTN_LEFT);
   int right = isPadButtonPressed(PAD_BTN_RIGHT);
   int cross = isPadButtonPressed(PAD_BTN_CROSS);

   // Cross is not an adjustment control for these numeric/display settings.
   if (playbackSettingsSelected == 2 || playbackSettingsSelected == 3 ||
       playbackSettingsSelected == 5 || playbackSettingsSelected == 8)
      cross = 0;

   if (playbackSettingsSelected == 5) {
      int offsetDelta = isRepeatDue(&playbackSubtitleOffsetRepeat, getPadButtonState(PAD_BTN_LEFT)) ? -1 :
                        isRepeatDue(&playbackSubtitleOffsetRepeat, getPadButtonState(PAD_BTN_RIGHT)) ? 1 : 0;
      if (offsetDelta) {
         int offset = getSubtitleOffset() + offsetDelta;
         if (setSubtitleOffset(offset) != 0) showToast(LOC(STR_COULD_NOT_SAVE_SUBTITLE_OFFSET));
      }
      return;
   }

   if (playbackSettingsSelected == 8) {
      static const int timeouts[] = { 2, 3, 5, 10, 15, 30, 60 };
      int current = getOsdTimeout(), selected = 0;
      for (int i = 0; i < 7; i++) if (timeouts[i] == current) selected = i;
      int delta = isRepeatDue(&playbackOsdTimeoutRepeat, getPadButtonState(PAD_BTN_LEFT)) ? -1 :
                  isRepeatDue(&playbackOsdTimeoutRepeat, getPadButtonState(PAD_BTN_RIGHT)) ? 1 : 0;
      if (delta) {
         selected += delta;
         if (selected < 0) selected = 0;
         if (selected > 6) selected = 6;
         if (setOsdTimeout(timeouts[selected]) != 0) showToast(LOC(STR_COULD_NOT_SAVE_OSD_TIMEOUT));
      }
      return;
   }

   if (!left && !right && !cross) return;

   if (playbackSettingsSelected == 0) {
      int enabled = getEnable1080p60();
      if (left) enabled = 0;
      else if (right) enabled = 1;
      else enabled = !enabled;
      if (setEnable1080p60(enabled) != 0) showToast(LOC(STR_COULD_NOT_SAVE_1080P_SETTING));
   } else if (playbackSettingsSelected == 1) {
      float current = playbackFontScale;
      setPlaybackUiScale(current >= 1.25f ? 1.0f : 1.5f);
   } else if (playbackSettingsSelected == 2) {
      int overscanDelta = isRepeatDue(&playbackOverscanRepeat, getPadButtonState(PAD_BTN_LEFT)) ? -1 :
                          isRepeatDue(&playbackOverscanRepeat, getPadButtonState(PAD_BTN_RIGHT)) ? 1 : 0;
      if (overscanDelta) {
         int overscan = getOverscanPercent() + overscanDelta;
         if (setOverscanPercent(overscan) != 0) showToast(LOC(STR_COULD_NOT_SAVE_OVERSCAN));
      }
   } else if (playbackSettingsSelected == 3) {
      int cropOverscanDelta = isRepeatDue(&playbackOverscanRepeat, getPadButtonState(PAD_BTN_LEFT)) ? -1 :
                              isRepeatDue(&playbackOverscanRepeat, getPadButtonState(PAD_BTN_RIGHT)) ? 1 : 0;
      if (cropOverscanDelta) {
         int cropOverscan = getCropToFillOverscanPercent() + cropOverscanDelta;
         if (setCropToFillOverscanPercent(cropOverscan) != 0)
            showToast(LOC(STR_COULD_NOT_SAVE_CROP_OVERSCAN));
      }
   } else if (playbackSettingsSelected == 4 && (cross || right)) {
      playbackSubtitleStyleOpen = 1;
      playbackSubtitleStyleSelected = 0;
      playbackSubtitleStyleRepeat.timer = playbackSubtitleStyleRepeat.repeats = 0;
   } else if (playbackSettingsSelected == 6 && (cross || right)) {
      openPlaybackAutoTranslateSettings();
   } else if (playbackSettingsSelected == 7 && (cross || right)) {
      openPlaybackSponsorSettings();
   }
}

static void handlePlaybackOptionsInput(void)
{
   if (playbackSubtitleStyleOpen) { updatePlaybackSubtitleStyle(); return; }
   if (playbackSettingsOpen) { updatePlaybackSettings(); return; }
   if (optionsDropdownOpen) {
      if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_LEFT)) {
         optionsDropdownOpen = 0;
         return;
      }
      int dropdownCount = optionsSelected == 3 ? subtitleMenuCount() : optionsValueCounts[optionsSelected];
      if (dropdownCount > 0) {
         if (isRepeatDue(&optionsDropdownRepeat, getPadButtonState(PAD_BTN_DOWN)))
            optionsDropdownSelected = (optionsDropdownSelected + 1) % dropdownCount;
         else if (isRepeatDue(&optionsDropdownRepeat, getPadButtonState(PAD_BTN_UP)))
            optionsDropdownSelected = (optionsDropdownSelected + dropdownCount - 1) % dropdownCount;
      }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         if (optionsSelected == 0) {
            int target = optionsQualityTargets[optionsDropdownSelected];
            if (target < 0) {
               preferredQualityHeight = preferredQualityFps = 0;
               target = state.autoResolutionIndex;
            } else {
               preferredQualityHeight = state.videoHeights[target];
               preferredQualityFps = state.videoFps[target];
            }
            if (target >= 0 && target != state.resolutionIndex) {
               state.audioFallbackAttempted = 0;
               if (!startResolutionSwitch(target, 0)) showToast(LOC(STR_COULD_NOT_START_QUALITY_CHANGE));
            } else if (target >= 0) {
               showToast(preferredQualityHeight ? LOC(STR_QUALITY_PREFERENCE_SAVED) : LOC(STR_QUALITY_AUTO));
            }
         } else if (optionsSelected == 1) {
            if (state.audioCount > 0)
               startAudioTrackSwitch(optionsDropdownSelected);
            else if (state.resolutionIndex >= 0 && state.resolutionIndex < state.videoCount &&
                     state.videoHasAudio[state.resolutionIndex])
               showToast(LOC(STR_ORIGINAL_UPLOAD_ALREADY_ACTIVE));
            else
               showToast(LOC(STR_NO_AUDIO_TRACKS_AVAILABLE));
         } else if (optionsSelected == 3) {
            int trackIndex = subtitleTrackAtMenuIndex(optionsDropdownSelected);
            if (!state.isLive && trackIndex >= 0 && trackIndex < state.captionCount) {
               CaptionTrack *track = &state.captionTracks[trackIndex];
               if (setPlaybackSubtitleLanguage(track->languageCode) == 0) {
                  lastSubtitlePick = trackIndex;
                  if (subtitlesEnabled) setSubtitleTrack(trackIndex);
                  char notice[112];
                  snprintf(notice, sizeof notice, LOC(STR_SUBTITLE_FORMAT), track->name);
                  showToast(notice);
               } else {
                  showToast(LOC(STR_COULD_NOT_SAVE_SUBTITLE_LANGUAGE));
               }
            } else {
               showToast(LOC(STR_NO_SUBTITLE_LANGUAGES_AVAILABLE));
            }
         }
         optionsDropdownOpen = 0;
      }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      optionsVisible = 0;
      return;
   }

   if (isRepeatDue(&optionsNavigateRepeat, getPadButtonState(PAD_BTN_DOWN)))
      optionsSelected = (optionsSelected + 1) % OPTIONS_COUNT;
   else if (isRepeatDue(&optionsNavigateRepeat, getPadButtonState(PAD_BTN_UP)))
      optionsSelected = (optionsSelected + OPTIONS_COUNT - 1) % OPTIONS_COUNT;

   int left = isPadButtonPressed(PAD_BTN_LEFT);
   int right = isPadButtonPressed(PAD_BTN_RIGHT);
   int cross = isPadButtonPressed(PAD_BTN_CROSS);

   if (optionsSelected == 4 && (right || cross)) {
      openChapters();
      if (state.chaptersVisible) optionsVisible = 0;
      return;
   }

   if (optionsSelected == 2 && (cross || left || right)) {
      int enabled = left ? 0 : right ? 1 : !subtitlesEnabled;
      if (enabled != subtitlesEnabled) {
         subtitlesEnabled = enabled;
         if (subtitlesEnabled) {
            applyPreferredSubtitleTrack();
            showSelectedSubtitleToast();
         } else {
            setSubtitleTrack(-1);
            showToast(LOC(STR_SUBTITLES_OFF));
         }
      }
      return;
   }

   if (optionsSelected == 5 && (cross || left || right)) {
      int enabled = left ? 0 : right ? 1 : !cropToFillEnabled;
      if (enabled != cropToFillEnabled) {
         cropToFillEnabled = enabled;
         cropPanX = cropPanY = 0.0f;
         showToast(cropToFillEnabled ? LOC(STR_CROP_TO_FILL_ON) : LOC(STR_CROP_TO_FILL_OFF));
      }
      return;
   }

   if (optionsSelected == 6 && (cross || left || right)) {
      int enabled = left ? 0 : right ? 1 : !getAutoplayEnabled();
      if (enabled != getAutoplayEnabled()) {
         setAutoplayEnabled(enabled);
         showToast(getAutoplayEnabled() ? LOC(STR_AUTOPLAY_ON) : LOC(STR_AUTOPLAY_OFF));
      }
      return;
   }

   if (optionsSelected == 7 && cross) {
      if (state.isLive || requestedIsLocal) return;
      char videoId[12];
      if (!extractPlaybackId(requestedInput, videoId)) {
         showToast(LOC(STR_VIDEO_CANNOT_BE_DOWNLOADED));
         return;
      }
      SearchResult item;
      memset(&item, 0, sizeof item);
      strCopy(item.videoId, sizeof item.videoId, videoId);
      strCopy(item.title, sizeof item.title, state.title);
      strCopy(item.author, sizeof item.author, state.author);
      const char *audioTrackId = "original";
      if (state.audioPick >= 0 && state.audioPick < state.audioCount &&
          strcmp(state.audioIds[state.audioPick], "__original_upload__") != 0)
         audioTrackId = state.audioIds[state.audioPick];
      const char *activeSubtitle = NULL;
      if (subtitlesEnabled && state.subtitlePick >= 0 && state.subtitlePick < state.captionCount)
         activeSubtitle = state.captionTracks[state.subtitlePick].languageCode;
      enqueuePlaybackDownload(&item, audioTrackId, activeSubtitle);
      optionsDownloadPressedUntil = getTimeUs() + 180000ULL;
      return;
   }

   if (optionsSelected == 8 && (right || cross)) {
      openPlaybackSettings();
      return;
   }

   if ((optionsSelected == 0 || optionsSelected == 1 || optionsSelected == 3) && (right || cross)) {
      if (optionsSelected == 0) {
         prepareQualityOptions();
         optionsDropdownSelected = preferredQualityHeight == 0 ? 0 : qualityDropdownIndexForCurrent();
         if (state.isLive) optionsDropdownSelected = 0;
      } else if (optionsSelected == 1) {
         prepareAudioOptions();
         optionsDropdownSelected = state.audioPick >= 0 && state.audioPick < optionsValueCounts[1] ? state.audioPick : 0;
      } else {
         prepareSubtitleOptions();
         int selectedTrack = (lastSubtitlePick >= 0 && lastSubtitlePick < state.captionCount) ?
                             lastSubtitlePick : findPreferredSubtitleTrack(getPlaybackSubtitleLanguage());
         optionsDropdownSelected = subtitleMainMenuIndexForTrack(selectedTrack);
      }
      optionsDropdownRepeat.timer = optionsDropdownRepeat.repeats = 0;
      optionsDropdownOpen = 1;
   }
}

static int playbackSettingsPanelWidth(void)
{
   // Measure the active locale directly. The labels may still contain the
   // previous language when this function is called immediately after a
   // language change, so measuring their cached textures can underestimate
   // the width required by Portuguese.
   const char *localizedSettingsNames[PLAYBACK_SETTINGS_COUNT] = {
      LOC(STR_ENABLE_1080P60), LOC(STR_UI_SCALING), LOC(STR_OVERSCAN), LOC(STR_OVERSCAN_CROP),
      LOC(STR_SUBTITLE_STYLE), LOC(STR_SUBTITLE_OFFSET), LOC(STR_AUTO_TRANSLATE_SUBTITLES),
      LOC(STR_SPONSORBLOCK), LOC(STR_OSD_TIMEOUT)
   };
   int widest = 0;
   int renderSize = (int)(playbackFontSize(20) * playbackFontScale + 0.5f);
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      int measured = (int)(measureFontText(&uiFont, renderSize, localizedSettingsNames[i]) + 0.5f);
      if (measured > widest) widest = measured;
   }

   // Keep the right-hand value/control column and both horizontal margins.
   // The label area in drawPlaybackSettings ends at panelW - 220.
   int preferred = widest + 40 + 220 + 40 + 24;
   if (preferred < PLAYBACK_SETTINGS_PANEL_W) preferred = PLAYBACK_SETTINGS_PANEL_W;
   int maxWidth = state.screenW - 48;
   return preferred < maxWidth ? preferred : maxWidth;
}

static int playbackSponsorPanelWidth(void)
{
   int width = state.screenW - 48;
   return width < PLAYBACK_SPONSOR_PANEL_W ? width : PLAYBACK_SPONSOR_PANEL_W;
}

static void drawPlaybackSettings(void)
{
   int panelW = playbackSettingsPanelWidth();
   int panelH = 72 + PLAYBACK_SETTINGS_COUNT * PLAYBACK_SETTINGS_ROW_H + 24;
   if (panelH > state.screenH - 48) panelH = state.screenH - 48;
   int x = (state.screenW - panelW) / 2;
   int y = (state.screenH - panelH) / 2;
   int settingsLabelWidth = panelW - 40 - 250 - 56;
   if (settingsLabelWidth < 120) settingsLabelWidth = 120;
   const char *localizedSettingsNames[PLAYBACK_SETTINGS_COUNT] = {
      LOC(STR_ENABLE_1080P60), LOC(STR_UI_SCALING), LOC(STR_OVERSCAN), LOC(STR_OVERSCAN_CROP),
      LOC(STR_SUBTITLE_STYLE), LOC(STR_SUBTITLE_OFFSET), LOC(STR_AUTO_TRANSLATE_SUBTITLES), LOC(STR_SPONSORBLOCK), LOC(STR_OSD_TIMEOUT)
   };
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      if (playbackSettingsLabels[i].width != settingsLabelWidth)
         playbackSettingsLabels[i].width = settingsLabelWidth;
      setLabelText(&playbackSettingsLabels[i], localizedSettingsNames[i]);
   }
   fillGfxRectangleFixed(0, 0, state.screenW, state.screenH, 0x78000000);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&playbackSettingsTitle);
   drawLabelAt(&playbackSettingsTitle,
               state.screenW / 2 - playbackSettingsTitle.tt.tex.w / 2, y + 28);

   char overscanValue[16], cropOverscanValue[16];
   int overscan = getOverscanPercent();
   int cropOverscan = getCropToFillOverscanPercent();
   if (overscan > 0) snprintf(overscanValue, sizeof overscanValue, "+%d%%", overscan);
   else snprintf(overscanValue, sizeof overscanValue, "%d%%", overscan);
   if (cropOverscan > 0) snprintf(cropOverscanValue, sizeof cropOverscanValue, "+%d%%", cropOverscan);
   else snprintf(cropOverscanValue, sizeof cropOverscanValue, "%d%%", cropOverscan);
   static const char *const playbackScaleNames[] = { "1.0x", "1.5x" };
   const char *scaleValue = playbackScaleNames[playbackFontScale >= 1.25f ? 1 : 0];
   if (strcmp(playbackSettingsValues[1].text, scaleValue) != 0)
      setLabelText(&playbackSettingsValues[1], scaleValue);
   setLabelText(&playbackSettingsValues[2], overscanValue);
   setLabelText(&playbackSettingsValues[3], cropOverscanValue);
   char subtitleOffsetValue[16];
   int subtitleOffset = getSubtitleOffset();
   snprintf(subtitleOffsetValue, sizeof subtitleOffsetValue, "%d", subtitleOffset);
   setLabelText(&playbackSettingsValues[4], LOC(STR_CONFIGURE));
   setLabelText(&playbackSettingsValues[5], subtitleOffsetValue);
   const char *autoPreference = getAutoTranslateLanguagePreference();
   char autoTranslateValue[64];
   if (strcmp(autoPreference, "off") == 0) {
      strCopy(autoTranslateValue, sizeof autoTranslateValue, LOC(STR_OFF));
   } else if (strcmp(autoPreference, "auto") == 0) {
      snprintf(autoTranslateValue, sizeof autoTranslateValue, LOC(STR_AUTO_TRANSLATE_VALUE_FORMAT),
               getAutoTranslateLanguage());
   } else {
      const char *languageName = getAutoTranslateLanguageName(autoPreference);
      strCopy(autoTranslateValue, sizeof autoTranslateValue, languageName ? languageName : autoPreference);
   }
   setLabelText(&playbackSettingsValues[6], autoTranslateValue);
   setLabelText(&playbackSettingsValues[7], LOC(STR_CONFIGURE));
   char osdTimeoutValue[16];
   snprintf(osdTimeoutValue, sizeof osdTimeoutValue, "%ds", getOsdTimeout());
   setLabelText(&playbackSettingsValues[8], osdTimeoutValue);

   int toggleW = 132;
   int toggleX = x + panelW - 40 - toggleW;
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      int rowY = y + 72 + i * PLAYBACK_SETTINGS_ROW_H;
      Label *name = &playbackSettingsLabels[i];
      int nameRight = (i == 0) ? toggleX - 24 : x + panelW - 220;
      int nameWidth = nameRight - (x + 40);
      if (nameWidth < 8) nameWidth = 8;
      if (name->width != nameWidth) {
         name->width = nameWidth;
         name->text[0] = '\0';
      }
      refreshLabelScale(name);
      moveLabel(name, x + 40, getCenteredLabelY(name, rowY, PLAYBACK_SETTINGS_ROW_H));
      drawLabel(name);
      if (i == 0) {
         int enabled = getEnable1080p60();
         int selected = enabled ? 1 : 0;
         int toggleY = rowY + (PLAYBACK_SETTINGS_ROW_H - 36) / 2;
         int selectedX = toggleX + selected * 66;
         fillGfxRoundedRectangleFixed(toggleX, toggleY, toggleW, 36, 8, activeTheme->surface);
         if (i == playbackSettingsSelected)
            fillGfxRoundedRectangleFixed(selectedX - 4, toggleY - 4, 74, 44, 10, 0x38FFFFFF);
         fillGfxRoundedRectangleFixed(selectedX, toggleY, 66, 36, 8, activeTheme->accent);
         for (int option = 0; option < 2; option++) {
            Label *word = &playbackSettingsToggleLabels[option];
            refreshLabelScale(word);
            setLabelColor(word, option == selected ? activeTheme->appBg : activeTheme->textPrimary);
            moveLabel(word, toggleX + option * 66 + (66 - word->tt.tex.w) / 2,
                      getCenteredLabelY(word, toggleY, 36));
            drawLabel(word);
         }
      } else {
         Label *value = &playbackSettingsValues[i];
         if (i == 6) {
            int labelRight = x + 40 + playbackSettingsLabels[i].tt.tex.w;
            int valueRight = x + panelW - 52;
            int available = valueRight - labelRight - 24;
            if (available < 8) available = 8;
            if (value->width != available) {
               value->width = available;
               value->text[0] = '\0';
            }
         }
         refreshLabelScale(value);
         int valueW = value->tt.tex.w + 24;
         if (valueW < 100) valueW = 100;
         int valueX = x + panelW - 40 - valueW;
         if (i == 6) {
            int labelRight = x + 40 + playbackSettingsLabels[i].tt.tex.w;
            int minX = labelRight + 24;
            if (valueX < minX) valueX = minX;
         }
         int valueY = getCenteredLabelY(value, rowY, PLAYBACK_SETTINGS_ROW_H);
         if (i == playbackSettingsSelected)
            fillGfxRoundedRectangleFixed(valueX, valueY - 5, valueW, value->tt.tex.h + 10,
                                         7, activeTheme->rowHighlight);
         setLabelColor(value, i == playbackSettingsSelected ? activeTheme->accent : activeTheme->textPrimary);
         moveLabel(value, valueX + (valueW - value->tt.tex.w) / 2, valueY);
         drawLabel(value);
         if (i == 4 || i == 6 || i == 7) {
            int tx = x + panelW - 24;
            int ty = rowY + PLAYBACK_SETTINGS_ROW_H / 2;
            drawGfxTriangle(tx - 7, ty - 4, activeTheme->textSecondary,
                            tx + 7, ty - 4, activeTheme->textSecondary,
                            tx, ty + 4, activeTheme->textSecondary);
         }
      }
   }
}

static void drawPlaybackAutoTranslateSettings(void)
{
   int panelW = playbackSettingsPanelWidth();
   int panelH = PLAYBACK_AUTO_TRANSLATE_PANEL_H;
   if (panelH > state.screenH - 32) panelH = state.screenH - 32;
   int x = (state.screenW - panelW) / 2;
   int y = (state.screenH - panelH) / 2;
   int count = autoTranslateMenuCount();
   int visible = count < PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS ? count : PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS;
   int first = playbackAutoTranslateSelected - visible / 2;
   if (first < 0) first = 0;
   if (first + visible > count) first = count - visible;

   fillGfxRectangleFixed(0, 0, state.screenW, state.screenH, 0x78000000);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&playbackAutoTranslateTitle);
   drawLabelAt(&playbackAutoTranslateTitle,
               state.screenW / 2 - playbackAutoTranslateTitle.tt.tex.w / 2, y + 20);

   int listY = y + 58;
   for (int row = 0; row < visible; row++) {
      int index = first + row;
      const char *name;
      if (index == 0) name = LOC(STR_OFF);
      else if (index == 1) name = LOC(STR_AUTO_CONTENT_REGION);
      else {
         const AutoTranslateLanguage *language = getAutoTranslateLanguageAt(index - 2);
         name = language ? language->name : LOC(STR_UNKNOWN);
      }
      Label *label = &playbackAutoTranslateRows[row];
      setLabelText(label, name);
      refreshLabelScale(label);
      int rowY = listY + row * PLAYBACK_AUTO_TRANSLATE_ROW_H;
      int labelY = getCenteredLabelY(label, rowY, PLAYBACK_AUTO_TRANSLATE_ROW_H);
      if (index == playbackAutoTranslateSelected)
         fillGfxRoundedRectangleFixed(x + 22, rowY + 2, panelW - 56,
                                      PLAYBACK_AUTO_TRANSLATE_ROW_H - 4, 6, activeTheme->rowHighlight);
      setLabelColor(label, index == playbackAutoTranslateSelected ? activeTheme->accent : activeTheme->textPrimary);
      drawLabelAt(label, x + 34, labelY);
   }

   if (count > visible) {
      int trackX = x + panelW - 22;
      int trackY = listY + 5;
      int trackH = visible * PLAYBACK_AUTO_TRANSLATE_ROW_H - 10;
      int thumbH = trackH * visible / count;
      if (thumbH < 24) thumbH = 24;
      int maxScroll = count - visible;
      int thumbY = trackY + (trackH - thumbH) * first / maxScroll;
      fillGfxRoundedRectangleFixed(trackX, trackY, 5, trackH, 3, activeTheme->surface);
      fillGfxRoundedRectangleFixed(trackX, thumbY, 5, thumbH, 3, activeTheme->accent);
   }
}

static void prepareSubtitleLabel(Label *label, uint32_t textColor)
{
   // Shadow/outline are rasterized into the label texture when its text or style changes.
   setLabelHighlight(label, getSubtitleStyle(3));
   if (label->color != textColor) setLabelColor(label, textColor);
}

static void getSubtitleLabelPadding(const Label *label, int *left, int *top, int *right, int *bottom)
{
   *left = *top = *right = *bottom = 0;
   if (label->highlightMode == 1) { *right = 4; *bottom = 4; }
   else if (label->highlightMode == 2) { *left = *top = *right = *bottom = 2; }
}

static void drawSubtitleLabelWithHighlight(Label *label, int x, int y)
{
   // The pre-composed shadow/outline and foreground are submitted as one texture draw.
   drawLabelAt(label, x, y);
}

static void drawPlaybackSubtitleStyle(void)
{
   const char *textNames[] = { LOC(STR_WHITE), LOC(STR_YELLOW), LOC(STR_CYAN), LOC(STR_GREEN), LOC(STR_BLUE), LOC(STR_RED) };
   static const uint32_t textColors[] = { 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFF4488FF, 0xFFFF4444 };
   const char *bgNames[] = { LOC(STR_BLACK), LOC(STR_DARK_GRAY), LOC(STR_GRAY), LOC(STR_BLUE), LOC(STR_TRANSPARENT) };
   static const uint32_t bgColors[] = { 0xFF000000, 0xFF303030, 0xFF777777, 0xFF102040, 0x00000000 };
   static const char *const opacityNames[] = { "0%", "25%", "50%", "80%", "100%" };
   const char *highlightNames[] = { LOC(STR_NONE), LOC(STR_SHADOW), LOC(STR_OUTLINE) };
   const char *fontSizeNames[] = { LOC(STR_SMALL), LOC(STR_MEDIUM), LOC(STR_DEFAULT), LOC(STR_LARGE), LOC(STR_EXTRA_LARGE), LOC(STR_HUGE) };
   const char *values[] = { textNames[getSubtitleStyle(0)], bgNames[getSubtitleStyle(1)],
                            opacityNames[getSubtitleStyle(2)], highlightNames[getSubtitleStyle(3)],
                            fontSizeNames[getSubtitleStyle(4)] };
   int panelW = playbackSettingsPanelWidth(), panelH = PLAYBACK_SUBTITLE_STYLE_PANEL_H;
   if (panelH > state.screenH - 48) panelH = state.screenH - 48;
   int x = (state.screenW - panelW) / 2, y = (state.screenH - panelH) / 2;
   fillGfxRectangleFixed(0, 0, state.screenW, state.screenH, 0x78000000);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&playbackSubtitleStyleTitle);
   drawLabelAt(&playbackSubtitleStyleTitle, state.screenW / 2 - playbackSubtitleStyleTitle.tt.tex.w / 2, y + 22);
   int previewX = x + 30, previewY = y + 66, previewW = panelW - 60, previewH = 92;
   fillGfxRoundedRectangleFixed(previewX, previewY, previewW, previewH, 7, 0xFF252525);
   uint32_t bg = bgColors[getSubtitleStyle(1)];
   bg = (bg & 0x00FFFFFF) | ((uint32_t[]){ 0, 64, 128, 204, 255 }[getSubtitleStyle(2)] << 24);
   uint32_t color = textColors[getSubtitleStyle(0)];
   playbackSubtitlePreview.size = playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4);
   setLabelText(&playbackSubtitlePreview, LOC(STR_SUBTITLE_PREVIEW));
   prepareSubtitleLabel(&playbackSubtitlePreview, color);
   refreshLabelScale(&playbackSubtitlePreview);
   int padL, padT, padR, padB;
   getSubtitleLabelPadding(&playbackSubtitlePreview, &padL, &padT, &padR, &padB);
   int textW = playbackSubtitlePreview.tt.tex.w - padL - padR;
   int textH = playbackSubtitlePreview.tt.tex.h - padT - padB;
   int tx = state.screenW / 2 - textW / 2, ty = previewY + (previewH - (textH + 12)) / 2 + 6;
   fillGfxRoundedRectangleFixed(tx - 12, ty - 6, textW + 24, textH + 12, 4, bg);
   drawSubtitleLabelWithHighlight(&playbackSubtitlePreview, tx - padL, ty - padT);
   for (int i = 0; i < PLAYBACK_SUBTITLE_STYLE_COUNT; i++) {
      int rowY = y + 176 + i * 52;
      Label *name = &playbackSubtitleStyleLabels[i]; refreshLabelScale(name);
      if (i == playbackSubtitleStyleSelected)
         fillGfxRoundedRectangleFixed(x + 18, rowY, panelW - 36, 46, 7, activeTheme->rowHighlight);
      setLabelColor(name, i == playbackSubtitleStyleSelected ? activeTheme->accent : activeTheme->textPrimary);
      drawLabelAt(name, x + 38, getCenteredLabelY(name, rowY, 46));
      Label *value = &playbackSettingsValues[0];
      setLabelText(value, values[i]); refreshLabelScale(value);
      setLabelColor(value, i == playbackSubtitleStyleSelected ? activeTheme->accent : activeTheme->textPrimary);
      drawLabelAt(value, x + panelW - 40 - value->tt.tex.w, getCenteredLabelY(value, rowY, 46));
   }
}

static void drawPlaybackSponsorSettings(void)
{
   int panelW = playbackSponsorPanelWidth();
   int panelH = PLAYBACK_SPONSOR_PANEL_H;
   if (panelH > state.screenH - 48) panelH = state.screenH - 48;
   int x = (state.screenW - panelW) / 2;
   int y = (state.screenH - panelH) / 2;
   fillGfxRectangleFixed(0, 0, state.screenW, state.screenH, 0x78000000);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&playbackSponsorTitle);
   drawLabelAt(&playbackSponsorTitle,
               state.screenW / 2 - playbackSponsorTitle.tt.tex.w / 2, y + 28);

   static const int actionValues[3] = { SPONSOR_ACTION_SKIP, SPONSOR_ACTION_ASK, SPONSOR_ACTION_KEEP };
   int optionW = PLAYBACK_SPONSOR_OPTION_W;
   int optionH = 40;
   int optionGroupW = 3 * optionW + 2 * PLAYBACK_SPONSOR_OPTION_GAP;
   int optionX = x + panelW - 32 - optionGroupW;
   int rowTop = y + 86;
   for (int row = 0; row < SPONSOR_CATEGORY_COUNT; row++) {
      int rowY = rowTop + row * PLAYBACK_SPONSOR_ROW_H;
      int focused = row == playbackSponsorSelected;
      if (focused)
         fillGfxRoundedRectangleFixed(x + 18, rowY, panelW - 36, PLAYBACK_SPONSOR_ROW_H - 2,
                                      7, activeTheme->rowHighlight);
      fillGfxCircle(x + 36, rowY + PLAYBACK_SPONSOR_ROW_H / 2, 7,
                    getSponsorCategoryColor((SponsorCategory)row));
      Label *name = &playbackSponsorCategoryLabels[row];
      setLabelText(name, getSponsorCategoryName((SponsorCategory)row));
      refreshLabelScale(name);
      setLabelColor(name, focused ? activeTheme->textPrimary : activeTheme->textSecondary);
      moveLabel(name, x + 52, getCenteredLabelY(name, rowY, PLAYBACK_SPONSOR_ROW_H));
      drawLabel(name);

      int chosen = getSponsorAction(row);
      for (int option = 0; option < 3; option++) {
         int ox = optionX + option * (optionW + PLAYBACK_SPONSOR_OPTION_GAP);
         int selected = chosen == actionValues[option];
         int optionY = rowY + (PLAYBACK_SPONSOR_ROW_H - optionH) / 2;
         uint32_t bg = selected ? (focused ? activeTheme->accent : activeTheme->surface) : activeTheme->surface;
         fillGfxRoundedRectangleFixed(ox, optionY, optionW, optionH, 7, bg);
         Label *word = &playbackSponsorActionLabels[option];
         setLabelText(word, option == 0 ? LOC(STR_SKIP) : option == 1 ? LOC(STR_ASK) : LOC(STR_KEEP));
         refreshLabelScale(word);
         setLabelColor(word, selected ? (focused ? activeTheme->appBg : activeTheme->textPrimary) : 0xFF888888);
         moveLabel(word, ox + (optionW - word->tt.tex.w) / 2,
                   getCenteredLabelY(word, optionY, optionH));
         drawLabel(word);
      }
   }
}

static void setOptionsCurrentValue(Label *label, const char *value, int width)
{
   char valueCopy[sizeof label->text];
   strCopy(valueCopy, sizeof valueCopy, value ? value : LOC(STR_UNAVAILABLE));
   if (label->width != width) {
      label->width = width;
      label->text[0] = '\0'; // force a reraster with the new available width
   }
   setLabelText(label, valueCopy);
   refreshLabelScale(label);
}

static void updateOptionsCurrentValues(int valueWidth)
{
   prepareQualityOptions();
   int qualityIndex = 0;
   if (preferredQualityHeight > 0) {
      for (int i = 1; i < optionsValueCounts[0]; i++) {
         int target = optionsQualityTargets[i];
         if (target >= 0 && target < state.videoCount &&
             state.videoHeights[target] == preferredQualityHeight &&
             (preferredQualityFps <= 0 || state.videoFps[target] == preferredQualityFps)) {
            qualityIndex = i;
            break;
         }
      }
      if (qualityIndex == 0 && state.resolutionIndex >= 0)
         qualityIndex = qualityDropdownIndexForCurrent();
   }
   if (qualityIndex < 0 || qualityIndex >= optionsValueCounts[0]) qualityIndex = 0;
   setOptionsCurrentValue(&optionsCurrentValueLabels[0], optionsQualityLabels[qualityIndex], 0);

   prepareAudioOptions();
   const char *audioValue = LOC(STR_UNAVAILABLE);
   if (state.audioPick >= 0 && state.audioPick < state.audioCount &&
       state.audioPick < optionsValueCounts[1]) {
      audioValue = optionsAudioLabels[state.audioPick];
   } else if (state.resolutionIndex >= 0 && state.resolutionIndex < state.videoCount &&
              state.videoHasAudio[state.resolutionIndex]) {
      audioValue = LOC(STR_ORIGINAL_UPLOAD_PLAYBACK);
   } else if (state.fallbackAudioUrl[0]) {
      audioValue = LOC(STR_ORIGINAL_UPLOAD_PLAYBACK);
   } else if (optionsValueCounts[1] > 0 && state.audioCount > 0) {
      audioValue = optionsAudioLabels[0];
   }
   setOptionsCurrentValue(&optionsCurrentValueLabels[1], audioValue, valueWidth);

   int subtitleIndex = state.subtitlePick;
   if (subtitleIndex < 0 || subtitleIndex >= state.captionCount) subtitleIndex = lastSubtitlePick;
   if (subtitleIndex < 0 || subtitleIndex >= state.captionCount)
      subtitleIndex = findPreferredSubtitleTrack(getPlaybackSubtitleLanguage());
   const char *subtitleValue = (subtitleIndex >= 0 && subtitleIndex < state.captionCount) ?
                               state.captionTracks[subtitleIndex].name : LOC(STR_UNAVAILABLE);
   setOptionsCurrentValue(&optionsCurrentValueLabels[2], subtitleValue, valueWidth);
}

static void drawPlaybackOptions(void)
{
   int panelW = 680;
   int widestLabel = 0;
   for (int i = 0; i < OPTIONS_COUNT; i++) {
      refreshLabelScale(&optionsLabels[i]);
      if (optionsLabels[i].tt.tex.w > widestLabel) widestLabel = optionsLabels[i].tt.tex.w;
   }
   int widestAudioValue = 0;
   int audioRenderSize = (int)(playbackFontSize(27) * playbackFontScale + 0.5f);
   for (int i = 0; i < optionsValueCounts[1]; i++) {
      int measured = (int)(measureFontText(&uiFont, audioRenderSize, optionsAudioLabels[i]) + 0.5f);
      if (measured > widestAudioValue) widestAudioValue = measured;
   }
   int requiredW = widestLabel + 38 + 240;
   int widestSubtitleValue = 0;
   int subtitleRenderSize = (int)(playbackFontSize(27) * playbackFontScale + 0.5f);
   for (int i = 0; i < state.captionCount; i++) {
      const char *name = state.captionTracks[i].name;
      int measured = (int)(measureFontText(&uiFont, subtitleRenderSize, name ? name : "") + 0.5f);
      if (measured > widestSubtitleValue) widestSubtitleValue = measured;
   }
   int audioRequiredW = 38 + widestAudioValue + 38 + 44 + 80;
   int subtitleRequiredW = 38 + widestSubtitleValue + 38 + 44 + 80;
   if (audioRequiredW > requiredW) requiredW = audioRequiredW;
   if (subtitleRequiredW > requiredW) requiredW = subtitleRequiredW;
   if (requiredW > panelW) panelW = requiredW;
   if (panelW > state.screenW - 32) panelW = state.screenW - 32;
   int rowH = state.screenH < 560 ? 44 : 54;
   int panelH = 70 + OPTIONS_COUNT * rowH;
   int x = (state.screenW - panelW) / 2;
   int y = (state.screenH - panelH) / 2;
   int valueFontSize = (int)(playbackFontSize(27) * playbackFontScale + 0.5f);
   int twoSpaceGap = (int)(measureFontText(&uiFont, valueFontSize, "  ") + 0.5f);
   int arrowLeft = x + panelW - 44 - 7;
   int valueRight = arrowLeft - twoSpaceGap;
   int valueWidth = valueRight - (x + 38 + twoSpaceGap);
   if (valueWidth < 8) valueWidth = 8;
   updateOptionsCurrentValues(valueWidth);
   // Each value gets the space left by its own row label, not by the widest
   // label in the menu. This is especially important for Audio and subtitles.
   for (int i = 0; i < OPTIONS_COUNT; i++) {
      int rowValueIndex = i == 1 ? 1 : i == 3 ? 2 : -1;
      if (rowValueIndex >= 0) {
         int available = valueRight - (x + 38 + optionsLabels[i].tt.tex.w + twoSpaceGap);
         if (available < 8) available = 8;
         setOptionsCurrentValue(&optionsCurrentValueLabels[rowValueIndex],
                                optionsCurrentValueLabels[rowValueIndex].text, available);
      }
   }

   // Preserve the interface font size. The label's logical width controls
   // ellipsis; do not shrink the font to compensate for a narrow value column.
   for (int valueIndex = 1; valueIndex <= 2; valueIndex++)
      refreshLabelScale(&optionsCurrentValueLabels[valueIndex]);

   // Quality is never ellipsized: right-align it immediately before the arrow.
   // If a future quality label is unusually long, reduce only that value's font
   // enough to keep the complete string inside the Options panel.
   Label *qualityValue = &optionsCurrentValueLabels[0];
   refreshLabelScale(qualityValue);
   int qualityMaxWidth = arrowLeft - (x + 38) - 12;
   int qualityBaseSize = playbackFontSize(27);
   int qualityRenderSize = (int)(qualityBaseSize * playbackFontScale + 0.5f);
   int qualityTextWidth = (int)(measureFontText(&uiFont, qualityRenderSize, qualityValue->text) + 0.5f);
   int qualityFitSize = qualityBaseSize;
   if (qualityTextWidth > qualityMaxWidth && qualityTextWidth > 0) {
      qualityFitSize = (int)(qualityBaseSize * ((float)qualityMaxWidth / (float)qualityTextWidth));
      if (qualityFitSize < 12) qualityFitSize = 12;
   }
   if (qualityValue->size != qualityFitSize) {
      qualityValue->size = qualityFitSize;
      qualityValue->fixedRenderScale = 0.0f;
      setLabelFixedRenderScale(qualityValue, playbackFontScale, 0);
   }

   // Slightly stronger panel fill: less of the video shows through the Options window.
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 14, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);

   for (int i = 0; i < OPTIONS_COUNT; i++) {
      int rowY = y + 32 + i * rowH;
      Label *name = &optionsLabels[i];
      int nameY = getCenteredLabelY(name, rowY, rowH);

      if (i == optionsSelected)
         fillGfxRoundedRectangleFixed(x + 18, nameY - 7, panelW - 36,
                                      name->tt.tex.h + 14, 7, activeTheme->rowHighlight);
      if (i == 7 && (state.isLive || requestedIsLocal))
         setLabelColor(name, activeTheme->textSecondary);
      else
         setLabelColor(name, activeTheme->textPrimary);
      drawLabelAt(name, x + 38, nameY);

      if (i == 0 || i == 1 || i == 3) {
         Label *value = &optionsCurrentValueLabels[i == 0 ? 0 : i == 1 ? 1 : 2];
         if (i == 0) {
            int qualityX = arrowLeft - value->tt.tex.w - 12;
            if (qualityX < x + 38) qualityX = x + 38;
            drawLabelAt(value, qualityX, getCenteredLabelY(value, rowY, rowH));
         } else {
            int labelRight = x + 38 + optionsLabels[i].tt.tex.w;
            int availableLeft = labelRight + twoSpaceGap;
            int valueXAligned = valueRight - value->tt.tex.w;
            if (valueXAligned < availableLeft) valueXAligned = availableLeft;
            drawLabelAt(value, valueXAligned, getCenteredLabelY(value, rowY, rowH));
         }
      }

      if (i == 2 || i == 5 || i == 6) {
         int enabled = i == 2 ? subtitlesEnabled :
                       i == 5 ? cropToFillEnabled : getAutoplayEnabled();
         int selected = enabled ? 1 : 0;
         int toggleW = 132, toggleH = 34;
         int toggleX = x + panelW - 38 - toggleW;
         int toggleY = rowY + (rowH - toggleH) / 2;
         int selectedX = toggleX + selected * 66;
         fillGfxRoundedRectangleFixed(toggleX, toggleY, toggleW, toggleH, 8, activeTheme->surface);
         if (i == optionsSelected)
            fillGfxRoundedRectangleFixed(selectedX - 4, toggleY - 4, 74, toggleH + 8,
                                         10, 0x38FFFFFF);
         fillGfxRoundedRectangleFixed(selectedX, toggleY, 66, toggleH, 8, activeTheme->accent);
         for (int option = 0; option < 2; option++) {
            Label *word = &playbackSettingsToggleLabels[option];
            refreshLabelScale(word);
            setLabelColor(word, option == selected ? activeTheme->appBg : activeTheme->textPrimary);
            moveLabel(word, toggleX + option * 66 + (66 - word->tt.tex.w) / 2,
                      getCenteredLabelY(word, toggleY, toggleH));
            drawLabel(word);
         }
      } else if (i == 7) {
         int disabled = state.isLive || requestedIsLocal;
         int buttonW = 150, buttonH = 34;
         int buttonX = x + panelW - 38 - buttonW;
         int buttonY = rowY + (rowH - buttonH) / 2;
         int pressed = !disabled && getTimeUs() < optionsDownloadPressedUntil;
         uint32_t buttonColor = pressed ? activeTheme->accent : activeTheme->surface;
         fillGfxRoundedRectangleFixed(buttonX, buttonY, buttonW, buttonH, 8, buttonColor);
         setLabelColor(&optionsDownloadLabel,
                       disabled ? activeTheme->textSecondary :
                       (pressed ? activeTheme->appBg : activeTheme->textPrimary));
         drawLabelAt(&optionsDownloadLabel,
                     buttonX + (buttonW - optionsDownloadLabel.tt.tex.w) / 2,
                     getCenteredLabelY(&optionsDownloadLabel, buttonY, buttonH));
      } else if (i == 0 || i == 1 || i == 3 || i == 4 || i == 6 || i == 8) {
         int tx = x + panelW - 44;
         int ty = rowY + rowH / 2;
         drawGfxTriangle(tx - 7, ty - 4, activeTheme->textSecondary,
                         tx + 7, ty - 4, activeTheme->textSecondary,
                         tx, ty + 4, activeTheme->textSecondary);
      }
   }

   if (optionsDropdownOpen) {
      int count = optionsSelected == 3 ? subtitleMenuCount() : optionsValueCounts[optionsSelected];
      int visibleCount = count < OPTIONS_DROPDOWN_VISIBLE_MAX ? count : OPTIONS_DROPDOWN_VISIBLE_MAX;
      int firstItem = 0;
      if (count > visibleCount) {
         firstItem = optionsDropdownSelected - visibleCount / 2;
         if (firstItem < 0) firstItem = 0;
         if (firstItem + visibleCount > count) firstItem = count - visibleCount;
      }
      int dropW = panelW - 48;
      int widestItem = 0;
      int itemRenderSize = (int)(playbackFontSize(24) * playbackFontScale + 0.5f);
      for (int item = 0; item < count; item++) {
         int trackIndex = optionsSelected == 3 ? subtitleTrackAtMenuIndex(item) : -1;
         const char *candidate = optionsSelected == 0 ? optionsQualityLabels[item] :
                                optionsSelected == 1 ? optionsAudioLabels[item] :
                                optionsSelected == 3 ? (trackIndex >= 0 ? state.captionTracks[trackIndex].name : LOC(STR_UNAVAILABLE)) :
                                LOC(optionsValueIds[optionsSelected][item]);
         int measured = (int)(measureFontText(&uiFont, itemRenderSize, candidate) + 0.5f);
         if (measured > widestItem) widestItem = measured;
      }
      int requiredDropW = widestItem + 64;
      if (requiredDropW > dropW) dropW = requiredDropW;
      if (dropW > state.screenW - 32) dropW = state.screenW - 32;
      int dropH = visibleCount * 44 + 20;
      int dx = x + panelW - dropW - 18;
      if (dx < x + 12) dx = x + 12;
      if (dx + dropW > state.screenW - 16) dx = state.screenW - dropW - 16;
      int dy = y + 32 + optionsSelected * rowH - 8;
      if (dy + dropH > state.screenH - 16) dy = state.screenH - dropH - 16;
      if (dy < 16) dy = 16;

      // Two restrained light layers create a halo without changing the submenu fill.
      fillGfxRoundedRectangleFixed(dx - 5, dy - 5, dropW + 10, dropH + 10,
                                   15, 0x20B9D8F4);
      fillGfxRoundedRectangleFixed(dx - 2, dy - 2, dropW + 4, dropH + 4,
                                   12, 0x30B9D8F4);
      fillGfxRoundedRectangleFixed(dx, dy, dropW, dropH, 10, 0xF0101724);
      for (int row = 0; row < visibleCount; row++) {
         int i = firstItem + row;
         Label *value = &optionsValueLabels[row];
         int subtitleTrackIndex = optionsSelected == 3 ? subtitleTrackAtMenuIndex(i) : -1;
         char subtitleItemText[64];
         const char *itemText = optionsSelected == 0 ? optionsQualityLabels[i] :
                                optionsSelected == 1 ? optionsAudioLabels[i] :
                                optionsSelected == 3 ? (subtitleTrackIndex >= 0 ?
                                   state.captionTracks[subtitleTrackIndex].name : LOC(STR_UNAVAILABLE)) :
                                LOC(optionsValueIds[optionsSelected][i]);
         if (optionsSelected == 3 && subtitleTrackIndex >= 0 &&
             subtitleIsAutoTranslated(&state.captionTracks[subtitleTrackIndex])) {
            snprintf(subtitleItemText, sizeof subtitleItemText, "* %s", itemText);
            itemText = subtitleItemText;
         }
         int itemWidth = dropW - (count > visibleCount ? 76 : 52);
         if (itemWidth < 80) itemWidth = 80;
         if (value->width != itemWidth) {
            value->width = itemWidth;
            value->text[0] = '\0';
         }
         setLabelText(value, itemText);
         int itemY = dy + 8 + row * 44;
         int labelY = getCenteredLabelY(value, itemY, 36);
         if (i == optionsDropdownSelected)
            fillGfxRoundedRectangleFixed(dx + 10, labelY - 6,
                                         dropW - 20, value->tt.tex.h + 12,
                                         6, 0xA05A7898);
         drawLabelAt(value, dx + 26, labelY);
      }
      if (count > visibleCount) {
         int trackX = dx + dropW - 16;
         int trackY = dy + 10;
         int trackH = dropH - 20;
         int thumbH = trackH * visibleCount / count;
         if (thumbH < 22) thumbH = 22;
         int maxFirst = count - visibleCount;
         int thumbY = trackY + (trackH - thumbH) * firstItem / maxFirst;
         fillGfxRoundedRectangleFixed(trackX, trackY, 5, trackH, 3, activeTheme->surface);
         fillGfxRoundedRectangleFixed(trackX, thumbY, 5, thumbH, 3, activeTheme->accent);
      }
   }
}

static void updatePlay(void)
{
   // BD Remote STOP is a global playback exit: unlike Circle, it must not be
   // consumed by the OSD, options, or any other playback overlay.
   if (isPadRemoteButtonPressed(PAD_REMOTE_STOP)) {
      logInfo("[yt] playback exit requested by user (BD Remote STOP)\n");
      requestPlaybackCancel();
      popScreen();
      return;
   }

   updateDownloadOverlay();
   if (isDownloadSpacePromptOpen()) return;
   if (consumeDownloadCompletion()) showToastFor(LOC(STR_DOWNLOAD_COMPLETE), 3000000ULL);

   if (state.liveFailureVisible) {
      if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
         if (isPadButtonPressed(PAD_BTN_CIRCLE))
            logInfo("[yt] playback exit requested by user (CIRCLE)\n");
         requestPlaybackCancel();
         popScreen();
         return;
      }
      return;
   }

   // During initial loading there is no adopted VideoPlayer yet. Treat Circle as an
   // interrupt before any HUD/paused-state query can inspect state.player.
   if (state.stage == STAGE_LOADING && isPadButtonPressed(PAD_BTN_CIRCLE)) {
      logInfo("[yt] playback exit requested by user (CIRCLE)\n");
      requestPlaybackCancel();
      popScreen();
      return;
   }

   // R2 is a global playback shortcut: overlays own navigation, but must not
   // suppress the Crop to Fill toggle while Options or its submenus are open.
   if (state.stage == STAGE_PLAYING && state.player && isPadButtonPressed(PAD_BTN_R2)) {
      cropToFillEnabled = !cropToFillEnabled;
      cropPanX = cropPanY = 0.0f;
      showToast(cropToFillEnabled ? LOC(STR_CROP_TO_FILL_ON) : LOC(STR_CROP_TO_FILL_OFF));
   }

   // an overlay owns the pad for the whole frame it was (or just became) visible, so the button that
   // closed or opened it can't leak into playback input below (e.g. cross toggling pause on the way out)
   int overlayHadPad = state.descriptionVisible || state.chaptersVisible || optionsVisible;
   int loadingPauseInput = 0;
   if (state.descriptionVisible) handleDescriptionInput();
   else if (state.chaptersVisible) handleChaptersInput();
   else if (optionsVisible) handlePlaybackOptionsInput();
   else {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
         // Circle dismisses playback controls first. This also overrides the paused/ended
         // always-visible seek bar; a second Circle exits to the results list.
         uint64_t now = sys_time_get_system_time();
         int volumeVisible = volumeMeter.shownUs != 0 && now - volumeMeter.shownUs < 2500000ULL;
         if (controlsVisible() || fullPlaybackHudVisible() || volumeVisible) {
            state.controlsDismissed = 1;
            state.controlsShownUs = 0;
            state.hudShownUs = 0;
            hideVolumeMeter(&volumeMeter);
         } else {
            logInfo("[yt] playback exit requested by user (CIRCLE)\n");
            requestPlaybackCancel();
            popScreen();
            return;
         }
      }
      if (state.stage == STAGE_LOADING && isPadButtonPressed(PAD_BTN_CROSS)) {
         state.pauseWhenReady = !state.pauseWhenReady;
         loadingPauseInput = 1;
      }
      if (isPadButtonPressed(PAD_BTN_R3)) state.showStats = !state.showStats;
      if (isPadButtonPressed(PAD_BTN_SELECT) && state.stage == STAGE_PLAYING && state.player) openDescription();
      if (isPadButtonPressed(PAD_BTN_START) && state.stage == STAGE_PLAYING && state.player) openPlaybackOptions();
   }

   // reap the worker once it's done; adopt the player it built
   if (state.workerDone && state.threadActive) {
      joinThread(state.workerTid);
      state.threadActive = 0;
      if (state.pendingPlayer) {
         state.player = state.pendingPlayer;
         state.pendingPlayer = NULL;
         state.liveReopenThreadActive = 0;
         state.liveRefreshActive = 0;
         state.vodRefreshActive = 0;
         state.message[0] = 0;
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
         if (state.pauseWhenReady) setVideoPaused(state.player, 1);
         setLabelText(&titleLabel, state.title);   // rasterise once, off the draw path
         setLabelText(&authorLabel, state.author);
         refreshPlaybackInfoHud();
         state.stage = STAGE_PLAYING;
         if (state.isLive && state.liveAndroidClient) {
            state.liveBaseRefreshAttempts = 0;
            state.liveBaseRefreshNextTryUs = sys_time_get_system_time() + liveBaseRefreshIntervalUs();
         }
         rememberAutoplayVideo(requestedInput);
         showControls();
      } else if (state.liveRefreshActive && !playbackCancelRequested()) {
         state.liveRefreshActive = 0;
         if (state.liveRefreshAttempts >= 5) showLiveRefreshFailure();
         else scheduleLiveRefresh();
      } else if (state.vodRefreshActive && !playbackCancelRequested()) {
         state.vodRefreshActive = 0;
         if (state.vodRefreshAttempts >= 5) showVodRefreshFailure();
         else scheduleVodHttpRefresh();
      } else if (state.liveReopenThreadActive && !playbackCancelRequested()) {
         state.liveReopenThreadActive = 0;
         logWarn("[yt-live-refresh] descriptor rebuild failed; falling back to a fresh YouTube resolve\n");
         scheduleLiveRefresh();
      }
   }

   if (state.liveBaseRefreshActive && state.liveBaseRefreshDone)
      reapLiveBaseRefresh();
   if (state.stage == STAGE_PLAYING && state.isLive && state.liveAndroidClient && state.player) {
      if (getVideoPlayerLiveRenewalNeeded(state.player))
         scheduleLiveBaseRefresh(1);
      else
         scheduleLiveBaseRefresh(0);
   }

   if (state.liveEndCheckActive && state.liveEndCheckDone) {
      joinThread(state.liveEndCheckTid);
      state.liveEndCheckActive = 0;
      if (state.liveEndCheckResult == 1) {
         state.liveEndDetected = 1;
         logInfo("[yt-live-end] YouTube confirms broadcast ended; handing off to autoplay\n");
      } else {
         logInfo("[yt-live-end] status still LIVE or inconclusive (status=%d); resuming normal renewal\n",
                 state.liveEndCheckResult);
         scheduleLiveRefresh();
      }
   }

   // Any HTTP error from a LIVE segment source is a session/transport failure.
   // Do not treat it as EOF: renew the extractor response and rebuild the complete player.
   if (state.stage == STAGE_PLAYING && state.isLive && !state.liveEndDetected && state.player &&
       isVideoPlayerFailed(state.player) && getVideoPlayerLiveError(state.player) != 0) {
      int error = getVideoPlayerLiveError(state.player);
#if BUILD_DIAG_LIVE_RECOVERY
      VideoLiveBufferStatus liveBuffer;
      memset(&liveBuffer, 0, sizeof liveBuffer);
      getVideoPlayerLiveBufferStatus(state.player, &liveBuffer);
      logWarn("[yt-live-renew-diag] trigger error=%d http=%d source=%d failed=%d attempts=%d itags=%d/%d separateAudio=%d audioAlive=%d liveSegmentSec=%d edgeSq=%ld firstSq=%ld bufferedKB=%llu aheadKB=%llu segmentsAhead=%d fetchInFlight=%d\n",
              error, getVideoPlayerLiveHttpError(state.player), getVideoPlayerSourceError(state.player),
              isVideoPlayerFailed(state.player), state.liveRefreshAttempts, state.vidItag, state.audItag,
              isVideoPlayerUsingSeparateAudio(state.player), isVideoPlayerSeparateAudioAlive(state.player),
              state.liveSegmentSec, state.liveEdgeSq, getVideoLiveFirstSq(state.player),
              (unsigned long long)(liveBuffer.bufferedBytes/1024ull),
              (unsigned long long)(liveBuffer.aheadBytes/1024ull), liveBuffer.segmentsAhead,
              liveBuffer.fetchInFlight);
#endif
      if (error == 404 && state.firstFrameSeen && state.liveLastFrameUs) {
         if (sys_time_get_system_time() - state.liveLastFrameUs >= LIVE_STALL_TIMEOUT_US) {
            logWarn("[yt-live-end] LIVE segment HTTP 404 persisted without new frames; verifying broadcast status\n");
            scheduleLiveEndCheck();
         }
         // A 404 for one numbered segment can be a transient edge condition. Wait for
         // the existing no-frame timeout before deciding between LIVE end and renewal.
      } else {
         if (error >= 400)
            logWarn("[yt-live-refresh] LIVE HTTP %d; renewing playback session\n", error);
         else
            logWarn("[yt-live-refresh] LIVE transport failed; renewing playback session\n");
         scheduleLiveRefresh();
      }
   }

   // VOD transport and HTTP failures use the same bounded full-session replacement strategy.
   // Decoder/demux failures without a source error remain separate and do not trigger network retries.
   if (state.stage == STAGE_PLAYING && !state.isLive && state.player &&
       isVideoPlayerFailed(state.player) && getVideoPlayerSourceError(state.player) != 0) {
      int error = getVideoPlayerSourceError(state.player);
      if (error >= 400)
         logWarn("[yt-vod-refresh] VOD HTTP %d; renewing playback session\n", error);
      else
         logWarn("[yt-vod-refresh] VOD transport failed; renewing playback session\n");
      scheduleVodHttpRefresh();
   }

   // A LIVE source can stop delivering frames without transitioning the player to failed.
   // Detect that separately from HTTP errors, while respecting pause and user seek operations.
   if (state.stage == STAGE_PLAYING && state.isLive && !state.liveEnded &&
       !state.liveEndDetected && !state.liveEndCheckActive && state.player && state.firstFrameSeen) {
      uint64_t liveNow = sys_time_get_system_time();
      if (isVideoPaused(state.player) || state.seeking) {
         state.liveLastFrameUs = liveNow;
      } else if (state.liveLastFrameUs &&
                 liveNow - state.liveLastFrameUs >= LIVE_STALL_TIMEOUT_US) {
         logWarn("[yt-live-end] no new LIVE frame for %llu ms; verifying broadcast status\n",
                 (unsigned long long)(LIVE_STALL_TIMEOUT_US / 1000ULL));
         scheduleLiveEndCheck();
      }
   }

   // Complete the audio-only handoff. The same VideoPlayer and H.264 decoder remain in place.
   if (state.audioSwitchDone && state.audioSwitchThreadActive) {
      joinThread(state.audioSwitchTid);
      state.audioSwitchThreadActive = 0;
      state.message[0] = 0;
      if (state.audioSwitchFallback) {
         state.audioPick = state.audioSwitchTarget;
         state.audItag = state.audioItags[state.audioPick];
         refreshPlaybackInfoHud();
         showControls();
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
      } else {
         showToast(LOC(STR_AUDIO_SWITCH_FAILED_VIDEO_CONTINUES));
      }
      if (!state.audioSwitchWasPaused && state.player) setVideoPaused(state.player, 0);
      if (!state.audioSwitchFallback && !state.audioFallbackAttempted) {
         state.audioFallbackAttempted = 1;
         int lowerProfile = findLowerResolutionProfile();
         if (lowerProfile >= 0) startResolutionSwitch(lowerProfile, 1);
      }
   }

   // Keep the safety warning visible long enough to read. This is latched per playback screen
   // because the player may be destroyed during the subsequent quality fallback.
   if (state.stage == STAGE_PLAYING && state.player && !state.decoderStopWarningShown &&
       isVideoPlayerDecoderStopUnsafe(state.player)) {
      showToastFor(LOC(STR_DECODER_FAILED_RESTART), 5000000ULL);
      state.decoderStopWarningShown = 1;
   }

   // Continuous per-playback quality adaptation. Each decision is based on a
   // complete measurement window; successful switches are monotonic (never up)
   // and separated by a cooldown to avoid oscillation.
   if (state.fpsFallbackRequested && state.stage == STAGE_PLAYING && state.player &&
       !state.resolutionSwitchThreadActive && !state.audioSwitchThreadActive &&
       !state.seeking && !isVideoPaused(state.player)) {
      int profile = state.fpsAdaptiveSwitches < 3 ? findAdaptiveFallbackProfile() : -1;
      state.fpsFallbackRequested = 0;
      if (profile >= 0) {
         logWarn("[yt-fps-adaptive] switching itag=%d %dp@%d (from itag=%d %dp@%d, switch=%d)\n",
                 state.videoItags[profile], state.videoHeights[profile], state.videoFps[profile],
                 state.vidItag, state.vidH, state.vidFps, state.fpsAdaptiveSwitches + 1);
         if (startResolutionSwitch(profile, 3)) {
            state.fpsAdaptiveSwitches++;
            state.fpsAdaptiveLastSwitchUs = sys_time_get_system_time();
         } else {
            logWarn("[yt-fps-adaptive] quality switch could not start\n");
         }
      } else {
         state.fpsAdaptiveSwitches = 3;
         logWarn("[yt-fps-adaptive] no lower-cost rendition available; keeping current stream\n");
      }
   }

   // A fatal TEE decode error triggers an invisible handoff to a lower rendition.
   // Keep the status label on LOC(STR_LOADING_ELLIPSIS) and never retry the failed TEE URL.
   if (state.stage == STAGE_PLAYING && state.player &&
       !state.teeRecoveryAttempted && !state.resolutionSwitchThreadActive &&
       !state.audioSwitchThreadActive && !state.seeking &&
       isVideoPlayerTeeFailed(state.player)) {
      state.teeRecoveryAttempted = 1;
      int lowerProfile = findLowerResolutionProfile();
      if (lowerProfile >= 0) {
         if (!startResolutionSwitch(lowerProfile, 2)) {
            strCopy(state.message, sizeof state.message, LOC(STR_VIDEO_DECODER_RECOVERY_COULD_NOT_START));
            state.stage = STAGE_FAILED;
         }
      } else {
         strCopy(state.message, sizeof state.message, LOC(STR_NO_COMPATIBLE_FALLBACK_QUALITY));
         state.stage = STAGE_FAILED;
      }
   }

   // Commit a resolution change only after its replacement player has reached a decoded pre-roll.
   if (state.resolutionSwitchDone && state.resolutionSwitchThreadActive) {
      joinThread(state.resolutionSwitchTid);
      state.resolutionSwitchThreadActive = 0;
      if (state.pendingResolutionPlayer) {
         int teeRecovery = state.resolutionSwitchTeeRecovery;
         int fpsFallback = state.resolutionSwitchFpsFallback;
         state.player = state.pendingResolutionPlayer;
         state.pendingResolutionPlayer = NULL;
         if (state.resolutionSwitchRestored) {
            state.resolutionIndex = state.resolutionSwitchOldIndex;
            if (preferredQualityHeight == 0)
               state.autoResolutionIndex = state.resolutionIndex;
         } else {
            state.resolutionIndex = state.resolutionSwitchTarget;
            if (preferredQualityHeight == 0)
               state.autoResolutionIndex = state.resolutionIndex;
            state.vidItag = state.videoItags[state.resolutionIndex];
            state.vidW = state.videoWidths[state.resolutionIndex];
            state.vidH = state.videoHeights[state.resolutionIndex];
            state.vidFps = state.videoFps[state.resolutionIndex];
            if (state.videoHeights[state.resolutionIndex] > 720) state.audioFallbackAttempted = 0;
            if (state.audioPick >= 0 && state.audioPick < state.audioCount)
               state.audItag = state.audioItags[state.audioPick];
            else if (state.fallbackAudioItag)
               state.audItag = state.fallbackAudioItag;
            refreshPlaybackInfoHud();
         }
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
         if (!state.resolutionSwitchWasPaused) setVideoPaused(state.player, 0);
         state.firstFrameSeen = 0;
         state.lastFrame = NULL;
         state.framesThisSecond = state.measuredFps = 0;
         state.fpsWindowStartUs = state.fpsWindowFrames = state.fpsWindowMaxGapUs = 0;
         state.fpsAdaptiveBadWindows = 0;
         state.fpsAdaptiveLowBufferWindows = 0;
         state.fpsFallbackRequested = 0;
         if (!fpsFallback && !teeRecovery) state.fpsAdaptiveSwitches = 0;
         state.fpsAdaptiveLastSwitchUs = sys_time_get_system_time();
         state.message[0] = 0;
         if (!teeRecovery) {
            if (fpsFallback) {
               if (state.resolutionSwitchRestored) state.fpsAdaptiveSwitches = 3;
               showToast(state.resolutionSwitchRestored
                         ? LOC(STR_QUALITY_FALLBACK_FAILED_RESTORED)
                         : LOC(STR_PLAYBACK_QUALITY_REDUCED));
            } else if (state.resolutionSwitchRestored) {
               showToast(LOC(STR_RESOLUTION_PREVIOUS_STREAM_RESTORED));
            } else {
               char notice[64];
               if (state.vidFps > 0)
                  snprintf(notice, sizeof notice, LOC(STR_VIDEO_FORMAT), state.vidH, state.vidFps);
               else
                  snprintf(notice, sizeof notice, LOC(STR_VIDEO_FORMAT_SIMPLE), state.vidH);
               showToast(notice);
            }
         }
         state.resolutionSwitchTeeRecovery = 0;
         state.resolutionSwitchFpsFallback = 0;
      } else {
         if (state.resolutionSwitchFpsFallback)
            showToast(LOC(STR_AUTOMATIC_QUALITY_FALLBACK_UNAVAILABLE));
         else if (!state.resolutionSwitchTeeRecovery)
            showToast(LOC(STR_RESOLUTION_SWITCH_RECOVERY_FAILED));
         state.resolutionSwitchTeeRecovery = 0;
         state.resolutionSwitchFpsFallback = 0;
      }
   }

   // A track can pass the initial AAC probe and then die during playback. Retry the current
   // audio track on the highest available lower video profile once; never loop retries.
   if (state.stage == STAGE_PLAYING && state.player &&
       (state.audioPick >= 0 || state.fallbackAudioUrl[0]) && !state.isLive &&
       !state.audioFallbackAttempted &&
       isVideoPlayerUsingSeparateAudio(state.player) &&
       !isVideoPlayerSeparateAudioAlive(state.player) && !isVideoEnded(state.player)) {
      float duration = getVideoDurationSeconds(state.player);
      float position = getVideoPositionSeconds(state.player);
      if (duration <= 0.0f || position < duration - 5.0f) {
         state.audioFallbackAttempted = 1;
         int lowerProfile = findLowerResolutionProfile();
         if (lowerProfile >= 0) startResolutionSwitch(lowerProfile, 1);
      }
   }

   if (state.nextWorkerDone && state.nextThreadActive) {
      joinThread(state.nextWorkerTid);
      state.nextThreadActive = 0;
      if (state.nextVideoId[0]) {
         if (!state.nextVideoTitle[0])
            snprintf(state.nextVideoTitle, sizeof state.nextVideoTitle, LOC(STR_VIDEO_ID_FORMAT), state.nextVideoId);
         setLabelText(&nextNoticeTitleLabel, state.nextVideoTitle);
         state.nextNoticeVisible = 1;
         state.nextNoticeStartedUs = sys_time_get_system_time();
         if (state.nextThumbJpeg && state.nextThumbJpegLen > 0) {
            if (requestImageMemAsync(state.nextThumbJpeg, (uint32_t)state.nextThumbJpegLen) == 0)
               state.nextImageRequested = 1;
            free(state.nextThumbJpeg);
            state.nextThumbJpeg = NULL;
            state.nextThumbJpegLen = 0;
         }
      } else {
         showToast(LOC(STR_NO_NEXT_RECOMMENDATION));
      }
   }

   if (state.nextImageRequested && !state.nextThumbTexture.offset) {
      ImageBuffer decoded;
      int imageResult = pollImageAsync(&decoded);
      if (imageResult == 1) {
         state.nextThumbTexture = uploadImageBuffer(&decoded);
         freeImageBuffer(&decoded);
         state.nextImageRequested = 0;
      } else if (imageResult < 0) {
         state.nextImageRequested = 0;
      }
   }

   if (state.nextNoticeVisible &&
       sys_time_get_system_time() - state.nextNoticeStartedUs >= AUTOPLAY_NOTICE_US) {
      char nextId[16];
      strCopy(nextId, sizeof nextId, state.nextVideoId);
      state.nextNoticeVisible = 0;
      if (state.nextThumbTexture.offset) retireGfxTexture(&state.nextThumbTexture);
      if (playlistQueueCount > 0) {
         playlistQueueCount = 0;
         playlistQueueCursor = -1;
      }
      requestPlaybackCancel();
      popScreen();
      launchVideo(nextId, 1);
      return;
   }

   // reap the sponsorblock fetch; its segments are safe to read once joined
   if (state.sbWorkerDone && state.sbThreadActive) {
      joinThread(state.sbWorkerTid);
      state.sbThreadActive = 0;
      state.sbReady = 1;
   }

   // Adopt the chapter prefetch started when Options was opened, or an on-demand fetch.
   if (state.chWorkerDone && state.chThreadActive) {
      joinThread(state.chWorkerTid);
      state.chThreadActive = 0;
      state.chapters = state.fetchedChapters;   // safe to adopt now the worker is reaped
   }

   // Refresh the combined panel texture after the asynchronous likes lookup completes.
   if (state.metadataWorkerDone && state.metadataThreadActive) {
      joinThread(state.metadataWorkerTid);
      state.metadataThreadActive = 0;
      state.descriptionRendered = 0;
      if (state.descriptionVisible) renderDescription();
   }

   // A synthetic translated track keeps working after its first cues are published.
   // A changed selection cancels the worker; its partial result is never reused.
   if (state.subThreadActive && !state.subWorkerDone) {
      if (state.subtitleFetched != state.subtitlePick) state.subtitleTranslation.cancel = 1;
      if (state.subtitleTranslation.cancel) state.subtitlesReady = 0;
      else if (state.subtitleTranslation.published && !state.subtitlesReady) {
         __sync_synchronize();
         state.subtitlesReady = 1;
      }
   }
   if (state.subtitlesReady && state.subtitleTranslation.stopped &&
       !state.subtitleTranslationStoppedShown && state.subtitleFetched == state.subtitlePick) {
      state.subtitleTranslationStoppedShown = 1;
      showToast(LOC(STR_TRANSLATION_STOPPED));
   }
   if (state.subWorkerDone && state.subThreadActive) {
      joinThread(state.subWorkerTid);
      state.subThreadActive = 0;
      state.subtitlesReady = (state.subtitleFetched == state.subtitlePick) &&
                             !state.subtitleTranslation.cancel;
      if (!state.subtitlesReady) state.subtitleFetched = -1;
      else if (state.subtitleTrack->count == 0) {
         int failedTrack = state.subtitleFetched;
         int translated = failedTrack >= 0 && failedTrack < state.captionCount &&
                          subtitleIsAutoTranslated(&state.captionTracks[failedTrack]);
         int fetchResult = state.subtitleFetchResult;
         state.subtitlePick = state.subtitleFetched = -1;
         subtitlesEnabled = 0;
         lastSubtitlePick = -1;
         state.subtitlesReady = 0;
         refreshPlaybackInfoHud();
         if (translated && fetchResult == -429)
            showToast(LOC(STR_GOOGLE_TRANSLATE_RATE_LIMITED));
         else if (translated)
            showToast(LOC(STR_AUTOMATIC_TRANSLATION_UNAVAILABLE));
         else
            showToast(LOC(STR_SUBTITLES_UNAVAILABLE));
      }
   }
   if (autoTranslateRefreshPending && !state.subThreadActive)
      refreshAutoTranslatedCaptionTrack();

   if (subtitlesEnabled && state.subtitlePick >= 0 &&
       state.subtitlePick != state.subtitleFetched && !state.subThreadActive) {
      if (!state.subtitleTrack) state.subtitleTrack = malloc(sizeof *state.subtitleTrack);
      if (state.subtitleTrack) {
         state.subtitlesReady = 0;
         state.subtitleScanFrom = -1;
         state.subtitleDisplayedCue = -1;
         state.subtitleDisplayedPending = 0;
         state.subtitleLastPosition = 0.0f;
         state.subWorkerDone = 0;
         state.subtitleFetchResult = 0;
         memset(&state.subtitleTranslation, 0, sizeof state.subtitleTranslation);
         state.subtitleTranslation.position = state.player ? getVideoPositionSeconds(state.player) : 0.0f;
         state.subtitleTranslationStoppedShown = 0;
         state.subtitleFetched = state.subtitlePick;
         state.subThreadActive = (spawnJoinableThread(&state.subWorkerTid, subtitleWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-subs") == 0);
         if (!state.subThreadActive) state.subtitleFetched = -1;
      }
   }

   // Keep the translator focused on the next visible chunk rather than translating from the start.
   if (state.subThreadActive && state.player)
      state.subtitleTranslation.position = getVideoPositionSeconds(state.player);

   if (state.subtitlesReady && state.subtitlePick >= 0 && state.stage == STAGE_PLAYING && state.player) {
      subtitleLabel.size = playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4);
#if BUILD_DIAG_SUBTITLE_RENDER
      uint64_t diagStartUs = sys_time_get_system_time();
      float subtitlePosition = getVideoPositionSeconds(state.player);
      recordSubtitleRenderDiag(SUB_DIAG_CLOCK, sys_time_get_system_time() - diagStartUs);
      diagStartUs = sys_time_get_system_time();
#else
      float subtitlePosition = getVideoPositionSeconds(state.player);
#endif
      if (subtitlePosition + 0.05f < state.subtitleLastPosition) state.subtitleScanFrom = -1;
      state.subtitleLastPosition = subtitlePosition;
      int activeCue = -1, activePending = 0;
      const char *subtitleCueText = getSubtitleCueText(state.subtitleTrack, subtitlePosition,
                                    &state.subtitleScanFrom, &activeCue, &activePending);
#if BUILD_DIAG_SUBTITLE_RENDER
      recordSubtitleRenderDiag(SUB_DIAG_CUE, sys_time_get_system_time() - diagStartUs);
      diagStartUs = sys_time_get_system_time();
#endif
      // The cue text is immutable once published. Only rebuild its texture when the active cue
      // changes or a translated cue flips from pending to ready.
      if (activeCue != state.subtitleDisplayedCue || activePending != state.subtitleDisplayedPending) {
         setLabelText(&subtitleLabel, subtitleCueText);
         state.subtitleDisplayedCue = activeCue;
         state.subtitleDisplayedPending = activePending;
      }
      // The cue text often stays unchanged while the user changes subtitle size.
      // Rebuild its texture only when the layout inputs actually changed.
      refreshLabelLayout(&subtitleLabel);
#if BUILD_DIAG_SUBTITLE_RENDER
      recordSubtitleRenderDiag(SUB_DIAG_LABEL, sys_time_get_system_time() - diagStartUs);
#endif
   }

   int playbackInputBlocked = overlayHadPad || loadingPauseInput;
   if (suppressPlaybackCrossUntilRelease) {
      PadButtonState crossState = getPadButtonState(PAD_BTN_CROSS);
      playbackInputBlocked = 1;
      if (crossState == PAD_BUTTON_STATE_RELEASED || crossState == PAD_BUTTON_STATE_UP)
         suppressPlaybackCrossUntilRelease = 0;
   }
   if (state.stage == STAGE_PLAYING && state.player && !playbackInputBlocked &&
       !state.descriptionVisible && !state.chaptersVisible)
      handlePlaybackInput();
   if (state.sbReady && state.stage == STAGE_PLAYING && state.player && !isVideoPaused(state.player) && !state.seeking) {
      autoSkipSponsor();
      int askIndex = findAskSponsorSegment();
      if (askIndex >= 0 && askIndex != state.askPromptShown) {
         state.askPromptShown = askIndex;
         state.askPromptStartedUs = sys_time_get_system_time();
      } else if (askIndex < 0) {
         state.askPromptShown = -1;
         state.askPromptStartedUs = 0;
      }
   }

   // Playlist order takes precedence over the global autoplay setting.
   if (state.stage == STAGE_PLAYING && state.player && !requestedIsLocal &&
       playlistQueueCount > 0 && playlistQueueCursor >= 0 &&
       playlistQueueCursor + 1 < playlistQueueCount &&
       (!state.isLive || state.liveEnded || state.liveEndDetected) &&
       (isVideoEnded(state.player) || state.liveEndDetected) && !state.nextAttempted) {
      state.nextAttempted = 1;
      launchPlaylistQueueIndex(playlistQueueCursor + 1);
      return;
   }

   // With autoplay disabled, ending a VOD behaves like pressing Circle:
   // return to the screen that launched this video instead of requesting /next.
   if (state.stage == STAGE_PLAYING && state.player &&
       (!state.isLive || state.liveEnded || state.liveEndDetected) &&
       (isVideoEnded(state.player) || state.liveEndDetected) &&
       !getAutoplayEnabled() && !state.nextAttempted) {
      requestPlaybackCancel();
      popScreen();
      return;
   }

   // Automatic playback: ask /next once after a VOD reaches its end. Network failure is non-fatal.
   if (state.stage == STAGE_PLAYING && state.player &&
       (!state.isLive || state.liveEnded || state.liveEndDetected) &&
       (isVideoEnded(state.player) || state.liveEndDetected) &&
       !state.nextAttempted && !state.nextThreadActive) {
      state.nextAttempted = 1;
      state.nextWorkerDone = 0;
      state.nextVideoId[0] = 0;
      state.nextThreadActive = (spawnJoinableThread(&state.nextWorkerTid, autoplayWorker, 0,
                                THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-autoplay") == 0);
      if (!state.nextThreadActive) showToast(LOC(STR_AUTOPLAY_UNAVAILABLE));
   }

   // status text: the failure reason, or LOC(STR_LOADING_ELLIPSIS) until playback. setLabelText skips unchanged
   // text, so calling it each frame only rasterises on a real change.
   if (state.audioSwitchThreadActive) setLabelText(&statusLabel, state.message[0] ? state.message : LOC(STR_LOADING_AUDIO));
   else if (state.resolutionSwitchThreadActive) {
      if (state.resolutionSwitchTeeRecovery) {
         setLabelText(&statusLabel, LOC(STR_LOADING));
      } else {
         setLabelText(&statusLabel, LOC(STR_CHANGING_RESOLUTION));
      }
   } else if (state.stage == STAGE_FAILED) setLabelText(&statusLabel, state.message);
   else if (state.stage != STAGE_PLAYING) {
      if (state.message[0] && strcmp(state.message, LOC(STR_LOADING_ELLIPSIS)) != 0) {
         setLabelText(&statusLabel, state.message);
      } else {
         setLabelText(&statusLabel, LOC(STR_LOADING));
      }
   }
}


// Measure the real presented frame rate from changes in the frame pointer.
// The adaptive window is repeated throughout playback, not just at startup.
static void measureFps(const uint8_t *frame)
{
   static uint64_t diagWindowUs;
   static uint64_t diagLastFrameUs;
   static uint64_t diagMaxFrameGapUs;
   static uint64_t diagCalls;
   static uint64_t diagPointerChanges;
   uint64_t now = sys_time_get_system_time();
   int newFrame = frame && frame != state.lastFrame;
   diagCalls++;
   if (newFrame) {
      uint64_t gap = diagLastFrameUs && now >= diagLastFrameUs ? now - diagLastFrameUs : 0;
      if (gap > diagMaxFrameGapUs) diagMaxFrameGapUs = gap;
      if (state.fpsWindowStartUs && gap > state.fpsWindowMaxGapUs)
         state.fpsWindowMaxGapUs = gap;
      diagLastFrameUs = now;
      diagPointerChanges++;
      state.framesThisSecond++;
      state.lastFrame = frame;
      if (state.isLive && state.stage == STAGE_PLAYING) state.liveLastFrameUs = now;
   }
   if (now - state.fpsTickUs >= 1000000) {
      state.measuredFps = state.framesThisSecond;
      state.framesThisSecond = 0;
      state.fpsTickUs = now;
   }
   if (!diagWindowUs) diagWindowUs = now;
   if (state.isLive && now - diagWindowUs >= 2000000ull) {
      logInfo("[render-live-diag] spanMs=%llu calls=%llu pointerChanges=%llu measuredFps=%d lastFrameAgeMs=%llu maxFrameGapMs=%llu framePtr=%p stage=%d live=%d paused=%d seeking=%d\n",
              (unsigned long long)((now-diagWindowUs)/1000ull), (unsigned long long)diagCalls,
              (unsigned long long)diagPointerChanges, state.measuredFps,
              (unsigned long long)(diagLastFrameUs && now >= diagLastFrameUs ? (now-diagLastFrameUs)/1000ull : 0),
              (unsigned long long)(diagMaxFrameGapUs/1000ull), (void *)frame, state.stage, state.isLive,
              state.player ? isVideoPaused(state.player) : 0, state.seeking);
      diagWindowUs = now;
      diagCalls = diagPointerChanges = diagMaxFrameGapUs = 0;
   }

   if (state.vidFps < 24 || state.stage != STAGE_PLAYING || !state.player ||
       isVideoPaused(state.player) || state.seeking || state.resolutionSwitchThreadActive ||
       state.audioSwitchThreadActive) {
      state.fpsWindowStartUs = state.fpsWindowFrames = state.fpsWindowMaxGapUs = 0;
      state.fpsAdaptiveBadWindows = 0;
      state.fpsAdaptiveLowBufferWindows = 0;
      state.fpsFallbackRequested = 0;
      return;
   }
   if (newFrame) {
      if (!state.fpsWindowStartUs) {
         state.fpsWindowStartUs = now;
         state.fpsWindowFrames = 0;
         state.fpsWindowMaxGapUs = 0;
      }
      state.fpsWindowFrames++;
   }
   if (state.fpsWindowStartUs && now - state.fpsWindowStartUs >= 8000000ull) {
      uint64_t elapsed = now - state.fpsWindowStartUs;
      int averageFps = elapsed ? (int)((state.fpsWindowFrames * 1000000ull + elapsed / 2) / elapsed) : 0;
      int minimumFps = (state.vidFps * 4 + 4) / 5;
      uint64_t maxGapUs = state.fpsWindowMaxGapUs;
      int lowFps = averageFps < minimumFps;
      // Never downgrade from one window, including a severe (<60%) window.
      // LIVE decisions also inspect the actual byte/segment cushion.
      int degradedWindow = lowFps || (maxGapUs >= 1000000ull && averageFps * 10 < state.vidFps * 9);
      VideoLiveBufferStatus bufferStatus;
      int haveLiveBuffer = state.isLive && getVideoPlayerLiveBufferStatus(state.player, &bufferStatus);
      int lowLiveBuffer = haveLiveBuffer && bufferStatus.aheadBytes < (2ull * 1024ull * 1024ull) &&
                          bufferStatus.segmentsAhead <= 1;
      if (degradedWindow) state.fpsAdaptiveBadWindows++;
      else state.fpsAdaptiveBadWindows = 0;
      if (lowLiveBuffer) state.fpsAdaptiveLowBufferWindows++;
      else state.fpsAdaptiveLowBufferWindows = 0;
      int requiredBadWindows = haveLiveBuffer && state.fpsAdaptiveLowBufferWindows < 2 ? 3 : 2;
      int fallbackEligible = state.fpsAdaptiveBadWindows >= requiredBadWindows;
      int cooldownReady = !state.fpsAdaptiveLastSwitchUs ||
                          (now >= state.fpsAdaptiveLastSwitchUs &&
                           now - state.fpsAdaptiveLastSwitchUs >= 20000000ull);
      uint64_t sinceSwitch = state.fpsAdaptiveLastSwitchUs && now >= state.fpsAdaptiveLastSwitchUs
                           ? now - state.fpsAdaptiveLastSwitchUs : 0;
      logInfo("[yt-fps-adaptive] spanMs=%llu averageFps=%d targetFps=%d minimumFps=%d maxGapMs=%llu lowFps=%d badWindows=%d lowBuffer=%d lowBufferWindows=%d aheadKB=%llu segmentsAhead=%d requiredWindows=%d fallbackEligible=%d switches=%d cooldownMs=%llu\n",
              (unsigned long long)(elapsed/1000ull), averageFps, state.vidFps, minimumFps,
              (unsigned long long)(maxGapUs/1000ull), lowFps, state.fpsAdaptiveBadWindows,
              lowLiveBuffer, state.fpsAdaptiveLowBufferWindows,
              (unsigned long long)(haveLiveBuffer ? bufferStatus.aheadBytes/1024ull : 0),
              haveLiveBuffer ? bufferStatus.segmentsAhead : -1, requiredBadWindows,
              fallbackEligible, state.fpsAdaptiveSwitches, (unsigned long long)(sinceSwitch/1000ull));
      if (fallbackEligible && state.fpsAdaptiveSwitches < 3 && cooldownReady)
         state.fpsFallbackRequested = 1;
      state.fpsWindowStartUs = now;
      state.fpsWindowFrames = 0;
      state.fpsWindowMaxGapUs = 0;
   }
}

// Time the CPU-side YUV draw submission. This does not measure completion on
// the RSX; it isolates the cost of preparing/submitting the video quad.
#if BUILD_DIAG_PLAY_RENDER
static void recordVideoRenderDraw(uint64_t pullUs, uint64_t drawUs, int frameW, int frameH)
{
   static uint64_t windowStartUs;
   static uint64_t pullTotalUs, pullMaxUs;
   static uint64_t drawTotalUs, drawMaxUs;
   static uint64_t calls;
   uint64_t now = sys_time_get_system_time();
   if (!windowStartUs) windowStartUs = now;
   pullTotalUs += pullUs;
   drawTotalUs += drawUs;
   if (pullUs > pullMaxUs) pullMaxUs = pullUs;
   if (drawUs > drawMaxUs) drawMaxUs = drawUs;
   calls++;
   if (now - windowStartUs >= 5000000ull) {
      logInfo("[video-render-diag] spanMs=%llu calls=%llu framePullAvgUs=%llu framePullMaxUs=%llu yuvSubmitAvgUs=%llu yuvSubmitMaxUs=%llu source=%dx%d\n",
              (unsigned long long)((now-windowStartUs)/1000ull), (unsigned long long)calls,
              (unsigned long long)(calls ? pullTotalUs/calls : 0), (unsigned long long)pullMaxUs,
              (unsigned long long)(calls ? drawTotalUs/calls : 0), (unsigned long long)drawMaxUs,
              frameW, frameH);
      windowStartUs = now;
      pullTotalUs = pullMaxUs = drawTotalUs = drawMaxUs = calls = 0;
   }
}
#endif

// rebuild the overlay lines. setLabelText skips unchanged text, so calling this per frame only
// rasterises when a value actually moves (fps + time once a second).
static void updateStatLabels(void)
{
   char line[160];
   int rate = 0, channels = 0;
   const char *audioName = NULL;
   getAudioTrackInfo(state.player, &rate, &channels);
   float pos = getVideoPositionSeconds(state.player), duration = getVideoDurationSeconds(state.player);

   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      audioName = state.audioNames[state.audioPick];

   snprintf(line, sizeof line, LOC(STR_VIDEO_STATS_FORMAT), state.vidItag, state.vidW, state.vidH);
   setLabelText(&statLabels[0], line);
   snprintf(line, sizeof line, LOC(STR_FPS_STATS_FORMAT), state.measuredFps, state.vidFps);
   setLabelText(&statLabels[1], line);
   if (state.audItag) {
      if (audioName && audioName[0])
         snprintf(line, sizeof line, LOC(STR_AUDIO_STATS_WITH_NAME_FORMAT),
                  state.audItag, audioName, rate, channels);
      else
         snprintf(line, sizeof line, LOC(STR_AUDIO_STATS_FORMAT),
                  state.audItag, rate, channels);
   } else {
      strCopy(line, sizeof line, LOC(STR_AUDIO_NONE_STATS));
   }
   setLabelText(&statLabels[2], line);
   snprintf(line, sizeof line, LOC(STR_TIME_STATS_FORMAT), (int)pos / 60, (int)pos % 60, (int)duration / 60, (int)duration % 60);
   setLabelText(&statLabels[3], line);
}

static void drawStatsOverlay(void)
{
   int panelX = 40, y = 44;
   int textX = panelX + statsTextPadding;
   int lineHeight = playbackSubtitleFontSize(30);
   int totalHeight = 0;
   int widestText = 0;
   for (int i = 0; i < STAT_LINES; i++) {
      int height = statLabels[i].tt.tex.h;
      int width = statLabels[i].tt.tex.w;
      totalHeight += height > lineHeight ? height : lineHeight;
      if (width > widestText) widestText = width;
      if (i + 1 < STAT_LINES) totalHeight += 4;
   }
   int panelWidth = widestText + 2 * statsTextPadding;
   fillGfxRectangle(panelX, y - 12, panelWidth, totalHeight + 24, activeTheme->badgeFill);
   for (int i = 0; i < STAT_LINES; i++) {
      int height = statLabels[i].tt.tex.h;
      drawLabelAt(&statLabels[i], textX, y);
      y += (height > lineHeight ? height : lineHeight) + 4;
   }
}

// seek bar + time show while scrubbing / paused / ended, else auto-hide after the last activity
static int controlsVisible(void)
{
   if (state.controlsDismissed) return 0;
   if (state.stage != STAGE_PLAYING || !state.player) return 0;
   uint64_t now = sys_time_get_system_time();
   if (isVideoPaused(state.player) || isVideoEnded(state.player) || state.seeking) return 1;
   if (state.hudShownUs != 0 &&
       now - state.hudShownUs < (uint64_t)getOsdTimeout() * 1000000ull) return 1;
   return state.controlsShownUs != 0 &&
          now - state.controlsShownUs < (uint64_t)getOsdTimeout() * 1000000ull;
}

static int fullPlaybackHudVisible(void)
{
   if (state.controlsDismissed) return 0;
   if (isVideoPaused(state.player)) return 1;
   return state.hudShownUs != 0 &&
          sys_time_get_system_time() - state.hudShownUs < (uint64_t)getOsdTimeout() * 1000000ull;
}

// paint each sponsor segment onto the bar in its category colour, over the progress fill so it stays
// visible in the already-watched region (as YouTube's SponsorBlock overlay does).
static void drawSponsorSegments(int barLeft, int span, int barTopY, int barH, float duration)
{
   if (duration <= 0.0f) return;
   for (int i = 0; i < state.sb.count; i++) {
      const SponsorSegment *segment = &state.sb.segments[i];
      float startFrac = segment->start / duration, endFrac = segment->end / duration;
      if (startFrac < 0.0f) startFrac = 0.0f;
      if (endFrac > 1.0f) endFrac = 1.0f;
      int segX = barLeft + (int)(startFrac * span);
      int segW = (int)((endFrac - startFrac) * span);
      if (segW < 2) segW = 2;
      fillGfxRectangle(segX, barTopY, segW, barH, getSponsorCategoryColor(segment->category));
   }
}

// Keep the LIVE badge brand-red in every theme, with compact 6x4 padding.
static void drawLiveBadgeAt(int x, int y)
{
   if (liveBadgeLanguage != getAppLanguage()) {
      setLabelText(&liveBadgeLabel, LOC(STR_LIVE_BADGE));
      setLabelScaleWithUi(&liveBadgeLabel, 1);
      liveBadgeLanguage = getAppLanguage();
   }
   const int padX = 6, padY = 4;
   int width = liveBadgeLabel.tt.tex.w + 2 * padX;
   int height = liveBadgeLabel.tt.tex.h + 2 * padY;
   fillGfxRectangle(x, y, width, height, 0xFFFF0000);
   setLabelColor(&liveBadgeLabel, 0xFFFFFFFF);
   drawLabelAt(&liveBadgeLabel, x + padX, y + padY);
}

// Keep the seek bar, subtitle baseline and two-row playback hints coordinated.
// The bar is raised to reserve a dedicated footer area for the larger controls.
static int playbackSeekBarY(void)
{
   if (state.screenH <= 576) return (int)(state.screenH * 0.77f);
   if (state.screenH >= 900) return (int)(state.screenH * 0.86f);
   return (int)(state.screenH * 0.82f);
}

// a red YouTube-style progress bar with current/total time and sponsor segments.
static void drawSeekBar(void)
{
   float duration = state.isLive ? liveTimelineLength() : getVideoDurationSeconds(state.player);
   float position = state.seeking ? state.seekTarget :
                    (state.isLive ? liveTimelinePosition() : getVideoPositionSeconds(state.player));
   float progress = duration > 0.0f ? position / duration : 0.0f;
   if (progress < 0.0f) progress = 0.0f;
   if (progress > 1.0f) progress = 1.0f;

   int barLeft  = (int)(state.screenW * 0.24f);
   int barRight = state.screenW - barLeft;
   int span     = barRight - barLeft;
   int barY     = playbackSeekBarY();
   int barH     = 8;
   int barTopY  = barY - barH / 2;
   int filledW  = (int)(progress * span);

   fillGfxRectangle(barLeft, barTopY, span, barH, activeTheme->seekTrack);
   fillGfxRectangle(barLeft, barTopY, filledW, barH, activeTheme->accent);   // progress (youtube red)
   if (!state.isLive && state.sbReady) drawSponsorSegments(barLeft, span, barTopY, barH, duration);
   if (!state.isLive && duration > 0.0f)   // chapter boundaries notch the bar, youtube-style
      for (int i = 1; i < state.chapters.count; i++)
         fillGfxRectangle(barLeft + (int)(state.chapters.chapters[i].start / duration * span), barTopY, 3, barH, activeTheme->seekNotch);
   fillGfxRectangle(barLeft + filledW - 3, barY - 11, 6, 22, activeTheme->textPrimary);   // scrubber handle

   char text[16];
   formatTime(text, sizeof text, (int)position);            setLabelText(&timeLeftLabel, text);
   formatTime(text, sizeof text, (int)(duration + 0.5f));   setLabelText(&timeRightLabel, text);
   drawLabelAt(&timeLeftLabel,  barLeft - 20 - timeLeftLabel.tt.tex.w, barY - timeLeftLabel.tt.tex.h / 2);
   int rightTimeX = barRight + 20;
   int rightTimeY = barY - timeRightLabel.tt.tex.h / 2;
   drawLabelAt(&timeRightLabel, rightTimeX, rightTimeY);

   if (state.isLive && !state.liveEnded && isLiveBehindAtEdge(duration - position)) {
      int badgeX = rightTimeX + timeRightLabel.tt.tex.w + 10;
      int badgeW = liveBadgeLabel.tt.tex.w + 12;
      int maxX = state.screenW - 40 - badgeW;
      if (badgeX > maxX) badgeX = maxX;
      drawLiveBadgeAt(badgeX, barY - (liveBadgeLabel.tt.tex.h + 8) / 2);
   }
}

// a top-of-screen HUD badge in the thumbnails' duration-tag style: label on a translucent black box,
// with breathing room either side so the text never touches the box edges.
#define BADGE_PAD_X 14

static void drawBadge(Label *label, int x, int y)
{
   fillGfxRectangle(x, y, label->tt.tex.w + 2 * BADGE_PAD_X, label->tt.tex.h + 8, activeTheme->badgeFill);
   drawLabelAt(label, x + BADGE_PAD_X, y + 4);
}

// title top-left + chosen subtitle language top-right, shown whenever the seek bar is up.
static void drawTitle(void)
{
   int y = 32;
   if (titleLabel.tt.tex.w > 0) {
      drawBadge(&titleLabel, 40, y);
      y += titleLabel.tt.tex.h + 20;
   }
   if (authorLabel.tt.tex.w > 0) drawBadge(&authorLabel, 40, y);
}

static void drawPlaybackInfoHud(void)
{
   if (playbackInfoLabel.tt.tex.w > 0)
      drawBadge(&playbackInfoLabel,
                state.screenW - 40 - (playbackInfoLabel.tt.tex.w + 2 * BADGE_PAD_X), 32);
}

// the active subtitle cue, centred above the seek bar on a dim backdrop.
static void drawSubtitle(void)
{
   if (state.subtitlePick < 0 || subtitleLabel.tt.tex.w == 0) return;
#if BUILD_DIAG_SUBTITLE_RENDER
   uint64_t subtitleDrawStartUs = sys_time_get_system_time();
#endif
   static const uint32_t textColors[] = { 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFF4488FF, 0xFFFF4444 };
   static const uint32_t bgColors[] = { 0xFF000000, 0xFF303030, 0xFF777777, 0xFF102040, 0x00000000 };
   prepareSubtitleLabel(&subtitleLabel, textColors[getSubtitleStyle(0)]);
   int padL, padT, padR, padB;
   getSubtitleLabelPadding(&subtitleLabel, &padL, &padT, &padR, &padB);
   int textW = subtitleLabel.tt.tex.w - padL - padR;
   int textH = subtitleLabel.tt.tex.h - padT - padB;
   int textX = state.screenW / 2 - textW / 2;
   // Move captions above the seek HUD only while that HUD is actually visible.
   // Once the OSD times out (or a full-screen overlay hides it), restore the
   // original subtitle baseline instead of leaving captions permanently raised.
   int seekHudVisible = controlsVisible() && !state.chaptersVisible &&
                        !state.descriptionVisible && !optionsVisible;
   int subtitleAnchorY = seekHudVisible
      ? playbackSeekBarY()
      : (int)(state.screenH * 0.94f);
   int textY = subtitleAnchorY - 40 - textH - getSubtitleOffset();
   int labelY = textY - padT;
   if (labelY < 12) labelY = 12;
   int maxY = state.screenH - subtitleLabel.tt.tex.h - 12;
   if (labelY > maxY) labelY = maxY;
   textY = labelY + padT;
   static const uint32_t bgOpacity[] = { 0, 64, 128, 204, 255 };
   uint32_t bg = bgColors[getSubtitleStyle(1)];
   bg = (bg & 0x00FFFFFF) | (bgOpacity[getSubtitleStyle(2)] << 24);
   fillGfxRectangle(textX - 12, textY - 6, textW + 24, textH + 12, bg);
   drawSubtitleLabelWithHighlight(&subtitleLabel, textX - padL, labelY);
#if BUILD_DIAG_SUBTITLE_RENDER
   recordSubtitleRenderDiag(SUB_DIAG_DRAW, sys_time_get_system_time() - subtitleDrawStartUs);
#endif
}

// the current bounded description page over a semi-transparent black layer.
static void drawDescription(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
   if (descriptionTexture.tex.w == 0) return;

   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   int visibleHeight = descriptionTexture.tex.h;
   if (visibleHeight > viewHeight) visibleHeight = viewHeight;
   if (visibleHeight <= 0) return;

   float sliceBottom = (float)visibleHeight / descriptionTexture.tex.h;
   drawGfxTexture(DESCRIPTION_MARGIN_X, DESCRIPTION_MARGIN_Y, descriptionTexture.tex.w, visibleHeight,
                  descriptionTexture.tex, 0.0f, 0.0f, 1.0f, sliceBottom, 0xFFFFFFFF, GFX_FILTER_NEAREST);
   char pageText[24];
   snprintf(pageText, sizeof pageText, "%d/%d", state.descriptionPage + 1, descriptionPageCount);
   setLabelText(&descriptionPageLabel, pageText);
   refreshLabelScale(&descriptionPageLabel);
   setLabelColor(&descriptionPageLabel, activeTheme->textSecondary);
   moveLabel(&descriptionPageLabel, state.screenW - DESCRIPTION_MARGIN_X - descriptionPageLabel.tt.tex.w,
             DESCRIPTION_MARGIN_Y - 34);
   drawLabel(&descriptionPageLabel);
}

// the chapter picker: a dim layer, then a centred column of rows windowed around the selection;
// the selected row gets a subtle highlight with a youtube-red accent on its left edge.
static void drawChapters(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);

   // window the rows around the selection so long lists stay reachable without a scrollbar
   int rows = state.chapters.count < CHAPTER_VISIBLE_ROWS ? state.chapters.count : CHAPTER_VISIBLE_ROWS;
   int firstRow = state.chapterSelected - rows / 2;
   if (firstRow > state.chapters.count - rows) firstRow = state.chapters.count - rows;
   if (firstRow < 0) firstRow = 0;

   int panelX = state.screenW / 2 - state.chapterPanelWidth / 2;
   int titleX = panelX + state.chapterTimeWidth + CHAPTER_TIME_GAP;
   int rowHeight = playbackChapterRowHeight();
   int topY   = state.screenH / 2 - rows * rowHeight / 2;

   for (int row = 0; row < rows; row++) {
      int index = firstRow + row;
      int rowY  = topY + row * rowHeight;
      if (index == state.chapterSelected) {
         fillGfxRectangle(panelX - 19, rowY, state.chapterPanelWidth + 43, rowHeight, activeTheme->rowHighlight);
         fillGfxRectangle(panelX - 24, rowY, 5, rowHeight, activeTheme->accent);   // red accent, flush with the highlight
      }
      drawLabelAt(&chapterTimeLabels[index], panelX + state.chapterTimeWidth - chapterTimeLabels[index].tt.tex.w,
                  getCenteredLabelY(&chapterTimeLabels[index], rowY, rowHeight));
      drawLabelAt(&chapterLabels[index], titleX, getCenteredLabelY(&chapterLabels[index], rowY, rowHeight));
   }
}

// A short Ask-category hint with the native PS3 Square button glyph.
static void drawAskSponsorPrompt(void)
{
   if (state.askPromptShown < 0 || state.askPromptStartedUs == 0 ||
       sys_time_get_system_time() - state.askPromptStartedUs >= ASK_PROMPT_VISIBLE_US)
      return;

   GfxTexture squareGlyph = getConsoleGlyph(GLYPH_SQUARE);
   int textH = askPromptPrefixLabel.tt.tex.h;
   if (askPromptSuffixLabel.tt.tex.h > textH) textH = askPromptSuffixLabel.tt.tex.h;
   if (textH < 1) return;

   int iconH = textH;
   int iconW = squareGlyph.h > 0 ? squareGlyph.w * iconH / squareGlyph.h : iconH;
   int gap = 8;
   int contentW = askPromptPrefixLabel.tt.tex.w + gap + iconW + gap + askPromptSuffixLabel.tt.tex.w;
   int boxW = contentW + 2 * BADGE_PAD_X;
   int boxH = textH + 8;
   int x = state.screenW - 40 - boxW;
   int y = fullPlaybackHudVisible() && playbackInfoLabel.tt.tex.h > 0
         ? 32 + playbackInfoLabel.tt.tex.h + 20 : 80;
   int maxY = state.screenH - boxH - 8;
   if (y > maxY) y = maxY;

   fillGfxRectangle(x, y, boxW, boxH, activeTheme->badgeFill);
   int textY = y + (boxH - askPromptPrefixLabel.tt.tex.h) / 2;
   drawLabelAt(&askPromptPrefixLabel, x + BADGE_PAD_X, textY);

   int iconX = x + BADGE_PAD_X + askPromptPrefixLabel.tt.tex.w + gap;
   int iconY = y + (boxH - iconH) / 2;
   if (squareGlyph.offset)
      drawGfxTexture(iconX, iconY, iconW, iconH, squareGlyph, 0.0f, 0.0f, 1.0f, 1.0f,
                     0xFFFFFFFF, GFX_FILTER_LINEAR);

   int suffixX = iconX + iconW + gap;
   int suffixY = y + (boxH - askPromptSuffixLabel.tt.tex.h) / 2;
   drawLabelAt(&askPromptSuffixLabel, suffixX, suffixY);
}

// a brief toast in the top-right notification spot, one badge row below the subtitle-language badge.
static void drawToast(void)
{
   uint64_t now = sys_time_get_system_time();
   if (state.liveReturnNoticeUs != 0 && now - state.liveReturnNoticeUs < 1000000ULL) {
      const int padX = 6, padY = 4, gap = 6;
      int backW = backToLabel.tt.tex.w + 2 * padX;
      int liveW = liveBadgeLabel.tt.tex.w + 2 * padX;
      int height = liveBadgeLabel.tt.tex.h + 2 * padY;
      int y = fullPlaybackHudVisible() && playbackInfoLabel.tt.tex.h > 0
            ? 32 + playbackInfoLabel.tt.tex.h + 20 : 80;
      int maxY = state.screenH - height - 8;
      if (y > maxY) y = maxY;
      int x = state.screenW - 40 - backW - gap - liveW;
      fillGfxRectangle(x, y, backW, height, activeTheme->badgeFill);
      drawLabelAt(&backToLabel, x + padX, y + padY);
      drawLiveBadgeAt(x + backW + gap, y);
      return;
   }

   uint64_t duration = state.toastDurationUs ? state.toastDurationUs : TOAST_VISIBLE_US;
   if (state.toastShownUs == 0 || now - state.toastShownUs >= duration) return;
   int y = fullPlaybackHudVisible() && playbackInfoLabel.tt.tex.h > 0
         ? 32 + playbackInfoLabel.tt.tex.h + 20 : 80;
   int maxY = state.screenH - toastLabel.tt.tex.h - 2 * BADGE_PAD_X - 8;
   if (y > maxY) y = maxY;
   drawBadge(&toastLabel, state.screenW - 40 - (toastLabel.tt.tex.w + 2 * BADGE_PAD_X), y);
}

// Fill the display while preserving the source aspect ratio. Any excess image area is
// positioned outside the screen and clipped by the RSX viewport (a centred crop).
static float clampCropPan(float value)
{
   if (value < -1.0f) return -1.0f;
   if (value >  1.0f) return  1.0f;
   return value;
}

// The right stick moves the oversized image within the crop window. The normalized
// offset is retained while playing and only affects the axis that actually overflows.
static void updateCropPan(void)
{
   uint64_t now = sys_time_get_system_time();
   float elapsed = cropPanLastUpdateUs ? (float)(now - cropPanLastUpdateUs) / 1000000.0f : 0.0f;
   cropPanLastUpdateUs = now;
   if (elapsed > 0.05f) elapsed = 0.05f;
   if (!cropToFillEnabled || elapsed <= 0.0f) return;

   Stick stick = getPadRightStick();
   const int deadZone = 20;
   float x = 0.0f, y = 0.0f;
   if (stick.x > deadZone) x = (float)(stick.x - deadZone) / (127 - deadZone);
   else if (stick.x < -deadZone) x = (float)(stick.x + deadZone) / (127 - deadZone);
   if (stick.y > deadZone) y = (float)(stick.y - deadZone) / (127 - deadZone);
   else if (stick.y < -deadZone) y = (float)(stick.y + deadZone) / (127 - deadZone);

   // Full travel takes roughly 1.6 seconds at maximum stick deflection.
   cropPanX = clampCropPan(cropPanX - x * 1.25f * elapsed);
   cropPanY = clampCropPan(cropPanY - y * 1.25f * elapsed);
}

static void getCropToFillRect(int frameW, int frameH, int *dx, int *dy, int *dw, int *dh)
{
   float frameAspect = (float)frameW / (float)frameH;
   float screenAspect = (float)state.screenW / (float)state.screenH;

   if (frameAspect > screenAspect) {
      *dh = state.screenH;
      *dw = (int)(state.screenH * frameAspect + 0.5f);
      int excess = *dw - state.screenW;
      *dx = (state.screenW - *dw) / 2 + (int)(cropPanX * excess / 2.0f);
      *dy = 0;
   } else {
      *dw = state.screenW;
      *dh = (int)(state.screenW / frameAspect + 0.5f);
      int excess = *dh - state.screenH;
      *dx = 0;
      *dy = (state.screenH - *dh) / 2 + (int)(cropPanY * excess / 2.0f);
   }

   // The regular overscan is applied by the display viewport to the whole HUD.
   // Cancel that transform for the video rectangle only, then apply the independent
   // Crop-to-Fill overscan value. The two settings therefore never add together.
   float regularScale = 1.0f + (float)getOverscanPercent() * 0.01f;
   float cropScale = 1.0f + (float)getCropToFillOverscanPercent() * 0.01f;
   float scale = cropScale / regularScale;
   float centerX = (float)state.screenW * 0.5f;
   float centerY = (float)state.screenH * 0.5f;
   float rectCenterX = (float)*dx + (float)*dw * 0.5f;
   float rectCenterY = (float)*dy + (float)*dh * 0.5f;
   *dw = (int)((float)*dw * scale + 0.5f);
   *dh = (int)((float)*dh * scale + 0.5f);
   rectCenterX = centerX + (rectCenterX - centerX) * scale;
   rectCenterY = centerY + (rectCenterY - centerY) * scale;
   *dx = (int)(rectCenterX - (float)*dw * 0.5f + 0.5f);
   *dy = (int)(rectCenterY - (float)*dh * 0.5f + 0.5f);
}

static void drawAutoplayNotice(void)
{
   uint64_t now = sys_time_get_system_time();
   float elapsed = (float)(now - state.nextNoticeStartedUs) / (float)AUTOPLAY_NOTICE_US;
   if (elapsed < 0.0f) elapsed = 0.0f;
   if (elapsed > 1.0f) elapsed = 1.0f;

   fillGfxRectangle(0, 0, state.screenW, state.screenH, 0xB8000000);
   int panelW = 1120, panelH = 610;
   int panelX = (state.screenW - panelW) / 2;
   int panelY = (state.screenH - panelH) / 2;
   fillGfxRectangle(panelX, panelY, panelW, panelH, activeTheme->surface);
   strokeGfxRectangle(panelX, panelY, panelW, panelH, 2, activeTheme->focusBorder);

   // The thumbnail is centred and kept at 16:9. A placeholder remains if download/decode fails.
   int headingX = state.screenW / 2 - nextNoticeHeadingLabel.tt.tex.w / 2;
   drawLabelAt(&nextNoticeHeadingLabel, headingX, panelY + 8);
   int thumbW = 800, thumbH = 450;
   int thumbX = state.screenW / 2 - thumbW / 2;
   int thumbY = panelY + 42;
   if (state.nextThumbTexture.offset) {
      drawGfxTextureFixedSize(thumbX, thumbY, thumbW, thumbH, state.nextThumbTexture,
                              0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, GFX_FILTER_LINEAR);
   } else {
      fillGfxRectangle(thumbX, thumbY, thumbW, thumbH, activeTheme->scrim);
   }

   int titleX = state.screenW / 2 - nextNoticeTitleLabel.tt.tex.w / 2;
   int titleY = thumbY + thumbH + 18;
   if (titleX < panelX + 30) titleX = panelX + 30;
   drawLabelAt(&nextNoticeTitleLabel, titleX, titleY);

   // 24 dots form a countdown ring; the lit arc grows as the three-second delay elapses.
   int cx = state.screenW / 2, cy = panelY + panelH - 32;
   int radius = 20;
   int lit = (int)(elapsed * 24.0f + 0.5f);
   for (int i = 0; i < 24; i++) {
      float angle = -1.5707963f + (float)i * 6.2831853f / 24.0f;
      int x = cx + (int)(cosf(angle) * radius);
      int y = cy + (int)(sinf(angle) * radius);
      fillGfxCircle(x, y, 4, i < lit ? activeTheme->accent : activeTheme->textSecondary);
   }
}

static void drawPlay(void)
{
   // Playback render-loop diagnostics: aggregate only, never log per frame.
   static uint64_t renderDiagWindowUs, renderDiagLastStartUs;
   static uint64_t renderDiagCalls, renderDiagIntervalsUs, renderDiagIntervalMaxUs;
   static uint64_t renderDiagTotalUs, renderDiagTotalMaxUs;
   static uint64_t renderDiagClearUs, renderDiagClearMaxUs;
   static uint64_t renderDiagPullUs, renderDiagPullMaxUs;
   static uint64_t renderDiagYuvUs, renderDiagYuvMaxUs;
   static uint64_t renderDiagRestUs, renderDiagRestMaxUs;
   static uint64_t renderDiagGap17, renderDiagGap20, renderDiagGap25;
   static uint64_t renderDiagGap33, renderDiagGap50, renderDiagGap100;
   static uint64_t renderDiagFrames, renderDiagNoFrame;
   uint64_t renderStartUs = sys_time_get_system_time();
   uint64_t renderIntervalUs = renderDiagLastStartUs && renderStartUs >= renderDiagLastStartUs
                             ? renderStartUs - renderDiagLastStartUs : 0;
   uint64_t clearStartUs, clearElapsedUs, framePullElapsedUs = 0, yuvDrawElapsedUs = 0;
   renderDiagLastStartUs = renderStartUs;

   // The selected SD UI zoom is intended for library/navigation screens only (including
   // 480-line anamorphic 16:9). Keep playback controls and overlays at their original
   // 1.0x geometry so the seek bar and other HUD elements retain their designed layout.
   float selectedUiScale = getGfxSdUiScale();
   setGfxSdUiScale(1.0f);

   clearStartUs = sys_time_get_system_time();
   clearGfx(COLOR_BLACK);   // clear the full output; overscan transforms video and HUD, not the black margins
   clearElapsedUs = sys_time_get_system_time() - clearStartUs;

   const uint8_t *frame = NULL;
   int renderHasFrame = 0;
   if (state.player) {
      int w, h;
      uint64_t framePullStartUs = sys_time_get_system_time();
      frame = getVideoFrame(state.player, &w, &h);
      uint64_t framePullUs = sys_time_get_system_time() - framePullStartUs;
      framePullElapsedUs = framePullUs;
#if BUILD_DIAG_PLAY_RENDER
      if (framePullUs >= 25000ull)
         logWarn("[play-render-stall] phase=frame-pull elapsedMs=%llu frame=%p stage=%d live=%d\n",
                 (unsigned long long)(framePullUs / 1000ull), frame, state.stage, state.isLive);
#endif
      renderHasFrame = frame != NULL;
      if (frame) {
         if (!state.firstFrameSeen) {
            showControls();   // bring up the HUD as playback starts; it auto-hides as usual
            if (!state.historyRecorded) {
               char videoId[12];
               if (extractPlaybackId(requestedInput, videoId)) {
                  int duration = (int)(getVideoDurationSeconds(state.player) + 0.5f);
                  if (duration > 0) setWatchedDuration(videoId, duration);
                  recordLocalHistory(videoId, state.title, state.author);
               }
               state.historyRecorded = 1;
            }
         }
         state.firstFrameSeen = 1;
         if (state.liveRefreshAttempts > 0) {
            uint64_t stableNow = sys_time_get_system_time();
            if (!state.liveRefreshStableSinceUs) state.liveRefreshStableSinceUs = stableNow;
            else if (stableNow - state.liveRefreshStableSinceUs >= 60000000ULL) {
               logInfo("[yt-live-refresh] LIVE session stable for 60s; renewal counter reset\n");
               state.liveRefreshAttempts = 0;
               state.liveRefreshStableSinceUs = 0;
            }
         }
         if (!state.isLive && state.vodRefreshAttempts > 0) {
            uint64_t stableNow = sys_time_get_system_time();
            if (!state.vodRefreshStableSinceUs) state.vodRefreshStableSinceUs = stableNow;
            else if (stableNow - state.vodRefreshStableSinceUs >= 60000000ULL) {
               logInfo("[yt-vod-refresh] VOD session stable for 60s; HTTP recovery counter reset\n");
               state.vodRefreshAttempts = 0;
               state.vodRefreshStableSinceUs = 0;
            }
         }
         if (state.isLive && !state.liveAnchored) {
            long actualFirst = getVideoLiveFirstSq(state.player);
            if (actualFirst >= 0) state.liveFirstSq = actualFirst;
            state.liveFirstPtsNs = getVideoPositionNs(state.player);
            state.liveAnchored = 1;
            if (state.liveReturnNoticePending) {
               state.liveReturnNoticePending = 0;
               state.liveReturnNoticeUs = sys_time_get_system_time();
            }
            logInfo("[yt] LIVE playback anchored at sq=%ld pts=%llu\n",
                    state.liveFirstSq, (unsigned long long)state.liveFirstPtsNs);
         }
         int dx, dy, dw, dh;
         if (cropToFillEnabled) getCropToFillRect(w, h, &dx, &dy, &dw, &dh);
         else                   getGfxLetterboxRect(w, h, &dx, &dy, &dw, &dh);
         if (!state.diagFirstYuvDraw)
            logInfo("[video-diag] first drawGfxYuvFrame begin source=%dx%d target=%dx%d\n",
                    w, h, dw, dh);
         uint64_t drawStartUs = sys_time_get_system_time();
         drawGfxYuvFrame(dx, dy, dw, dh, frame, w, h);
         yuvDrawElapsedUs = sys_time_get_system_time() - drawStartUs;
#if BUILD_DIAG_PLAY_RENDER
         if (yuvDrawElapsedUs >= 25000ull)
            logWarn("[play-render-stall] phase=yuv-draw elapsedMs=%llu source=%dx%d target=%dx%d\n",
                    (unsigned long long)(yuvDrawElapsedUs / 1000ull), w, h, dw, dh);
#endif
#if BUILD_DIAG_PLAY_RENDER
         recordVideoRenderDraw(framePullUs, yuvDrawElapsedUs, w, h);
#endif
         if (!state.diagFirstYuvDraw) {
            state.diagFirstYuvDraw = 1;
            logInfo("[video-diag] first drawGfxYuvFrame complete\n");
         }
      }
      measureFps(frame);
      if (state.showStats && frame) { updateStatLabels(); drawStatsOverlay(); }
      int overlayUp = state.chaptersVisible || state.descriptionVisible || optionsVisible;   // a full-screen overlay hides the HUD noise
      if (state.stage == STAGE_PLAYING && controlsVisible() && !overlayUp) {
         if (!state.isLive || state.liveDvr) {
            drawSeekBar();
            float uiScale = getGfxUiScaleFactor();
            if (uiScale < 1.0f) uiScale = 1.0f;
            // SD output keeps hint glyphs at 1.5x independently of UI zoom;
            // HD output follows the configured UI scale. Reserve the footer
            // using the same effective height as drawButtonHintsFitted().
            float hintScale = isGfxSdVideoOutput() ? 1.5f : uiScale;
            int hintGlyphHeight = (int)(playbackHints.glyphHeight * hintScale + 0.5f);
            int hintBottomY = state.screenH - hintGlyphHeight - 16;
            drawButtonHintsFitted(&playbackHints, state.screenW, hintBottomY, 8, 2);
         }
      }
      if (state.stage == STAGE_PLAYING && fullPlaybackHudVisible() && !overlayUp) {
         drawTitle();
         drawPlaybackInfoHud();
      }
      if (state.stage == STAGE_PLAYING && !overlayUp) drawVolumeMeter(&volumeMeter);
      // Draw captions first. All overlays, especially Options and its dropdowns,
      // must remain visually above the active subtitle cue.
      if (state.stage == STAGE_PLAYING && (!overlayUp || optionsVisible)) drawSubtitle();
      if (state.chaptersVisible) drawChapters();
      if (state.descriptionVisible) drawDescription();
      if (state.stage == STAGE_PLAYING && (!overlayUp || optionsVisible)) drawToast();
      if (state.stage == STAGE_PLAYING && (!overlayUp || optionsVisible)) drawAskSponsorPrompt();
      if (state.nextNoticeVisible) drawAutoplayNotice();
      // Draw Options and its playback-owned settings submenus above captions and every HUD layer.
      if (optionsVisible) drawPlaybackOptions();
      if (playbackSettingsOpen) drawPlaybackSettings();
      if (playbackAutoTranslateOpen) drawPlaybackAutoTranslateSettings();
      if (playbackSponsorOpen) drawPlaybackSponsorSettings();
      if (playbackSubtitleStyleOpen) drawPlaybackSubtitleStyle();
   }

   // until the first frame is presented (open, then the async resume seek's reload) keep the loading
   // status up instead of a black screen; with no player this shows the failure message.
   if (!state.firstFrameSeen || state.audioSwitchThreadActive || state.resolutionSwitchThreadActive) {
      if (state.stage == STAGE_FAILED) {
         int statusX = state.screenW / 2 - statusLabel.tt.tex.w / 2;
         int statusY = state.screenH / 2 - statusLabel.tt.tex.h / 2;
         drawLabelAt(&statusLabel, statusX, statusY);
      } else {
         drawLoadingIndicator(state.screenW / 2, state.screenH / 2, &loadingAnimationStartedUs);
      }
   } else {
      loadingAnimationStartedUs = 0;
   }

   if (state.liveFailureVisible) {
      int panelW = state.screenW - 80, panelH = 300;
      if (panelW > 860) panelW = 860;
      int panelX = (state.screenW - panelW) / 2;
      int panelY = (state.screenH - panelH) / 2;
      fillGfxRectangle(0, 0, state.screenW, state.screenH, 0xD0000000);
      fillGfxRoundedRectangleFixed(panelX, panelY, panelW, panelH, 14, activeTheme->surface);
      strokeGfxRectangle(panelX, panelY, panelW, panelH, 2, activeTheme->focusBorder);
      drawLabelAt(&liveFailureTitleLabel, state.screenW / 2 - liveFailureTitleLabel.tt.tex.w / 2, panelY + 54);
      drawLabelAt(&liveFailureBodyLabel, state.screenW / 2 - liveFailureBodyLabel.tt.tex.w / 2, panelY + 112);
      liveFailureHints.y = panelY + panelH - 56;
      drawButtonHints(&liveFailureHints, state.screenW);
   }

   // Always restore the selected library scale before the next screen is drawn.
   // This must not depend on HUD visibility or playback state: otherwise the
   // temporary 1.0x playback scale can leak into navigation.
   setGfxSdUiScale(selectedUiScale);

   // Split the UI-thread frame into clear, frame acquisition, YUV submission and remaining HUD.
   // The interval includes stalls outside drawPlay as well as missed scheduling opportunities.
   {
      uint64_t renderEndUs = sys_time_get_system_time();
      uint64_t totalUs = renderEndUs >= renderStartUs ? renderEndUs - renderStartUs : 0;
      uint64_t restUs = totalUs > clearElapsedUs + framePullElapsedUs + yuvDrawElapsedUs
                      ? totalUs - clearElapsedUs - framePullElapsedUs - yuvDrawElapsedUs : 0;
#if BUILD_DIAG_PLAY_RENDER
      if (totalUs >= 25000ull)
         logWarn("[play-render-stall] phase=draw-play totalMs=%llu clearMs=%llu pullMs=%llu yuvMs=%llu restMs=%llu stage=%d live=%d paused=%d seeking=%d\n",
                 (unsigned long long)(totalUs / 1000ull),
                 (unsigned long long)(clearElapsedUs / 1000ull),
                 (unsigned long long)(framePullElapsedUs / 1000ull),
                 (unsigned long long)(yuvDrawElapsedUs / 1000ull),
                 (unsigned long long)(restUs / 1000ull),
                 state.stage, state.isLive, state.player ? isVideoPaused(state.player) : 0, state.seeking);
#endif
      if (!renderDiagWindowUs) renderDiagWindowUs = renderStartUs;
      renderDiagCalls++;
      renderDiagIntervalsUs += renderIntervalUs;
      if (renderIntervalUs > renderDiagIntervalMaxUs) renderDiagIntervalMaxUs = renderIntervalUs;
      if (renderIntervalUs > 17000ull) renderDiagGap17++;
      if (renderIntervalUs >= 20000ull) renderDiagGap20++;
      if (renderIntervalUs >= 25000ull) renderDiagGap25++;
      if (renderIntervalUs >= 33333ull) renderDiagGap33++;
      if (renderIntervalUs >= 50000ull) renderDiagGap50++;
      if (renderIntervalUs >= 100000ull) renderDiagGap100++;
      renderDiagTotalUs += totalUs;
      if (totalUs > renderDiagTotalMaxUs) renderDiagTotalMaxUs = totalUs;
      renderDiagClearUs += clearElapsedUs;
      if (clearElapsedUs > renderDiagClearMaxUs) renderDiagClearMaxUs = clearElapsedUs;
      renderDiagPullUs += framePullElapsedUs;
      if (framePullElapsedUs > renderDiagPullMaxUs) renderDiagPullMaxUs = framePullElapsedUs;
      renderDiagYuvUs += yuvDrawElapsedUs;
      if (yuvDrawElapsedUs > renderDiagYuvMaxUs) renderDiagYuvMaxUs = yuvDrawElapsedUs;
      renderDiagRestUs += restUs;
      if (restUs > renderDiagRestMaxUs) renderDiagRestMaxUs = restUs;
      if (renderHasFrame) renderDiagFrames++;
      else renderDiagNoFrame++;

      if (renderEndUs - renderDiagWindowUs >= 5000000ull) {
         logInfo("[play-render-diag] spanMs=%llu calls=%llu avgIntervalUs=%llu maxIntervalUs=%llu gap17=%llu gap20=%llu gap25=%llu gap33=%llu gap50=%llu gap100=%llu avgTotalUs=%llu maxTotalUs=%llu clearAvgUs=%llu clearMaxUs=%llu pullAvgUs=%llu pullMaxUs=%llu yuvAvgUs=%llu yuvMaxUs=%llu restAvgUs=%llu restMaxUs=%llu frameCalls=%llu noFrameCalls=%llu stage=%d live=%d paused=%d seeking=%d\n",
                 (unsigned long long)((renderEndUs-renderDiagWindowUs)/1000ull),
                 (unsigned long long)renderDiagCalls,
                 (unsigned long long)(renderDiagCalls ? renderDiagIntervalsUs/renderDiagCalls : 0),
                 (unsigned long long)renderDiagIntervalMaxUs,
                 (unsigned long long)renderDiagGap17, (unsigned long long)renderDiagGap20,
                 (unsigned long long)renderDiagGap25, (unsigned long long)renderDiagGap33,
                 (unsigned long long)renderDiagGap50, (unsigned long long)renderDiagGap100,
                 (unsigned long long)(renderDiagCalls ? renderDiagTotalUs/renderDiagCalls : 0),
                 (unsigned long long)renderDiagTotalMaxUs,
                 (unsigned long long)(renderDiagCalls ? renderDiagClearUs/renderDiagCalls : 0),
                 (unsigned long long)renderDiagClearMaxUs,
                 (unsigned long long)(renderDiagCalls ? renderDiagPullUs/renderDiagCalls : 0),
                 (unsigned long long)renderDiagPullMaxUs,
                 (unsigned long long)(renderDiagCalls ? renderDiagYuvUs/renderDiagCalls : 0),
                 (unsigned long long)renderDiagYuvMaxUs,
                 (unsigned long long)(renderDiagCalls ? renderDiagRestUs/renderDiagCalls : 0),
                 (unsigned long long)renderDiagRestMaxUs,
                 (unsigned long long)renderDiagFrames, (unsigned long long)renderDiagNoFrame,
                 state.stage, state.isLive, state.player ? isVideoPaused(state.player) : 0, state.seeking);
         renderDiagWindowUs = renderEndUs;
         renderDiagCalls = renderDiagIntervalsUs = renderDiagIntervalMaxUs = 0;
         renderDiagTotalUs = renderDiagTotalMaxUs = 0;
         renderDiagClearUs = renderDiagClearMaxUs = 0;
         renderDiagPullUs = renderDiagPullMaxUs = 0;
         renderDiagYuvUs = renderDiagYuvMaxUs = 0;
         renderDiagRestUs = renderDiagRestMaxUs = 0;
         renderDiagGap17 = renderDiagGap20 = renderDiagGap25 = 0;
         renderDiagGap33 = renderDiagGap50 = renderDiagGap100 = 0;
         renderDiagFrames = renderDiagNoFrame = 0;
      }
   }
#if BUILD_DIAG_SUBTITLE_RENDER
   flushSubtitleRenderDiag(sys_time_get_system_time());
#endif
}

static void joinPlaybackWorker(const char *name, sys_ppu_thread_t tid)
{
   uint64_t started = sys_time_get_system_time();
   int rc = joinThread(tid);
   uint64_t elapsed = sys_time_get_system_time() - started;
   logInfo("[play-cancel] worker=%s joinMs=%llu rc=%d\n", name,
           (unsigned long long)(elapsed / 1000ull), rc);
}

static void termPlay(void)
{
   uint64_t teardownStartedUs = sys_time_get_system_time();
   cropPanX = cropPanY = 0.0f;
   cropPanLastUpdateUs = 0;
   // Shut down playback-owned sockets before joining workers. This wakes resolver, stream and
   // prefetch threads without cancelling downloads or unrelated HTTP work.
   requestPlaybackCancel();
   state.resolutionSwitchAborting = 1;
   if (state.threadActive) { joinPlaybackWorker("playback", state.workerTid); state.threadActive = 0; }
   if (state.sbThreadActive) { joinPlaybackWorker("sponsor", state.sbWorkerTid); state.sbThreadActive = 0; }   // usually already done
   if (state.subThreadActive) {
      state.subtitleTranslation.cancel = 1;
      joinPlaybackWorker("subtitles", state.subWorkerTid);
      state.subThreadActive = 0;
   }
   if (state.audioSwitchThreadActive) { joinPlaybackWorker("audio-switch", state.audioSwitchTid); state.audioSwitchThreadActive = 0; }
   if (state.resolutionSwitchThreadActive) { joinPlaybackWorker("resolution-switch", state.resolutionSwitchTid); state.resolutionSwitchThreadActive = 0; }
   if (state.chThreadActive) { joinPlaybackWorker("chapters", state.chWorkerTid); state.chThreadActive = 0; }
   if (state.metadataThreadActive) { joinPlaybackWorker("metadata", state.metadataWorkerTid); state.metadataThreadActive = 0; }
   if (state.liveBaseRefreshActive) { joinPlaybackWorker("live-base-refresh", state.liveBaseRefreshTid); state.liveBaseRefreshActive = 0; }
   if (state.liveEndCheckActive) { joinPlaybackWorker("live-end-check", state.liveEndCheckTid); state.liveEndCheckActive = 0; }
   if (state.liveBaseRefreshInfo) { freeStreamInfo(state.liveBaseRefreshInfo); state.liveBaseRefreshInfo = NULL; }
   if (state.nextThreadActive) { joinPlaybackWorker("autoplay", state.nextWorkerTid); state.nextThreadActive = 0; }
   free(state.description);
   state.description = NULL;
   free(state.subtitleTrack);
   state.subtitleTrack = NULL;
   free(state.captionTracks);
   state.captionTracks = NULL;
   state.captionCount = 0;
   // Save the resume position on exit. A finished video stores its full duration so the grid can
   // render a 100% progress line; the resume path already treats positions near the end as completed.
   // Only save after a frame played, and keep typed URLs out of the ID-keyed history.
   if (state.player && state.firstFrameSeen && requestedIsLocal) {
      char downloadedId[12];
      if (getRequestedDownloadedId(downloadedId)) {
         int duration = (int)(getVideoDurationSeconds(state.player) + 0.5f);
         int position = isVideoEnded(state.player) ? duration
                                                   : (int)getVideoPositionSeconds(state.player);
         setDownloadedProgress(downloadedId, position, duration);
      }
   } else if (state.player && state.firstFrameSeen && (!state.isLive || state.liveEnded)) {
      char historyId[12];
      if (extractPlaybackId(requestedInput, historyId)) {
         int savedPosition = isVideoEnded(state.player) ? (int)(getVideoDurationSeconds(state.player) + 0.5f)
                                                       : (int)getVideoPositionSeconds(state.player);
         setWatchedDuration(historyId, (int)(getVideoDurationSeconds(state.player) + 0.5f));
         setWatchedPosition(historyId, savedPosition);
      }
   }
   if (state.player || state.pendingPlayer || state.pendingResolutionPlayer || state.retiringResolutionPlayer || state.liveRetiringPlayer || state.vodRetiringPlayer)
      finishGfx();   // RSX may still be sampling a frame buffer
   if (state.player) { destroyVideoPlayer(state.player); state.player = NULL; }
   if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
   if (state.pendingResolutionPlayer) { destroyVideoPlayer(state.pendingResolutionPlayer); state.pendingResolutionPlayer = NULL; }
   if (state.retiringResolutionPlayer) { destroyVideoPlayer(state.retiringResolutionPlayer); state.retiringResolutionPlayer = NULL; }
   if (state.liveRetiringPlayer) { destroyVideoPlayer(state.liveRetiringPlayer); state.liveRetiringPlayer = NULL; }
   if (state.vodRetiringPlayer) { destroyVideoPlayer(state.vodRetiringPlayer); state.vodRetiringPlayer = NULL; }
   freeLabel(&descriptionPageLabel);
   freeLabel(&statusLabel);
   for (int i = 0; i < STAT_LINES; i++) freeLabel(&statLabels[i]);
   freeLabel(&timeLeftLabel);
   freeLabel(&timeRightLabel);
   freeLabel(&liveBadgeLabel);
   freeLabel(&backToLabel);
   freeLabel(&liveFailureTitleLabel);
   freeLabel(&liveFailureBodyLabel);
   termButtonHints(&liveFailureHints);
   termButtonHints(&playbackHints);
   freeLabel(&toastLabel);
   freeLabel(&askPromptPrefixLabel);
   freeLabel(&askPromptSuffixLabel);
   freeLabel(&titleLabel);
   freeLabel(&authorLabel);
   freeLabel(&nextNoticeHeadingLabel);
   freeLabel(&nextNoticeTitleLabel);
   freeLabel(&subtitleLabel);
   freeLabel(&playbackInfoLabel);
   for (int i = 0; i < MAX_CHAPTERS; i++) { freeLabel(&chapterLabels[i]); freeLabel(&chapterTimeLabels[i]); }
   for (int i = 0; i < OPTIONS_COUNT; i++) freeLabel(&optionsLabels[i]);
   for (int i = 0; i < OPTIONS_DROPDOWN_MAX; i++) freeLabel(&optionsValueLabels[i]);
   freeLabel(&optionsDownloadLabel);
   for (int i = 0; i < 3; i++) freeLabel(&optionsCurrentValueLabels[i]);
   freeLabel(&playbackSettingsTitle);
   freeLabel(&playbackAutoTranslateTitle);
   for (int i = 0; i < PLAYBACK_AUTO_TRANSLATE_VISIBLE_ROWS; i++) freeLabel(&playbackAutoTranslateRows[i]);
   freeLabel(&playbackSubtitleStyleTitle);
   for (int i = 0; i < PLAYBACK_SUBTITLE_STYLE_COUNT; i++) freeLabel(&playbackSubtitleStyleLabels[i]);
   freeLabel(&playbackSubtitlePreview);
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      freeLabel(&playbackSettingsLabels[i]);
      freeLabel(&playbackSettingsValues[i]);
   }
   for (int i = 0; i < 2; i++) freeLabel(&playbackSettingsToggleLabels[i]);
   freeLabel(&playbackSponsorTitle);
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++) freeLabel(&playbackSponsorCategoryLabels[i]);
   for (int i = 0; i < 3; i++) freeLabel(&playbackSponsorActionLabels[i]);
   freeVolumeMeter(&volumeMeter);
   clearDescriptionPages();
   if (state.nextThumbJpeg) { free(state.nextThumbJpeg); state.nextThumbJpeg = NULL; }
   if (state.nextImageRequested) cancelImageAsync();
   if (state.nextThumbTexture.offset) { finishGfx(); freeGfxTexture(&state.nextThumbTexture); }
   closeFont(&uiFont);
   logInfo("[play-cancel] teardownComplete totalMs=%llu\n",
           (unsigned long long)((sys_time_get_system_time() - teardownStartedUs) / 1000ull));
}

void getPlaybackReturnVideoIds(char origin[16], char current[16])
{
   if (origin) strCopy(origin, 16, playbackOriginVideoId);
   if (current) {
      current[0] = 0;
      if (!requestedIsLocal) extractPlaybackId(requestedInput, current);
   }
}

Screen playScreen = { initPlay, NULL, updatePlay, drawPlay, NULL, termPlay, SCREEN_TERMINATED };
