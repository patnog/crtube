// storage - plain-text preferences + watch history under /dev_hdd0/tmp/crtube/ (see storage.h).

#include "app-config.h"
#include "storage.h"
#include "vfs.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DATA_DIR        CRTUBE_DATA_DIR
#define HISTORY_PATH    DATA_DIR "/history.txt"
#define DOWNLOAD_PROGRESS_PATH DATA_DIR "/download-progress.txt"
#define LOCAL_HISTORY_PATH DATA_DIR "/local-history.txt"
#define SUBS_PATH       DATA_DIR "/subscriptions.txt"
#define WATCHLATER_PATH DATA_DIR "/watchlater.txt"
#define SEARCH_HISTORY_PATH DATA_DIR "/searches.txt"
#define PLAYLISTS_DIR       DATA_DIR "/playlists"
#define LOCAL_PLAYLISTS_DIR PLAYLISTS_DIR "/ps3"
#define LOCAL_PLAYLIST_INDEX_PATH LOCAL_PLAYLISTS_DIR "/index.txt"
#define PLAYLIST_WATCH_LATER_PATH PLAYLISTS_DIR "/watch-later.txt"

#define MAX_WATCHLATER          200
#define WATCHLATER_LINE_MAX     480     // one serialized SearchResult (8 tab-separated fields + newline)
#define WATCHLATER_BUFFER_BYTES (MAX_WATCHLATER * WATCHLATER_LINE_MAX + 16)

#define HISTORY_CAP    1000     // watched videos kept (oldest dropped past this)
#define VIDEO_ID_LEN   12       // 11 chars + NUL
#define ENTRY_LINE_MAX 40       // "videoId position duration\n"
#define HISTORY_BUFFER_BYTES (HISTORY_CAP * ENTRY_LINE_MAX + 16)   // whole-file load/save buffer
#define LOCAL_HISTORY_CAP          50
#define LOCAL_HISTORY_LINE_MAX     240
#define LOCAL_HISTORY_BUFFER_BYTES (LOCAL_HISTORY_CAP * LOCAL_HISTORY_LINE_MAX + 16)
#define SEARCH_HISTORY_BUFFER_BYTES (MAX_SEARCH_HISTORY * (SEARCH_QUERY_LEN + 1) + 1)
#define LOCAL_PLAYLIST_CAP          64
#define LOCAL_PLAYLIST_VIDEO_CAP    200
#define LOCAL_PLAYLIST_INDEX_BYTES  (LOCAL_PLAYLIST_CAP * (LOCAL_PLAYLIST_ID_LEN + LOCAL_PLAYLIST_NAME_LEN + 32) + 1)
#define LOCAL_PLAYLIST_LINE_MAX     WATCHLATER_LINE_MAX
#define LOCAL_PLAYLIST_BUFFER_BYTES (LOCAL_PLAYLIST_VIDEO_CAP * LOCAL_PLAYLIST_LINE_MAX + 1)
#define PLAYLIST_WATCH_LATER_CAP    64
#define PLAYLIST_WATCH_LATER_LINE   420
#define PLAYLIST_WATCH_LATER_BYTES  (PLAYLIST_WATCH_LATER_CAP * PLAYLIST_WATCH_LATER_LINE + 1)

// watch history held in memory; the file is the durable copy. a plain ring: once full, the oldest slot is
// overwritten (rare - 1000 videos), and the file is rewritten from the live set. each entry also carries the
// last playback position for resume; a completed video stores its full duration.
typedef struct { char id[VIDEO_ID_LEN]; int position; int duration; } WatchEntry;
static WatchEntry watched[HISTORY_CAP];
static int        watchedCount;
static int        watchedNext;  // next slot to write when the ring is full

// Downloaded videos use a separate resume database so offline playback never reads or overwrites
// the online VOD position for the same YouTube ID.
static WatchEntry downloadedProgress[HISTORY_CAP];
static int        downloadedProgressCount;

// PS3-local history is distinct from the watched/resume database above. It keeps display metadata
// so the History page can render after a restart without requesting account history from YouTube.
static SearchResult localHistory[LOCAL_HISTORY_CAP];
static int          localHistoryCount;

static char searchHistory[MAX_SEARCH_HISTORY][SEARCH_QUERY_LEN];
static int  searchHistoryCount;

static void loadSearchHistory(void)
{
   char *buffer = malloc(SEARCH_HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(SEARCH_HISTORY_PATH, buffer, SEARCH_HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && searchHistoryCount < MAX_SEARCH_HISTORY;
           line = strtok(NULL, "\r\n")) {
         if (!line[0]) continue;
         int duplicate = 0;
         for (int i = 0; i < searchHistoryCount; i++)
            if (strcmp(searchHistory[i], line) == 0) { duplicate = 1; break; }
         if (!duplicate) strCopy(searchHistory[searchHistoryCount++], SEARCH_QUERY_LEN, line);
      }
   }
   free(buffer);
}

static void saveSearchHistory(void)
{
   char buffer[SEARCH_HISTORY_BUFFER_BYTES];
   int length = 0;
   for (int i = 0; i < searchHistoryCount; i++) {
      int remaining = (int)sizeof(buffer) - length;
      if (remaining <= 1) break;
      int written = snprintf(buffer + length, remaining, "%s\n", searchHistory[i]);
      if (written < 0 || written >= remaining) break;
      length += written;
   }
   writeFile(SEARCH_HISTORY_PATH, buffer, (uint64_t)length);
}

static int findSearchHistory(const char *query)
{
   for (int i = 0; i < searchHistoryCount; i++)
      if (strcmp(searchHistory[i], query) == 0) return i;
   return -1;
}

static void dropSearchHistoryAt(int index)
{
   for (int i = index; i + 1 < searchHistoryCount; i++)
      strCopy(searchHistory[i], SEARCH_QUERY_LEN, searchHistory[i + 1]);
   if (searchHistoryCount > 0) searchHistoryCount--;
}

