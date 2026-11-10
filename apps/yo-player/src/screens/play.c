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
#include "chapters.h"           // description timestamp lines -> chapter list
#include "dbg.h"                // logInfo/logError (bridge)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>              // snprintf (stats overlay)
#include <sys/sys_time.h>       // sys_time_get_system_time (fps measure)
#include <math.h>                // autoplay countdown ring

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
#define CROSS_RESTART_HOLD_US 3000000ULL
#define SEEK_ACCEL_1_US     1500000ULL
#define SEEK_ACCEL_2_US     3000000ULL
#define SEEK_ACCEL_3_US     5000000ULL
#define SEEK_APPLY_IDLE_US  400000ULL
#define CONTROLS_VISIBLE_US 2000000ULL
#define HUD_VISIBLE_US      3000000ULL
#define TOAST_VISIBLE_US    2000000ULL   // how long a toast ("Skipped ...", "Subtitles: ...") stays up
#define ASK_PROMPT_VISIBLE_US 3000000ULL
#define AUTOPLAY_NOTICE_US  3000000ULL
#define AUTOPLAY_THUMB_CAP  (128 * 1024)

// subtitles: Triangle toggles captions on/off; language selection lives in Options.
#define SUBTITLE_TEXT_SIZE 30
#define MAX_AUDIO_TRACKS 128

// description overlay: SELECT toggles the description full-screen; up/down scroll it.
#define DESCRIPTION_MARGIN_X    120
#define DESCRIPTION_MARGIN_Y    80
#define DESCRIPTION_TEXT_SIZE   24
#define DESCRIPTION_SCROLL_STEP 40

// chapter picker: START toggles a centred list of the video's chapters; up/down select
// (the window follows the selection), cross jumps there, circle/START close without jumping.
#define CHAPTER_TEXT_SIZE    24
#define CHAPTER_ROW_HEIGHT   46
#define CHAPTER_VISIBLE_ROWS 12
#define CHAPTER_TIME_GAP     24   // between the right-aligned time column and the title column

#define OPTIONS_COUNT 8
#define MAX_PLAYBACK_QUALITIES 12
#define OPTIONS_DROPDOWN_MAX MAX_AUDIO_TRACKS
#define OPTIONS_DROPDOWN_VISIBLE_MAX 10
static const char *optionsNames[OPTIONS_COUNT] = {"Quality", "Audio", "Subtitles", "Subs language", "Chapters", "Crop to Fill", "Autoplay", "Playback Settings"};
static const char *optionsValues[OPTIONS_COUNT][OPTIONS_DROPDOWN_MAX] = {
   {"Auto"},
   {"Default"},
   {"Off", "On"},
   {"English"},
   {"Open chapters"},
   {"Off", "On"},
   {"Off", "On"},
   {"Open settings"}
};
static int optionsValueCounts[OPTIONS_COUNT] = {1, 1, 2, 1, 1, 2, 2, 1};
static char optionsQualityLabels[OPTIONS_DROPDOWN_MAX][32];
static char optionsAudioLabels[MAX_AUDIO_TRACKS][64];
static int optionsQualityTargets[OPTIONS_DROPDOWN_MAX];
static int preferredQualityHeight, preferredQualityFps; // 0/0 means Auto; persists across videos in RAM
static int optionsVisible, optionsDropdownOpen, optionsSelected, optionsDropdownSelected;
static int lastSubtitlePick = -1;     // preserve the selected track while captions are switched OFF
static int suppressPlaybackCrossUntilRelease;
static ButtonRepeat optionsNavigateRepeat, optionsDropdownRepeat, chapterNavigateRepeat;
static Label optionsLabels[OPTIONS_COUNT];
static Label optionsValueLabels[OPTIONS_DROPDOWN_MAX];
static Label optionsCurrentValueLabels[3]; // Quality, Audio, Subs language

// Playback-owned settings panel. It deliberately shares the persisted settings APIs
// with navigation, but owns its input state, labels, layout and SponsorBlock submenu.
#define PLAYBACK_SETTINGS_COUNT 5
#define PLAYBACK_SETTINGS_ROW_H 54
#define PLAYBACK_SETTINGS_PANEL_W 650
#define PLAYBACK_SETTINGS_PANEL_H 430
#define PLAYBACK_SUBTITLE_STYLE_COUNT 5
#define PLAYBACK_SUBTITLE_STYLE_PANEL_H 490
#define PLAYBACK_SPONSOR_PANEL_W 780
#define PLAYBACK_SPONSOR_PANEL_H 470
#define PLAYBACK_SPONSOR_ROW_H 42
#define PLAYBACK_SPONSOR_OPTION_W 82
#define PLAYBACK_SPONSOR_OPTION_GAP 5
#define PLAYBACK_SD_SCALE_PATH CRTUBE_DATA_DIR "/sd-ui-scale.txt"
static int playbackSettingsOpen, playbackSettingsSelected;
static int playbackSubtitleStyleOpen, playbackSubtitleStyleSelected;
static ButtonRepeat playbackSubtitleStyleRepeat;
static Label playbackSubtitleStyleTitle, playbackSubtitleStyleLabels[PLAYBACK_SUBTITLE_STYLE_COUNT], playbackSubtitlePreview;
static int playbackSponsorOpen, playbackSponsorSelected;
static ButtonRepeat playbackSettingsRepeat, playbackSponsorRepeat;
static Label playbackSettingsTitle, playbackSettingsLabels[PLAYBACK_SETTINGS_COUNT];
static Label playbackSettingsValues[PLAYBACK_SETTINGS_COUNT];
static Label playbackSettingsToggleLabels[2];
static Label playbackSponsorTitle, playbackSponsorCategoryLabels[SPONSOR_CATEGORY_COUNT];
static Label playbackSponsorActionLabels[3];

// the video to play, set by playVideo() before the screen is pushed. defaults to
// a short public clip so pushing the screen directly still does something.
static char requestedInput[256] = "jNQXAC9IVRw";   // "Me at the zoo"
static char requestedLocalPath[MAX_PATH_LEN];
static int requestedIsLocal;

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
   volatile int     cancelRequested; // cooperative cancellation for the initial playback worker
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
   char             description[2048]; // resolved video description (worker sets, UI reads after adoption)
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
   int              descriptionScroll;   // pixels scrolled down into the text

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
   const uint8_t   *lastFrame;        // fps measure: a changed pointer means a newly presented frame
   int              framesThisSecond, measuredFps;
   uint64_t         fpsTickUs;

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

   // subtitles: Options selects a track; Triangle toggles it on/off. A worker fetches it in the background
   // (updatePlay respawns the fetch whenever the pick and the fetched track disagree), and once ready
   // the cue for the current position drives subtitleLabel each frame.
   CaptionTrack    *captionTracks;     // dynamically sized to include every source and translated track
   int              captionCount;
   int              subtitlePick;        // chosen track index, -1 = off
   int              subtitleFetched;     // track index the fetch (in flight or done) belongs to, -1 = none
   int              subtitlesReady;      // fetched track is safe to read
   int              subtitleScanFrom;    // cue lookup cache (getSubtitleCueText)
   SubtitleTrack   *subtitleTrack;       // heap; ~800 KB, allocated on first enable, freed on exit
   volatile int     subWorkerDone;
   int              subtitleFetchResult; // fetchSubtitles result: 0, network error, or -HTTP status
   int              subThreadActive;
   sys_ppu_thread_t subWorkerTid;
} state;

#define STAT_LINES 4

static Font  uiFont;
static Label statusLabel;
static Label statLabels[STAT_LINES];
static Label timeLeftLabel, timeRightLabel;   // seek bar: current time (left) / total (right)
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
static TextTexture descriptionTexture;        // whole description as one tall texture (scrolled by draw)
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
   // Interrupt active resolver/stream requests before joining the playback worker.
   setHttpRequestsCancelled(1);
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
   if (strncmp(descriptor, "dashseg|", 8) != 0 && strncmp(descriptor, "dashvod|", 8) != 0) return;
   long firstSq = atol(descriptor + 8);
   const char *edge = strchr(descriptor + 8, '|');
   if (!edge) return;
   edge = strchr(edge + 1, '|');
   if (!edge) return;
   snprintf(out, cap, "%.7s|%ld|%ld|%ld|%s", descriptor, firstSq, edgeSq, playSq, edge + 1);
}

// Wait for the decoder/audio pipeline to produce a usable pre-roll. This runs on the
// playback worker, never on the UI thread. A timeout is treated as a failed rendition
// and allows the diagnostic fallback to try another format.
static int waitForInitialPlayerReady(VideoPlayer *player, unsigned timeoutMs)
{
   uint64_t started = sys_time_get_system_time();
   while (player && !playbackCancelRequested() && !isVideoPlayerReady(player) &&
          sys_time_get_system_time() - started < (uint64_t)timeoutMs * 1000ull)
      sleepMs(50);
   return player && !playbackCancelRequested() && isVideoPlayerReady(player);
}

static int fallbackFormatUsable(const StreamFormat *f)
{
   if (!f || !f->hasVideo || f->needsCipher || !f->url[0]) return 0;
   if (strcmp(f->container, "mp4") != 0) return 0;
   if (f->height <= 0 || f->height > 1080 || f->fps <= 0 || f->fps > 60) return 0;
   if (f->height > 720 && f->fps > 30) return 0;
   return 1;
}

static const StreamFormat *findFallbackItag(const StreamInfo *info, int itag)
{
   for (int i = 0; i < info->formatCount; i++)
      if (info->formats[i].itag == itag && fallbackFormatUsable(&info->formats[i]))
         return &info->formats[i];
   return NULL;
}

