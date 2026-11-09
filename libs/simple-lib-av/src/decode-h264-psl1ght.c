// Native PSL1GHT wrapper around GameOS VDEC H.264.

#include "decode-h264.h"
#include "decode-h264-tee.h"
#include "thread.h"
#include "dbg.h"

#include <codec/vdec.h>
#include <malloc.h>
#include <ppu-asm.h>
#include <ppu-types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/systime.h>
#include <sysmodule/sysmodule.h>

#define PICOUT_TIME_RING 32
#define VDEC_SPU_COUNT 4
#define VDEC_PPU_PRIORITY 300
#define VDEC_SPU_PRIORITY 100
#define VDEC_PPU_STACK_SIZE 0x40000

#define H264_BACKEND_VDEC 0
#define H264_BACKEND_TEE  1

struct H264Decoder {
   int backend;                 // 0 = native PSL1GHT VDEC, 1 = TEE Decoder
   H264TeeBackend *tee;
   u32 handle;
   void *workMemory;
   size_t workMemorySize;
   int width, height;
   sys_lwmutex_t lock;
   opd32 callbackOpd;

   int auFedCount;
   int alternateSkipEnabled;
   volatile int auDoneCount;
   volatile int picPending;
   volatile int seqDone;
   volatile int errored;
   int auRejectedCount;
   int picIncompleteCount;
   int diagAuBeginLogged;
   int diagAuLogged;
   int diagGetPicItemLogged;
   int diagGetPictureLogged;
   int diagFirstAuDoneLogged;
   int diagFirstPicoutLogged;
   int diagFirstSeqDoneLogged;

   uint64_t picoutUs[PICOUT_TIME_RING];
   volatile int picoutWrite;
   int picoutRead;
   uint64_t lastReadyUs;
};

static uint64_t nowUs(void)
{
   uint64_t seconds = 0, nanoseconds = 0;
   if (sysGetCurrentTime(&seconds, &nanoseconds) != 0) return 0;
   return seconds * 1000000ull + nanoseconds / 1000ull;
}

static u32 vdecCallbackPsl(u32 handle, u32 type, u32 data, u32 arg)
{
   (void)handle;
   H264Decoder *decoder = (H264Decoder *)(uintptr_t)arg;
   if (!decoder) return 0;

   lock(&decoder->lock);
   switch (type) {
      case VDEC_CALLBACK_AUDONE:
         decoder->auDoneCount++;
         if ((s32)data != 0) {
            decoder->auRejectedCount++;
            if (decoder->auRejectedCount == 1 || decoder->auRejectedCount % 256 == 0)
               logWarn("[decode-h264] decoder rejected AU #%d, rc=0x%x\n",
                       decoder->auRejectedCount, data);
         }
         break;
      case VDEC_CALLBACK_PICOUT:
         decoder->picoutUs[decoder->picoutWrite & (PICOUT_TIME_RING - 1)] = nowUs();
         decoder->picoutWrite++;
         decoder->picPending++;
         break;
      case VDEC_CALLBACK_SEQDONE:
         decoder->seqDone = 1;
         break;
      case VDEC_CALLBACK_ERROR:
         decoder->errored = 1;
         logError("[decode-h264] VDEC fatal error 0x%x\n", data);
         break;
      default:
         break;
   }
   unlock(&decoder->lock);
   return 0;
}

