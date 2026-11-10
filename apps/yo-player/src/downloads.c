// downloads - background queue that remuxes a video's streams into one .mp4 on disk (see downloads.h).

#include "app-config.h"
#include "downloads.h"
#include "stream-select.h"      // pickBestVideo / pickBestAudio
#include "mux-mp4.h"
#include "demux.h"              // VideoDemuxer + readVideoAu / readAudioAu
#include "extractor.h"
#include "settings.h"
#include "subtitles.h"
#include "video-export.h"   // hand the finished file to the xmb

#include "vfs.h"                // renamePath/deleteFile + MAX_PATH_LEN
#include "thread.h"            // lwmutex + spawnJoinableThread
#include "string-utilities.h"   // strCopy
#include "font.h"
#include "ui/label.h"
#include "theme.h"
#include "gfx.h"
#include "dbg.h"
#include "http.h"
#include "path.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_QUEUE          16
#define DISPLAY_TITLE_MAX  15       // maximum visible title characters, including the ellipsis
#define OVERLAY_TEXT_SIZE  19
#define DOWNLOAD_TOAST_US  1000000ULL
#define DOWNLOAD_LIBRARY_DIR CRTUBE_DATA_DIR "/downloads"
#define DOWNLOAD_THUMB_CAP (96 * 1024)       // matches the grid's Watch Later / duration badge

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
static Label downloadToastLabel;
static uint64_t downloadToastShownAt;
static unsigned int consumedCompletionSerial;

// ---- filename ----

// the video id, which is always plain characters: the export refuses filenames with spaces or brackets,
// and the name the XMB ends up showing comes from the title handed to the export, not from this one.
static void buildDownloadPath(char *path, int cap, const char *videoId)
{
   snprintf(path, cap, "%s/%s.mp4", DOWNLOAD_LIBRARY_DIR, videoId);
}

static void buildSidecarPath(char *path, int cap, const char *videoPath, const char *extension)
{
   strCopy(path, cap, videoPath);
   char *dot = strrchr(path, '.');
   if (dot) *dot = 0;
   strncat(path, extension, (size_t)(cap - strlen(path) - 1));
}

static void buildSubtitlePath(char *path, int cap, const char *videoId, const char *languageCode)
{
   snprintf(path, cap, "%s/%s.%s.srt", DOWNLOAD_LIBRARY_DIR, videoId, languageCode);
}

static void buildSubtitleManifestPath(char *path, int cap, const char *videoId)
{
   snprintf(path, cap, "%s/%s.subs", DOWNLOAD_LIBRARY_DIR, videoId);
}

static void clearDownloadSubtitleSidecars(const char *videoId)
{
   char manifestPath[MAX_PATH_LEN], manifest[128], subtitlePath[MAX_PATH_LEN];
   buildSubtitleManifestPath(manifestPath, sizeof manifestPath, videoId);
   int length = readFile(manifestPath, manifest, sizeof manifest - 1);
   if (length > 0) {
      manifest[length] = 0;
      char *line = manifest;
      while (*line) {
         char *end = strpbrk(line, "\r\n");
         if (end) *end = 0;
         if (line[0]) {
            snprintf(subtitlePath, sizeof subtitlePath, "%s/%s.%s.srt", DOWNLOAD_LIBRARY_DIR, videoId, line);
            deleteFile(subtitlePath);
         }
         if (!end) break;
         line = end + 1;
         while (*line == '\r' || *line == '\n') line++;
      }
   }
   deleteFile(manifestPath);
}

static int languageBaseMatches(const char *a, const char *b)
{
   if (!a || !b || !a[0] || !b[0]) return 0;
   int i = 0;
   while (a[i] && b[i] && a[i] != '-' && b[i] != '-') {
      char ca = a[i], cb = b[i];
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb) return 0;
      i++;
   }
   return i > 0 && (a[i] == 0 || a[i] == '-' || b[i] == 0 || b[i] == '-');
}

static void audioLanguageFromTrackId(const char *trackId, char *out, int cap)
{
   if (!out || cap <= 0) return;
   out[0] = 0;
   if (!trackId || !trackId[0]) return;
   int n = 0;
   while (trackId[n] && trackId[n] != '.' && n < cap - 1) { out[n] = trackId[n]; n++; }
   out[n] = 0;
   if (strcmp(out, "und") == 0 || strcmp(out, "unknown") == 0 || strcmp(out, "original") == 0)
      out[0] = 0;
}

static const CaptionTrack *findDownloadCaption(const StreamInfo *info, const char *languageCode)
{
   if (!info || !languageCode || !languageCode[0]) return NULL;
   const CaptionTrack *best = NULL;
   int bestRank = 99;
   for (int i = 0; i < info->captionCount; i++) {
      const CaptionTrack *track = &info->captions[i];
      if (!languageBaseMatches(track->languageCode, languageCode)) continue;
      int rank = track->isAutoTranslated ? 2 : track->isAutoGenerated ? 1 : 0;
      if (rank < bestRank) { best = track; bestRank = rank; }
   }
   return best;
}

