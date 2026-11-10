#pragma once

#include <stdint.h>
#include "tee_decoder/tee_decoder.h"

typedef struct H264TeeBackend H264TeeBackend;

#define H264_TEE_BACKLOG_LIMIT 48

/* Main-thread settings handoff and TEE resource-state checks. */
void requestH264TeeDisableSetting(const char *reason);
int isH264TeeBackendIdle(void);

/* TEE backend adapter. Input is one complete Annex-B access unit per call. */
H264TeeBackend *openH264TeeBackend(int width, int height, char *error, int errorSize);
int feedH264TeeBackend(H264TeeBackend *backend, const uint8_t *data, int size, uint64_t pts);
int getH264TeeBackendFrame(H264TeeBackend *backend, void *yuvOut,
                           int *outWidth, int *outHeight, uint64_t *outPts);
int getH264TeeBackendBacklog(const H264TeeBackend *backend);
void setH264TeeBackendCatchUpSkip(H264TeeBackend *backend, int enabled);
int getH264TeeBackendPicturesDecoded(const H264TeeBackend *backend);
int flushH264TeeBackend(H264TeeBackend *backend);
const char *getH264TeeBackendError(const H264TeeBackend *backend);
void closeH264TeeBackend(H264TeeBackend *backend);