H264Decoder *createH264Decoder(int width, int height, int level, int maxRefFrames)
{
   H264Decoder *decoder = (H264Decoder *)calloc(1, sizeof *decoder);
   if (!decoder) return NULL;
   decoder->width = width;
   decoder->height = height;
   createLock(&decoder->lock);

   s32 baseRet = sysModuleLoad(SYSMODULE_VDEC);
   s32 avcRet = sysModuleLoad(SYSMODULE_VDEC_H264);
   if ((baseRet != 0 && baseRet != (s32)SYSMODULE_ERR_DUPLICATE) ||
       (avcRet != 0 && avcRet != (s32)SYSMODULE_ERR_DUPLICATE)) {
      logError("[decode-h264] module load failed base=0x%x h264=0x%x\n",
               baseRet, avcRet);
      goto fail;
   }

   vdecType type;
   type.codec_type = VDEC_CODEC_TYPE_H264;
   type.profile_level = (u32)level;

   vdecAttr attr;
   s32 result = vdecQueryAttr(&type, &attr);
   if (result != 0) {
      logError("[decode-h264] QueryAttr failed 0x%x for level %d\n", result, level);
      goto fail;
   }

   // QueryAttr reports the complete work area for this codec level.  SPU count
   // controls execution resources; it is not a memory-size multiplier.
   size_t requested = (size_t)attr.mem_size;
   decoder->workMemorySize = (requested + 0xfffffu) & ~(size_t)0xfffffu;
   decoder->workMemory = memalign(1024 * 1024, decoder->workMemorySize);
   if (!decoder->workMemory) {
      logError("[decode-h264] %u KB arena allocation failed\n",
               (unsigned)(decoder->workMemorySize / 1024));
      goto fail;
   }

   vdecConfig config;
   memset(&config, 0, sizeof config);
   config.mem_addr = (u32)(uintptr_t)decoder->workMemory;
   config.mem_size = (u32)decoder->workMemorySize;
   config.ppu_thread_prio = VDEC_PPU_PRIORITY;
   config.ppu_thread_stack_size = VDEC_PPU_STACK_SIZE;
   config.spu_thread_prio = VDEC_SPU_PRIORITY;
   config.num_spus = VDEC_SPU_COUNT;

   vdecClosure closure;
   closure.fn = __build_opd32((opd64 *)(uintptr_t)(vdecCallback)vdecCallbackPsl,
                              &decoder->callbackOpd);
   closure.arg = (u32)(uintptr_t)decoder;

   logInfo("[vdec-diag] vdecOpen begin size=%dx%d level=%d arena=%u KB\n",
           width, height, level, (unsigned)(decoder->workMemorySize / 1024));
   result = vdecOpen(&type, &config, &closure, &decoder->handle);
   if (result != 0) {
      logError("[decode-h264] vdecOpen failed 0x%x\n", result);
      goto fail;
   }
   logInfo("[vdec-diag] vdecOpen complete handle=0x%x\n", decoder->handle);
   logInfo("[vdec-diag] vdecStartSequence begin\n");
   result = vdecStartSequence(decoder->handle);
   if (result != 0) {
      logError("[decode-h264] StartSequence failed 0x%x\n", result);
      vdecClose(decoder->handle);
      decoder->handle = 0;
      goto fail;
   }
   logInfo("[vdec-diag] vdecStartSequence complete\n");

   logInfo("[decode-h264] PSL1GHT VDEC %dx%d level %d, refs=%d, arena=%u KB\n",
           width, height, level, maxRefFrames,
           (unsigned)(decoder->workMemorySize / 1024));
   return decoder;

fail:
   destroyH264Decoder(decoder);
   return NULL;
}

H264Decoder *createH264SonyDecoder(int width, int height, int level, int maxRefFrames)
{
   if (!isH264TeeBackendIdle()) {
      logWarn("[decode-h264] Sony VDEC start deferred: TEE still owns or may own SPU resources\n");
      return NULL;
   }
   return createH264Decoder(width, height, level, maxRefFrames);
}

