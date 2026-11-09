// video-grid - the reusable windowed video-result grid (see video-grid.h).
//
// A background worker fetches result pages (via the screen-supplied GridFetchFn) and thumbnail jpegs; the
// jpegs are kept and only the on-screen band of tiles is "materialised" (thumbnail decoded to a texture +
// labels rasterised), freed again on scroll-away and re-decoded from the kept jpeg on the way back, so VRAM
// stays flat no matter how many results are loaded. The worker is stopped before playback so no per-call
// http fetch overlaps the streaming http client.

#include "video-grid.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "http.h"
#include "image-loader.h"
#include "dbg.h"
#include "string-utilities.h"   // strCopy
#include "account.h"            // getAccountRevision
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GRID_GAP    22
#define TILE_TEXT_H 54          // space under each thumbnail for the title + meta line
#define ROW_TITLE   21
#define META_SIZE   15
#define DUR_SIZE    19
#define WATCHLATER_BADGE_SIZE DUR_SIZE
#define MARGIN_ROWS 2           // rows kept materialised above and below the visible band
#define THUMB_LOOKAHEAD_ROWS 2  // fetch two rows below the visible band before the user scrolls
#define DECODES_PER_FRAME 1
#define THUMB_CAP   (96 * 1024) // one mqdefault jpeg fits easily
#define THUMB_UA    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"
#define MEMBERS_ONLY_CACHE_CAP 512

#define WATCHED_ALPHA 70        // label alpha for already-watched tiles
#define WATCH_PROGRESS_H 4      // red resume indicator along the thumbnail bottom edge
#define WATCH_PROGRESS_COLOR 0xFFFF0000

// Positive Members-only results are reused across grids/screens for the current account session.
// IDs are kept in FIFO order; a sorted slot index provides binary-search lookups without duplicating IDs.
// Negative and failed checks are deliberately not cached.
static char memberOnlyIdCache[MEMBERS_ONLY_CACHE_CAP][12];
static uint16_t memberOnlySortedSlots[MEMBERS_ONLY_CACHE_CAP];
static volatile int memberOnlyIdCacheCount;
static int memberOnlyIdCacheHead;
static int memberOnlyCacheAccountRevision = -1;
static volatile int memberOnlyCacheLock;
#define MEMBERS_ONLY_INFLIGHT_CAP 64

typedef struct {
   char videoId[12];
   int accountRevision;
} MemberOnlyInFlight;
static MemberOnlyInFlight memberOnlyInFlight[MEMBERS_ONLY_INFLIGHT_CAP];
static int memberOnlyInFlightCount;
#define MEMBER_IDLE_FRAMES 30

static void lockMemberOnlyCache(void)
{
   while (__sync_lock_test_and_set(&memberOnlyCacheLock, 1)) {}
}

static void unlockMemberOnlyCache(void)
{
   __sync_lock_release(&memberOnlyCacheLock);
}

// Returns the lower-bound position in the sorted slot index and whether the ID exists.
static int findMemberOnlyIdPosition(const char *videoId, int *found)
{
   int low = 0;
   int high = memberOnlyIdCacheCount;
   while (low < high) {
      int mid = low + (high - low) / 2;
      int slot = memberOnlySortedSlots[mid];
      int cmp = strcmp(memberOnlyIdCache[slot], videoId);
      if (cmp < 0) low = mid + 1;
      else high = mid;
   }
   *found = low < memberOnlyIdCacheCount &&
            strcmp(memberOnlyIdCache[memberOnlySortedSlots[low]], videoId) == 0;
   return low;
}

static void syncMemberOnlyCacheAccountLocked(void)
{
   int revision = getAccountRevision();
   if (revision != memberOnlyCacheAccountRevision) {
      memberOnlyIdCacheCount = 0;
      memberOnlyIdCacheHead = 0;
      memberOnlyInFlightCount = 0;
      __sync_synchronize();
      memberOnlyCacheAccountRevision = revision;
   }
}

static int isMemberOnlyIdCached(const char *videoId)
{
   if (!videoId || !videoId[0]) return 0;
   lockMemberOnlyCache();
   syncMemberOnlyCacheAccountLocked();
   int found = 0;
   findMemberOnlyIdPosition(videoId, &found);
   unlockMemberOnlyCache();
   return found;
}

static int isMemberOnlyCheckInFlight(const char *videoId)
{
   if (!videoId || !videoId[0]) return 0;
   lockMemberOnlyCache();
   syncMemberOnlyCacheAccountLocked();
   int revision = memberOnlyCacheAccountRevision;
   int found = 0;
   for (int i = 0; i < memberOnlyInFlightCount; i++) {
      if (memberOnlyInFlight[i].accountRevision == revision &&
          strcmp(memberOnlyInFlight[i].videoId, videoId) == 0) {
         found = 1;
         break;
      }
   }
   unlockMemberOnlyCache();
   return found;
}

// Reserve an ID before spawning a worker so separate grids cannot issue duplicate lookups.
static int claimMemberOnlyCheck(const char *videoId, int *accountRevision)
{
   if (!videoId || !videoId[0]) return 0;
   lockMemberOnlyCache();
   syncMemberOnlyCacheAccountLocked();
   int revision = memberOnlyCacheAccountRevision;
   for (int i = 0; i < memberOnlyInFlightCount; i++) {
      if (memberOnlyInFlight[i].accountRevision == revision &&
          strcmp(memberOnlyInFlight[i].videoId, videoId) == 0) {
         unlockMemberOnlyCache();
         return 0;
      }
   }
   if (memberOnlyInFlightCount >= MEMBERS_ONLY_INFLIGHT_CAP) {
      unlockMemberOnlyCache();
      return 0;
   }
   int slot = memberOnlyInFlightCount++;
   strCopy(memberOnlyInFlight[slot].videoId, sizeof memberOnlyInFlight[slot].videoId, videoId);
   memberOnlyInFlight[slot].accountRevision = revision;
   if (accountRevision) *accountRevision = revision;
   unlockMemberOnlyCache();
   return 1;
}

static int isMemberOnlyAccountRevisionCurrent(int accountRevision)
{
   lockMemberOnlyCache();
   syncMemberOnlyCacheAccountLocked();
   int current = memberOnlyCacheAccountRevision == accountRevision;
   unlockMemberOnlyCache();
   return current;
}

