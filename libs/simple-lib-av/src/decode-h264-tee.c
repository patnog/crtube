#include "decode-h264-tee.h"
#include "dbg.h"
#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEE_CLOSE_WAIT_MS 5000

static volatile int teeAvailability = -1;
static volatile int teeCloseDone;
static volatile int teeCloseActive;
static volatile int teeResourceQuarantined;
static volatile int teeDisableRequestPending;
static volatile int teeOperationLock;
static volatile int teeDecoderOpen;
static volatile int teeFlushActive;
static volatile int teeFlushDone;
static volatile int teeFlushResult;

int consumeVideoPlayerTeeDisableRequest(void)
{
   return __sync_lock_test_and_set(&teeDisableRequestPending, 0);
}

void requestH264TeeDisableSetting(const char *reason)
{
   logWarn("[decode-h264] requesting experimental 1080p disable: %s\n",
           reason ? reason : "TEE decoder failure");
   __sync_lock_test_and_set(&teeDisableRequestPending, 1);
}

int isH264TeeBackendIdle(void)
{
   return !teeOperationLock && !teeResourceQuarantined && !teeDecoderOpen;
}

static void reportTeeSafetyFailure(const char *reason)
{
   logError("[decode-h264] TEE safety operation failed or timed out: %s\n", reason);
   __sync_lock_test_and_set(&teeDisableRequestPending, 1);
}

static void teeCloseWorker(uint64_t arg)
{
   tdec *decoder = (tdec *)(uintptr_t)arg;
   tdec_close(decoder);
   __sync_lock_test_and_set(&teeDecoderOpen, 0);
   __sync_lock_test_and_set(&teeCloseActive, 0);
   __sync_lock_test_and_set(&teeOperationLock, 0);
   __sync_synchronize();
   __sync_lock_test_and_set(&teeCloseDone, 1);
   exitThread();
}

/* tdec_close() can wait indefinitely for an SPE that failed to stop. Run it
 * outside the caller (often the playback/UI teardown path) and never free the
 * decoder's memory while that worker may still be using it. */
static int closeTeeBounded(tdec *decoder)
{
   sys_ppu_thread_t thread;

   if (!decoder) return 1;
   if (teeResourceQuarantined || __sync_lock_test_and_set(&teeOperationLock, 1)) {
      /* This decoder cannot be handed to the already-running TEE operation. */
      __sync_lock_test_and_set(&teeResourceQuarantined, 1);
      reportTeeSafetyFailure("another TEE operation is active or resources are quarantined; decoder left allocated");
      return 0;
   }
   __sync_lock_test_and_set(&teeCloseActive, 1);
   __sync_lock_test_and_set(&teeCloseDone, 0);
   if (spawnThread(&thread, teeCloseWorker, (uint64_t)(uintptr_t)decoder,
                   THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "tdec-close") != 0) {
      __sync_lock_test_and_set(&teeCloseActive, 0);
      __sync_lock_test_and_set(&teeOperationLock, 0);
      /* No worker owns this decoder, but it is unsafe to try another TEE while
       * the unclosed instance and its SPE resources remain allocated. */
      __sync_lock_test_and_set(&teeResourceQuarantined, 1);
      reportTeeSafetyFailure("could not create the protected close thread; decoder left allocated");
      return 0;
   }

   for (int waited = 0; waited < TEE_CLOSE_WAIT_MS; waited += 10) {
      if (teeCloseDone) return 1;
      sleepMs(10);
   }
   if (teeCloseDone) return 1;

   /* The detached worker still owns decoder. Do not free it, reuse TEE, or
    * attempt a second close: all three could race an SPE that is still active. */
   reportTeeSafetyFailure("decoder did not close within 5 seconds; decoder left allocated");
   return 0;
}

