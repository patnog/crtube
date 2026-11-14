// http - transport-agnostic HTTP client: transport binding + one-shot request/response (see http.h).
//
// This TU is the LIGHT half: binding a transport, the url predicate, and the buffered request/response
// helpers. It uses no malloc and spawns no threads, so a malloc-free VSH PRX can call isHttpUrl / fetchHttp
// without dragging in libc's heap. The seekable streaming engine (malloc + 4 MB ring + prefetch thread)
// lives in http-stream.c and is pulled only by a caller that references openHttpStream (see http-internal.h).

#include "http.h"
#include "http-internal.h"   // DEFAULT_UA, getActiveTransport
#include "dbg.h"

#include <string.h>
#include <sys/sys_time.h>

#define MAX_STALL_RETRIES 15   // consecutive transient stalls tolerated before a one-shot body is abandoned
#define LIVE_SEGMENT_TIMEOUT_US 20000000ull // hard cap for one DASH segment request, including slow trickle bodies

static const HttpTransport *transport;
static int                 transportLatched;   // set once a modern (override) transport claimed the slot

void bindHttpTransport(const HttpTransport *ops, int override)
{
   if (transportLatched && !override) return;   // modern already won; ignore a later system bind
   transport = ops;
   if (override) transportLatched = 1;
}

const HttpTransport *getActiveTransport(void) { return transport; }

void shutdownHttp(void)
{
   if (transport && transport->shutdown) transport->shutdown();
}

void setHttpRequestsCancelled(int cancelled)
{
   if (transport && transport->setRequestsCancelled)
      transport->setRequestsCancelled(cancelled != 0);
}

void setHttpRequestGroup(HttpRequestGroup *group)
{
   if (transport && transport->setRequestGroup) transport->setRequestGroup(group);
}

void cancelHttpRequestGroup(HttpRequestGroup *group)
{
   if (!group) return;
   group->cancelled = 1;
   __sync_synchronize();
   if (transport && transport->cancelRequestGroup) transport->cancelRequestGroup(group);
}

int isHttpUrl(const char *path)
{
   return strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0;
}

// section: one-shot request/response

int fetchHttp(const char *method, const char *url, const HttpHeader *headers, int headerCount,
              const void *body, int bodyLen, char *out, int cap, int *outLen, int *status)
{
   return fetchHttpCapturing(method, url, headers, headerCount, body, bodyLen, out, cap, outLen, status, NULL);
}

