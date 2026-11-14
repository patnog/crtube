#pragma once

// downloads - background video downloads. Square/R3 on a grid queues a video; a single low-priority worker
// drains the queue one at a time, remuxing the same H.264 + AAC streams the player would show into one
// .mp4, then handing it to video-export so it turns up in the XMB's Video column. The active download's
// progress shows at the lower-left below the browse-screen button hints. Everything runs off the UI
// thread; the queue is cancelled and cleared on app exit.

#include <stdint.h>
#include "extractor.h"   // SearchResult / CaptionTrack
#include "chapters.h"

// Startup-only, bounded initialization: locks + overlay labels. Must not touch HDD;
// the library worker is started lazily by requestDownloadManagementRefresh().
void initDownloads(void);       // call after initFont
void shutdownDownloads(void);   // cancel + clear the queue, join the worker, free the overlay; call at exit

#define DOWNLOAD_MANAGEMENT_MAX_ITEMS 256
#define DOWNLOAD_MANAGEMENT_TITLE_LEN 160
#define DOWNLOAD_MANAGEMENT_VISIBLE_ITEMS 7

typedef enum {
   DOWNLOAD_MANAGEMENT_ONGOING = 0,
   DOWNLOAD_MANAGEMENT_COMPLETED = 1,
   DOWNLOAD_MANAGEMENT_FAILED = 2
} DownloadManagementCategory;
typedef struct {
   char videoId[16];
   char title[DOWNLOAD_MANAGEMENT_TITLE_LEN];
   int percent;
   int active;
   int cancelling;
   int finalizing;
   int failed;
   uint64_t modifiedAt;
} DownloadManagementItem;

// Lazy library worker: all HDD enumeration, sidecar reads, migration and deletes
// happen off the UI thread. Getters below only copy the in-memory cache.
void requestDownloadManagementRefresh(void);
int getDownloadManagementItems(int category, int offset, DownloadManagementItem *out,
                               int capacity, int *total, int *loading);
int getDownloadManagementIds(int category, char (*out)[16], int capacity);
int queueDeleteManagedDownloads(const char *ids[], int count);
int getDownloadLibraryStats(uint64_t *usedBytes, uint64_t *freeBytes, int *ready);
// 0 = accepted, 1 = not found, -1 = already finalizing / cannot cancel.
int cancelManagedDownload(const char *videoId);

void enqueueDownload(const SearchResult *item);   // queue using global download preferences
void enqueuePlaybackDownload(const SearchResult *item, const char *audioTrackId,
                             const char *activeSubtitleLanguage);
int getDownloadedVideos(SearchResults *out, int sortAZ);
unsigned int getDownloadLibraryRevision(void);
int deleteDownloadedVideo(const char *videoId);
int getDownloadedSubtitleTracks(const char *videoPath, CaptionTrack *out, int capacity);
int loadDownloadedChapters(const char *videoPath, ChapterList *chapters);
int getDownloadedVideoPath(const char *videoId, char *path, int cap);
int loadDownloadedMetadata(const char *videoPath, char *title, int titleCap,
                           char *author, int authorCap, char *description, int descriptionCap,
                           char *publishedDate, int publishedCap,
                           char *viewCount, int viewsCap, char *likeCount, int likesCap);

int isDownloadSpacePromptOpen(void);
void updateDownloadOverlay(void);         // refresh the "Downloading ..." label; call each frame in a screen update
void drawDownloadOverlay(int screenWidth); // draw progress and the short enqueue toast; call in a screen draw
int consumeDownloadCompletion(void); // consume a completion event on the UI thread
