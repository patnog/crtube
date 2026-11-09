// yo-player - a native PS3 YouTube client (in progress).
//
// Boilerplate mirrors file-manager (the proven full app): RTC + network up
// front, bridge logging, VSYNC-ON gfx, then the frame loop. It boots into the
// home screen (trending categories); searching / channels / playback stream via
// simple-lib-av. Bring-up diagnostics go to the bridge log (dbg.h).

#include <sys/process.h>

#ifdef PSL1GHT
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#endif

#include "app-config.h"
#include "app.h"
#include "vfs.h"
#include "http.h"
#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "audio.h"
#include "font.h"
#include "screen-manager.h"
#include "screens/home.h"
#include "storage.h"
#include "account.h"
#include "settings.h"
#include "video-player.h"
#include "downloads.h"
#include "video-export.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"
#include "ui/stats.h"
#ifndef PSL1GHT
#include "bridge-client.h"
#endif
#include "dbg.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

#ifdef PSL1GHT
// PSL1GHT's historical SYS_PROCESS_PARAM macro still advertises SDK 1.92.
// Match the known-good official Yo! Player EBOOT's 4.75.001 SDK field while
// retaining PSL1GHT's latest supported process-parameter structure revision.
// Its public sys_process_param_t is also missing the ninth crash-dump field
// required by revision 3.30 (despite documenting it), so define the complete
// 36-byte ABI layout locally.
#define YO_PLAYER_SDK_VERSION_475_001 0x00475001
typedef struct YoPlayerProcessParam {
   u32 size;
   u32 magic;
   u32 version;
   u32 sdkVersion;
   s32 priority;
   u32 stackSize;
   u32 mallocPageSize;
   u32 ppcSegment;
   u32 crashDumpParamAddress;
} YoPlayerProcessParam;

YoPlayerProcessParam __sys_process_param
   __attribute__((aligned(8), section(".sys_proc_param"), unused)) = {
      sizeof(YoPlayerProcessParam),
      SYS_PROCESS_SPAWN_MAGIC,
      SYS_PROCESS_SPAWN_VERSION_330,
      YO_PLAYER_SDK_VERSION_475_001,
      PROCESS_PRIORITY_DEFAULT,
      PROCESS_STACK_SIZE_64KB,
      SYS_PROCESS_SPAWN_MALLOC_PAGE_SIZE_1M,
      SYS_PROCESS_SPAWN_PPC_SEG_DEFAULT,
      0
   };
#else
SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)
#endif

// Keep the PSL1GHT boot trace independent of RTC, VFS, printf, networking and
// graphics.  A launch that returns to XMB can therefore still identify the
// exact subsystem which was entered last on real hardware.
#ifdef PSL1GHT
static const char *const bootLogPaths[] = {
   // The installed title directory remains writable when media-service launch
   // policy denies access to the shared temporary directory.
   "/dev_hdd0/game/CRTUBE001/USRDIR/crtube-boot.log",
   "/dev_hdd0/tmp/crtube/crtube-boot.log"
};

static void bootMark(const char *text, uint64_t length)
{
   for (unsigned int i = 0; i < sizeof(bootLogPaths) / sizeof(bootLogPaths[0]); i++) {
      int descriptor = -1;
      uint64_t written = 0;
      int flags = CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND;
      if (cellFsOpen(bootLogPaths[i], flags, &descriptor, NULL, 0) != CELL_FS_SUCCEEDED) continue;
      cellFsWrite(descriptor, text, length, &written);
      cellFsClose(descriptor);
   }
}

#define BOOT_MARK(text) bootMark((text), sizeof(text) - 1)
#else
#define BOOT_MARK(text) ((void)0)
#endif

