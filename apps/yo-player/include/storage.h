#pragma once

// storage - tiny persistent preferences + watch history, kept as plain text files under
// /dev_hdd0/tmp/crtube/ (via the VFS layer). No user/account binding: back up or move the folder and
// anyone can restore it. initStorage() must run once at startup before the getters are used.

#include "extractor.h"   // SearchResult / SearchResults (watch-later stores full entries)

void initStorage(void);

#define MAX_SEARCH_HISTORY 32
#define SEARCH_QUERY_LEN   128

// Search queries are stored newest-first. Repeating a query moves it to the front; the oldest falls off at 32.
int  getSearchHistory(char out[][SEARCH_QUERY_LEN], int max);
void addSearchHistory(const char *query);
void clearSearchHistory(void);

// PS3-local playback history: 50 newest-first video entries, persisted independently from watch/resume state.
int  getLocalHistory(SearchResults *out);
void recordLocalHistory(const char *videoId, const char *title, const char *author);

// watch history: videoIds the user has played. Used to fade already-watched tiles in the grid.
int  isWatched(const char *videoId);
void markWatched(const char *videoId);

// resume: last playback position (seconds) per watched video, so playback picks up where it left off.
// getWatchedPosition returns 0 if unknown; a finished video stores its duration, which the player
// recognizes as too close to the end to resume.
int  getWatchedPosition(const char *videoId);
int  getWatchedDuration(const char *videoId);
void setWatchedPosition(const char *videoId, int seconds);
void setWatchedDuration(const char *videoId, int durationSeconds);

// Downloaded-file playback progress is stored separately from online VOD watch history.
int  getDownloadedPosition(const char *videoId);
void setDownloadedProgress(const char *videoId, int positionSeconds, int durationSeconds);

#define CHANNEL_ID_LEN    32
#define MAX_SUBSCRIPTIONS 128

// subscribed channel ids, seeded to subscriptions.txt on first run. fills ids[0..count-1], returns the count.
int  getSubscriptions(char ids[][CHANNEL_ID_LEN], int max);

// subscribe toggle: isSubscribed reports the current state; setSubscribed adds or removes the channel and
// rewrites subscriptions.txt immediately. Only real "UC..." ids are stored; duplicates never occur.
int  isSubscribed(const char *channelId);
void setSubscribed(const char *channelId, int subscribed);

// bumped whenever setSubscribed actually changes the set. Screens that cache the subscriptions feed compare
// this against the value they last loaded at, and refetch when it moved.
int  getSubscriptionsRevision(void);

// watch-later: a saved queue of full video entries (watchlater.txt), also shown as its own home category.
// isWatchLater powers the tile badge; toggleWatchLater queues the video (its metadata is kept so the
// category renders without a re-fetch) or removes it if already queued; getWatchLater fills a feed
// newest-first. getWatchLaterRevision bumps on every real change so cached feeds know to refetch.
int  isWatchLater(const char *videoId);
void toggleWatchLater(const SearchResult *item);
int  getWatchLater(SearchResults *out);
int  getWatchLaterRevision(void);

// PS3 playlist library. Watch Later stores YouTube playlist references; PS3 stores local video lists.
#define LOCAL_PLAYLIST_NAME_LEN 64
#define LOCAL_PLAYLIST_ID_LEN   16
int  isPlaylistWatchLater(const char *playlistId);
int  addPlaylistToWatchLater(const SearchResult *playlist);
int  getPlaylistWatchLater(SearchResults *out);
int  getLocalPlaylistCount(void);
int  getLocalPlaylistInfo(int newestIndex, char *idOut, int idCapacity,
                         char *nameOut, int nameCapacity);
int  getLocalPlaylistRevision(void);
int  getPlaylistWatchLaterRevision(void);
int  getLocalPlaylists(SearchResults *out);
int  getLocalPlaylistVideos(const char *playlistId, SearchResults *out);
int  createLocalPlaylist(const char *name, const SearchResult *firstVideo,
                         char *playlistIdOut, int playlistIdCapacity);
int  addVideoToLocalPlaylist(const char *playlistId, const SearchResult *item);
int  removeVideoFromLocalPlaylist(const char *playlistId, const char *videoId);
int  deleteLocalPlaylist(const char *playlistId);
int  renameLocalPlaylist(const char *playlistId, const char *name);
int  removePlaylistFromWatchLater(const char *playlistId);
int  createEmptyLocalPlaylist(const char *name, char *playlistIdOut, int playlistIdCapacity);
