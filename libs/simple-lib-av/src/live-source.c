// live-source - continuous fragmented-mp4 stream over a DASH live sq/<n> segment sequence (see live-source.h).
//
// A background prefetch thread fetches segments ahead into a rolling window, so the demuxer/decoder reading
// the window never blocks on a segment download - without it, playback stalls once per segment and then
// bursts to catch up. One lock guards the window (producer = prefetch, consumer = the reading decode thread).
//
// A segment always goes into the window whole (V1.1 Test10). The window used to be a fixed 8 MB that cut a
// segment short when it did not fit, and 1080p60 live has segments of up to 4 MB a 2 s segment and up to 10 MB a
// 5 s one (measured 2026-11-07): the demuxer then read the next segment's bytes as samples and a box size out of
// the middle of an mdat, and waited for good at an offset that never comes - the picture stopped while the sound
// played on (TEE's console, Test9). Now each segment is fetched straight into the window's free end (no second
// buffer), a video stream's window is sized at the open for the kept history, the segment being read and a whole
// one ahead (no growing it - and no copy of it - while the TEE Decoder holds 91 MB), and the prefetch fetches the
// next segment once there is room for it, or at once when the reader has begun the last one.

#include "live-source.h"
#include "http.h"
#include "string-utilities.h"   // strCopy
#include "thread.h"             // lock + thread helpers
#include "dbg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/sys_time.h>

#define WINDOW_VIDEO       (24 * 1024 * 1024)   // a video stream's window: history, the segment read, one ahead
                                                // (1 + 10 + 10 MB for 5 s segments of 1080p60)
#define WINDOW_AUDIO       (2 * 1024 * 1024)    // an audio stream's: its segments are ~80 KB
#define WINDOW_MAX         (40 * 1024 * 1024)   // a segment bigger than any before may grow the window up to this
#define WINDOW_BACKLOG     (1024 * 1024)        // history kept before the read position (the demuxer re-reads a box)
#define FETCH_MARGIN       (64 * 1024)          // room beyond a segment, so a full fetch is told from a cut one
#define TARGET_AHEAD       (8 * 1024 * 1024)    // how far ahead of the reader the prefetch keeps the window filled...
#define MAX_SEGMENTS_AHEAD 6                    // ...in at most this many whole segments the reader has not begun
                                                // (up to ~12 s at 2 s/segment; byte and window limits still apply)
#define SEGMENT_RING       16                   // where the latest segments begin, to count those ahead
#define START_BEHIND_EDGE  3                    // initial start position behind the current live edge
#define STARTUP_BUFFER_SEGMENTS 3                 // 3 x 2 s segments ~= 6 s; release reader only after priming
#define EDGE_POLL_MS       250                  // wait between retries for the next segment at the live edge
#define RECONNECT_MS       500                  // initial delay after a failed segment fetch
#define RECONNECT_MAX_MS   5000                 // capped exponential retry delay
#define MAX_FETCH_FAILURES 5                    // stop a source instead of retrying forever
#define PREFETCH_IDLE_MS   8                    // buffered far enough ahead: brief idle
#define READ_WAIT_MS       2                    // reader has nothing buffered yet: let the prefetch catch up
#define SPEC_FETCH_CAP     (12 * 1024 * 1024)    // bounded staging buffer for one speculative video segment

// googlevideo segment urls are signed, so any user-agent is accepted.
static const char *SEGMENT_UA = "Mozilla/5.0 (PLAYSTATION 3; 4.9)";

// Cross-stream fetch activity: audio and video have independent prefetch threads. Atomic counters
// expose whether one stream is occupying the transport while the other is starved.
static volatile int g_liveAudioFetches;
static volatile int g_liveVideoFetches;

struct LiveSource {
   char     base[2048];   // "<...>/" - append "sq/<n>"
   long     nextSq;       // next segment number to fetch (prefetch-thread owned)
   long     edgeSq;       // the live edge the source was opened with (an ended stream's last segment)
   int      ended;        // a stream that is over: edgeSq is the end
   volatile int finished; // ...and its last segment is in: a read past it is the end of the stream
   volatile long firstSq; // the segment the stream began with, -1 until it is in
   int      firstDone;    // the first (init-bearing) segment has been emitted
   int      startupReleased; // reader may consume data after the startup buffer is primed
   int      failStreak;   // consecutive failed fetches of the current segment, for log throttling
   int      forbiddenStreak;
   volatile int httpError; // HTTP response status (400+)
   volatile int sourceError; // terminal source error: HTTP status or -1 for transport failure

   uint8_t *window;       // rolling buffer of the virtual stream (guarded by lock; only the prefetch moves or grows it)
   int      windowCap;    // its size: WINDOW_VIDEO or WINDOW_AUDIO, up to WINDOW_MAX
   int      largest;      // the biggest segment so far (prefetch-thread owned)
   int      windowLen;
   uint64_t windowStart;  // virtual offset of window[0]
   uint64_t position;     // current read position (virtual)
   uint64_t segStart[SEGMENT_RING];   // virtual offsets the latest segments begin at...
   long     segSq[SEGMENT_RING];      // matching DASH sequence numbers, for read-side availability diagnostics
   int      segCount;     // ...and how many segments went in

   sys_lwmutex_t    lock;
   int              lockReady;
   sys_ppu_thread_t prefetchThread;
   int              threadActive;
   volatile int     running;
   volatile int     cancel;
   int              isAudio;
   uint8_t         *specBuffer;
   int              specCapacity;

