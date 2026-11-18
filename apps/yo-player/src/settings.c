// settings - load (and first-launch create) yo-player's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "app-config.h"
#include "settings.h"
#include "localization.h"
#include "sponsorblock.h"
#include "settings-file.h"
#include "gfx.h"
#include "dbg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH CRTUBE_DATA_DIR "/settings.txt"

// the file written on first launch: each setting at its default, with a comment explaining the values.
static const char *DEFAULT_SETTINGS =
   "# yo-player settings - Autoplay, overscan, content region and theme apply immediately\n"
   "\n"
   "autoplay=on\n"
   "# on = play the next recommendation when a video ends; off = return to the library\n"
   "automatic-update-checks=on\n"
   "# on = check GitHub releases at startup; off = only check manually in Settings > Updates\n"
   "enable-1080p60=off\n"
   "# on = allow experimental TEE 1080p50/60 playback; off = Sony VDEC only, up to 1080p30\n"
   "subtitle-language=auto\n"
   "playback-subtitle-language=auto\n"
   "auto-translate-language=auto\n"
   "subtitle-text-color=0\n"
   "subtitle-background-color=0\n"
   "subtitle-background-opacity=3\n"
   "subtitle-highlight=0\n"
   "subtitle-font-size=2\n"
   "# Subtitle style: text color 0-5, background 0-4, opacity 0-4, highlight 0-2\n"
   "# subtitle language: auto follows the content region; otherwise a language code is a manual override\n"
   "# playback-subtitle-language affects Playback > Options > Subs language only; download settings use subtitle-language\n"
   "# auto-translate-language: auto follows Content Region; off disables automatic translation; otherwise use a language code\n"
   "region=US\n"
   "app-language=en\n"
   "# application interface language (currently: en)\n"
   "# country used for YouTube content lists (two-letter gl code); default US\n"
   "\n"
   "sponsorblock-mode=ads\n"
   "# Legacy global mode, used only to migrate older installations. Per-category actions below take precedence.\n"
   "sponsorblock-sponsor=skip\n"
   "sponsorblock-selfpromo=skip\n"
   "sponsorblock-interaction=skip\n"
   "sponsorblock-intro=keep\n"
   "sponsorblock-outro=keep\n"
   "sponsorblock-music-offtopic=keep\n"
   "sponsorblock-filler=keep\n"
   "# Each category accepts skip, ask, or keep.\n"
   "\n"
   "overscan=0\n"
   "overscan-crop-to-fill=0\n"
   "screensaver-timeout=30\n"
   "osd-timeout=2\n"
   "# playback OSD inactivity timeout in seconds: 2, 3, 5, 10, 15, 30 or 60\n"
   "# output zoom in percent (-15 to +15); 0 keeps the original framing\n"
   "# independent video zoom used only when Crop to Fill is enabled (-15 to +15)\n"
   "subtitle-offset=0\n"
   "# subtitle vertical offset in virtual screen pixels (-150 to +150); positive moves up, negative moves down\n"
   "\n"
   "theme=crtube\n"
   "# colour theme: crtube, blue, green, light, monochrome or custom\n"
   "\n"
   "sort-search=0\n"
   "sort-recommended=0\n"
   "sort-account-subscriptions=0\n"
   "sort-ps3-subscriptions=0\n"
   "sort-trending-gaming=0\n"
   "sort-trending-sports=0\n"
   "sort-trending-podcasts=0\n"
   "sort-live-popular=0\n"
   "sort-live-music=0\n"
   "sort-live-gaming=0\n"
   "sort-live-sports=0\n"
   "sort-live-podcasts=0\n"
   "sort-live-news=0\n"
   "sort-account-playlists=0\n"
   "sort-local-playlists-watch-later=0\n"
   "sort-local-playlists-ps3=0\n"
   "download-resolution=0\n"
   "download-audio-language=original\n"
   "download-audio-auto-dub=on\n"
   "download-primary-subtitles=on\n"
   "download-secondary-subtitle=off\n";

static SponsorblockMode sponsorblockMode = SPONSORBLOCK_ADS;
static int sponsorActions[SPONSOR_CATEGORY_COUNT] = {
   SPONSOR_ACTION_SKIP, SPONSOR_ACTION_SKIP, SPONSOR_ACTION_SKIP,
   SPONSOR_ACTION_KEEP, SPONSOR_ACTION_KEEP, SPONSOR_ACTION_KEEP, SPONSOR_ACTION_KEEP
};
static const char *SPONSOR_SETTING_KEYS[SPONSOR_CATEGORY_COUNT] = {
   "sponsorblock-sponsor", "sponsorblock-selfpromo", "sponsorblock-interaction",
   "sponsorblock-intro", "sponsorblock-outro", "sponsorblock-music-offtopic", "sponsorblock-filler"
};
static int autoplayEnabled = 1;
static int enable1080p60 = 0;
static char subtitleLanguage[12] = "auto";
static char playbackSubtitleLanguage[12] = "auto";
static char autoTranslateLanguage[12] = "auto";
static int overscanPercent = 0;
static int cropToFillOverscanPercent = 0;
static int screensaverTimeout = 30;
static int osdTimeoutSeconds = 2;
static int subtitleOffsetPixels = 0;
static int subtitleStyle[5] = { 0, 0, 3, 0, 2 };
static const char *SUBTITLE_STYLE_KEYS[5] = { "subtitle-text-color", "subtitle-background-color", "subtitle-background-opacity", "subtitle-highlight", "subtitle-font-size" };
static const int SUBTITLE_STYLE_MAX[5] = { 5, 4, 4, 2, 5 };

static const char *SORT_SETTING_KEYS[SORT_PREF_COUNT] = {
   "sort-search", "sort-recommended", "sort-account-subscriptions", "sort-ps3-subscriptions",
   "sort-trending-gaming", "sort-trending-sports", "sort-trending-podcasts",
   "sort-live-popular", "sort-live-music", "sort-live-gaming", "sort-live-sports",
   "sort-live-podcasts", "sort-live-news", "sort-account-playlists",
   "sort-local-playlists-watch-later", "sort-local-playlists-ps3"
};
static const int SORT_SETTING_MAX[SORT_PREF_COUNT] = {
   2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1
};
static int sortPreferences[SORT_PREF_COUNT];
static int downloadResolution;
static char downloadAudioLanguage[12] = "original";
static int downloadAutoDub = 1;
static int downloadPrimarySubtitles = 1;
static char downloadSecondarySubtitle[12] = "off";
static int automaticUpdateChecks = 1;
static char ignoredUpdateTag[64];
static char lastDownloadedUpdateVersion[64];