H264Decoder *createH264DecoderForStream(int width, int height, int level,
                                        int maxRefFrames, int fps, const H264Config *config)
{
   // TEE Decoder is reserved for progressive 1080p-class H.264 at 50/60 fps.
   // Every SPS/PPS must pass the TEE parser; the Sony SPS-rewrite path remains untouched.
   if (fps >= 50 && width >= 1920 && height >= 1000 &&
       width <= 1920 && height <= 1088) {
      char reason[200];
      if (!isH264TeeCompatible(config, width, height, reason, sizeof(reason))) {
         logInfo("[decode-h264] TEE not selected for %dx%d@%d: %s; using Sony VDEC\n",
                 width, height, fps, reason[0] ? reason : "unsupported stream parameters");
         return createH264SonyDecoder(width, height, level, maxRefFrames);
      }
      if (!isH264TeeBackendIdle()) {
         logWarn("[decode-h264] TEE resources are still active or quarantined; refusing to start another decoder\n");
         return NULL;
      }

      H264Decoder *decoder = (H264Decoder *)calloc(1, sizeof *decoder);
      char error[200];
      if (!decoder) return NULL;
      decoder->backend = H264_BACKEND_TEE;
      decoder->width = width;
      decoder->height = height;
      decoder->tee = openH264TeeBackend(width, height, error, sizeof(error));
      if (!decoder->tee) {
         logWarn("[decode-h264] TEE backend unavailable for %dx%d@%d: %s; using Sony VDEC\n",
                 width, height, fps, error[0] ? error : "initialization failed");
         free(decoder);
         if (!isH264TeeBackendIdle()) return NULL;
         requestH264TeeDisableSetting("TEE initialization failed");
         return createH264SonyDecoder(width, height, level, maxRefFrames);
      }
      logInfo("[decode-h264] selected TEE backend for %dx%d@%d\n", width, height, fps);
      return decoder;
   }

   return createH264SonyDecoder(width, height, level, maxRefFrames);
}

int isH264DecoderTee(const H264Decoder *decoder)
{
   return decoder && decoder->backend == H264_BACKEND_TEE;
}

void setH264DecoderAlternateSkip(H264Decoder *decoder, int enabled)
{
   if (!decoder || decoder->backend == H264_BACKEND_TEE) return;
   decoder->alternateSkipEnabled = enabled != 0;
   if (decoder->alternateSkipEnabled)
      logInfo("[decode-h264] alternating skip-non-reference mode enabled\n");
}

int decodeAuH264(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts)
{
   if (!decoder || !data || size <= 0) return -1;
   if (decoder->backend == H264_BACKEND_TEE)
      return feedH264TeeBackend(decoder->tee, data, size, pts);
   if (decoder->errored) return -1;

   // PSL1GHT's public VDEC binding is known to accept microsecond timestamps.
   uint64_t ptsUs = pts / 1000ull;
   vdecAU au;
   memset(&au, 0, sizeof au);
   au.packet_addr = (u32)(uintptr_t)data;
   au.packet_size = (u32)size;
   au.pts.low = (u32)ptsUs;
   au.pts.hi = (u32)(ptsUs >> 32);
   // The demuxer supplies presentation time only.  Mark DTS unknown so VDEC
   // derives decode order instead of treating B-frame PTS as a decode stamp.
   au.dts.low = VDEC_TS_INVALID;
   au.dts.hi = VDEC_TS_INVALID;
   au.userdata = pts;

   u32 mode = VDEC_DECODER_MODE_NORMAL;
   if (decoder->alternateSkipEnabled && (decoder->auFedCount & 1))
      mode = VDEC_DECODER_MODE_SKIP_NON_REF;
   if (!decoder->diagAuBeginLogged) {
      decoder->diagAuBeginLogged = 1;
      logInfo("[vdec-diag] first vdecDecodeAu begin bytes=%d ptsUs=%llu mode=%u\n",
              size, (unsigned long long)ptsUs, mode);
   }
   s32 result = vdecDecodeAu(decoder->handle, mode, &au);
   if (result == 0) {
      decoder->auFedCount++;
      if (decoder->auFedCount <= 10)
         logInfo("[vdec-diag] AU accepted #%d bytes=%d\n", decoder->auFedCount, size);
      if (!decoder->diagAuLogged) {
         decoder->diagAuLogged = 1;
         logInfo("[vdec-diag] first vdecDecodeAu complete\n");
      }
      return 0;
   }
   if (result == (s32)VDEC_ERROR_BUSY) return 1;
   logError("[decode-h264] DecodeAu failed 0x%x\n", result);
   return -1;
}