// Try the normal resolver pick first. If it cannot produce a decoded pre-roll,
// test muxed MP4 and progressively higher adaptive AVC candidates. The successful
// candidate becomes the actual initial stream; the regular selector remains unchanged.
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

   // Test the known muxed and AVC renditions first, then any other compatible MP4
   // video formats. Keep the list bounded so one bad video cannot stall playback forever.
   static const int preferredItags[] = { 18, 134, 135, 136, 137 };
   const StreamFormat *attempts[12];
   int count = 0;
   for (int i = 0; i < (int)(sizeof preferredItags / sizeof preferredItags[0]); i++) {
      const StreamFormat *f = findFallbackItag(info, preferredItags[i]);
      if (!f || f == video || strcmp(f->url, video->url) == 0) continue;
      int duplicate = 0;
      for (int j = 0; j < count; j++) if (strcmp(attempts[j]->url, f->url) == 0) duplicate = 1;
      if (!duplicate && count < (int)(sizeof attempts / sizeof attempts[0])) attempts[count++] = f;
   }
   for (int i = 0; i < info->formatCount && count < (int)(sizeof attempts / sizeof attempts[0]); i++) {
      const StreamFormat *f = &info->formats[i];
      if (!fallbackFormatUsable(f) || f == video || strcmp(f->url, video->url) == 0) continue;
      int duplicate = 0;
      for (int j = 0; j < count; j++) if (strcmp(attempts[j]->url, f->url) == 0) duplicate = 1;
      if (!duplicate) attempts[count++] = f;
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
      snprintf(optionsQualityLabels[0], sizeof optionsQualityLabels[0], "Auto (%dp%d)",
               state.videoHeights[state.autoResolutionIndex], state.videoFps[state.autoResolutionIndex]);
   } else {
      strCopy(optionsQualityLabels[0], sizeof optionsQualityLabels[0], "Auto");
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
         strCopy(optionsAudioLabels[0], sizeof optionsAudioLabels[0], "Original upload");
      else
         strCopy(optionsAudioLabels[0], sizeof optionsAudioLabels[0], "Unavailable");
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
   return count;
}

static int subtitleMenuCount(void)
{
   int count = subtitleMainTrackCount();
   return count > 0 ? count : 1;
}

// Resolve a visible row to its actual CaptionTrack index. Translated tracks are hidden.
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
   return -1;
}

static int subtitleMainMenuIndexForTrack(int trackIndex)
{
   if (trackIndex < 0 || trackIndex >= state.captionCount ||
       subtitleIsAutoTranslated(&state.captionTracks[trackIndex])) return 0;

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
      if (subtitleIsAutoGenerated(&state.captionTracks[i]) &&
          subtitleLanguageBaseMatches(state.captionTracks[i].languageCode, "en")) return i;
   return -1;
}

static void worker(uint64_t arg)
{
   (void)arg;
   StreamInfo *info = NULL;
   if (requestedIsLocal) {
      char fallback[256] = "Downloaded video";
      const char *base = strrchr(requestedLocalPath, '/');
      if (base) {
         strCopy(fallback, sizeof fallback, base + 1);
         char *extension = strrchr(fallback, '.');
         if (extension) *extension = 0;
      }
      strCopy(state.title, sizeof state.title, fallback);
      strCopy(state.author, sizeof state.author, "Downloaded");
      loadDownloadedMetadata(requestedLocalPath, state.title, sizeof state.title,
                             state.author, sizeof state.author,
                             state.description, sizeof state.description);
      CaptionTrack localTracks[2];
      int localTrackCount = getDownloadedSubtitleTracks(requestedLocalPath, localTracks, 2);
      if (localTrackCount > 0) {
         state.captionTracks = (CaptionTrack *)malloc((size_t)localTrackCount * sizeof *state.captionTracks);
         if (state.captionTracks) {
            memcpy(state.captionTracks, localTracks, (size_t)localTrackCount * sizeof *state.captionTracks);
            state.captionCount = localTrackCount;
            state.subtitlePick = 0;
            lastSubtitlePick = 0;
         }
      }
      parseChapters(state.description, &state.chapters);
      strCopy(state.likeCount, sizeof state.likeCount, "Offline");
      state.isLive = 0;
      state.videoCount = 0;
      state.resolutionIndex = -1;
      state.autoResolutionIndex = -1;
      state.pendingPlayer = createVideoPlayerSplitCancelable(requestedLocalPath, NULL,
                               allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);
      if (!state.pendingPlayer || !waitForInitialPlayerReady(state.pendingPlayer, 10000)) {
         if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
         if (!playbackCancelRequested()) fail("could not play downloaded file");
         goto done;
      }
      state.vidItag = state.audItag = state.vidW = state.vidH = state.vidFps = 0;
      goto done;
   }
   info = calloc(1, sizeof *info);   // large format/URL table; keep it off the stack
   const Extractor *extractor = findExtractor(requestedInput);
   if (playbackCancelRequested()) goto done;
   if (!info || !extractor) { fail("unrecognised link or id"); goto done; }

   // resolve (stage stays STAGE_LOADING, set by initPlay's memset, until we adopt or fail)
   int extractResult = extractor->extract(requestedInput, info);
   if (playbackCancelRequested()) goto done;
   if (extractResult != 0 || info->formatCount == 0) {
      fail(extractResult == -2 ? "This video is available to channel members only." : "resolve failed");
      goto done;
   }
   strCopy(state.title, sizeof state.title, info->title);
   strCopy(state.author, sizeof state.author, info->author);
   strCopy(state.description, sizeof state.description, info->description);
   strCopy(state.viewCount, sizeof state.viewCount, info->viewCount);
   strCopy(state.publishedDate, sizeof state.publishedDate, info->publishedDate);
   parseChapters(state.description, &state.chapters);
   if (info->captionCount > 0) {
      state.captionTracks = malloc((size_t)info->captionCount * sizeof *state.captionTracks);
      if (state.captionTracks) {
         for (int i = 0; i < info->captionCount; i++)
            state.captionTracks[state.captionCount++] = info->captions[i];
      } else {
         logError("[subtitles] unable to allocate playback caption list\n");
      }
   }

   const StreamFormat *video = pickBestPlaybackVideo(info);
   if (!video) { fail("no playable mp4 video"); goto done; }
   int isLivePlayback = info->isLive || info->isPostLive || video->isLiveSegmented;
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
   if (state.videoCount == 0) { fail("no playable mp4 video"); goto done; }
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
   if (selectedQuality < 0) { fail("no playable mp4 video"); goto done; }
   video = qualityFormats[selectedQuality];
   state.resolutionIndex = selectedQuality;
   // A muxed rendition carries its own audio. Video-only choices use the original/default AAC track.
   const StreamFormat *audio = video->hasAudio ? NULL : availableAudio;
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
            strCopy(state.audioNames[n], sizeof state.audioNames[n], "Original upload");
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
      // Match an explicitly identified non-dubbed original first. Never default to an auto-dub.
      for (int j = 0; j < state.audioCount; j++) {
         if (strcmp(state.audioIds[j], "__original_upload__") == 0) { defaultPick = j; break; }
         const StreamFormat *candidate = pickAudioForTrack(info, state.audioIds[j]);
         if (candidate && !candidate->audioIsAutoDubbed) { defaultPick = j; break; }
      }
      // Match YouTube's declared default only if it is not marked as an auto-dub.
      if (defaultPick < 0 && info->defaultAudioTrackId[0])
         for (int j = 0; j < state.audioCount; j++)
            if (strcmp(state.audioIds[j], info->defaultAudioTrackId) == 0) {
               const StreamFormat *candidate = pickAudioForTrack(info, state.audioIds[j]);
               if (candidate && !candidate->audioIsAutoDubbed) defaultPick = j;
               break;
            }
      // Some player responses omit the captions-level ID but mark a default adaptive format.
      if (defaultPick < 0)
         for (int i = 0; i < info->formatCount && defaultPick < 0; i++) {
            const StreamFormat *f = &info->formats[i];
            if (!f->hasAudio || f->audioIsAutoDubbed || !f->audioIsDefault || !f->audioTrackId[0]) continue;
            for (int j = 0; j < state.audioCount; j++)
               if (strcmp(state.audioIds[j], f->audioTrackId) == 0) { defaultPick = j; break; }
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

   // Open the resolver's normal pick first. For VOD only, wait for a decoded pre-roll;
   // if that rendition cannot become ready, automatically test muxed/AVC alternatives.
   // Live playback keeps the existing non-blocking startup path because segmented live
   // streams have different readiness and retry behaviour.
   if (playbackCancelRequested()) goto done;
   state.pendingPlayer = openInitialWithFallback(info, &video, &audio, !info->isLive && !video->isLiveSegmented);
   if (playbackCancelRequested()) {
      if (state.pendingPlayer) {
         destroyVideoPlayer(state.pendingPlayer);
         state.pendingPlayer = NULL;
      }
      goto done;
   }
   if (!state.pendingPlayer) { fail("primary and fallback streams failed"); goto done; }

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
   state.subtitlePick = (subtitlesEnabled && !state.isLive) ?
                        findPreferredSubtitleTrack(getSubtitleLanguage()) : -1;
   lastSubtitlePick = state.subtitlePick;

   // Resume by the canonical video ID, including when playback was started from a pasted YouTube URL.
   // The same ID is used by local history and by the saved position table.
   {
      char resumeId[12];
      int resumeAt = (!state.isLive && extractPlaybackId(requestedInput, resumeId))
                   ? getWatchedPosition(resumeId) : 0;
      float duration = getVideoDurationSeconds(state.pendingPlayer);
      if (resumeAt > 3 && (duration <= 0 || resumeAt < duration - 10))
         seekVideoPlayer(state.pendingPlayer, (float)resumeAt);
   }

done:
   if (info) free(info->captions);
   free(info);
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
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
   fetchSponsorSegments(requestedInput, &state.sb);
   __sync_synchronize();
   state.sbWorkerDone = 1;
   exitThread();
}

// asks /next for youtube's own chapter list; spawned only when the description parse found none.
static void chapterWorker(uint64_t arg)
{
   (void)arg;
   fetchChapters(requestedInput, &state.fetchedChapters);
   __sync_synchronize();
   state.chWorkerDone = 1;
   exitThread();
}

// WEB metadata is deferred until SELECT opens the panel; it never delays playback startup.
static void metadataWorker(uint64_t arg)
{
   (void)arg;
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
         strCopy(state.likeCount, sizeof state.likeCount, "Unavailable");
      }
   } else {
      strCopy(state.likeCount, sizeof state.likeCount, "Unavailable");
   }
   __sync_synchronize();
   state.metadataWorkerDone = 1;
   exitThread();
}