// Languages represented by the countries in Content Region. Countries with multiple
// common languages contribute each language once to this shared download-language list.
static const DownloadLanguage DOWNLOAD_LANGUAGES[] = {
   { "bg", "Bulgarian" }, { "hr", "Croatian" }, { "cs", "Czech" },
   { "da", "Danish" }, { "nl", "Dutch" }, { "en", "English" },
   { "fi", "Finnish" }, { "fr", "French" }, { "de", "German" },
   { "el", "Greek" }, { "hu", "Hungarian" }, { "ga", "Irish" },
   { "it", "Italian" }, { "ja", "Japanese" }, { "ko", "Korean" },
   { "no", "Norwegian" }, { "pl", "Polish" }, { "pt", "Portuguese" },
   { "ro", "Romanian" }, { "ru", "Russian" }, { "sr", "Serbian" },
   { "es", "Spanish" }, { "sv", "Swedish" }, { "tr", "Turkish" },
   { "uk", "Ukrainian" }
};
#define DOWNLOAD_LANGUAGE_COUNT ((int)(sizeof DOWNLOAD_LANGUAGES / sizeof DOWNLOAD_LANGUAGES[0]))

// Google Translate language targets exposed by Playback > Playback Settings.
// Keep this list independent from download-language preferences.
static const AutoTranslateLanguage AUTO_TRANSLATE_LANGUAGES[] = {
   { "af", "Afrikaans" }, { "sq", "Albanian" }, { "am", "Amharic" },
   { "ar", "Arabic" }, { "hy", "Armenian" }, { "az", "Azerbaijani" },
   { "eu", "Basque" }, { "be", "Belarusian" }, { "bn", "Bengali" },
   { "bs", "Bosnian" }, { "bg", "Bulgarian" }, { "ca", "Catalan" },
   { "ceb", "Cebuano" }, { "zh-CN", "Chinese (Simplified)" },
   { "zh-TW", "Chinese (Traditional)" }, { "co", "Corsican" },
   { "hr", "Croatian" }, { "cs", "Czech" }, { "da", "Danish" },
   { "nl", "Dutch" }, { "en", "English" }, { "eo", "Esperanto" },
   { "et", "Estonian" }, { "tl", "Filipino" }, { "fi", "Finnish" },
   { "fr", "French" }, { "fy", "Frisian" }, { "gl", "Galician" },
   { "ka", "Georgian" }, { "de", "German" }, { "el", "Greek" },
   { "gu", "Gujarati" }, { "ht", "Haitian Creole" }, { "ha", "Hausa" },
   { "haw", "Hawaiian" }, { "iw", "Hebrew" }, { "hi", "Hindi" },
   { "hmn", "Hmong" }, { "hu", "Hungarian" }, { "is", "Icelandic" },
   { "ig", "Igbo" }, { "id", "Indonesian" }, { "ga", "Irish" },
   { "it", "Italian" }, { "ja", "Japanese" }, { "jw", "Javanese" },
   { "kn", "Kannada" }, { "kk", "Kazakh" }, { "km", "Khmer" },
   { "ko", "Korean" }, { "ku", "Kurdish" }, { "ky", "Kyrgyz" },
   { "lo", "Lao" }, { "la", "Latin" }, { "lv", "Latvian" },
   { "lt", "Lithuanian" }, { "lb", "Luxembourgish" },
   { "mk", "Macedonian" }, { "mg", "Malagasy" }, { "ms", "Malay" },
   { "ml", "Malayalam" }, { "mt", "Maltese" }, { "mi", "Maori" },
   { "mr", "Marathi" }, { "mn", "Mongolian" }, { "my", "Myanmar" },
   { "ne", "Nepali" }, { "no", "Norwegian" }, { "ny", "Nyanja" },
   { "or", "Odia" }, { "ps", "Pashto" }, { "fa", "Persian" },
   { "pl", "Polish" }, { "pt", "Portuguese" },
   { "pt-BR", "Portuguese (Brazil)" }, { "pa", "Punjabi" },
   { "ro", "Romanian" }, { "ru", "Russian" }, { "sm", "Samoan" },
   { "gd", "Scots Gaelic" }, { "sr", "Serbian" }, { "st", "Sesotho" },
   { "sn", "Shona" }, { "sd", "Sindhi" }, { "si", "Sinhala" },
   { "sk", "Slovak" }, { "sl", "Slovenian" }, { "so", "Somali" },
   { "es", "Spanish" }, { "su", "Sundanese" }, { "sw", "Swahili" },
   { "sv", "Swedish" }, { "tg", "Tajik" }, { "ta", "Tamil" },
   { "tt", "Tatar" }, { "te", "Telugu" }, { "th", "Thai" },
   { "tr", "Turkish" }, { "tk", "Turkmen" }, { "uk", "Ukrainian" },
   { "ur", "Urdu" }, { "ug", "Uyghur" }, { "uz", "Uzbek" },
   { "vi", "Vietnamese" }, { "cy", "Welsh" }, { "xh", "Xhosa" },
   { "yi", "Yiddish" }, { "yo", "Yoruba" }, { "zu", "Zulu" }
};
#define AUTO_TRANSLATE_LANGUAGE_COUNT ((int)(sizeof AUTO_TRANSLATE_LANGUAGES / sizeof AUTO_TRANSLATE_LANGUAGES[0]))