int getSearchHistory(char out[][SEARCH_QUERY_LEN], int max)
{
   if (!out || max <= 0) return 0;
   int count = searchHistoryCount < max ? searchHistoryCount : max;
   for (int i = 0; i < count; i++) strCopy(out[i], SEARCH_QUERY_LEN, searchHistory[i]);
   return count;
}

void addSearchHistory(const char *query)
{
   if (!query || !query[0]) return;

   char clean[SEARCH_QUERY_LEN];
   int length = 0;
   while (query[length] && length + 1 < (int)sizeof(clean)) {
      char c = query[length];
      if (c == '\r' || c == '\n' || c == '\t') c = ' ';
      clean[length] = c;
      length++;
   }
   clean[length] = 0;
   if (!clean[0]) return;

   int existing = findSearchHistory(clean);
   if (existing >= 0) dropSearchHistoryAt(existing);
   else if (searchHistoryCount == MAX_SEARCH_HISTORY) searchHistoryCount--;

   for (int i = searchHistoryCount; i > 0; i--)
      strCopy(searchHistory[i], SEARCH_QUERY_LEN, searchHistory[i - 1]);
   strCopy(searchHistory[0], SEARCH_QUERY_LEN, clean);
   searchHistoryCount++;
   saveSearchHistory();
}

void clearSearchHistory(void)
{
   searchHistoryCount = 0;
   saveSearchHistory();
}

static int isDownloadProgressKey(const char *id)
{
   if (!id || strlen(id) != 11) return 0;
   for (int i = 0; i < 11; i++) {
      char c = id[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
   }
   return 1;
}

static void loadDownloadedProgress(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(DOWNLOAD_PROGRESS_PATH, buffer, HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && downloadedProgressCount < HISTORY_CAP;
           line = strtok(NULL, "\r\n")) {
         char id[VIDEO_ID_LEN]; int position = 0, duration = 0;
         if (sscanf(line, "%11s %d %d", id, &position, &duration) == 3 &&
             isDownloadProgressKey(id)) {
            WatchEntry *entry = &downloadedProgress[downloadedProgressCount++];
            strCopy(entry->id, VIDEO_ID_LEN, id);
            entry->position = position;
            entry->duration = duration;
         }
      }
   }
   free(buffer);
}

static WatchEntry *findDownloadedProgress(const char *videoId)
{
   if (!isDownloadProgressKey(videoId)) return NULL;
   for (int i = 0; i < downloadedProgressCount; i++)
      if (strcmp(downloadedProgress[i].id, videoId) == 0) return &downloadedProgress[i];
   return NULL;
}

static void saveDownloadedProgress(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < downloadedProgressCount; i++) {
      int remaining = HISTORY_BUFFER_BYTES - length;
      if (remaining <= 1) break;
      int written = snprintf(buffer + length, (size_t)remaining, "%s %d %d\n",
                             downloadedProgress[i].id, downloadedProgress[i].position,
                             downloadedProgress[i].duration);
      if (written < 0 || written >= remaining) break;
      length += written;
   }
   writeFile(DOWNLOAD_PROGRESS_PATH, buffer, (uint64_t)length);
   free(buffer);
}

static void loadHistory(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(HISTORY_PATH, buffer, HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && watchedCount < HISTORY_CAP; line = strtok(NULL, "\r\n")) {
         char id[VIDEO_ID_LEN]; int position = 0, duration = 0;
         if (line[0] && sscanf(line, "%11s %d %d", id, &position, &duration) >= 1) {
            strCopy(watched[watchedCount].id, VIDEO_ID_LEN, id);
            watched[watchedCount].position = position;
            watched[watchedCount].duration = duration;
            watchedCount++;
         }
      }
   }
   free(buffer);
}