static const CaptionTrack *findOriginalUploadCaption(const StreamInfo *info)
{
   if (!info) return NULL;
   const CaptionTrack *best = NULL;
   int bestRank = 99;
   for (int i = 0; i < info->captionCount; i++) {
      const CaptionTrack *track = &info->captions[i];
      if (!track->languageCode[0] || track->isAutoTranslated) continue;
      /* ASR is tied to the spoken source language; creator-uploaded captions
         can also be translated tracks when the audio track ID is absent. */
      int rank = track->isAutoGenerated ? 0 : 1;
      if (rank < bestRank) { best = track; bestRank = rank; }
   }
   return best;
}

static int saveDownloadSubtitles(const char *videoId, const StreamInfo *info,
                                 const char *primaryLanguage, const char *secondaryLanguage)
{
   char manifestPath[MAX_PATH_LEN], subtitlePath[MAX_PATH_LEN];
   char manifest[64] = "";
   char resolvedPrimaryLanguage[12] = "";
   int used = 0, saved = 0;
   clearDownloadSubtitleSidecars(videoId);
   if (getDownloadPrimarySubtitlesEnabled()) {
      const CaptionTrack *track = (primaryLanguage && primaryLanguage[0]) ?
         findDownloadCaption(info, primaryLanguage) : findOriginalUploadCaption(info);
      /* A muxed one-track MP4 may not expose audioTrackId. In that case use the
         creator's source caption (or its ASR track) as the original-language match. */
      if (!track && (!primaryLanguage || !primaryLanguage[0]))
         track = findOriginalUploadCaption(info);
      if (track) {
         buildSubtitlePath(subtitlePath, sizeof subtitlePath, videoId, track->languageCode);
         if (saveSubtitlesSrt(track->url, subtitlePath) == 0) {
            used += snprintf(manifest + used, sizeof manifest - (size_t)used, "%s\n", track->languageCode);
            strCopy(resolvedPrimaryLanguage, sizeof resolvedPrimaryLanguage, track->languageCode);
            saved++;
         } else logWarn("[dl] primary subtitles unavailable for %s (%s)\n", videoId, track->languageCode);
      }
   }
   if (secondaryLanguage && secondaryLanguage[0] && strcmp(secondaryLanguage, "off") != 0 &&
       (!saved || !languageBaseMatches(resolvedPrimaryLanguage, secondaryLanguage))) {
      const CaptionTrack *track = findDownloadCaption(info, secondaryLanguage);
      if (track) {
         int duplicate = saved && languageBaseMatches(resolvedPrimaryLanguage, track->languageCode);
         if (!duplicate) {
            buildSubtitlePath(subtitlePath, sizeof subtitlePath, videoId, track->languageCode);
            if (saveSubtitlesSrt(track->url, subtitlePath) == 0) {
               if (used < (int)sizeof manifest - 1)
                  used += snprintf(manifest + used, sizeof manifest - (size_t)used, "%s\n", track->languageCode);
               saved++;
            } else logWarn("[dl] secondary subtitles unavailable for %s (%s)\n", videoId, track->languageCode);
         }
      }
   }
   buildSubtitleManifestPath(manifestPath, sizeof manifestPath, videoId);
   if (saved && writeFile(manifestPath, manifest, (uint64_t)used) != 0)
      logWarn("[dl] subtitle manifest write failed for %s\n", videoId);
   else if (!saved) deleteFile(manifestPath);
   return saved;
}