static const ContentRegion REGIONS[] = {
   { "DE", "Germany", "de" }, { "AT", "Austria", "de" }, { "CH", "Switzerland", "de" },
   { "AU", "Australia", "en" }, { "BE", "Belgium", "nl" }, { "BR", "Brazil", "pt-BR" },
   { "BG", "Bulgaria", "bg" }, { "CA", "Canada", "en" }, { "HR", "Croatia", "hr" },
   { "CZ", "Czechia", "cs" }, { "DK", "Denmark", "da" }, { "FI", "Finland", "fi" },
   { "FR", "France", "fr" }, { "GR", "Greece", "el" }, { "HU", "Hungary", "hu" },
   { "IE", "Ireland", "en" }, { "IT", "Italy", "it" }, { "JP", "Japan", "ja" },
   { "MX", "Mexico", "es" }, { "NL", "Netherlands", "nl" }, { "NO", "Norway", "no" },
   { "PL", "Poland", "pl" }, { "PT", "Portugal", "pt" }, { "RO", "Romania", "ro" },
   { "RU", "Russia", "ru" }, { "RS", "Serbia", "sr" }, { "KR", "South Korea", "ko" },
   { "ES", "Spain", "es" }, { "SE", "Sweden", "sv" }, { "TR", "Turkey", "tr" },
   { "UA", "Ukraine", "uk" }, { "GB", "United Kingdom", "en" }, { "US", "United States", "en" }
};
#define REGION_COUNT ((int)(sizeof REGIONS / sizeof REGIONS[0]))
static volatile int regionIndex = -1;
static unsigned regionRevision;
static int regionUnsaved;
static int applicationLanguage = APP_LANGUAGE_EN;

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
const char *getContentRegionLanguageCode(void) { return REGIONS[getContentRegionIndex()].languageCode; }
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
   char text[4096];
   overscanPercent = 0;
   cropToFillOverscanPercent = 0;
   screensaverTimeout = 30;
   subtitleOffsetPixels = 0;
   regionIndex = defaultRegionIndex();
   regionRevision = 0;
   regionUnsaved = 0;
   applicationLanguage = APP_LANGUAGE_EN;
   setAppLanguage(APP_LANGUAGE_EN);
   if (loadSettingsFile(SETTINGS_PATH, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[settings] created %s with defaults\n", SETTINGS_PATH);

   const char *updateChecks = findSettingValue(text, "automatic-update-checks");
   automaticUpdateChecks = 1;
   if (updateChecks) {
      if (settingValueEquals(updateChecks, "off")) automaticUpdateChecks = 0;
      else if (!settingValueEquals(updateChecks, "on"))
         logWarn("[settings] unknown automatic-update-checks value, using on\n");
   } else if (rewriteSettingKey(SETTINGS_PATH, "automatic-update-checks", "on") != 0) {
      logWarn("[settings] could not add automatic-update-checks default\n");
   }
   const char *downloadedVersion = findSettingValue(text, "last-downloaded-update-version");
   lastDownloadedUpdateVersion[0] = 0;
   if (downloadedVersion) {
      int i = 0;
      while (downloadedVersion[i] && downloadedVersion[i] != '\r' && downloadedVersion[i] != '\n' &&
             downloadedVersion[i] != ' ' && i < (int)sizeof lastDownloadedUpdateVersion - 1) {
         lastDownloadedUpdateVersion[i] = downloadedVersion[i]; i++;
      }
      lastDownloadedUpdateVersion[i] = 0;
   }
   const char *ignoredTag = findSettingValue(text, "ignored-update-tag");
   ignoredUpdateTag[0] = 0;
   if (ignoredTag) {
      int i = 0;
      while (ignoredTag[i] && ignoredTag[i] != '\r' && ignoredTag[i] != '\n' &&
             ignoredTag[i] != ' ' && i < (int)sizeof ignoredUpdateTag - 1) {
         ignoredUpdateTag[i] = ignoredTag[i]; i++;
      }
      ignoredUpdateTag[i] = 0;
   }

   for (int i = 0; i < SORT_PREF_COUNT; i++) {
      const char *value = findSettingValue(text, SORT_SETTING_KEYS[i]);
      int parsed = value ? atoi(value) : 0;
      if (parsed < 0 || parsed > SORT_SETTING_MAX[i]) parsed = 0;
      sortPreferences[i] = parsed;
      if (!value && rewriteSettingKey(SETTINGS_PATH, SORT_SETTING_KEYS[i], "0") != 0)
         logWarn("[settings] could not add %s default\n", SORT_SETTING_KEYS[i]);
   }

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

   const char *experimental1080 = findSettingValue(text, "enable-1080p60");
   if (experimental1080) {
      if      (settingValueEquals(experimental1080, "on"))  enable1080p60 = 1;
      else if (settingValueEquals(experimental1080, "off")) enable1080p60 = 0;
      else {
         enable1080p60 = 0;
         logWarn("[settings] unknown enable-1080p60 value, using off\\n");
      }
   } else {
      enable1080p60 = 0;
      if (rewriteSettingKey(SETTINGS_PATH, "enable-1080p60", "off") != 0)
         logWarn("[settings] couldn't add enable-1080p60 default to %s\\n", SETTINGS_PATH);
   }

   const char *subtitleLang = findSettingValue(text, "subtitle-language");
   if (!subtitleLang) {
      snprintf(subtitleLanguage, sizeof subtitleLanguage, "auto");
      if (rewriteSettingKey(SETTINGS_PATH, "subtitle-language", "auto") != 0)
         logWarn("[settings] could not add default subtitle language\n");
   } else {
      int length = 0, valid = 1;
      while (subtitleLang[length] && subtitleLang[length] != '\r' && subtitleLang[length] != '\n' &&
             subtitleLang[length] != ' ' && subtitleLang[length] != '\t') {
         char c = subtitleLang[length];
         if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-')) valid = 0;
         length++;
         if (length >= (int)sizeof subtitleLanguage) { valid = 0; break; }
      }
      if (length < 2 || !valid) {
         snprintf(subtitleLanguage, sizeof subtitleLanguage, "auto");
         logWarn("[settings] invalid subtitle-language, using auto\n");
         if (rewriteSettingKey(SETTINGS_PATH, "subtitle-language", "auto") != 0)
            logWarn("[settings] could not reset subtitle language\n");
      } else {
         memcpy(subtitleLanguage, subtitleLang, (size_t)length);
         subtitleLanguage[length] = 0;
      }
   }

   const char *playbackSubtitleLang = findSettingValue(text, "playback-subtitle-language");
   if (!playbackSubtitleLang) {
      // Preserve the previous shared preference for existing installations. From now on,
      // playback and downloads persist independently.
      snprintf(playbackSubtitleLanguage, sizeof playbackSubtitleLanguage, "%s", subtitleLanguage);
      if (rewriteSettingKey(SETTINGS_PATH, "playback-subtitle-language", playbackSubtitleLanguage) != 0)
         logWarn("[settings] could not add default playback subtitle language\n");
   } else {
      int length = 0, valid = 1;
      while (playbackSubtitleLang[length] && playbackSubtitleLang[length] != '\r' &&
             playbackSubtitleLang[length] != '\n' && playbackSubtitleLang[length] != ' ' &&
             playbackSubtitleLang[length] != '\t') {
         char c = playbackSubtitleLang[length];
         if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-')) valid = 0;
         length++;
         if (length >= (int)sizeof playbackSubtitleLanguage) { valid = 0; break; }
      }
      if (length < 2 || !valid) {
         snprintf(playbackSubtitleLanguage, sizeof playbackSubtitleLanguage, "auto");
         logWarn("[settings] invalid playback-subtitle-language, using auto\n");
         if (rewriteSettingKey(SETTINGS_PATH, "playback-subtitle-language", "auto") != 0)
            logWarn("[settings] could not reset playback subtitle language\n");
      } else {
         memcpy(playbackSubtitleLanguage, playbackSubtitleLang, (size_t)length);
         playbackSubtitleLanguage[length] = 0;
      }
   }

   const char *autoTranslate = findSettingValue(text, "auto-translate-language");
   if (!autoTranslate) {
      snprintf(autoTranslateLanguage, sizeof autoTranslateLanguage, "auto");
      if (rewriteSettingKey(SETTINGS_PATH, "auto-translate-language", "auto") != 0)
         logWarn("[settings] could not add auto-translate-language default\n");
   } else {
      int length = 0, valid = 1;
      while (autoTranslate[length] && autoTranslate[length] != '\r' &&
             autoTranslate[length] != '\n' && autoTranslate[length] != ' ' &&
             autoTranslate[length] != '\t') {
         char c = autoTranslate[length];
         if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-')) valid = 0;
         length++;
         if (length >= (int)sizeof autoTranslateLanguage) { valid = 0; break; }
      }
      autoTranslateLanguage[0] = 0;
      if (length >= 2 && valid) {
         char candidate[sizeof autoTranslateLanguage];
         memcpy(candidate, autoTranslate, (size_t)length);
         candidate[length] = 0;
         int found = strcmp(candidate, "auto") == 0 || strcmp(candidate, "off") == 0;
         for (int i = 0; !found && i < AUTO_TRANSLATE_LANGUAGE_COUNT; i++)
            if (strcmp(candidate, AUTO_TRANSLATE_LANGUAGES[i].code) == 0) found = 1;
         if (found) snprintf(autoTranslateLanguage, sizeof autoTranslateLanguage, "%s", candidate);
      }
      if (!autoTranslateLanguage[0]) {
         snprintf(autoTranslateLanguage, sizeof autoTranslateLanguage, "auto");
         logWarn("[settings] invalid auto-translate-language, using auto\n");
         if (rewriteSettingKey(SETTINGS_PATH, "auto-translate-language", "auto") != 0)
            logWarn("[settings] could not reset auto-translate-language\n");
      }
   }

   for (int i = 0; i < 5; i++) {
      const char *value = findSettingValue(text, SUBTITLE_STYLE_KEYS[i]);
      int parsed = value ? atoi(value) : subtitleStyle[i];
      if (parsed < 0 || parsed > SUBTITLE_STYLE_MAX[i]) parsed = subtitleStyle[i];
      subtitleStyle[i] = parsed;
      if (!value) {
         char fallback[8]; snprintf(fallback, sizeof fallback, "%d", parsed);
         if (rewriteSettingKey(SETTINGS_PATH, SUBTITLE_STYLE_KEYS[i], fallback) != 0)
            logWarn("[settings] could not add %s default\n", SUBTITLE_STYLE_KEYS[i]);
      }
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

   const char *appLanguage = findSettingValue(text, "app-language");
   applicationLanguage = APP_LANGUAGE_EN;
   if (appLanguage) {
      for (int i = 0; i < APP_LANGUAGE_COUNT; i++) {
         if (strcmp(appLanguage, getAppLanguageCode(i)) == 0) {
            applicationLanguage = i;
            break;
         }
      }
      if (applicationLanguage == APP_LANGUAGE_EN && strcmp(appLanguage, "en") != 0)
         logWarn("[settings] unsupported app-language '%s', using en\n", appLanguage);
   }
   setAppLanguage(applicationLanguage);
   if (!appLanguage && rewriteSettingKey(SETTINGS_PATH, "app-language", "en") != 0)
      logWarn("[settings] could not add default app-language\n");

   const char *downloadValue = findSettingValue(text, "download-resolution");
   downloadResolution = downloadValue ? atoi(downloadValue) : DOWNLOAD_RESOLUTION_AUTO;
   if (downloadResolution < DOWNLOAD_RESOLUTION_AUTO || downloadResolution > DOWNLOAD_RESOLUTION_1080P)
      downloadResolution = DOWNLOAD_RESOLUTION_AUTO;
   if (!downloadValue && rewriteSettingKey(SETTINGS_PATH, "download-resolution", "0") != 0)
      logWarn("[settings] could not add download-resolution default\n");

   downloadValue = findSettingValue(text, "download-audio-language");
   if (downloadValue && (settingValueEquals(downloadValue, "original") ||
                         settingValueEquals(downloadValue, "off"))) {
      snprintf(downloadAudioLanguage, sizeof downloadAudioLanguage, "original");
   } else if (downloadValue) {
      int found = 0;
      for (int i = 0; i < DOWNLOAD_LANGUAGE_COUNT; i++)
         if (settingValueEquals(downloadValue, DOWNLOAD_LANGUAGES[i].code)) {
            snprintf(downloadAudioLanguage, sizeof downloadAudioLanguage, "%s", DOWNLOAD_LANGUAGES[i].code);
            found = 1; break;
         }
      if (!found) snprintf(downloadAudioLanguage, sizeof downloadAudioLanguage, "original");
   } else {
      snprintf(downloadAudioLanguage, sizeof downloadAudioLanguage, "original");
      if (rewriteSettingKey(SETTINGS_PATH, "download-audio-language", "original") != 0)
         logWarn("[settings] could not add download-audio-language default\n");
   }

   downloadValue = findSettingValue(text, "download-audio-auto-dub");
   downloadAutoDub = downloadValue ? settingValueEquals(downloadValue, "on") : 1;
   if (!downloadValue && rewriteSettingKey(SETTINGS_PATH, "download-audio-auto-dub", "on") != 0)
      logWarn("[settings] could not add download-audio-auto-dub default\n");

   downloadValue = findSettingValue(text, "download-primary-subtitles");
   downloadPrimarySubtitles = downloadValue ? settingValueEquals(downloadValue, "on") : 1;
   if (!downloadValue && rewriteSettingKey(SETTINGS_PATH, "download-primary-subtitles", "on") != 0)
      logWarn("[settings] could not add download-primary-subtitles default\n");

   downloadValue = findSettingValue(text, "download-secondary-subtitle");
   if (downloadValue && settingValueEquals(downloadValue, "off")) {
      snprintf(downloadSecondarySubtitle, sizeof downloadSecondarySubtitle, "off");
   } else if (downloadValue) {
      int found = 0;
      for (int i = 0; i < DOWNLOAD_LANGUAGE_COUNT; i++)
         if (settingValueEquals(downloadValue, DOWNLOAD_LANGUAGES[i].code)) {
            snprintf(downloadSecondarySubtitle, sizeof downloadSecondarySubtitle, "%s", DOWNLOAD_LANGUAGES[i].code);
            found = 1; break;
         }
      if (!found) snprintf(downloadSecondarySubtitle, sizeof downloadSecondarySubtitle, "off");
   } else {
      snprintf(downloadSecondarySubtitle, sizeof downloadSecondarySubtitle, "off");
      if (rewriteSettingKey(SETTINGS_PATH, "download-secondary-subtitle", "off") != 0)
         logWarn("[settings] could not add download-secondary-subtitle default\n");
   }

   const char *mode = findSettingValue(text, "sponsorblock-mode");
   if (mode) {
      if      (settingValueEquals(mode, "off")) sponsorblockMode = SPONSORBLOCK_OFF;
      else if (settingValueEquals(mode, "ads")) sponsorblockMode = SPONSORBLOCK_ADS;
      else if (settingValueEquals(mode, "all")) sponsorblockMode = SPONSORBLOCK_ALL;
      else logWarn("[settings] unknown sponsorblock-mode value, using ads\n");
   }

   // Migrate the old global mode into independent category actions once. Existing explicit per-category
   // values always win; missing values inherit the old mode (ads = the three paid-promotion categories).
   for (int category = 0; category < SPONSOR_CATEGORY_COUNT; category++) {
      const char *value = findSettingValue(text, SPONSOR_SETTING_KEYS[category]);
      int fallback = SPONSOR_ACTION_KEEP;
      if (sponsorblockMode == SPONSORBLOCK_ALL) fallback = SPONSOR_ACTION_SKIP;
      else if (sponsorblockMode == SPONSORBLOCK_ADS && category <= SPONSOR_INTERACTION)
         fallback = SPONSOR_ACTION_SKIP;
      if (!value) {
         sponsorActions[category] = fallback;
         const char *seed = fallback == SPONSOR_ACTION_SKIP ? "skip" : "keep";
         if (rewriteSettingKey(SETTINGS_PATH, SPONSOR_SETTING_KEYS[category], seed) != 0)
            logWarn("[settings] could not seed %s\n", SPONSOR_SETTING_KEYS[category]);
      } else if (settingValueEquals(value, "skip")) sponsorActions[category] = SPONSOR_ACTION_SKIP;
      else if (settingValueEquals(value, "ask")) sponsorActions[category] = SPONSOR_ACTION_ASK;
      else if (settingValueEquals(value, "keep")) sponsorActions[category] = SPONSOR_ACTION_KEEP;
      else {
         sponsorActions[category] = fallback;
         logWarn("[settings] invalid %s value, using migrated default\n", SPONSOR_SETTING_KEYS[category]);
      }
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
         if (parsed < -15) parsed = -15;
         if (parsed > 15) parsed = 15;
         overscanPercent = (int)parsed;
      } else {
         logWarn("[settings] invalid overscan value, using 0\n");
         overscanPercent = 0;
      }
   }
   setGfxOverscan(overscanPercent);

   const char *cropOverscan = findSettingValue(text, "overscan-crop-to-fill");
   if (!cropOverscan) {
      if (rewriteSettingKey(SETTINGS_PATH, "overscan-crop-to-fill", "0") != 0)
         logWarn("[settings] could not add default overscan-crop-to-fill key\n");
   } else {
      char *end = NULL;
      long parsed = strtol(cropOverscan, &end, 10);
      if (end != cropOverscan && (*end == 0 || *end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
         if (parsed < -15) parsed = -15;
         if (parsed > 15) parsed = 15;
         cropToFillOverscanPercent = (int)parsed;
      } else {
         logWarn("[settings] invalid overscan-crop-to-fill value, using 0\n");
         cropToFillOverscanPercent = 0;
      }
   }

   const char *screensaverTimeoutValue = findSettingValue(text, "screensaver-timeout");
   if (!screensaverTimeoutValue) {
      if (rewriteSettingKey(SETTINGS_PATH, "screensaver-timeout", "30") != 0)
         logWarn("[settings] could not add default screensaver-timeout key\n");
   } else {
      char *end = NULL;
      long parsed = strtol(screensaverTimeoutValue, &end, 10);
      if (end != screensaverTimeoutValue &&
          (*end == 0 || *end == '\n' || *end == '\r' || *end == ' ' || *end == '\t') &&
          (parsed == 0 || (parsed >= 30 && parsed <= 300 && parsed % 30 == 0))) {
         screensaverTimeout = (int)parsed;
      } else {
         screensaverTimeout = 30;
         logWarn("[settings] invalid screensaver-timeout value, using 30\n");
      }
   }

   const char *osdTimeoutValue = findSettingValue(text, "osd-timeout");
   if (!osdTimeoutValue) {
      if (rewriteSettingKey(SETTINGS_PATH, "osd-timeout", "2") != 0)
         logWarn("[settings] could not add default osd-timeout key\n");
   } else {
      char *end = NULL;
      long parsed = strtol(osdTimeoutValue, &end, 10);
      if (end != osdTimeoutValue &&
          (*end == 0 || *end == '\n' || *end == '\r' || *end == ' ' || *end == '\t') &&
          (parsed == 2 || parsed == 3 || parsed == 5 || parsed == 10 || parsed == 15 || parsed == 30 || parsed == 60)) {
         osdTimeoutSeconds = (int)parsed;
      } else {
         osdTimeoutSeconds = 2;
         logWarn("[settings] invalid osd-timeout value, using 2\n");
      }
   }

   const char *subtitleOffsetValue = findSettingValue(text, "subtitle-offset");
   if (!subtitleOffsetValue) {
      if (rewriteSettingKey(SETTINGS_PATH, "subtitle-offset", "0") != 0)
         logWarn("[settings] could not add default subtitle-offset key\n");
   } else {
      char *end = NULL;
      long parsed = strtol(subtitleOffsetValue, &end, 10);
      if (end != subtitleOffsetValue && (*end == 0 || *end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
         if (parsed < -150) parsed = -150;
         if (parsed > 150) parsed = 150;
         subtitleOffsetPixels = (int)parsed;
      } else {
         logWarn("[settings] invalid subtitle-offset value, using 0\n");
         subtitleOffsetPixels = 0;
      }
   }
}

int getSortPreference(SortPreference setting)
{
   if (setting < 0 || setting >= SORT_PREF_COUNT) return 0;
   return sortPreferences[setting];
}

int setSortPreference(SortPreference setting, int value)
{
   if (setting < 0 || setting >= SORT_PREF_COUNT || value < 0 || value > SORT_SETTING_MAX[setting]) return -1;
   if (sortPreferences[setting] == value) return 0;
   char text[8];
   snprintf(text, sizeof text, "%d", value);
   if (rewriteSettingKey(SETTINGS_PATH, SORT_SETTING_KEYS[setting], text) != 0) {
      logWarn("[settings] could not persist %s=%s\n", SORT_SETTING_KEYS[setting], text);
      return -1;
   }
   sortPreferences[setting] = value;
   return 0;
}

SponsorblockMode getSponsorblockMode(void) { return sponsorblockMode; }

int getSponsorAction(int category)
{
   if (category < 0 || category >= SPONSOR_CATEGORY_COUNT) return SPONSOR_ACTION_KEEP;
   return sponsorActions[category];
}

int setSponsorAction(int category, int action)
{
   if (category < 0 || category >= SPONSOR_CATEGORY_COUNT) return -1;
   if (action != SPONSOR_ACTION_SKIP && action != SPONSOR_ACTION_ASK && action != SPONSOR_ACTION_KEEP) return -1;
   if (sponsorActions[category] == action) return 0;
   const char *value = action == SPONSOR_ACTION_SKIP ? "skip" :
                       action == SPONSOR_ACTION_ASK ? "ask" : "keep";
   if (rewriteSettingKey(SETTINGS_PATH, SPONSOR_SETTING_KEYS[category], value) != 0) {
      logWarn("[settings] could not persist %s=%s\n", SPONSOR_SETTING_KEYS[category], value);
      return -1;
   }
   sponsorActions[category] = action;
   logInfo("[settings] %s=%s\n", SPONSOR_SETTING_KEYS[category], value);
   return 0;
}

int hasSponsorBlockActions(void)
{
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++)
      if (sponsorActions[i] != SPONSOR_ACTION_KEEP) return 1;
   return 0;
}

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

const char *getSubtitleLanguage(void)
{
   return strcmp(subtitleLanguage, "auto") == 0 ? getContentRegionLanguageCode() : subtitleLanguage;
}

int setSubtitleLanguage(const char *languageCode)
{
   if (!languageCode) return -1;
   if (strcmp(languageCode, "auto") == 0) {
      if (strcmp(subtitleLanguage, "auto") == 0) return 0;
      if (rewriteSettingKey(SETTINGS_PATH, "subtitle-language", "auto") != 0) {
         logWarn("[settings] couldn't persist subtitle-language=auto\n");
         return -1;
      }
      strCopy(subtitleLanguage, sizeof subtitleLanguage, "auto");
      logInfo("[settings] subtitle-language=auto (region %s)\n", getContentRegionLanguageCode());
      return 0;
   }
   int length = 0;
   while (languageCode[length]) {
      char c = languageCode[length];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') || length >= (int)sizeof subtitleLanguage - 1)
         return -1;
      length++;
   }
   if (length < 2) return -1;
   if (strcmp(subtitleLanguage, languageCode) == 0) return 0;
   if (rewriteSettingKey(SETTINGS_PATH, "subtitle-language", languageCode) != 0) {
      logWarn("[settings] couldn't persist subtitle-language=%s\n", languageCode);
      return -1;
   }
   snprintf(subtitleLanguage, sizeof subtitleLanguage, "%s", languageCode);
   logInfo("[settings] subtitle-language=%s\n", subtitleLanguage);
   return 0;
}

const char *getPlaybackSubtitleLanguage(void)
{
   return strcmp(playbackSubtitleLanguage, "auto") == 0 ? getContentRegionLanguageCode() : playbackSubtitleLanguage;
}

int setPlaybackSubtitleLanguage(const char *languageCode)
{
   if (!languageCode) return -1;
   if (strcmp(languageCode, "auto") == 0) {
      if (strcmp(playbackSubtitleLanguage, "auto") == 0) return 0;
      if (rewriteSettingKey(SETTINGS_PATH, "playback-subtitle-language", "auto") != 0) {
         logWarn("[settings] couldn't persist playback-subtitle-language=auto\n");
         return -1;
      }
      strCopy(playbackSubtitleLanguage, sizeof playbackSubtitleLanguage, "auto");
      logInfo("[settings] playback-subtitle-language=auto (region %s)\n", getContentRegionLanguageCode());
      return 0;
   }
   int length = 0;
   while (languageCode[length]) {
      char c = languageCode[length];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') || length >= (int)sizeof playbackSubtitleLanguage - 1)
         return -1;
      length++;
   }
   if (length < 2) return -1;
   if (strcmp(playbackSubtitleLanguage, languageCode) == 0) return 0;
   if (rewriteSettingKey(SETTINGS_PATH, "playback-subtitle-language", languageCode) != 0) {
      logWarn("[settings] couldn't persist playback-subtitle-language=%s\n", languageCode);
      return -1;
   }
   snprintf(playbackSubtitleLanguage, sizeof playbackSubtitleLanguage, "%s", languageCode);
   logInfo("[settings] playback-subtitle-language=%s\n", playbackSubtitleLanguage);
   return 0;
}

const char *getAutoTranslateLanguage(void)
{
   if (strcmp(autoTranslateLanguage, "off") == 0) return "off";
   return strcmp(autoTranslateLanguage, "auto") == 0 ? getContentRegionLanguageCode() : autoTranslateLanguage;
}

const char *getAutoTranslateLanguagePreference(void) { return autoTranslateLanguage; }
int getAutoTranslateLanguageCount(void) { return AUTO_TRANSLATE_LANGUAGE_COUNT; }
const AutoTranslateLanguage *getAutoTranslateLanguageAt(int index)
{
   return index >= 0 && index < AUTO_TRANSLATE_LANGUAGE_COUNT ? &AUTO_TRANSLATE_LANGUAGES[index] : NULL;
}
const char *getAutoTranslateLanguageName(const char *languageCode)
{
   if (!languageCode) return NULL;
   for (int i = 0; i < AUTO_TRANSLATE_LANGUAGE_COUNT; i++)
      if (strcmp(languageCode, AUTO_TRANSLATE_LANGUAGES[i].code) == 0)
         return AUTO_TRANSLATE_LANGUAGES[i].name;
   return NULL;
}

int setAutoTranslateLanguage(const char *languageCode)
{
   if (!languageCode || !languageCode[0]) return -1;
   int valid = strcmp(languageCode, "auto") == 0 || strcmp(languageCode, "off") == 0;
   for (int i = 0; !valid && i < AUTO_TRANSLATE_LANGUAGE_COUNT; i++)
      if (strcmp(languageCode, AUTO_TRANSLATE_LANGUAGES[i].code) == 0) valid = 1;
   if (!valid) return -1;
   if (strcmp(autoTranslateLanguage, languageCode) == 0) return 0;
   if (rewriteSettingKey(SETTINGS_PATH, "auto-translate-language", languageCode) != 0) {
      logWarn("[settings] couldn't persist auto-translate-language=%s\n", languageCode);
      return -1;
   }
   snprintf(autoTranslateLanguage, sizeof autoTranslateLanguage, "%s", languageCode);
   logInfo("[settings] auto-translate-language=%s (resolved=%s)\n",
           autoTranslateLanguage, getAutoTranslateLanguage());
   return 0;
}

int getEnable1080p60(void) { return enable1080p60; }

int setEnable1080p60(int enabled)
{
   enabled = enabled ? 1 : 0;
   if (enabled == enable1080p60) return 0;

   /* Fail closed: if TEE shutdown forces this option OFF, disable it in memory
    * even when settings.txt cannot be rewritten. Enabling still requires a
    * successful save so the UI never advertises an unsaved ON state. */
   if (!enabled) enable1080p60 = 0;
   if (rewriteSettingKey(SETTINGS_PATH, "enable-1080p60", enabled ? "on" : "off") != 0) {
      logWarn("[settings] couldn't persist enable-1080p60=%s\n", enabled ? "on" : "off");
      return -1;
   }
   enable1080p60 = enabled;
   logInfo("[settings] enable-1080p60=%s\n", enabled ? "on" : "off");
   return 0;
}

int getOverscanPercent(void) { return overscanPercent; }
int getCropToFillOverscanPercent(void) { return cropToFillOverscanPercent; }

int setCropToFillOverscanPercent(int percent)
{
   if (percent < -15) percent = -15;
   if (percent > 15) percent = 15;
   if (percent == cropToFillOverscanPercent) return 0;

   char value[8];
   int length = snprintf(value, sizeof value, "%d", percent);
   if (length <= 0 || length >= (int)sizeof value ||
       rewriteSettingKey(SETTINGS_PATH, "overscan-crop-to-fill", value) != 0) {
      logWarn("[settings] could not persist overscan-crop-to-fill=%d\n", percent);
      return -1;
   }

   cropToFillOverscanPercent = percent;
   logInfo("[settings] overscan-crop-to-fill set to %d%%\n", percent);
   return 0;
}

int setOverscanPercent(int percent)
{
   if (percent < -15) percent = -15;
   if (percent > 15) percent = 15;
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

int getSubtitleOffset(void) { return subtitleOffsetPixels; }

int setSubtitleOffset(int pixels)
{
   if (pixels < -150) pixels = -150;
   if (pixels > 150) pixels = 150;
   if (pixels == subtitleOffsetPixels) return 0;

   char value[8];
   int length = snprintf(value, sizeof value, "%d", pixels);
   if (length <= 0 || length >= (int)sizeof value ||
       rewriteSettingKey(SETTINGS_PATH, "subtitle-offset", value) != 0) {
      logWarn("[settings] could not persist subtitle-offset=%d\n", pixels);
      return -1;
   }

   subtitleOffsetPixels = pixels;
   logInfo("[settings] subtitle-offset=%d px\n", subtitleOffsetPixels);
   return 0;
}

int getOsdTimeout(void) { return osdTimeoutSeconds; }

int setOsdTimeout(int seconds)
{
    if (seconds != 2 && seconds != 3 && seconds != 5 &&
        seconds != 10 && seconds != 15 && seconds != 30 && seconds != 60) return -1;
    if (seconds == osdTimeoutSeconds) return 0;

    char value[8];
    int length = snprintf(value, sizeof value, "%d", seconds);
    if (length <= 0 || length >= (int)sizeof value ||
        rewriteSettingKey(SETTINGS_PATH, "osd-timeout", value) != 0) {
       logWarn("[settings] could not persist osd-timeout=%d\n", seconds);
       return -1;
    }

    osdTimeoutSeconds = seconds;
    logInfo("[settings] osd-timeout=%d\n", osdTimeoutSeconds);
    return 0;
}

int getScreensaverTimeout(void) { return screensaverTimeout; }

int setScreensaverTimeout(int seconds)
{
   if (seconds != 0 && (seconds < 30 || seconds > 300 || seconds % 30 != 0)) return -1;
   if (seconds == screensaverTimeout) return 0;

   char value[8];
   int length = snprintf(value, sizeof value, "%d", seconds);
   if (length <= 0 || length >= (int)sizeof value ||
       rewriteSettingKey(SETTINGS_PATH, "screensaver-timeout", value) != 0) {
      logWarn("[settings] could not persist screensaver-timeout=%d\n", seconds);
      return -1;
   }

   screensaverTimeout = seconds;
   logInfo("[settings] screensaver-timeout=%d\n", screensaverTimeout);
   return 0;
}

int getApplicationLanguage(void)
{
   return applicationLanguage;
}

int setApplicationLanguage(int language)
{
   if (language < 0 || language >= APP_LANGUAGE_COUNT) return -1;
   const char *code = getAppLanguageCode(language);
   if (rewriteSettingKey(SETTINGS_PATH, "app-language", code) != 0) {
      logWarn("[settings] could not persist app-language=%s\n", code);
      return -1;
   }
   applicationLanguage = language;
   setAppLanguage(language);
   logInfo("[settings] app-language=%s\n", code);
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

int getSubtitleStyle(int setting)
{
   return setting >= 0 && setting < 5 ? subtitleStyle[setting] : 0;
}

int setSubtitleStyle(int setting, int value)
{
   if (setting < 0 || setting >= 5 || value < 0 || value > SUBTITLE_STYLE_MAX[setting]) return -1;
   if (subtitleStyle[setting] == value) return 0;
   char text[8]; snprintf(text, sizeof text, "%d", value);
   if (rewriteSettingKey(SETTINGS_PATH, SUBTITLE_STYLE_KEYS[setting], text) != 0) return -1;
   subtitleStyle[setting] = value;
   return 0;
}


static int isDownloadLanguageCode(const char *code)
{
   if (!code) return 0;
   for (int i = 0; i < DOWNLOAD_LANGUAGE_COUNT; i++)
      if (strcmp(code, DOWNLOAD_LANGUAGES[i].code) == 0) return 1;
   return 0;
}

static int persistDownloadSetting(const char *key, const char *value)
{
   if (rewriteSettingKey(SETTINGS_PATH, key, value) != 0) {
      logWarn("[settings] could not persist %s=%s\n", key, value);
      return -1;
   }
   return 0;
}

int getDownloadResolution(void) { return downloadResolution; }
int setDownloadResolution(int resolution)
{
   if (resolution < DOWNLOAD_RESOLUTION_AUTO || resolution > DOWNLOAD_RESOLUTION_1080P) return -1;
   if (resolution == downloadResolution) return 0;
   char value[8]; snprintf(value, sizeof value, "%d", resolution);
   if (persistDownloadSetting("download-resolution", value) != 0) return -1;
   downloadResolution = resolution;
   return 0;
}

const char *getDownloadAudioLanguage(void) { return downloadAudioLanguage; }
int setDownloadAudioLanguage(const char *languageCode)
{
   if (!languageCode || (strcmp(languageCode, "original") != 0 && !isDownloadLanguageCode(languageCode))) return -1;
   if (strcmp(downloadAudioLanguage, languageCode) == 0) return 0;
   if (persistDownloadSetting("download-audio-language", languageCode) != 0) return -1;
   snprintf(downloadAudioLanguage, sizeof downloadAudioLanguage, "%s", languageCode);
   return 0;
}

int getDownloadAutoDubEnabled(void) { return downloadAutoDub; }
int setDownloadAutoDubEnabled(int enabled)
{
   enabled = enabled ? 1 : 0;
   if (enabled == downloadAutoDub) return 0;
   if (persistDownloadSetting("download-audio-auto-dub", enabled ? "on" : "off") != 0) return -1;
   downloadAutoDub = enabled;
   return 0;
}

int getDownloadPrimarySubtitlesEnabled(void) { return downloadPrimarySubtitles; }
int setDownloadPrimarySubtitlesEnabled(int enabled)
{
   enabled = enabled ? 1 : 0;
   if (enabled == downloadPrimarySubtitles) return 0;
   if (persistDownloadSetting("download-primary-subtitles", enabled ? "on" : "off") != 0) return -1;
   downloadPrimarySubtitles = enabled;
   return 0;
}

const char *getDownloadSecondarySubtitleLanguage(void) { return downloadSecondarySubtitle; }
int setDownloadSecondarySubtitleLanguage(const char *languageCode)
{
   if (!languageCode || (strcmp(languageCode, "off") != 0 && !isDownloadLanguageCode(languageCode))) return -1;
   if (strcmp(downloadSecondarySubtitle, languageCode) == 0) return 0;
   if (persistDownloadSetting("download-secondary-subtitle", languageCode) != 0) return -1;
   snprintf(downloadSecondarySubtitle, sizeof downloadSecondarySubtitle, "%s", languageCode);
   return 0;
}

int getDownloadLanguageCount(void) { return DOWNLOAD_LANGUAGE_COUNT; }
const DownloadLanguage *getDownloadLanguageAt(int index)
{
   return index >= 0 && index < DOWNLOAD_LANGUAGE_COUNT ? &DOWNLOAD_LANGUAGES[index] : NULL;
}

int getAutomaticUpdateChecks(void) { return automaticUpdateChecks; }
int setAutomaticUpdateChecks(int enabled)
{
   enabled = enabled != 0;
   if (rewriteSettingKey(SETTINGS_PATH, "automatic-update-checks", enabled ? "on" : "off") != 0)
      return -1;
   automaticUpdateChecks = enabled;
   return 0;
}
const char *getIgnoredUpdateTag(void) { return ignoredUpdateTag; }
int setIgnoredUpdateTag(const char *tag)
{
   if (!tag || !tag[0] || strlen(tag) >= sizeof ignoredUpdateTag ||
       strchr(tag, '\n') || strchr(tag, '\r')) return -1;
   if (rewriteSettingKey(SETTINGS_PATH, "ignored-update-tag", tag) != 0) return -1;
   snprintf(ignoredUpdateTag, sizeof ignoredUpdateTag, "%s", tag);
   return 0;
}

const char *getLastDownloadedUpdateVersion(void) { return lastDownloadedUpdateVersion; }
int setLastDownloadedUpdateVersion(const char *version)
{
   if (!version || !version[0] || strlen(version) >= sizeof lastDownloadedUpdateVersion ||
       strchr(version, '\n') || strchr(version, '\r')) return -1;
   if (rewriteSettingKey(SETTINGS_PATH, "last-downloaded-update-version", version) != 0) return -1;
   snprintf(lastDownloadedUpdateVersion, sizeof lastDownloadedUpdateVersion, "%s", version);
   return 0;
}