static int isLocalHistoryId(const char *id)
{
   if (!id || strlen(id) != 11) return 0;
   for (int i = 0; i < 11; i++) {
      char c = id[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
   }
   return 1;
}

static void copyHistoryText(char *out, size_t capacity, const char *input)
{
   size_t n = 0;
   if (!capacity) return;
   if (input) {
      while (*input && n + 1 < capacity) {
         char c = *input++;
         out[n++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
      }
   }
   out[n] = 0;
}

static void loadLocalHistory(void)
{
   char *buffer = malloc(LOCAL_HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(LOCAL_HISTORY_PATH, buffer, LOCAL_HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && localHistoryCount < LOCAL_HISTORY_CAP;
           line = strtok(NULL, "\r\n")) {
         char *title = strchr(line, '\t');
         if (!title) continue;
         *title++ = 0;
         char *author = strchr(title, '\t');
         if (!author) continue;
         *author++ = 0;
         if (!isLocalHistoryId(line)) continue;

         int duplicate = 0;
         for (int i = 0; i < localHistoryCount; i++)
            if (strcmp(localHistory[i].videoId, line) == 0) { duplicate = 1; break; }
         if (duplicate) continue;

         SearchResult *item = &localHistory[localHistoryCount++];
         memset(item, 0, sizeof *item);
         strCopy(item->videoId, sizeof item->videoId, line);
         strCopy(item->title, sizeof item->title, title[0] ? title : "Untitled video");
         strCopy(item->author, sizeof item->author, author);
         strCopy(item->published, sizeof item->published, "On PS3");
      }
   }
   free(buffer);
}

static void saveLocalHistory(void)
{
   char *buffer = malloc(LOCAL_HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < localHistoryCount; i++) {
      char title[sizeof localHistory[i].title];
      char author[sizeof localHistory[i].author];
      copyHistoryText(title, sizeof title, localHistory[i].title);
      copyHistoryText(author, sizeof author, localHistory[i].author);
      int remaining = LOCAL_HISTORY_BUFFER_BYTES - length;
      int written = snprintf(buffer + length, (size_t)remaining, "%s\t%s\t%s\n",
                             localHistory[i].videoId, title, author);
      if (written < 0 || written >= remaining) break;
      length += written;
   }
   writeFile(LOCAL_HISTORY_PATH, buffer, (uint64_t)length);
   free(buffer);
}

int getLocalHistory(SearchResults *out)
{
   if (!out) return 0;
   memset(out, 0, sizeof *out);
   out->count = localHistoryCount;
   if (out->count > 0) {
      memcpy(out->items, localHistory, (size_t)out->count * sizeof out->items[0]);
      for (int i = 0; i < out->count; i++) {
         int duration = getWatchedDuration(out->items[i].videoId);
         if (duration > 0) {
            int hours = duration / 3600;
            int minutes = (duration / 60) % 60;
            int seconds = duration % 60;
            if (hours > 0)
               snprintf(out->items[i].duration, sizeof out->items[i].duration,
                        "%d:%02d:%02d", hours, minutes, seconds);
            else
               snprintf(out->items[i].duration, sizeof out->items[i].duration,
                        "%d:%02d", minutes, seconds);
         }
      }
   }
   return out->count;
}

void recordLocalHistory(const char *videoId, const char *title, const char *author)
{
   if (!isLocalHistoryId(videoId)) return;

   // Re-watching moves the existing entry to the newest position; no duplicate IDs are retained.
   for (int i = 0; i < localHistoryCount; i++) {
      if (strcmp(localHistory[i].videoId, videoId) == 0) {
         if (i + 1 < localHistoryCount)
            memmove(&localHistory[i], &localHistory[i + 1],
                    (size_t)(localHistoryCount - i - 1) * sizeof localHistory[0]);
         localHistoryCount--;
         break;
      }
   }

   if (localHistoryCount == LOCAL_HISTORY_CAP) localHistoryCount--;
   if (localHistoryCount > 0)
      memmove(localHistory + 1, localHistory,
              (size_t)localHistoryCount * sizeof localHistory[0]);

   memset(&localHistory[0], 0, sizeof localHistory[0]);
   strCopy(localHistory[0].videoId, sizeof localHistory[0].videoId, videoId);
   strCopy(localHistory[0].title, sizeof localHistory[0].title,
           title && title[0] ? title : "Untitled video");
   strCopy(localHistory[0].author, sizeof localHistory[0].author, author ? author : "");
   strCopy(localHistory[0].published, sizeof localHistory[0].published, "On PS3");
   localHistoryCount++;
   saveLocalHistory();
}

// a resumable key is a bare videoId; a typed url (carries '/' or ':') never enters history.
static int isHistoryKey(const char *videoId) { return videoId[0] && !strchr(videoId, '/') && !strchr(videoId, ':'); }

static WatchEntry *findWatched(const char *videoId)
{
   for (int i = 0; i < watchedCount; i++)
      if (strcmp(watched[i].id, videoId) == 0) return &watched[i];
   return NULL;
}

// add a fresh entry (ring-overwrites the oldest when full) at position 0.
static WatchEntry *addWatched(const char *videoId)
{
   WatchEntry *entry;
   if (watchedCount < HISTORY_CAP) entry = &watched[watchedCount++];
   else { entry = &watched[watchedNext]; watchedNext = (watchedNext + 1) % HISTORY_CAP; }
   strCopy(entry->id, VIDEO_ID_LEN, videoId);
   entry->position = 0;
   entry->duration = 0;
   return entry;
}

// seeded on first run; the user edits subscriptions.txt to change these. one channel id per line.
static const char *DEFAULT_SUBS =
   "UCX6OQ3DkcsbYNE6H8uQQuVA\n"   // MrBeast
   "UCXuqSBlHAE6Xw-yeJA0Tunw\n"   // Linus Tech Tips
   "UCBJycsmduvYEL83R_U4JriQ\n"   // MKBHD
   "UCHnyfMqiRRG1u-2MsSQLbXA\n"   // Veritasium
   "UCsXVk37bltHxD1rDPwtNM8Q\n";  // Kurzgesagt

static void seedSubscriptions(void)
{
   if (!fileExists(SUBS_PATH)) writeFile(SUBS_PATH, DEFAULT_SUBS, strlen(DEFAULT_SUBS));
}

// ---- watch-later queue (full entries so the category renders offline) ----

static SearchResult watchLater[MAX_WATCHLATER];
static int          watchLaterCount;
static int          watchLaterRevision;

// split "videoId\tchannelId\ttitle\tduration\tauthor\tviews\tpublished\tisLive" into item. 1 ok, 0 malformed.
static int parseWatchLater(char *line, SearchResult *item)
{
   char *field[8];
   int count = 0;
   field[count++] = line;
   for (char *cursor = line; *cursor && count < 8; cursor++)
      if (*cursor == '\t') { *cursor = 0; field[count++] = cursor + 1; }
   if (count < 8) return 0;

   memset(item, 0, sizeof *item);
   strCopy(item->videoId,   sizeof item->videoId,   field[0]);
   strCopy(item->channelId, sizeof item->channelId, field[1]);
   strCopy(item->title,     sizeof item->title,     field[2]);
   strCopy(item->duration,  sizeof item->duration,  field[3]);
   strCopy(item->author,    sizeof item->author,    field[4]);
   strCopy(item->views,     sizeof item->views,     field[5]);
   strCopy(item->published, sizeof item->published, field[6]);
   item->isLive = atoi(field[7]);
   return item->videoId[0] != 0;
}

static void loadWatchLater(void)
{
   char *buffer = malloc(WATCHLATER_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(WATCHLATER_PATH, buffer, WATCHLATER_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && watchLaterCount < MAX_WATCHLATER; line = strtok(NULL, "\r\n"))
         if (parseWatchLater(line, &watchLater[watchLaterCount])) watchLaterCount++;
   }
   free(buffer);
}

static void saveWatchLater(void)
{
   char *buffer = malloc(WATCHLATER_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < watchLaterCount; i++) {
      const SearchResult *item = &watchLater[i];
      length += snprintf(buffer + length, WATCHLATER_LINE_MAX + 1, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                         item->videoId, item->channelId, item->title, item->duration,
                         item->author, item->views, item->published, item->isLive);
   }
   writeFile(WATCHLATER_PATH, buffer, length);
   free(buffer);
}

int getWatchLaterRevision(void) { return watchLaterRevision; }

int isWatchLater(const char *videoId)
{
   for (int i = 0; i < watchLaterCount; i++)
      if (strcmp(watchLater[i].videoId, videoId) == 0) return 1;
   return 0;
}

void toggleWatchLater(const SearchResult *item)
{
   for (int i = 0; i < watchLaterCount; i++)
      if (strcmp(watchLater[i].videoId, item->videoId) == 0) {   // already queued: remove it
         for (int j = i; j < watchLaterCount - 1; j++) watchLater[j] = watchLater[j + 1];
         watchLaterCount--;
         saveWatchLater();
         watchLaterRevision++;
         return;
      }
   if (watchLaterCount >= MAX_WATCHLATER) return;
   watchLater[watchLaterCount++] = *item;   // add newest at the end
   saveWatchLater();
   watchLaterRevision++;
}

// fill out with the queued videos, newest first. returns the count.
int getWatchLater(SearchResults *out)
{
   memset(out, 0, sizeof *out);
   for (int i = watchLaterCount - 1; i >= 0 && out->count < MAX_SEARCH_RESULTS; i--)
      out->items[out->count++] = watchLater[i];
   return out->count;
}


typedef struct {
   char id[LOCAL_PLAYLIST_ID_LEN];
   char name[LOCAL_PLAYLIST_NAME_LEN];
   int count;
   char newestVideoId[16];
} LocalPlaylistMeta;

static LocalPlaylistMeta localPlaylists[LOCAL_PLAYLIST_CAP];
static int localPlaylistCount;
static int localPlaylistNextId = 1;
static int localPlaylistIndexLoaded;
static int localPlaylistRevision;
static int playlistWatchLaterRevision;
static SearchResult playlistWatchLater[PLAYLIST_WATCH_LATER_CAP];
static int playlistWatchLaterCount;
static int playlistWatchLaterLoaded;

static int validLocalPlaylistId(const char *id)
{
   if (!id || strncmp(id, "pl", 2) != 0 || !id[2]) return 0;
   for (int i = 2; id[i]; i++)
      if (id[i] < '0' || id[i] > '9') return 0;
   return 1;
}

static void localPlaylistPath(const char *id, char *path, int capacity)
{
   snprintf(path, capacity, LOCAL_PLAYLISTS_DIR "/%s.txt", id);
}

static void saveLocalPlaylistIndex(void)
{
   char *buffer = malloc(LOCAL_PLAYLIST_INDEX_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < localPlaylistCount; i++) {
      int remaining = LOCAL_PLAYLIST_INDEX_BYTES - length;
      if (remaining <= 1) break;
      int written = snprintf(buffer + length, remaining, "%s\t%s\t%d\t%s\n",
                             localPlaylists[i].id, localPlaylists[i].name,
                             localPlaylists[i].count, localPlaylists[i].newestVideoId);
      if (written < 0 || written >= remaining) break;
      length += written;
   }
   writeFile(LOCAL_PLAYLIST_INDEX_PATH, buffer, (uint64_t)length);
   free(buffer);
}

static void loadLocalPlaylistIndex(void)
{
   if (localPlaylistIndexLoaded) return;
   localPlaylistIndexLoaded = 1;
   char *buffer = malloc(LOCAL_PLAYLIST_INDEX_BYTES);
   if (!buffer) return;
   int length = readFile(LOCAL_PLAYLIST_INDEX_PATH, buffer, LOCAL_PLAYLIST_INDEX_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && localPlaylistCount < LOCAL_PLAYLIST_CAP;
           line = strtok(NULL, "\r\n")) {
         char *name = strchr(line, '\t');
         if (!name) continue;
         *name++ = 0;
         char *countText = strchr(name, '\t');
         if (countText) *countText++ = 0;
         char *newestId = countText ? strchr(countText, '\t') : NULL;
         if (newestId) *newestId++ = 0;
         if (!validLocalPlaylistId(line) || !name[0]) continue;
         int duplicate = 0;
         for (int i = 0; i < localPlaylistCount; i++)
            if (strcmp(localPlaylists[i].id, line) == 0) { duplicate = 1; break; }
         if (duplicate) continue;
         LocalPlaylistMeta *meta = &localPlaylists[localPlaylistCount];
         memset(meta, 0, sizeof *meta);
         strCopy(meta->id, sizeof meta->id, line);
         copyHistoryText(meta->name, sizeof meta->name, name);
         meta->count = countText ? atoi(countText) : -1;
         if (meta->count < 0 || meta->count > LOCAL_PLAYLIST_VIDEO_CAP) meta->count = -1;
         if (newestId) strCopy(meta->newestVideoId, sizeof meta->newestVideoId, newestId);
         int number = atoi(line + 2);
         if (number >= localPlaylistNextId) localPlaylistNextId = number + 1;
         localPlaylistCount++;
      }
   }
   free(buffer);
}

static void serializeLocalVideo(char *out, int capacity, const SearchResult *item)
{
   char videoId[16], channelId[32], title[160], duration[24], author[48], views[24], published[24];
   copyHistoryText(videoId, sizeof videoId, item->videoId);
   copyHistoryText(channelId, sizeof channelId, item->channelId);
   copyHistoryText(title, sizeof title, item->title);
   copyHistoryText(duration, sizeof duration, item->duration);
   copyHistoryText(author, sizeof author, item->author);
   copyHistoryText(views, sizeof views, item->views);
   copyHistoryText(published, sizeof published, item->published);
   snprintf(out, capacity, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
            videoId, channelId, title, duration, author, views, published, item->isLive ? 1 : 0);
}

static int readLocalPlaylist(const char *id, SearchResult *items, int maxItems)
{
   if (!validLocalPlaylistId(id) || !items || maxItems <= 0) return -1;
   char path[160];
   localPlaylistPath(id, path, sizeof path);
   char *buffer = malloc(LOCAL_PLAYLIST_BUFFER_BYTES);
   if (!buffer) return -1;
   int length = readFile(path, buffer, LOCAL_PLAYLIST_BUFFER_BYTES - 1);
   int count = 0;
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && count < maxItems; line = strtok(NULL, "\r\n"))
         if (parseWatchLater(line, &items[count])) count++;
   }
   free(buffer);
   return count;
}