static int validDownloadId(const char *id)
{
   if (!id || strlen(id) != 11) return 0;
   for (int i = 0; i < 11; i++) {
      char c = id[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
   }
   return 1;
}

static int compareDownloadText(const char *a, const char *b)
{
   while (*a && *b) {
      unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
      if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
      if (ca != cb) return ca < cb ? -1 : 1;
   }
   return (*a != 0) - (*b != 0);
}

static void saveDownloadMetadata(const char *videoPath, const StreamInfo *info, const char *fallbackTitle,
                                 const char *publishedDate, const char *viewCount, const char *likeCount)
{
   char path[MAX_PATH_LEN], data[3000];
   buildSidecarPath(path, sizeof path, videoPath, ".meta");
   const char *title = info->title[0] ? info->title : fallbackTitle;
   int length = snprintf(data, sizeof data, "%s\n%s\n#CRTMETA2\n%s\n%s\n%s\n%d\n%s",
                         title, info->author, publishedDate ? publishedDate : "",
                         viewCount ? viewCount : "", likeCount ? likeCount : "Offline",
                         info->durationSeconds > 0 ? info->durationSeconds : 0, info->description);
   if (length > 0 && length < (int)sizeof data) {
      if (writeFile(path, data, (uint64_t)length) != 0)
         logWarn("[dl] metadata write failed for %s\n", videoPath);
   }
}

static void saveDownloadThumbnail(const char *videoPath, const char *videoId)
{
   char path[MAX_PATH_LEN], url[128];
   uint8_t *jpeg = (uint8_t *)malloc(DOWNLOAD_THUMB_CAP);
   if (!jpeg) return;
   buildSidecarPath(path, sizeof path, videoPath, ".jpg");
   snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", videoId);
   HttpHeader headers[] = {
      { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/125.0.0.0 Safari/537.36" },
      { "Accept-Encoding", "identity" }
   };
   int length = 0, status = 0;
   int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)jpeg, DOWNLOAD_THUMB_CAP, &length, &status);
   if (rc == 0 && status == 200 && length > 0) {
      if (writeFile(path, (const char *)jpeg, (uint64_t)length) != 0)
         logWarn("[dl] thumbnail write failed for %s\n", videoId);
   } else {
      logWarn("[dl] thumbnail fetch failed for %s rc=%d status=%d\n", videoId, rc, status);
   }
   free(jpeg);
}

int loadDownloadedMetadata(const char *videoPath, char *title, int titleCap,
                           char *author, int authorCap, char *description, int descriptionCap,
                           char *publishedDate, int publishedCap,
                           char *viewCount, int viewsCap, char *likeCount, int likesCap)
{
   char path[MAX_PATH_LEN], data[3000];
   buildSidecarPath(path, sizeof path, videoPath, ".meta");
   if (publishedDate && publishedCap > 0) publishedDate[0] = 0;
   if (viewCount && viewsCap > 0) viewCount[0] = 0;
   if (likeCount && likesCap > 0) likeCount[0] = 0;
   int length = readFile(path, data, sizeof data - 1);
   if (length <= 0) return -1;
   data[length] = 0;
   char *first = strchr(data, '\n');
   if (!first) return -1;
   *first++ = 0;
   char *second = strchr(first, '\n');
   if (!second) return -1;
   *second++ = 0;
   if (data[0] && title && titleCap > 0) strCopy(title, titleCap, data);
   if (first[0] && author && authorCap > 0) strCopy(author, authorCap, first);
   if (strncmp(second, "#CRTMETA2\n", 10) == 0) {
      char *line = second + 10;
      char *publishedEnd = strchr(line, '\n');
      if (!publishedEnd) return -1;
      *publishedEnd++ = 0;
      char *viewsEnd = strchr(publishedEnd, '\n');
      if (!viewsEnd) return -1;
      *viewsEnd++ = 0;
      char *likesEnd = strchr(viewsEnd, '\n');
      if (!likesEnd) return -1;
      *likesEnd++ = 0;
      char *durationEnd = strchr(likesEnd, '\n');
      if (!durationEnd) return -1;
      *durationEnd++ = 0;
      if (publishedDate && publishedCap > 0 && line[0]) strCopy(publishedDate, publishedCap, line);
      if (viewCount && viewsCap > 0 && publishedEnd[0]) strCopy(viewCount, viewsCap, publishedEnd);
      if (likeCount && likesCap > 0 && viewsEnd[0]) strCopy(likeCount, likesCap, viewsEnd);
      if (description && descriptionCap > 0) strCopy(description, descriptionCap, durationEnd);
   } else if (strncmp(second, "#CRTMETA1\n", 10) == 0) {
      char *line = second + 10;
      char *publishedEnd = strchr(line, '\n');
      if (!publishedEnd) return -1;
      *publishedEnd++ = 0;
      char *viewsEnd = strchr(publishedEnd, '\n');
      if (!viewsEnd) return -1;
      *viewsEnd++ = 0;
      char *likesEnd = strchr(viewsEnd, '\n');
      if (!likesEnd) return -1;
      *likesEnd++ = 0;
      if (publishedDate && publishedCap > 0 && line[0]) strCopy(publishedDate, publishedCap, line);
      if (viewCount && viewsCap > 0 && publishedEnd[0]) strCopy(viewCount, viewsCap, publishedEnd);
      if (likeCount && likesCap > 0 && viewsEnd[0]) strCopy(likeCount, likesCap, viewsEnd);
      if (description && descriptionCap > 0) strCopy(description, descriptionCap, likesEnd);
   } else if (description && descriptionCap > 0) {
      strCopy(description, descriptionCap, second);
   }
   return 0;
}

static uint32_t downloadReadBe32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
          ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t downloadReadBe64(const uint8_t *p)
{
   return ((uint64_t)downloadReadBe32(p) << 32) | downloadReadBe32(p + 4);
}

