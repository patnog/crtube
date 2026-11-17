// subtitles - fetch + parse one YouTube timed-text track (see subtitles.h).
//
// The track url returns the classic "srv1" XML: <text start="9.53" dur="5.07">line</text> per cue.
// We request that format explicitly (&fmt=srv1) so the layout is deterministic, decode the XML
// entities (auto captions are often double-escaped, hence the repeated pass), and keep one flat
// time-ordered cue array the player scans forward through during playback.

#include "subtitles.h"
#include "app-config.h"
#include "thread.h"             // getTimeUs
#include "http.h"
#include "text-sanitize.h"      // decodeXmlEntities / stripMarkupTags / removeInvisibleMarks / flattenWhitespace
#include "string-utilities.h"   // strCopy, truncateUtf8
#include "dbg.h"
#include "vfs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SUBS_RESP_CAP (1024 * 1024)   // hardware-observed: a multi-hour video's track passed 512 KB

// parse one <text start=".." dur="..">content</text> element at `element`; returns the scan position
// after it, or NULL when no further element exists before `end`.
static const char *parseCue(const char *element, const char *end, SubtitleTrack *out)
{
   const char *tag = strstr(element, "<text ");
   if (!tag || tag >= end) return NULL;

   float start = 0, dur = 0;
   const char *startAttr = strstr(tag, "start=\"");
   const char *durAttr   = strstr(tag, "dur=\"");
   const char *open      = strchr(tag, '>');
   const char *close     = open ? strstr(open, "</text>") : NULL;
   if (!open || !close || close >= end) return NULL;
   if (startAttr && startAttr < open) start = (float)atof(startAttr + 7);
   if (durAttr && durAttr < open)     dur   = (float)atof(durAttr + 5);
   if (dur <= 0.0f) dur = 3.0f;   // rare missing duration: show the line briefly rather than dropping it

   // clean in a roomy scratch buffer FIRST, truncate LAST: styled tracks wrap a short line in
   // hundreds of bytes of tags, and truncating the raw text mid-tag leaves tag fragments visible.
   // (the caller's loop bounds out->count, so no cap check is needed here.)
   char raw[1024];
   int length = (int)(close - open - 1);
   if (length > (int)sizeof raw - 1) length = (int)sizeof raw - 1;
   memcpy(raw, open + 1, length);
   raw[length] = 0;
   for (int pass = 0; pass < 4 && decodeXmlEntities(raw); pass++) {}   // repeat: styled tracks double-escape
   stripMarkupTags(raw);
   removeInvisibleMarks(raw);
   flattenWhitespace(raw);
   truncateUtf8(raw, SUBTITLE_TEXT_MAX - 1);

   if (raw[0]) {
      // word-highlight ("karaoke") tracks repeat one line as many short cues; merge the repeats so
      // the line neither flickers nor burns through the cue cap.
      SubtitleCue *previous = out->count ? &out->cues[out->count - 1] : NULL;
      if (previous && strcmp(previous->text, raw) == 0 && start <= previous->end + 0.5f) {
         if (start + dur > previous->end) previous->end = start + dur;
      } else {
         SubtitleCue *cue = &out->cues[out->count++];
         strCopy(cue->text, sizeof cue->text, raw);
         cue->start = start;
         cue->end   = start + dur;
      }
   }
   return close + 7;
}