int getAuBacklogH264(const H264Decoder *decoder)
{
   if (!decoder) return 0;
   if (decoder->backend == H264_BACKEND_TEE)
      return getH264TeeBackendBacklog(decoder->tee);
   return decoder->auFedCount - decoder->auDoneCount;
}

int getH264DecoderPicturesDecoded(const H264Decoder *decoder)
{
   if (!decoder || decoder->backend != H264_BACKEND_TEE) return 0;
   return getH264TeeBackendPicturesDecoded(decoder->tee);
}

static void discardReportedPictures(H264Decoder *decoder,
                                    const vdecPictureFormat *format)
{
   while (decoder->picPending > 0) {
      lock(&decoder->lock);
      decoder->picPending--;
      unlock(&decoder->lock);
      u32 pictureAddress = 0;
      if (vdecGetPicItem(decoder->handle, &pictureAddress) == 0 && pictureAddress)
         (void)vdecGetPicture(decoder->handle, format, NULL);
   }
}

int resetH264Decoder(H264Decoder *decoder)
{
   if (!decoder) return -1;
   if (decoder->backend == H264_BACKEND_TEE)
      return flushH264TeeBackend(decoder->tee);
   if (!decoder->handle) return -1;
   decoder->seqDone = 0;

   s32 result = 0;
   int tries = 0;
   do {
      result = vdecEndSequence(decoder->handle);
      if (result == (s32)VDEC_ERROR_BUSY) sleepMs(1);
   } while (result == (s32)VDEC_ERROR_BUSY && ++tries < 1000);
   if (result != 0) return -1;

   vdecPictureFormat format = { VDEC_PICFMT_YUV420P, VDEC_COLOR_MATRIX_BT709, 0xff };
   for (int waited = 0; !decoder->seqDone && !decoder->errored && waited < 2000; waited++) {
      discardReportedPictures(decoder, &format);
      sleepMs(1);
   }
   discardReportedPictures(decoder, &format);
   if (decoder->seqDone && !decoder->diagFirstSeqDoneLogged) {
      decoder->diagFirstSeqDoneLogged = 1;
      logInfo("[vdec-diag] first SEQDONE callback observed\n");
   }
   if (!decoder->seqDone) return -1;

   decoder->auFedCount = 0;
   decoder->auDoneCount = 0;
   decoder->picPending = 0;
   decoder->picoutWrite = 0;
   decoder->picoutRead = 0;
   decoder->lastReadyUs = 0;
   decoder->errored = 0;

   for (tries = 0; (result = vdecStartSequence(decoder->handle)) != 0 && tries < 512; tries++)
      sleepMs(1);
   return result == 0 ? 0 : -1;
}