static void applyPendingTeeDisableRequest(void)
{
   if (!consumeVideoPlayerTeeDisableRequest() || !getEnable1080p60()) return;
   if (setEnable1080p60(0) == 0)
      logWarn("[settings] Enable 1080p (experimental) switched OFF after TEE failure or unsafe close\n");
   else
      logError("[settings] TEE failure; experimental 1080p disabled in memory but could not be saved\n");
}

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   BOOT_MARK("main\n");
   appRegisterExitCallback();
   BOOT_MARK("sysutil callback\n");
   initRtc();
   BOOT_MARK("rtc\n");
   initVfs();                    // file i/o routing (the temp download lands via openFs)
   BOOT_MARK("vfs\n");

   // Select this build’s private log before any app or library diagnostics are emitted.
   setDebugLogPath(CRTUBE_LOG_PATH);
   VfsFile logFile;
   if (openFs(CRTUBE_LOG_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &logFile) == 0)
      closeFs(&logFile);

   logBuildVersion();
   initTheme();                  // palette from themes.txt: every colour below reads activeTheme
   BOOT_MARK("theme\n");

   // Mark both sides of module loading and network initialisation separately. If
   // sysmodule loading fails, keep the UI usable offline instead of calling into
   // an unavailable sys_net entry point.
   int netRc = cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
   BOOT_MARK("network module load returned\n");
   if (netRc == 0 || (uint32_t)netRc == 0x80012001u) { // loaded, or already loaded
      BOOT_MARK("network init begin\n");
      netRc = sys_net_initialize_network();
      BOOT_MARK("network init returned\n");
   } else {
      BOOT_MARK("network module load failed\n");
   }
   BOOT_MARK("network\n");
   initModernHttp();   // bind the modern (BearSSL) http transport; all requests + media streams go through it
   BOOT_MARK("http\n");
#ifndef PSL1GHT
   // The loopback debug bridge is a companion to the Sony-SDK development
   // environment.  It is optional and unavailable in a standalone PSL1GHT
   // package, so do not start its background socket thread on retail hardware.
   registerWithBridge("app", "crtube-sdk");
#endif

   if (initGfx(GFX_VSYNC_ON) != 0) { BOOT_MARK("gfx failed\n"); return 1; }
   BOOT_MARK("gfx\n");
   if (initAudio() != 0) { BOOT_MARK("audio failed\n"); return 1; }
   BOOT_MARK("audio\n");
   if (initFont() != 0) { BOOT_MARK("font failed\n"); return 1; }
   BOOT_MARK("font\n");
   if (initIconFont() != 0) logError("[yt] embedded icon font failed to load; icons will be blank\n");
   BOOT_MARK("icon font\n");
   initPad();
   BOOT_MARK("pad\n");

   initStats(5, 5, 14, activeTheme->textSecondary);
   initStorage();          // prefs (last category) + watch history, under this build’s private data directory
   initAccount();          // saved YouTube sign-in, if any (account.txt beside the prefs)
   loadSettings();         // user-editable settings.txt (created with defaults on first launch)
   loadConsoleGlyphs();    // decode the console's own button glyphs for the on-screen hints
   initVideoExport();      // resolves the download staging folder, so it runs before initDownloads
   initDownloads();        // background download queue + its progress overlay
   BOOT_MARK("services\n");

   logInfo("[yt] net rc=%d\n", netRc);
   openHome();       // boot into the home screen (trending categories)
   BOOT_MARK("home\n");

   while (!appExitRequested) {
      appPoll();
      applyPendingTeeDisableRequest();
      updateVideoExport();   // right after the sysutil pump, which is what delivers its results
      updatePad();
      updateScreen();

      beginGfxFrame();
      clearGfx(activeTheme->appBg);
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   applyPendingTeeDisableRequest();
   shutdownAccount();     // stop a sign-in poll before the http layer goes away
   shutdownDownloads();   // cancel + join any in-flight download before the http/vfs layers go away
   shutdownVideoExport();
   freeConsoleGlyphs();
   termStats();
   termAudio();
   termFont();
   termGfx();
   shutdownHttp();
   shutdownVfs();
   return 0;
}