// Read only the MP4 movie header. This also supplies duration for files downloaded
// before duration was added to the metadata sidecar.
static int readDownloadedMp4Duration(const char *videoPath)
{
   VideoSource source;
   if (openVideoSource(&source, videoPath) != 0) return 0;

   uint64_t fileSize = getVideoSourceSize(&source);
   uint64_t moovStart = 0, moovEnd = 0, pos = 0;
   for (int guard = 0; guard < 256 && pos + 8 <= fileSize; guard++) {
      uint8_t box[16];
      if (readVideoSourceAt(&source, pos, box, 8) != 0) break;
      uint64_t boxSize = downloadReadBe32(box);
      uint32_t boxType = downloadReadBe32(box + 4);
      uint64_t headerSize = 8;
      if (boxSize == 1) {
         if (readVideoSourceAt(&source, pos + 8, box + 8, 8) != 0) break;
         boxSize = downloadReadBe64(box + 8);
         headerSize = 16;
      } else if (boxSize == 0) {
         boxSize = fileSize - pos;
      }
      if (boxSize < headerSize || pos + boxSize > fileSize) break;
      if (boxType == 0x6D6F6F76) {
         moovStart = pos + headerSize;
         moovEnd = pos + boxSize;
         break;
      }
      pos += boxSize;
   }

   int durationSeconds = 0;
   if (moovStart) {
      pos = moovStart;
      for (int guard = 0; guard < 64 && pos + 8 <= moovEnd; guard++) {
         uint8_t box[16];
         if (readVideoSourceAt(&source, pos, box, 8) != 0) break;
         uint64_t boxSize = downloadReadBe32(box);
         uint32_t boxType = downloadReadBe32(box + 4);
         uint64_t headerSize = 8;
         if (boxSize == 1) {
            if (readVideoSourceAt(&source, pos + 8, box + 8, 8) != 0) break;
            boxSize = downloadReadBe64(box + 8);
            headerSize = 16;
         } else if (boxSize == 0) {
            boxSize = moovEnd - pos;
         }
         if (boxSize < headerSize || pos + boxSize > moovEnd) break;
         if (boxType == 0x6D766864) {
            uint8_t mvhd[32];
            if (readVideoSourceAt(&source, pos + headerSize, mvhd, sizeof mvhd) == 0) {
               uint32_t timescale = mvhd[0] == 1 ? downloadReadBe32(mvhd + 20)
                                                  : downloadReadBe32(mvhd + 12);
               uint64_t ticks = mvhd[0] == 1 ? downloadReadBe64(mvhd + 24)
                                              : downloadReadBe32(mvhd + 16);
               if (timescale) durationSeconds = (int)(ticks / timescale);
            }
            break;
         }
         pos += boxSize;
      }
   }

   closeVideoSource(&source);
   return durationSeconds > 0 ? durationSeconds : 0;
}

static int loadDownloadedDuration(const char *videoPath)
{
   char path[MAX_PATH_LEN], data[3000];
   buildSidecarPath(path, sizeof path, videoPath, ".dur");
   int length = readFile(path, data, sizeof data - 1);
   if (length > 0) {
      data[length] = 0;
      int cached = atoi(data);
      if (cached > 0) return cached;
   }

   buildSidecarPath(path, sizeof path, videoPath, ".meta");
   length = readFile(path, data, sizeof data - 1);
   if (length > 0) {
      data[length] = 0;
      char *first = strchr(data, '\n');
      char *second = first ? strchr(first + 1, '\n') : NULL;
      if (second && strncmp(second + 1, "#CRTMETA2\n", 10) == 0) {
         char *line = second + 11;
         for (int i = 0; i < 3; i++) {
            line = strchr(line, '\n');
            if (!line) break;
            line++;
         }
         if (line && *line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            int duration = atoi(line);
            if (duration > 0) return duration;
         }
      }
   }

   int duration = readDownloadedMp4Duration(videoPath);
   if (duration > 0) {
      char value[16];
      snprintf(value, sizeof value, "%d", duration);
      buildSidecarPath(path, sizeof path, videoPath, ".dur");
      writeFile(path, value, (uint64_t)strlen(value));
   }
   return duration;
}

unsigned int getDownloadLibraryRevision(void)
{
   return dl.completionSerial;
}