static int writeLocalPlaylist(const char *id, const SearchResult *items, int count)
{
   if (!validLocalPlaylistId(id) || (count > 0 && !items) || count < 0 || count > LOCAL_PLAYLIST_VIDEO_CAP) return -1;
   char path[160];
   localPlaylistPath(id, path, sizeof path);
   char *buffer = malloc(LOCAL_PLAYLIST_BUFFER_BYTES);
   if (!buffer) return -1;
   int length = 0;
   for (int i = 0; i < count; i++) {
      char line[LOCAL_PLAYLIST_LINE_MAX + 1];
      serializeLocalVideo(line, sizeof line, &items[i]);
      int lineLength = (int)strlen(line);
      if (length + lineLength >= LOCAL_PLAYLIST_BUFFER_BYTES) break;
      memcpy(buffer + length, line, (size_t)lineLength);
      length += lineLength;
   }
   int result = writeFile(path, buffer, (uint64_t)length);
   free(buffer);
   return result;
}

int getLocalPlaylistCount(void)
{
   loadLocalPlaylistIndex();
   return localPlaylistCount;
}

int getLocalPlaylistInfo(int newestIndex, char *idOut, int idCapacity,
                         char *nameOut, int nameCapacity)
{
   loadLocalPlaylistIndex();
   if (newestIndex < 0 || newestIndex >= localPlaylistCount) return -1;
   LocalPlaylistMeta *meta = &localPlaylists[localPlaylistCount - 1 - newestIndex];
   if (idOut && idCapacity > 0) strCopy(idOut, (size_t)idCapacity, meta->id);
   if (nameOut && nameCapacity > 0) strCopy(nameOut, (size_t)nameCapacity, meta->name);
   return 0;
}

