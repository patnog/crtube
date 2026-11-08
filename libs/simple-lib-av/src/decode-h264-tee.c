#include "decode-h264-tee.h"
#include "dbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int teeAvailability = -1;

int isH264TeeAvailable(void)
{
   H264TeeBackend *probe;
   char error[200];

   if (teeAvailability >= 0) return teeAvailability;

   probe = openH264TeeBackend(1920, 1080, error, sizeof(error));
   if (!probe) {
      teeAvailability = 0;
      logWarn("[decode-h264] TEE availability probe failed: %s\n",
              error[0] ? error : "initialization failed");
      return 0;
   }

   closeH264TeeBackend(probe);
   teeAvailability = 1;
   logInfo("[decode-h264] TEE backend is available for 1080p50/60\n");
   return 1;
}

struct H264TeeBackend {
   tdec *decoder;
   int fed;
   int output;
   int width;
   int height;
   char error[200];
};

H264TeeBackend *openH264TeeBackend(int width, int height, char *error, int errorSize)
{
   H264TeeBackend *backend;
   tdec_config config;

   if (error && errorSize > 0) error[0] = 0;
   if (width <= 0 || height <= 0 || width > 1920 || height > 1088) {
      if (error && errorSize > 0)
         snprintf(error, (size_t)errorSize, "unsupported coded size %dx%d", width, height);
      return NULL;
   }

   backend = (H264TeeBackend *)calloc(1, sizeof(*backend));
   if (!backend) {
      if (error && errorSize > 0) snprintf(error, (size_t)errorSize, "no memory for TEE adapter");
      return NULL;
   }

   tdec_default(&config);
   config.tier = TDEC_TIER_1;
   config.max_width = width;
   config.max_height = height;
   config.pictures = 12;
   config.input_bytes = 4u << 20;
   config.records_bytes = height > 720 ? (32u << 20) : (8u << 20);
   config.timeout_ms = 2000;
   config.ncabac = 1;
   config.npixel = 3;

   backend->decoder = tdec_open(&config, backend->error, sizeof(backend->error));
   if (!backend->decoder) {
      if (error && errorSize > 0)
         snprintf(error, (size_t)errorSize, "%s",
                  backend->error[0] ? backend->error : "TEE decoder initialization failed");
      free(backend);
      return NULL;
   }

   backend->width = width;
   backend->height = height;
   logInfo("[decode-h264] TEE backend opened for %dx%d\n", width, height);
   return backend;
}

int feedH264TeeBackend(H264TeeBackend *backend, const uint8_t *data, int size, uint64_t pts)
{
   int result;
   const char *why;

   if (!backend || !backend->decoder || !data || size <= 0) return -1;
   why = tdec_error(backend->decoder);
   if (why) {
      snprintf(backend->error, sizeof(backend->error), "%s", why);
      return -1;
   }

   result = tdec_feed(backend->decoder, data, (size_t)size, TDEC_ANNEXB, (int64_t)pts);
   if (result == 1) {
      backend->fed++;
      return 0;
   }
   if (result == 0) return 1;

   snprintf(backend->error, sizeof(backend->error),
            "TEE rejected access unit (%d bytes)", size);
   return -1;
}

int getH264TeeBackendFrame(H264TeeBackend *backend, void *yuvOut,
                           int *outWidth, int *outHeight, uint64_t *outPts)
{
   tdec_picture picture;
   int result;
   const char *why;

   if (!backend || !backend->decoder || !yuvOut) return -1;
   result = tdec_get_picture(backend->decoder, &picture);
   if (result == 0) {
      why = tdec_error(backend->decoder);
      if (why) {
         snprintf(backend->error, sizeof(backend->error), "%s", why);
         return -1;
      }
      return 0;
   }
   if (result < 0) {
      why = tdec_error(backend->decoder);
      snprintf(backend->error, sizeof(backend->error), "%s",
               why ? why : "TEE decoder stopped before end of stream");
      return -1;
   }

   tdec_copy_i420(&picture, (uint8_t *)yuvOut);
   *outWidth = picture.width;
   *outHeight = picture.height;
   *outPts = (uint64_t)picture.pts;
   tdec_release_picture(backend->decoder, &picture);
   backend->output++;
   return 1;
}

int getH264TeeBackendBacklog(const H264TeeBackend *backend)
{
   return backend ? backend->fed - backend->output : 0;
}

int flushH264TeeBackend(H264TeeBackend *backend)
{
   const char *why;
   if (!backend || !backend->decoder) return -1;
   if (tdec_flush(backend->decoder) == 0) {
      backend->fed = 0;
      backend->output = 0;
      backend->error[0] = 0;
      return 0;
   }
   why = tdec_error(backend->decoder);
   snprintf(backend->error, sizeof(backend->error), "%s",
            why ? why : "TEE decoder flush failed");
   return -1;
}

const char *getH264TeeBackendError(const H264TeeBackend *backend)
{
   if (!backend) return "TEE backend is not initialized";
   if (backend->error[0]) return backend->error;
   return backend->decoder ? tdec_error(backend->decoder) : "TEE decoder is not initialized";
}

void closeH264TeeBackend(H264TeeBackend *backend)
{
   if (!backend) return;
   if (backend->decoder) tdec_close(backend->decoder);
   free(backend);
}
