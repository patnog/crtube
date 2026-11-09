#pragma once

// settings - user-editable options in /dev_hdd0/tmp/yo-player/settings.txt. the file is created
// with defaults and explanatory comments on first launch, so it can be edited over FTP; an unknown
// or malformed value silently falls back to its default. loadSettings() must run once at startup.

// when auto-skip acts on community-flagged SponsorBlock segments.
typedef enum {
   SPONSORBLOCK_OFF,   // never skip anything (the segments aren't even fetched)
   SPONSORBLOCK_ADS,   // skip paid promotions only: sponsors, self-promo, like/subscribe reminders
   SPONSORBLOCK_ALL    // also skip intros, outros, filler and non-music sections
} SponsorblockMode;

void loadSettings(void);   // reads settings.txt, creating it with defaults when missing

SponsorblockMode getSponsorblockMode(void); // legacy global mode, retained for settings-file migration

// Per-category SponsorBlock action: SPONSOR_ACTION_SKIP / ASK / KEEP.
int getSponsorAction(int category);
int setSponsorAction(int category, int action);
int hasSponsorBlockActions(void); // true when at least one category is Skip or Ask

// Automatic recommendation playback after a VOD ends. Defaults to enabled.
int getAutoplayEnabled(void);
// Persists the value immediately; returns 0 on success, -1 if settings.txt could not be updated.
int setAutoplayEnabled(int enabled);

// Experimental 1080p50/60 playback through the TEE decoder. Defaults to off.
int getEnable1080p60(void);
int setEnable1080p60(int enabled);

// Preferred subtitle language code (e.g. en or pt-BR); persisted to settings.txt.
const char *getSubtitleLanguage(void);
int setSubtitleLanguage(const char *languageCode);

// Uniform display overscan in percent (-10..+10). Changes are saved immediately to settings.txt.
int getOverscanPercent(void);
int setOverscanPercent(int percent);  // 0 on success, -1 if persistence fails

// YouTube content region (gl): changes the country used by browse/search list requests.
typedef struct { const char *code; const char *name; } ContentRegion;
int getContentRegionCount(void);
const ContentRegion *getContentRegionAt(int index);
int getContentRegionIndex(void);
const char *getContentRegionCode(void);
int setContentRegionIndex(int index);
unsigned getContentRegionRevision(void);

// Independent subtitle appearance preferences: text colour, background colour, opacity and highlight.
int getSubtitleStyle(int setting);
int setSubtitleStyle(int setting, int value);
