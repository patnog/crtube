// theme.c - built-in CRTube palettes, runtime theme selection and themes.txt compatibility.

#include "app-config.h"
#include "theme.h"
#include "theme-registry.h"
#include "vfs.h"
#include "settings-file.h"
#include "dbg.h"
#include <stdlib.h>
#include <stdio.h>

#define TMP_DIR        CRTUBE_DATA_DIR
#define THEMES_PATH    TMP_DIR "/themes.txt"
#define SETTINGS_PATH  TMP_DIR "/settings.txt"
#define SETTINGS_BUFFER 4096

static const Theme themeYouTube = {
   .appBg = 0xFF0F0F0F, .surface = 0xFF212121, .accent = 0xFFFF0000,
   .focusBorder = 0xFFFFFFFF, .textPrimary = 0xFFFFFFFF, .textSecondary = 0xFFAAAAAA,
   .badgeFill = 0xCC000000, .scrim = 0xD9000000, .rowHighlight = 0x28FFFFFF,
   .seekTrack = 0x66FFFFFF, .seekNotch = 0xFF000000,
   .watchedThumbTint = 0xFF4D4D4D, .focusThickness = 3
};

static const uint32_t ACCENT_PALETTE[] = {
   0xFFFF1744, 0xFFFF6D00, 0xFFFFC400, 0xFFAEEA00, 0xFF00FF41,
   0xFF00BFA5, 0xFF00E5FF, 0xFF1E88E5, 0xFF536DFE,
   0xFFAA00FF, 0xFFFF4081, 0xFFBDBDBD, 0xFF8B0000,
   0xFFB71C1C, 0xFF880E4F, 0xFF7B1FA2, 0xFF00695C, 0xFF283593
};
#define ACCENT_PALETTE_COUNT ((int)(sizeof ACCENT_PALETTE / sizeof ACCENT_PALETTE[0]))

static Theme presetThemes[5];
static Theme customTheme;
static Theme themes[MAX_THEMES];
static ThemeRegistry registry;
static int activePreset = THEME_PRESET_CRTUBE;
static int customBackgroundDark = 1;
static uint32_t customAccent = 0xFFFF0000;

const Theme *activeTheme = &themeYouTube;

static Theme makeTheme(int dark, uint32_t accent)
{
   Theme t = themeYouTube;
   t.accent = accent;
   if (dark) {
      t.appBg = 0xFF101010;
      t.surface = 0xFF242424;
      t.focusBorder = 0xFFFFFFFF;
      t.textPrimary = 0xFFFFFFFF;
      t.textSecondary = 0xFFAAAAAA;
      t.badgeFill = 0xCC000000;
      t.scrim = 0xD9000000;
      t.rowHighlight = 0x28FFFFFF;
      t.seekTrack = 0x66FFFFFF;
      t.seekNotch = 0xFF000000;
      t.watchedThumbTint = 0xFF4D4D4D;
   } else {
      t.appBg = 0xFFF1F1F1;
      t.surface = 0xFFDADADA;
      t.focusBorder = 0xFF303030;
      t.textPrimary = 0xFF181818;
      t.textSecondary = 0xFF626262;
      t.badgeFill = 0xCC000000;
      t.scrim = 0x99000000;
      t.rowHighlight = 0x22000000;
      t.seekTrack = 0x55000000;
      t.seekNotch = 0xFFFFFFFF;
      t.watchedThumbTint = 0xFFB0B0B0;
   }
   return t;
}