static void releaseMemberOnlyCheck(const char *videoId, int accountRevision)
{
   if (!videoId || !videoId[0]) return;
   lockMemberOnlyCache();
   for (int i = 0; i < memberOnlyInFlightCount; i++) {
      if (memberOnlyInFlight[i].accountRevision == accountRevision &&
          strcmp(memberOnlyInFlight[i].videoId, videoId) == 0) {
         int last = --memberOnlyInFlightCount;
         if (i != last) memberOnlyInFlight[i] = memberOnlyInFlight[last];
         break;
      }
   }
   unlockMemberOnlyCache();
}

static void cacheMemberOnlyId(const char *videoId)
{
   if (!videoId || !videoId[0] || strlen(videoId) >= sizeof memberOnlyIdCache[0]) return;
   lockMemberOnlyCache();
   syncMemberOnlyCacheAccountLocked();

   int found = 0;
   int position = findMemberOnlyIdPosition(videoId, &found);
   if (found) {
      unlockMemberOnlyCache();
      return;
   }

   int slot;
   int count = memberOnlyIdCacheCount;
   if (count == MEMBERS_ONLY_CACHE_CAP) {
      // Evict the oldest ID, then reuse its ring slot.
      slot = memberOnlyIdCacheHead;
      int oldPosition = findMemberOnlyIdPosition(memberOnlyIdCache[slot], &found);
      if (found && oldPosition < count - 1)
         memmove(&memberOnlySortedSlots[oldPosition],
                 &memberOnlySortedSlots[oldPosition + 1],
                 (size_t)(count - oldPosition - 1) * sizeof memberOnlySortedSlots[0]);
      memberOnlyIdCacheHead = (memberOnlyIdCacheHead + 1) % MEMBERS_ONLY_CACHE_CAP;
      memberOnlyIdCacheCount = --count;
      position = findMemberOnlyIdPosition(videoId, &found);
   } else {
      slot = (memberOnlyIdCacheHead + count) % MEMBERS_ONLY_CACHE_CAP;
   }

   strCopy(memberOnlyIdCache[slot], sizeof memberOnlyIdCache[slot], videoId);
   if (position < count)
      memmove(&memberOnlySortedSlots[position + 1], &memberOnlySortedSlots[position],
              (size_t)(count - position) * sizeof memberOnlySortedSlots[0]);
   memberOnlySortedSlots[position] = (uint16_t)slot;
   __sync_synchronize();
   memberOnlyIdCacheCount = count + 1;
   unlockMemberOnlyCache();
}

static void applyMemberOnlyCacheRange(VideoGrid *grid, int start, int end)
{
   if (end > grid->results.count) end = grid->results.count;
   for (int i = start; i < end; i++) {
      if (!grid->results.items[i].isPlaylist &&
          isMemberOnlyIdCached(grid->results.items[i].videoId))
         grid->membersOnly[i] = GRID_MEMBER_YES;
   }
   __sync_synchronize();
}

// ---- thumbnail fetch (worker side) ----

static void thumbFetchThread(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   uint8_t *buffer = (uint8_t *)malloc(THUMB_CAP);
   if (buffer) {
      HttpHeader headers[] = { { "User-Agent", THUMB_UA }, { "Accept-Encoding", "identity" } };
      for (;;) {
         int i = __sync_fetch_and_add(&grid->thumbNext, 1);
         if (i >= grid->thumbEnd || grid->stopWorker) break;

         if (grid->thumbs[i].state != THUMB_NONE) continue;
         SearchResult *item = &grid->results.items[i];
         const char *thumbnailVideoId = item->videoId;
         if (item->isPlaylist) {
            // Resolve a playlist's first video off the UI thread. Cached playlist results retain
            // thumbnailVideoId, so revisiting the Playlists page does not repeat this browse call.
            if (!item->thumbnailVideoId[0]) {
               SearchResults *playlistPage = (SearchResults *)malloc(sizeof *playlistPage);
               if (playlistPage) {
                  if (getAccountPlaylistVideos(item->playlistId, NULL, playlistPage) == 0 &&
                      playlistPage->count > 0 && playlistPage->items[0].videoId[0])
                     strCopy(item->thumbnailVideoId, sizeof item->thumbnailVideoId,
                             playlistPage->items[0].videoId);
                  free(playlistPage);
               }
            }
            thumbnailVideoId = item->thumbnailVideoId;
            if (grid->stopWorker) {
               grid->thumbs[i].state = THUMB_NONE;
               break;
            }
         }
         if (!thumbnailVideoId[0]) {
            grid->thumbs[i].state = THUMB_FAILED;
            continue;
         }
         char url[128];
         snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", thumbnailVideoId);
         int length = 0, status = 0;
         int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)buffer, THUMB_CAP, &length, &status);
         if (grid->stopWorker) {
            grid->thumbs[i].state = THUMB_NONE;
            break;
         }
         if (rc == 0 && status == 200 && length > 0) {
            uint8_t *copy = (uint8_t *)malloc(length);
            if (copy) {
               memcpy(copy, buffer, length);
               grid->thumbs[i].jpeg = copy; grid->thumbs[i].jpegLen = length;
               __sync_synchronize();
               grid->thumbs[i].state = THUMB_FETCHED;
               logInfo("[grid] thumbnail %s fetched (%d bytes)\n", thumbnailVideoId, length);
            } else {
               grid->thumbs[i].state = THUMB_FAILED;
               logWarn("[grid] thumbnail %s skipped (out of memory)\n", thumbnailVideoId);
            }
         } else {
            grid->thumbs[i].state = THUMB_FAILED;
            logWarn("[grid] thumbnail %s failed rc=%d status=%d bytes=%d\n", thumbnailVideoId, rc, status, length);
         }
      }
      free(buffer);
   } else {
      for (;;) {
         int i = __sync_fetch_and_add(&grid->thumbNext, 1);
         if (i >= grid->thumbEnd || grid->stopWorker) break;
         if (grid->thumbs[i].state == THUMB_NONE) grid->thumbs[i].state = THUMB_FAILED;
      }
   }
   exitThread();
}

