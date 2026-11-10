#pragma once

// video-grid - a reusable, pad-navigable grid of video results. Owns the whole display engine: a background
// worker that fetches result pages + thumbnail jpegs, windowed materialisation (only the on-screen band of
// tiles holds a decoded thumbnail texture + rasterised labels, so VRAM stays flat at any scroll depth),
// 2D D-pad navigation with hold-to-scroll, and infinite scroll. Screens supply a GridFetchFn (which encodes
// the mode: trending / search / channel + sort) and read back the selection; the grid knows nothing about
// modes. See search.c / home.c for callers.

#include "extractor.h"          // SearchResults / SearchResult
#include "font.h"               // Font
#include "ui/label.h"           // Label
#include "button-repeat.h"      // ButtonRepeat
#include "thread.h"             // sys_ppu_thread_t
#include <stdint.h>

#define GRID_COLS_WIDE 3
#define GRID_THREADS  4         // bounded parallel thumbnail requests; BearSSL has a per-connection pool

typedef enum { GRID_RUNNING, GRID_READY, GRID_EMPTY } GridStage;

// what the background worker is doing this spawn. LOAD/CACHE fetch page 1 (CACHE restores kept results first);
// MORE appends the next continuation page; THUMBS fetches one item selected from the live viewport priority.
typedef enum { JOB_LOAD, JOB_CACHE, JOB_MORE, JOB_THUMBS, JOB_MEMBER_CHECK } GridJob;

typedef enum {
   GRID_MEMBER_UNKNOWN,
   GRID_MEMBER_CHECKING,
   GRID_MEMBER_YES,
   GRID_MEMBER_NO,
   GRID_MEMBER_FAILED
} GridMemberState;

// fetch one page into out: token NULL = first page, else a continuation / sort token. 0 ok, negative error.
typedef int (*GridFetchFn)(const char *token, SearchResults *out, void *user);

// one thumbnail: jpeg fetched by the worker and kept; the texture is decoded only while the tile is in the
// visible window (tex.offset != 0) and freed when it scrolls away, re-decoding from the kept jpeg on return.
typedef enum { THUMB_NONE, THUMB_FETCHED, THUMB_DECODING, THUMB_FAILED } ThumbState;
typedef struct {
   volatile ThumbState state;
   uint8_t   *jpeg;
   int        jpegLen;
   GfxTexture tex;
} GridThumb;

typedef struct {
   Font            *font;
   int              x, y, width, height;                   // grid rectangle on screen
   int              columns, gap, tileTextH, metaOffset;   // mode-specific layout
   int              titleSize, metaSize;                   // base label sizes; SD labels rasterise larger
   float            labelScale;                            // last scale used to rasterise visible tile labels
   int              tileW, thumbH, rowStep, visibleRows;   // derived geometry

   SearchResults    results;
   GridThumb        thumbs[MAX_SEARCH_RESULTS];
   Label            rows[MAX_SEARCH_RESULTS];              // per-tile title
   Label            metas[MAX_SEARCH_RESULTS];             // per-tile "author - views - age"
   Label            durations[MAX_SEARCH_RESULTS];         // per-tile duration / LIVE badge
   Label            watchLaterBadge;                       // shared "Watch Later" tag drawn on queued tiles
   Label            membersOnlyBadge;                       // shared "*Members only" tag drawn on restricted tiles
   volatile GridMemberState membersOnly[MAX_SEARCH_RESULTS];
   int              memberCheckIndex;
   int              memberCheckAccountRevision;
   int              memberIdleFrames;
   int              suppressWatchLaterBadge;
   int              sortMode;                           // local result ordering: relevance, views, or newest
   int              playlistSortMode;                   // playlist ordering: newest or A-Z
   int              sortPendingFrom;                      // first item from a page awaiting UI-thread merge

   int              selected, scrollRow, winStart, winEnd;
   int              inputSuppressed, focusHidden;
   ButtonRepeat     horizontalRepeat, verticalRepeat;

   // background worker + thumbnail fetch cursor
   volatile GridStage stage;
   volatile int     searchParsed, workerDone, stopWorker;
   GridJob          job;
   int              thumbRequestIndex;                     // first thumbnail index selected by the priority scheduler
   int              thumbRequestEnd;                       // exclusive end of the current viewport/lookahead batch
   int              decodeIndex;                           // thumbnail currently decoding asynchronously, or -1
   int              threadActive, reloadPending;
   volatile int     backgroundPaused;
   sys_ppu_thread_t workerTid;
   volatile int     thumbNext, thumbEnd;          // bounded range for the single active thumbnail worker

   GridFetchFn      fetch;
   void            *fetchUser;
   const SearchResults *pendingCached;                     // set = next (re)load restores these instantly
   int              (*isWatched)(const char *videoId);     // fade predicate (NULL = no fade)
   int              (*watchedPosition)(const char *videoId); // online VOD resume position
   int              (*downloadedPosition)(const char *videoId); // downloaded-file resume position
   int              (*isWatchLater)(const char *videoId);  // video Watch Later badge predicate (NULL = no badge)
   int              (*isPlaylistWatchLater)(const char *playlistId); // playlist Watch Later badge predicate
} VideoGrid;

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height);
void termVideoGrid(VideoGrid *grid);

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId));
void setGridWatchedPosition(VideoGrid *grid, int (*watchedPosition)(const char *videoId));
void setGridDownloadedPosition(VideoGrid *grid, int (*downloadedPosition)(const char *videoId));
void setGridWatchLaterPredicate(VideoGrid *grid, int (*isWatchLater)(const char *videoId));
void setGridPlaylistWatchLaterPredicate(VideoGrid *grid, int (*isWatchLater)(const char *playlistId));
void setGridSortMode(VideoGrid *grid, SortOrder mode);
void setGridPlaylistSortMode(VideoGrid *grid, PlaylistSortOrder mode);
void setGridSortByPublished(VideoGrid *grid, int enabled); // compatibility wrapper for Newest
void setGridInputSuppressed(VideoGrid *grid, int suppressed);
void setGridFocusHidden(VideoGrid *grid, int hidden);

// load a fresh feed from page 1. non-blocking: if a load is already running it's swapped once that finishes,
// so mode/sort switches never freeze the frame.
void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user);
// restore already-known results instantly (grid then only re-fetches thumbnails); fetch/user drive paging.
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user);

void updateVideoGrid(VideoGrid *grid);   // nav + scroll + window + decode + infinite scroll; call each frame
void drawVideoGrid(VideoGrid *grid);
void stopVideoGrid(VideoGrid *grid);     // block until the worker stops (before playback / teardown)
void pauseVideoGrid(VideoGrid *grid);     // let the active job finish, but do not schedule another
void resumeVideoGrid(VideoGrid *grid);

GridStage            gridStage(const VideoGrid *grid);
int                  gridBusy(const VideoGrid *grid);       // a fetch worker is running (results may still be growing)
int                  gridStopRequested(const VideoGrid *grid); // a source swap asked the worker to stop (bail long fetches)
const SearchResult  *gridSelected(const VideoGrid *grid);   // NULL if empty
const SearchResults *gridResults(const VideoGrid *grid);    // for caching + channel sort tokens
