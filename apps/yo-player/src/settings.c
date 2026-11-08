// settings - load (and first-launch create) yo-player's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "app-config.h"
#include "settings.h"
#include "settings-file.h"
#include "gfx.h"
#include "dbg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH CRTUBE_DATA_DIR "/settings.txt"

// the file written on first launch: each setting at its default, with a comment explaining the values.
static const char *DEFAULT_SETTINGS =
   "# yo-player settings - Autoplay, overscan and content region apply immediately; theme applies on next launch\n"
   "\n"
   "autoplay=on\n"
   "# on = play the next recommendation when a video ends; off = return to the library\n"
   "region=US\n"
   "# country used for YouTube content lists (two-letter gl code); default US\n"
   "\n"
   "sponsorblock-mode=ads\n"
   "# off = never skip anything, ads = skip paid promotions only (sponsors, self-promo,\n"
   "# like/subscribe reminders), all = also skip intros, outros, filler and non-music sections\n"
   "\n"
   "overscan=0\n"
   "# output zoom in percent (-10 to +10); 0 keeps the original framing\n"
   "\n"
   "theme=youtube\n"
   "# which colour theme to start in - one of the [Name] blocks in themes.txt (lower case,\n"
   "# spaces become hyphens). the shipped one is youtube.\n";

static SponsorblockMode sponsorblockMode = SPONSORBLOCK_ADS;
static int autoplayEnabled = 1;
static int overscanPercent = 0;

static const ContentRegion REGIONS[] = {
   { "DE", "Germany" }, { "AT", "Austria" }, { "CH", "Switzerland" },
   { "AU", "Australia" }, { "BE", "Belgium" }, { "BR", "Brazil" },
   { "BG", "Bulgaria" }, { "CA", "Canada" }, { "HR", "Croatia" },
   { "CZ", "Czechia" }, { "DK", "Denmark" }, { "FI", "Finland" },
   { "FR", "France" }, { "GR", "Greece" }, { "HU", "Hungary" },
   { "IE", "Ireland" }, { "IT", "Italy" }, { "JP", "Japan" },
   { "MX", "Mexico" }, { "NL", "Netherlands" }, { "NO", "Norway" },
   { "PL", "Poland" }, { "PT", "Portugal" }, { "RO", "Romania" },
   { "RU", "Russia" }, { "RS", "Serbia" }, { "KR", "South Korea" },
   { "ES", "Spain" }, { "SE", "Sweden" }, { "TR", "Turkey" },
   { "UA", "Ukraine" }, { "GB", "United Kingdom" }, { "US", "United States" }
};
#define REGION_COUNT ((int)(sizeof REGIONS / sizeof REGIONS[0]))
static volatile int regionIndex = -1;
static unsigned regionRevision;
static int regionUnsaved;

static int defaultRegionIndex(void)
{
   for (int i = 0; i < REGION_COUNT; i++) if (strcmp(REGIONS[i].code, "US") == 0) return i;
   return 0;
}

int getContentRegionCount(void) { return REGION_COUNT; }
const ContentRegion *getContentRegionAt(int index)
{
   return index >= 0 && index < REGION_COUNT ? &REGIONS[index] : NULL;
}
int getContentRegionIndex(void)
{
   int index = regionIndex;
   return index >= 0 && index < REGION_COUNT ? index : defaultRegionIndex();
}
const char *getContentRegionCode(void) { return REGIONS[getContentRegionIndex()].code; }
unsigned getContentRegionRevision(void) { return regionRevision; }

static int parseRegion(const char *value)
{
   if (!value || !value[0] || !value[1]) return -1;
   for (int i = 0; i < REGION_COUNT; i++) {
      if (value[0] == REGIONS[i].code[0] && value[1] == REGIONS[i].code[1] &&
          (value[2] == 0 || value[2] == '\r' || value[2] == '\n' || value[2] == ' ' || value[2] == '\t')) return i;
   }
   return -1;
}