// Fetch the current visible/lookahead range on bounded thumbnail workers.
static void fetchThumbnailsRange(VideoGrid *grid, int from, int to)
{
   if (to <= from) return;
   logInfo("[grid] fetching %d thumbnails from %d\n", to - from, from);
   grid->thumbNext = from;
   grid->thumbEnd = to;
   int threadLimit = GRID_THREADS;
   for (int i = from; i < to; i++) {
      if (grid->results.items[i].isPlaylist) { threadLimit = 1; break; }
   }
   sys_ppu_thread_t threads[GRID_THREADS];
   int spawned = 0;
   for (int t = 0; t < threadLimit; t++)
      if (spawnJoinableThread(&threads[spawned], thumbFetchThread, (uint64_t)(uintptr_t)grid,
                              THREAD_PRIORITY_LOW, 128 * 1024, "yt-thumb") == 0)
         spawned++;
   for (int t = 0; t < spawned; t++) joinThread(threads[t]);
   if (spawned == 0) {
      for (int i = from; i < to && i < grid->results.count; i++)
         if (grid->thumbs[i].state == THUMB_NONE) grid->thumbs[i].state = THUMB_FAILED;
   }
   logInfo("[grid] thumbnail batch finished\n");
}

// append a fetched page's items, skipping duplicate video or playlist IDs. sortTokens (channel chips) are kept
// from the first page - continuation pages don't carry them.
static void appendPage(VideoGrid *grid, const SearchResults *page)
{
   for (int i = 0; i < page->count && grid->results.count < MAX_SEARCH_RESULTS; i++) {
      const SearchResult *item = &page->items[i];
      int duplicate = 0;
      for (int j = 0; j < grid->results.count; j++) {
         const SearchResult *existing = &grid->results.items[j];
         if (item->isPlaylist && existing->isPlaylist) {
            if (strcmp(existing->playlistId, item->playlistId) == 0) { duplicate = 1; break; }
         } else if (!item->isPlaylist && !existing->isPlaylist &&
                    strcmp(existing->videoId, item->videoId) == 0) {
            duplicate = 1;
            break;
         }
      }
      if (duplicate) continue;
      grid->results.items[grid->results.count] = *item;
      __sync_synchronize();
      grid->results.count++;
   }
   strCopy(grid->results.continuation, sizeof grid->results.continuation, page->continuation);
}

// The thumbnail queue is rebuilt from the current viewport whenever a worker becomes idle. The selected
// tile wins, then the remaining visible/lookahead tiles are ordered by distance from the selection.
// Navigation naturally reprioritizes pending work without storing a stale queue.
static int isTileVisible(const VideoGrid *grid, int index)
{
   int row = index / grid->columns;
   return index >= 0 && index < grid->results.count &&
          row >= grid->scrollRow && row < grid->scrollRow + grid->visibleRows;
}

static int thumbnailFetchEnd(const VideoGrid *grid)
{
   int end = (grid->scrollRow + grid->visibleRows + THUMB_LOOKAHEAD_ROWS) * grid->columns;
   if (end > grid->results.count) end = grid->results.count;
   return end;
}

static int hasPendingVisibleThumbnailDecode(const VideoGrid *grid)
{
   if (grid->decodeIndex >= 0) return 1;
   int start = grid->scrollRow * grid->columns;
   int end = (grid->scrollRow + grid->visibleRows) * grid->columns;
   if (end > grid->results.count) end = grid->results.count;
   for (int i = start; i < end; i++) {
      if (grid->thumbs[i].state == THUMB_DECODING ||
          (grid->thumbs[i].state == THUMB_FETCHED && !grid->thumbs[i].tex.offset))
         return 1;
   }
   return 0;
}

static int nextVisibleThumbnail(const VideoGrid *grid)
{
   int start = grid->scrollRow * grid->columns;
   int end = thumbnailFetchEnd(grid);
   int best = -1;
   int bestDistance = 0x7fffffff;
   for (int i = start; i < end; i++) {
      if (grid->thumbs[i].state != THUMB_NONE) continue;
      int distance = i > grid->selected ? i - grid->selected : grid->selected - i;
      if (distance < bestDistance) {
         best = i;
         bestDistance = distance;
      }
   }
   return best;
}

// Playlist JPEGs are useful only near the viewport. Keep the visible band and the grid's existing
// materialisation margin; evict older playlist JPEGs after their RSX texture has also left that window.
static void evictPlaylistThumbCache(VideoGrid *grid)
{
   int start = (grid->scrollRow - MARGIN_ROWS) * grid->columns;
   int end = (grid->scrollRow + grid->visibleRows + MARGIN_ROWS) * grid->columns;
   if (start < 0) start = 0;
   if (end > grid->results.count) end = grid->results.count;
   for (int i = 0; i < grid->results.count; i++) {
      if (!grid->results.items[i].isPlaylist || (i >= start && i < end)) continue;
      if (grid->thumbs[i].tex.offset || grid->thumbs[i].state != THUMB_FETCHED) continue;
      if (grid->thumbs[i].jpeg) {
         free(grid->thumbs[i].jpeg);
         grid->thumbs[i].jpeg = NULL;
      }
      grid->thumbs[i].jpegLen = 0;
      grid->thumbs[i].state = THUMB_NONE;
   }
}