int getLocalPlaylistRevision(void) { return localPlaylistRevision; }
int getPlaylistWatchLaterRevision(void) { return playlistWatchLaterRevision; }

int getLocalPlaylists(SearchResults *out)
{
   if (!out) return -1;
   loadLocalPlaylistIndex();
   memset(out, 0, sizeof *out);
   char *buffer = NULL;
   int migrated = 0;
   int migrationFailed = 0;
   for (int i = localPlaylistCount - 1; i >= 0 && out->count < MAX_SEARCH_RESULTS; i--) {
      LocalPlaylistMeta *meta = &localPlaylists[i];
      if (meta->count < 0) {
         if (!buffer) buffer = (char *)malloc(LOCAL_PLAYLIST_BUFFER_BYTES);
         if (buffer) {
            migrated = 1;
            char path[160];
            localPlaylistPath(meta->id, path, sizeof path);
            int length = readFile(path, buffer, LOCAL_PLAYLIST_BUFFER_BYTES - 1);
            meta->count = 0;
            meta->newestVideoId[0] = 0;
            if (length > 0) {
               buffer[length] = 0;
               char *line = buffer;
               char *end = buffer + length;
               while (line < end) {
                  char *next = strchr(line, '\n');
                  if (next) *next = 0;
                  if (*line) {
                     if (meta->count == 0) {
                        SearchResult first;
                        if (parseWatchLater(line, &first))
                           strCopy(meta->newestVideoId, sizeof meta->newestVideoId, first.videoId);
                     }
                     meta->count++;
                  }
                  if (!next) break;
                  line = next + 1;
               }
            }
         } else {
            migrationFailed = 1;
         }
      }
      int displayCount = meta->count >= 0 ? meta->count : 0;
      SearchResult *tile = &out->items[out->count++];
      memset(tile, 0, sizeof *tile);
      tile->isPlaylist = 1;
      tile->isLocalPlaylist = 1;
      strCopy(tile->localPlaylistId, sizeof tile->localPlaylistId, meta->id);
      strCopy(tile->title, sizeof tile->title, meta->name);
      strCopy(tile->author, sizeof tile->author, "This PS3");
      snprintf(tile->duration, sizeof tile->duration, "%d video%s", displayCount,
               displayCount == 1 ? "" : "s");
      strCopy(tile->published, sizeof tile->published, "On PS3");
      strCopy(tile->thumbnailVideoId, sizeof tile->thumbnailVideoId, meta->newestVideoId);
   }
   free(buffer);
   if (migrated && !migrationFailed) saveLocalPlaylistIndex();
   return out->count;
}

int getLocalPlaylistVideos(const char *playlistId, SearchResults *out)
{
   if (!out) return -1;
   memset(out, 0, sizeof *out);
   int count = readLocalPlaylist(playlistId, out->items, MAX_SEARCH_RESULTS);
   if (count < 0) return -1;
   out->count = count;
   return count;
}