int getFrameH264(H264Decoder *decoder, void *yuvOut, int *outWidth,
                 int *outHeight, uint64_t *outPts)
{
   if (!decoder) return -1;
   if (decoder->backend == H264_BACKEND_TEE)
      return getH264TeeBackendFrame(decoder->tee, yuvOut, outWidth, outHeight, outPts);
   if (decoder->errored) return -1;
   if (!decoder->diagFirstAuDoneLogged && decoder->auDoneCount > 0) {
      decoder->diagFirstAuDoneLogged = 1;
      logInfo("[vdec-diag] first AUDONE callback observed done=%d\n", decoder->auDoneCount);
   }
   if (decoder->picPending <= 0) return 0;
   if (!decoder->diagFirstPicoutLogged) {
      decoder->diagFirstPicoutLogged = 1;
      logInfo("[vdec-diag] first PICOUT callback observed pending=%d\n", decoder->picPending);
   }

   u32 pictureAddress = 0;
   if (!decoder->diagGetPicItemLogged)
      logInfo("[vdec-diag] first vdecGetPicItem begin pending=%d\n", decoder->picPending);
   s32 itemResult = vdecGetPicItem(decoder->handle, &pictureAddress);
   if (!decoder->diagGetPicItemLogged) {
      decoder->diagGetPicItemLogged = 1;
      logInfo("[vdec-diag] first vdecGetPicItem complete rc=0x%x address=0x%x\n",
              itemResult, pictureAddress);
   }
   if (itemResult != 0 || !pictureAddress) return 0;
   const vdecPicture *picture = (const vdecPicture *)(uintptr_t)pictureAddress;

   lock(&decoder->lock);
   decoder->picPending--;
   decoder->lastReadyUs = decoder->picoutUs[decoder->picoutRead & (PICOUT_TIME_RING - 1)];
   decoder->picoutRead++;
   unlock(&decoder->lock);

   if ((s32)picture->status != 0) {
      decoder->picIncompleteCount++;
      if (decoder->picIncompleteCount == 1 || decoder->picIncompleteCount % 256 == 0)
         logWarn("[decode-h264] incomplete picture #%d, status=0x%x\n",
                 decoder->picIncompleteCount, picture->status);
   }

   const vdecH264Info *info = (const vdecH264Info *)(uintptr_t)picture->codec_specific_addr;
   int width = info && info->width ? info->width : decoder->width;
   int height = info && info->height ? info->height : decoder->height;
   *outPts = picture->userdata[0];

   vdecPictureFormat format;
   format.format_type = VDEC_PICFMT_YUV420P;
   format.color_matrix = height >= 720 ? VDEC_COLOR_MATRIX_BT709 : VDEC_COLOR_MATRIX_BT601;
   format.alpha = 0xff;

   if (picture->attr == VDEC_PICTURE_SKIPPED) {
      (void)vdecGetPicture(decoder->handle, &format, NULL);
      return 0;
   }
   if (!decoder->diagGetPictureLogged)
      logInfo("[vdec-diag] first vdecGetPicture begin size=%dx%d\n", width, height);
   s32 result = vdecGetPicture(decoder->handle, &format, yuvOut);
   if (!decoder->diagGetPictureLogged) {
      decoder->diagGetPictureLogged = 1;
      logInfo("[vdec-diag] first vdecGetPicture complete rc=0x%x\n", result);
   }
   if (result != 0) {
      logError("[decode-h264] GetPicture failed 0x%x\n", result);
      return -1;
   }

   *outWidth = width;
   *outHeight = height;
   return 1;
}

uint64_t getDecoderReadyUs(const H264Decoder *decoder)
{
   return decoder && decoder->backend != H264_BACKEND_TEE ? decoder->lastReadyUs : 0;
}

void destroyH264Decoder(H264Decoder *decoder)
{
   if (!decoder) return;
   if (decoder->backend == H264_BACKEND_TEE) {
      closeH264TeeBackend(decoder->tee);
      free(decoder);
      return;
   }
   if (decoder->backend != H264_BACKEND_TEE) {
      logInfo("[vdec-diag] decoder summary fed=%d done=%d picout=%d incomplete=%d firstAuDone=%d firstPicout=%d firstGetPicItem=%d firstGetPicture=%d\n",
              decoder->auFedCount, decoder->auDoneCount, decoder->picoutWrite,
              decoder->picIncompleteCount, decoder->diagFirstAuDoneLogged,
              decoder->diagFirstPicoutLogged, decoder->diagGetPicItemLogged,
              decoder->diagGetPictureLogged);
   }
   if (decoder->handle) {
      (void)vdecEndSequence(decoder->handle);
      (void)vdecClose(decoder->handle);
   }
   free(decoder->workMemory);
   destroyLock(&decoder->lock);
   free(decoder);
}