static void worker(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   logInfo("[diag-grid] worker begin grid=%p job=%d stage=%d\n", grid, grid->job, grid->stage);
   switch (grid->job) {
   case JOB_LOAD: {
      logInfo("[diag-grid] JOB_LOAD fetch begin grid=%p\n", grid);
      int rc = grid->fetch(NULL, &grid->results, grid->fetchUser);
      logInfo("[diag-grid] JOB_LOAD fetch returned rc=%d count=%d\n", rc, grid->results.count);
      if (rc == 0) applyMemberOnlyCacheRange(grid, 0, grid->results.count);
      // Publish the metadata before thumbnail work starts. drawVideoGrid() can render placeholders
      // immediately while the independent thumbnail workers fill the visible band and lookahead.
      grid->stage = (rc == 0 && grid->results.count > 0) ? GRID_READY : GRID_EMPTY;
      __sync_synchronize();
      grid->searchParsed = 1;
      if (grid->stage == GRID_READY && !grid->backgroundPaused) {
         int index = nextVisibleThumbnail(grid);
         if (index >= 0) fetchThumbnailsRange(grid, index, thumbnailFetchEnd(grid));
      }
      break;
   }
   case JOB_CACHE: {   // cached results are shown immediately; fetch one current-viewport thumbnail
      if (!grid->backgroundPaused) {
         int index = nextVisibleThumbnail(grid);
         if (index >= 0) fetchThumbnailsRange(grid, index, thumbnailFetchEnd(grid));
      }
      break;
   }
   case JOB_THUMBS: {  // one item per worker; the next index is chosen again from the live viewport
      int index = grid->thumbRequestIndex;
      int end = grid->thumbRequestEnd;
      if (index >= 0 && index < end && index < grid->results.count)
         fetchThumbnailsRange(grid, index, end);
      break;
   }
   case JOB_MORE: {    // append the next page; its thumbnails are picked up lazily by a later JOB_THUMBS
      SearchResults *page = (SearchResults *)malloc(sizeof *page);
      if (page && grid->fetch(grid->results.continuation, page, grid->fetchUser) == 0) {
         int base = grid->results.count;
         appendPage(grid, page);
         applyMemberOnlyCacheRange(grid, base, grid->results.count);
         if (grid->results.count == base) grid->results.continuation[0] = 0;   // nothing new: stop paging
      }
      free(page);
      break;
   }
   case JOB_MEMBER_CHECK: {
      int i = grid->memberCheckIndex;
      if (i >= 0 && i < grid->results.count && !grid->results.items[i].isPlaylist) {
         const char *videoId = grid->results.items[i].videoId;
         int rc = -1;
         if (!grid->stopWorker) {
            const Extractor *extractor = findExtractor(videoId);
            rc = extractor && extractor->isVideoMembersOnly
                 ? extractor->isVideoMembersOnly(videoId) : -1;
         }
         if (grid->stopWorker) {
            grid->membersOnly[i] = GRID_MEMBER_UNKNOWN;
         } else {
            grid->membersOnly[i] = rc > 0 ? GRID_MEMBER_YES
                                 : rc == 0 ? GRID_MEMBER_NO : GRID_MEMBER_FAILED;
            if (rc > 0 && isMemberOnlyAccountRevisionCurrent(grid->memberCheckAccountRevision))
               cacheMemberOnlyId(videoId);
            __sync_synchronize();
            logInfo("[grid] members-only check %s result=%s\n",
                    videoId, rc > 0 ? "yes" : rc == 0 ? "no" : "unavailable");
         }
         releaseMemberOnlyCheck(videoId, grid->memberCheckAccountRevision);
      }
      break;
   }
   }
   __sync_synchronize();
   grid->workerDone = 1;
   logInfo("[diag-grid] worker done grid=%p job=%d stage=%d count=%d\n", grid, grid->job, grid->stage, grid->results.count);
   exitThread();
}

