#pragma once

#include <stdint.h>
#include "gfx.h"
#include "theme.h"
#include "thread.h"

/* Eight-dot rotating indicator, with contrast selected from the theme background. */
static inline void drawLoadingIndicator(int centerX, int centerY, uint64_t *startedUs)
{
   static const int offsetX[8] = { 0, 24, 34, 24, 0, -24, -34, -24 };
   static const int offsetY[8] = { -34, -24, 0, 24, 34, 24, 0, -24 };
   static const unsigned char levels[8] = { 255, 224, 176, 128, 88, 56, 32, 16 };
   uint32_t bg = activeTheme->appBg;
   unsigned r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
   int lightBackground = (299 * r + 587 * g + 114 * b) / 1000 >= 150;
   uint64_t now = getTimeUs();
   if (!*startedUs) *startedUs = now;
   unsigned active = (unsigned)(((now - *startedUs) / 80000ULL) & 7ULL);

   for (unsigned i = 0; i < 8; ++i) {
      unsigned age = (i + 8 - active) & 7;
      unsigned value = levels[age];
      if (lightBackground) value = 255 - value;
      uint32_t color = 0xFF000000u | (value << 16) | (value << 8) | value;
      fillGfxCircle(centerX + offsetX[i], centerY + offsetY[i], 12, color);
   }
}
