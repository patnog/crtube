#include "app-config.h"
#include "updater.h"
#include "version.h"
#include "http.h"
#include "json.h"
#include "settings.h"
#include "thread.h"
#include "dbg.h"
#include "vfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_API_URL "https://api.github.com/repos/patnog/crtube/releases/latest"
#define UPDATE_RESPONSE_CAP (64 * 1024)
#define UPDATE_TAG_CAP 64
#define UPDATE_NOTES_CAP 24576
#define UPDATE_DOWNLOAD_DIR "/dev_hdd0/tmp/crtube/update"
#define UPDATE_PACKAGE_DIR "/dev_hdd0/packages"
#define UPDATE_ASSET_URL_CAP 2048
#define UPDATE_IO_CHUNK (64 * 1024)

static volatile int checkState = UPDATE_CHECK_IDLE;
static volatile int workerActive;
static int workerThreadCreated;
static int lastCheckManual;
static char latestTag[UPDATE_TAG_CAP];
static char latestVersion[UPDATE_TAG_CAP];
static char latestNotes[UPDATE_NOTES_CAP];
static char latestReleaseTitle[192];
static sys_ppu_thread_t updateThread;
static volatile int downloadState; /* 0 idle, 1 running, 2 complete, -1 failed */
static volatile unsigned long long downloadBytes;
static volatile unsigned long long downloadTotal;
static sys_ppu_thread_t downloadThread;
static int downloadThreadCreated;
static char downloadedPath[256];
static char assetUrl[UPDATE_ASSET_URL_CAP];

static int parseVersion(const char *text, unsigned long parts[3])
{
   if (!text || !*text) return 0;
   if (*text == 'v' || *text == 'V') text++;
   char *end;
   for (int i = 0; i < 3; i++) {
      if (!(*text >= '0' && *text <= '9')) return 0;
      parts[i] = strtoul(text, &end, 10);
      if (end == text) return 0;
      text = end;
      if (i < 2) {
         if (*text++ != '.') return 0;
      }
   }
   return *text == 0 || *text == '-' || *text == '+';
}

static int compareVersions(const char *a, const char *b)
{
   unsigned long av[3], bv[3];
   if (!parseVersion(a, av) || !parseVersion(b, bv)) return 0;
   for (int i = 0; i < 3; i++) {
      if (av[i] > bv[i]) return 1;
      if (av[i] < bv[i]) return -1;
   }
   return 0;
}

static int hasSavedPackage(const char *version, const char *path)
{
   const char *saved = getLastDownloadedUpdateVersion();
   return saved && version && path && strcmp(saved, version) == 0 && fileExists(path);
}


static int extractPkgAsset(const char *json, int length, char *url, int cap)
{
   int assetsStart = 0, assetsEnd = 0;
   if (findJsonArray(json, length, "assets", &assetsStart, &assetsEnd) != 0) {
      logWarn("[update] release JSON has no assets array\\n");
      return -1;
   }

   int offset = assetsStart;
   int objectStart = 0, objectEnd = 0;
   while ((offset = readJsonObject(json, assetsEnd, offset, &objectStart, &objectEnd)) > 0) {
      char name[128] = {0};
      char downloadUrl[UPDATE_ASSET_URL_CAP] = {0};
      int objectLength = objectEnd - objectStart;
      const char *object = json + objectStart;

      if (getJsonText(object, objectLength, "name", name, sizeof name) != 0)
         continue;

      logInfo("[update] release asset: %s\\n", name);
      if (strcmp(name, "crtube.pkg") != 0)
         continue;

      if (getJsonText(object, objectLength, "browser_download_url",
                      downloadUrl, sizeof downloadUrl) != 0) {
         logWarn("[update] crtube.pkg asset has no browser_download_url\\n");
         return -1;
      }
      if (!isHttpUrl(downloadUrl)) {
         logWarn("[update] crtube.pkg asset URL is invalid\\n");
         return -1;
      }
      if ((int)strlen(downloadUrl) >= cap) {
         logWarn("[update] crtube.pkg asset URL exceeds buffer\\n");
         return -1;
      }
      snprintf(url, cap, "%s", downloadUrl);
      return 0;
   }

   logWarn("[update] no exact crtube.pkg asset found in assets array\\n");
   return -1;
}