static int spawnWorker(VideoGrid *grid)
{
   grid->stopWorker = 0;
   grid->workerDone = 0;
   grid->threadActive = (spawnJoinableThread(&grid->workerTid, worker, (uint64_t)(uintptr_t)grid,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-feed") == 0);
   return grid->threadActive;
}

// ---- tile windowing (UI side) ----

static void cancelGridDecode(VideoGrid *grid)
{
   if (grid->decodeIndex < 0) return;
   int i = grid->decodeIndex;
   cancelImageAsync();
   if (i < grid->results.count && grid->thumbs[i].state == THUMB_DECODING)
      grid->thumbs[i].state = THUMB_FETCHED;
   grid->decodeIndex = -1;
}

static void freeThumbnails(VideoGrid *grid)
{
   cancelGridDecode(grid);
   finishGfx();
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      if (grid->thumbs[i].jpeg) { free(grid->thumbs[i].jpeg); grid->thumbs[i].jpeg = NULL; }
      freeGfxTexture(&grid->thumbs[i].tex);
   }
   memset(grid->thumbs, 0, sizeof grid->thumbs);
}

static void resetFeed(VideoGrid *grid)
{
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   memset(&grid->results, 0, sizeof grid->results);
   grid->stage = GRID_RUNNING;
   grid->searchParsed = grid->workerDone = 0;
   grid->thumbRequestIndex = -1;
   grid->thumbRequestEnd = 0;
   memset(grid->membersOnly, 0, sizeof grid->membersOnly);
   grid->memberCheckIndex = -1;
   grid->memberIdleFrames = 0;
   grid->decodeIndex = -1;
   grid->selected = grid->scrollRow = grid->winStart = grid->winEnd = 0;
}

static void beginFeed(VideoGrid *grid)
{
   logInfo("[diag-grid] beginFeed grid=%p job=%d threadActive=%d\n", grid, grid->job, grid->threadActive);
   if (grid->job == JOB_CACHE) {   // results already in hand: show immediately, worker only fetches thumbnails
      applyMemberOnlyCacheRange(grid, 0, grid->results.count);
      grid->stage = GRID_READY;
      __sync_synchronize();
      grid->searchParsed = 1;
   }
   if (!spawnWorker(grid)) {
      logInfo("[diag-grid] beginFeed spawnWorker failed grid=%p\n", grid);
      grid->workerDone = 1;
      if (grid->job != JOB_CACHE) { grid->stage = GRID_EMPTY; grid->searchParsed = 1; }
   } else {
      logInfo("[diag-grid] beginFeed spawnWorker succeeded grid=%p tid=%u\n", grid, (unsigned)grid->workerTid);
   }
}

// (re)load the current source: restore pendingCached results instantly if set (worker then only refetches
// thumbnails), else fetch page 1 fresh. run either immediately (idle) or deferred after the worker is reaped.
static void applyDeferredSource(VideoGrid *grid)
{
   resetFeed(grid);
   if (grid->pendingCached) { grid->results = *grid->pendingCached; grid->job = JOB_CACHE; }
   else                       grid->job = JOB_LOAD;
   beginFeed(grid);
}

static void loadMore(VideoGrid *grid)
{
   grid->job = JOB_MORE;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// fetch the next band of thumbnails the window has scrolled toward (lazy, non-blocking)
static void loadThumbnails(VideoGrid *grid)
{
   grid->job = JOB_THUMBS;
   if (!spawnWorker(grid)) {
      int i = grid->thumbRequestIndex;
      if (i >= 0 && i < grid->results.count) grid->thumbs[i].state = THUMB_FAILED;
      grid->workerDone = 1;
   }
}

// Members-only checks run after thumbnail and paging work. The selected tile is checked first,
// followed by visible tiles ordered by distance. Off-screen checks are only eligible after the
// visible band is complete and navigation has been idle for MEMBER_IDLE_FRAMES.
static int hasVisibleMemberCheckInFlight(const VideoGrid *grid)
{
   int start = grid->scrollRow * grid->columns;
   int end = (grid->scrollRow + grid->visibleRows) * grid->columns;
   if (end > grid->results.count) end = grid->results.count;
   for (int i = start; i < end; i++) {
      if (grid->results.items[i].isPlaylist || grid->membersOnly[i] != GRID_MEMBER_UNKNOWN) continue;
      if (isMemberOnlyCheckInFlight(grid->results.items[i].videoId)) return 1;
   }
   return 0;
}

static int nextMemberCheck(VideoGrid *grid, int visibleOnly)
{
   int best = -1;
   int bestDistance = 0x7fffffff;

   if (visibleOnly && isTileVisible(grid, grid->selected)) {
      int i = grid->selected;
      if (!grid->results.items[i].isPlaylist && grid->membersOnly[i] == GRID_MEMBER_UNKNOWN) {
         const char *id = grid->results.items[i].videoId;
         if (isMemberOnlyIdCached(id)) grid->membersOnly[i] = GRID_MEMBER_YES;
         else if (!isMemberOnlyCheckInFlight(id)) return i;
      }
   }

   for (int i = 0; i < grid->results.count; i++) {
      int visible = isTileVisible(grid, i);
      if (visibleOnly != visible || grid->results.items[i].isPlaylist ||
          grid->membersOnly[i] != GRID_MEMBER_UNKNOWN) continue;
      const char *id = grid->results.items[i].videoId;
      if (isMemberOnlyIdCached(id)) {
         grid->membersOnly[i] = GRID_MEMBER_YES;
         continue;
      }
      if (isMemberOnlyCheckInFlight(id)) continue;
      int distance = i > grid->selected ? i - grid->selected : grid->selected - i;
      if (distance < bestDistance) {
         best = i;
         bestDistance = distance;
      }
   }
   return best;
}

static void loadMemberCheck(VideoGrid *grid, int index)
{
   if (index < 0 || index >= grid->results.count || grid->results.items[index].isPlaylist) return;
   int accountRevision = -1;
   const char *videoId = grid->results.items[index].videoId;
   if (!claimMemberOnlyCheck(videoId, &accountRevision)) return;
   grid->memberCheckIndex = index;
   grid->memberCheckAccountRevision = accountRevision;
   grid->membersOnly[index] = GRID_MEMBER_CHECKING;
   grid->job = JOB_MEMBER_CHECK;
   if (!spawnWorker(grid)) {
      grid->membersOnly[index] = GRID_MEMBER_UNKNOWN;
      releaseMemberOnlyCheck(videoId, accountRevision);
      grid->workerDone = 1;
   }
}

// join the non-empty metadata parts with " - " into "author - views - age".
static void composeMeta(char *out, int cap, const SearchResult *item)
{
   const char *parts[3] = { item->author, item->views, item->published };
   int length = 0;
   out[0] = 0;
   for (int i = 0; i < 3; i++) {
      if (!parts[i][0]) continue;
      length += snprintf(out + length, cap - length, "%s%s", length ? " \xe2\x80\xa2 " : "", parts[i]);
      if (length >= cap) break;
   }
}

// Show only the first source line. The existing single-line ellipsis label
// handles titles that are wider than the thumbnail.
static void formatGridTitle(VideoGrid *grid, const char *source, char *out, int cap, float scale)
{
   (void)grid;
   (void)scale;
   int length = 0;
   while (source[length] && source[length] != '\n' && source[length] != '\r' && length < cap - 1) {
      out[length] = source[length];
      length++;
   }
   out[length] = 0;
}

static void materializeTile(VideoGrid *grid, int i)
{
   char meta[128];
   const SearchResult *item = &grid->results.items[i];
   char title[LABEL_MAX_TEXT];
   formatGridTitle(grid, item->title, title, sizeof title, getGfxUiScaleFactor());
   setLabelText(&grid->rows[i], title);
   composeMeta(meta, sizeof meta, item);
   setLabelText(&grid->metas[i], meta);
   setLabelText(&grid->durations[i], item->isLive ? "LIVE" : item->duration);
}

static void dematerializeTile(VideoGrid *grid, int i)
{
   freeLabel(&grid->rows[i]);
   freeLabel(&grid->metas[i]);
   freeLabel(&grid->durations[i]);
   if (grid->thumbs[i].tex.offset) { freeGfxTexture(&grid->thumbs[i].tex); memset(&grid->thumbs[i].tex, 0, sizeof grid->thumbs[i].tex); }
}

// keep exactly the visible band (+ margin) materialised. safe without an RSX flush: a tile leaving the
// MARGIN_ROWS-wide window wasn't drawn last frame (the grid draws only the inner visible rows).
static void reconcileWindow(VideoGrid *grid)
{
   int start = (grid->scrollRow - MARGIN_ROWS) * grid->columns;
   int end   = (grid->scrollRow + grid->visibleRows + MARGIN_ROWS) * grid->columns;
   if (start < 0) start = 0;
   if (end > grid->results.count) end = grid->results.count;
   if (start == grid->winStart && end == grid->winEnd) return;

   int lo = start < grid->winStart ? start : grid->winStart;
   int hi = end   > grid->winEnd   ? end   : grid->winEnd;
   for (int i = lo; i < hi; i++) {
      int wasIn = (i >= grid->winStart && i < grid->winEnd);
      int nowIn = (i >= start && i < end);
      if      (nowIn && !wasIn) materializeTile(grid, i);
      else if (wasIn && !nowIn) dematerializeTile(grid, i);
   }
   grid->winStart = start;
   grid->winEnd   = end;
}

static void decodeWindowThumbnails(VideoGrid *grid)
{
   if (grid->decodeIndex >= 0) {
      ImageBuffer decoded = { 0, 0, 0, 0 };
      int rc = pollImageAsync(&decoded);
      if (rc == 0) return;

      int i = grid->decodeIndex;
      grid->decodeIndex = -1;
      if (i >= 0 && i < grid->results.count && grid->thumbs[i].state == THUMB_DECODING) {
         int row = i / grid->columns;
         int visible = row >= grid->scrollRow && row < grid->scrollRow + grid->visibleRows;
         if (rc > 0 && visible) {
            grid->thumbs[i].tex = uploadImageBuffer(&decoded);
            if (grid->thumbs[i].tex.offset) {
               grid->thumbs[i].state = THUMB_FETCHED;
               logInfo("[grid] thumbnail %s decoded and uploaded\n", grid->results.items[i].videoId);
            } else {
               grid->thumbs[i].state = THUMB_FAILED;
               logWarn("[grid] thumbnail %s texture upload failed\n", grid->results.items[i].videoId);
            }
         } else if (rc > 0) {
            // It scrolled away while decoding. Keep the JPEG and defer upload until visible again.
            grid->thumbs[i].state = THUMB_FETCHED;
         } else {
            grid->thumbs[i].state = THUMB_FAILED;
            logWarn("[grid] thumbnail %s JPEG decode failed\n", grid->results.items[i].videoId);
         }
      }
      if (rc > 0) freeImageBuffer(&decoded);
   }

   // Decode the selected tile first when its JPEG is ready, then the nearest ready visible tile.
   // Keep the global image-loader strictly serial: only one asynchronous decode may be outstanding.
   int next = -1;
   int selected = grid->selected;
   if (selected >= grid->winStart && selected < grid->winEnd &&
       isTileVisible(grid, selected) &&
       grid->thumbs[selected].state == THUMB_FETCHED &&
       grid->thumbs[selected].jpeg && !grid->thumbs[selected].tex.offset) {
      next = selected;
   } else {
      int bestDistance = 0x7fffffff;
      for (int i = grid->winStart; i < grid->winEnd; i++) {
         if (!isTileVisible(grid, i) || grid->thumbs[i].state != THUMB_FETCHED ||
             !grid->thumbs[i].jpeg || grid->thumbs[i].tex.offset) continue;
         int distance = i > selected ? i - selected : selected - i;
         if (distance < bestDistance) {
            next = i;
            bestDistance = distance;
         }
      }
   }

   if (next >= 0) {
      if (requestImageMemAsync(grid->thumbs[next].jpeg, (uint32_t)grid->thumbs[next].jpegLen) == 0) {
         grid->thumbs[next].state = THUMB_DECODING;
         grid->decodeIndex = next;
         logInfo("[grid] thumbnail %s decode queued\n", grid->results.items[next].videoId);
      } else {
         grid->thumbs[next].state = THUMB_FAILED;
         logWarn("[grid] thumbnail %s decode queue failed\n", grid->results.items[next].videoId);
      }
   }
}

// left/right step one tile and wrap onto the previous/next row when they exist; up/down step a whole row.
static void moveSelection(VideoGrid *grid, int delta)
{
   int target = grid->selected + delta;
   if (target >= 0 && target < grid->results.count) grid->selected = target;
}

// ---- public interface ----

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height)
{
   memset(grid, 0, sizeof *grid);
   grid->decodeIndex = -1;
   grid->thumbRequestIndex = -1;
   grid->font = font;
   grid->x = x; grid->y = y; grid->width = width; grid->height = height;
   int sdOutput = isGfxSdVideoOutput();
   // Keep the navigation grid at 3 columns on 16:9 and 2 columns on 4:3.
   // Three larger columns make the thumbnails readable from the couch.
   int fourThreeSd = sdOutput && getGfxScreenWidth() <= 1600;
   grid->columns = fourThreeSd ? 2 : GRID_COLS_WIDE;
   grid->gap = sdOutput ? 10 : GRID_GAP;
   // Reserve enough text space for the biggest 2.0x preset. This keeps grid
   // coordinates and thumbnail size fixed when L2 changes font size.
   grid->titleSize = sdOutput ? 15 : ROW_TITLE;
   grid->metaSize = sdOutput ? 11 : META_SIZE;
   grid->tileTextH = sdOutput
      ? 6 + grid->titleSize * 2 + 6 + grid->metaSize * 2 + 4
      : TILE_TEXT_H;
   grid->metaOffset = sdOutput ? grid->titleSize * 2 + 6 : ROW_TITLE + 8;

   int maxTileW = (width - (grid->columns - 1) * grid->gap) / grid->columns;
   // Fit exactly two navigation rows in the available vertical area at every
   // output resolution. The height constraint also prevents 720p layouts from
   // growing thumbnails so tall that the second row falls below the screen.
   int maxStep = height / 2;
   int maxThumbH = maxStep - grid->tileTextH - grid->gap;
   int widthFromHeight = maxThumbH > 0 ? maxThumbH * 16 / 9 : 1;
   if (maxTileW > widthFromHeight) maxTileW = widthFromHeight;
   if (maxTileW < 1) maxTileW = 1;
   grid->tileW = maxTileW;
   grid->thumbH = grid->tileW * 9 / 16;
   grid->rowStep = grid->thumbH + grid->tileTextH + grid->gap;
   grid->visibleRows = height / grid->rowStep;
   if (grid->visibleRows < 1) grid->visibleRows = 1;
   if (grid->visibleRows > 2) grid->visibleRows = 2;
   int usedWidth = grid->columns * grid->tileW + (grid->columns - 1) * grid->gap;
   grid->x = x + (width - usedWidth) / 2;

   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      initLabel(&grid->rows[i],      font, 0, 0, grid->tileW, AUTO, grid->titleSize, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->metas[i],     font, 0, 0, grid->tileW, AUTO, grid->metaSize, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->durations[i], font, 0, 0, AUTO,        AUTO, DUR_SIZE,       activeTheme->textPrimary,   TEXT_NOWRAP,          "");
      float uiScale = getGfxUiScaleFactor();
      if (uiScale < 1.0f) uiScale = 1.0f;
      if (sdOutput) {
         // Use a larger title at 1.5x and 2.0x. Metadata uses the same
         // rendered font size and is placed directly under the title.
         float titleScale = uiScale >= 1.40f ? 2.0f : 1.5f;
         int titleFontSize = (int)(grid->titleSize * titleScale / uiScale + 0.5f);
         grid->rows[i].size = titleFontSize;
         grid->metas[i].size = titleFontSize;
         grid->durations[i].size = (int)(DUR_SIZE * 1.5f / uiScale + 0.5f);
      }
      grid->rows[i].wrap = TEXT_NOWRAP_ELLIPSIS;
      setLabelScaleWithUi(&grid->rows[i], 1);
      setLabelScaleWithUi(&grid->metas[i], 1);
      setLabelScaleWithUi(&grid->durations[i], 1);
   }
   initLabel(&grid->watchLaterBadge, font, 0, 0, AUTO, AUTO, WATCHLATER_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Watch Later");
   setLabelScaleWithUi(&grid->watchLaterBadge, 1);
   initLabel(&grid->membersOnlyBadge, font, 0, 0, AUTO, AUTO, DUR_SIZE, 0xFF00FF00, TEXT_NOWRAP, "*Members only");
   float badgeScale = getGfxUiScaleFactor();
   if (badgeScale < 1.0f) badgeScale = 1.0f;
   if (sdOutput) grid->membersOnlyBadge.size = (int)(DUR_SIZE * 1.5f / badgeScale + 0.5f);
   setLabelScaleWithUi(&grid->membersOnlyBadge, 1);
   grid->memberCheckIndex = -1;
   grid->labelScale = getGfxUiScaleFactor();
}

void termVideoGrid(VideoGrid *grid)
{
   stopVideoGrid(grid);
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   freeLabel(&grid->watchLaterBadge);
   freeLabel(&grid->membersOnlyBadge);
}

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId)) { grid->isWatched = isWatched; }
void setGridWatchedPosition(VideoGrid *grid, int (*watchedPosition)(const char *videoId)) { grid->watchedPosition = watchedPosition; }
void setGridWatchLaterPredicate(VideoGrid *grid, int (*isWatchLater)(const char *videoId)) { grid->isWatchLater = isWatchLater; }