   // Read-side starvation and producer-side fetch telemetry. Read counters share the window lock.
   uint64_t diagReadCalls, diagReadBytes, diagWaitEvents, diagWaitUs, diagWaitMaxUs;
   uint64_t diagEmptyPolls, diagMaxAvailable;
   uint64_t totalReadCalls, totalReadBytes, totalWaitEvents, totalWaitUs, totalWaitMaxUs, totalEmptyPolls;
   uint64_t diagLastReadReportUs, diagLastStallReportUs;
   uint64_t diagLastScheduleReportUs, diagLastFetchSuccessUs;
   uint64_t diagLastStartupWaitReportUs;
   uint64_t lastRequestEndUs, lastPublishUs;
   uint64_t totalRequestGapUs, totalRequestUs, totalRequestCount;
   uint64_t totalPublishDelayUs;
   volatile int fetchInFlight;
   uint64_t totalFetches, totalFetchBytes, totalFetchUs, totalFetchFailures, totalEdgeWaits;
};

// offset of the first 'moof' box (skipping a segment's leading ftyp/styp/sidx/moov/emsg). 0 if none found.
static int firstMoofOffset(const uint8_t *data, int length)
{
   uint32_t pos = 0;
   while (pos + 8 <= (uint32_t)length) {
      uint32_t size = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
      if (data[pos + 4] == 'm' && data[pos + 5] == 'o' && data[pos + 6] == 'o' && data[pos + 7] == 'f') return (int)pos;
      if (size < 8) break;
      pos += size;
   }
   return 0;
}

// drops the stream already read from the window's front, keeping WINDOW_BACKLOG before the read position (all of
// it when the reader is past the window's end). Caller holds the lock.
static void dropHistory(LiveSource *source)
{
   uint64_t keepFrom = source->position > source->windowStart + WINDOW_BACKLOG ? source->position - WINDOW_BACKLOG
                                                                               : source->windowStart;
   uint64_t drop = keepFrom - source->windowStart;
   if (drop > (uint64_t)source->windowLen) drop = (uint64_t)source->windowLen;
   if (drop == 0) return;
   memmove(source->window, source->window + drop, (size_t)((uint64_t)source->windowLen - drop));
   source->windowLen -= (int)drop;
   source->windowStart += drop;
}

// makes the window hold at least `need` bytes (in whole MB, up to WINDOW_MAX) - for a segment bigger than any
// before, which the open's size did not plan for. Under the lock, as the reader copies out of it. 1 ok, 0 too big
// or out of memory.
static int growWindow(LiveSource *source, uint64_t need)
{
   if (need <= (uint64_t)source->windowCap) return 1;
   if (need > WINDOW_MAX) return 0;
   uint64_t want = (need + 0xFFFFF) & ~(uint64_t)0xFFFFF;
   lock(&source->lock);
   uint8_t *bigger = (uint8_t *)realloc(source->window, (size_t)want);
   if (bigger) { source->window = bigger; source->windowCap = (int)want; }
   unlock(&source->lock);
   if (bigger) logInfo("[live] window now %d MB\n", (int)(want >> 20));
   return bigger != NULL;
}

// whole segments in the window that the reader has not begun (caller holds the lock)
static int segmentsAhead(const LiveSource *source)
{
   int ahead = 0, kept = source->segCount < SEGMENT_RING ? source->segCount : SEGMENT_RING;
   int first = source->segCount > SEGMENT_RING ? source->segCount - SEGMENT_RING : 0;
   for (int i = 0; i < kept; i++) {
      int slot = (first + i) % SEGMENT_RING;
      if (source->segStart[slot] >= source->position) ahead++;
   }
   return ahead;
}

typedef struct {
   char url[2200];
   uint8_t *buffer;
   int capacity;
   int length;
   int status;
   int result;
   long sq;
   sys_ppu_thread_t thread;
   uint64_t startedUs;
   uint64_t finishedUs;
   volatile int done;
} LiveSpecFetch;

static void liveSpecFetchThread(uint64_t arg)
{
   LiveSpecFetch *job = (LiveSpecFetch *)(uintptr_t)arg;
   HttpHeader headers[] = { { "User-Agent", SEGMENT_UA }, { "Accept-Encoding", "identity" } };
   char contentLength[24];
   HttpHeaderCapture whole = { "content-length", contentLength, sizeof contentLength };
   job->startedUs = sys_time_get_system_time();
   __sync_add_and_fetch(&g_liveVideoFetches, 1);
   job->result = fetchHttpCapturing("GET", job->url, headers, 2, NULL, 0,
                                    (char *)job->buffer, job->capacity, &job->length,
                                    &job->status, &whole);
   __sync_sub_and_fetch(&g_liveVideoFetches, 1);
   job->finishedUs = sys_time_get_system_time();
   __sync_synchronize();
   job->done = 1;   // last access to the stack-owned job before this worker exits
   exitThread();
}

static int liveSpecJoin(LiveSpecFetch *job)
{
   if (!job) return 0;
   int joinRc = joinThread(job->thread);
   if (joinRc == 0) return 1;
   logWarn("[live-prefetch-parallel] worker join failed rc=%d; waiting for worker completion\n", joinRc);
   while (!job->done) sleepMs(10);
   __sync_synchronize();
   return 1;
}