int addVideoToLocalPlaylist(const char *playlistId, const SearchResult *item)
{
   if (!validLocalPlaylistId(playlistId) || !item || !item->videoId[0] || item->isPlaylist) return -1;
   SearchResult *items = (SearchResult *)malloc(sizeof(SearchResult) * LOCAL_PLAYLIST_VIDEO_CAP);
   if (!items) return -1;
   int count = readLocalPlaylist(playlistId, items, LOCAL_PLAYLIST_VIDEO_CAP);
   if (count < 0) { free(items); return -1; }
   for (int i = 0; i < count; i++) {
      if (strcmp(items[i].videoId, item->videoId) == 0) {
         if (i > 0) memmove(items + 1, items, (size_t)i * sizeof items[0]);
         items[0] = *item;
         int result = writeLocalPlaylist(playlistId, items, count);
         free(items);
         if (result == 0) {
            loadLocalPlaylistIndex();
            for (int n = 0; n < localPlaylistCount; n++) if (strcmp(localPlaylists[n].id, playlistId) == 0) {
               strCopy(localPlaylists[n].newestVideoId, sizeof localPlaylists[n].newestVideoId, item->videoId);
               saveLocalPlaylistIndex();
               break;
            }
            localPlaylistRevision++;
         }
         return result;
      }
   }
   if (count == LOCAL_PLAYLIST_VIDEO_CAP) count--;
   if (count > 0) memmove(items + 1, items, (size_t)count * sizeof items[0]);
   items[0] = *item;
   count++;
   int result = writeLocalPlaylist(playlistId, items, count);
   free(items);
   if (result == 0) {
      loadLocalPlaylistIndex();
      for (int n = 0; n < localPlaylistCount; n++) if (strcmp(localPlaylists[n].id, playlistId) == 0) {
         localPlaylists[n].count = count;
         strCopy(localPlaylists[n].newestVideoId, sizeof localPlaylists[n].newestVideoId, item->videoId);
         saveLocalPlaylistIndex();
         break;
      }
      localPlaylistRevision++;
   }
   return result;
}

int createLocalPlaylist(const char *name, const SearchResult *firstVideo,
                        char *playlistIdOut, int playlistIdCapacity)
{
   if (!name || !name[0] || (firstVideo && (!firstVideo->videoId[0] || firstVideo->isPlaylist))) return -1;
   loadLocalPlaylistIndex();
   if (localPlaylistCount >= LOCAL_PLAYLIST_CAP) return -1;
   char cleanName[LOCAL_PLAYLIST_NAME_LEN];
   copyHistoryText(cleanName, sizeof cleanName, name);
   if (!cleanName[0]) return -1;
   char id[LOCAL_PLAYLIST_ID_LEN], path[160];
   do {
      snprintf(id, sizeof id, "pl%04d", localPlaylistNextId++);
      localPlaylistPath(id, path, sizeof path);
   } while (fileExists(path));
   if (writeLocalPlaylist(id, firstVideo, firstVideo ? 1 : 0) != 0) return -1;
   strCopy(localPlaylists[localPlaylistCount].id, LOCAL_PLAYLIST_ID_LEN, id);
   strCopy(localPlaylists[localPlaylistCount].name, LOCAL_PLAYLIST_NAME_LEN, cleanName);
   localPlaylists[localPlaylistCount].count = firstVideo ? 1 : 0;
   if (firstVideo) strCopy(localPlaylists[localPlaylistCount].newestVideoId, sizeof localPlaylists[localPlaylistCount].newestVideoId, firstVideo->videoId);
   localPlaylistCount++;
   saveLocalPlaylistIndex();
   localPlaylistRevision++;
   if (playlistIdOut && playlistIdCapacity > 0) strCopy(playlistIdOut, (size_t)playlistIdCapacity, id);
   return 0;
}

int createEmptyLocalPlaylist(const char *name, char *playlistIdOut, int playlistIdCapacity)
{
   return createLocalPlaylist(name, NULL, playlistIdOut, playlistIdCapacity);
}

int removeVideoFromLocalPlaylist(const char *playlistId, const char *videoId)
{
   if (!validLocalPlaylistId(playlistId) || !videoId || !videoId[0]) return -1;
   SearchResult *items = (SearchResult *)malloc(sizeof(SearchResult) * LOCAL_PLAYLIST_VIDEO_CAP);
   if (!items) return -1;
   int count = readLocalPlaylist(playlistId, items, LOCAL_PLAYLIST_VIDEO_CAP);
   if (count < 0) { free(items); return -1; }
   int found = -1;
   for (int i = 0; i < count; i++) if (!strcmp(items[i].videoId, videoId)) { found = i; break; }
   if (found < 0) { free(items); return 0; }
   if (found + 1 < count) memmove(items + found, items + found + 1, (size_t)(count - found - 1) * sizeof items[0]);
   count--;
   char newestVideoId[16] = "";
   if (count > 0) strCopy(newestVideoId, sizeof newestVideoId, items[0].videoId);
   int rc = writeLocalPlaylist(playlistId, items, count);
   free(items);
   if (rc == 0) {
      loadLocalPlaylistIndex();
      for (int i = 0; i < localPlaylistCount; i++) if (!strcmp(localPlaylists[i].id, playlistId)) {
         localPlaylists[i].count = count;
         strCopy(localPlaylists[i].newestVideoId, sizeof localPlaylists[i].newestVideoId, newestVideoId);
         break;
      }
      saveLocalPlaylistIndex(); localPlaylistRevision++;
   }
   return rc;
}

int deleteLocalPlaylist(const char *playlistId)
{
   if (!validLocalPlaylistId(playlistId)) return -1;
   loadLocalPlaylistIndex();
   int index = -1;
   for (int i = 0; i < localPlaylistCount; i++) if (!strcmp(localPlaylists[i].id, playlistId)) { index = i; break; }
   if (index < 0) return 0;
   char path[160]; localPlaylistPath(playlistId, path, sizeof path); deleteFile(path);
   if (index + 1 < localPlaylistCount) memmove(&localPlaylists[index], &localPlaylists[index + 1], (size_t)(localPlaylistCount - index - 1) * sizeof localPlaylists[0]);
   localPlaylistCount--; saveLocalPlaylistIndex(); localPlaylistRevision++;
   return 0;
}

