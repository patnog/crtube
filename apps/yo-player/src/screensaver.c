#include "screensaver.h"

#include "app-config.h"
#include "gfx.h"
#include "image-loader.h"
#include "thread.h"
#include "dbg.h"

#include <math.h>
#include <stdint.h>

#define SCREENSAVER_LOGO_PATH "/dev_hdd0/game/CRTUBE001/USRDIR/BOUNCING_LOGO.JPG"
#define SCREENSAVER_LOGO_HEIGHT 0.27f
#define SCREENSAVER_SPEED_X 241.875f
#define SCREENSAVER_SPEED_Y 174.375f
#define SCREENSAVER_MAX_FRAME_SECONDS 0.25f

static GfxTexture logoTexture;
static int logoLoaded;
static int logoRequested;
static int logoFailed;
static int active;
static float logoX;
static float logoY;
static float velocityX = SCREENSAVER_SPEED_X;
static float velocityY = SCREENSAVER_SPEED_Y;
static uint64_t lastUpdateUs;

static void updateLogoSize(int *width, int *height)
{
   int screenW = getGfxScreenWidth();
   int screenH = getGfxScreenHeight();
   *height = (int)(screenH * SCREENSAVER_LOGO_HEIGHT);
   if (*height < 1) *height = 1;
   *width = logoTexture.h > 0
      ? (int)((float)*height * (float)logoTexture.w / (float)logoTexture.h)
      : *height;
   if (*width < 1) *width = 1;
   if (*width > screenW) {
      *width = screenW;
      *height = logoTexture.w > 0
         ? (int)((float)*width * (float)logoTexture.h / (float)logoTexture.w)
         : screenH;
   }
}

static void reflectAxis(float *position, float *velocity, float maximum)
{
   if (maximum <= 0.0f) {
      *position = 0.0f;
      *velocity = fabsf(*velocity);
      return;
   }

   while (*position < 0.0f || *position > maximum) {
      if (*position < 0.0f) {
         *position = -*position;
         *velocity = fabsf(*velocity);
      } else {
         *position = maximum - (*position - maximum);
         *velocity = -fabsf(*velocity);
      }
   }
}

static void pollScreenSaverLogo(void)
{
   if (!logoRequested || logoLoaded || logoFailed) return;

   ImageBuffer decoded = { 0, 0, 0, 0 };
   int result = pollImagePriorityAsync(&decoded);
   if (result == 0) return;

   logoRequested = 0;
   if (result < 0) {
      logoFailed = 1;
      logError("[screensaver] could not decode %s\n", SCREENSAVER_LOGO_PATH);
      return;
   }

   // The shared image-loader worker decodes to heap memory; upload remains on the main thread.
   logoTexture = uploadImageBuffer(&decoded);
   freeImageBuffer(&decoded);
   logoLoaded = logoTexture.offset != 0 && logoTexture.w > 0 && logoTexture.h > 0;
   if (!logoLoaded) {
      logoFailed = 1;
      logError("[screensaver] texture upload failed for %s\n", SCREENSAVER_LOGO_PATH);
      return;
   }

   int width, height;
   updateLogoSize(&width, &height);
   logoX = (float)(getGfxScreenWidth() - width) * 0.5f;
   logoY = (float)(getGfxScreenHeight() - height) * 0.5f;
   lastUpdateUs = getTimeUs();
   logInfo("[screensaver] logo ready (%dx%d)\n", logoTexture.w, logoTexture.h);
}

void termScreenSaver(void)
{
   active = 0;
   logoRequested = 0;
   cancelImagePriorityAsync();
   if (logoLoaded) {
      freeGfxTexture(&logoTexture);
      logoLoaded = 0;
   }
   lastUpdateUs = 0;
}

void startScreenSaver(void)
{
   static int activationWaitLogged;
   if (active) return;

   if (!logoLoaded) {
      if (!logoRequested && !logoFailed) {
         if (requestImagePriorityAsync(SCREENSAVER_LOGO_PATH) == 0) {
            logoRequested = 1;
            logInfo("[screensaver] logo queued at high priority\n");
         } else {
            logoFailed = 1;
            logError("[screensaver] could not queue logo decode\n");
         }
      }
      if (!activationWaitLogged) {
         logWarn("[screensaver] waiting for logo (requested=%d failed=%d)\n",
            logoRequested, logoFailed);
         activationWaitLogged = 1;
      }
      return;
   }

   activationWaitLogged = 0;
   int width, height;
   updateLogoSize(&width, &height);
   logoX = (float)(getGfxScreenWidth() - width) * 0.5f;
   logoY = (float)(getGfxScreenHeight() - height) * 0.5f;
   velocityX = SCREENSAVER_SPEED_X;
   velocityY = SCREENSAVER_SPEED_Y;
   lastUpdateUs = getTimeUs();
   active = 1;
   logInfo("[screensaver] activated\n");
}

void stopScreenSaver(void)
{
   if (active) logInfo("[screensaver] dismissed by input\n");
   active = 0;
   lastUpdateUs = 0;
}

int isScreenSaverActive(void)
{
   return active;
}

void updateScreenSaver(void)
{
   pollScreenSaverLogo();
   if (!active || !logoLoaded) return;

   uint64_t now = getTimeUs();
   if (!lastUpdateUs) {
      lastUpdateUs = now;
      return;
   }

   float elapsed = (float)(now - lastUpdateUs) / 1000000.0f;
   lastUpdateUs = now;
   if (elapsed < 0.0f) elapsed = 0.0f;
   if (elapsed > SCREENSAVER_MAX_FRAME_SECONDS)
      elapsed = SCREENSAVER_MAX_FRAME_SECONDS;

   int width, height;
   updateLogoSize(&width, &height);
   float maxX = (float)(getGfxScreenWidth() - width);
   float maxY = (float)(getGfxScreenHeight() - height);

   logoX += velocityX * elapsed;
   logoY += velocityY * elapsed;
   reflectAxis(&logoX, &velocityX, maxX);
   reflectAxis(&logoY, &velocityY, maxY);
}

void drawScreenSaver(void)
{
   clearGfx(0xFF000000u);
   if (!active || !logoLoaded) return;

   int width, height;
   updateLogoSize(&width, &height);
   drawGfxTextureFixedSize(
      (int)logoX, (int)logoY, width, height,
      logoTexture, 0.0f, 0.0f, 1.0f, 1.0f,
      0xFFFFFFFFu, GFX_FILTER_LINEAR
   );
}