int getDownloadedVideos(SearchResults *out, int sortAZ)
{
   memset(out, 0, sizeof *out);
   makeDir(DOWNLOAD_LIBRARY_DIR);
   VfsDir dir;
   if (openDir(DOWNLOAD_LIBRARY_DIR, &dir) != 0) return -1;
   char name[256];
   VfsEntryType type;
   while (out->count < MAX_SEARCH_RESULTS && readDir(&dir, name, sizeof name, &type) > 0) {
      if (type != VFS_ENTRY_FILE || strlen(name) != 15 || strcmp(name + 11, ".mp4") != 0) continue;
      name[11] = 0;
      if (!validDownloadId(name)) continue;
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof path, "%s/%s.mp4", DOWNLOAD_LIBRARY_DIR, name);
      VfsStat stat;
      if (statPath(path, &stat) != 0 || stat.isDir) continue;
      SearchResult *item = &out->items[out->count++];
      memset(item, 0, sizeof *item);
      strCopy(item->videoId, sizeof item->videoId, name);
      strCopy(item->title, sizeof item->title, name);
      strCopy(item->author, sizeof item->author, "Downloaded");
      int duration = loadDownloadedDuration(path);
      if (duration > 0) {
         int hours = duration / 3600;
         int minutes = (duration / 60) % 60;
         int seconds = duration % 60;
         if (hours > 0)
            snprintf(item->duration, sizeof item->duration, "%d:%02d:%02d", hours, minutes, seconds);
         else
            snprintf(item->duration, sizeof item->duration, "%d:%02d", minutes, seconds);
      }
      item->isLocalDownload = 1;
      item->downloadedAt = stat.mtime;
      char description[2048], published[24], views[32], likes[32];
      loadDownloadedMetadata(path, item->title, sizeof item->title,
                             item->author, sizeof item->author,
                             description, sizeof description,
                             published, sizeof published, views, sizeof views, likes, sizeof likes);
   }
   closeDir(&dir);
   for (int i = 1; i < out->count; i++) {
      SearchResult item = out->items[i];
      int j = i;
      while (j > 0) {
         int before = sortAZ ? compareDownloadText(item.title, out->items[j - 1].title) < 0
                             : item.downloadedAt > out->items[j - 1].downloadedAt;
         if (!before) break;
         out->items[j] = out->items[j - 1];
         j--;
      }
      out->items[j] = item;
   }
   return out->count;
}

int getDownloadedVideoPath(const char *videoId, char *path, int cap)
{
   if (!validDownloadId(videoId) || !path || cap <= 0) return -1;
   snprintf(path, cap, "%s/%s.mp4", DOWNLOAD_LIBRARY_DIR, videoId);
   return fileExists(path) ? 0 : -1;
}

int getDownloadedSubtitleTracks(const char *videoPath, CaptionTrack *out, int capacity)
{
   if (!videoPath || !out || capacity <= 0) return 0;
   const char *base = strrchr(videoPath, '/');
   base = base ? base + 1 : videoPath;
   char videoId[16];
   if (strlen(base) != 15 || strcmp(base + 11, ".mp4") != 0) return 0;
   memcpy(videoId, base, 11); videoId[11] = 0;
   if (!validDownloadId(videoId)) return 0;
   char manifestPath[MAX_PATH_LEN], manifest[128];
   buildSubtitleManifestPath(manifestPath, sizeof manifestPath, videoId);
   int length = readFile(manifestPath, manifest, sizeof manifest - 1);
   if (length <= 0) return 0;
   manifest[length] = 0;
   int count = 0;
   char *line = manifest;
   while (*line && count < capacity) {
      char *end = strpbrk(line, "\r\n");
      if (end) *end = 0;
      if (line[0]) {
         CaptionTrack *track = &out[count];
         memset(track, 0, sizeof *track);
         strCopy(track->languageCode, sizeof track->languageCode, line);
         strCopy(track->name, sizeof track->name, line);
         snprintf(track->url, sizeof track->url, "%s/%s.%s.srt", DOWNLOAD_LIBRARY_DIR, videoId, line);
         track->isLocalFile = 1;
         if (fileExists(track->url)) count++;
      }
      if (!end) break;
      line = end + 1;
      while (*line == '\r' || *line == '\n') line++;
   }
   return count;
}

