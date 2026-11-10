// stream-select - format selection shared by playback and download (see stream-select.h).

#include "stream-select.h"
#include "settings.h"
#include "decode-h264.h"
#include "dbg.h"
#include <string.h>

// Sony VDEC remains limited to 1080p30 / 720p60. Only route 1080p50/60
// candidates into the selector when the separate TEE backend passes its
// one-time runtime initialization probe.
static int decodableVideo(const StreamFormat *format, int allowTee)
{
   if (format->height <= 0 || format->fps <= 0 || format->fps > 60) return 0;
   if (format->height <= 720) return 1;
   if (format->height <= 1080 && format->fps <= 30) return 1;
   return format->width >= 1920 && format->height == 1080 &&
          format->fps >= 50 && allowTee && isH264TeeAvailable();
}

static const StreamFormat *pickBestVideoInternal(const StreamInfo *info, int allowTee)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;   // mp4 == avc1 for these itags
      if (!decodableVideo(format, allowTee)) continue;
      if (!best || format->height > best->height || (format->height == best->height && format->fps > best->fps))
         best = format;
   }

   // Only dump candidates when selection fails. This keeps normal logs unchanged and
   // bounds one diagnostic burst to MAX_STREAM_FORMATS short lines.
   if (!best) {
      logError("[yt] VIDEO SELECT FAILED: formats=%d\n", info->formatCount);
      for (int i = 0; i < info->formatCount; i++) {
         const StreamFormat *f = &info->formats[i];
         const char *reason = "not-video";
         if (f->hasVideo) {
            if (f->needsCipher) reason = "cipher";
            else if (!f->url[0]) reason = "no-url";
            else if (strcmp(f->container, "mp4") != 0) reason = "container";
            else if (!decodableVideo(f, allowTee)) reason = "resolution-fps";
            else reason = "eligible-check-selector";
         } else if (f->hasAudio) {
            reason = f->needsCipher ? "audio-cipher" : (!f->url[0] ? "audio-no-url" : "audio-only");
         }
         logError("[yt] fmt[%d] itag=%d v=%d a=%d ct=%s cipher=%d url=%d size=%dx%d fps=%d reason=%s track=%s\n",
                  i, f->itag, f->hasVideo, f->hasAudio, f->container,
                  f->needsCipher, f->url[0] != 0, f->width, f->height, f->fps,
                  reason, f->audioTrackId[0] ? f->audioTrackId : "-");
      }
   } else {
      logInfo("[yt] video selector chose itag=%d %dx%d fps=%d\n", best->itag, best->width, best->height, best->fps);
   }
   return best;
}

const StreamFormat *pickBestVideo(const StreamInfo *info)
{
   return pickBestVideoInternal(info, 0);
}

const StreamFormat *pickBestPlaybackVideo(const StreamInfo *info)
{
   return pickBestVideoInternal(info, getEnable1080p60());
}

int isPlaybackVideoFormatDecodable(const StreamFormat *format)
{
   if (!format || !format->hasVideo || format->needsCipher || !format->url[0]) return 0;
   if (strcmp(format->container, "mp4") != 0) return 0;
   return decodableVideo(format, getEnable1080p60());
}

const StreamFormat *pickVideoForProfile(const StreamInfo *info, int profile)
{
   if (!info || profile < 0 || profile > 2) return NULL;

   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;
      if (format->height <= 0 || format->fps <= 0 || format->fps > 60) continue;

      if (profile == 0 && format->height > 480) continue;
      if (profile == 1 && (format->height <= 480 || format->height > 720)) continue;
      if (profile == 2 && (format->height <= 720 || format->height > 1080 || format->fps > 30)) continue;

      if (!best || format->height > best->height ||
          (format->height == best->height && format->fps > best->fps))
         best = format;
   }
   return best;
}

const StreamFormat *pickBestAudio(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasAudio || format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;
      if (!best || format->itag == 140) best = format;
   }
   return best;
}


