// button-hints - centered "[glyph] caption" pad-hint row (see ui/button-hints.h).

#include "ui/button-hints.h"
#include "string-utilities.h"
#include <string.h>

#define ICON_GAP 8    // between a glyph and its caption
#define HINT_GAP 30   // between one hint and the next
#define PAIR_GAP 8    // between two caption-less glyphs that form a pair (L1 + R1)

void initButtonHints(ButtonHints *bar, Font *font, int y, int glyphHeight, int captionSize, uint32_t captionColor)
{
   memset(bar, 0, sizeof *bar);
   bar->font         = font;
   bar->y            = y;
   bar->glyphHeight  = glyphHeight;
   bar->captionSize  = captionSize;
   bar->captionColor = captionColor;
}

int addButtonHint(ButtonHints *bar, GfxTexture glyph, const char *caption)
{
   if (bar->count >= MAX_BUTTON_HINTS) return -1;
   ButtonHint *hint = &bar->hints[bar->count++];
   hint->glyph     = glyph;
   hint->drawWidth = glyph.h > 0 ? glyph.w * bar->glyphHeight / glyph.h : bar->glyphHeight;
   hint->pairNext  = (caption == NULL || caption[0] == 0);
   initLabel(&hint->caption, bar->font, 0, 0, AUTO, AUTO, bar->captionSize, bar->captionColor, TEXT_NOWRAP, caption);
   // a caption-less hint is just the glyph; drop the icon gap so it clusters with the next one
   hint->width = hint->drawWidth + (hint->pairNext ? 0 : ICON_GAP + hint->caption.tt.tex.w);
   return bar->count - 1;
}

void setButtonHintCaption(ButtonHints *bar, int index, const char *caption)
{
   if (index < 0 || index >= bar->count) return;
   ButtonHint *hint = &bar->hints[index];
   setLabelText(&hint->caption, caption);
   hint->pairNext = (caption == NULL || caption[0] == 0);
   hint->width = hint->drawWidth + (hint->pairNext ? 0 : ICON_GAP + hint->caption.tt.tex.w);
}

// UI geometry is transformed by gfx.c after these coordinates are submitted.
// Stable SD hint rows compensate their anchors and spacing before drawing so
// the enlarged glyphs/captions remain inside the screen-safe footer area.
static float hintScale(const ButtonHints *bar)
{
   float scale = bar->stableLayout ? getGfxUiScaleFactor() : 1.0f;
   return scale < 1.0f ? 1.0f : scale;
}

static float hintCurrentScale(void)
{
   float scale = getGfxUiScaleFactor();
   return scale < 1.0f ? 1.0f : scale;
}

static int hintGlyphSize(const ButtonHints *bar, int baseSize)
{
   float fit = bar->fitScale > 0.0f ? bar->fitScale : 1.0f;
   if (!isGfxSdVideoOutput()) return (int)(baseSize * fit + 0.5f);
   return (int)(baseSize * 1.5f * fit / hintCurrentScale() + 0.5f);
}

// Captions use the normal transformed draw path. Compensate their raster size
// for the current UI zoom so their final on-screen size remains 1.5x.
static void refreshHintCaptionSizes(ButtonHints *bar)
{
   float scale = isGfxSdVideoOutput() ? hintCurrentScale() : 1.0f;
   float fit = bar->fitScale > 0.0f ? bar->fitScale : 1.0f;
   if (bar->renderedUiScale == scale && bar->renderedFitScale == fit) return;

   int size = isGfxSdVideoOutput()
      ? (int)(bar->captionSize * 1.5f * fit / scale + 0.5f)
      : (int)(bar->captionSize * fit + 0.5f);
   if (size < 1) size = 1;
   for (int i = 0; i < bar->count; i++) {
      Label *caption = &bar->hints[i].caption;
      if (caption->size == size) continue;
      char text[LABEL_MAX_TEXT];
      strCopy(text, sizeof text, caption->text);
      freeLabel(caption);
      caption->size = size;
      setLabelText(caption, text);
   }
   bar->renderedUiScale = scale;
   bar->renderedFitScale = fit;
}

static int hintIconGap(const ButtonHints *bar)
{
   float scale = hintScale(bar);
   float fit = bar->fitScale > 0.0f ? bar->fitScale : 1.0f;
   return bar->stableLayout ? (int)(ICON_GAP * fit / scale + 0.5f) : (int)(ICON_GAP * fit + 0.5f);
}

static int hintRowGap(const ButtonHints *bar, int paired)
{
   int base = paired ? PAIR_GAP : (bar->gap > 0 ? bar->gap : HINT_GAP);
   float scale = hintScale(bar);
   float fit = bar->fitScale > 0.0f ? bar->fitScale : 1.0f;
   return bar->stableLayout ? (int)(base * fit / scale + 0.5f) : (int)(base * fit + 0.5f);
}