// Resolve YouTube's next recommendation off the UI thread; playback is untouched if this fails.
static void autoplayWorker(uint64_t arg)
{
   (void)arg;
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
   exitThread();
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
   launchVideo(input, 1);
}

void playLocalVideo(const char *path)
{
   if (!path || !path[0]) return;
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
   SCALE_PLAYBACK_LABEL(statusLabel);
   for (int i = 0; i < STAT_LINES; i++) {
      statLabels[i].size = playbackSubtitleFontSize(22);
      statLabels[i].fixedRenderScale = 0.0f;
      setLabelFixedRenderScale(&statLabels[i], 1.0f, 0);
   }
   SCALE_PLAYBACK_LABEL(timeLeftLabel);
   SCALE_PLAYBACK_LABEL(timeRightLabel);
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
   for (int i = 0; i < 3; i++) SCALE_PLAYBACK_LABEL(optionsCurrentValueLabels[i]);
   SCALE_PLAYBACK_LABEL(playbackSettingsTitle);
   SCALE_PLAYBACK_LABEL(playbackSubtitleStyleTitle);
   for (int i = 0; i < PLAYBACK_SUBTITLE_STYLE_COUNT; i++) SCALE_PLAYBACK_LABEL(playbackSubtitleStyleLabels[i]);
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
   setHttpRequestsCancelled(0);
   memset(&state, 0, sizeof state);
   state.askPromptShown = -1;
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   playbackFontScale = getGfxSdUiScale();
   cropPanX = cropPanY = 0.0f;
   cropPanLastUpdateUs = sys_time_get_system_time();
   if (playbackFontScale < 1.0f) playbackFontScale = 1.0f;

   uiFont = openSystemFont(FONT_POP);
   initLabel(&statusLabel, &uiFont, 0, 0, state.screenW - 80, AUTO, playbackFontSize(28), activeTheme->textPrimary, TEXT_WRAP, "");
   statsTextPadding = (int)(measureFontText(&uiFont, playbackSubtitleFontSize(22), "   ") + 0.5f);
   if (statsTextPadding < 3) statsTextPadding = 3;
   int statsTextMaxWidth = state.screenW - 80 - 2 * statsTextPadding;
   for (int i = 0; i < STAT_LINES; i++)
      initLabel(&statLabels[i], &uiFont, 0, 0, statsTextMaxWidth, AUTO, playbackSubtitleFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");
   initLabel(&timeLeftLabel,  &uiFont, 0, 0, 200, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&timeRightLabel, &uiFont, 0, 0, 200, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabelRaw(&toastLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");   // raw: shows track names
   initLabelRaw(&askPromptPrefixLabel, &uiFont, 0, 0, 180, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, "Press");
   initLabelRaw(&askPromptSuffixLabel, &uiFont, 0, 0, 260, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_NOWRAP, "to skip sponsor");
   initLabelRaw(&titleLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(22), activeTheme->textPrimary, TEXT_WRAP, "");
   initLabelRaw(&authorLabel, &uiFont, 0, 0, state.screenW / 2 - 80, AUTO, playbackFontSize(20), activeTheme->textSecondary, TEXT_WRAP, "");
   initLabelRaw(&nextNoticeHeadingLabel, &uiFont, 0, 0, 500, AUTO, playbackFontSize(22), activeTheme->accent, TEXT_NOWRAP, "NEXT VIDEO");
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
   for (int i = 0; i < OPTIONS_COUNT; i++)
      initLabelRaw(&optionsLabels[i], &uiFont, 0, 0, 520, AUTO, playbackFontSize(27), activeTheme->textPrimary, TEXT_NOWRAP, optionsNames[i]);
   for (int i = 0; i < OPTIONS_DROPDOWN_MAX; i++)
      initLabelRaw(&optionsValueLabels[i], &uiFont, 0, 0, 360, AUTO, playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabelRaw(&optionsCurrentValueLabels[0], &uiFont, 0, 0, 0, AUTO,
                playbackFontSize(27), activeTheme->textSecondary, TEXT_NOWRAP, "");
   initLabelRaw(&optionsCurrentValueLabels[1], &uiFont, 0, 0, 400, AUTO,
                playbackFontSize(27), activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabelRaw(&optionsCurrentValueLabels[2], &uiFont, 0, 0, 400, AUTO,
                playbackFontSize(27), activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");

   playbackSettingsOpen = playbackSponsorOpen = playbackSubtitleStyleOpen = 0;
   playbackSettingsSelected = playbackSponsorSelected = 0;
   initLabelRaw(&playbackSettingsTitle, &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, "Playback Settings");
   static const char *playbackSettingNames[PLAYBACK_SETTINGS_COUNT] = {
      "Enable 1080p (experimental)", "UI Scaling", "Overscan", "Subtitle Style", "SponsorBlock"
   };
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      initLabelRaw(&playbackSettingsLabels[i], &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 230, AUTO,
                   playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
                   playbackSettingNames[i]);
      initLabelRaw(&playbackSettingsValues[i], &uiFont, 0, 0, 150, AUTO,
                   playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, "");
   }
   initLabelRaw(&playbackSettingsToggleLabels[0], &uiFont, 0, 0, 60, AUTO,
                playbackFontSize(18), activeTheme->textPrimary, TEXT_NOWRAP, "OFF");
   initLabelRaw(&playbackSettingsToggleLabels[1], &uiFont, 0, 0, 60, AUTO,
                playbackFontSize(18), activeTheme->textPrimary, TEXT_NOWRAP, "ON");
   initLabelRaw(&playbackSubtitleStyleTitle, &uiFont, 0, 0, PLAYBACK_SETTINGS_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, "Subtitle Style");
   static const char *subtitleStyleNames[PLAYBACK_SUBTITLE_STYLE_COUNT] = {
      "Text Color", "Background Color", "Background Opacity", "Text Highlight", "Font Size"
   };
   for (int i = 0; i < PLAYBACK_SUBTITLE_STYLE_COUNT; i++)
      initLabelRaw(&playbackSubtitleStyleLabels[i], &uiFont, 0, 0, 300, AUTO,
                   playbackFontSize(20), activeTheme->textPrimary, TEXT_NOWRAP, subtitleStyleNames[i]);
   initLabelRaw(&playbackSubtitlePreview, &uiFont, 0, 0, 520, AUTO,
                playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4), 0xFFFFFFFF, TEXT_WRAP, "Lorem Ipsum");
   initLabelRaw(&playbackSponsorTitle, &uiFont, 0, 0, PLAYBACK_SPONSOR_PANEL_W - 48, AUTO,
                playbackFontSize(24), activeTheme->textPrimary, TEXT_NOWRAP, "SponsorBlock");
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++)
      initLabelRaw(&playbackSponsorCategoryLabels[i], &uiFont, 0, 0, 340, AUTO,
                   playbackFontSize(18), activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
                   getSponsorCategoryName((SponsorCategory)i));
   initLabelRaw(&playbackSponsorActionLabels[0], &uiFont, 0, 0, PLAYBACK_SPONSOR_OPTION_W, AUTO,
                playbackFontSize(16), activeTheme->textPrimary, TEXT_NOWRAP, "Skip");
   initLabelRaw(&playbackSponsorActionLabels[1], &uiFont, 0, 0, PLAYBACK_SPONSOR_OPTION_W, AUTO,
                playbackFontSize(16), activeTheme->textPrimary, TEXT_NOWRAP, "Ask");
   initLabelRaw(&playbackSponsorActionLabels[2], &uiFont, 0, 0, PLAYBACK_SPONSOR_OPTION_W, AUTO,
                playbackFontSize(16), activeTheme->textPrimary, TEXT_NOWRAP, "Keep");

   state.subtitlePick = state.subtitleFetched = -1;   // default off (memset left them 0)

   initVolumeMeter(&volumeMeter, &uiFont, activeTheme->accent, volumeLevel);
   applyPlaybackUiLabelScale(playbackFontScale);
   layoutVolumeMeter(&volumeMeter, state.screenW, state.screenH);

   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-play") == 0);
   if (!state.threadActive) fail("couldn't start worker");

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

// Rebuild the right-side playback HUD only on input/state changes, never in the render loop.
static void refreshPlaybackInfoHud(void)
{
   char info[256];
   int used = 0;
   if (state.vidW > 0 && state.vidH > 0)
      used = snprintf(info, sizeof info, "%dx%d", state.vidW, state.vidH);
   else
      used = snprintf(info, sizeof info, "Resolution unavailable");

   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      used += snprintf(info + used, sizeof info - used, "\nAudio: %s", state.audioNames[state.audioPick]);
   else if (state.audItag && state.audItag == state.vidItag)
      used += snprintf(info + used, sizeof info - used, "\nAudio: Embedded");
   else if (state.audItag)
      used += snprintf(info + used, sizeof info - used, "\nAudio: AAC");
   else
      used += snprintf(info + used, sizeof info - used, "\nAudio: unavailable");

   if (state.subtitlePick >= 0 && state.subtitlePick < state.captionCount)
      snprintf(info + used, sizeof info - used, "\nSubtitles: %s", state.captionTracks[state.subtitlePick].name);

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
   state.subtitlePick = trackIndex;
   if (trackIndex >= 0) lastSubtitlePick = trackIndex;
   state.subtitlesReady = 0;
   state.subtitleScanFrom = 0;
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
      showToast("Subtitles: OFF");
      return;
   }

   const CaptionTrack *track = &state.captionTracks[trackIndex];
   if (track->isAutoGenerated && !strstr(track->name, "auto-generated") &&
       !strstr(track->name, "auto-translated")) {
      snprintf(notice, sizeof notice, "Subtitles: %s (auto-generated)", track->name);
   } else {
      snprintf(notice, sizeof notice, "Subtitles: %s", track->name);
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
         track = findPreferredSubtitleTrack(getSubtitleLanguage());
      if (track < 0) {
         for (int i = 0; i < state.captionCount; i++)
            if (!subtitleIsAutoTranslated(&state.captionTracks[i])) { track = i; break; }
      }
   }
   setSubtitleTrack(track);
}

// fetches the picked caption track in the background; updatePlay reaps it and respawns on a new pick.
static void subtitleWorker(uint64_t arg)
{
   (void)arg;
   CaptionTrack *track = &state.captionTracks[state.subtitleFetched];
   state.subtitleFetchResult = track->isLocalFile ?
      loadSubtitlesSrt(track->url, state.subtitleTrack) :
      fetchSubtitles(track->url, state.subtitleTrack);   // a failure leaves count 0
   __sync_synchronize();
   state.subWorkerDone = 1;
   exitThread();
}

// Triangle only toggles subtitles. The selected track is retained while OFF.
static void toggleSubtitles(void)
{
   if (state.captionCount == 0 || state.isLive) { showToast("No subtitles"); return; }

   if (subtitlesEnabled) {
      subtitlesEnabled = 0;
      setSubtitleTrack(-1);
      showToast("Subtitles: OFF");
      return;
   }

   subtitlesEnabled = 1;
   applyPreferredSubtitleTrack();
   if (state.subtitlePick < 0) {
      subtitlesEnabled = 0;
      showToast("No subtitles available");
      return;
   }
   showSelectedSubtitleToast();
}

static void audioSwitchWorker(uint64_t arg)
{
   (void)arg;
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
   exitThread();
}

static VideoPlayer *openReadyResolutionPlayer(const char *videoUrl, const char *audioUrl, float position)
{
   VideoPlayer *replacement = createVideoPlayerSplit(videoUrl, audioUrl && audioUrl[0] ? audioUrl : NULL,
                                                     allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (!replacement) return NULL;
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
   while (!isVideoPlayerReady(replacement) &&
          sys_time_get_system_time() - started < 15000000ull) sleepMs(50);
   if (!isVideoPlayerReady(replacement)) {
      logWarn("[video-diag] resolution stream did not reach decoded pre-roll\n");
      destroyVideoPlayer(replacement);
      return NULL;
   }
   return replacement;
}

static void resolutionSwitchWorker(uint64_t arg)
{
   (void)arg;
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
      strCopy(state.message, sizeof state.message, "Video decoder recovery failed");
      state.stage = STAGE_FAILED;
   }
   if (!replacement && !state.resolutionSwitchAborting && !state.resolutionSwitchTeeRecovery) {
      strCopy(state.message, sizeof state.message, "Resolution switch and recovery failed");
      state.stage = STAGE_FAILED;
   }

   state.pendingResolutionPlayer = replacement;
   __sync_synchronize();
   state.resolutionSwitchDone = 1;
   exitThread();
}

static int startResolutionSwitch(int target, int automaticFallback)
{
   if (target < 0 || target >= state.videoCount || !state.videoAvailable[target] ||
       !state.player || state.resolutionSwitchThreadActive || state.audioSwitchThreadActive ||
       state.seeking || (state.isLive && automaticFallback != 2)) return 0;
   if (strcmp(state.videoUrls[target], state.videoUrls[state.resolutionIndex]) == 0) return 0;

   state.resolutionSwitchTarget = target;
   state.resolutionSwitchOldIndex = state.resolutionIndex;
   state.resolutionSwitchRestored = 0;
   state.resolutionSwitchAborting = 0;
   state.resolutionSwitchTeeRecovery = (automaticFallback == 2);
   state.resolutionSwitchWasPaused = isVideoPaused(state.player);
   state.resolutionSwitchWasFirstFrameSeen = state.firstFrameSeen;
   state.resolutionSwitchPosition = state.isLive ? 0.0f : getVideoPositionSeconds(state.player);
   if (state.isLive) {
      state.liveAnchored = 0;
      state.liveFirstPtsNs = 0;
   }
   strCopy(state.resolutionSwitchVideoUrl, sizeof state.resolutionSwitchVideoUrl, state.videoUrls[target]);
   strCopy(state.resolutionSwitchOldVideoUrl, sizeof state.resolutionSwitchOldVideoUrl,
           state.videoUrls[state.resolutionIndex]);
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
      if (automaticFallback != 2) showToast("Resolution switch could not start");
      return 0;
   }
   if (automaticFallback != 2) {
      char notice[96];
      if (state.videoFps[target] > 0) {
         if (automaticFallback)
            snprintf(notice, sizeof notice, "Audio issue; reducing to %dp @ %d FPS",
                     state.videoHeights[target], state.videoFps[target]);
         else
            snprintf(notice, sizeof notice, "Changing to %dp @ %d FPS",
                     state.videoHeights[target], state.videoFps[target]);
      } else {
         if (automaticFallback)
            snprintf(notice, sizeof notice, "Audio issue; reducing to %dp", state.videoHeights[target]);
         else
            snprintf(notice, sizeof notice, "Changing to %dp", state.videoHeights[target]);
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

static void startAudioTrackSwitch(int target)
{
   if (state.seeking) {
      logWarn("[audio-diag] audio selection ignored while a seek transaction is pending\n");
      showToast("Wait for seek to finish before switching audio");
      return;
   }
   if (target < 0 || target >= state.audioCount) {
      showToast("Audio track unavailable");
      return;
   }
   if (target == state.audioPick) return;
   if (state.isLive || !state.player || state.audioSwitchThreadActive) {
      showToast("Audio track busy/unavailable");
      return;
   }

   state.audioSwitchPosition = getVideoPositionSeconds(state.player);
   state.audioSwitchWasPaused = isVideoPaused(state.player);
   state.audioSwitchTarget = target;
   state.audioFallbackAttempted = 0;
   state.audioSwitchFallback = 0;
   setVideoPaused(state.player, 1);
   snprintf(state.message, sizeof state.message, "Loading audio: %s", state.audioNames[target]);
   refreshPlaybackInfoHud();
   showControls();
   state.audioSwitchDone = 0;
   state.audioSwitchThreadActive = (spawnJoinableThread(&state.audioSwitchTid, audioSwitchWorker, 0,
                                    THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-audio") == 0);
   if (!state.audioSwitchThreadActive) {
      state.message[0] = 0;
      if (!state.audioSwitchWasPaused) setVideoPaused(state.player, 0);
      showToast("Audio switch failed");
   }
}

// accumulates a scrub target while left/right is held; the engine seek fires once input goes quiet
static void nudgeSeek(float deltaSeconds, uint64_t nowUs)
{
   if (!state.seeking) { state.seeking = 1; state.seekTarget = getVideoPositionSeconds(state.player); }
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
   VideoPlayer *old = state.liveRetiringPlayer;
   state.liveRetiringPlayer = NULL;
   if (old) destroyVideoPlayer(old);

   VideoPlayer *replacement = NULL;
   if (!playbackCancelRequested())
      replacement = createVideoPlayerSplitCancelable(state.liveVideoOpen,
                         state.liveAudioOpen[0] ? state.liveAudioOpen : NULL,
                         allocGfxVideoBuffer, freeGfxVideoBuffer, &state.cancelRequested);
   if (playbackCancelRequested() && replacement) {
      destroyVideoPlayer(replacement);
      replacement = NULL;
   }
   if (!replacement && !playbackCancelRequested()) fail("LIVE seek/reconnect failed");
   state.pendingPlayer = replacement;
   __sync_synchronize();
   state.workerDone = 1;
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
      showToast("This live stream has rewind turned off");
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
         showToast("This live stream has rewind turned off");
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

static void handlePlaybackInput(void)
{
   if (state.audioSwitchThreadActive || state.resolutionSwitchThreadActive || state.liveReopenThreadActive)
      return; // keep the source stable until the staged handoff commits
   uint64_t nowUs = sys_time_get_system_time();

   if (isPadButtonPressed(PAD_BTN_L2)) {
      refreshPlaybackInfoHud();
      showFullPlaybackHud();
   }

   updateCropPan();

   if (isPadButtonPressed(PAD_BTN_R2)) {
      cropToFillEnabled = !cropToFillEnabled;
      cropPanX = cropPanY = 0.0f;
      showToast(cropToFillEnabled ? "Crop to fill: ON" : "Crop to fill: OFF");
   }


   if (state.isLive) {
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         setVideoPaused(state.player, !isVideoPaused(state.player));
         if (isVideoPaused(state.player)) showFullPlaybackHud();
         else showControls();
      }
   } else {
      static uint64_t crossHoldStartUs;
      static int crossRestartTriggered;
      PadButtonState crossState = getPadButtonState(PAD_BTN_CROSS);

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
         if (!crossRestartTriggered) {
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
         snprintf(notice, sizeof notice, "Skipped %s", getSponsorCategoryName(segment->category));
         showToast(notice);
      }
   }

   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) toggleSubtitles();

   if (isPadButtonPressed(PAD_BTN_L1)) {
      if (requestedIsLocal) { showToast("Unavailable for downloaded videos"); return; }
      char previousId[12];
      if (previousPlaybackId(previousId)) {
         popScreen();
         launchVideo(previousId, 0);
         return;
      }
      showToast("No previous video");
   }
   if (isPadButtonPressed(PAD_BTN_R1) && state.isLive && !state.liveEnded && state.liveSegmentSec > 0) {
      float actualBehind = liveTimelineLength() - liveTimelinePosition();
      float targetBehind = liveTimelineLength() - (state.seeking ? state.seekTarget : liveTimelinePosition());
      state.seeking = 0;
      if (isVideoPaused(state.player) || !isLiveBehindAtEdge(actualBehind) ||
          !isLiveBehindAtEdge(targetBehind)) {
         if (reopenLiveAt(-1)) showToast("Back to LIVE");
         else showToast("Could not return to LIVE");
         return;
      }
   }

   if (isPadButtonPressed(PAD_BTN_R1)) {
      char forwardId[12];
      if (state.nextThreadActive || state.nextNoticeVisible) {
         showToast("Next video is already loading");
      } else if (nextPlaybackId(forwardId)) {
         // Reuse the already-known forward entry before asking YouTube for another video.
         popScreen();
         launchVideo(forwardId, 0);
         return;
      } else if (requestedIsLocal) {
         showToast("Unavailable for downloaded videos");
      } else {
         state.nextAttempted = 1;
         state.nextWorkerDone = 0;
         state.nextVideoId[0] = 0;
         state.nextVideoTitle[0] = 0;
         state.nextThreadActive = (spawnJoinableThread(&state.nextWorkerTid, autoplayWorker, 0,
                                   THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-next") == 0);
         if (!state.nextThreadActive) showToast("Next video unavailable");
         else showToast("Loading next recommendation...");
      }
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
      snprintf(notice, sizeof notice, "Skipped %s", getSponsorCategoryName(segment->category));
      showToast(notice);
      return;
   }
}

// Compose both sections into one texture so metadata and description share the same font scale,
// width and scrolling behaviour while remaining visually distinct through section headings.
static void renderDescription(void)
{
   char content[3072];
   char views[48];
   const char *rawViews = state.viewCount[0] ? state.viewCount : "Unavailable";
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

   snprintf(content, sizeof content,
            "METADATA\nViews: %s\nPublished: %s\nLikes: %s\n"
            "----------------------------------------\n"
            "DESCRIPTION\n\n%s",
            views, state.publishedDate[0] ? state.publishedDate : "Unavailable",
            state.likeCount[0] ? state.likeCount : "Loading...",
            state.description[0] ? state.description : "No description.");
   if (descriptionTexture.tex.w) freeTextTexture(&descriptionTexture);
   renderFontRaw(&descriptionTexture, &uiFont, (int)(playbackFontSize(DESCRIPTION_TEXT_SIZE) * playbackFontScale + 0.5f), content,
                 activeTheme->textPrimary, state.screenW - 2 * DESCRIPTION_MARGIN_X, TEXT_WRAP);
   state.descriptionRendered = 1;
}

// SELECT opens the metadata + description panel. Likes are fetched asynchronously on first open;
// the request never delays playback startup or blocks drawing/input.
static void openDescription(void)
{
   if (!state.metadataFetchTried) {
      state.metadataFetchTried = 1;
      if (requestedIsLocal) {
         strCopy(state.likeCount, sizeof state.likeCount, "Offline");
      } else {
      state.likeCount[0] = 0;
      state.metadataWorkerDone = 0;
      state.metadataThreadActive = (spawnJoinableThread(&state.metadataWorkerTid, metadataWorker, 0,
                                     THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-metadata") == 0);
      if (!state.metadataThreadActive)
         strCopy(state.likeCount, sizeof state.likeCount, "Unavailable");
      }
   }
   if (!state.descriptionRendered) renderDescription();
   state.descriptionScroll = 0;
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

   static ButtonRepeat scrollRepeat;
   if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_DOWN)))    state.descriptionScroll += DESCRIPTION_SCROLL_STEP;
   else if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_UP))) state.descriptionScroll -= DESCRIPTION_SCROLL_STEP;

   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   int maxScroll = descriptionTexture.tex.h > viewHeight ? descriptionTexture.tex.h - viewHeight : 0;
   if (state.descriptionScroll > maxScroll) state.descriptionScroll = maxScroll;
   if (state.descriptionScroll < 0) state.descriptionScroll = 0;
}

// START opens the chapter picker: the rows are rasterised once (off the draw path), the widest time
// and title size the two columns, and the selection starts on the chapter the playhead is in.
static void openChapters(void)
{
   // Avoid fetching YouTube's large /next response on every playback. Fetch only when the user asks
   // for chapters; the request stays off the draw path and can be cancelled cleanly on exit.
   if (state.isLive) { showToast("No chapters"); return; }
   if (state.chapters.count == 0) {
      if (!state.chFetchTried && isBareVideoId(requestedInput)) {
         state.chFetchTried = 1;
         state.chWorkerDone = 0;
         state.chThreadActive = (spawnJoinableThread(&state.chWorkerTid, chapterWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-chapters") == 0);
      }
      showToast(state.chThreadActive ? "Loading chapters..." : "No chapters");
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

static void openPlaybackSettings(void)
{
   playbackSettingsOpen = 1;
   playbackSponsorOpen = 0;
   playbackSubtitleStyleOpen = 0;
   playbackSettingsSelected = 0;
   playbackSettingsRepeat.timer = playbackSettingsRepeat.repeats = 0;
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
      showToast("Could not save SponsorBlock setting");
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
      if (setSubtitleStyle(setting, value) != 0) showToast("Could not save subtitle style");
   }
}

static void updatePlaybackSettings(void)
{
   if (playbackSubtitleStyleOpen) { updatePlaybackSubtitleStyle(); return; }
   if (playbackSponsorOpen) { updatePlaybackSponsorSettings(); return; }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { playbackSettingsOpen = 0; return; }
   if (isRepeatDue(&playbackSettingsRepeat, getPadButtonState(PAD_BTN_DOWN)))
      playbackSettingsSelected = (playbackSettingsSelected + 1) % PLAYBACK_SETTINGS_COUNT;
   else if (isRepeatDue(&playbackSettingsRepeat, getPadButtonState(PAD_BTN_UP)))
      playbackSettingsSelected = (playbackSettingsSelected + PLAYBACK_SETTINGS_COUNT - 1) % PLAYBACK_SETTINGS_COUNT;

   int left = isPadButtonPressed(PAD_BTN_LEFT);
   int right = isPadButtonPressed(PAD_BTN_RIGHT);
   int cross = isPadButtonPressed(PAD_BTN_CROSS);
   if (!left && !right && !cross) return;

   if (playbackSettingsSelected == 0) {
      int enabled = getEnable1080p60();
      if (left) enabled = 0;
      else if (right) enabled = 1;
      else enabled = !enabled;
      if (setEnable1080p60(enabled) != 0) showToast("Could not save 1080p setting");
   } else if (playbackSettingsSelected == 1) {
      float current = playbackFontScale;
      setPlaybackUiScale(current >= 1.25f ? 1.0f : 1.5f);
   } else if (playbackSettingsSelected == 2) {
      int delta = left ? -1 : 1;
      int overscan = getOverscanPercent() + delta;
      if (setOverscanPercent(overscan) != 0) showToast("Could not save overscan setting");
   } else if (playbackSettingsSelected == 3 && (cross || right)) {
      playbackSubtitleStyleOpen = 1;
      playbackSubtitleStyleSelected = 0;
      playbackSubtitleStyleRepeat.timer = playbackSubtitleStyleRepeat.repeats = 0;
   } else if (playbackSettingsSelected == 4 && (cross || right)) {
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
               if (!startResolutionSwitch(target, 0)) showToast("Could not start quality change");
            } else if (target >= 0) {
               showToast(preferredQualityHeight ? "Quality preference saved" : "Quality: Auto");
            }
         } else if (optionsSelected == 1) {
            if (state.audioCount > 0)
               startAudioTrackSwitch(optionsDropdownSelected);
            else if (state.resolutionIndex >= 0 && state.resolutionIndex < state.videoCount &&
                     state.videoHasAudio[state.resolutionIndex])
               showToast("Original upload already active");
            else
               showToast("No audio tracks available");
         } else if (optionsSelected == 3) {
            int trackIndex = subtitleTrackAtMenuIndex(optionsDropdownSelected);
            if (!state.isLive && trackIndex >= 0 && trackIndex < state.captionCount) {
               CaptionTrack *track = &state.captionTracks[trackIndex];
               if (setSubtitleLanguage(track->languageCode) == 0) {
                  lastSubtitlePick = trackIndex;
                  if (subtitlesEnabled) setSubtitleTrack(trackIndex);
                  char notice[112];
                  snprintf(notice, sizeof notice, "Subtitle: %s", track->name);
                  showToast(notice);
               } else {
                  showToast("Could not save subtitle language");
               }
            } else {
               showToast("No subtitle languages available");
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
            showToast("Subtitles: OFF");
         }
      }
      return;
   }

   if (optionsSelected == 5 && (cross || left || right)) {
      int enabled = left ? 0 : right ? 1 : !cropToFillEnabled;
      if (enabled != cropToFillEnabled) {
         cropToFillEnabled = enabled;
         cropPanX = cropPanY = 0.0f;
         showToast(cropToFillEnabled ? "Crop to fill: ON" : "Crop to fill: OFF");
      }
      return;
   }

   if (optionsSelected == 6 && (cross || left || right)) {
      int enabled = left ? 0 : right ? 1 : !getAutoplayEnabled();
      if (enabled != getAutoplayEnabled()) {
         setAutoplayEnabled(enabled);
         showToast(getAutoplayEnabled() ? "Autoplay: ON" : "Autoplay: OFF");
      }
      return;
   }

   if (optionsSelected == 7 && (right || cross)) {
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
                             lastSubtitlePick : findPreferredSubtitleTrack(getSubtitleLanguage());
         optionsDropdownSelected = subtitleMainMenuIndexForTrack(selectedTrack);
      }
      optionsDropdownRepeat.timer = optionsDropdownRepeat.repeats = 0;
      optionsDropdownOpen = 1;
   }
}

static int playbackSettingsPanelWidth(void)
{
   int width = state.screenW - 48;
   return width < PLAYBACK_SETTINGS_PANEL_W ? width : PLAYBACK_SETTINGS_PANEL_W;
}

static int playbackSponsorPanelWidth(void)
{
   int width = state.screenW - 48;
   return width < PLAYBACK_SPONSOR_PANEL_W ? width : PLAYBACK_SPONSOR_PANEL_W;
}

static void drawPlaybackSettings(void)
{
   int panelW = playbackSettingsPanelWidth();
   int panelH = PLAYBACK_SETTINGS_PANEL_H;
   if (panelH > state.screenH - 48) panelH = state.screenH - 48;
   int x = (state.screenW - panelW) / 2;
   int y = (state.screenH - panelH) / 2;
   fillGfxRectangleFixed(0, 0, state.screenW, state.screenH, 0x78000000);
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);
   refreshLabelScale(&playbackSettingsTitle);
   drawLabelAt(&playbackSettingsTitle,
               state.screenW / 2 - playbackSettingsTitle.tt.tex.w / 2, y + 28);

   char overscanValue[16];
   int overscan = getOverscanPercent();
   if (overscan > 0) snprintf(overscanValue, sizeof overscanValue, "+%d%%", overscan);
   else snprintf(overscanValue, sizeof overscanValue, "%d%%", overscan);
   static const char *const playbackScaleNames[] = { "1.0x", "1.5x" };
   const char *scaleValue = playbackScaleNames[playbackFontScale >= 1.25f ? 1 : 0];
   if (strcmp(playbackSettingsValues[1].text, scaleValue) != 0)
      setLabelText(&playbackSettingsValues[1], scaleValue);
   setLabelText(&playbackSettingsValues[2], overscanValue);
   setLabelText(&playbackSettingsValues[3], "Configure");
   setLabelText(&playbackSettingsValues[4], "Configure");

   int toggleW = 132;
   int toggleX = x + panelW - 40 - toggleW;
   for (int i = 0; i < PLAYBACK_SETTINGS_COUNT; i++) {
      int rowY = y + 72 + i * PLAYBACK_SETTINGS_ROW_H;
      Label *name = &playbackSettingsLabels[i];
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
         refreshLabelScale(value);
         int valueW = value->tt.tex.w + 24;
         if (valueW < 100) valueW = 100;
         int valueX = x + panelW - 40 - valueW;
         int valueY = getCenteredLabelY(value, rowY, PLAYBACK_SETTINGS_ROW_H);
         if (i == playbackSettingsSelected)
            fillGfxRoundedRectangleFixed(valueX, valueY - 5, valueW, value->tt.tex.h + 10,
                                         7, activeTheme->rowHighlight);
         setLabelColor(value, i == playbackSettingsSelected ? activeTheme->accent : activeTheme->textPrimary);
         moveLabel(value, valueX + (valueW - value->tt.tex.w) / 2, valueY);
         drawLabel(value);
         if (i == 3 || i == 4) {
            int tx = x + panelW - 24;
            int ty = rowY + PLAYBACK_SETTINGS_ROW_H / 2;
            drawGfxTriangle(tx - 7, ty - 4, activeTheme->textSecondary,
                            tx + 7, ty - 4, activeTheme->textSecondary,
                            tx, ty + 4, activeTheme->textSecondary);
         }
      }
   }
}

static void drawSubtitleLabelWithHighlight(Label *label, int x, int y, uint32_t textColor)
{
   int highlight = getSubtitleStyle(3);
   if (highlight == 1) {
      // A heavier, more visible drop shadow, offset down and to the right.
      setLabelColor(label, 0xFF000000);
      drawLabelAt(label, x + 3, y + 3);
      drawLabelAt(label, x + 5, y + 5);
   } else if (highlight == 2) {
      // Two-pixel outline around the glyphs for stronger contrast on bright scenes.
      setLabelColor(label, 0xFF000000);
      for (int oy = -2; oy <= 2; oy++) for (int ox = -2; ox <= 2; ox++)
         if (ox == -2 || ox == 2 || oy == -2 || oy == 2)
            drawLabelAt(label, x + ox, y + oy);
   }
   setLabelColor(label, textColor);
   drawLabelAt(label, x, y);
}

static void drawPlaybackSubtitleStyle(void)
{
   static const char *const textNames[] = { "White", "Yellow", "Cyan", "Green", "Blue", "Red" };
   static const uint32_t textColors[] = { 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFF4488FF, 0xFFFF4444 };
   static const char *const bgNames[] = { "Black", "Dark Gray", "Gray", "Blue", "Transparent" };
   static const uint32_t bgColors[] = { 0xFF000000, 0xFF303030, 0xFF777777, 0xFF102040, 0x00000000 };
   static const char *const opacityNames[] = { "0%", "25%", "50%", "80%", "100%" };
   static const char *const highlightNames[] = { "None", "Shadow", "Outline" };
   static const char *const fontSizeNames[] = { "Small", "Medium", "Default", "Large", "Extra Large", "Huge" };
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
   setLabelText(&playbackSubtitlePreview, "Lorem Ipsum");
   setLabelColor(&playbackSubtitlePreview, color);
   refreshLabelScale(&playbackSubtitlePreview);
   int textW = playbackSubtitlePreview.tt.tex.w + 24;
   int textH = playbackSubtitlePreview.tt.tex.h + 12;
   int tx = state.screenW / 2 - textW / 2, ty = previewY + (previewH - textH) / 2;
   fillGfxRoundedRectangleFixed(tx, ty, textW, textH, 4, bg);
   drawSubtitleLabelWithHighlight(&playbackSubtitlePreview,
      state.screenW / 2 - playbackSubtitlePreview.tt.tex.w / 2, ty + 6, color);
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
   int optionH = 32;
   int optionGroupW = 3 * optionW + 2 * PLAYBACK_SPONSOR_OPTION_GAP;
   int optionX = x + panelW - 32 - optionGroupW;
   int rowTop = y + 86;
   for (int row = 0; row < SPONSOR_CATEGORY_COUNT; row++) {
      int rowY = rowTop + row * PLAYBACK_SPONSOR_ROW_H;
      int focused = row == playbackSponsorSelected;
      if (focused)
         fillGfxRoundedRectangleFixed(x + 18, rowY, panelW - 36, PLAYBACK_SPONSOR_ROW_H - 2,
                                      7, activeTheme->rowHighlight);
      fillGfxCircle(x + 34, rowY + PLAYBACK_SPONSOR_ROW_H / 2, 5,
                    getSponsorCategoryColor((SponsorCategory)row));
      Label *name = &playbackSponsorCategoryLabels[row];
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
   if (label->width != width) {
      label->width = width;
      label->text[0] = '\0'; // force a reraster with the new available width
   }
   setLabelText(label, value ? value : "Unavailable");
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
   const char *audioValue = "Unavailable";
   if (state.audioPick >= 0 && state.audioPick < state.audioCount &&
       state.audioPick < optionsValueCounts[1]) {
      audioValue = optionsAudioLabels[state.audioPick];
   } else if (state.resolutionIndex >= 0 && state.resolutionIndex < state.videoCount &&
              state.videoHasAudio[state.resolutionIndex]) {
      audioValue = "Original upload";
   } else if (state.fallbackAudioUrl[0]) {
      audioValue = "Original upload";
   } else if (optionsValueCounts[1] > 0 && state.audioCount > 0) {
      audioValue = optionsAudioLabels[0];
   }
   setOptionsCurrentValue(&optionsCurrentValueLabels[1], audioValue, valueWidth);

   int subtitleIndex = state.subtitlePick;
   if (subtitleIndex < 0 || subtitleIndex >= state.captionCount) subtitleIndex = lastSubtitlePick;
   if (subtitleIndex < 0 || subtitleIndex >= state.captionCount)
      subtitleIndex = findPreferredSubtitleTrack(getSubtitleLanguage());
   const char *subtitleValue = (subtitleIndex >= 0 && subtitleIndex < state.captionCount) ?
                               state.captionTracks[subtitleIndex].name : "Unavailable";
   setOptionsCurrentValue(&optionsCurrentValueLabels[2], subtitleValue, valueWidth);
}

static void drawPlaybackOptions(void)
{
   int panelW = 680;
   int rowH = state.screenH < 560 ? 44 : 54;
   int panelH = 70 + OPTIONS_COUNT * rowH;
   int x = (state.screenW - panelW) / 2;
   int y = (state.screenH - panelH) / 2;
   int valueFontSize = (int)(optionsLabels[3].size * playbackFontScale + 0.5f);
   int twoSpaceGap = (int)(measureFontText(&uiFont, valueFontSize, "  ") + 0.5f);
   int valueX = x + 38 + optionsLabels[3].tt.tex.w + twoSpaceGap;
   int arrowLeft = x + panelW - 44 - 7;
   int valueRight = arrowLeft - twoSpaceGap;
   int valueWidth = valueRight - valueX;
   if (valueWidth < 8) valueWidth = 8;
   updateOptionsCurrentValues(valueWidth);

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
   fillGfxRoundedRectangleFixed(x, y, panelW, panelH, 14, 0xC0101724);
   fillGfxRoundedRectangleFixed(x, y, panelW, 4, 2, activeTheme->accent);

   for (int i = 0; i < OPTIONS_COUNT; i++) {
      int rowY = y + 32 + i * rowH;
      Label *name = &optionsLabels[i];
      int nameY = getCenteredLabelY(name, rowY, rowH);

      if (i == optionsSelected)
         fillGfxRoundedRectangleFixed(x + 18, nameY - 7, panelW - 36,
                                      name->tt.tex.h + 14, 7, 0x805A7898);
      drawLabelAt(name, x + 38, nameY);

      if (i == 0 || i == 1 || i == 3) {
         Label *value = &optionsCurrentValueLabels[i == 0 ? 0 : i == 1 ? 1 : 2];
         if (i == 0) {
            int qualityX = arrowLeft - value->tt.tex.w - 12;
            if (qualityX < x + 38) qualityX = x + 38;
            drawLabelAt(value, qualityX, getCenteredLabelY(value, rowY, rowH));
         } else {
            int valueXAligned = arrowLeft - value->tt.tex.w - 12;
            if (valueXAligned < valueX) valueXAligned = valueX;
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
      } else if (i == 0 || i == 1 || i == 3 || i == 4 || i == 6 || i == 7) {
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
      int dropW = 420;
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
         const char *itemText = optionsSelected == 0 ? optionsQualityLabels[i] :
                                optionsSelected == 1 ? optionsAudioLabels[i] :
                                optionsSelected == 3 ? (subtitleTrackIndex >= 0 ?
                                   state.captionTracks[subtitleTrackIndex].name : "Unavailable") :
                                optionsValues[optionsSelected][i];
         setLabelText(value, itemText);
         int itemY = dy + 8 + row * 44;
         int labelY = getCenteredLabelY(value, itemY, 36);
         if (i == optionsDropdownSelected)
            fillGfxRoundedRectangleFixed(dx + 10, labelY - 6,
                                         dropW - 20, value->tt.tex.h + 12,
                                         6, 0xA05A7898);
         drawLabelAt(value, dx + 26, labelY);
      }
   }
}

static void updatePlay(void)
{
   if (consumeDownloadCompletion()) showToastFor("Download Complete", 3000000ULL);

   // During initial loading there is no adopted VideoPlayer yet. Treat Circle as an
   // interrupt before any HUD/paused-state query can inspect state.player.
   if (state.stage == STAGE_LOADING && isPadButtonPressed(PAD_BTN_CIRCLE)) {
      requestPlaybackCancel();
      popScreen();
      return;
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
         state.message[0] = 0;
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
         if (state.pauseWhenReady) setVideoPaused(state.player, 1);
         setLabelText(&titleLabel, state.title);   // rasterise once, off the draw path
         setLabelText(&authorLabel, state.author);
         refreshPlaybackInfoHud();
         state.stage = STAGE_PLAYING;
         rememberAutoplayVideo(requestedInput);
         showControls();
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
         showToast("Audio switch failed; video continues");
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
      showToastFor("Decoder failed to stop safely. Please restart the application.", 5000000ULL);
      state.decoderStopWarningShown = 1;
   }

   // A fatal TEE decode error triggers an invisible handoff to a lower rendition.
   // Keep the status label on "Loading..." and never retry the failed TEE URL.
   if (state.stage == STAGE_PLAYING && state.player &&
       !state.teeRecoveryAttempted && !state.resolutionSwitchThreadActive &&
       !state.audioSwitchThreadActive && !state.seeking &&
       isVideoPlayerTeeFailed(state.player)) {
      state.teeRecoveryAttempted = 1;
      int lowerProfile = findLowerResolutionProfile();
      if (lowerProfile >= 0) {
         if (!startResolutionSwitch(lowerProfile, 2)) {
            strCopy(state.message, sizeof state.message, "Video decoder recovery could not start");
            state.stage = STAGE_FAILED;
         }
      } else {
         strCopy(state.message, sizeof state.message, "No compatible fallback video quality");
         state.stage = STAGE_FAILED;
      }
   }

   // Commit a resolution change only after its replacement player has reached a decoded pre-roll.
   if (state.resolutionSwitchDone && state.resolutionSwitchThreadActive) {
      joinThread(state.resolutionSwitchTid);
      state.resolutionSwitchThreadActive = 0;
      if (state.pendingResolutionPlayer) {
         int teeRecovery = state.resolutionSwitchTeeRecovery;
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
         state.message[0] = 0;
         if (!teeRecovery) {
            if (state.resolutionSwitchRestored) {
               showToast("Resolution unavailable; previous stream restored");
            } else {
               char notice[64];
               if (state.vidFps > 0)
                  snprintf(notice, sizeof notice, "Video: %dp @ %d FPS", state.vidH, state.vidFps);
               else
                  snprintf(notice, sizeof notice, "Video: %dp", state.vidH);
               showToast(notice);
            }
         }
         state.resolutionSwitchTeeRecovery = 0;
      } else {
         if (!state.resolutionSwitchTeeRecovery)
            showToast("Resolution switch and recovery failed");
         state.resolutionSwitchTeeRecovery = 0;
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
            snprintf(state.nextVideoTitle, sizeof state.nextVideoTitle, "Video %s", state.nextVideoId);
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
         showToast("No next recommendation");
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

   // subtitles: reap a finished fetch (stale if the pick moved mid-fetch), then start one whenever
   // the pick and the fetched track disagree
   if (state.subWorkerDone && state.subThreadActive) {
      joinThread(state.subWorkerTid);
      state.subThreadActive = 0;
      state.subtitlesReady = (state.subtitleFetched == state.subtitlePick);
      if (!state.subtitlesReady) state.subtitleFetched = -1;   // pick moved mid-fetch: re-picking must refetch
      else if (state.subtitleTrack->count == 0) {   // fetch failed or empty track: drop to Off, visibly
         int failedTrack = state.subtitleFetched;
         int fetchResult = state.subtitleFetchResult;
         int translated = failedTrack >= 0 && failedTrack < state.captionCount &&
                          subtitleIsAutoTranslated(&state.captionTracks[failedTrack]);
         state.subtitlePick = state.subtitleFetched = -1;
         subtitlesEnabled = 0;
         lastSubtitlePick = -1;
         state.subtitlesReady = 0;
         refreshPlaybackInfoHud();
         if (translated && fetchResult == -429)
            showToast("YouTube rate-limited translation");
         else if (translated && fetchResult == 0)
            showToast("YouTube returned empty translation");
         else
            showToast("Subtitles unavailable");
      }
   }
   if (state.subtitlePick >= 0 && state.subtitlePick != state.subtitleFetched && !state.subThreadActive) {
      if (!state.subtitleTrack) state.subtitleTrack = malloc(sizeof *state.subtitleTrack);
      if (state.subtitleTrack) {
         state.subtitlesReady = 0;
         state.subtitleScanFrom = 0;
         state.subWorkerDone = 0;
         state.subtitleFetchResult = 0;
         state.subtitleFetched = state.subtitlePick;
         state.subThreadActive = (spawnJoinableThread(&state.subWorkerTid, subtitleWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-subs") == 0);
         if (!state.subThreadActive) state.subtitleFetched = -1;   // spawn failed: retry next frame
      }
   }

   // feed the cue covering the current position into the label; setLabelText only rasterises on change
   if (state.subtitlesReady && state.subtitlePick >= 0 && state.stage == STAGE_PLAYING && state.player) {
      subtitleLabel.size = playbackSubtitleFontSize(22 + getSubtitleStyle(4) * 4);
      setLabelText(&subtitleLabel, getSubtitleCueText(state.subtitleTrack, getVideoPositionSeconds(state.player), &state.subtitleScanFrom));
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

   // With autoplay disabled, ending a VOD behaves like pressing Circle:
   // return to the screen that launched this video instead of requesting /next.
   if (state.stage == STAGE_PLAYING && state.player && (!state.isLive || state.liveEnded) &&
       isVideoEnded(state.player) && !getAutoplayEnabled()) {
      popScreen();
      return;
   }

   // Automatic playback: ask /next once after a VOD reaches its end. Network failure is non-fatal.
   if (state.stage == STAGE_PLAYING && state.player && (!state.isLive || state.liveEnded) &&
       isVideoEnded(state.player) && !state.nextAttempted && !state.nextThreadActive) {
      state.nextAttempted = 1;
      state.nextWorkerDone = 0;
      state.nextVideoId[0] = 0;
      state.nextThreadActive = (spawnJoinableThread(&state.nextWorkerTid, autoplayWorker, 0,
                                THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-autoplay") == 0);
      if (!state.nextThreadActive) showToast("Autoplay unavailable");
   }

   // status text: the failure reason, or "Loading..." until playback. setLabelText skips unchanged
   // text, so calling it each frame only rasterises on a real change.
   if (state.audioSwitchThreadActive) setLabelText(&statusLabel, state.message[0] ? state.message : "Loading audio...");
   else if (state.resolutionSwitchThreadActive)
      setLabelText(&statusLabel, state.resolutionSwitchTeeRecovery ? "Loading..." : "Changing resolution...");
   else if (state.stage == STAGE_FAILED) setLabelText(&statusLabel, state.message);
   else if (state.stage != STAGE_PLAYING) setLabelText(&statusLabel, state.message[0] ? state.message : "Loading...");
}

// measure the real presented frame rate: a changed frame pointer is a new frame; tally them per second.
static void measureFps(const uint8_t *frame)
{
   if (frame && frame != state.lastFrame) { state.framesThisSecond++; state.lastFrame = frame; }
   uint64_t now = sys_time_get_system_time();
   if (now - state.fpsTickUs >= 1000000) { state.measuredFps = state.framesThisSecond; state.framesThisSecond = 0; state.fpsTickUs = now; }
}

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

   snprintf(line, sizeof line, "Video  itag %d   %dx%d   H.264", state.vidItag, state.vidW, state.vidH);
   setLabelText(&statLabels[0], line);
   snprintf(line, sizeof line, "FPS  %d   (target %d)", state.measuredFps, state.vidFps);
   setLabelText(&statLabels[1], line);
   if (state.audItag) {
      if (audioName && audioName[0])
         snprintf(line, sizeof line, "Audio  itag %d   %s\nAAC %d Hz   %d ch",
                  state.audItag, audioName, rate, channels);
      else
         snprintf(line, sizeof line, "Audio  itag %d\nAAC %d Hz   %d ch",
                  state.audItag, rate, channels);
   } else {
      strCopy(line, sizeof line, "Audio  none");
   }
   setLabelText(&statLabels[2], line);
   snprintf(line, sizeof line, "Time  %d:%02d / %d:%02d", (int)pos / 60, (int)pos % 60, (int)duration / 60, (int)duration % 60);
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
   if (state.hudShownUs != 0 && now - state.hudShownUs < HUD_VISIBLE_US) return 1;
   return state.controlsShownUs != 0 && now - state.controlsShownUs < CONTROLS_VISIBLE_US;
}

static int fullPlaybackHudVisible(void)
{
   if (state.controlsDismissed) return 0;
   if (isVideoPaused(state.player)) return 1;
   return state.hudShownUs != 0 &&
          sys_time_get_system_time() - state.hudShownUs < HUD_VISIBLE_US;
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

// a red YouTube-style progress bar low on the screen with current/total time on each side, drawn from
// plain rectangles (no spritesheet), with sponsor segments marked over the fill.
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
   int barY     = (int)(state.screenH * 0.94f);
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
   drawLabelAt(&timeRightLabel, barRight + 20, barY - timeRightLabel.tt.tex.h / 2);
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
   int x = state.screenW / 2 - subtitleLabel.tt.tex.w / 2;
   int y = (int)(state.screenH * 0.94f) - 40 - subtitleLabel.tt.tex.h;
   static const uint32_t textColors[] = { 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFF4488FF, 0xFFFF4444 };
   static const uint32_t bgColors[] = { 0xFF000000, 0xFF303030, 0xFF777777, 0xFF102040, 0x00000000 };
   uint32_t bg = bgColors[getSubtitleStyle(1)];
   bg = (bg & 0x00FFFFFF) | ((uint32_t[]){ 0, 64, 128, 204, 255 }[getSubtitleStyle(2)] << 24);
   fillGfxRectangle(x - 12, y - 6, subtitleLabel.tt.tex.w + 24, subtitleLabel.tt.tex.h + 12, bg);
   drawSubtitleLabelWithHighlight(&subtitleLabel, x, y, textColors[getSubtitleStyle(0)]);
}

// the description over the whole video: a semi-transparent black layer, then the slice of the text
// texture the scroll position selects (the texture is drawn through a window, so no clipping is needed).
static void drawDescription(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
   if (descriptionTexture.tex.w == 0) return;

   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   int visibleHeight = descriptionTexture.tex.h - state.descriptionScroll;
   if (visibleHeight > viewHeight) visibleHeight = viewHeight;
   if (visibleHeight <= 0) return;

   float sliceTop    = (float)state.descriptionScroll / descriptionTexture.tex.h;
   float sliceBottom = (float)(state.descriptionScroll + visibleHeight) / descriptionTexture.tex.h;
   drawGfxTexture(DESCRIPTION_MARGIN_X, DESCRIPTION_MARGIN_Y, descriptionTexture.tex.w, visibleHeight,
                  descriptionTexture.tex, 0.0f, sliceTop, 1.0f, sliceBottom, 0xFFFFFFFF, GFX_FILTER_NEAREST);
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
   uint64_t duration = state.toastDurationUs ? state.toastDurationUs : TOAST_VISIBLE_US;
   if (state.toastShownUs == 0 || sys_time_get_system_time() - state.toastShownUs >= duration) return;
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
   // The selected SD UI zoom is intended for library/navigation screens only (including
   // 480-line anamorphic 16:9). Keep playback controls and overlays at their original
   // 1.0x geometry so the seek bar and other HUD elements retain their designed layout.
   float selectedUiScale = getGfxSdUiScale();
   setGfxSdUiScale(1.0f);

   clearGfx(COLOR_BLACK);   // clear the full output; overscan transforms video and HUD, not the black margins

   const uint8_t *frame = NULL;
   if (state.player) {
      int w, h;
      frame = getVideoFrame(state.player, &w, &h);
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
         if (state.isLive && !state.liveAnchored) {
            long actualFirst = getVideoLiveFirstSq(state.player);
            if (actualFirst >= 0) state.liveFirstSq = actualFirst;
            state.liveFirstPtsNs = getVideoPositionNs(state.player);
            state.liveAnchored = 1;
            logInfo("[yt] LIVE playback anchored at sq=%ld pts=%llu\n",
                    state.liveFirstSq, (unsigned long long)state.liveFirstPtsNs);
         }
         int dx, dy, dw, dh;
         if (cropToFillEnabled) getCropToFillRect(w, h, &dx, &dy, &dw, &dh);
         else                   getGfxLetterboxRect(w, h, &dx, &dy, &dw, &dh);
         if (!state.diagFirstYuvDraw)
            logInfo("[video-diag] first drawGfxYuvFrame begin source=%dx%d target=%dx%d\n",
                    w, h, dw, dh);
         drawGfxYuvFrame(dx, dy, dw, dh, frame, w, h);
         if (!state.diagFirstYuvDraw) {
            state.diagFirstYuvDraw = 1;
            logInfo("[video-diag] first drawGfxYuvFrame complete\n");
         }
      }
      measureFps(frame);
      if (state.showStats && frame) { updateStatLabels(); drawStatsOverlay(); }
      int overlayUp = state.chaptersVisible || state.descriptionVisible || optionsVisible;   // a full-screen overlay hides the HUD noise
      if (state.stage == STAGE_PLAYING && controlsVisible() && !overlayUp) {
         if (!state.isLive || state.liveDvr) drawSeekBar();
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
      if (playbackSponsorOpen) drawPlaybackSponsorSettings();
      if (playbackSubtitleStyleOpen) drawPlaybackSubtitleStyle();
   }

   // until the first frame is presented (open, then the async resume seek's reload) keep the loading
   // status up instead of a black screen; with no player this shows the failure message.
   if (!state.firstFrameSeen || state.audioSwitchThreadActive || state.resolutionSwitchThreadActive) {
      int statusX = state.screenW / 2 - statusLabel.tt.tex.w / 2;
      int statusY = state.screenH / 2 - statusLabel.tt.tex.h / 2;
      if (state.audioSwitchThreadActive)
         drawBadge(&statusLabel, statusX - BADGE_PAD_X, statusY - 4);
      else
         drawLabelAt(&statusLabel, statusX, statusY);
   }

   // Restore the selected library scale before the next screen is drawn.
   setGfxSdUiScale(selectedUiScale);
}

static void termPlay(void)
{
   cropPanX = cropPanY = 0.0f;
   cropPanLastUpdateUs = 0;
   // Wake worker threads blocked in TLS connect/read before joining them. Without this, leaving during
   // a slow YouTube resolve can make the screen manager wait through several 10-second socket timeouts.
   requestPlaybackCancel();
   state.resolutionSwitchAborting = 1;
   if (state.threadActive) { joinThread(state.workerTid); state.threadActive = 0; }
   if (state.sbThreadActive) { joinThread(state.sbWorkerTid); state.sbThreadActive = 0; }   // usually already done
   if (state.subThreadActive) { joinThread(state.subWorkerTid); state.subThreadActive = 0; }
   if (state.audioSwitchThreadActive) { joinThread(state.audioSwitchTid); state.audioSwitchThreadActive = 0; }
   if (state.resolutionSwitchThreadActive) { joinThread(state.resolutionSwitchTid); state.resolutionSwitchThreadActive = 0; }
   if (state.chThreadActive) { joinThread(state.chWorkerTid); state.chThreadActive = 0; }
   if (state.metadataThreadActive) { joinThread(state.metadataWorkerTid); state.metadataThreadActive = 0; }
   if (state.nextThreadActive) { joinThread(state.nextWorkerTid); state.nextThreadActive = 0; }
   free(state.subtitleTrack);
   state.subtitleTrack = NULL;
   free(state.captionTracks);
   state.captionTracks = NULL;
   state.captionCount = 0;
   // Save the resume position on exit. A finished video stores its full duration so the grid can
   // render a 100% progress line; the resume path already treats positions near the end as completed.
   // Only save after a frame played, and keep typed URLs out of the ID-keyed history.
   if (state.player && state.firstFrameSeen && (!state.isLive || state.liveEnded)) {
      char historyId[12];
      if (extractPlaybackId(requestedInput, historyId)) {
         int savedPosition = isVideoEnded(state.player) ? (int)(getVideoDurationSeconds(state.player) + 0.5f)
                                                       : (int)getVideoPositionSeconds(state.player);
         setWatchedDuration(historyId, (int)(getVideoDurationSeconds(state.player) + 0.5f));
         setWatchedPosition(historyId, savedPosition);
      }
   }
   if (state.player || state.pendingPlayer || state.pendingResolutionPlayer || state.retiringResolutionPlayer || state.liveRetiringPlayer)
      finishGfx();   // RSX may still be sampling a frame buffer
   if (state.player) { destroyVideoPlayer(state.player); state.player = NULL; }
   if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
   if (state.pendingResolutionPlayer) { destroyVideoPlayer(state.pendingResolutionPlayer); state.pendingResolutionPlayer = NULL; }
   if (state.retiringResolutionPlayer) { destroyVideoPlayer(state.retiringResolutionPlayer); state.retiringResolutionPlayer = NULL; }
   if (state.liveRetiringPlayer) { destroyVideoPlayer(state.liveRetiringPlayer); state.liveRetiringPlayer = NULL; }
   freeLabel(&statusLabel);
   for (int i = 0; i < STAT_LINES; i++) freeLabel(&statLabels[i]);
   freeLabel(&timeLeftLabel);
   freeLabel(&timeRightLabel);
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
   for (int i = 0; i < 3; i++) freeLabel(&optionsCurrentValueLabels[i]);
   freeLabel(&playbackSettingsTitle);
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
   freeTextTexture(&descriptionTexture);
   if (state.nextThumbJpeg) { free(state.nextThumbJpeg); state.nextThumbJpeg = NULL; }
   if (state.nextImageRequested) cancelImageAsync();
   if (state.nextThumbTexture.offset) { finishGfx(); freeGfxTexture(&state.nextThumbTexture); }
   closeFont(&uiFont);
   setHttpRequestsCancelled(0);
}

Screen playScreen = { initPlay, NULL, updatePlay, drawPlay, NULL, termPlay, SCREEN_TERMINATED };