// fetch the next segment into the window while optionally fetching its successor into a bounded staging buffer;
// publish the staged successor only after the current segment, preserving sq order. 0 ok, 1 edge,
// 2 an ended stream's last segment is in, -1 error.
static int fetchNextSegment(LiveSource *source)
{
   if (source->ended && source->nextSq > source->edgeSq) return 2;
   char url[2200];
   // A manifest-derived base takes a path segment ("<base>sq/<n>"). The X-Head-Seqnum fallback in
   // youtube.c (used when the client gets no dashManifestUrl) hands over a query-style base that
   // already ends in "&sq=", where the number is appended directly instead.
   size_t baseLen = strlen(source->base);
   if (baseLen > 0 && source->base[baseLen - 1] == '=')
      snprintf(url, sizeof url, "%s%ld", source->base, source->nextSq);
   else
      snprintf(url, sizeof url, "%ssq/%ld", source->base, source->nextSq);
   HttpHeader headers[] = { { "User-Agent", SEGMENT_UA }, { "Accept-Encoding", "identity" } };
   char wholeText[24];
   HttpHeaderCapture whole = { "content-length", wholeText, sizeof wholeText };   // lowercase: see youtube.c

   int length = 0, status = 0, rc;
   uint8_t *at;
   uint64_t fetchStartUs = sys_time_get_system_time();
   if (!source->isAudio && source->firstDone) {
      int wantedSpecCapacity = source->largest + FETCH_MARGIN;
      if (wantedSpecCapacity > SPEC_FETCH_CAP) wantedSpecCapacity = SPEC_FETCH_CAP;
      if (wantedSpecCapacity > source->specCapacity) {
         uint8_t *bigger = (uint8_t *)realloc(source->specBuffer, (size_t)wantedSpecCapacity);
         if (bigger) {
            source->specBuffer = bigger;
            source->specCapacity = wantedSpecCapacity;
         } else if (!source->specBuffer) {
            logWarn("[live-prefetch-parallel] staging allocation failed; using sequential fetch\n");
         }
      }
   }
   LiveSpecFetch spec;
   int specStarted = 0;
   memset(&spec, 0, sizeof spec);
   for (;;) {
      // room at the window's end; the read history goes first when that end is short of the biggest segment yet
      // (of half the window before the first)
      lock(&source->lock);
      int wanted = source->largest > 0 ? source->largest + FETCH_MARGIN : source->windowCap / 2;
      if (source->windowCap - source->windowLen < wanted) dropHistory(source);
      at = source->window + source->windowLen;
      int room = source->windowCap - source->windowLen;
      uint64_t droppable = source->position > source->windowStart + WINDOW_BACKLOG
                         ? source->position - WINDOW_BACKLOG - source->windowStart : 0;
      if (droppable > (uint64_t)source->windowLen) droppable = (uint64_t)source->windowLen;
      uint64_t roomWithHistory = (uint64_t)room + droppable;
      uint64_t aheadNow = source->windowStart + (uint64_t)source->windowLen > source->position
                        ? source->windowStart + (uint64_t)source->windowLen - source->position : 0;
      int segmentsNow = segmentsAhead(source);
      int canSpeculate = !source->isAudio && source->firstDone &&
                         (!source->ended || source->nextSq < source->edgeSq) && source->specBuffer &&
                         source->largest > 0 &&
                         source->specCapacity >= (source->largest + FETCH_MARGIN > SPEC_FETCH_CAP
                            ? SPEC_FETCH_CAP : source->largest + FETCH_MARGIN) &&
                         aheadNow < TARGET_AHEAD && segmentsNow < MAX_SEGMENTS_AHEAD - 1 &&
                         roomWithHistory >= 2ull * ((uint64_t)source->largest + FETCH_MARGIN);
      unlock(&source->lock);

      if (!specStarted && canSpeculate) {
         size_t specBaseLen = strlen(source->base);
         if (specBaseLen > 0 && source->base[specBaseLen - 1] == '=')
            snprintf(spec.url, sizeof spec.url, "%s%ld", source->base, source->nextSq + 1);
         else
            snprintf(spec.url, sizeof spec.url, "%ssq/%ld", source->base, source->nextSq + 1);
         spec.buffer = source->specBuffer;
         spec.capacity = source->specCapacity;
         spec.sq = source->nextSq + 1;
         if (spawnJoinableThread(&spec.thread, liveSpecFetchThread, (uint64_t)(uintptr_t)&spec,
                                 THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "live-spec-fetch") == 0) {
            specStarted = 1;
            logInfo("[live-prefetch-parallel] start currentSq=%ld speculativeSq=%ld capKB=%d\n",
                    source->nextSq, spec.sq, spec.capacity / 1024);
         }
      }

      uint64_t requestStartUs = sys_time_get_system_time();
      uint64_t previousRequestEndUs = source->lastRequestEndUs;
      uint64_t requestGapUs = previousRequestEndUs && requestStartUs > previousRequestEndUs
                            ? requestStartUs - previousRequestEndUs : 0;
      source->fetchInFlight = 1;
      if (source->isAudio) __sync_add_and_fetch(&g_liveAudioFetches, 1);
      else __sync_add_and_fetch(&g_liveVideoFetches, 1);
      rc = fetchHttpCapturing("GET", url, headers, 2, NULL, 0, (char *)at, room, &length, &status, &whole);
      uint64_t requestEndUs = sys_time_get_system_time();
      source->fetchInFlight = 0;
      if (source->isAudio) __sync_sub_and_fetch(&g_liveAudioFetches, 1);
      else __sync_sub_and_fetch(&g_liveVideoFetches, 1);
      source->lastRequestEndUs = requestEndUs;
      source->totalRequestUs += requestEndUs - requestStartUs;
      source->totalRequestGapUs += requestGapUs;
      source->totalRequestCount++;
      logInfo("[live-fetch-cycle] type=%s sq=%ld result=%d status=%d bytes=%d requestMs=%llu completionGapMs=%llu activeAudio=%d activeVideo=%d\n",
              source->isAudio ? "audio" : "video", source->nextSq, rc, status, length,
              (unsigned long long)((requestEndUs-requestStartUs)/1000ull),
              (unsigned long long)(requestGapUs/1000ull), g_liveAudioFetches, g_liveVideoFetches);
      // fetchHttp stops at room - 1 without saying the body went on: a segment that did not fit is fetched
      // again into a window grown to its Content-Length (or twice what came where the reply has none)
      if (rc != 0 || (status != 200 && status != 206) || length < room - 1) break;
      uint64_t body = (uint64_t)atol(wholeText);
      uint64_t need = (uint64_t)source->windowLen + (body > (uint64_t)length ? body : 2 * (uint64_t)length) + FETCH_MARGIN;
      if (!growWindow(source, need)) {
         if (specStarted) liveSpecJoin(&spec);
         source->sourceError = -1;
         __sync_synchronize();
         logError("[live] sq/%ld cannot fit the rolling window (%d KB fetched, %d KB declared); stopping source\n",
                  source->nextSq, length / 1024, (int)(body / 1024));
         return -1;
      }
   }
   int specReady = 0;
   if (specStarted) {
      int specJoined = liveSpecJoin(&spec);
      source->totalRequestUs += spec.finishedUs - spec.startedUs;
      source->totalRequestCount++;
      specReady = specJoined && source->running && !source->cancel &&
                  rc == 0 && (status == 200 || status == 206) && length > 0 &&
                  spec.result == 0 && (spec.status == 200 || spec.status == 206) &&
                  spec.length > 0 && spec.length < spec.capacity - 1;
      logInfo("[live-prefetch-parallel] currentSq=%ld speculativeSq=%ld currentResult=%d currentStatus=%d speculativeResult=%d speculativeStatus=%d speculativeBytes=%d speculativeMs=%llu accepted=%d\n",
              source->nextSq, spec.sq, rc, status, spec.result, spec.status, spec.length,
              (unsigned long long)((spec.finishedUs > spec.startedUs ? spec.finishedUs-spec.startedUs : 0)/1000ull),
              specReady);
   }
   if (rc == 0 && (status == 204 || status == 404)) {
      source->failStreak = 0;
      source->forbiddenStreak = 0;
      // 204: not generated yet, we're at the live edge (googlevideo holds the request ~7.5 s first). A 404 behind
      // the edge is a segment the server no longer has - a rewind past its window (answered after ~8 s): before
      // anything of the stream went out, it begins at the live edge instead. Otherwise wait for the edge.
      if (status == 404 && !source->ended && !source->firstDone && source->nextSq < source->edgeSq - START_BEHIND_EDGE) {
         logWarn("[live] sq/%ld is gone (404): beginning at the live edge, sq/%ld\n", source->nextSq,
                 source->edgeSq - START_BEHIND_EDGE);
         source->nextSq = source->edgeSq - START_BEHIND_EDGE;
         return 0;   // fetch that one at once
      }
      uint64_t edgeElapsedUs = sys_time_get_system_time()-fetchStartUs;
      source->totalEdgeWaits++;
      logInfo("[live-fetch-diag] type=%s sq=%ld edgeWait status=%d rc=%d fetchMs=%llu nextSq=%ld edgeSq=%ld\n",
              source->isAudio ? "audio" : "video", source->nextSq, status, rc,
              (unsigned long long)(edgeElapsedUs/1000ull), source->nextSq, source->edgeSq);
      return 1;
   }
   if (rc != 0 || length <= 0 || (status != 200 && status != 206)) {
      if (status >= 400 && status < 500 && status != 403 && status != 404 &&
          status != 408 && status != 429) {
         source->httpError = status;
         source->sourceError = status;
         __sync_synchronize();
         logError("[live] non-retryable HTTP %d on sq/%ld; stopping source\n", status, source->nextSq);
         return -1;
      }
      if (status == 403) {
         source->forbiddenStreak++;
         if (source->forbiddenStreak >= 3) {
            source->httpError = 403;
            source->sourceError = 403;
            __sync_synchronize();
            logError("[live] persistent HTTP 403 on sq/%ld; stopping this source\n", source->nextSq);
            return -1;
         }
      } else {
         source->forbiddenStreak = 0;
      }
      source->totalFetchFailures++;
      source->failStreak++;
      if (source->failStreak == 1 || source->failStreak == MAX_FETCH_FAILURES)
         logError("[live-fetch-diag] type=%s sq=%ld failed rc=%d status=%d bytes=%d fetchMs=%llu failStreak=%d/%d\n",
                  source->isAudio ? "audio" : "video", source->nextSq, rc, status, length,
                  (unsigned long long)((sys_time_get_system_time()-fetchStartUs)/1000ull),
                  source->failStreak, MAX_FETCH_FAILURES);
      if (source->failStreak >= MAX_FETCH_FAILURES) {
         source->sourceError = status >= 400 ? status : -1;
         if (source->sourceError >= 400) source->httpError = source->sourceError;
         __sync_synchronize();
         logError("[live] source stopped after %d consecutive fetch failures (sq/%ld, error=%d)\n",
                  source->failStreak, source->nextSq, source->sourceError);
      }
      return -1;
   }
   source->failStreak = 0;
   source->forbiddenStreak = 0;
   source->httpError = 0;
   source->sourceError = 0;
   source->totalFetches++;
   source->totalFetchBytes += (uint64_t)length;
   source->totalFetchUs += sys_time_get_system_time() - fetchStartUs;
   source->diagLastFetchSuccessUs = sys_time_get_system_time();
   if (length > source->largest) source->largest = length;

   // every segment after the stream's first comes with the init again: from its first moof on
   int offset = source->firstDone ? firstMoofOffset(at, length) : 0;
   lock(&source->lock);
   if (offset > 0) memmove(at, at + offset, (size_t)(length - offset));
   source->segStart[source->segCount % SEGMENT_RING] = source->windowStart + (uint64_t)source->windowLen;
   source->segSq[source->segCount % SEGMENT_RING] = source->nextSq;
   source->segCount++;
   source->lastPublishUs = sys_time_get_system_time();
   source->totalPublishDelayUs += source->lastPublishUs - fetchStartUs;
   source->windowLen += length - offset;
   if (!source->firstDone) source->firstSq = source->nextSq;
   source->firstDone = 1;
   uint64_t bufferedNow = (uint64_t)source->windowLen;
   uint64_t aheadNow = source->windowStart + (uint64_t)source->windowLen > source->position
                     ? source->windowStart + (uint64_t)source->windowLen - source->position : 0;
   int windowCapNow = source->windowCap;
   unlock(&source->lock);
   logInfo("[live-fetch-diag] type=%s sq=%ld status=%d rc=%d bytes=%d payload=%d strip=%d fetchMs=%llu requestToPublishMs=%llu bufferedKB=%llu aheadKB=%llu windowKB=%d largestKB=%d\n",
           source->isAudio ? "audio" : "video", source->nextSq, status, rc, length, length-offset, offset,
           (unsigned long long)((sys_time_get_system_time()-fetchStartUs)/1000ull),
           (unsigned long long)((source->lastPublishUs-fetchStartUs)/1000ull),
           (unsigned long long)(bufferedNow/1024ull), (unsigned long long)(aheadNow/1024ull),
           windowCapNow/1024, source->largest/1024);
   source->nextSq++;

   // A speculative response is only published after its predecessor has been committed. It is
   // discarded on any primary failure, HTTP edge response, truncation, or sequence mismatch.
   if (specReady && spec.sq == source->nextSq && (!source->ended || source->nextSq <= source->edgeSq)) {
      int specOffset = firstMoofOffset(spec.buffer, spec.length);
      int specPayload = spec.length - specOffset;
      lock(&source->lock);
      dropHistory(source);
      uint64_t specNeed = (uint64_t)source->windowLen + (uint64_t)specPayload + FETCH_MARGIN;
      unlock(&source->lock);
      if (specNeed > (uint64_t)source->windowCap && !growWindow(source, specNeed)) {
         logWarn("[live-prefetch-parallel] speculative sq/%ld discarded: window growth failed\n", spec.sq);
      } else {
         lock(&source->lock);
         if (source->nextSq == spec.sq && source->windowCap - source->windowLen >= specPayload) {
            uint8_t *destination = source->window + source->windowLen;
            if (specOffset > 0) memmove(spec.buffer, spec.buffer + specOffset, (size_t)specPayload);
            memcpy(destination, spec.buffer, (size_t)specPayload);
            source->segStart[source->segCount % SEGMENT_RING] = source->windowStart + (uint64_t)source->windowLen;
            source->segSq[source->segCount % SEGMENT_RING] = spec.sq;
            source->segCount++;
            source->windowLen += specPayload;
            source->largest = spec.length > source->largest ? spec.length : source->largest;
            source->lastPublishUs = sys_time_get_system_time();
            source->totalPublishDelayUs += source->lastPublishUs - spec.startedUs;
            source->totalFetches++;
            source->totalFetchBytes += (uint64_t)spec.length;
            source->totalFetchUs += spec.finishedUs - spec.startedUs;
            source->diagLastFetchSuccessUs = spec.finishedUs;
            source->nextSq++;
            logInfo("[live-prefetch-parallel] published sq=%ld bytes=%d payload=%d aheadKB=%llu\n",
                    spec.sq, spec.length, specPayload,
                    (unsigned long long)((source->windowStart + source->windowLen > source->position
                       ? source->windowStart + source->windowLen - source->position : 0) / 1024ull));
         } else {
            logWarn("[live-prefetch-parallel] speculative sq/%ld discarded: sequence or window changed\n", spec.sq);
         }
         unlock(&source->lock);
      }
   }
   return 0;
}