int deleteDownloadedVideo(const char *videoId)
{
   if (!validDownloadId(videoId)) return -1;
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof path, "%s/%s.mp4", DOWNLOAD_LIBRARY_DIR, videoId);
   if (deleteFile(path) != 0) return -1;
   snprintf(path, sizeof path, "%s/%s.meta", DOWNLOAD_LIBRARY_DIR, videoId);
   deleteFile(path);
   snprintf(path, sizeof path, "%s/%s.jpg", DOWNLOAD_LIBRARY_DIR, videoId);
   deleteFile(path);
   clearDownloadSubtitleSidecars(videoId);
   return 0;
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
   int videoOpen = 0, separateAudioOpen = 0, muxedAudio = 0;
   Mp4Muxer mux;

   if (openVideoDemuxer(video, videoUrl) != 0) { logError("[dl] video demux open failed\n"); goto cleanup; }
   videoOpen = 1;
   if (audio && openAudioDemuxer(audio, audioUrl) == 0) separateAudioOpen = 1;
   else if (video->hasAudio && video->audioRate > 0 && video->audioChannels > 0) muxedAudio = 1;

   const uint8_t *audioConfig = separateAudioOpen ? audio->audioConfig
                               : (muxedAudio ? video->audioConfig : NULL);
   int audioConfigSize = separateAudioOpen ? audio->audioConfigSize
                        : (muxedAudio ? video->audioConfigSize : 0);

   if (openMp4Muxer(&mux, outPath, &video->h264, video->width, video->height, video->frameDurationNs,
                    separateAudioOpen ? audio->audioRate : (muxedAudio ? video->audioRate : 0),
                    separateAudioOpen ? audio->audioChannels : (muxedAudio ? video->audioChannels : 0),
                    audioConfig, audioConfigSize) != 0)
      goto cleanup;

   logInfo("[dl] remux audio source=%s\\n",
           separateAudioOpen ? "separate" : (muxedAudio ? "muxed" : "none"));

   // Muxed MP4 sources queue AAC while readVideoAu advances their video timeline.
   // Separate DASH audio sources are read directly from their own demuxer.
   VideoAu videoAu;
   AudioAu audioAu;
   int videoRc = readVideoAu(video, &videoAu);
   int audioRc = separateAudioOpen ? readAudioAu(audio, &audioAu)
                 : (muxedAudio ? takeAudioAu(video, &audioAu) : 0);
   uint64_t total = video->durationNs;
   int failed = videoRc != 1 || audioRc < 0;
   unsigned int videoSamplesWritten = 0;
   uint64_t lastVideoPts = 0;

   while (!failed && !dl.cancelAll && (videoRc == 1 || audioRc == 1)) {
      if (videoRc == 1 && (audioRc != 1 || videoAu.pts <= audioAu.pts)) {
         if (writeMp4Video(&mux, videoAu.data, videoAu.size, videoAu.pts, videoAu.keyframe)) { failed = 1; break; }
         videoSamplesWritten++;
         lastVideoPts = videoAu.pts;
         if (total) { uint64_t done = videoAu.pts * 100 / total; dl.percent = done > 100 ? 100 : (int)done; }
         videoRc = readVideoAu(video, &videoAu);
         if (videoRc < 0) { failed = 1; break; }
         if (muxedAudio) audioRc = takeAudioAu(video, &audioAu);
      } else {
         if (writeMp4Audio(&mux, audioAu.data, audioAu.size, audioAu.pts)) { failed = 1; break; }
         audioRc = separateAudioOpen ? readAudioAu(audio, &audioAu)
                   : (muxedAudio ? takeAudioAu(video, &audioAu) : 0);
         if (audioRc < 0) { failed = 1; break; }
      }
   }

   if (closeMp4Muxer(&mux)) failed = 1;
   if (videoSamplesWritten == 0) failed = 1;
   if (total && videoSamplesWritten > 0) {
      uint64_t tolerance = video->frameDurationNs ? video->frameDurationNs * 3 : 500000000ULL;
      if (lastVideoPts + tolerance < total) {
         logError("[dl] video stream ended early: wrote %u samples, last PTS=%llu duration=%llu\\n",
                  videoSamplesWritten, (unsigned long long)lastVideoPts, (unsigned long long)total);
         failed = 1;
      }
   }
   if (!failed && !dl.cancelAll) { dl.percent = 100; result = 0; }

cleanup:
   if (videoOpen) closeVideoDemuxer(video);
   if (separateAudioOpen) closeVideoDemuxer(audio);
   free(video);
   free(audio);
   return result;
}