static int copyUpdatePackageToXmb(const char *source)
{
   if (!source || !source[0] || !fileExists(source)) return -1;
   const char *name = strrchr(source, '/');
   name = name ? name + 1 : source;
   if (!name[0]) return -1;

   char destination[320];
   snprintf(destination, sizeof destination, UPDATE_PACKAGE_DIR "/%s", name);
   makeDirPath(UPDATE_PACKAGE_DIR);

   if (fileExists(destination)) {
      int deleteRc = deleteFile(destination);
      logInfo("[update-download] old XMB package delete rc=%d path=%s\n",
              deleteRc, destination);
      if (deleteRc != 0) return -1;
   }

   char *buffer = (char *)malloc(UPDATE_IO_CHUNK);
   if (!buffer) return -1;
   int copyRc = copyFile(source, destination, buffer, UPDATE_IO_CHUNK);
   free(buffer);
   logInfo("[update-download] XMB package copy rc=%d source=%s destination=%s\n",
           copyRc, source, destination);
   if (copyRc != 0) {
      deleteFile(destination);
      return -1;
   }
   return 0;
}

static void updateDownloadWorker(uint64_t unused)
{
   (void)unused;
   char path[256], temp[272];
   snprintf(path, sizeof path, UPDATE_DOWNLOAD_DIR "/crtube-%s.pkg", latestVersion);
   snprintf(temp, sizeof temp, "%s.part", path);
   logInfo("[update-download] worker begin version=%s url=%s final=%s temp=%s\n",
           latestVersion, assetUrl, path, temp);
   makeDirPath(UPDATE_DOWNLOAD_DIR);
   if (hasSavedPackage(latestVersion, path)) {
      snprintf(downloadedPath, sizeof downloadedPath, "%s", path);
      if (copyUpdatePackageToXmb(path) == 0) {
         setLastDownloadedUpdateVersion(latestVersion);
         downloadState = 2;
         logInfo("[update-download] worker reused package and refreshed XMB copy path=%s\n", path);
      } else {
         downloadState = -1;
         logWarn("[update-download] could not refresh XMB package copy path=%s\n", path);
      }
      workerActive = 0;
      exitThread();
      return;
   }
   int deleteRc = deleteFile(temp);
   logInfo("[update-download] stale partial cleanup rc=%d path=%s\n", deleteRc, temp);
   logInfo("[update-download] opening HTTP stream url=%s\n", assetUrl);
   HttpStream *stream = openHttpStream(assetUrl);
   if (!stream) {
      logWarn("[update-download] openHttpStream failed\n");
      downloadState = -1; workerActive = 0; exitThread(); return;
   }
   uint64_t total = getHttpStreamSize(stream);
   logInfo("[update-download] HTTP stream opened expected_bytes=%llu\n", (unsigned long long)total);
   downloadTotal = total;
   VfsFile file;
   int openRc = openFs(temp, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file);
   if (openRc != 0) {
      logWarn("[update-download] openFs failed rc=%d path=%s\n", openRc, temp);
      closeHttpStream(stream); downloadState = -1; workerActive = 0; exitThread(); return;
   }
   char *buffer = (char *)malloc(UPDATE_IO_CHUNK);
   int failed = buffer == NULL;
   if (failed) logWarn("[update-download] buffer allocation failed bytes=%d\n", UPDATE_IO_CHUNK);
   while (!failed) {
      int64_t got = readHttpStream(stream, buffer, UPDATE_IO_CHUNK);
      if (got < 0) {
         logWarn("[update-download] stream read failed bytes=%llu\n", downloadBytes);
         failed = 1; break;
      }
      if (got == 0) {
         logInfo("[update-download] stream EOF bytes=%llu expected=%llu\n",
                 downloadBytes, downloadTotal);
         break;
      }
      int64_t written = writeFs(&file, buffer, (uint64_t)got);
      if (written != got) {
         logWarn("[update-download] file write failed requested=%lld written=%lld offset=%llu\n",
                 (long long)got, (long long)written, downloadBytes);
         failed = 1; break;
      }
      downloadBytes += (unsigned long long)got;
   }
   free(buffer);
   logInfo("[update-download] closing HTTP stream bytes=%llu expected=%llu failed=%d\n",
           downloadBytes, downloadTotal, failed);
   closeHttpStream(stream);
   int closeRc = closeFs(&file);
   if (closeRc != 0) {
      logWarn("[update-download] closeFs failed rc=%d\n", closeRc);
      failed = 1;
   }
   if (!failed && downloadBytes > 0 && (!total || downloadBytes == total)) {
      logInfo("[update-download] syncing completed file path=%s\n", temp);
      syncVfs(temp);
      int renameRc = renamePath(temp, path);
      logInfo("[update-download] rename rc=%d from=%s to=%s\n", renameRc, temp, path);
      if (renameRc == 0) {
         snprintf(downloadedPath, sizeof downloadedPath, "%s", path);
         if (copyUpdatePackageToXmb(path) == 0) {
            setLastDownloadedUpdateVersion(latestVersion);
            downloadState = 2;
            logInfo("[update-download] package committed version=%s path=%s\n", latestVersion, path);
         } else {
            logWarn("[update-download] package downloaded but XMB copy failed path=%s\n", path);
            downloadState = -1;
            failed = 1;
         }
      } else failed = 1;
   } else {
      logWarn("[update-download] validation failed bytes=%llu expected=%llu failed=%d\n",
              downloadBytes, downloadTotal, failed);
      failed = 1;
   }
   if (failed) {
      int deleteRc = deleteFile(temp);
      logWarn("[update-download] failed cleanup rc=%d temp=%s\n", deleteRc, temp);
      downloadState = -1;
   }
   workerActive = 0;
   logInfo("[update-download] finished state=%d bytes=%llu total=%llu path=%s\n",
           downloadState, downloadBytes, downloadTotal, downloadState == 2 ? downloadedPath : "(none)");
   exitThread();
}