static void prefetchThread(uint64_t arg)
{
   LiveSource *source = (LiveSource *)(uintptr_t)arg;
   while (source->running && !source->cancel && !source->sourceError) {
      lock(&source->lock);
      uint64_t end = source->windowStart + (uint64_t)source->windowLen;
      uint64_t ahead = end > source->position ? end - source->position : 0;
      int segments = segmentsAhead(source);
      // the room a fetch would have: the free end and the history it may drop
      uint64_t droppable = source->position > source->windowStart + WINDOW_BACKLOG
                         ? source->position - WINDOW_BACKLOG - source->windowStart : 0;
      if (droppable > (uint64_t)source->windowLen) droppable = (uint64_t)source->windowLen;
      uint64_t room = (uint64_t)(source->windowCap - source->windowLen) + droppable;
      int primed = source->firstDone;
      int startupReleased = source->startupReleased;
      uint64_t nowUs = sys_time_get_system_time();
      int reportSchedule = !source->diagLastScheduleReportUs || nowUs-source->diagLastScheduleReportUs >= 1000000ull;
      uint64_t sinceFetchMs = source->diagLastFetchSuccessUs ? (nowUs-source->diagLastFetchSuccessUs)/1000ull : 0;
      long nextSq = source->nextSq;
      int largest = source->largest;
      int windowLen = source->windowLen;
      int windowCap = source->windowCap;
      int activeAudioFetches = g_liveAudioFetches;
      int activeVideoFetches = g_liveVideoFetches;
      unlock(&source->lock);

      // Once the reader has begun the last segment in, the next one comes at once (a whole one ahead is what
      // carries 5 s segments of 10 MB). Otherwise up to TARGET_AHEAD in at most MAX_SEGMENTS_AHEAD segments - and
      // only when one as big as the biggest so far fits: the reader frees the room as it goes.
      // Startup has a segment-count requirement independent of byte size. A single large first
      // video segment can exceed TARGET_AHEAD; stopping here leaves readLiveSource waiting forever
      // for three segments while this producer refuses to fetch the second one.
      int startupNeedsSegments = !startupReleased && segments < STARTUP_BUFFER_SEGMENTS;
      int fetch = !primed || startupNeedsSegments || segments == 0 ||
                  (ahead < TARGET_AHEAD && segments < MAX_SEGMENTS_AHEAD &&
                   room >= (uint64_t)largest + FETCH_MARGIN);
      if (reportSchedule) {
         source->diagLastScheduleReportUs = nowUs;
         logInfo("[live-schedule-diag] type=%s action=%s reason=%s nextSq=%ld primed=%d aheadKB=%llu segmentsAhead=%d roomKB=%llu windowKB=%d/%d largestKB=%d sinceFetchMs=%llu activeAudio=%d activeVideo=%d edgeSq=%ld\n",
                 source->isAudio ? "audio" : "video", fetch ? "fetch" : "wait",
                 fetch ? (!primed ? "initial" : startupNeedsSegments ? "startup-buffer" :
                          segments == 0 ? "reader-entered-last-segment" : "buffer-below-target") :
                 ahead >= TARGET_AHEAD ? "target-ahead-reached" :
                 segments >= MAX_SEGMENTS_AHEAD ? "segment-count-limit" :
                 room < (uint64_t)largest + FETCH_MARGIN ? "window-room" : "idle",
                 nextSq, primed, (unsigned long long)(ahead/1024ull), segments,
                 (unsigned long long)(room/1024ull), windowLen/1024, windowCap/1024,
                 largest/1024, (unsigned long long)sinceFetchMs, activeAudioFetches, activeVideoFetches, source->edgeSq);
      }
      if (!fetch) { sleepMs(PREFETCH_IDLE_MS); continue; }

      int rc = fetchNextSegment(source);
      if (rc == 2) {                              // an ended stream is all in: the reader ends after it
         __sync_synchronize();
         source->finished = 1;
         logInfo("[live] the ended stream's last segment, sq/%ld, is in\n", source->edgeSq);
         break;
      }
      if (rc == 1) sleepMs(EDGE_POLL_MS);        // at the edge, wait then retry the same sq (it's live, more is coming)
      else if (rc < 0) {
         unsigned delay = RECONNECT_MS;
         for (int retry = 1; retry < source->failStreak && delay < RECONNECT_MAX_MS; retry++)
            delay = delay > RECONNECT_MAX_MS / 2 ? RECONNECT_MAX_MS : delay * 2;
         if (delay > RECONNECT_MAX_MS) delay = RECONNECT_MAX_MS;
         for (unsigned waited = 0; waited < delay && source->running &&
              !source->cancel && !source->sourceError; waited += 50) sleepMs(50);
      }
   }
   exitThread();
}