static int languageCodeMatches(const char *trackId, const char *languageCode)
{
   if (!trackId || !trackId[0] || !languageCode || !languageCode[0]) return 0;
   char code[16];
   int n = 0;
   while (trackId[n] && trackId[n] != '.' && trackId[n] != '_' && n < (int)sizeof code - 1) {
      code[n] = trackId[n]; n++;
   }
   code[n] = 0;
   if (!n) return 0;
   for (int i = 0; i < n; i++) if (code[i] >= 'A' && code[i] <= 'Z') code[i] = (char)(code[i] - 'A' + 'a');
   char wanted[16];
   int w = 0;
   while (languageCode[w] && languageCode[w] != '-' && w < (int)sizeof wanted - 1) {
      char c = languageCode[w++];
      wanted[w - 1] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
   }
   wanted[w] = 0;
   int trackBase = (int)strcspn(code, "-");
   return (int)strlen(wanted) == trackBase && strncmp(code, wanted, (size_t)trackBase) == 0;
}

const StreamFormat *pickDownloadVideo(const StreamInfo *info, int maxHeight, int preferSeparateAudio)
{
   if (!info) return NULL;
   const StreamFormat *best = NULL;
   const StreamFormat *bestSeparate = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0] ||
          strcmp(format->container, "mp4") != 0 || !decodableVideo(format, getEnable1080p60())) continue;
      if (maxHeight > 0 && format->height > maxHeight) continue;
      if (!best || format->height > best->height ||
          (format->height == best->height && format->fps > best->fps)) best = format;
      if (!format->hasAudio && (!bestSeparate || format->height > bestSeparate->height ||
          (format->height == bestSeparate->height && format->fps > bestSeparate->fps)))
         bestSeparate = format;
   }
   return preferSeparateAudio && bestSeparate ? bestSeparate : best;
}

const StreamFormat *pickDownloadMuxedVideo(const StreamInfo *info, int maxHeight)
{
   if (!info) return NULL;
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || !format->hasAudio || format->needsCipher || !format->url[0] ||
          strcmp(format->container, "mp4") != 0 || !decodableVideo(format, getEnable1080p60())) continue;
      if (maxHeight > 0 && format->height > maxHeight) continue;
      if (!best || format->height > best->height ||
          (format->height == best->height && format->fps > best->fps)) best = format;
   }
   return best;
}

const StreamFormat *pickDownloadAudio(const StreamInfo *info, const char *languageCode,
                                      int allowAutoDubbed)
{
   if (!info) return NULL;
   int original = !languageCode || !languageCode[0] || strcmp(languageCode, "original") == 0;
   const StreamFormat *best = NULL;
   const StreamFormat *originalFallback = NULL;
   const StreamFormat *defaultTrack = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasAudio || format->hasVideo || format->needsCipher || !format->url[0] ||
          strcmp(format->container, "mp4") != 0) continue;
      if (format->audioIsAutoDubbed && !allowAutoDubbed) continue;
      if (!format->audioIsAutoDubbed &&
          (!originalFallback || format->itag == 140)) originalFallback = format;
      if (original) {
         if (format->audioIsAutoDubbed) continue;
         if (format->audioIsDefault) return format;
         if (info->defaultAudioTrackId[0] &&
             strcmp(format->audioTrackId, info->defaultAudioTrackId) == 0) defaultTrack = format;
         continue;
      }
      if (!languageCodeMatches(format->audioTrackId, languageCode)) continue;
      if (!best || (best->audioIsAutoDubbed && !format->audioIsAutoDubbed) ||
          (best->audioIsAutoDubbed == format->audioIsAutoDubbed && format->itag == 140))
         best = format;
   }
   if (best) return best;
   if (original) return defaultTrack ? defaultTrack : originalFallback;
   // A missing requested dub is deliberately not replaced with a different language here;
   // the caller performs the explicit original-upload fallback.
   return NULL;
}