static int hintDrawY(const ButtonHints *bar)
{
   if (!bar->stableLayout) return bar->y;
   float scale = hintScale(bar);
   int screenH = getGfxScreenHeight();
   float center = screenH * 0.5f;
   // Keep the rendered top edge at the caller's reserved footer coordinate;
   // the glyph and caption themselves still grow with the selected zoom.
   return (int)(center + (bar->y - center) / scale + 0.5f);
}

static int drawHint(ButtonHints *bar, ButtonHint *hint, int x, int y)
{
   int glyphH = hintGlyphSize(bar, bar->glyphHeight);
   int glyphW = hintGlyphSize(bar, hint->drawWidth);
   int rowMid = y + glyphH / 2;
   if (hint->glyph.offset)
      drawGfxTexture(x, y, glyphW, glyphH, hint->glyph, 0, 0, 1, 1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
   x += glyphW;
   if (hint->pairNext) return x;   // glyph only - the caption belongs to the hint it pairs with
   x += hintIconGap(bar);
   moveLabel(&hint->caption, x, rowMid - hint->caption.tt.tex.h / 2);
   drawLabel(&hint->caption);
   return x + hint->caption.tt.tex.w;
}

void setButtonHintGap(ButtonHints *bar, int gap)
{
   bar->gap = gap;
}

void setButtonHintsStableLayout(ButtonHints *bar, int enabled)
{
   if (bar) {
      bar->stableLayout = enabled != 0;
      bar->renderedUiScale = 0.0f;
   }
}

void setButtonHintShown(ButtonHints *bar, int index, int shown)
{
   if (index < 0 || index >= bar->count) return;
   bar->hints[index].hidden = !shown;
}

void drawButtonHintsAt(ButtonHints *bar, int x)
{
   refreshHintCaptionSizes(bar);
   int y = hintDrawY(bar);
   for (int i = 0; i < bar->count; i++) {
      if (bar->hints[i].hidden) continue;

      x = drawHint(bar, &bar->hints[i], x, y);
      x += hintRowGap(bar, bar->hints[i].pairNext);
   }
}

void drawButtonHints(ButtonHints *bar, int screenWidth)
{
   refreshHintCaptionSizes(bar);
   int total = 0;
   for (int i = 0; i < bar->count; i++) {
      if (bar->hints[i].hidden) continue;

      ButtonHint *hint = &bar->hints[i];
      total += hintGlyphSize(bar, hint->drawWidth);
      if (!hint->pairNext) total += hintIconGap(bar) + hint->caption.tt.tex.w;
      if (i < bar->count - 1) total += hintRowGap(bar, hint->pairNext);
   }

   drawButtonHintsAt(bar, (screenWidth - total) / 2);
}

#define HINT_FIT_MAX_ROWS 2

typedef struct {
   int indices[MAX_BUTTON_HINTS];
   int counts[HINT_FIT_MAX_ROWS];
   int widths[HINT_FIT_MAX_ROWS];
   int rows;
} HintFitLayout;

static int hintFitRowWidth(const ButtonHints *bar, const int *widths,
                           const int *indices, int start, int end)
{
   int total = 0;
   for (int i = start; i < end; i++) {
      total += widths[indices[i]];
      if (i + 1 < end)
         total += hintRowGap(bar, bar->hints[indices[i]].pairNext);
   }
   return total;
}

static int buildHintFitLayoutAtScale(ButtonHints *bar, int screenWidth, int maxRows,
                                     HintFitLayout *layout)
{
   int widths[MAX_BUTTON_HINTS];
   int visible = 0;
   const int inf = 0x3fffffff;
   int dp[HINT_FIT_MAX_ROWS + 1][MAX_BUTTON_HINTS + 1];
   int previous[HINT_FIT_MAX_ROWS + 1][MAX_BUTTON_HINTS + 1];

   memset(layout, 0, sizeof *layout);
   refreshHintCaptionSizes(bar);
   if (maxRows < 1) maxRows = 1;
   if (maxRows > HINT_FIT_MAX_ROWS) maxRows = HINT_FIT_MAX_ROWS;

   for (int i = 0; i < bar->count; i++) {
      ButtonHint *hint = &bar->hints[i];
      if (hint->hidden) continue;
      layout->indices[visible] = i;
      widths[i] = hintGlyphSize(bar, hint->drawWidth);
      if (!hint->pairNext)
         widths[i] += hintIconGap(bar) + hint->caption.tt.tex.w;
      visible++;
   }
   if (visible == 0) return 0;

   for (int r = 0; r <= HINT_FIT_MAX_ROWS; r++)
      for (int i = 0; i <= MAX_BUTTON_HINTS; i++) {
         dp[r][i] = inf;
         previous[r][i] = -1;
      }
   dp[0][0] = 0;

   for (int r = 1; r <= maxRows; r++) {
      for (int end = r; end <= visible; end++) {
         for (int start = r - 1; start < end; start++) {
            if (dp[r - 1][start] == inf) continue;
            int rowWidth = hintFitRowWidth(bar, widths, layout->indices, start, end);
            int candidate = dp[r - 1][start] > rowWidth ? dp[r - 1][start] : rowWidth;
            if (candidate < dp[r][end]) {
               dp[r][end] = candidate;
               previous[r][end] = start;
            }
         }
      }
   }

   float scale = hintCurrentScale();
   int capacity = (int)(screenWidth / scale + 0.5f);
   int chosen = 0;
   for (int r = 1; r <= maxRows; r++) {
      if (dp[r][visible] <= capacity) { chosen = r; break; }
   }
   if (!chosen) {
      chosen = 1;
      for (int r = 2; r <= maxRows; r++)
         if (dp[r][visible] < dp[chosen][visible]) chosen = r;
   }

   int end = visible;
   for (int r = chosen; r >= 1; r--) {
      int start = previous[r][end];
      if (start < 0) start = 0;
      layout->counts[r - 1] = end - start;
      layout->widths[r - 1] = hintFitRowWidth(bar, widths, layout->indices, start, end);
      end = start;
   }
   layout->rows = chosen;
   return chosen;
}

static int buildHintFitLayout(ButtonHints *bar, int screenWidth, int maxRows,
                              HintFitLayout *layout)
{
   if (!bar) return 0;
   if (maxRows < 1) maxRows = 1;
   if (maxRows > 2) maxRows = 2;

   // First preserve the configured size. If two rows still exceed the safe
   // horizontal area, progressively scale glyphs, captions and gaps together.
   float initialFit = bar->fitScale > 0.0f ? bar->fitScale : 1.0f;
   for (int step = 0; step <= 18; step++) {
      bar->fitScale = initialFit - step * 0.05f;
      if (bar->fitScale < 0.1f) bar->fitScale = 0.1f;
      bar->renderedUiScale = 0.0f;
      refreshHintCaptionSizes(bar);
      int rows = buildHintFitLayoutAtScale(bar, screenWidth, maxRows, layout);
      int widest = 0;
      for (int i = 0; i < rows; i++)
         if (layout->widths[i] > widest) widest = layout->widths[i];
      float scale = hintCurrentScale();
      int capacity = (int)(screenWidth / scale + 0.5f);
      if (widest <= capacity || step == 18 || bar->fitScale <= 0.1f) return rows;
   }
   return 0;
}

int getButtonHintsFitRows(ButtonHints *bar, int screenWidth, int maxRows)
{
   HintFitLayout layout;
   if (!bar) return 0;
   return buildHintFitLayout(bar, screenWidth, maxRows, &layout);
}

int drawButtonHintsFitted(ButtonHints *bar, int screenWidth, int bottomY, int rowGap, int maxRows)
{
   HintFitLayout layout;
   int rows = buildHintFitLayout(bar, screenWidth, maxRows, &layout);
   if (rows <= 0) return 0;

   float scale = hintCurrentScale();
   float centerY = getGfxScreenHeight() * 0.5f;
   int glyphHeight = (int)(hintGlyphSize(bar, bar->glyphHeight) * scale + 0.5f);
   float fit = bar->fitScale > 0.0f ? bar->fitScale : 1.0f;
   int rowStep = glyphHeight + (int)(rowGap * fit + 0.5f);
   int rowOffset = 0;
   for (int row = 0; row < rows; row++) {
      int targetY = bottomY - (rows - 1 - row) * rowStep;
      int y = (int)(centerY + (targetY - centerY) / scale + 0.5f);
      int x = (screenWidth - layout.widths[row]) / 2;
      for (int j = 0; j < layout.counts[row]; j++) {
         int hintIndex = layout.indices[rowOffset++];
         x = drawHint(bar, &bar->hints[hintIndex], x, y);
         if (j + 1 < layout.counts[row])
            x += hintRowGap(bar, bar->hints[hintIndex].pairNext);
      }
   }
   return rows;
}

void termButtonHints(ButtonHints *bar)
{
   for (int i = 0; i < bar->count; i++) freeLabel(&bar->hints[i].caption);
   bar->count = 0;
}
