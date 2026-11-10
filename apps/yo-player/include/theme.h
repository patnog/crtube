#pragma once

#include <stdint.h>

typedef struct {
   uint32_t appBg;
   uint32_t surface;
   uint32_t accent;
   uint32_t focusBorder;
   uint32_t textPrimary;
   uint32_t textSecondary;
   uint32_t badgeFill;
   uint32_t scrim;
   uint32_t rowHighlight;
   uint32_t seekTrack;
   uint32_t seekNotch;
   uint32_t watchedThumbTint;
   int focusThickness;
} Theme;

typedef enum {
   THEME_PRESET_CRTUBE = 0,
   THEME_PRESET_BLUE,
   THEME_PRESET_GREEN,
   THEME_PRESET_LIGHT,
   THEME_PRESET_MONOCHROME,
   THEME_PRESET_CUSTOM,
   THEME_PRESET_COUNT
} ThemePreset;

extern const Theme *activeTheme;

void initTheme(void);
int getActiveThemePreset(void);
const char *getThemePresetName(int preset);
void getThemePresetPreview(int preset, uint32_t *background, uint32_t *accent);
int getCustomThemeBackgroundDark(void);
uint32_t getCustomThemeAccent(void);
int getThemeAccentPaletteCount(void);
uint32_t getThemeAccentPaletteColor(int index);
int applyThemePreset(int preset);
int saveCustomThemeSettings(int darkBackground, uint32_t accent);
int applyCustomTheme(int darkBackground, uint32_t accent);