int isH264TeeAvailable(void)
{
   H264TeeBackend *probe;
   char error[200];

   if (teeResourceQuarantined) {
      reportTeeSafetyFailure("previous TEE decoder resources are quarantined");
      return 0;
   }
   if (teeOperationLock) {
      logInfo("[decode-h264] TEE availability probe deferred while another TEE operation is active\n");
      return 0;
   }
   if (teeDecoderOpen) return 1;  // a successfully opened decoder already proves runtime availability
   if (teeAvailability >= 0) return teeAvailability;

   probe = openH264TeeBackend(1920, 1080, error, sizeof(error));
   if (!probe) {
      /* A concurrent close can make this probe temporarily unavailable. Do
       * not cache that transient state: the user may retry after it completes. */
      if (teeResourceQuarantined) {
         reportTeeSafetyFailure("previous TEE decoder resources are quarantined");
         return 0;
      }
      if (teeOperationLock) {
         logInfo("[decode-h264] TEE availability probe deferred while another TEE operation is active\n");
         return 0;
      }
      if (teeDecoderOpen) return 1;  // another player opened TEE between the probe checks
      teeAvailability = 0;
      logWarn("[decode-h264] TEE availability probe failed: %s\n",
              error[0] ? error : "initialization failed");
      return 0;
   }

   closeH264TeeBackend(probe);
   /* A timed-out probe is not a permanent availability result. Once its close
    * worker finishes, the user may re-enable the setting and try TEE again. */
   if (!isH264TeeBackendIdle()) return 0;
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
   if (teeResourceQuarantined || __sync_lock_test_and_set(&teeOperationLock, 1)) {
      const char *reason = teeResourceQuarantined ? "previous TEE decoder resources are quarantined" :
                                                 "another TEE operation is active";
      if (error && errorSize > 0) snprintf(error, (size_t)errorSize, "%s", reason);
      return NULL;
   }
   if (teeDecoderOpen) {
      __sync_lock_test_and_set(&teeOperationLock, 0);
      if (error && errorSize > 0) snprintf(error, (size_t)errorSize, "another TEE decoder is already open");
      return NULL;
   }

   backend = (H264TeeBackend *)calloc(1, sizeof(*backend));
   if (!backend) {
      __sync_lock_test_and_set(&teeOperationLock, 0);
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
      __sync_lock_test_and_set(&teeOperationLock, 0);
      return NULL;
   }

   __sync_lock_test_and_set(&teeDecoderOpen, 1);
   __sync_lock_test_and_set(&teeOperationLock, 0);
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

int getH264TeeBackendPicturesDecoded(const H264TeeBackend *backend)
{
   return backend && backend->decoder ? tdec_pictures_decoded(backend->decoder) : 0;
}

static void teeFlushWorker(uint64_t arg)
{
   tdec *decoder = (tdec *)(uintptr_t)arg;
   int result = tdec_flush(decoder);
   __sync_lock_test_and_set(&teeFlushResult, result);
   __sync_lock_test_and_set(&teeFlushActive, 0);
   __sync_lock_test_and_set(&teeOperationLock, 0);
   __sync_synchronize();
   __sync_lock_test_and_set(&teeFlushDone, 1);
   exitThread();
}

int flushH264TeeBackend(H264TeeBackend *backend)
{
   const char *why;
   sys_ppu_thread_t thread;

   if (!backend || !backend->decoder) return -1;
   if (teeResourceQuarantined || __sync_lock_test_and_set(&teeOperationLock, 1)) {
      snprintf(backend->error, sizeof(backend->error), "another TEE operation is active or quarantined");
      return -1;
   }
   __sync_lock_test_and_set(&teeFlushActive, 1);
   __sync_lock_test_and_set(&teeFlushDone, 0);
   __sync_lock_test_and_set(&teeFlushResult, -1);
   if (spawnThread(&thread, teeFlushWorker, (uint64_t)(uintptr_t)backend->decoder,
                   THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "tdec-flush") != 0) {
      __sync_lock_test_and_set(&teeFlushActive, 0);
      __sync_lock_test_and_set(&teeOperationLock, 0);
      __sync_lock_test_and_set(&teeResourceQuarantined, 1);
      reportTeeSafetyFailure("could not create the protected flush thread; TEE left quarantined");
      snprintf(backend->error, sizeof(backend->error), "could not start protected TEE flush");
      return -1;
   }

   for (int waited = 0; waited < TEE_CLOSE_WAIT_MS; waited += 10) {
      if (teeFlushDone) break;
      sleepMs(10);
   }
   if (!teeFlushDone) {
      __sync_lock_test_and_set(&teeResourceQuarantined, 1);
      reportTeeSafetyFailure("TEE flush did not complete within 5 seconds; decoder left quarantined");
      snprintf(backend->error, sizeof(backend->error), "TEE flush timed out after 5 seconds");
      return -1;
   }

   if (teeFlushResult == 0) {
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
   int closed = 1;

   if (!backend) return;
   if (backend->decoder) {
      logInfo("[decode-h264] closing TEE backend\n");
      closed = closeTeeBounded(backend->decoder);
      if (closed)
         logInfo("[decode-h264] TEE backend closed\n");
      else
         logError("[decode-h264] TEE decoder remains allocated because close did not complete safely\n");
   }

   /* A close or timed-out flush worker owns only the tdec pointer, not this adapter.
    * The decoder itself is intentionally retained when the operation is unsafe. */
   free(backend);
}