static const ThemeColorField COLOR_FIELDS[] = {
   { "appBg", offsetof(Theme, appBg) },
   { "surface", offsetof(Theme, surface) },
   { "accent", offsetof(Theme, accent) },
   { "focusBorder", offsetof(Theme, focusBorder) },
   { "textPrimary", offsetof(Theme, textPrimary) },
   { "textSecondary", offsetof(Theme, textSecondary) },
   { "badgeFill", offsetof(Theme, badgeFill) },
   { "scrim", offsetof(Theme, scrim) },
   { "rowHighlight", offsetof(Theme, rowHighlight) },
   { "seekTrack", offsetof(Theme, seekTrack) },
   { "seekNotch", offsetof(Theme, seekNotch) },
   { "watchedThumbTint", offsetof(Theme, watchedThumbTint) }
};
#define COLOR_FIELD_COUNT ((int)(sizeof COLOR_FIELDS / sizeof COLOR_FIELDS[0]))

static const char *THEMES_FILE_HEADER =
   "# CRTube themes - edit over FTP; changes apply on the next launch.\n"
   "# Each [Name] block inherits the built-in CRTube palette unless overridden.\n"
   "# Colours are #RRGGBB or #RRGGBBAA (alpha last).\n\n";

static void buildPresetThemes(void)
{
   presetThemes[THEME_PRESET_CRTUBE] = makeTheme(1, 0xFFFF0000);
   presetThemes[THEME_PRESET_BLUE] = makeTheme(1, 0xFF1E88E5);
   presetThemes[THEME_PRESET_GREEN] = makeTheme(1, 0xFF00FF41);
   presetThemes[THEME_PRESET_LIGHT] = makeTheme(0, 0xFF8B0000);
   presetThemes[THEME_PRESET_MONOCHROME] = makeTheme(0, 0xFF333333);
}

static void applySettingsTheme(void)
{
   char text[SETTINGS_BUFFER];
   int length = readFile(SETTINGS_PATH, text, sizeof text - 1);
   if (length <= 0) return;
   text[length] = '\0';

   const char *slug = findSettingValue(text, "theme");
   if (!slug || settingValueEquals(slug, "crtube")) {
      activePreset = THEME_PRESET_CRTUBE;
      activeTheme = &presetThemes[THEME_PRESET_CRTUBE];
      return;
   }
   if (settingValueEquals(slug, "youtube")) {
      int legacyIndex = findThemeBySlug(&registry, "youtube");
      activePreset = THEME_PRESET_CRTUBE;
      activeTheme = legacyIndex >= 0 ? (const Theme *)getRegisteredTheme(&registry, legacyIndex) :
                                      &presetThemes[THEME_PRESET_CRTUBE];
      return;
   }
   if (settingValueEquals(slug, "blue")) {
      activePreset = THEME_PRESET_BLUE; activeTheme = &presetThemes[activePreset]; return;
   }
   if (settingValueEquals(slug, "green")) {
      activePreset = THEME_PRESET_GREEN; activeTheme = &presetThemes[activePreset]; return;
   }
   if (settingValueEquals(slug, "light")) {
      activePreset = THEME_PRESET_LIGHT; activeTheme = &presetThemes[activePreset]; return;
   }
   if (settingValueEquals(slug, "monochrome")) {
      activePreset = THEME_PRESET_MONOCHROME; activeTheme = &presetThemes[activePreset]; return;
   }
   if (settingValueEquals(slug, "custom")) {
      const char *bg = findSettingValue(text, "theme-custom-bg");
      const char *color = findSettingValue(text, "theme-custom-accent");
      customBackgroundDark = !bg || settingValueEquals(bg, "dark");
      if (color && color[0] == '#') {
         unsigned long parsed = strtoul(color + 1, NULL, 16);
         customAccent = 0xFF000000u | ((uint32_t)parsed & 0x00FFFFFFu);
      }
      customTheme = makeTheme(customBackgroundDark, customAccent);
      activePreset = THEME_PRESET_CUSTOM;
      activeTheme = &customTheme;
      return;
   }

   // Preserve support for themes manually added to themes.txt.
   int themeIndex = findThemeBySlug(&registry, slug);
   if (themeIndex >= 0) {
      activePreset = -1;
      activeTheme = (const Theme *)getRegisteredTheme(&registry, themeIndex);
   }
}

