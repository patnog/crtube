#pragma once

typedef enum {
   UPDATE_CHECK_IDLE,
   UPDATE_CHECK_RUNNING,
   UPDATE_CHECK_UP_TO_DATE,
   UPDATE_CHECK_AVAILABLE,
   UPDATE_CHECK_FAILED
} UpdateCheckState;

int startUpdateCheck(int manual);
void shutdownUpdater(void);
UpdateCheckState getUpdateCheckState(void);
int isUpdateAvailable(void);
int shouldNotifyUpdate(void);
const char *getInstalledVersion(void);
const char *getLatestVersion(void);
const char *getLatestReleaseNotes(void);
const char *getLatestReleaseTitle(void);
const char *getLatestReleaseTag(void);
int ignoreCurrentUpdate(void);
int disableAutomaticUpdateChecks(void);
int isUpdateCheckRunning(void);

int startUpdateDownload(void);
int startUpdateDownloadOverwrite(void);
int getUpdateDownloadState(void);
unsigned long long getUpdateDownloadBytes(void);
unsigned long long getUpdateDownloadTotal(void);
const char *getDownloadedUpdatePath(void);
int isUpdateDownloaded(void);