// both source-changes are non-blocking: if a load is in flight, ask it to stop and defer the swap until the
// reap in updateVideoGrid, so mode / category switches never freeze the frame on an in-flight fetch.
static void changeSource(VideoGrid *grid, GridFetchFn fetch, void *user, const SearchResults *cached)
{
   grid->fetch = fetch;
   grid->fetchUser = user;
   grid->pendingCached = cached;
   if (!grid->threadActive) { applyDeferredSource(grid); return; }
   grid->stopWorker = 1;
   grid->reloadPending = 1;
   grid->stage = GRID_RUNNING;
}

void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, NULL); }
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, cached); }

void stopVideoGrid(VideoGrid *grid)
{
   if (grid->threadActive) { grid->stopWorker = 1; joinThread(grid->workerTid); grid->threadActive = 0; }
   grid->reloadPending = 0;
   cancelGridDecode(grid);
}

void pauseVideoGrid(VideoGrid *grid) { grid->backgroundPaused = 1; }
void resumeVideoGrid(VideoGrid *grid) { grid->backgroundPaused = 0; }

void updateVideoGrid(VideoGrid *grid)
{
   // reap a finished worker; a deferred source swap then resets and starts the new feed
   if (grid->workerDone && grid->threadActive) {
      joinThread(grid->workerTid); grid->threadActive = 0;
      if (grid->reloadPending) { grid->reloadPending = 0; applyDeferredSource(grid); }
   }
   if (grid->stage != GRID_READY) return;

   reconcileWindow(grid);
   evictPlaylistThumbCache(grid);
   decodeWindowThumbnails(grid);

   if (!grid->backgroundPaused) {
      if      (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveSelection(grid, 1);
      else if (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_LEFT)))  moveSelection(grid, -1);
      if      (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_DOWN)))  moveSelection(grid, grid->columns);
      else if (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_UP)))    moveSelection(grid, -grid->columns);

      PadButtonState right = getPadButtonState(PAD_BTN_RIGHT);
      PadButtonState left  = getPadButtonState(PAD_BTN_LEFT);
      PadButtonState down  = getPadButtonState(PAD_BTN_DOWN);
      PadButtonState up    = getPadButtonState(PAD_BTN_UP);
      int navigating = right == PAD_BUTTON_STATE_PRESSED || right == PAD_BUTTON_STATE_HELD ||
                       left == PAD_BUTTON_STATE_PRESSED || left == PAD_BUTTON_STATE_HELD ||
                       down == PAD_BUTTON_STATE_PRESSED || down == PAD_BUTTON_STATE_HELD ||
                       up == PAD_BUTTON_STATE_PRESSED || up == PAD_BUTTON_STATE_HELD;
      if (navigating) grid->memberIdleFrames = 0;
      else if (grid->memberIdleFrames < MEMBER_IDLE_FRAMES) grid->memberIdleFrames++;
   } else {
      grid->memberIdleFrames = 0;
   }

   int selRow = grid->selected / grid->columns;
   if (selRow < grid->scrollRow)                            grid->scrollRow = selRow;
   else if (selRow >= grid->scrollRow + grid->visibleRows)  grid->scrollRow = selRow - grid->visibleRows + 1;

   // Rebuild thumbnail priority from the current viewport after every completed request. Only one
   // thumbnail request is in flight; a scroll during that request changes the very next item selected.
   if (!grid->backgroundPaused && !grid->threadActive && !grid->reloadPending) {
      int thumbnailIndex = nextVisibleThumbnail(grid);
      if (thumbnailIndex >= 0) {
         grid->thumbRequestIndex = thumbnailIndex;
         grid->thumbRequestEnd = thumbnailFetchEnd(grid);
         loadThumbnails(grid);
      } else if (grid->results.continuation[0] && grid->results.count < MAX_SEARCH_RESULTS &&
                 selRow >= ((grid->results.count - 1) / grid->columns) - 1) {
         loadMore(grid);
      } else if (!hasPendingVisibleThumbnailDecode(grid)) {
         int memberIndex = nextMemberCheck(grid, 1);
         if (memberIndex >= 0) {
            loadMemberCheck(grid, memberIndex);
         } else if (grid->memberIdleFrames >= MEMBER_IDLE_FRAMES &&
                    !hasVisibleMemberCheckInFlight(grid)) {
            // No visible member checks or thumbnail work remain; fill the cache gradually in idle time.
            memberIndex = nextMemberCheck(grid, 0);
            if (memberIndex >= 0) loadMemberCheck(grid, memberIndex);
         }
      }
   }
}