void initTheme(void)
{
   makeDir(TMP_DIR);
   buildPresetThemes();
   customTheme = makeTheme(1, 0xFFFF0000);
   initThemeRegistry(&registry, themes, sizeof(Theme), &themeYouTube, "YouTube",
                     COLOR_FIELDS, COLOR_FIELD_COUNT);
   activeTheme = &presetThemes[THEME_PRESET_CRTUBE];

   if (!fileExists(THEMES_PATH)) {
      int rc = writeThemeTemplate(&registry, THEMES_PATH, THEMES_FILE_HEADER);
      if (rc == 0) logInfo("[theme] created %s\n", THEMES_PATH);
      else logError("[theme] couldn't create %s\n", THEMES_PATH);
   }
   loadThemeFile(&registry, THEMES_PATH);
   applySettingsTheme();
}

int getActiveThemePreset(void) { return activePreset; }

const char *getThemePresetName(int preset)
{
   static const char *names[THEME_PRESET_COUNT] = {
      "CRTube", "Blue", "Green", "Light", "Monochrome", "Custom"
   };
   return preset >= 0 && preset < THEME_PRESET_COUNT ? names[preset] : "Custom";
}

void getThemePresetPreview(int preset, uint32_t *background, uint32_t *accent)
{
   const Theme *theme = NULL;
   if (preset >= 0 && preset < 5) theme = &presetThemes[preset];
   else if (preset == THEME_PRESET_CUSTOM) theme = &customTheme;
   else theme = &presetThemes[THEME_PRESET_CRTUBE];
   if (background) *background = theme->appBg;
   if (accent) *accent = theme->accent;
}

int getCustomThemeBackgroundDark(void) { return customBackgroundDark; }
uint32_t getCustomThemeAccent(void) { return customAccent; }
int getThemeAccentPaletteCount(void) { return ACCENT_PALETTE_COUNT; }
uint32_t getThemeAccentPaletteColor(int index)
{
   return index >= 0 && index < ACCENT_PALETTE_COUNT ? ACCENT_PALETTE[index] : ACCENT_PALETTE[0];
}

int applyThemePreset(int preset)
{
   if (preset < 0 || preset >= 5) return -1;
   activePreset = preset;
   activeTheme = &presetThemes[preset];
   const char *slugs[5] = { "crtube", "blue", "green", "light", "monochrome" };
   int rc = rewriteSettingKey(SETTINGS_PATH, "theme", slugs[preset]);
   if (rc != 0) logError("[theme] couldn't persist preset %s\n", slugs[preset]);
   return rc;
}

int saveCustomThemeSettings(int darkBackground, uint32_t accent)
{
   char colorText[16];
   customBackgroundDark = darkBackground ? 1 : 0;
   customAccent = 0xFF000000u | (accent & 0x00FFFFFFu);
   customTheme = makeTheme(customBackgroundDark, customAccent);

   snprintf(colorText, sizeof colorText, "#%06X", (unsigned)(customAccent & 0x00FFFFFFu));
   int rcBg = rewriteSettingKey(SETTINGS_PATH, "theme-custom-bg",
                                customBackgroundDark ? "dark" : "light");
   int rcAccent = rewriteSettingKey(SETTINGS_PATH, "theme-custom-accent", colorText);
   if (rcBg || rcAccent)
      logError("[theme] custom theme settings persistence failed\n");
   return (rcBg || rcAccent) ? -1 : 0;
}

int applyCustomTheme(int darkBackground, uint32_t accent)
{
   int rcSaved = saveCustomThemeSettings(darkBackground, accent);
   activePreset = THEME_PRESET_CUSTOM;
   activeTheme = &customTheme;
   int rcTheme = rewriteSettingKey(SETTINGS_PATH, "theme", "custom");
   if (rcSaved || rcTheme)
      logError("[theme] custom theme applied but settings persistence failed\n");
   return (rcSaved || rcTheme) ? -1 : 0;
}
