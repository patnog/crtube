// storage - plain-text preferences + watch history under /dev_hdd0/tmp/yo-player/ (see storage.h).

#include "app-config.h"
#include "storage.h"
#include "vfs.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DATA_DIR        CRTUBE_DATA_DIR
#define HISTORY_PATH    DATA_DIR "/history.txt"
#define LOCAL_HISTORY_PATH DATA_DIR "/local-history.txt"
#define SUBS_PATH       DATA_DIR "/subscriptions.txt"
#define WATCHLATER_PATH DATA_DIR "/watchlater.txt"

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

// watch history held in memory; the file is the durable copy. a plain ring: once full, the oldest slot is
// overwritten (rare - 1000 videos), and the file is rewritten from the live set. each entry also carries the
// last playback position for resume; a completed video stores its full duration.
typedef struct { char id[VIDEO_ID_LEN]; int position; int duration; } WatchEntry;
static WatchEntry watched[HISTORY_CAP];
static int        watchedCount;
static int        watchedNext;  // next slot to write when the ring is full

// PS3-local history is distinct from the watched/resume database above. It keeps display metadata
// so the History page can render after a restart without requesting account history from YouTube.
static SearchResult localHistory[LOCAL_HISTORY_CAP];
static int          localHistoryCount;

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

void initStorage(void)
{
   makeDir(DATA_DIR);
   loadHistory();
   loadLocalHistory();
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