int renameLocalPlaylist(const char *playlistId, const char *name)
{
   if (!validLocalPlaylistId(playlistId) || !name || !name[0]) return -1;
   loadLocalPlaylistIndex();
   char cleanName[LOCAL_PLAYLIST_NAME_LEN];
   copyHistoryText(cleanName, sizeof cleanName, name);
   if (!cleanName[0]) return -1;
   for (int i = 0; i < localPlaylistCount; i++) {
      if (!strcmp(localPlaylists[i].id, playlistId)) {
         strCopy(localPlaylists[i].name, sizeof localPlaylists[i].name, cleanName);
         saveLocalPlaylistIndex();
         localPlaylistRevision++;
         return 0;
      }
   }
   return -1;
}

static void loadPlaylistWatchLater(void)
{
   if (playlistWatchLaterLoaded) return;
   playlistWatchLaterLoaded = 1;
   char *buffer = malloc(PLAYLIST_WATCH_LATER_BYTES);
   if (!buffer) return;
   int length = readFile(PLAYLIST_WATCH_LATER_PATH, buffer, PLAYLIST_WATCH_LATER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && playlistWatchLaterCount < PLAYLIST_WATCH_LATER_CAP;
           line = strtok(NULL, "\r\n")) {
         char *fields[6]; int fieldCount = 0;
         fields[fieldCount++] = line;
         for (char *p = line; *p && fieldCount < 6; p++) if (*p == '\t') { *p = 0; fields[fieldCount++] = p + 1; }
         if (fieldCount != 6 || !fields[0][0]) continue;
         SearchResult *item = &playlistWatchLater[playlistWatchLaterCount++];
         memset(item, 0, sizeof *item);
         item->isPlaylist = 1;
         strCopy(item->playlistId, sizeof item->playlistId, fields[0]);
         strCopy(item->title, sizeof item->title, fields[1]);
         strCopy(item->author, sizeof item->author, fields[2]);
         strCopy(item->duration, sizeof item->duration, fields[3]);
         strCopy(item->published, sizeof item->published, fields[4]);
         strCopy(item->thumbnailVideoId, sizeof item->thumbnailVideoId, fields[5]);
      }
   }
   free(buffer);
}

static void savePlaylistWatchLater(void)
{
   char *buffer = malloc(PLAYLIST_WATCH_LATER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < playlistWatchLaterCount; i++) {
      SearchResult *item = &playlistWatchLater[i];
      char title[160], author[48], duration[24], published[24];
      copyHistoryText(title, sizeof title, item->title);
      copyHistoryText(author, sizeof author, item->author);
      copyHistoryText(duration, sizeof duration, item->duration);
      copyHistoryText(published, sizeof published, item->published);
      int remaining = PLAYLIST_WATCH_LATER_BYTES - length;
      if (remaining <= 1) break;
      int written = snprintf(buffer + length, remaining, "%s\t%s\t%s\t%s\t%s\t%s\n",
                             item->playlistId, title, author, duration, published, item->thumbnailVideoId);
      if (written < 0 || written >= remaining) break;
      length += written;
   }
   writeFile(PLAYLIST_WATCH_LATER_PATH, buffer, (uint64_t)length);
   free(buffer);
}

int isPlaylistWatchLater(const char *playlistId)
{
   if (!playlistId || !playlistId[0]) return 0;
   loadPlaylistWatchLater();
   for (int i = 0; i < playlistWatchLaterCount; i++)
      if (strcmp(playlistWatchLater[i].playlistId, playlistId) == 0) return 1;
   return 0;
}

int addPlaylistToWatchLater(const SearchResult *playlist)
{
   if (!playlist || !playlist->isPlaylist || !playlist->playlistId[0]) return -1;
   loadPlaylistWatchLater();
   for (int i = 0; i < playlistWatchLaterCount; i++) {
      if (strcmp(playlistWatchLater[i].playlistId, playlist->playlistId) == 0) {
         if (i + 1 < playlistWatchLaterCount)
            memmove(&playlistWatchLater[i], &playlistWatchLater[i + 1],
                    (size_t)(playlistWatchLaterCount - i - 1) * sizeof playlistWatchLater[0]);
         playlistWatchLaterCount--;
         break;
      }
   }
   if (playlistWatchLaterCount == PLAYLIST_WATCH_LATER_CAP) {
      memmove(playlistWatchLater, playlistWatchLater + 1,
              (size_t)(PLAYLIST_WATCH_LATER_CAP - 1) * sizeof playlistWatchLater[0]);
      playlistWatchLaterCount--;
   }
   playlistWatchLater[playlistWatchLaterCount] = *playlist;
   playlistWatchLater[playlistWatchLaterCount].isLocalPlaylist = 0;
   playlistWatchLaterCount++;
   savePlaylistWatchLater();
   playlistWatchLaterRevision++;
   return 0;
}

int removePlaylistFromWatchLater(const char *playlistId)
{
   if (!playlistId || !playlistId[0]) return -1;
   loadPlaylistWatchLater();
   for (int i = 0; i < playlistWatchLaterCount; i++) if (!strcmp(playlistWatchLater[i].playlistId, playlistId)) {
      if (i + 1 < playlistWatchLaterCount) memmove(&playlistWatchLater[i], &playlistWatchLater[i + 1], (size_t)(playlistWatchLaterCount - i - 1) * sizeof playlistWatchLater[0]);
      playlistWatchLaterCount--; savePlaylistWatchLater(); playlistWatchLaterRevision++; return 0;
   }
   return 0;
}