int fetchHttpCapturing(const char *method, const char *url, const HttpHeader *headers, int headerCount,
                       const void *body, int bodyLen, char *out, int cap, int *outLen, int *status,
                       HttpHeaderCapture *capture)
{
   if (outLen) *outLen = 0;
   if (status) *status = 0;
   if (capture) capture->value[0] = '\0';
   if (!transport) { logError("[http] no transport bound\n"); return -1; }

   int liveSegment = url && strstr(url, "googlevideo.com") && strstr(url, "/videoplayback/") &&
                     (strstr(url, "/sq/") || strstr(url, "&sq="));
   long liveSq = -1;
   if (liveSegment) {
      const char *sqText = strstr(url, "/sq/");
      if (sqText) sqText += 4;
      else {
         sqText = strstr(url, "&sq=");
         if (sqText) sqText += 4;
      }
      if (sqText) {
         liveSq = 0;
         while (*sqText >= '0' && *sqText <= '9') liveSq = liveSq * 10 + (*sqText++ - '0');
      }
   }
   const char *liveType = liveSegment && (strstr(url, "audio%2F") || strstr(url, "audio/mp4")) ? "audio" : "video";
   uint64_t requestStartUs = liveSegment ? sys_time_get_system_time() : 0;
   void *handle = transport->open(method, url, headers, headerCount, body, bodyLen, status, NULL);
   uint64_t openedUs = liveSegment ? sys_time_get_system_time() : 0;
   if (!handle) {
      if (liveSegment)
         logWarn("[live-http-diag] type=%s sq=%ld open-failed openMs=%llu\n", liveType, liveSq,
                 (unsigned long long)((openedUs-requestStartUs)/1000ull));
      return -1;
   }

   char contentLength[32] = "", contentRange[96] = "", transferEncoding[32] = "", contentType[64] = "";
   if (liveSegment && transport->getResponseHeader) {
      transport->getResponseHeader(handle, "content-length", contentLength, sizeof contentLength);
      transport->getResponseHeader(handle, "content-range", contentRange, sizeof contentRange);
      transport->getResponseHeader(handle, "transfer-encoding", transferEncoding, sizeof transferEncoding);
      transport->getResponseHeader(handle, "content-type", contentType, sizeof contentType);
   }
   if (capture && transport->getResponseHeader)
      transport->getResponseHeader(handle, capture->name, capture->value, capture->cap);

   int written = 0, stalls = 0, failed = 0;
   uint64_t firstByteUs = 0, lastByteUs = 0, maxReadCallUs = 0, maxInterReadUs = 0;
   uint64_t readCallUs = 0;
   unsigned int readCalls = 0, stallReads = 0;
   int nextProgressBytes = 256 * 1024;
   uint64_t lastProgressLogUs = openedUs;
   while (cap > 0 && written < cap - 1) {
      if (liveSegment && sys_time_get_system_time() - requestStartUs >= LIVE_SEGMENT_TIMEOUT_US) {
         logWarn("[live-http-diag] type=%s sq=%ld total request deadline exceeded after %llu ms\n",
                 liveType, liveSq,
                 (unsigned long long)((sys_time_get_system_time()-requestStartUs)/1000ull));
         failed = 1;
         break;
      }
      uint64_t readStartUs = liveSegment ? sys_time_get_system_time() : 0;
      int64_t got = transport->read(handle, out + written, (uint64_t)(cap - 1 - written));
      uint64_t readEndUs = liveSegment ? sys_time_get_system_time() : 0;
      if (liveSegment) {
         uint64_t duration = readEndUs - readStartUs;
         readCallUs += duration;
         if (duration > maxReadCallUs) maxReadCallUs = duration;
         if (got > 0) {
            if (!firstByteUs) firstByteUs = readEndUs;
            if (lastByteUs && readStartUs > lastByteUs && readStartUs-lastByteUs > maxInterReadUs)
               maxInterReadUs = readStartUs-lastByteUs;
            lastByteUs = readEndUs;
            readCalls++;
         } else if (got == -2) {
            stallReads++;
         }
      }
      if (got == -2) { if (++stalls > MAX_STALL_RETRIES) { failed = 1; break; } continue; }   // gave up waiting: body incomplete
      if (got == -1) { failed = 1; break; }                                    // connection died mid-body
      if (got == 0) break;                                                     // clean end of body
      stalls = 0;
      written += (int)got;
      if (liveSegment) {
         uint64_t progressUs = sys_time_get_system_time();
         if (written >= nextProgressBytes || progressUs - lastProgressLogUs >= 500000ull) {
            logInfo("[live-http-progress] type=%s sq=%ld elapsedMs=%llu bytes=%d readCalls=%u\n",
                    liveType, liveSq, (unsigned long long)((progressUs-openedUs)/1000ull), written, readCalls);
            nextProgressBytes = ((written / (256 * 1024)) + 1) * (256 * 1024);
            lastProgressLogUs = progressUs;
         }
      }
   }
   if (cap > 0) out[written] = '\0';
   if (outLen) *outLen = written;

   if (liveSegment) {
      uint64_t doneUs = sys_time_get_system_time();
      logInfo("[live-http-diag] type=%s sq=%ld status=%d result=%d bytes=%d openMs=%llu firstByteMs=%llu bodyMs=%llu totalMs=%llu readCalls=%u stallReads=%u readTimeMs=%llu maxReadMs=%llu maxGapMs=%llu contentLength=%s contentRange=%s transferEncoding=%s contentType=%s\n",
              liveType, liveSq, status ? *status : 0, failed ? -1 : 0, written,
              (unsigned long long)((openedUs-requestStartUs)/1000ull),
              (unsigned long long)(firstByteUs ? (firstByteUs-openedUs)/1000ull : 0),
              (unsigned long long)(firstByteUs ? (doneUs-firstByteUs)/1000ull : 0),
              (unsigned long long)((doneUs-requestStartUs)/1000ull), readCalls, stallReads,
              (unsigned long long)(readCallUs/1000ull), (unsigned long long)(maxReadCallUs/1000ull),
              (unsigned long long)(maxInterReadUs/1000ull), contentLength[0] ? contentLength : "-",
              contentRange[0] ? contentRange : "-", transferEncoding[0] ? transferEncoding : "-",
              contentType[0] ? contentType : "-");
   }
   transport->close(handle);
   // a transport death or a stall we gave up on means the body is truncated; the http.h contract promises a
   // negative return on transport failure so callers don't parse a partial body as a complete response.
   return failed ? -1 : 0;
}

int getHttp(const char *url, char *out, int cap, int *outLen, int *status)
{
   HttpHeader userAgent = { "User-Agent", DEFAULT_UA };
   return fetchHttp("GET", url, &userAgent, 1, NULL, 0, out, cap, outLen, status);
}