static void drawTile(VideoGrid *grid, int i)
{
   int row = i / grid->columns, col = i % grid->columns;
   int tx = grid->x + col * (grid->tileW + grid->gap);
   int ty = grid->y + (row - grid->scrollRow) * grid->rowStep;
   const SearchResult *item = &grid->results.items[i];
   int totalSeconds = 0, watchedSeconds = 0;
   int progressWidth = 0;
   if (!item->isPlaylist && !item->isLive && grid->watchedPosition && item->duration[0]) {
      int first = 0, second = 0, third = 0;
      int fields = sscanf(item->duration, "%d:%d:%d", &first, &second, &third);
      if (fields == 2) totalSeconds = first * 60 + second;
      else if (fields == 3) totalSeconds = first * 3600 + second * 60 + third;
      if (totalSeconds > 0) {
         watchedSeconds = grid->watchedPosition(item->videoId);
         if (watchedSeconds > 0) {
            if (watchedSeconds > totalSeconds) watchedSeconds = totalSeconds;
            progressWidth = (int)((float)grid->tileW * (float)watchedSeconds / (float)totalSeconds + 0.5f);
            if (progressWidth < 1) progressWidth = 1;
         }
      }
   }
   // A partially watched tile stays at full image brightness; only entries without a resume position
   // use the existing WATCHED dimming treatment. Completed videos store their duration as the position.
   int watched = progressWidth == 0 && !item->isPlaylist && !item->isLive &&
                 grid->isWatched && grid->isWatched(item->videoId);

   if (i == grid->selected) {
      int ring = activeTheme->focusThickness;
      fillGfxRectangleFixed(tx - ring, ty - ring, grid->tileW + 2 * ring, grid->thumbH + 2 * ring, activeTheme->focusBorder);
   }

   if (grid->thumbs[i].tex.offset)
      drawGfxTextureFixedSize(tx, ty, grid->tileW, grid->thumbH, grid->thumbs[i].tex, 0, 0, 1, 1, watched ? activeTheme->watchedThumbTint : 0xFFFFFFFF, GFX_FILTER_LINEAR);
   else
      fillGfxRectangleFixed(tx, ty, grid->tileW, grid->thumbH, activeTheme->surface);

   if (progressWidth > 0)
      fillGfxRectangleFixed(tx, ty + grid->thumbH - WATCH_PROGRESS_H, progressWidth, WATCH_PROGRESS_H,
                            WATCH_PROGRESS_COLOR);

   int padX = 6;
   int padY = 4;
   int edgeGap = 6;
   if (item->duration[0] || item->isLive) {
      int badgeW = grid->durations[i].tt.tex.w + 2 * padX;
      int badgeH = grid->durations[i].tt.tex.h + 2 * padY;
      int badgeX = tx + grid->tileW - badgeW - edgeGap, badgeY = ty + grid->thumbH - badgeH - edgeGap;
      fillGfxRectangleFixed(badgeX, badgeY, badgeW, badgeH, item->isLive ? activeTheme->accent : activeTheme->badgeFill);
      drawLabelAt(&grid->durations[i], badgeX + padX, badgeY + padY);
   }

   if (grid->membersOnly[i] == GRID_MEMBER_YES) {
      int badgeW = grid->membersOnlyBadge.tt.tex.w + 2 * padX;
      int badgeH = grid->membersOnlyBadge.tt.tex.h + 2 * padY;
      int badgeX = tx + edgeGap, badgeY = ty + grid->thumbH - badgeH - edgeGap;
      fillGfxRectangleFixed(badgeX, badgeY, badgeW, badgeH, 0xFF000000);
      drawLabelAt(&grid->membersOnlyBadge, badgeX + padX, badgeY + padY);
   }

   if (!item->isPlaylist && grid->isWatchLater && grid->isWatchLater(item->videoId)) {
      int badgeW = grid->watchLaterBadge.tt.tex.w + 2 * padX;
      int badgeH = grid->watchLaterBadge.tt.tex.h + 2 * padY;
      int badgeX = tx + grid->tileW - badgeW - edgeGap, badgeY = ty + 6;
      fillGfxRectangleFixed(badgeX, badgeY, badgeW, badgeH, activeTheme->badgeFill);
      drawLabelAt(&grid->watchLaterBadge, badgeX + padX, badgeY + padY);
   }

   int textY = ty + grid->thumbH + 6, alpha = watched ? WATCHED_ALPHA : 255;
   moveLabel(&grid->rows[i], tx, textY);
   drawLabelAlpha(&grid->rows[i], alpha);
   // Keep metadata directly below the title with no inter-line gap.
   moveLabel(&grid->metas[i], tx, textY + grid->rows[i].tt.tex.h);
   drawLabelAlpha(&grid->metas[i], alpha);
}