int startUpdateDownload(void)
{
   logInfo("[update-download] start requested state=%d worker=%d version=%s url_present=%d\n",
           downloadState, workerActive, latestVersion, assetUrl[0] != 0);
   if (downloadState == 1 || workerActive || !latestVersion[0] || !assetUrl[0]) {
      logWarn("[update-download] start rejected: busy or release metadata missing\n");
      return -1;
   }
   char existing[256];
   snprintf(existing, sizeof existing, UPDATE_DOWNLOAD_DIR "/crtube-%s.pkg", latestVersion);
   if (hasSavedPackage(latestVersion, existing)) {
      if (copyUpdatePackageToXmb(existing) != 0) {
         logWarn("[update-download] existing package could not be copied to XMB path=%s\n", existing);
         return -1;
      }
      snprintf(downloadedPath, sizeof downloadedPath, "%s", existing);
      setLastDownloadedUpdateVersion(latestVersion);
      downloadState = 2;
      logInfo("[update-download] start reused package and refreshed XMB copy path=%s\n", existing);
      return 0;
   }
   if (compareVersions(latestVersion, CRTUBE_VERSION) < 0) {
      logWarn("[update-download] start rejected: latest version is not newer latest=%s installed=%s\n",
              latestVersion, CRTUBE_VERSION);
      return -1;
   }
   if (downloadThreadCreated) { joinThread(downloadThread); downloadThreadCreated = 0; }
   downloadBytes = downloadTotal = 0;
   downloadState = 1;
   int rc = spawnJoinableThread(&downloadThread, updateDownloadWorker, 0,
                                THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "crtube-pkg-download");
   logInfo("[update-download] worker spawn rc=%d\n", rc);
   if (rc != 0) { downloadState = -1; return -1; }
   downloadThreadCreated = 1;
   return 0;
}
int startUpdateDownloadOverwrite(void)
{
   logInfo("[update-download] overwrite requested state=%d worker=%d version=%s\n",
           downloadState, workerActive, latestVersion);
   if (downloadState == 1 || workerActive || !latestVersion[0] || !assetUrl[0]) {
      logWarn("[update-download] overwrite rejected: busy or release metadata missing\n");
      return -1;
   }

   char existing[256], xmbCopy[320], temp[272];
   char previous[256], previousXmb[320];
   snprintf(existing, sizeof existing, UPDATE_DOWNLOAD_DIR "/crtube-%s.pkg", latestVersion);
   snprintf(xmbCopy, sizeof xmbCopy, UPDATE_PACKAGE_DIR "/crtube-%s.pkg", latestVersion);
   snprintf(temp, sizeof temp, "%s.part", existing);

   /* Remove the previously downloaded version from both locations, as well
    * as any copies matching the currently advertised release. */
   const char *savedVersion = getLastDownloadedUpdateVersion();
   unsigned long savedVersionParts[3];
   if (savedVersion && parseVersion(savedVersion, savedVersionParts)) {
      snprintf(previous, sizeof previous, UPDATE_DOWNLOAD_DIR "/crtube-%s.pkg", savedVersion);
      snprintf(previousXmb, sizeof previousXmb, UPDATE_PACKAGE_DIR "/crtube-%s.pkg", savedVersion);
      if (strcmp(previous, existing) != 0 && fileExists(previous)) {
         int deleteRc = deleteFile(previous);
         logInfo("[update-download] overwrite delete previous update copy rc=%d path=%s\n", deleteRc, previous);
         if (deleteRc != 0) return -1;
      }
      if (strcmp(previousXmb, xmbCopy) != 0 && fileExists(previousXmb)) {
         int deleteRc = deleteFile(previousXmb);
         logInfo("[update-download] overwrite delete previous XMB copy rc=%d path=%s\n", deleteRc, previousXmb);
         if (deleteRc != 0) return -1;
      }
   }
   if (fileExists(existing)) {
      int deleteRc = deleteFile(existing);
      logInfo("[update-download] overwrite delete update copy rc=%d path=%s\n", deleteRc, existing);
      if (deleteRc != 0) return -1;
   }
   if (fileExists(xmbCopy)) {
      int deleteRc = deleteFile(xmbCopy);
      logInfo("[update-download] overwrite delete XMB copy rc=%d path=%s\n", deleteRc, xmbCopy);
      if (deleteRc != 0) return -1;
   }
   int tempDeleteRc = deleteFile(temp);
   logInfo("[update-download] overwrite delete partial rc=%d path=%s\n", tempDeleteRc, temp);
   downloadedPath[0] = 0;
   downloadState = 0;
   downloadBytes = downloadTotal = 0;

   return startUpdateDownload();
}