LiveSource *openLiveSource(const char *baseUrl, long startSq, long edgeSq, long playSq, int ended)
{
   LiveSource *source = (LiveSource *)calloc(1, sizeof *source);
   if (!source) return NULL;
   // googlevideo urls name the stream's type ("mime=audio%2Fmp4" as a parameter, ".../mime/audio%2Fmp4/..." in a
   // manifest's path): an audio stream's segments need a fraction of a video stream's window
   int audio = strstr(baseUrl, "audio%2F") != NULL || strstr(baseUrl, "audio/mp4") != NULL;
   source->isAudio = audio;
   source->windowCap = audio ? WINDOW_AUDIO : WINDOW_VIDEO;
   source->window = (uint8_t *)malloc(source->windowCap);
   if (!source->window) { closeLiveSource(source); return NULL; }
   strCopy(source->base, sizeof source->base, baseUrl);
   long start = playSq >= 0 ? playSq : ended ? startSq : edgeSq - START_BEHIND_EDGE;
   if (ended && start > edgeSq) start = edgeSq;
   source->nextSq = start > startSq ? start : startSq;
   source->edgeSq = edgeSq;
   source->ended = ended;
   source->firstSq = -1;
   createLock(&source->lock);
   source->lockReady = 1;
   logInfo("[live] open %s sq %ld..%ld, starting at %ld%s\n", ended ? "ended stream" : "live", startSq, edgeSq,
           source->nextSq, playSq >= 0 ? " (asked for)" : "");

   source->running = 1;
   source->threadActive = spawnJoinableThread(&source->prefetchThread, prefetchThread, (uint64_t)(uintptr_t)source,
                          THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "live-fetch") == 0;
   if (!source->threadActive) { closeLiveSource(source); return NULL; }
   return source;
}