void drawVideoGrid(VideoGrid *grid)
{
   if (grid->stage != GRID_READY) return;

   // Re-rasterise only when L2 changes the preset, never from inside drawLabel.
   float scale = getGfxUiScaleFactor();
   if (scale != grid->labelScale) {
      float sizeScale = scale < 1.0f ? 1.0f : scale;
      for (int i = grid->winStart; i < grid->winEnd; i++) {
         if (isGfxSdVideoOutput()) {
            float titleScale = sizeScale >= 1.40f ? 2.0f : 1.5f;
            int titleFontSize = (int)(grid->titleSize * titleScale / sizeScale + 0.5f);
            grid->rows[i].size = titleFontSize;
            grid->metas[i].size = titleFontSize;
            grid->durations[i].size = (int)(DUR_SIZE * 1.5f / sizeScale + 0.5f);
         } else {
            grid->rows[i].size = grid->titleSize;
            grid->metas[i].size = grid->metaSize;
            grid->durations[i].size = DUR_SIZE;
         }
         grid->rows[i].wrap = TEXT_NOWRAP_ELLIPSIS;
         refreshLabelScale(&grid->rows[i]);
         refreshLabelScale(&grid->metas[i]);
         refreshLabelScale(&grid->durations[i]);
         char title[LABEL_MAX_TEXT];
         formatGridTitle(grid, grid->results.items[i].title, title, sizeof title, scale);
         setLabelText(&grid->rows[i], title);
      }
      grid->membersOnlyBadge.size = isGfxSdVideoOutput()
         ? (int)(DUR_SIZE * 1.5f / sizeScale + 0.5f) : DUR_SIZE;
      refreshLabelScale(&grid->watchLaterBadge);
      refreshLabelScale(&grid->membersOnlyBadge);
      grid->labelScale = scale;
   }

   for (int i = 0; i < grid->results.count; i++) {
      int row = i / grid->columns;
      if (row >= grid->scrollRow && row < grid->scrollRow + grid->visibleRows) drawTile(grid, i);
   }
}

GridStage            gridStage(const VideoGrid *grid)    { return grid->stage; }
int                  gridBusy(const VideoGrid *grid)     { return grid->threadActive && grid->job == JOB_MORE; }
int                  gridStopRequested(const VideoGrid *grid) { return grid->stopWorker; }
const SearchResults *gridResults(const VideoGrid *grid)  { return &grid->results; }
const SearchResult  *gridSelected(const VideoGrid *grid)
{
   return grid->results.count ? &grid->results.items[grid->selected] : NULL;
}
