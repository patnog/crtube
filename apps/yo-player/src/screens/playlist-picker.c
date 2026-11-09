#include "app-config.h"
#include "screens/playlist-picker.h"
#include "storage.h"
#include "font.h"
#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "ui/label.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "button-repeat.h"
#include "thread.h"
#include "string-utilities.h"
#include <string.h>
#include <stdio.h>

#define PICKER_MAX_ROWS 66
#define PICKER_VISIBLE_ROWS 9
#define PICKER_PANEL_MAX_W 720
#define PICKER_ROW_H 42
#define PICKER_TITLE_H 54
#define PICKER_FOOTER_H 48
#define PICKER_MARGIN 24

typedef struct {
   char id[LOCAL_PLAYLIST_ID_LEN];
   char name[LOCAL_PLAYLIST_NAME_LEN];
} PickerPlaylist;

static struct {
   SearchResult item;
   PickerPlaylist playlists[64];
   int playlistCount;
   int count;
   int selected;
   int scroll;
   int visible;
   int screenW, screenH;
   int panelW, panelH;
   int oskDone;
   char createdName[LOCAL_PLAYLIST_NAME_LEN];
   ButtonRepeat navigateRepeat;
} picker;

static Font pickerFont;
static Label pickerTitle;
static Label pickerRows[PICKER_MAX_ROWS];
static ButtonHints pickerHints;

static void onPlaylistNameEntered(const char *name)
{
   picker.createdName[0] = 0;
   if (name && name[0]) strCopy(picker.createdName, sizeof picker.createdName, name);
   picker.oskDone = 1;
}

static void keepPickerSelectionVisible(void)
{
   if (picker.selected < picker.scroll) picker.scroll = picker.selected;
   else if (picker.selected >= picker.scroll + picker.visible)
      picker.scroll = picker.selected - picker.visible + 1;
   if (picker.scroll < 0) picker.scroll = 0;
   int maxScroll = picker.count - picker.visible;
   if (maxScroll < 0) maxScroll = 0;
   if (picker.scroll > maxScroll) picker.scroll = maxScroll;
}

static void finishPicker(void)
{
   popScreen();
}

static void initPicker(void)
{
   picker.screenW = getGfxScreenWidth();
   picker.screenH = getGfxScreenHeight();
   picker.playlistCount = getLocalPlaylistCount();
   if (picker.playlistCount < 0) picker.playlistCount = 0;
   if (picker.playlistCount > 64) picker.playlistCount = 64;
   for (int i = 0; i < picker.playlistCount; i++)
      getLocalPlaylistInfo(i, picker.playlists[i].id, sizeof picker.playlists[i].id,
                           picker.playlists[i].name, sizeof picker.playlists[i].name);
   picker.count = picker.playlistCount + 2; // Watch Later + local playlists + Create playlist
   picker.selected = 0;
   picker.scroll = 0;
   int maxRows = (picker.screenH - 150) / PICKER_ROW_H;
   if (maxRows < 3) maxRows = 3;
   if (maxRows > PICKER_VISIBLE_ROWS) maxRows = PICKER_VISIBLE_ROWS;
   picker.visible = picker.count < maxRows ? picker.count : maxRows;
   picker.oskDone = 0;
   picker.createdName[0] = 0;
   memset(&picker.navigateRepeat, 0, sizeof picker.navigateRepeat);

   float scale = getGfxUiScaleFactor();
   if (scale < 1.0f) scale = 1.0f;
   picker.panelW = picker.screenW - 48;
   if (picker.panelW > PICKER_PANEL_MAX_W) picker.panelW = PICKER_PANEL_MAX_W;
   picker.panelH = PICKER_TITLE_H + picker.visible * PICKER_ROW_H + PICKER_FOOTER_H;
   if (picker.panelH > picker.screenH - 32) picker.panelH = picker.screenH - 32;

   pickerFont = openSystemFont(FONT_POP);
   initLabel(&pickerTitle, &pickerFont, 0, 0, picker.panelW - 48, AUTO, 26,
             activeTheme->textPrimary, TEXT_NOWRAP, "Add to playlist");
   setLabelScaleWithUi(&pickerTitle, 1);
   for (int i = 0; i < PICKER_MAX_ROWS; i++) {
      const char *name = "";
      if (i == 0) name = "Watch Later";
      else if (i == picker.count - 1) name = "Create playlist";
      else if (i - 1 < picker.playlistCount) name = picker.playlists[i - 1].name;
      initLabel(&pickerRows[i], &pickerFont, 0, 0, picker.panelW - 72, AUTO, 21,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, name);
      setLabelScaleWithUi(&pickerRows[i], 1);
   }
   initButtonHints(&pickerHints, &pickerFont, 0, 28, 15, activeTheme->textSecondary);
   addButtonHint(&pickerHints, getConsoleGlyph(GLYPH_CROSS), "Add");
   addButtonHint(&pickerHints, getConsoleGlyph(GLYPH_CIRCLE), "Cancel");
   setButtonHintsStableLayout(&pickerHints, 1);
   keepPickerSelectionVisible();
}