void loadSettings(void)
{
   char text[2048];
   overscanPercent = 0;
   regionIndex = defaultRegionIndex();
   regionRevision = 0;
   regionUnsaved = 0;
   if (loadSettingsFile(SETTINGS_PATH, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[settings] created %s with defaults\n", SETTINGS_PATH);

   autoplayEnabled = 1;
   const char *autoplay = findSettingValue(text, "autoplay");
   if (autoplay) {
      if      (settingValueEquals(autoplay, "on"))  autoplayEnabled = 1;
      else if (settingValueEquals(autoplay, "off")) autoplayEnabled = 0;
      else logWarn("[settings] unknown autoplay value, using on\n");
   } else {
      // Migrate existing settings.txt files without disturbing their other settings.
      autoplayEnabled = 1;
      if (rewriteSettingKey(SETTINGS_PATH, "autoplay", "on") != 0)
         logWarn("[settings] couldn't add autoplay default to %s\n", SETTINGS_PATH);
   }

   const char *region = findSettingValue(text, "region");
   int parsedRegion = parseRegion(region);
   if (parsedRegion >= 0) {
      regionIndex = parsedRegion;
      logInfo("[settings] content region %s (%s)\n", REGIONS[parsedRegion].code, REGIONS[parsedRegion].name);
   } else {
      regionIndex = defaultRegionIndex();
      if (region) logWarn("[settings] unknown content region, using US\n");
      if (!region && rewriteSettingKey(SETTINGS_PATH, "region", "US") != 0)
         logWarn("[settings] could not add default content region\n");
   }

   const char *mode = findSettingValue(text, "sponsorblock-mode");
   if (mode) {
      if      (settingValueEquals(mode, "off")) sponsorblockMode = SPONSORBLOCK_OFF;
      else if (settingValueEquals(mode, "ads")) sponsorblockMode = SPONSORBLOCK_ADS;
      else if (settingValueEquals(mode, "all")) sponsorblockMode = SPONSORBLOCK_ALL;
      else logWarn("[settings] unknown sponsorblock-mode value, using ads\n");
   }

   const char *overscan = findSettingValue(text, "overscan");
   if (!overscan) {
      // Add the new key to existing installations without disturbing their other settings/comments.
      if (rewriteSettingKey(SETTINGS_PATH, "overscan", "0") != 0)
         logWarn("[settings] could not add default overscan key\n");
   }
   if (overscan) {
      char *end = NULL;
      long parsed = strtol(overscan, &end, 10);
      if (end != overscan && (*end == 0 || *end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
         if (parsed < -10) parsed = -10;
         if (parsed > 10) parsed = 10;
         overscanPercent = (int)parsed;
      } else {
         logWarn("[settings] invalid overscan value, using 0\n");
         overscanPercent = 0;
      }
   }
   setGfxOverscan(overscanPercent);
}

SponsorblockMode getSponsorblockMode(void) { return sponsorblockMode; }

int getAutoplayEnabled(void) { return autoplayEnabled; }

int setAutoplayEnabled(int enabled)
{
   enabled = enabled ? 1 : 0;
   if (enabled == autoplayEnabled) return 0;
   if (rewriteSettingKey(SETTINGS_PATH, "autoplay", enabled ? "on" : "off") != 0) {
      logWarn("[settings] couldn't persist autoplay=%s\n", enabled ? "on" : "off");
      return -1;
   }
   autoplayEnabled = enabled;
   logInfo("[settings] autoplay=%s\n", enabled ? "on" : "off");
   return 0;
}

int getOverscanPercent(void) { return overscanPercent; }

int setOverscanPercent(int percent)
{
   if (percent < -10) percent = -10;
   if (percent > 10) percent = 10;
   if (percent == overscanPercent) return 0;

   char value[8];
   int length = snprintf(value, sizeof value, "%d", percent);
   if (length <= 0 || length >= (int)sizeof value ||
       rewriteSettingKey(SETTINGS_PATH, "overscan", value) != 0) {
      logWarn("[settings] could not persist overscan=%d\n", percent);
      return -1;
   }

   overscanPercent = percent;
   setGfxOverscan(overscanPercent);
   logInfo("[settings] overscan set to %d%%\n", overscanPercent);
   return 0;
}

int setContentRegionIndex(int index)
{
   if (index < 0 || index >= REGION_COUNT) return -1;
   int old = getContentRegionIndex();
   if (index == old && !regionUnsaved) return 0;
   if (index != old) { regionIndex = index; regionRevision++; }
   if (rewriteSettingKey(SETTINGS_PATH, "region", REGIONS[index].code) != 0) {
      regionUnsaved = 1;
      logWarn("[settings] could not persist region=%s\n", REGIONS[index].code);
      return -1;
   }
   regionUnsaved = 0;
   logInfo("[settings] content region %s -> %s\n", REGIONS[old].code, REGIONS[index].code);
   return 0;
}