int getUpdateDownloadState(void) { return downloadState; }
unsigned long long getUpdateDownloadBytes(void) { return downloadBytes; }
unsigned long long getUpdateDownloadTotal(void) { return downloadTotal; }
const char *getDownloadedUpdatePath(void) { return downloadedPath; }
int isUpdateDownloaded(void)
{
   if (downloadState == 2 && downloadedPath[0] && fileExists(downloadedPath)) return 1;
   const char *saved = getLastDownloadedUpdateVersion();
   if (!saved || !saved[0]) return 0;
   snprintf(downloadedPath, sizeof downloadedPath, UPDATE_DOWNLOAD_DIR "/crtube-%s.pkg", saved);
   if (!fileExists(downloadedPath)) return 0;
   downloadState = 2;
   return 1;
}


static void updateWorker(uint64_t unused)
{
   (void)unused;
   char *response = (char *)malloc(UPDATE_RESPONSE_CAP);
   int responseLength = 0, status = 0;
   static const HttpHeader headers[] = {
      { "Accept", "application/vnd.github+json" },
      { "X-GitHub-Api-Version", "2022-11-28" },
      { "User-Agent", "CRTube-Update-Checker" }
   };

   if (!response) {
      checkState = UPDATE_CHECK_FAILED;
      workerActive = 0;
      exitThread();
      return;
   }

   char contentType[128] = {0};
   HttpHeaderCapture capture = { "Content-Type", contentType, sizeof contentType };

   logInfo("[update-diag] request begin method=GET url=%s installed=%s headers=%d\n",
           UPDATE_API_URL, CRTUBE_VERSION, (int)(sizeof headers / sizeof headers[0]));

   int rc = fetchHttpCapturing("GET", UPDATE_API_URL, headers,
                               (int)(sizeof headers / sizeof headers[0]),
                               NULL, 0, response, UPDATE_RESPONSE_CAP,
                               &responseLength, &status, &capture);

   logInfo("[update-diag] response rc=%d http_status=%d body_length=%d content_type=%s\n",
           rc, status, responseLength, contentType[0] ? contentType : "(not captured)");

   if (rc != 0 || status != 200 || responseLength <= 0) {
      if (responseLength > 0)
         logWarn("[update-diag] failure body preview (%d bytes): %.*s\n",
                 responseLength, responseLength < 512 ? responseLength : 512, response);
      else
         logWarn("[update-diag] no response body; transport/open/read failed before a usable HTTP body\n");
      logWarn("[update] GitHub check failed rc=%d status=%d body_length=%d\n",
              rc, status, responseLength);
      free(response);
      checkState = UPDATE_CHECK_FAILED;
      workerActive = 0;
      exitThread();
      return;
   }

   logInfo("[update-diag] HTTP 200 body preview (%d bytes): %.*s\n",
           responseLength, responseLength < 512 ? responseLength : 512, response);

   char tag[UPDATE_TAG_CAP] = {0};
   char title[sizeof latestReleaseTitle] = {0};
   char notes[UPDATE_NOTES_CAP] = {0};
   int tagResult = getJsonText(response, responseLength, "tag_name", tag, sizeof tag);
   int tagVersionValid = tagResult == 0 &&
                         parseVersion(tag, (unsigned long[3]){0, 0, 0});
   logInfo("[update-diag] JSON tag_name result=%d value=%s valid_semver=%d\n",
           tagResult, tagResult == 0 ? tag : "(missing)", tagVersionValid);

   if (!tagVersionValid) {
      logWarn("[update-diag] couldn't extract a valid three-part tag_name from GitHub response\n");
      logWarn("[update-diag] JSON parse failure body preview: %.*s\n",
              responseLength < 768 ? responseLength : 768, response);
      free(response);
      checkState = UPDATE_CHECK_FAILED;
      workerActive = 0;
      exitThread();
      return;
   }
   getJsonText(response, responseLength, "name", title, sizeof title);
   const char *bodyField = strstr(response, "\"body\"");
   if (bodyField) {
      const char *value = strchr(bodyField, ':');
      if (value) {
         value++;
         while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
         if (*value == '"') {
            int remaining = responseLength - (int)(value + 1 - response);
            decodeJsonString(value + 1, remaining, notes, sizeof notes);
         }
      }
   }
   assetUrl[0] = 0;
   if (extractPkgAsset(response, responseLength, assetUrl, sizeof assetUrl) != 0)
      logWarn("[update] release has no crtube.pkg asset\n");
   free(response);

   snprintf(latestTag, sizeof latestTag, "%s", tag);
   snprintf(latestVersion, sizeof latestVersion, "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
   snprintf(latestNotes, sizeof latestNotes, "%s", notes);
   snprintf(latestReleaseTitle, sizeof latestReleaseTitle, "%s", title[0] ? title : tag);

   int versionComparison = compareVersions(latestVersion, CRTUBE_VERSION);
   checkState = versionComparison > 0 ? UPDATE_CHECK_AVAILABLE : UPDATE_CHECK_UP_TO_DATE;
   logInfo("[update-diag] comparison installed=%s latest=%s comparison=%d state=%d manual=%d notes_length=%d\n",
           CRTUBE_VERSION, latestVersion, versionComparison, checkState, lastCheckManual,
           (int)strlen(latestNotes));
   workerActive = 0;
   exitThread();
}

int startUpdateCheck(int manual)
{
   if (workerActive) return 0;
   if (workerThreadCreated) {
      joinThread(updateThread);
      workerThreadCreated = 0;
   }
   workerActive = 1;
   lastCheckManual = manual != 0;
   checkState = UPDATE_CHECK_RUNNING;
   int rc = spawnJoinableThread(&updateThread, updateWorker, 0,
                                THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "crtube-update");
   if (rc != 0) {
      workerActive = 0;
      checkState = UPDATE_CHECK_FAILED;
      logWarn("[update] couldn't start worker rc=%d\n", rc);
      return -1;
   }
   workerThreadCreated = 1;
   return 0;
}

void shutdownUpdater(void)
{
   if (workerActive) setHttpRequestsCancelled(1);
   if (workerThreadCreated) { joinThread(updateThread); workerThreadCreated = 0; }
   if (downloadThreadCreated) { joinThread(downloadThread); downloadThreadCreated = 0; }
   workerActive = 0;
}

UpdateCheckState getUpdateCheckState(void) { return (UpdateCheckState)checkState; }
int isUpdateAvailable(void) { return checkState == UPDATE_CHECK_AVAILABLE; }
int isUpdateCheckRunning(void) { return workerActive != 0; }
const char *getInstalledVersion(void) { return CRTUBE_VERSION; }
const char *getLatestVersion(void) { return latestVersion[0] ? latestVersion : "Unknown"; }
const char *getLatestReleaseNotes(void) { return latestNotes; }
const char *getLatestReleaseTitle(void) { return latestReleaseTitle[0] ? latestReleaseTitle : getLatestReleaseTag(); }
const char *getLatestReleaseTag(void) { return latestTag; }
int shouldNotifyUpdate(void)
{
   return isUpdateAvailable() && strcmp(getIgnoredUpdateTag(), latestTag) != 0;
}

int disableAutomaticUpdateChecks(void) { return setAutomaticUpdateChecks(0); }

int ignoreCurrentUpdate(void)
{
   if (!latestTag[0]) return -1;
   return setIgnoredUpdateTag(latestTag);
}