static void updatePicker(void)
{
   if (oskInputActive()) return;
   if (picker.oskDone) {
      picker.oskDone = 0;
      if (picker.createdName[0]) {
         char id[LOCAL_PLAYLIST_ID_LEN];
         if (createLocalPlaylist(picker.createdName, &picker.item, id, sizeof id) == 0)
            finishPicker();
         picker.createdName[0] = 0;
      }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { finishPicker(); return; }
   if (isRepeatDue(&picker.navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) {
      if (picker.selected + 1 < picker.count) picker.selected++;
      keepPickerSelectionVisible();
   } else if (isRepeatDue(&picker.navigateRepeat, getPadButtonState(PAD_BTN_UP))) {
      if (picker.selected > 0) picker.selected--;
      keepPickerSelectionVisible();
   }
   if (!isPadButtonPressed(PAD_BTN_CROSS)) return;

   if (picker.selected == 0) {
      toggleWatchLater(&picker.item);
      finishPicker();
      return;
   }
   if (picker.selected == picker.count - 1) {
      char defaultName[LOCAL_PLAYLIST_NAME_LEN];
      snprintf(defaultName, sizeof defaultName, "Playlist %d", getLocalPlaylistCount() + 1);
      if (!oskInputBegin("Create playlist", defaultName, onPlaylistNameEntered))
         picker.oskDone = 0;
      return;
   }

   int index = picker.selected - 1;
   if (index >= 0 && index < picker.playlistCount) {
      addVideoToLocalPlaylist(picker.playlists[index].id, &picker.item);
      finishPicker();
   }
}

static void drawPicker(void)
{
   int x = (picker.screenW - picker.panelW) / 2;
   int y = (picker.screenH - picker.panelH) / 2;
   fillGfxRoundedRectangleFixed(x, y, picker.panelW, picker.panelH, 12, activeTheme->appBg);
   fillGfxRoundedRectangleFixed(x + 1, y + 1, picker.panelW - 2, picker.panelH - 2, 11,
                                activeTheme->surface);
   moveLabel(&pickerTitle, x + 24, y + 18);
   drawLabel(&pickerTitle);

   int listY = y + PICKER_TITLE_H;
   for (int row = 0; row < picker.visible; row++) {
      int index = picker.scroll + row;
      int rowY = listY + row * PICKER_ROW_H;
      if (index == picker.selected)
         fillGfxRoundedRectangleFixed(x + 10, rowY + 2, picker.panelW - 20, PICKER_ROW_H - 4,
                                      8, activeTheme->rowHighlight);
      moveLabel(&pickerRows[index], x + 28, rowY + (PICKER_ROW_H - pickerRows[index].tt.tex.h) / 2);
      setLabelColor(&pickerRows[index], index == picker.selected ? activeTheme->accent : activeTheme->textPrimary);
      drawLabel(&pickerRows[index]);
   }
   pickerHints.y = y + picker.panelH - PICKER_FOOTER_H + 8;
   drawButtonHints(&pickerHints, picker.screenW);
}

static void termPicker(void)
{
   for (int i = 0; i < PICKER_MAX_ROWS; i++) freeLabel(&pickerRows[i]);
   freeLabel(&pickerTitle);
   termButtonHints(&pickerHints);
   closeFont(&pickerFont);
}

void openLocalPlaylistPicker(const SearchResult *item)
{
   if (!item || !item->videoId[0] || item->isPlaylist) return;
   picker.item = *item;
   pushScreen(&playlistPickerScreen);
}

Screen playlistPickerScreen = { initPicker, NULL, updatePicker, drawPicker, NULL, termPicker, SCREEN_TERMINATED };