static void runOneDownload(const QueueItem *item)
{
   StreamInfo *info = (StreamInfo *)calloc(1, sizeof *info);
   const Extractor *extractor = findExtractor(item->videoId);
   int extractResult = (info && extractor) ? extractor->extract(item->videoId, info) : -1;
   if (!info || !extractor || extractResult != 0 || info->formatCount == 0) {
      logError("[dl] resolve failed for %s\n", item->videoId);
      if (info) { free(info->captions); free(info); }
      return;
   }
   if (info->isLive) {
      logInfo("[dl] %s is currently LIVE, not downloadable\n", item->videoId);
      free(info->captions);
      free(info);
      return;
   }

   int maxHeight = 0;
   switch (getDownloadResolution()) {
      case DOWNLOAD_RESOLUTION_480P: maxHeight = 480; break;
      case DOWNLOAD_RESOLUTION_720P: maxHeight = 720; break;
      case DOWNLOAD_RESOLUTION_1080P: maxHeight = 1080; break;
      default: break;
   }
   const char *requestedAudioLanguage = getDownloadAudioLanguage();
   const StreamFormat *video = pickDownloadVideo(info, maxHeight,
      requestedAudioLanguage && strcmp(requestedAudioLanguage, "original") != 0);
   if (!video) { logError("[dl] no downloadable video for %s\n", item->videoId); free(info->captions); free(info); return; }
   if (video->isLiveSegmented) { logInfo("[dl] %s is a live stream, not downloadable\n", item->videoId); free(info->captions); free(info); return; }

   int allowAutoDubbed = getDownloadAutoDubEnabled();
   const StreamFormat *audio = NULL;
   char actualAudioLanguage[16] = "";
   if (!video->hasAudio) {
      audio = pickDownloadAudio(info, requestedAudioLanguage, allowAutoDubbed);
      if (!audio && requestedAudioLanguage && strcmp(requestedAudioLanguage, "original") != 0) {
         logInfo("[dl] requested audio %s unavailable for %s; falling back to original upload\n",
                 requestedAudioLanguage, item->videoId);
         audio = pickDownloadAudio(info, "original", 0);
      }
   }
   if (video->hasAudio && requestedAudioLanguage && strcmp(requestedAudioLanguage, "original") != 0)
      logInfo("[dl] requested audio %s unavailable as a separate stream for %s; using original muxed audio\n",
              requestedAudioLanguage, item->videoId);
   if (!video->hasAudio && !audio) {
      const StreamFormat *muxedFallback = pickDownloadMuxedVideo(info, maxHeight);
      if (muxedFallback) {
         logInfo("[dl] no separate original audio stream for %s; using muxed original upload\n", item->videoId);
         video = muxedFallback;
      }
   }
   if (audio) audioLanguageFromTrackId(audio->audioTrackId, actualAudioLanguage, sizeof actualAudioLanguage);
   if (!actualAudioLanguage[0]) {
      audioLanguageFromTrackId(video->audioTrackId, actualAudioLanguage, sizeof actualAudioLanguage);
      if (!actualAudioLanguage[0]) audioLanguageFromTrackId(info->defaultAudioTrackId, actualAudioLanguage, sizeof actualAudioLanguage);
   }

   char finalPath[MAX_PATH_LEN], partPath[MAX_PATH_LEN];
   buildDownloadPath(finalPath, sizeof finalPath, item->videoId);
   snprintf(partPath, sizeof partPath, "%s.part", finalPath);
   logInfo("[dl] %s -> %s (video itag %d, audio itag %d, audio=%s)\n",
           item->videoId, finalPath, video->itag,
           audio ? audio->itag : (video->hasAudio ? video->itag : 0),
           actualAudioLanguage[0] ? actualAudioLanguage : "unknown");

   int rc = remux(video->url, audio ? audio->url : NULL, partPath);
   if (rc != 0) {
      free(info->captions); free(info); deleteFile(partPath);
      logInfo("[dl] %s %s\n", item->videoId, dl.cancelAll ? "cancelled" : "failed");
   } else if (renamePath(partPath, finalPath) != 0) {
      free(info->captions); free(info); deleteFile(partPath);
      logError("[dl] %s rename failed\n", item->videoId);
   } else {
      char metadataViews[32], metadataPublished[24], metadataLikes[32];
      strCopy(metadataViews, sizeof metadataViews, info->viewCount);
      strCopy(metadataPublished, sizeof metadataPublished, info->publishedDate);
      strCopy(metadataLikes, sizeof metadataLikes, "Offline");
      if (extractor->getVideoMetadata) {
         char fetchedViews[32] = "", fetchedPublished[24] = "", fetchedLikes[32] = "";
         if (extractor->getVideoMetadata(item->videoId, fetchedViews, sizeof fetchedViews,
                                         fetchedPublished, sizeof fetchedPublished,
                                         fetchedLikes, sizeof fetchedLikes) == 0) {
            if (fetchedViews[0]) strCopy(metadataViews, sizeof metadataViews, fetchedViews);
            if (fetchedPublished[0]) strCopy(metadataPublished, sizeof metadataPublished, fetchedPublished);
            if (fetchedLikes[0]) strCopy(metadataLikes, sizeof metadataLikes, fetchedLikes);
         }
      }
      if (!info->description[0])
         logWarn("[dl] description unavailable after extraction for %s; metadata will not contain a description\n", item->videoId);
      else
         logInfo("[dl] description captured for %s (%d bytes)\n", item->videoId, (int)strlen(info->description));
      saveDownloadMetadata(finalPath, info, item->title, metadataPublished, metadataViews, metadataLikes);
      saveDownloadThumbnail(finalPath, item->videoId);
      const char *primaryLanguage = getDownloadPrimarySubtitlesEnabled() ? actualAudioLanguage : "";
      saveDownloadSubtitles(item->videoId, info, primaryLanguage,
                            getDownloadSecondarySubtitleLanguage());
      free(info->captions); free(info);
      logInfo("[dl] %s done\n", item->videoId);
      if (strcmp(DOWNLOAD_LIBRARY_DIR, getVideoStagingDir()) != 0) {
         char exportPath[MAX_PATH_LEN];
         uint8_t *copyBuffer = (uint8_t *)malloc(64 * 1024);
         snprintf(exportPath, sizeof exportPath, "%s/%s.mp4", getVideoStagingDir(), item->videoId);
         if (copyBuffer && copyFile(finalPath, exportPath, copyBuffer, 64 * 1024) == 0)
            queueVideoExport(exportPath, item->title);
         else
            logWarn("[dl] retained in library; xmb export copy failed for %s\n", item->videoId);
         free(copyBuffer);
      } else {
         queueVideoExport(finalPath, item->title);
      }
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
   else if (!dl.cancelAll) {
      downloadToastShownAt = getTimeUs();
      setLabelText(&downloadToastLabel, "Download started");
   }

   if (spawn && spawnJoinableThread(&dl.worker, downloadWorker, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-dl") != 0) {
      lock(&dl.mutex); dl.workerSpawned = 0; unlock(&dl.mutex);
      downloadToastShownAt = 0;
      setLabelText(&downloadToastLabel, "");
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
   if (!dl.initialized) return;
   if (dl.active && overlayLabel.tt.tex.w > 0) {
      int badgeW = overlayLabel.tt.tex.w + 12, badgeH = OVERLAY_TEXT_SIZE + 8;
      int screenHeight = getGfxScreenHeight();
      int x = 12, y = screenHeight - badgeH - 10;
      fillGfxRectangle(x, y, badgeW, badgeH, activeTheme->badgeFill);
      drawLabelAt(&overlayLabel, x + 6, y + 4);
   }
   if (downloadToastShownAt && getTimeUs() - downloadToastShownAt < DOWNLOAD_TOAST_US &&
       downloadToastLabel.tt.tex.w > 0) {
      refreshLabelScale(&downloadToastLabel);
      int x = (screenWidth - downloadToastLabel.tt.tex.w) / 2;
      int y = (getGfxScreenHeight() - downloadToastLabel.tt.tex.h) / 2;
      fillGfxRoundedRectangleFixed(x - 20, y - 12,
                                   downloadToastLabel.tt.tex.w + 40, downloadToastLabel.tt.tex.h + 24,
                                   10, activeTheme->badgeFill);
      drawLabelAt(&downloadToastLabel, x, y);
   }
}

// ---- lifecycle ----

static void migrateStagedDownloads(void)
{
   const char *staging = getVideoStagingDir();
   if (!staging || !staging[0] || strcmp(staging, DOWNLOAD_LIBRARY_DIR) == 0) return;
   VfsDir dir;
   if (openDir(staging, &dir) != 0) return;
   uint8_t *buffer = (uint8_t *)malloc(64 * 1024);
   if (!buffer) { closeDir(&dir); return; }
   char name[256], source[MAX_PATH_LEN], destination[MAX_PATH_LEN];
   VfsEntryType type;
   while (readDir(&dir, name, sizeof name, &type) > 0) {
      if (type != VFS_ENTRY_FILE || strlen(name) != 15 || strcmp(name + 11, ".mp4") != 0) continue;
      name[11] = 0;
      if (!validDownloadId(name)) continue;
      snprintf(source, sizeof source, "%s/%s.mp4", staging, name);
      snprintf(destination, sizeof destination, "%s/%s.mp4", DOWNLOAD_LIBRARY_DIR, name);
      if (!fileExists(destination) && copyFile(source, destination, buffer, 64 * 1024) == 0) {
         const char *extensions[] = { ".meta", ".jpg" };
         for (int i = 0; i < 2; i++) {
            snprintf(source, sizeof source, "%s/%s%s", staging, name, extensions[i]);
            snprintf(destination, sizeof destination, "%s/%s%s", DOWNLOAD_LIBRARY_DIR, name, extensions[i]);
            if (fileExists(source) && !fileExists(destination)) copyFile(source, destination, buffer, 64 * 1024);
         }
         logInfo("[dl] migrated existing download %s into library\n", name);
      }
   }
   free(buffer);
   closeDir(&dir);
}

void initDownloads(void)
{
   memset(&dl, 0, sizeof dl);
   makeDir(DOWNLOAD_LIBRARY_DIR);
   migrateStagedDownloads();
   createLock(&dl.mutex);
   uiFont = openSystemFont(FONT_POP);
   initLabel(&overlayLabel, &uiFont, 0, 0, AUTO, AUTO, OVERLAY_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&downloadToastLabel, &uiFont, 0, 0, AUTO, AUTO, 23, activeTheme->textPrimary, TEXT_NOWRAP, "");
   downloadToastShownAt = 0;
   /* Rasterize at the selected UI scale without scaling the screen-space anchor. */
   setLabelScaleWithUi(&overlayLabel, 1);
   setLabelScaleWithUi(&downloadToastLabel, 1);
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
   freeLabel(&downloadToastLabel);
   closeFont(&uiFont);
   destroyLock(&dl.mutex);
   dl.initialized = 0;
}