int64_t readLiveSource(LiveSource *source, void *buffer, uint64_t length)
{
   uint64_t waitStartUs = 0;
   for (;;) {
      if (source->cancel) return 0;
      __sync_synchronize();
      if (source->sourceError) return -1;
      int finished = source->finished;
      __sync_synchronize();
      int startupReleasedNow = 0;
      int startupPrimedSegments = 0;
      uint64_t startupAhead = 0;
      long startupEdgeSq = 0;
      lock(&source->lock);
      if (!source->startupReleased) {
         startupPrimedSegments = segmentsAhead(source);
         int startupReady = source->ended || source->finished || startupPrimedSegments >= STARTUP_BUFFER_SEGMENTS;
         if (startupReady) {
            source->startupReleased = 1;
            startupReleasedNow = 1;
            startupAhead = source->windowStart + (uint64_t)source->windowLen > source->position
                         ? source->windowStart + (uint64_t)source->windowLen - source->position : 0;
            startupEdgeSq = source->edgeSq;
         }
      }
      int waitingForStartup = !source->startupReleased;
      if (source->position < source->windowStart) { unlock(&source->lock); return -1; }
      if (waitingForStartup) {
         uint64_t nowUs = sys_time_get_system_time();
         int reportStartupWait = !source->diagLastStartupWaitReportUs ||
                                 nowUs - source->diagLastStartupWaitReportUs >= 2000000ull;
         uint64_t startupAvailable = source->windowStart + (uint64_t)source->windowLen > source->position
                                   ? source->windowStart + (uint64_t)source->windowLen - source->position : 0;
         int startupSegmentsAhead = startupPrimedSegments;
         long startupNextSq = source->nextSq;
         long startupEdge = source->edgeSq;
         if (reportStartupWait) source->diagLastStartupWaitReportUs = nowUs;
         unlock(&source->lock);
         if (reportStartupWait)
            logWarn("[live-startup-wait] type=%s waitingSegments=%d requiredSegments=%d availableKB=%llu nextSq=%ld edgeSq=%ld\n",
                    source->isAudio ? "audio" : "video", startupSegmentsAhead, STARTUP_BUFFER_SEGMENTS,
                    (unsigned long long)(startupAvailable / 1024ull), startupNextSq, startupEdge);
         sleepMs(READ_WAIT_MS);
         continue;
      }
      uint64_t available = source->windowStart + source->windowLen > source->position
                         ? source->windowStart + source->windowLen - source->position : 0;
      if (available > 0) {
         uint64_t take = length < available ? length : available;
         memcpy(buffer, source->window + (source->position - source->windowStart), take);
         source->position += take;
         source->diagReadCalls++;
         source->diagReadBytes += take;
         source->totalReadCalls++;
         source->totalReadBytes += take;
         if (available > source->diagMaxAvailable) source->diagMaxAvailable = available;
         uint64_t nowUs = sys_time_get_system_time();
         if (waitStartUs) {
            uint64_t waited = nowUs - waitStartUs;
            source->diagWaitEvents++;
            source->diagWaitUs += waited;
            if (waited > source->diagWaitMaxUs) source->diagWaitMaxUs = waited;
            source->totalWaitEvents++;
            source->totalWaitUs += waited;
            if (waited > source->totalWaitMaxUs) source->totalWaitMaxUs = waited;
            source->diagLastStallReportUs = 0;
         }
         uint64_t ahead = source->windowStart + source->windowLen > source->position
                        ? source->windowStart + source->windowLen - source->position : 0;
         int doReport = !source->diagLastReadReportUs || nowUs - source->diagLastReadReportUs >= 2000000ull;
         uint64_t reportCalls=0, reportBytes=0, reportWaits=0, reportWaitUs=0, reportWaitMax=0, reportEmpty=0, reportMaxAvail=0;
         uint64_t pos=source->position, winStart=source->windowStart, winLen=(uint64_t)source->windowLen;
         long edgeSq=source->edgeSq;
         int httpError=source->httpError;
         int readSegmentsAhead=segmentsAhead(source);
         long newestSq=source->segCount > 0 ? source->segSq[(source->segCount-1) % SEGMENT_RING] : -1;
         uint64_t publishAgeMs=source->lastPublishUs && nowUs >= source->lastPublishUs ? (nowUs-source->lastPublishUs)/1000ull : 0;
         int activeAudioFetches=g_liveAudioFetches, activeVideoFetches=g_liveVideoFetches;
         if (doReport) {
            source->diagLastReadReportUs = nowUs;
            reportCalls=source->diagReadCalls; reportBytes=source->diagReadBytes;
            reportWaits=source->diagWaitEvents; reportWaitUs=source->diagWaitUs; reportWaitMax=source->diagWaitMaxUs;
            reportEmpty=source->diagEmptyPolls; reportMaxAvail=source->diagMaxAvailable;
            source->diagReadCalls=source->diagReadBytes=source->diagWaitEvents=source->diagWaitUs=0;
            source->diagWaitMaxUs=source->diagEmptyPolls=source->diagMaxAvailable=0;
         }
         unlock(&source->lock);
         if (startupReleasedNow)
            logInfo("[live-startup-buffer] type=%s released segments=%d targetSegments=%d aheadKB=%llu edgeSq=%ld\n",
                    source->isAudio ? "audio" : "video", startupPrimedSegments, STARTUP_BUFFER_SEGMENTS,
                    (unsigned long long)(startupAhead/1024ull), startupEdgeSq);
         if (doReport)
            logInfo("[live-read-diag] type=%s spanMs=2000 calls=%llu bytes=%llu avgRead=%llu waitEvents=%llu waitMs=%llu waitMaxMs=%llu emptyPolls=%llu maxAvailableKB=%llu bufferedKB=%llu aheadKB=%llu positionKB=%llu windowStartKB=%llu nextSq=%ld segmentsAhead=%d newestSq=%ld publishAgeMs=%llu activeAudio=%d activeVideo=%d edgeSq=%ld httpError=%d\n",
                    source->isAudio ? "audio" : "video", (unsigned long long)reportCalls,
                    (unsigned long long)reportBytes, (unsigned long long)(reportCalls ? reportBytes/reportCalls : 0),
                    (unsigned long long)reportWaits, (unsigned long long)(reportWaitUs/1000ull),
                    (unsigned long long)(reportWaitMax/1000ull), (unsigned long long)reportEmpty,
                    (unsigned long long)(reportMaxAvail/1024ull), (unsigned long long)(winLen/1024ull),
                    (unsigned long long)(ahead/1024ull), (unsigned long long)(pos/1024ull),
                    (unsigned long long)(winStart/1024ull), source->nextSq, readSegmentsAhead, newestSq,
                    (unsigned long long)publishAgeMs, activeAudioFetches, activeVideoFetches, edgeSq, httpError);
         return (int64_t)take;
      }
      uint64_t emptyNowUs = sys_time_get_system_time();
      source->diagEmptyPolls++;
      source->totalEmptyPolls++;
      uint64_t emptyPosition = source->position;
      uint64_t emptyBuffered = (uint64_t)source->windowLen;
      uint64_t emptyAhead = source->windowStart + (uint64_t)source->windowLen > source->position
                          ? source->windowStart + (uint64_t)source->windowLen - source->position : 0;
      long emptyEdgeSq = source->edgeSq;
      int emptyHttpError = source->httpError;
      uint64_t emptyPolls = source->diagEmptyPolls;
      int emptySegmentsAhead = segmentsAhead(source);
      long emptyNewestSq = source->segCount > 0 ? source->segSq[(source->segCount-1) % SEGMENT_RING] : -1;
      uint64_t emptyPublishAgeMs = source->lastPublishUs && emptyNowUs >= source->lastPublishUs
                                 ? (emptyNowUs-source->lastPublishUs)/1000ull : 0;
      int emptyFetchInFlight = source->fetchInFlight;
      int activeAudioFetches = g_liveAudioFetches, activeVideoFetches = g_liveVideoFetches;
      unlock(&source->lock);
      if (startupReleasedNow)
         logInfo("[live-startup-buffer] type=%s released segments=%d targetSegments=%d aheadKB=%llu edgeSq=%ld\n",
                 source->isAudio ? "audio" : "video", startupPrimedSegments, STARTUP_BUFFER_SEGMENTS,
                 (unsigned long long)(startupAhead/1024ull), startupEdgeSq);
      if (finished) return 0;
      uint64_t nowUs = emptyNowUs;
      if (!waitStartUs) waitStartUs = nowUs;
      if (!source->diagLastStallReportUs) source->diagLastStallReportUs = waitStartUs;
      if (nowUs - source->diagLastStallReportUs >= 2000000ull) {
         source->diagLastStallReportUs = nowUs;
         logWarn("[live-read-stall] type=%s waitingMs=%llu emptyPolls=%llu bufferedKB=%llu aheadKB=%llu positionKB=%llu nextSq=%ld segmentsAhead=%d newestSq=%ld publishAgeMs=%llu fetchInFlight=%d activeAudio=%d activeVideo=%d edgeSq=%ld httpError=%d\n",
                 source->isAudio ? "audio" : "video", (unsigned long long)((nowUs-waitStartUs)/1000ull),
                 (unsigned long long)emptyPolls, (unsigned long long)(emptyBuffered/1024ull),
                 (unsigned long long)(emptyAhead/1024ull), (unsigned long long)(emptyPosition/1024ull),
                 source->nextSq, emptySegmentsAhead, emptyNewestSq, (unsigned long long)emptyPublishAgeMs,
                 emptyFetchInFlight, activeAudioFetches, activeVideoFetches, emptyEdgeSq, emptyHttpError);
      }
      sleepMs(READ_WAIT_MS);
   }
}