int fetchSubtitles(const char *url, SubtitleTrack *out)
{
   memset(out, 0, sizeof *out);

   // Force the XML format this parser understands, while preserving tlang and all
   // signed query parameters. The translation target must survive this rewrite.
   char fullUrl[2200];
   strCopy(fullUrl, sizeof fullUrl, url);
   char *fmt = strstr(fullUrl, "&fmt=");
   if (!fmt) fmt = strstr(fullUrl, "?fmt=");
   if (fmt) {
      char *afterValue = strchr(fmt + 1, '&');
      if (afterValue) memmove(fmt, afterValue, strlen(afterValue) + 1);
      else *fmt = 0;
   }
   size_t urlLength = strlen(fullUrl);
   snprintf(fullUrl + urlLength, sizeof fullUrl - urlLength, "%cfmt=srv1",
            strchr(fullUrl, '?') ? '&' : '?');

   char *resp = malloc(SUBS_RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = getHttp(fullUrl, resp, SUBS_RESP_CAP, &respLen, &status);
   if (rc < 0)        { logWarn("[subs] fetch failed rc=%d\n", rc); free(resp); return rc; }
   logInfo("[subs] HTTP status=%d bytes=%d translated=%d\n",
           status, respLen, strstr(url, "tlang=") != NULL);
   if (status != 200) { logWarn("[subs] unexpected status=%d\n", status); free(resp); return -status; }

   const char *end = resp + respLen;
   const char *scan = resp;
   while (scan && out->count < MAX_SUBTITLE_CUES) scan = parseCue(scan, end, out);
   logInfo("[subs] %d cue(s) parsed (%d bytes)\n", out->count, respLen);
   if (strstr(url, "tlang=") && out->count == 0)
      logWarn("[subs] YouTube returned an empty translated track; verify tlang source/target availability\n");
   if (respLen >= SUBS_RESP_CAP - 1 || out->count == MAX_SUBTITLE_CUES)
      logWarn("[subs] track truncated at a cap, later lines will be missing\n");

   free(resp);
   return 0;
}



#define SUBTITLE_CACHE_DIR CRTUBE_DATA_DIR "/subtitle-cache"
#define SUBTITLE_CACHE_MARKER SUBTITLE_CACHE_DIR "/.cleanup"
#define SUBTITLE_CACHE_LIMIT (10ULL * 1024ULL * 1024ULL)
#define SUBTITLE_CACHE_MAX_AGE (30ULL * 24ULL * 60ULL * 60ULL)
#define SUBTITLE_CACHE_CLEAN_INTERVAL (24ULL * 60ULL * 60ULL)

static int hasSuffix(const char *name, const char *suffix)
{
   size_t nameLength = strlen(name), suffixLength = strlen(suffix);
   return nameLength >= suffixLength && strcmp(name + nameLength - suffixLength, suffix) == 0;
}

static int isCacheSrt(const char *name)
{
   return hasSuffix(name, ".srt");
}

static int isCacheTemporary(const char *name)
{
   return hasSuffix(name, ".tmp");
}

static void subtitleCacheAccessPath(const char *path, char *out, int capacity)
{
   snprintf(out, capacity, "%s.access", path);
}

void touchSubtitleTranslationCache(const char *path)
{
   if (!path || !path[0]) return;
   char accessPath[MAX_PATH_LEN];
   subtitleCacheAccessPath(path, accessPath, sizeof accessPath);
   writeFile(accessPath, "1", 1);
}

static int scanCache(uint64_t *totalBytes, char *oldestPath, int oldestCapacity,
                     uint64_t *oldestMtime, const char *excludePath)
{
   VfsDir dir;
   char name[256], path[MAX_PATH_LEN];
   VfsEntryType type;
   int result = 0;
   *totalBytes = 0;
   *oldestMtime = UINT64_MAX;
   if (oldestPath && oldestCapacity > 0) oldestPath[0] = 0;
   if (openDir(SUBTITLE_CACHE_DIR, &dir) != 0) return -1;
   while ((result = readDir(&dir, name, sizeof name, &type)) > 0) {
      if (type != VFS_ENTRY_FILE || !isCacheSrt(name)) continue;
      snprintf(path, sizeof path, "%s/%s", SUBTITLE_CACHE_DIR, name);
      if (excludePath && strcmp(path, excludePath) == 0) continue;
      VfsStat stat;
      if (statPath(path, &stat) != 0 || stat.isDir) continue;
      *totalBytes += stat.size;
      if (oldestPath && stat.mtime < *oldestMtime) {
         *oldestMtime = stat.mtime;
         strCopy(oldestPath, oldestCapacity, path);
      }
   }
   closeDir(&dir);
   return result < 0 ? -1 : 0;
}

void maintainSubtitleTranslationCache(void)
{
   makeDir(SUBTITLE_CACHE_DIR);
   uint64_t now = getTimeUs() / 1000000ULL;
   VfsStat marker;
   int dailyDue = statPath(SUBTITLE_CACHE_MARKER, &marker) != 0 || marker.mtime == 0 ||
      (now >= marker.mtime && now - marker.mtime >= SUBTITLE_CACHE_CLEAN_INTERVAL);

   // Remove abandoned writes on every launch. Expire completed online translations once per day.
   // Delete one item per scan: directory backends need not support deleting the current entry.
   for (;;) {
      VfsDir dir;
      char name[256], path[MAX_PATH_LEN];
      VfsEntryType type;
      int found = 0;
      if (openDir(SUBTITLE_CACHE_DIR, &dir) != 0) break;
      int readResult;
      while ((readResult = readDir(&dir, name, sizeof name, &type)) > 0) {
         if (type != VFS_ENTRY_FILE || (!isCacheTemporary(name) &&
             !(isCacheSrt(name) && dailyDue) && !hasSuffix(name, ".access"))) continue;
         snprintf(path, sizeof path, "%s/%s", SUBTITLE_CACHE_DIR, name);
         int remove = isCacheTemporary(name);
         if (hasSuffix(name, ".access")) {
            char subtitlePath[MAX_PATH_LEN];
            strCopy(subtitlePath, sizeof subtitlePath, path);
            subtitlePath[strlen(subtitlePath) - strlen(".access")] = 0;
            remove = !fileExists(subtitlePath); // orphaned access marker
         } else if (!remove && dailyDue && isCacheSrt(name)) {
            VfsStat stat, accessStat;
            char accessPath[MAX_PATH_LEN];
            uint64_t lastAccess = 0;
            if (statPath(path, &stat) == 0) lastAccess = stat.mtime;
            subtitleCacheAccessPath(path, accessPath, sizeof accessPath);
            if (statPath(accessPath, &accessStat) == 0 && accessStat.mtime > 0)
               lastAccess = accessStat.mtime;
            if (lastAccess > 0 && now >= lastAccess && now - lastAccess >= SUBTITLE_CACHE_MAX_AGE)
               remove = 1;
         }
         if (remove) { found = 1; break; }
      }
      closeDir(&dir);
      if (readResult < 0 || !found) break;
      int wasSubtitle = isCacheSrt(path);
      if (deleteFile(path) != 0) break;
      if (wasSubtitle) {
         char accessPath[MAX_PATH_LEN];
         subtitleCacheAccessPath(path, accessPath, sizeof accessPath);
         deleteFile(accessPath);
      }
   }
   if (dailyDue) {
      writeFile(SUBTITLE_CACHE_MARKER, "1", 1);
      logInfo("[subs] translation cache daily expiry checked (30 days)\n");
   }
}

int reserveSubtitleTranslationCacheSpace(const char *newPath, uint64_t newSize)
{
   if (!newPath || newSize > SUBTITLE_CACHE_LIMIT) return -1;
   uint64_t total = 0, oldestMtime = 0;
   char oldest[MAX_PATH_LEN];
   // Exclude the previous entry at newPath: it will be replaced after the temporary file is complete.
   if (scanCache(&total, oldest, sizeof oldest, &oldestMtime, newPath) != 0) return -1;
   while (total > SUBTITLE_CACHE_LIMIT - newSize) {
      if (!oldest[0]) return -1;
      VfsStat stat;
      uint64_t freed = 0;
      if (statPath(oldest, &stat) == 0) freed = stat.size;
      if (deleteFile(oldest) != 0 || freed == 0) return -1;
      char accessPath[MAX_PATH_LEN];
      subtitleCacheAccessPath(oldest, accessPath, sizeof accessPath);
      deleteFile(accessPath);
      total = total > freed ? total - freed : 0;
      if (scanCache(&total, oldest, sizeof oldest, &oldestMtime, newPath) != 0) return -1;
   }
   return 0;
}

const char *getSubtitleCueText(const SubtitleTrack *track, float seconds, int *scanFrom,
                               int *activeCue, int *activePending)
{
   if (activeCue) *activeCue = -1;
   if (activePending) *activePending = 0;
   if (*scanFrom >= track->count || seconds < track->cues[*scanFrom].start) *scanFrom = 0;   // sought backwards

   // auto captions "roll up": cue windows overlap the next line's, so among the cues covering this
   // moment always show the LATEST-started one - returning the first match displays the previous
   // sentence for its whole overlap and reads as lagging subtitles.
   const char *text = "";
   for (int i = *scanFrom; i < track->count; i++) {
      if (track->cues[i].start > seconds) break;
      *scanFrom = i;
      if (seconds < track->cues[i].end) {
         int pending = ((volatile const SubtitleCue *)&track->cues[i])->pending;
         text = pending ? "" : track->cues[i].text;
         if (activeCue) *activeCue = i;
         if (activePending) *activePending = pending;
      }
   }
   __sync_synchronize();
   return text;
}


#define SUBTITLE_SRT_CAP (1024 * 1024)

static void formatSrtTime(char *out, int cap, float seconds)
{
   long ms = (long)(seconds * 1000.0f + 0.5f);
   if (ms < 0) ms = 0;
   long hours = ms / 3600000L; ms %= 3600000L;
   long minutes = ms / 60000L; ms %= 60000L;
   long secs = ms / 1000L; ms %= 1000L;
   snprintf(out, cap, "%02ld:%02ld:%02ld,%03ld", hours, minutes, secs, ms);
}

int saveSubtitleTrackSrt(const SubtitleTrack *track, const char *path)
{
   if (!track || !path || track->count <= 0) return -1;
   size_t capacity = (size_t)track->count * (SUBTITLE_TEXT_MAX + 64) + 1;
   char *output = (char *)malloc(capacity);
   if (!output) return -1;
   size_t used = 0;
   for (int i = 0; i < track->count; i++) {
      char start[24], end[24];
      formatSrtTime(start, sizeof start, track->cues[i].start);
      formatSrtTime(end, sizeof end, track->cues[i].end);
      int written = snprintf(output + used, capacity - used, "%d\n%s --> %s\n%s\n\n",
                             i + 1, start, end, track->cues[i].text);
      if (written < 0 || (size_t)written >= capacity - used) { used = 0; break; }
      used += (size_t)written;
   }
   int rc = used ? writeFile(path, output, (uint64_t)used) : -1;
   free(output);
   return rc;
}

int saveSubtitlesSrt(const char *url, const char *path)
{
   if (!url || !path) return -1;
   SubtitleTrack *track = (SubtitleTrack *)malloc(sizeof *track);
   if (!track) return -1;
   int rc = fetchSubtitles(url, track);
   if (rc == 0 && track->count > 0) rc = saveSubtitleTrackSrt(track, path);
   else if (rc == 0) rc = -1;
   free(track);
   return rc;
}

static int parseSrtTimestamp(const char *line, int *startMs, int *endMs)
{
   int sh, sm, ss, sms, eh, em, es, ems;
   if (sscanf(line, "%d:%d:%d,%d --> %d:%d:%d,%d",
              &sh, &sm, &ss, &sms, &eh, &em, &es, &ems) != 8) return 0;
   *startMs = ((sh * 60 + sm) * 60 + ss) * 1000 + sms;
   *endMs = ((eh * 60 + em) * 60 + es) * 1000 + ems;
   return *endMs > *startMs;
}

static char *nextSrtLine(char **cursor)
{
   if (!cursor || !*cursor || !**cursor) return NULL;
   char *line = *cursor;
   char *end = strpbrk(line, "\r\n");
   if (end) {
      char eol = *end;
      *end = 0;
      char *next = end + 1;
      if (eol == '\r' && *next == '\n') next++;
      *cursor = next;
   } else {
      *cursor = line + strlen(line);
   }
   return line;
}

int loadSubtitlesSrt(const char *path, SubtitleTrack *out)
{
   if (!path || !out) return -1;
   memset(out, 0, sizeof *out);
   char *text = (char *)malloc(SUBTITLE_SRT_CAP + 1);
   if (!text) return -1;
   int length = readFile(path, text, SUBTITLE_SRT_CAP);
   if (length <= 0) { free(text); return -1; }
   if (length > SUBTITLE_SRT_CAP) length = SUBTITLE_SRT_CAP;
   text[length] = 0;

   char *cursor = text;
   char *line;
   while (out->count < MAX_SUBTITLE_CUES && (line = nextSrtLine(&cursor)) != NULL) {
      if (!line[0]) continue;
      char *timestamp = nextSrtLine(&cursor);
      int startMs = 0, endMs = 0;
      if (!timestamp || !parseSrtTimestamp(timestamp, &startMs, &endMs)) continue;

      SubtitleCue *cue = &out->cues[out->count];
      cue->start = startMs / 1000.0f;
      cue->end = endMs / 1000.0f;
      size_t used = 0;
      while ((line = nextSrtLine(&cursor)) != NULL && line[0]) {
         size_t add = strlen(line);
         if (used && used < sizeof cue->text - 1) cue->text[used++] = ' ';
         if (add > sizeof cue->text - 1 - used) add = sizeof cue->text - 1 - used;
         memcpy(cue->text + used, line, add);
         used += add;
         cue->text[used] = 0;
      }
      if (cue->text[0]) out->count++;
   }
   free(text);
   return out->count > 0 ? 0 : -1;
}
