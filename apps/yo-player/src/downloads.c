// downloads - background queue that remuxes a video's streams into one .mp4 on disk (see downloads.h).

#include "downloads.h"
#include "stream-select.h"      // pickBestVideo / pickBestAudio
#include "mux-mp4.h"
#include "demux.h"              // VideoDemuxer + readVideoAu / readAudioAu
#include "extractor.h"
#include "video-export.h"   // hand the finished file to the xmb

#include "vfs.h"                // renamePath/deleteFile + MAX_PATH_LEN
#include "thread.h"            // lwmutex + spawnJoinableThread
#include "string-utilities.h"   // strCopy
#include "font.h"
#include "ui/label.h"
#include "theme.h"
#include "gfx.h"
#include "dbg.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_QUEUE          16
#define DISPLAY_TITLE_MAX  15       // maximum visible title characters, including the ellipsis
#define OVERLAY_TEXT_SIZE  19       // matches the grid's Watch Later / duration badge

typedef struct { char videoId[16]; char title[160]; } QueueItem;

static struct {
   sys_lwmutex_t    mutex;         // guards queue[], count, workerSpawned
   QueueItem        queue[MAX_QUEUE];
   int              count;
   int              workerSpawned;
   sys_ppu_thread_t worker;

   volatile int     cancelAll;     // app exit: abort the current download and drain the queue
   volatile int     active;        // a download is currently running (drives the overlay)
   volatile int     percent;
   volatile unsigned int completionSerial; // incremented by the worker after a successful download
   char             activeTitle[160];
   char             activeId[16];  // videoId of the running download (for dedupe; valid while active)

   int              initialized;
} dl;

static Font  uiFont;
static Label overlayLabel;
static unsigned int consumedCompletionSerial;

// ---- filename ----

// the video id, which is always plain characters: the export refuses filenames with spaces or brackets,
// and the name the XMB ends up showing comes from the title handed to the export, not from this one.
static void buildDownloadPath(char *path, int cap, const char *videoId)
{
   snprintf(path, cap, "%s/%s.mp4", getVideoStagingDir(), videoId);
}

// ---- the remux itself ----

// remux the video (and optional audio) url into one .mp4 at outPath. reads elementary access units from
// the demuxers and writes MP4 samples, checking cancelAll each unit. 0 on a complete file, -1 otherwise
// (the partial file is removed by the caller).
static int remux(const char *videoUrl, const char *audioUrl, const char *outPath)
{
   VideoDemuxer *video = (VideoDemuxer *)malloc(sizeof *video);
   VideoDemuxer *audio = audioUrl ? (VideoDemuxer *)malloc(sizeof *audio) : NULL;
   if (!video || (audioUrl && !audio)) { free(video); free(audio); return -1; }

   int result = -1;
   int videoOpen = 0, hasAudio = 0;
   Mp4Muxer mux;

   if (openVideoDemuxer(video, videoUrl) != 0) { logError("[dl] video demux open failed\n"); goto cleanup; }
   videoOpen = 1;
   if (audio && openAudioDemuxer(audio, audioUrl) == 0) hasAudio = 1;

   if (openMp4Muxer(&mux, outPath, &video->h264, video->width, video->height, video->frameDurationNs,
                    hasAudio ? audio->audioRate : 0, hasAudio ? audio->audioChannels : 0) != 0)
      goto cleanup;

   // interleave by presentation time: hold one pending AU from each stream and write whichever comes first.
   VideoAu videoAu;
   AudioAu audioAu;
   int videoRc = readVideoAu(video, &videoAu);
   int audioRc = hasAudio ? readAudioAu(audio, &audioAu) : 0;
   uint64_t total = video->durationNs;
   int failed = 0;

   while (!dl.cancelAll && (videoRc == 1 || audioRc == 1)) {
      if (videoRc == 1 && (audioRc != 1 || videoAu.pts <= audioAu.pts)) {
         if (writeMp4Video(&mux, videoAu.data, videoAu.size, videoAu.pts, videoAu.keyframe)) { failed = 1; break; }
         if (total) { uint64_t done = videoAu.pts * 100 / total; dl.percent = done > 100 ? 100 : (int)done; }
         videoRc = readVideoAu(video, &videoAu);
         if (videoRc < 0) { failed = 1; break; }
      } else {
         if (writeMp4Audio(&mux, audioAu.data, audioAu.size, audioAu.pts)) { failed = 1; break; }
         audioRc = readAudioAu(audio, &audioAu);
         if (audioRc < 0) { failed = 1; break; }
      }
   }

   if (closeMp4Muxer(&mux)) failed = 1;
   if (!failed && !dl.cancelAll) { dl.percent = 100; result = 0; }

cleanup:
   if (videoOpen) closeVideoDemuxer(video);
   if (hasAudio) closeVideoDemuxer(audio);
   free(video);
   free(audio);
   return result;
}