int getPlaylistWatchLater(SearchResults *out)
{
   if (!out) return -1;
   loadPlaylistWatchLater();
   memset(out, 0, sizeof *out);
   for (int i = playlistWatchLaterCount - 1; i >= 0 && out->count < MAX_SEARCH_RESULTS; i--)
      out->items[out->count++] = playlistWatchLater[i];
   return out->count;
}

void initStorage(void)
{
   makeDir(DATA_DIR);
   makeDir(PLAYLISTS_DIR);
   makeDir(LOCAL_PLAYLISTS_DIR);
   loadLocalPlaylistIndex();
   loadPlaylistWatchLater();
   loadHistory();
   loadDownloadedProgress();
   loadLocalHistory();
   loadSearchHistory();
   seedSubscriptions();
   loadWatchLater();
}

int getSubscriptions(char ids[][CHANNEL_ID_LEN], int max)
{
   char buffer[MAX_SUBSCRIPTIONS * CHANNEL_ID_LEN + 16];
   int length = readFile(SUBS_PATH, buffer, sizeof buffer - 1);
   if (length <= 0) return 0;
   buffer[length] = 0;

   int count = 0;
   for (char *line = strtok(buffer, "\r\n"); line && count < max; line = strtok(NULL, "\r\n"))
      if (line[0] == 'U' && line[1] == 'C') { strCopy(ids[count], CHANNEL_ID_LEN, line); count++; }   // skip blanks/comments
   return count;
}

// rewrite subscriptions.txt from an id array (one channel id per line).
static void saveSubscriptions(char ids[][CHANNEL_ID_LEN], int count)
{
   char buffer[MAX_SUBSCRIPTIONS * CHANNEL_ID_LEN + 16];
   int length = 0;
   for (int i = 0; i < count; i++)
      length += snprintf(buffer + length, CHANNEL_ID_LEN + 2, "%s\n", ids[i]);
   writeFile(SUBS_PATH, buffer, length);
}

static int subsRevision;   // bumped on every real subscribe/unsubscribe so cached feeds know to refetch
int getSubscriptionsRevision(void) { return subsRevision; }

int isSubscribed(const char *channelId)
{
   char ids[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int count = getSubscriptions(ids, MAX_SUBSCRIPTIONS);
   for (int i = 0; i < count; i++)
      if (strcmp(ids[i], channelId) == 0) return 1;
   return 0;
}

void setSubscribed(const char *channelId, int subscribed)
{
   if (channelId[0] != 'U' || channelId[1] != 'C') return;   // only real UC ids
   char ids[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int count = getSubscriptions(ids, MAX_SUBSCRIPTIONS);
   int found = -1;
   for (int i = 0; i < count; i++)
      if (strcmp(ids[i], channelId) == 0) { found = i; break; }

   if (subscribed && found < 0) {
      if (count >= MAX_SUBSCRIPTIONS) return;
      strCopy(ids[count++], CHANNEL_ID_LEN, channelId);
   } else if (!subscribed && found >= 0) {
      for (int i = found; i < count - 1; i++) strCopy(ids[i], CHANNEL_ID_LEN, ids[i + 1]);
      count--;
   } else {
      return;   // already in the desired state
   }
   saveSubscriptions(ids, count);
   subsRevision++;
}

int isWatched(const char *videoId) { return findWatched(videoId) != NULL; }

int getWatchedPosition(const char *videoId)
{
   WatchEntry *entry = findWatched(videoId);
   return entry ? entry->position : 0;
}

int getWatchedDuration(const char *videoId)
{
   WatchEntry *entry = findWatched(videoId);
   return entry ? entry->duration : 0;
}

// rewrite history.txt from the in-memory set ("id position duration"; older two-field files remain valid).
static void saveHistory(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < watchedCount; i++)
      length += snprintf(buffer + length, ENTRY_LINE_MAX + 1, "%s %d %d\n", watched[i].id, watched[i].position, watched[i].duration);
   writeFile(HISTORY_PATH, buffer, length);
   free(buffer);
}

void markWatched(const char *videoId)
{
   if (!isWatched(videoId)) setWatchedPosition(videoId, 0);   // add new at position 0; keep an existing position
}

void setWatchedPosition(const char *videoId, int seconds)
{
   if (!isHistoryKey(videoId)) return;
   WatchEntry *entry = findWatched(videoId);
   if (!entry) entry = addWatched(videoId);
   entry->position = seconds;
   saveHistory();
}

void setWatchedDuration(const char *videoId, int durationSeconds)
{
   if (!isHistoryKey(videoId) || durationSeconds <= 0) return;
   WatchEntry *entry = findWatched(videoId);
   if (!entry) entry = addWatched(videoId);
   entry->duration = durationSeconds;
   saveHistory();
}

int getDownloadedPosition(const char *videoId)
{
   WatchEntry *entry = findDownloadedProgress(videoId);
   return entry ? entry->position : 0;
}

int isDownloadedWatched(const char *videoId)
{
   WatchEntry *entry = findDownloadedProgress(videoId);
   return entry && entry->duration > 0 && entry->position >= entry->duration;
}

void setDownloadedProgress(const char *videoId, int positionSeconds, int durationSeconds)
{
   if (!isDownloadProgressKey(videoId)) return;
   WatchEntry *entry = findDownloadedProgress(videoId);
   if (!entry) {
      if (downloadedProgressCount >= HISTORY_CAP) {
         memmove(downloadedProgress, downloadedProgress + 1,
                 (HISTORY_CAP - 1) * sizeof downloadedProgress[0]);
         downloadedProgressCount--;
      }
      entry = &downloadedProgress[downloadedProgressCount++];
      memset(entry, 0, sizeof *entry);
      strCopy(entry->id, VIDEO_ID_LEN, videoId);
   }
   entry->position = positionSeconds < 0 ? 0 : positionSeconds;
   if (durationSeconds > 0) entry->duration = durationSeconds;
   saveDownloadedProgress();
}