int seekLiveSource(LiveSource *source, uint64_t offset)
{
   lock(&source->lock);
   // not before the kept history; and not further past the window's end than a whole window - a demuxer that
   // jumps that far has read a box size out of the middle of the data, and waiting there would be for ever
   int ok = offset >= source->windowStart && offset <= source->windowStart + (uint64_t)source->windowLen + WINDOW_MAX;
   if (ok) source->position = offset;
   unlock(&source->lock);
   if (!ok && offset >= source->windowStart) logError("[live] a seek far past the stream's end refused\n");
   return ok ? 0 : -1;
}

uint64_t getLiveSourcePosition(const LiveSource *source) { return source->position; }

long getLiveSourceFirstSq(const LiveSource *source) { return source->firstSq >= 0 ? source->firstSq : source->nextSq; }
int getLiveSourceHttpError(const LiveSource *source) { return source ? source->httpError : 0; }
int getLiveSourceError(const LiveSource *source)
{
   if (!source) return 0;
   __sync_synchronize();
   return source->sourceError;
}

void cancelLiveSource(LiveSource *source) { if (source) source->cancel = 1; }

void closeLiveSource(LiveSource *source)
{
   if (!source) return;
   source->running = 0;
   source->cancel = 1;
   if (source->threadActive) {
      uint64_t joinStartUs = sys_time_get_system_time();
      int joinRc = joinThread(source->prefetchThread);
      uint64_t joinMs = (sys_time_get_system_time() - joinStartUs) / 1000ull;
      if (joinRc != 0) {
         logError("[live] prefetch join failed rc=%d after %llums; retaining source resources to avoid use-after-free\n",
                  joinRc, (unsigned long long)joinMs);
         return;
      }
      source->threadActive = 0;
      if (joinMs >= 2000)
         logWarn("[live] prefetch teardown waited %llums\n", (unsigned long long)joinMs);
   }
   if (source->lockReady) {
      lock(&source->lock);
      logInfo("[live-final-diag] type=%s readCalls=%llu readBytes=%llu waitEvents=%llu waitMs=%llu waitMaxMs=%llu emptyPolls=%llu positionKB=%llu windowStartKB=%llu bufferedKB=%llu nextSq=%ld edgeSq=%ld failStreak=%d httpError=%d fetches=%llu fetchMB=%llu fetchAvgMs=%llu fetchFailures=%llu edgeWaits=%llu requestAvgMs=%llu completionGapAvgMs=%llu publishDelayAvgMs=%llu\n",
              source->isAudio ? "audio" : "video", (unsigned long long)source->totalReadCalls,
              (unsigned long long)source->totalReadBytes, (unsigned long long)source->totalWaitEvents,
              (unsigned long long)(source->totalWaitUs/1000ull), (unsigned long long)(source->totalWaitMaxUs/1000ull),
              (unsigned long long)source->totalEmptyPolls, (unsigned long long)(source->position/1024ull),
              (unsigned long long)(source->windowStart/1024ull), (unsigned long long)((uint64_t)source->windowLen/1024ull),
              source->nextSq, source->edgeSq, source->failStreak, source->httpError,
              (unsigned long long)source->totalFetches, (unsigned long long)(source->totalFetchBytes/(1024ull*1024ull)),
              (unsigned long long)(source->totalFetches ? source->totalFetchUs/source->totalFetches/1000ull : 0),
              (unsigned long long)source->totalFetchFailures, (unsigned long long)source->totalEdgeWaits,
              (unsigned long long)(source->totalRequestCount ? source->totalRequestUs/source->totalRequestCount/1000ull : 0),
              (unsigned long long)(source->totalRequestCount ? source->totalRequestGapUs/source->totalRequestCount/1000ull : 0),
              (unsigned long long)(source->totalFetches ? source->totalPublishDelayUs/source->totalFetches/1000ull : 0));
      unlock(&source->lock);
      destroyLock(&source->lock);
      source->lockReady = 0;
   }
   free(source->specBuffer);
   free(source->window);
   free(source);
}