static void runOneDownload(const QueueItem *item)
{
   StreamInfo *info = (StreamInfo *)malloc(sizeof *info);
   const Extractor *extractor = findExtractor(item->videoId);
   if (!info || !extractor || extractor->extract(item->videoId, info) != 0 || info->formatCount == 0) {
      logError("[dl] resolve failed for %s\n", item->videoId);
      free(info);
      return;
   }

   const StreamFormat *video = pickBestVideo(info);
   const StreamFormat *audio = (video && !video->hasAudio) ? pickBestAudio(info) : NULL;
   if (!video) { logError("[dl] no downloadable video for %s\n", item->videoId); free(info); return; }
   if (video->isLiveSegmented) { logInfo("[dl] %s is a live stream, not downloadable\n", item->videoId); free(info); return; }

   char finalPath[MAX_PATH_LEN], partPath[MAX_PATH_LEN];
   buildDownloadPath(finalPath, sizeof finalPath, item->videoId);
   snprintf(partPath, sizeof partPath, "%s.part", finalPath);
   logInfo("[dl] %s -> %s (video itag %d, audio itag %d)\n", item->videoId, finalPath, video->itag, audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   // download to a .part file and only publish it on success, so an interrupted or duplicate download can
   // never truncate or delete an already-finished file.
   int rc = remux(video->url, audio ? audio->url : NULL, partPath);
   free(info);

   if (rc != 0) { deleteFile(partPath); logInfo("[dl] %s %s\n", item->videoId, dl.cancelAll ? "cancelled" : "failed"); }
   else if (renamePath(partPath, finalPath) != 0) { deleteFile(partPath); logError("[dl] %s rename failed\n", item->videoId); }
   else {
      logInfo("[dl] %s done\n", item->videoId);
      queueVideoExport(finalPath, item->title);
      __sync_add_and_fetch(&dl.completionSerial, 1);
   }
}

// ---- worker + queue ----

// one persistent low-priority worker: drains the queue one item at a time, then parks until more arrive or
// the app exits. spawned lazily on the first enqueue and joined once in shutdownDownloads.
static void downloadWorker(uint64_t arg)
{
   (void)arg;
   for (;;) {
      QueueItem item;
      lock(&dl.mutex);
      if (dl.cancelAll) { dl.active = 0; unlock(&dl.mutex); break; }
      if (dl.count == 0) { dl.active = 0; unlock(&dl.mutex); sleepMs(100); continue; }
      item = dl.queue[0];
      for (int i = 1; i < dl.count; i++) dl.queue[i - 1] = dl.queue[i];
      dl.count--;
      dl.active = 1;
      dl.percent = 0;
      strCopy(dl.activeTitle, sizeof dl.activeTitle, item.title);
      strCopy(dl.activeId, sizeof dl.activeId, item.videoId);
      unlock(&dl.mutex);

      runOneDownload(&item);
   }
   exitThread();
}

void enqueueDownload(const SearchResult *item)
{
   if (!dl.initialized || !item->videoId[0]) return;

   int spawn = 0, duplicate = 0, full = 0;
   lock(&dl.mutex);
   if (dl.active && strcmp(dl.activeId, item->videoId) == 0) duplicate = 1;
   for (int i = 0; i < dl.count && !duplicate; i++)
      if (strcmp(dl.queue[i].videoId, item->videoId) == 0) duplicate = 1;

   if (!dl.cancelAll && !duplicate) {
      if (dl.count >= MAX_QUEUE) full = 1;
      else {
         strCopy(dl.queue[dl.count].videoId, sizeof dl.queue[dl.count].videoId, item->videoId);
         strCopy(dl.queue[dl.count].title,   sizeof dl.queue[dl.count].title,   item->title);
         dl.count++;
         if (!dl.workerSpawned) { dl.workerSpawned = 1; spawn = 1; }
      }
   }
   unlock(&dl.mutex);

   if (duplicate) logInfo("[dl] %s already queued or downloading, skipped\n", item->videoId);
   else if (full) logWarn("[dl] queue full, %s skipped\n", item->videoId);

   if (spawn && spawnJoinableThread(&dl.worker, downloadWorker, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-dl") != 0) {
      lock(&dl.mutex); dl.workerSpawned = 0; unlock(&dl.mutex);
      logError("[dl] worker spawn failed\n");
   }
}

// ---- overlay ----

// Clip by UTF-8 code points, reserving three of the 15 visible characters for "...".
static int clipDisplayTitle(char *out, int cap, const char *title)
{
   const unsigned char *p = (const unsigned char *)title;
   int chars = 0;
   while (*p && chars < DISPLAY_TITLE_MAX) {
      unsigned char c = *p;
      p += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
      chars++;
   }
   if (!*p) { strCopy(out, cap, title); return 0; }

   p = (const unsigned char *)title;
   for (int i = 0; i < DISPLAY_TITLE_MAX - 3 && *p; i++) {
      unsigned char c = *p;
      p += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
   }
   int bytes = (int)(p - (const unsigned char *)title);
   if (bytes > cap - 4) bytes = cap - 4;
   if (bytes < 0) bytes = 0;
   memcpy(out, title, bytes);
   out[bytes] = 0;
   return 1;
}

// "Downloading "<title>" NN%" with the title limited to 15 visible characters;
// "(+N)" is appended when more downloads are queued.
static void formatOverlay(char *out, int cap)
{
   char clipped[sizeof dl.activeTitle];
   int truncated = clipDisplayTitle(clipped, sizeof clipped, dl.activeTitle);

   int queued = dl.count;
   if (queued > 0) snprintf(out, cap, "Downloading \"%s%s\" %d%% (+%d)", clipped, truncated ? "..." : "", dl.percent, queued);
   else            snprintf(out, cap, "Downloading \"%s%s\" %d%%", clipped, truncated ? "..." : "", dl.percent);
}

int consumeDownloadCompletion(void)
{
   if (!dl.initialized) return 0;
   unsigned int serial = dl.completionSerial;
   if (serial == consumedCompletionSerial) return 0;
   consumedCompletionSerial = serial;
   return 1;
}

void updateDownloadOverlay(void)
{
   if (!dl.initialized) return;
   /* Completions while browsing should not produce a stale toast on a later playback. */
   consumedCompletionSerial = dl.completionSerial;
   if (!dl.active) return;
   char text[128];
   formatOverlay(text, sizeof text);
   setLabelText(&overlayLabel, text);   // only rasterises on a real change
}

void drawDownloadOverlay(int screenWidth)
{
   if (!dl.initialized || !dl.active || overlayLabel.tt.tex.w == 0) return;
   int badgeW = overlayLabel.tt.tex.w + 12, badgeH = OVERLAY_TEXT_SIZE + 8;
   int x = screenWidth - badgeW - 6 - OVERLAY_TEXT_SIZE, y = 50; // extra right offset of approximately two characters
   fillGfxRectangle(x, y, badgeW, badgeH, activeTheme->badgeFill);
   drawLabelAt(&overlayLabel, x + 6, y + 4);
}

// ---- lifecycle ----

void initDownloads(void)
{
   memset(&dl, 0, sizeof dl);
   createLock(&dl.mutex);
   uiFont = openSystemFont(FONT_POP);
   initLabel(&overlayLabel, &uiFont, 0, 0, AUTO, AUTO, OVERLAY_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "");
   /* Rasterize at the selected UI scale without scaling the screen-space anchor. */
   setLabelScaleWithUi(&overlayLabel, 1);
   consumedCompletionSerial = 0;
   dl.initialized = 1;
}

void shutdownDownloads(void)
{
   if (!dl.initialized) return;
   dl.cancelAll = 1;
   if (dl.workerSpawned) joinThread(dl.worker);
   dl.count = 0;
   finishGfx();   // the overlay label may have been sampled last frame
   freeLabel(&overlayLabel);
   closeFont(&uiFont);
   destroyLock(&dl.mutex);
   dl.initialized = 0;
}
