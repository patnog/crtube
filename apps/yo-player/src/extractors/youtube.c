// youtube extractor - InnerTube player request + parse.
//
// POSTs the player endpoint as the VISIONOS (Apple Vision Pro) client, which returns
// direct (unciphered) urls and - unlike plain ANDROID/WEB - is not behind the
// GVS PoToken wall, so its adaptive (fragmented) urls above 360p don't 403.
// Those streams are fragmented mp4 (moof); the demuxer handles them.
//
// Fragile by nature: Google keeps closing these token-exempt clients. ANDROID_VR
// (Oculus) was capped to ~60 seconds of media on 2026-08-26; VISIONOS replaced it
// and serves full-length urls. Expect this to need swapping again in future.

#include "app-config.h"
#include "extractor.h"
#include "http.h"               // fetchHttp
#include "string-utilities.h"   // strCopy
#include "dbg.h"                // logInfo (resolve diagnostics)
#include "vfs.h"                // readFile/writeFile/deleteFile (persist the visitor session)
#include "account.h"            // getAccessToken (signed-in feeds)
#include "account-oauth.h"      // OAUTH_ACCESS_TOKEN_MAX
#include "settings.h"           // getContentRegionCode: region for list requests
#include "youtube-tv.h"         // parseTvTiles

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PLAYER_URL "https://www.youtube.com/youtubei/v1/player?prettyPrint=false"
#define SEARCH_URL "https://www.youtube.com/youtubei/v1/search?prettyPrint=false"
#define BROWSE_URL "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false"
#define RESP_CAP   (2 * 1024 * 1024)   // player JSON can pass 1 MB; a truncated response loses formats (and their urls)

// two clients, each for what it does best:
//  - VISIONOS (playback), see below. WEB/plain ANDROID are PoToken-gated for
//    adaptive (their googlevideo urls 403 above 360p - only progressive itag 18
//    is token-exempt); TVHTML5 hit the bot wall; iOS gave only fragmented adaptive.
//  - VISIONOS (Apple Vision Pro) is currently exempt from the GVS PO-token wall and
//    returns direct (un-ciphered) adaptive urls that serve full-length, so it plays
//    above 360p past the ~60s cap that now hits ANDROID_VR (the client it replaced).
//  - WEB (search): search isn't PoToken-gated, and WEB reliably returns the
//    videoRenderer result structure we parse.
#define PLAYER_VERSION "1.02"
#define WEB_VERSION    "2.20250701.00.00"

// visitor session endpoint: mints an anonymous visitorData that clears the
// logged-out LOGIN_REQUIRED ("confirm you're not a bot") wall (see extract()).
// the key below is youtube's own public web key, shipped in every youtube.com page. not a credential.
#define VISITOR_URL "https://www.youtube.com/youtubei/v1/visitor_id?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false"

// the VISIONOS client object, without the wrapping braces so visitorData can
// be appended before the player request closes it.
#define PLAYER_CLIENT \
   "\"clientName\":\"VISIONOS\",\"clientVersion\":\"" PLAYER_VERSION "\"," \
   "\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\"," \
   "\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\",\"hl\":\"en\",\"gl\":\"US\""
#define WEB_CONTEXT_FMT \
   "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"%s\"}}"

static const char *PLAYER_UA =
   "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
static const char *WEB_UA =
   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";

// player body: the client + the minted visitorData, then the video id. two %s: visitorData, videoId.
static const char *PLAYER_BODY_FMT =
   "{\"context\":{\"client\":{" PLAYER_CLIENT ",\"visitorData\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}";

// live streams need the ANDROID_VR client: VISIONOS returns only an HLS manifest for them, which the live
// path can't read, while ANDROID_VR still returns the DASH manifest it needs. Live fetches discrete
// segments, so the ~60s cap that pushed VOD off ANDROID_VR does not apply here.
#define LIVE_VERSION "1.65.10"
#define LIVE_CLIENT \
   "\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"" LIVE_VERSION "\"," \
   "\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32," \
   "\"osName\":\"Android\",\"osVersion\":\"12L\",\"hl\":\"en\",\"gl\":\"US\"," \
   "\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0"
static const char *LIVE_UA =
   "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
static const char *LIVE_BODY_FMT =
   "{\"context\":{\"client\":{" LIVE_CLIENT ",\"visitorData\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}";

static const char *VISITOR_BODY   = "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"}}}";   // visitor session stays on its original fixed region
// Search keeps YouTube's mixed result types (videos and playlists). These params apply only the sort
// order; the previous video-type filter hid every playlist. Newest remains best-effort server-side and
// VideoGrid also sorts fetched video pages locally by published age.
static const char *SEARCH_SORT_PARAMS[SORT_COUNT] = {
   "",       // SORT_RELEVANCE (youtube default, no filter)
   "CAM=",   // SORT_VIEWS (view-count sort only)
   "CAI=",   // SORT_NEWEST (upload-date sort only)
};
#define SEARCH_BODY_FMT WEB_CONTEXT_FMT ",\"query\":\"%s\",\"params\":\"%s\"}"
// trending feed. YouTube retired the aggregated Trending page in 2025 (FEtrending now redirects to an empty
// home feed); NewPipe/FreeTube/Invidious replaced it with per-category channel-tab browses that still work
// logged-out. three %s: gl, browseId, params. see TRENDING_CATEGORIES for the ids.
#define TRENDING_BODY_FMT WEB_CONTEXT_FMT ",\"browseId\":\"%s\",\"params\":\"%s\"}"
#define CONTINUATION_BODY_FMT WEB_CONTEXT_FMT ",\"continuation\":\"%s\"}"   // gl, continuation token
#define NEXT_URL "https://www.youtube.com/youtubei/v1/next?prettyPrint=false"
#define NEXT_BODY_FMT "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"%s\"}},\"videoId\":\"%s\"}"

static int hex4(const char *p)
{
   int value = 0;
   for (int i = 0; i < 4; i++) {
      char c = p[i], digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else return -1;
      value = value * 16 + digit;
   }
   return value;
}

// copy the json string value of "key" found within [from,end), unescaping the
// usual escapes plus \uXXXX (ascii only - urls and titles are ascii; higher
// code points are dropped). returns 1 if the key was found.
static int jsonString(const char *from, const char *end, const char *key, char *out, int cap)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":\"", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) { if (cap) out[0] = 0; return 0; }
   p += patLen;

   int i = 0;
   while (p < end && *p && *p != '"' && i < cap - 1) {
      if (*p != '\\') { out[i++] = *p++; continue; }
      if (++p >= end) break;                       // consume backslash
      char c = *p++;
      switch (c) {
         case 'n': out[i++] = '\n'; break;
         case 't': out[i++] = '\t'; break;
         case 'r': out[i++] = '\r'; break;
         case 'b': out[i++] = '\b'; break;
         case 'f': out[i++] = '\f'; break;
         case 'u': {
            if (p + 4 > end) { p = end; break; }
            int value = hex4(p);
            p += 4;
            if (value >= 0 && value <= 0x7F) out[i++] = (char)value;   // ascii only
            break;
         }
         default: out[i++] = c; break;              // \/ \\ \" and any other: keep literal
      }
   }
   out[i] = 0;
   return 1;
}

// read the (possibly quoted) integer value of "key" within [from,end). 1 if found.
static int jsonInt(const char *from, const char *end, const char *key, int *out)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) return 0;
   p += patLen;
   if (*p == '"') p++;      // some numeric fields are quoted (e.g. lengthSeconds)
   *out = atoi(p);
   return 1;
}

// Read a JSON boolean field, allowing optional whitespace after the colon.
static int jsonBool(const char *from, const char *end, const char *key)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\"", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) return 0;
   p += patLen;
   while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
   if (p >= end || *p++ != ':') return 0;
   while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
   return (size_t)(end - p) >= 4 && strncmp(p, "true", 4) == 0;
}

// Return the end of a JSON object beginning at `start` (exclusive), respecting
// quoted strings and escapes. This keeps diagnostic fields scoped to their object.
static const char *jsonObjectEnd(const char *start, const char *end)
{
   if (!start || start >= end || *start != '{') return NULL;
   int depth = 0, inString = 0, escaped = 0;
   for (const char *p = start; p < end; p++) {
      char c = *p;
      if (inString) {
         if (escaped) escaped = 0;
         else if (c == '\\') escaped = 1;
         else if (c == '"') inString = 0;
         continue;
      }
      if (c == '"') inString = 1;
      else if (c == '{') depth++;
      else if (c == '}') {
         depth--;
         if (depth == 0) return p + 1;
      }
   }
   return NULL;
}

static const char *jsonArrayEnd(const char *start, const char *end)
{
   if (!start || start >= end || *start != '[') return NULL;
   int depth = 0, inString = 0, escaped = 0;
   for (const char *p = start; p < end; p++) {
      char c = *p;
      if (inString) {
         if (escaped) escaped = 0;
         else if (c == '\\') escaped = 1;
         else if (c == '"') inString = 0;
         continue;
      }
      if (c == '"') inString = 1;
      else if (c == '[') depth++;
      else if (c == ']') {
         depth--;
         if (depth == 0) return p + 1;
      }
   }
   return NULL;
}

static const char *jsonObjectForKey(const char *from, const char *end, const char *key,
                                    const char **objectEnd)
{
   char pat[64];
   int n = snprintf(pat, sizeof pat, "\"%s\":", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) return NULL;
   p += n;
   while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
   if (p >= end || *p != '{') return NULL;
   const char *close = jsonObjectEnd(p, end);
   if (!close) return NULL;
   if (objectEnd) *objectEnd = close;
   return p;
}

// Metadata shared by the VISIONOS player response and alternative-client retries.
static void parseVideoMetadata(const char *resp, const char *end, StreamInfo *out)
{
   const char *details = strstr(resp, "\"videoDetails\"");
   if (details && details < end)
      jsonString(details, end, "viewCount", out->viewCount, sizeof out->viewCount);

   const char *microEnd = NULL;
   const char *micro = jsonObjectForKey(resp, end, "playerMicroformatRenderer", &microEnd);
   if (micro) {
      if (!jsonString(micro, microEnd, "publishDate", out->publishedDate, sizeof out->publishedDate))
         jsonString(micro, microEnd, "uploadDate", out->publishedDate, sizeof out->publishedDate);
   }
}

// Emit a complete JSON object in bounded log lines. Keep chunks short enough
// for the PS3 log bridge and include offsets so the object can be reconstructed.
static void logJsonObject(const char *label, const char *start, const char *end)
{
   if (!start || !end || end <= start) {
      logError("[yt] %s: unavailable\n", label);
      return;
   }

   int total = (int)(end - start);
   const int chunkSize = 180;
   int chunks = (total + chunkSize - 1) / chunkSize;
   logError("[yt] %s begin bytes=%d chunks=%d\n", label, total, chunks);

   for (int offset = 0, part = 0; offset < total; offset += chunkSize, part++) {
      int count = total - offset;
      if (count > chunkSize) count = chunkSize;
      char chunk[181];
      memcpy(chunk, start + offset, count);
      chunk[count] = 0;
      logError("[yt] %s[%d/%d] offset=%d: %s\n",
               label, part + 1, chunks, offset, chunk);
   }

   logError("[yt] %s end\n", label);
}

// Copy the first few string entries from a JSON string array (e.g. messages).
static int jsonArrayStrings(const char *from, const char *end, const char *key,
                            char values[][192], int maxValues)
{
   char pat[64];
   int n = snprintf(pat, sizeof pat, "\"%s\":", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) return 0;
   p += n;
   while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
   if (p >= end || *p != '[') return 0;
   p++;
   int count = 0;
   while (p < end && *p && *p != ']' && count < maxValues) {
      if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') { p++; continue; }
      if (*p != '"') break;
      p++;
      int out = 0;
      while (p < end && *p && *p != '"') {
         char c = *p++;
         if (c == '\\' && p < end) {
            c = *p++;
            if (c == 'n') c = ' ';
            else if (c == 'r' || c == 't') c = ' ';
            else if (c == 'u') {
               if (p + 4 > end) break;
               int value = hex4(p);
               p += 4;
               c = (value >= 0 && value <= 0x7f) ? (char)value : '?';
            }
         }
         if (out < 191) values[count][out++] = c;
      }
      values[count][out] = 0;
      if (p < end && *p == '"') p++;
      count++;
   }
   return count;
}

static int matches(const char *input)
{
   if (!input) return 0;
   if (strstr(input, "youtu")) return 1;     // youtube.com / youtu.be url
   return (int)strlen(input) == 11;          // bare 11-char video id
}

static int isIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// copy the 11-char youtube video id from a url (watch?v=, youtu.be/, /shorts/,
// /embed/, /live/) or a bare id into out. returns 1 only if a full 11-char id was found.
static int extractVideoId(const char *input, char *out, int cap)
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id) id = ((int)strlen(input) == 11) ? input : NULL;   // otherwise a bare id
   if (!id) return 0;

   int length = 0;
   while (length < 11 && length < cap - 1 && isIdChar(id[length])) { out[length] = id[length]; length++; }
   out[length] = 0;
   return length == 11;
}

// parse one formats array (region [arrayStart, regionEnd)) into out->formats.
// progressive entries are muxed (video+audio); adaptive entries are video-only
// or audio-only, classified from the mimeType.
static void parseFormatArray(const char *arrayStart, const char *regionEnd, int progressive, StreamInfo *out)
{
   const char *scan = arrayStart;
   int diagnosticCount = 0;
   while (out->formatCount < MAX_STREAM_FORMATS) {
      const char *object = strstr(scan, "\"itag\":");
      if (!object || object >= regionEnd) break;
      const char *next = strstr(object + 7, "\"itag\":");
      const char *objectEnd = (next && next < regionEnd) ? next : regionEnd;

      StreamFormat *format = &out->formats[out->formatCount];
      memset(format, 0, sizeof *format);
      jsonInt(object, objectEnd, "itag", &format->itag);
      jsonInt(object, objectEnd, "width", &format->width);
      jsonInt(object, objectEnd, "height", &format->height);
      jsonInt(object, objectEnd, "fps", &format->fps);
      format->needsCipher = !jsonString(object, objectEnd, "url", format->url, sizeof format->url);

      // mimeType is like: video/mp4; codecs="avc1.640028". only mark a stream playable when its codec is
      // one we can actually decode - H.264 (avc1/avc3) video, AAC (mp4a) audio. newer itags carry AV1/VP9
      // video or Opus audio in an mp4/webm container that our demuxer+decoder can't play (itag 787 = AV1),
      // and picking one of those over a real avc1 stream made the video fail to open.
      char mimeType[64] = "";
      jsonString(object, objectEnd, "mimeType", mimeType, sizeof mimeType);
      // Adaptive audio formats identify the language and whether YouTube marked it as original/auto-dubbed.
      const char *audioTrack = strstr(object, "\"audioTrack\":{");
      if (audioTrack && audioTrack < objectEnd) {
         // displayName can itself contain nested JSON objects. Do not stop at its first closing
         // brace: that truncated the metadata before audioIsDefault/isAutoDubbed on some responses.
         jsonString(audioTrack, objectEnd, "id", format->audioTrackId, sizeof format->audioTrackId);
         jsonString(audioTrack, objectEnd, "displayName", format->audioTrackName, sizeof format->audioTrackName);
         const char *flag = strstr(audioTrack, "\"audioIsDefault\":true");
         format->audioIsDefault = flag && flag < objectEnd;
         flag = strstr(audioTrack, "\"isAutoDubbed\":true");
         format->audioIsAutoDubbed = flag && flag < objectEnd;
      }
      strCopy(format->container, sizeof format->container, strstr(mimeType, "webm") ? "webm" : "mp4");
      int isAvc = strstr(mimeType, "avc1") != NULL || strstr(mimeType, "avc3") != NULL;
      int isAac = strstr(mimeType, "mp4a") != NULL;
      // Temporary diagnostics: log at most 128 raw entries per formats array,
      // including entries discarded below because their codec is unsupported.
      if (diagnosticCount < 128) {
         logInfo("[yt] rawfmt[%d] itag=%d mime=%s url=%d cipher=%d size=%dx%d fps=%d avc=%d aac=%d progressive=%d audioId='%s' name='%s' default=%d autoDubbed=%d\n",
                 diagnosticCount, format->itag, mimeType, format->url[0] != 0,
                 format->needsCipher, format->width, format->height, format->fps,
                 isAvc, isAac, progressive, format->audioTrackId, format->audioTrackName,
                 format->audioIsDefault, format->audioIsAutoDubbed);
         diagnosticCount++;
      }
      if (progressive)                              format->hasVideo = format->hasAudio = 1;
      else if (strncmp(mimeType, "audio/", 6) == 0) format->hasAudio = isAac;
      else                                          format->hasVideo = isAvc;

      // keep only formats we can decode (H.264 video / AAC audio). dropping VP9/AV1/Opus frees array slots
      // so the AAC audio - which youtube lists AFTER all the video formats - isn't pushed past the cap.
      if (format->hasVideo || format->hasAudio) out->formatCount++;
      scan = objectEnd;
   }
}

// parse the caption track list into out->captions. the list ends where the renderer's next field
// starts (entries hold nested arrays, so a plain ']' scan would cut early); each entry leads with
// "baseUrl". the display name sits in the entry's name block as "text" (runs) or "simpleText".
static int parseCaptionEntry(const char *entry, const char *entryEnd, CaptionTrack *track, int *isAuto)
{
   memset(track, 0, sizeof *track);
   if (!jsonString(entry, entryEnd, "baseUrl", track->url, sizeof track->url) ||
       !jsonString(entry, entryEnd, "languageCode", track->languageCode, sizeof track->languageCode))
      return 0;
   if (!jsonString(entry, entryEnd, "text", track->name, sizeof track->name) &&
       !jsonString(entry, entryEnd, "simpleText", track->name, sizeof track->name))
      strCopy(track->name, sizeof track->name, track->languageCode);
   char kind[16];
   jsonString(entry, entryEnd, "kind", kind, sizeof kind);
   *isAuto = strcmp(kind, "asr") == 0;
   track->isAutoGenerated = *isAuto;
   track->isTranslatable = jsonBool(entry, entryEnd, "isTranslatable");
   return 1;
}

static int buildTranslatedCaptionUrl(const char *sourceUrl, const char *languageCode,
                                     char *out, int outCap)
{
   char base[MAX_STREAM_URL];
   strCopy(base, sizeof base, sourceUrl);

   // Remove any existing format/translation parameters. Keep the signed URL's
   // remaining query fields intact, then append tlang and srv1 explicitly.
   const char *parameterNames[] = { "&fmt=", "?fmt=", "&tlang=", "?tlang=" };
   for (int i = 0; i < 4; i++) {
      char *parameter = strstr(base, parameterNames[i]);
      if (!parameter) continue;
      char *next = strchr(parameter + 1, '&');
      if (next) memmove(parameter, next, strlen(next) + 1);
      else *parameter = 0;
   }

   int length = snprintf(out, outCap, "%s%ctlang=%s&fmt=srv1", base,
                         strchr(base, '?') ? '&' : '?', languageCode);
   return length >= 0 && length < outCap;
}

static int appendCaptionTrack(StreamInfo *out, const CaptionTrack *track)
{
   if (out->captionCount >= out->captionCapacity) {
      int capacity = out->captionCapacity ? out->captionCapacity * 2 : 16;
      CaptionTrack *tracks = realloc(out->captions, (size_t)capacity * sizeof *tracks);
      if (!tracks) return 0;
      out->captions = tracks;
      out->captionCapacity = capacity;
   }
   out->captions[out->captionCount++] = *track;
   return 1;
}

static void parseCaptionTracks(const char *resp, const char *end, StreamInfo *out)
{
   const char *captions = strstr(resp, "\"captionTracks\"");
   if (!captions) return;
   const char *listEnd = strstr(captions, "\"audioTracks\"");
   if (!listEnd) listEnd = strstr(captions, "\"translationLanguages\"");
   if (!listEnd || listEnd > end) listEnd = end;

   // Keep every source track in YouTube's original order. The old implementation
   // reserved only eight slots and discarded all but six non-preferred languages.
   const char *scan = captions;
   while (scan < listEnd) {
      const char *entry = strstr(scan, "\"baseUrl\"");
      if (!entry || entry >= listEnd) break;
      const char *next = strstr(entry + 1, "\"baseUrl\"");
      const char *entryEnd = (next && next < listEnd) ? next : listEnd;
      CaptionTrack track;
      int isAuto = 0;
      if (parseCaptionEntry(entry, entryEnd, &track, &isAuto) &&
          !appendCaptionTrack(out, &track)) {
         logError("[yt] unable to allocate caption track list\n");
         return;
      }
      scan = entryEnd;
   }

   // YouTube exposes translation targets separately from captionTracks. Build
   // translated URLs only from a source track explicitly marked translatable.
   CaptionTrack sourceTrack;
   int hasSource = 0;
   for (int i = 0; i < out->captionCount; i++) {
      if (out->captions[i].isTranslatable && !out->captions[i].isAutoGenerated) {
         sourceTrack = out->captions[i];
         hasSource = 1;
         break;
      }
   }
   if (!hasSource) {
      for (int i = 0; i < out->captionCount; i++) {
         if (out->captions[i].isTranslatable) {
            sourceTrack = out->captions[i];
            hasSource = 1;
            break;
         }
      }
   }
   const char *translations = strstr(captions, "\"translationLanguages\"");
   if (!hasSource && translations && translations < end)
      logInfo("[yt] translation targets present but no caption track is marked translatable\n");
   if (hasSource && translations && translations < end) {
      const char *array = strchr(translations, '[');
      const char *arrayEnd = array ? jsonArrayEnd(array, end) : NULL;
      if (array && arrayEnd) {
         scan = array + 1;
         while (scan < arrayEnd - 1) {
            const char *entry = strchr(scan, '{');
            if (!entry || entry >= arrayEnd - 1) break;
            const char *entryEnd = jsonObjectEnd(entry, arrayEnd);
            if (!entryEnd) break;

            CaptionTrack translated;
            memset(&translated, 0, sizeof translated);
            if (jsonString(entry, entryEnd, "languageCode", translated.languageCode,
                           sizeof translated.languageCode) &&
                translated.languageCode[0]) {
               int duplicate = 0;
               for (int i = 0; i < out->captionCount; i++) {
                  if (out->captions[i].isAutoTranslated &&
                      strcmp(out->captions[i].languageCode, translated.languageCode) == 0) {
                     duplicate = 1;
                     break;
                  }
               }

               if (!duplicate) {
                  // translationLanguages may expose languageName as a plain string
                  // or as a nested simpleText/text object depending on the client.
                  jsonString(entry, entryEnd, "languageName", translated.name,
                             sizeof translated.name);
                  if (!translated.name[0])
                     jsonString(entry, entryEnd, "simpleText", translated.name,
                                sizeof translated.name);
                  if (!translated.name[0])
                     jsonString(entry, entryEnd, "text", translated.name,
                                sizeof translated.name);
                  if (!translated.name[0])
                     strCopy(translated.name, sizeof translated.name, translated.languageCode);

                  char languageName[sizeof translated.name];
                  strCopy(languageName, sizeof languageName, translated.name);
                  snprintf(translated.name, sizeof translated.name, "%.*s (auto-translated)",
                           (int)sizeof translated.name - 19, languageName);

                  if (buildTranslatedCaptionUrl(sourceTrack.url, translated.languageCode,
                                                translated.url, sizeof translated.url)) {
                     translated.isAutoTranslated = 1;
                     translated.isTranslatable = 0;
                     translated.isAutoGenerated = 0;
                     if (!appendCaptionTrack(out, &translated)) {
                        logError("[yt] unable to allocate translated caption list\n");
                        return;
                     }
                  } else {
                     logWarn("[yt] translated caption URL too long for %s\n", translated.languageCode);
                  }
               }
            }
            scan = entryEnd;
         }
      }
   }
   if (out->captionCount)
      logInfo("[yt] %d caption track(s), including translated languages\n", out->captionCount);
}

// parse both stream arrays. iOS returns only adaptiveFormats (separate video and
// audio); WEB/TVHTML5 would also carry a progressive "formats" (muxed) array.
static void parseFormats(const char *resp, const char *end, StreamInfo *out)
{
   const char *streaming = strstr(resp, "\"streamingData\"");
   if (!streaming) return;
   const char *progressive = strstr(streaming, "\"formats\"");
   const char *adaptive    = strstr(streaming, "\"adaptiveFormats\"");

   if (progressive)
      parseFormatArray(progressive, (adaptive && adaptive > progressive) ? adaptive : end, 1, out);
   if (adaptive)
      parseFormatArray(adaptive, end, 0, out);
}

// The anonymous visitor session. Without it a logged-out player request gets LOGIN_REQUIRED. Minting
// a fresh one every launch is itself a bot signal that re-triggers the wall, so it persists to disk:
// we reuse a saved session and only mint (and save) when there's none, or a saved one goes stale.
#define VISITOR_CACHE_PATH CRTUBE_DATA_DIR "/visitor.txt"

static char visitorData[192];
static int  visitorFromDisk;   // the current session was loaded from the cache (vs minted this launch)

static int loadVisitorData(void)
{
   return readFile(VISITOR_CACHE_PATH, visitorData, sizeof visitorData) > 0 && visitorData[0] != 0;
}

static void saveVisitorData(void)
{
   writeFile(VISITOR_CACHE_PATH, visitorData, strlen(visitorData));
}

// a saved session that hit LOGIN_REQUIRED is stale: forget it so the next play mints a fresh one once.
static void dropVisitorData(void)
{
   deleteFile(VISITOR_CACHE_PATH);
   visitorData[0] = 0;
   visitorFromDisk = 0;
}

static int ensureVisitorData(void)
{
   if (visitorData[0]) return 0;
   if (loadVisitorData()) { visitorFromDisk = 1; return 0; }   // reuse the persisted session

   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
   };
   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", VISITOR_URL, headers, 4, VISITOR_BODY, (int)strlen(VISITOR_BODY), resp, RESP_CAP, &respLen, &status);
   int ok = (rc == 0 && status == 200 && jsonString(resp, resp + respLen, "visitorData", visitorData, sizeof visitorData));
   free(resp);

   if (!ok) { logError("[yt] visitor_id failed rc=%d status=%d\n", rc, status); return -1; }
   urlDecode(visitorData, visitorData, sizeof visitorData);   // endpoint returns it percent-encoded (%3D)
   visitorFromDisk = 0;
   logInfo("[yt] visitorData minted (%d chars)\n", (int)strlen(visitorData));
   return 0;
}

// XML text nodes escape '&' as "&amp;"; the BaseURL query string needs it back to fetch.
static void unescapeAmp(char *text)
{
   char *write = text;
   for (char *read = text; *read; ) {
      if (strncmp(read, "&amp;", 5) == 0) { *write++ = '&'; read += 5; }
      else *write++ = *read++;
   }
   *write = 0;
}

// copy the first `attr="..."` (or `<tag>...` with close="<") value found at/after text into out; empty if absent.
static void extractBetween(const char *text, const char *open, const char *close, char *out, int cap)
{
   out[0] = 0;
   const char *start = strstr(text, open);
   if (!start) return;
   start += strlen(open);
   const char *stop = strstr(start, close);
   if (!stop) return;
   int length = (int)(stop - start);
   if (length >= cap) length = cap - 1;
   memcpy(out, start, length);
   out[length] = 0;
}

// a live stream's itag urls are a rolling DVR window whose byte-0 init rolls off, so play by DASH segments
// instead: parse the manifest and, for each adaptiveFormat, replace its url with that Representation's
// segment base ("<base>sq/<n>/...") and record the available sq range (window start .. live edge).
static void parseLiveManifest(const char *resp, const char *end, StreamInfo *out, const char *kind)
{
   char manifestUrl[1400];
   if (!jsonString(resp, end, "dashManifestUrl", manifestUrl, sizeof manifestUrl)) return;

   int cap = 4 * 1024 * 1024;   // live DVR manifests enumerate every segment and can pass 1 MB
   char *mpd = (char *)malloc(cap);
   if (!mpd) return;
   HttpHeader headers[] = { { "User-Agent", PLAYER_UA }, { "Accept-Encoding", "identity" } };
   int length = 0, status = 0;
   if (fetchHttp("GET", manifestUrl, headers, 2, NULL, 0, mpd, cap, &length, &status) != 0 || status != 200) {
      logError("[yt] live manifest fetch status=%d len=%d\n", status, length);
      free(mpd);
      return;
   }
   if (length >= cap - 1) logWarn("[yt] live manifest truncated at %d bytes\n", length);

   for (int i = 0; i < out->formatCount; i++) {
      StreamFormat *format = &out->formats[i];
      char idTag[24];
      snprintf(idTag, sizeof idTag, "id=\"%d\"", format->itag);
      const char *rep = strstr(mpd, idTag);
      if (!rep) continue;
      const char *repEnd = strstr(rep, "</Representation>");
      if (!repEnd) repEnd = mpd + length;

      char base[MAX_STREAM_URL];
      extractBetween(rep, "<BaseURL>", "</BaseURL>", base, sizeof base);
      if (!base[0] || strstr(rep, "<BaseURL>") > repEnd) continue;   // this rep has no own BaseURL
      unescapeAmp(base);

      // first + last media="sq/<n>/..." within this representation = the window start and the live edge
      long startSq = -1, edgeSq = -1;
      for (const char *media = rep; (media = strstr(media, "media=\"sq/")) != NULL && media < repEnd; media++) {
         long number = 0;
         for (const char *digit = media + 10; *digit >= '0' && *digit <= '9'; digit++) number = number * 10 + (*digit - '0');
         if (startSq < 0) startSq = number;
         edgeSq = number;
      }
      if (edgeSq < 0) continue;

      format->isLiveSegmented = 1;
      format->liveStartSq = startSq;
      format->liveEdgeSq = edgeSq;
      // the player opens this through the live segment source (see video-source / live-source)
      snprintf(format->url, sizeof format->url, "%s|%ld|%ld|%s", kind, startSq, edgeSq, base);
      logInfo("[yt] live itag %d: sq %ld..%ld baseTail=%s\n", format->itag, startSq, edgeSq,
              (int)strlen(base) > 40 ? base + strlen(base) - 40 : base);
   }
   free(mpd);
}

// POST the player endpoint with one client identity, into resp. 0 on a 200 with a body, else negative.
// Fetch using a client object supplied without its surrounding braces.
static int fetchPlayerClient(const char *clientFields, const char *userAgent,
                             const char *clientName, const char *clientVersion,
                             const char *videoId, char *resp, int *respLen)
{
   char body[2048];
   int bodyLen = snprintf(body, sizeof body,
      "{\"context\":{\"client\":{%s,\"visitorData\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
      clientFields, visitorData, videoId);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return -1;
   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", userAgent },
      { "X-YouTube-Client-Name", clientName },
      { "X-YouTube-Client-Version", clientVersion },
      { "X-Goog-Visitor-Id", visitorData },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };
   int status = 0;
   int rc = fetchHttp("POST", PLAYER_URL, headers, 7, body, bodyLen,
                      resp, RESP_CAP, respLen, &status);
   if (rc < 0) return rc;
   if (status != 200) return -status;
   return 0;
}

static int retryUnplayableWithOtherClients(const char *videoId, char *resp,
                                           int *respLen, StreamInfo *out)
{
   const struct {
      const char *name;
      const char *version;
      const char *ua;
      const char *fields;
   } clients[] = {
      { "WEB", WEB_VERSION, WEB_UA,
        "\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "ANDROID", "20.10.38", WEB_UA,
        "\"clientName\":\"ANDROID\",\"clientVersion\":\"20.10.38\",\"androidSdkVersion\":34,\"hl\":\"en\",\"gl\":\"US\"" },
      { "IOS", "20.10.4", WEB_UA,
        "\"clientName\":\"IOS\",\"clientVersion\":\"20.10.4\",\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\",\"osName\":\"iPhone\",\"osVersion\":\"18.3.0.22D63\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "WEB_EMBEDDED_PLAYER", WEB_VERSION, WEB_UA,
        "\"clientName\":\"WEB_EMBEDDED_PLAYER\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "TVHTML5_SIMPLY_EMBEDDED_PLAYER", WEB_VERSION, WEB_UA,
        "\"clientName\":\"TVHTML5_SIMPLY_EMBEDDED_PLAYER\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "MUSIC", WEB_VERSION, WEB_UA,
        "\"clientName\":\"MUSIC\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "ANDROID_MUSIC", "8.10.52", WEB_UA,
        "\"clientName\":\"ANDROID_MUSIC\",\"clientVersion\":\"8.10.52\",\"androidSdkVersion\":34,\"hl\":\"en\",\"gl\":\"US\"" },
      { "MWEB", WEB_VERSION, WEB_UA,
        "\"clientName\":\"MWEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "TVHTML5", WEB_VERSION, WEB_UA,
        "\"clientName\":\"TVHTML5\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"" },
      { "ANDROID_VR", LIVE_VERSION, LIVE_UA,
        "\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"" LIVE_VERSION "\",\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32,\"osName\":\"Android\",\"osVersion\":\"12L\",\"hl\":\"en\",\"gl\":\"US\"" },
   };
   const int count = (int)(sizeof clients / sizeof clients[0]);
   for (int i = 0; i < count; i++) {
      logInfo("[yt] UNPLAYABLE; retrying video=%s with client=%s\n", videoId, clients[i].name);
      int rc = fetchPlayerClient(clients[i].fields, clients[i].ua,
                                 (i == 0 ? "1" : i == 1 ? "3" : i == 2 ? "5" :
                                  i == 3 ? "56" : i == 4 ? "85" : i == 5 ? "67" :
                                  i == 6 ? "21" : i == 7 ? "2" : i == 8 ? "7" : "28"),
                                 clients[i].version, videoId, resp, respLen);
      if (rc != 0) {
         logError("[yt] retry client=%s request failed rc=%d\n", clients[i].name, rc);
         continue;
      }
      const char *end = resp + *respLen;
      const char *playEnd = NULL;
      const char *play = jsonObjectForKey(resp, end, "playabilityStatus", &playEnd);
      char status[48] = "";
      if (play) jsonString(play, playEnd, "status", status, sizeof status);
      logInfo("[yt] retry client=%s playability=%s respLen=%d\n",
              clients[i].name, status[0] ? status : "(missing)", *respLen);
      if (strcmp(status, "UNPLAYABLE") == 0) continue;

      out->formatCount = 0;
      parseFormats(resp, end, out);

      // Some clients expose adaptive URLs that require a PoToken unavailable here.
      // Keep only progressive muxed formats for these clients.
      if (strcmp(clients[i].name, "WEB") == 0 ||
          strcmp(clients[i].name, "ANDROID") == 0 ||
          strcmp(clients[i].name, "MWEB") == 0) {
         int kept = 0;
         for (int j = 0; j < out->formatCount; j++) {
            StreamFormat *format = &out->formats[j];
            if (!format->hasVideo || !format->hasAudio) continue;
            if (kept != j) out->formats[kept] = *format;
            kept++;
         }
         out->formatCount = kept;
      }

      if (out->formatCount > 0) {
         const char *details = strstr(resp, "\"videoDetails\"");
         if (details) {
            if (!out->title[0]) jsonString(details, end, "title", out->title, sizeof out->title);
            if (!out->author[0]) jsonString(details, end, "author", out->author, sizeof out->author);
            if (!out->description[0]) jsonString(details, end, "shortDescription", out->description, sizeof out->description);
            if (out->durationSeconds <= 0) {
               char lengthText[16];
               if (jsonString(details, end, "lengthSeconds", lengthText, sizeof lengthText))
                  out->durationSeconds = atoi(lengthText);
            }
         }
         parseVideoMetadata(resp, end, out);
         if (out->captionCount == 0) parseCaptionTracks(resp, end, out);
         logInfo("[yt] retry succeeded client=%s formats=%d\n",
                 clients[i].name, out->formatCount);
         return 1;
      }
      logInfo("[yt] retry client=%s returned no supported formats\n", clients[i].name);
   }
   logError("[yt] all alternative clients failed for video=%s\n", videoId);
   return 0;
}

static int fetchPlayer(const char *bodyFmt, const char *userAgent, const char *clientName,
                       const char *clientVersion, const char *videoId, char *resp, int *respLen)
{
   char body[1024];
   int bodyLen = snprintf(body, sizeof body, bodyFmt, visitorData, videoId);
   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", userAgent },
      { "X-YouTube-Client-Name", clientName },
      { "X-YouTube-Client-Version", clientVersion },
      { "X-Goog-Visitor-Id", visitorData },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };
   int status = 0;
   int rc = fetchHttp("POST", PLAYER_URL, headers, 7, body, bodyLen, resp, RESP_CAP, respLen, &status);
   if (rc < 0) {
      logError("[yt] player POST failed videoId=%s headerClientName=%s clientVersion=%s rc=%d bodyLen=%d\n",
               videoId, clientName, clientVersion, rc, bodyLen);
      return rc;
   }
   logInfo("[yt] player HTTP videoId=%s headerClientName=%s clientVersion=%s status=%d respLen=%d bodyLen=%d cachedVisitorData=%s\n",
           videoId, clientName, clientVersion, status, *respLen, bodyLen,
           visitorData[0] ? "present" : "absent");
   if (status != 200) return -status;
   return 0;
}

// Resolve only the video's channel metadata. This is intentionally separate from feed parsing
// and playback extraction so account tiles pay the request cost only when Triangle is pressed.
static int youtubeGetVideoChannelId(const char *videoId, char *channelId, int channelIdCap)
{
   if (!videoId || !videoId[0] || !channelId || channelIdCap < 3) return -1;
   channelId[0] = 0;
   if (ensureVisitorData() != 0) {
      logError("[yt] channel lookup: visitor session unavailable for %s\n", videoId);
      return -1;
   }

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0;
   int rc = fetchPlayer(PLAYER_BODY_FMT, PLAYER_UA, "101", PLAYER_VERSION, videoId, resp, &respLen);
   if (rc != 0) {
      logError("[yt] channel lookup: player request failed video=%s rc=%d\n", videoId, rc);
      free(resp);
      return rc;
   }

   const char *end = resp + respLen;
   const char *details = strstr(resp, "\"videoDetails\"");
   int found = details && jsonString(details, end, "channelId", channelId, channelIdCap)
               && channelId[0] == 'U' && channelId[1] == 'C';
   if (!found) {
      channelId[0] = 0;
      logError("[yt] channel lookup: videoDetails.channelId missing video=%s\n", videoId);
   }
   free(resp);
   return found ? 0 : -1;
}


// Fetch public metadata with WEB. WEB may report UNPLAYABLE while still returning
// videoDetails and playerMicroformatRenderer; metadata does not depend on playback status.
static int youtubeGetVideoMetadata(const char *videoId,
                                   char *viewCount, int viewCountCap,
                                   char *publishedDate, int publishedDateCap,
                                   char *likeCount, int likeCountCap)
{
   if (!videoId || strlen(videoId) != 11 ||
       !viewCount || viewCountCap < 2 ||
       !publishedDate || publishedDateCap < 2 ||
       !likeCount || likeCountCap < 2) return -1;

   viewCount[0] = publishedDate[0] = likeCount[0] = 0;
   if (ensureVisitorData() != 0) {
      logWarn("[yt] metadata visitor session unavailable video=%s\n", videoId);
      return -1;
   }

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0;
   int rc = fetchPlayerClient(
      "\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"",
      WEB_UA, "1", WEB_VERSION, videoId, resp, &respLen);
   if (rc != 0) {
      logWarn("[yt] WEB metadata player request failed video=%s rc=%d\n", videoId, rc);
      free(resp);
      return rc;
   }

   const char *end = resp + respLen;
   const char *detailsEnd = NULL;
   const char *details = jsonObjectForKey(resp, end, "videoDetails", &detailsEnd);
   if (details)
      jsonString(details, detailsEnd, "viewCount", viewCount, viewCountCap);

   const char *microEnd = NULL;
   const char *micro = jsonObjectForKey(resp, end, "playerMicroformatRenderer", &microEnd);
   if (micro) {
      // Prefer microformat values, which are also present on WEB responses that
      // have no playable formats. Fall back to videoDetails for views/channel.
      char rawDate[64] = "";
      if (jsonString(micro, microEnd, "publishDate", rawDate, sizeof rawDate) ||
          jsonString(micro, microEnd, "uploadDate", rawDate, sizeof rawDate)) {
         int dateLen = (int)strlen(rawDate);
         if (dateLen > 10) dateLen = 10;
         if (dateLen > 0 && dateLen < publishedDateCap) {
            memcpy(publishedDate, rawDate, dateLen);
            publishedDate[dateLen] = 0;
         }
      }
      jsonString(micro, microEnd, "likeCount", likeCount, likeCountCap);
      if (!viewCount[0])
         jsonString(micro, microEnd, "viewCount", viewCount, viewCountCap);
   }

   free(resp);
   if (!viewCount[0] && !publishedDate[0] && !likeCount[0]) {
      logWarn("[yt] WEB metadata unavailable video=%s\n", videoId);
      return -1;
   }
   logInfo("[yt] WEB metadata video=%s views=%s published=%s likes=%s\n",
           videoId, viewCount, publishedDate, likeCount);
   return 0;
}

static int isMembersOnlyResponse(const char *text);

// Resolve membership restriction and current LIVE state from one WEB player response.
// Membership is read only from playability status, not arbitrary description text.
static int youtubeGetVideoStatus(const char *videoId, int *isLive)
{
   if (isLive) *isLive = 0;
   if (!videoId || strlen(videoId) != 11) return -1;
   if (ensureVisitorData() != 0) return -1;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0;
   int rc = fetchPlayerClient(
      "\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"",
      WEB_UA, "1", WEB_VERSION, videoId, resp, &respLen);
   if (rc != 0) {
      logWarn("[yt] members-only check request failed video=%s rc=%d\n", videoId, rc);
      free(resp);
      return -1;
   }

   const char *end = resp + respLen;
   if (isLive)
      *isLive = strstr(resp, "\"isLive\":true") != NULL ||
                strstr(resp, "\"isLiveNow\":true") != NULL;
   const char *playEnd = NULL;
   const char *play = jsonObjectForKey(resp, end, "playabilityStatus", &playEnd);
   int membersOnly = 0;
   if (play) {
      char reason[512] = "";
      char messages[3][192] = {{0}};
      jsonString(play, playEnd, "reason", reason, sizeof reason);
      membersOnly = isMembersOnlyResponse(reason);
      int messageCount = jsonArrayStrings(play, playEnd, "messages", messages, 3);
      for (int i = 0; !membersOnly && i < messageCount; i++)
         membersOnly = isMembersOnlyResponse(messages[i]);
   }

   if (!membersOnly) {
      const char *screenEnd = NULL;
      const char *screen = jsonObjectForKey(resp, end, "errorScreen", &screenEnd);
      const char *rendererEnd = NULL;
      const char *renderer = screen ? jsonObjectForKey(screen, screenEnd,
                                      "playerErrorMessageRenderer", &rendererEnd) : NULL;
      if (renderer) {
         char reason[256] = "";
         char subreason[384] = "";
         jsonString(renderer, rendererEnd, "reason", reason, sizeof reason);
         const char *subEnd = NULL;
         const char *sub = jsonObjectForKey(renderer, rendererEnd, "subreason", &subEnd);
         if (sub) {
            if (!jsonString(sub, subEnd, "simpleText", subreason, sizeof subreason))
               jsonString(sub, subEnd, "text", subreason, sizeof subreason);
         }
         membersOnly = isMembersOnlyResponse(reason) || isMembersOnlyResponse(subreason);
      }
   }

   free(resp);
   logInfo("[yt] video status video=%s membersOnly=%s live=%d\n",
           videoId, membersOnly ? "yes" : "no", isLive ? *isLive : -1);
   return membersOnly;
}

static int youtubeIsVideoMembersOnly(const char *videoId)
{
   return youtubeGetVideoStatus(videoId, NULL);
}

int getNextVideoId(const char *videoId, char *nextVideoId, int nextVideoIdCap, char *nextTitle, int nextTitleCap, const char watchedIds[][12], int watchedCount)
{
   if (!videoId || !videoId[0] || !nextVideoId || nextVideoIdCap < 12) return -1;
   nextVideoId[0] = 0;
   if (nextTitle && nextTitleCap > 0) nextTitle[0] = 0;

   char body[512];
   int bodyLen = snprintf(body, sizeof body, NEXT_BODY_FMT, WEB_VERSION, getContentRegionCode(), videoId);
   if (bodyLen <= 0 || bodyLen >= (int)sizeof body) return -1;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };
   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", NEXT_URL, headers, sizeof headers / sizeof headers[0],
                      body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0 || status != 200) {
      logWarn("[yt] autoplay /next failed rc=%d status=%d\n", rc, status);
      free(resp);
      return rc < 0 ? rc : -status;
   }

   const char *end = resp + respLen;
   const char *secondary = strstr(resp, "\"secondaryResults\"");
   const char *results = secondary && secondary < end ? strstr(secondary, "\"results\":[") : NULL;
   if (results && results < end) {
      const char *scan = results + 10;
      while (scan < end) {
         const char *item = strstr(scan, "\"lockupViewModel\":{");
         if (!item || item >= end) break;
         const char *next = strstr(item + 1, "\"lockupViewModel\":{");
         const char *itemEnd = next && next < end ? next : end;
         char candidate[16] = "";
         if (jsonString(item, itemEnd, "contentId", candidate, sizeof candidate) &&
             strlen(candidate) == 11 && strcmp(candidate, videoId) != 0) {
            int valid = 1;
            for (int i = 0; i < 11; i++) if (!isIdChar(candidate[i])) valid = 0;
            int alreadyWatched = 0;
            if (valid) {
               for (int i = 0; i < watchedCount; i++) {
                  if (strcmp(candidate, watchedIds[i]) == 0) {
                     alreadyWatched = 1;
                     break;
                  }
               }
            }
            if (valid && !alreadyWatched) {
               strCopy(nextVideoId, nextVideoIdCap, candidate);
               if (nextTitle && nextTitleCap > 0) {
                  const char *metadata = strstr(item, "\"lockupMetadataViewModel\"");
                  const char *title = metadata && metadata < itemEnd ? strstr(metadata, "\"title\":{") : NULL;
                  if (title && title < itemEnd)
                     jsonString(title, itemEnd, "content", nextTitle, nextTitleCap);
               }
               break;
            }
            if (alreadyWatched)
               logInfo("[yt] autoplay skipped watched recommendation: %s\n", candidate);
         }
         scan = itemEnd;
      }
   }
   free(resp);
   if (!nextVideoId[0]) {
      logWarn("[yt] autoplay: no recommendation in /next response\n");
      return -1;
   }
   logInfo("[yt] autoplay recommendation: %s -> %s\n", videoId, nextVideoId);
   return 0;
}

static int isMembersOnlyResponse(const char *text)
{
   static const char *phrases[] = { "members-only", "members only", "membros" };
   if (!text) return 0;
   for (int k = 0; k < 3; k++) {
      size_t n = strlen(phrases[k]);
      for (const char *p = text; *p; p++) {
         size_t i = 0;
         while (i < n && p[i]) {
            char a = p[i], b = phrases[k][i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) break;
            i++;
         }
         if (i == n) return 1;
      }
   }
   return 0;
}

static int extract(const char *input, StreamInfo *out)
{
   memset(out, 0, sizeof *out);

   char videoId[16];
   if (!extractVideoId(input, videoId, sizeof videoId)) return -1;
   if (ensureVisitorData() != 0) return -1;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0;
   logInfo("[yt] resolve begin videoId=%s clientName=VISIONOS clientNameHeader=101 clientVersion=%s visitorData=%s\n",
           videoId, PLAYER_VERSION, visitorData[0] ? "present" : "absent");
   int rc = fetchPlayer(PLAYER_BODY_FMT, PLAYER_UA, "101", PLAYER_VERSION, videoId, resp, &respLen);
   if (rc != 0) {
      logError("[yt] resolve request failed videoId=%s clientName=VISIONOS clientVersion=%s rc=%d\n",
               videoId, PLAYER_VERSION, rc);
      free(resp);
      return rc;
   }
   const char *end = resp + respLen;
   logInfo("[yt] resolve response videoId=%s clientName=VISIONOS clientVersion=%s respLen=%d\n",
           videoId, PLAYER_VERSION, respLen);

   // metadata, copied into out so it survives a live re-fetch overwriting resp below
   const char *details = strstr(resp, "\"videoDetails\"");
   jsonString(details ? details : resp, end, "title", out->title, sizeof out->title);
   jsonString(details ? details : resp, end, "author", out->author, sizeof out->author);
   jsonString(details ? details : resp, end, "shortDescription", out->description, sizeof out->description);
   char lenText[16];
   if (jsonString(details ? details : resp, end, "lengthSeconds", lenText, sizeof lenText))
      out->durationSeconds = atoi(lenText);
   parseVideoMetadata(resp, end, out);
   parseCaptionTracks(resp, end, out);
   // Resolve the default audio ID from captions.playerCaptionsTracklistRenderer.audioTracks.
   {
      const char *audioTracks = strstr(resp, "\"audioTracks\":[");
      int defaultIndex = -1;
      if (audioTracks && jsonInt(resp, end, "defaultAudioTrackIndex", &defaultIndex) && defaultIndex >= 0) {
         const char *scan = audioTracks;
         for (int i = 0; i <= defaultIndex; i++) {
            const char *id = strstr(scan, "\"audioTrackId\":\"");
            if (!id || id >= end) break;
            if (i == defaultIndex) jsonString(id, end, "audioTrackId", out->defaultAudioTrackId, sizeof out->defaultAudioTrackId);
            scan = id + 1;
         }
      }
   }

   // Active LIVE and the short post-live DVR recording are both numbered DASH segment streams.
   int liveNow = strstr(resp, "\"isLive\":true") != NULL;
   int postLive = !liveNow && strstr(resp, "\"isPostLiveDvr\":true") != NULL;
   if ((liveNow || postLive) && !strstr(resp, "\"dashManifestUrl\"")) {
      if (fetchPlayer(LIVE_BODY_FMT, LIVE_UA, "28", LIVE_VERSION, videoId, resp, &respLen) == 0)
         end = resp + respLen;
      postLive = !liveNow && strstr(resp, "\"isPostLiveDvr\":true") != NULL;
   }

   parseFormats(resp, end, out);
   out->isLive = liveNow;
   out->isPostLive = postLive;
   if (liveNow || postLive) {
      const char *streaming = strstr(resp, "\"streamingData\"");
      out->liveDvr = strstr(resp, "\"isLiveDvrEnabled\":true") != NULL;
      jsonInt(streaming ? streaming : resp, end, "targetDurationSec", &out->liveSegmentSec);
      jsonInt(streaming ? streaming : resp, end, "maxDvrDurationSec", &out->liveWindowSec);
      logInfo("[yt] %s (dash=%d formats=%d dur=%d) rewind=%s segment=%ds window=%ds\n",
              liveNow ? "live stream" : "post-live recording",
              strstr(resp, "\"dashManifestUrl\"") ? 1 : 0, out->formatCount, out->durationSeconds,
              out->liveDvr ? "on" : "off", out->liveSegmentSec, out->liveWindowSec);
      parseLiveManifest(resp, end, out, postLive ? "dashvod" : "dashseg");
   }

   int membersOnly = isMembersOnlyResponse(resp);

   if (out->formatCount == 0) {
      if (isMembersOnlyResponse(resp)) membersOnly = 1;
      const char *streamingData = strstr(resp, "\"streamingData\"");
      const char *formats = streamingData ? strstr(streamingData, "\"formats\":[") : NULL;
      const char *adaptive = streamingData ? strstr(streamingData, "\"adaptiveFormats\":[") : NULL;
      logError("[yt] resolve parse videoId=%s streamingData=%d formats=%d adaptiveFormats=%d parsedFormatCount=%d\n",
               videoId, streamingData && streamingData < end,
               formats && formats < end, adaptive && adaptive < end, out->formatCount);

      char statusText[48] = "", reason[256] = "", errorReason[256] = "";
      char subreason[384] = "";
      char messages[3][192] = {{0}};
      const char *playEnd = NULL;
      const char *play = jsonObjectForKey(resp, end, "playabilityStatus", &playEnd);
      jsonString(play ? play : resp, play ? playEnd : end, "status", statusText, sizeof statusText);
      if (play) {
         jsonString(play, playEnd, "reason", reason, sizeof reason);
         int messageCount = jsonArrayStrings(play, playEnd, "messages", messages, 3);
         logError("[yt] no formats videoId=%s clientName=VISIONOS clientVersion=%s respLen=%d playability=%s reason='%s'\n",
                  videoId, PLAYER_VERSION, respLen, statusText, reason);
         logJsonObject("playabilityStatus", play, playEnd);
         for (int i = 0; i < messageCount; i++)
            logError("[yt] playability message[%d]='%s'\n", i, messages[i]);
      } else {
         logError("[yt] no formats videoId=%s clientName=VISIONOS clientVersion=%s respLen=%d playabilityStatus=missing\n",
                  videoId, PLAYER_VERSION, respLen);
      }

      const char *screenEnd = NULL;
      const char *screen = jsonObjectForKey(resp, end, "errorScreen", &screenEnd);
      const char *rendererEnd = NULL;
      const char *renderer = screen ? jsonObjectForKey(screen, screenEnd,
                                      "playerErrorMessageRenderer", &rendererEnd) : NULL;
      if (renderer) {
         logJsonObject("errorScreen.playerErrorMessageRenderer", renderer, rendererEnd);
         const char *subEnd = NULL;
         const char *sub = jsonObjectForKey(renderer, rendererEnd, "subreason", &subEnd);
         jsonString(renderer, rendererEnd, "reason", errorReason, sizeof errorReason);
         if (sub) {
            if (!jsonString(sub, subEnd, "simpleText", subreason, sizeof subreason))
               jsonString(sub, subEnd, "text", subreason, sizeof subreason);
         } else {
            subreason[0] = 0;
         }
         logError("[yt] errorScreen reason='%s' subreason='%s'\n", errorReason, subreason);
      } else if (screen) {
         logError("[yt] errorScreen present; playerErrorMessageRenderer missing\n");
      }
      if (visitorFromDisk && strcmp(statusText, "LOGIN_REQUIRED") == 0) dropVisitorData();   // stale session

      // Only retry VOD requests explicitly rejected by VISIONOS. Keep the normal
      // VISIONOS path unchanged for every other response, and never route live
      // requests through the VOD client fallback.
      if (!liveNow && !strstr(input, "/live/") &&
          !strstr(resp, "\"isLiveContent\":true") &&
          strcmp(statusText, "UNPLAYABLE") == 0) {
         logInfo("[yt] VISIONOS returned UNPLAYABLE; trying alternate VOD clients\n");
         if (retryUnplayableWithOtherClients(videoId, resp, &respLen, out) > 0) {
            logInfo("[yt] resolved videoId=%s with alternate client formats=%d\n",
                    videoId, out->formatCount);
            if (!visitorFromDisk) { saveVisitorData(); visitorFromDisk = 1; }
         }
      }
   } else {
      logInfo("[yt] resolved videoId=%s clientName=VISIONOS clientVersion=%s formats=%d\n",
              videoId, PLAYER_VERSION, out->formatCount);
      if (!visitorFromDisk) { saveVisitorData(); visitorFromDisk = 1; }   // persist a session that works
   }

   if (out->formatCount == 0 && isMembersOnlyResponse(resp)) membersOnly = 1;

   /* Some player clients omit videoDetails.shortDescription even when the
      video has a public description. Retry only this missing field with WEB. */
   if (!out->description[0]) {
      int metadataLen = 0;
      int metadataRc = fetchPlayerClient(
         "\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"",
         WEB_UA, "1", WEB_VERSION, videoId, resp, &metadataLen);
      if (metadataRc == 0 && metadataLen > 0) {
         const char *metadataEnd = resp + metadataLen;
         const char *metadataDetails = strstr(resp, "\"videoDetails\"");
         if (metadataDetails)
            jsonString(metadataDetails, metadataEnd, "shortDescription", out->description, sizeof out->description);
      }
      if (out->description[0])
         logInfo("[yt] description recovered from WEB metadata video=%s bytes=%d\n",
                 videoId, (int)strlen(out->description));
      else
         logWarn("[yt] description unavailable from player clients video=%s\n", videoId);
   }

   free(resp);
   return membersOnly && out->formatCount == 0 ? -2 : 0;
}

// copy src into out as a json string body value, escaping " and \ (and dropping
// control chars). the query comes from the on-screen keyboard.
static void jsonEscape(const char *src, char *out, int cap)
{
   int i = 0;
   for (; *src && i < cap - 2; src++) {
      unsigned char c = (unsigned char)*src;
      if (c == '"' || c == '\\') { out[i++] = '\\'; out[i++] = (char)c; }
      else if (c >= 0x20)        { out[i++] = (char)c; }
   }
   out[i] = 0;
}

// pull a nested string from a videoRenderer sub-object: find `container` (e.g. "\"ownerText\""), then read
// `key` (e.g. "text" for a runs array, "simpleText" for a plain value) within the block. Missing = left empty.
static void readField(const char *block, const char *end, const char *container, const char *key, char *out, int cap)
{
   const char *pos = strstr(block, container);
   if (pos && pos < end) jsonString(pos, end, key, out, cap);
}

// the next feed item at or after `from`. Search can return videos, playlists and newer lockups;
// trending shelves use gridVideoRenderer. Returns the nearest item (NULL if none).
static const char *nextItemBlock(const char *from)
{
   static const char *keys[] = { "\"videoRenderer\"", "\"gridVideoRenderer\"", "\"lockupViewModel\"", "\"playlistRenderer\":{" };
   const char *best = NULL;
   for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
      const char *hit = strstr(from, keys[i]);
      if (hit && (!best || hit < best)) best = hit;
   }
   return best;
}

static int isDuplicate(const SearchResults *out, const char *videoId)
{
   for (int i = 0; i < out->count; i++) if (strcmp(out->items[i].videoId, videoId) == 0) return 1;
   return 0;
}

static int isDuplicatePlaylist(const SearchResults *out, const char *playlistId)
{
   for (int i = 0; i < out->count; i++)
      if (out->items[i].isPlaylist && strcmp(out->items[i].playlistId, playlistId) == 0) return 1;
   return 0;
}

static int isSearchPlaylistId(const char *id)
{
   int length = (int)strlen(id);
   if (length < 2 || length > 61) return 0;
   for (const char *p = id; *p; p++) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
   }
   return 1;
}

static void readPlaylistThumbnailVideoId(const char *block, const char *end, char *out, int cap)
{
   const char *scan = block;
   while ((scan = strstr(scan, "\"url\":\"")) != NULL && scan < end) {
      char url[256];
      if (jsonString(scan, end, "url", url, sizeof url)) {
         const char *video = strstr(url, "/vi/");
         if (video) {
            video += 4;
            const char *slash = strchr(video, '/');
            if (slash && slash - video == 11 && cap > 11) {
               int valid = 1;
               for (int i = 0; i < 11; i++) {
                  char c = video[i];
                  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_')) valid = 0;
               }
               if (valid) { memcpy(out, video, 11); out[11] = 0; return; }
            }
         }
      }
      scan += 7;
   }
   if (cap) out[0] = 0;
}

static void readObjectStringField(const char *block, const char *end, const char *objectKey,
                                  const char *valueKey, char *out, int cap)
{
   const char *objectEnd = NULL;
   const char *object = jsonObjectForKey(block, end, objectKey, &objectEnd);
   if (!object || !jsonString(object, objectEnd, valueKey, out, cap)) {
      if (cap) out[0] = 0;
   }
}

static void readObjectTextField(const char *block, const char *end, const char *objectKey,
                                char *out, int cap)
{
   readObjectStringField(block, end, objectKey, "text", out, cap);
   if (!out[0]) readObjectStringField(block, end, objectKey, "simpleText", out, cap);
}

static void readPlaylistCount(const char *block, const char *end, char *out, int cap)
{
   readObjectTextField(block, end, "videoCountText", out, cap);
   if (!out[0]) readObjectTextField(block, end, "videoCountShortText", out, cap);
   if (!out[0]) readObjectTextField(block, end, "thumbnailText", out, cap);
   if (!out[0]) {
      char countText[16];
      if (jsonString(block, end, "videoCount", countText, sizeof countText)) {
         snprintf(out, cap, "%s videos", countText);
      } else {
         int count = 0;
         if (jsonInt(block, end, "videoCount", &count) && count >= 0)
            snprintf(out, cap, "%d videos", count);
      }
   }
}

static int parsePlaylistRenderer(const char *block, const char *end, SearchResult *item)
{
   const char *open = strchr(block, '{');
   if (!open || open >= end) return 0;
   const char *close = jsonObjectEnd(open, end);
   if (!close || close > end) close = end;
   if (!jsonString(open, close, "playlistId", item->playlistId, sizeof item->playlistId) ||
       !isSearchPlaylistId(item->playlistId)) return 0;
   readObjectTextField(open, close, "title", item->title, sizeof item->title);
   if (!item->title[0]) return 0;
   readObjectTextField(open, close, "shortBylineText", item->author, sizeof item->author);
   readPlaylistCount(open, close, item->duration, sizeof item->duration);
   readPlaylistThumbnailVideoId(open, close, item->thumbnailVideoId, sizeof item->thumbnailVideoId);
   item->isPlaylist = 1;
   return 1;
}

static int parseLockupPlaylistItem(const char *block, const char *end, SearchResult *item)
{
   const char *playlistType = strstr(block, "\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\"");
   if (!playlistType || playlistType >= end) return 0;
   if (!jsonString(block, end, "contentId", item->playlistId, sizeof item->playlistId) ||
       !isSearchPlaylistId(item->playlistId)) return 0;
   readObjectStringField(block, end, "lockupMetadataViewModel", "content", item->title, sizeof item->title);
   if (!item->title[0]) return 0;
   readField(block, end, "\"metadataParts\"", "content", item->author, sizeof item->author);
   readObjectTextField(block, end, "thumbnailBadgeViewModel", item->duration, sizeof item->duration);
   readPlaylistThumbnailVideoId(block, end, item->thumbnailVideoId, sizeof item->thumbnailVideoId);
   item->isPlaylist = 1;
   return 1;
}

// copy the first "content":"..." value within [from,end) whose text ends with `suffix` (e.g. " views",
// " ago"). lockupViewModel metadata rows are plain content strings, not keyed by field.
static void readContentEndingWith(const char *from, const char *end, const char *suffix, char *out, int cap)
{
   int suffixLen = (int)strlen(suffix);
   const char *p = from;
   while ((p = strstr(p, "\"content\":\"")) != NULL && p + 11 <= end) {
      p += 11;                                       // past "content":"
      const char *close = memchr(p, '"', end - p);
      if (!close) break;
      int length = (int)(close - p);
      if (length >= suffixLen && length < cap && strncmp(close - suffixLen, suffix, suffixLen) == 0) {
         memcpy(out, p, length); out[length] = 0; return;
      }
      p = close + 1;
   }
}

// the owning channel's UC id: the first "browseId":"UC..." in the item block (the video owner for a
// videoRenderer, the author for a lockup). left empty if absent.
static void readChannelId(const char *block, const char *end, char *out, int cap)
{
   const char *p = strstr(block, "\"browseId\":\"UC");
   if (p && p < end) jsonString(p, end, "browseId", out, cap);
   else if (cap) out[0] = 0;
}

// fill one result from a videoRenderer (search) or gridVideoRenderer (trending) block. 1 if it has a title.
static int parseRendererItem(const char *block, const char *end, SearchResult *item)
{
   if (!jsonString(block, end, "videoId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;
   readChannelId(block, end, item->channelId, sizeof item->channelId);
   readField(block, end, "\"title\"", "text", item->title, sizeof item->title);
   if (!item->title[0])   // gridVideoRenderer (trending) uses a plain simpleText title
      readField(block, end, "\"title\"", "simpleText", item->title, sizeof item->title);
   readField(block, end, "\"lengthText\"",        "simpleText", item->duration,  sizeof item->duration);
   readField(block, end, "\"ownerText\"",         "text",       item->author,    sizeof item->author);
   if (!item->author[0])   // trending items carry the channel in shortBylineText instead
      readField(block, end, "\"shortBylineText\"", "text", item->author, sizeof item->author);
   readField(block, end, "\"viewCountText\"",     "simpleText", item->views,     sizeof item->views);
   readField(block, end, "\"publishedTimeText\"", "simpleText", item->published, sizeof item->published);
   const char *livePos = strstr(block, "\"style\":\"LIVE\"");   // thumbnailOverlayTimeStatusRenderer
   item->isLive = (livePos && livePos < end);
   return item->title[0] != 0;
}

// fill one result from a lockupViewModel VIDEO block (youtube's newer feed-item format, seen on channel
// pages and other surfaces). 1 if it's a playable video with a title; albums/playlists (other contentTypes) are skipped.
static int parseLockupItem(const char *block, const char *end, SearchResult *item)
{
   const char *videoType = strstr(block, "\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\"");
   if (!videoType || videoType >= end) return 0;
   if (!jsonString(block, end, "contentId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;

   readChannelId(block, end, item->channelId, sizeof item->channelId);
   readField(block, end, "\"lockupMetadataViewModel\"", "content", item->title, sizeof item->title);
   readField(block, end, "\"metadataParts\"",           "content", item->author, sizeof item->author);
   // on a channel's own page the first metadata part is the view count, not the channel name - drop it so
   // the caller can substitute the real channel name (the channel is implied on that page).
   if (strstr(item->author, " views") || strstr(item->author, " watching")) item->author[0] = 0;
   readField(block, end, "\"thumbnailBadgeViewModel\"", "text",    item->duration, sizeof item->duration);
   readContentEndingWith(block, end, " views", item->views, sizeof item->views);
   readContentEndingWith(block, end, " ago",   item->published, sizeof item->published);
   const char *livePos = strstr(block, "\"style\":\"LIVE\"");
   item->isLive = (livePos && livePos < end) || strcmp(item->duration, "LIVE") == 0;
   if (item->isLive) item->duration[0] = 0;
   return item->title[0] != 0;
}

// Scan WEB innertube feed items. Search can mix videoRenderer and playlistRenderer; newer surfaces
// may use lockupViewModel. Videos and playlists are deduplicated by their respective IDs.
static void parseVideoResults(const char *resp, int respLen, SearchResults *out)
{
   const char *end = resp + respLen;
   const char *scan = resp;
   while (out->count < MAX_SEARCH_RESULTS) {
      const char *block = nextItemBlock(scan);
      if (!block) break;
      const char *next = nextItemBlock(block + 1);
      const char *blockEnd = (next && next < end) ? next : end;

      SearchResult *item = &out->items[out->count];
      memset(item, 0, sizeof *item);
      int isPlaylistRenderer = strncmp(block + 1, "playlistRenderer", 15) == 0;
      int isLockup = strncmp(block + 1, "lockupViewModel", 15) == 0;
      int ok = isPlaylistRenderer ? parsePlaylistRenderer(block, blockEnd, item) :
               isLockup ? (parseLockupPlaylistItem(block, blockEnd, item) ||
                           parseLockupItem(block, blockEnd, item)) :
               parseRendererItem(block, blockEnd, item);
      // drop members-only videos: they resolve to UNPLAYABLE for an anonymous session (no stream, no author)
      const char *membersBadge = strstr(block, "Members only");
      if (!item->isPlaylist && membersBadge && membersBadge < blockEnd) ok = 0;
      if (ok && (item->isPlaylist ? !isDuplicatePlaylist(out, item->playlistId)
                                  : !isDuplicate(out, item->videoId))) out->count++;
      scan = blockEnd;
   }

   // the "load more" token for the next page. anchor on the OBJECT ("...":{) not the bare name - youtube's
   // response carries a schema list of all renderer names ("...","...") and the bare string matches that
   // first, yielding a garbage token. the real continuationItemRenderer holds continuationCommand > token.
   const char *more = strstr(resp, "\"continuationItemRenderer\":{");
   if (more && more < end) jsonString(more, end, "token", out->continuation, sizeof out->continuation);

   // channel feeds carry the channel's own title in metadata; used to label videos on a channel page that
   // omit their author (the channel is implied). absent in search/trending responses (left empty). anchor on
   // the OBJECT ("...":{) - the bare name appears first in youtube's schema list, and the title after it is a
   // tab ("Home"), not the channel name.
   const char *channelMeta = strstr(resp, "\"channelMetadataRenderer\":{");
   if (channelMeta && channelMeta < end) jsonString(channelMeta, end, "title", out->channelName, sizeof out->channelName);

   // channel Videos feeds carry Latest/Popular/Oldest sort chips; each chipViewModel holds a continuation
   // token the screen re-browses with to re-sort. absent in search/trending responses (left empty).
   static const char *SORT_CHIP_LABELS[3] = { "Latest", "Popular", "Oldest" };
   for (int chip = 0; chip < 3; chip++) {
      char pat[48];
      snprintf(pat, sizeof pat, "\"chipViewModel\":{\"text\":\"%s\"", SORT_CHIP_LABELS[chip]);
      const char *found = strstr(resp, pat);
      if (found && found < end) jsonString(found, end, "token", out->sortTokens[chip], MAX_SORT_TOKEN);
   }
}

// POST a WEB innertube endpoint with a prebuilt json body, then parse videoRenderers from the response.
static int postAndParse(const char *url, const char *body, int bodyLen, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   HttpHeader headers[] = {   // WEB_UA is a pointer, so these can't be a file-scope const array
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };
   int headerCount = sizeof headers / sizeof headers[0];

   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", url, headers, headerCount, body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0)        { free(resp); return rc; }
   if (status != 200) { free(resp); return -status; }

   parseVideoResults(resp, respLen, out);
   free(resp);
   return 0;
}

static int search(const char *query, SortOrder sort, const char *continuation, SearchResults *out)
{
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (continuation) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, continuation);
   else {
      char escaped[256];
      jsonEscape(query, escaped, sizeof escaped);
      if (sort < 0 || sort >= SORT_COUNT) sort = SORT_RELEVANCE;
      bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, gl, escaped, SEARCH_SORT_PARAMS[sort]);
   }
   return postAndParse(SEARCH_URL, body, bodyLen, out);
}

// the surviving per-category channel-tab feeds, each a real feed logged-out. order matches TrendingCategory.
static const struct { const char *browseId, *params; } TRENDING_FEEDS[TREND_COUNT] = {
   { "UCOpNcN46UbXVtpKMrmU4Abg", "Egh0cmVuZGluZw%3D%3D" },              // TREND_GAMING
   { "UCEgdi0XIXXZ-qJOFPf4JSKw", "EglzcG9ydHN0YWK4AQCSAwDyBgQKAjIA" },  // TREND_SPORTS
   { "FEpodcasts_destination",   "qgcCCAM%3D" },                        // TREND_PODCASTS
};

static int fetchTrending(TrendingCategory category, const char *continuation, SearchResults *out)
{
   if (category < 0 || category >= TREND_COUNT) return -1;
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (continuation) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, continuation);
   else bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, TRENDING_FEEDS[category].browseId, TRENDING_FEEDS[category].params);
   return postAndParse(BROWSE_URL, body, bodyLen, out);
}

// a channel's Videos tab (sorted Latest by default). token NULL = first page via the videos-tab params;
// else a sort-chip or next-page continuation token. the response fills out->sortTokens for the sort chips.
#define CHANNEL_VIDEOS_PARAMS "EgZ2aWRlb3PyBgQKAjoA"
static int getChannel(const char *channelId, const char *token, SearchResults *out)
{
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (token) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, token);
   else       bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, channelId, CHANNEL_VIDEOS_PARAMS);
   return postAndParse(BROWSE_URL, body, bodyLen, out);
}

// The channel Streams tab includes live, upcoming and archived broadcasts. Keep only the
// entries carrying YouTube's active LIVE badge so they can be promoted above regular uploads.
#define CHANNEL_LIVE_PARAMS "EgdzdHJlYW1z8gYECgJ6AA=="
static int getChannelLive(const char *channelId, SearchResults *out)
{
   if (!channelId || !channelId[0] || !out) return -1;
   const char *gl = getContentRegionCode();
   char body[512];
   int bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, channelId, CHANNEL_LIVE_PARAMS);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) {
      memset(out, 0, sizeof *out);
      return -1;
   }

   int rc = postAndParse(BROWSE_URL, body, bodyLen, out);
   if (rc < 0) return rc;

   int kept = 0;
   for (int i = 0; i < out->count; i++) {
      if (!out->items[i].isLive) continue;
      if (kept != i) out->items[kept] = out->items[i];
      kept++;
   }
   out->count = kept;
   out->continuation[0] = 0;
   logInfo("[yt] channel live channel=%s active=%d\n", channelId, kept);
   return 0;
}

// Channel category feeds use YouTube's own browse tabs. Streams intentionally retain
// archived broadcasts as VOD entries; Videos remains the default channel landing feed.
#define CHANNEL_SHORTS_PARAMS    "EgZzaG9ydHPyBgQKAjoA"
#define CHANNEL_PLAYLISTS_PARAMS "EghwbGF5bGlzdHPyBgQKAjoA"
static int getChannelCategory(const char *channelId, int category, const char *continuation, SearchResults *out)
{
   if (!channelId || !channelId[0] || !out || category < 0 || category > 3) return -1;
   if (category == 0) return getChannel(channelId, continuation, out);

   const char *params = category == 1 ? CHANNEL_LIVE_PARAMS :
                        category == 2 ? CHANNEL_SHORTS_PARAMS : CHANNEL_PLAYLISTS_PARAMS;
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen = continuation
      ? snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, continuation)
      : snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, channelId, params);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) {
      memset(out, 0, sizeof *out);
      return -1;
   }
   int rc = postAndParse(BROWSE_URL, body, bodyLen, out);
   if (rc == 0 && category == 1) {
      // Streams includes current, upcoming and archived broadcasts. Keep the full
      // feed here; the isLive flag distinguishes broadcasts currently on air.
      for (int i = 0; i < out->count; i++)
         if (!out->items[i].channelId[0]) strCopy(out->items[i].channelId, sizeof out->items[i].channelId, channelId);
      int liveCount = 0;
      for (int i = 0; i < out->count; i++) {
         if (!out->items[i].isLive) continue;
         if (i != liveCount) {
            SearchResult liveItem = out->items[i];
            memmove(&out->items[liveCount + 1], &out->items[liveCount],
                    (i - liveCount) * sizeof(out->items[0]));
            out->items[liveCount] = liveItem;
         }
         liveCount++;
      }
   }
   return rc;
}

// ---- signed-in account feeds ----
// YouTube only accepts an OAuth bearer token from the TV client (WEB answers 400 with one), and the TV
// client answers in its own tile layout, read by youtube-tv.c. Playback stays on the anonymous player
// client above: the account is only for browsing.
static int postTvBrowse(const char *body, int bodyLen, const char *accessToken, char *resp, int *respLen, int *status)
{
   char authorization[OAUTH_ACCESS_TOKEN_MAX + 8];
   snprintf(authorization, sizeof authorization, "Bearer %s", accessToken);
   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", TV_USER_AGENT },
      { "X-YouTube-Client-Name", TV_CLIENT_NAME_ID },
      { "X-YouTube-Client-Version", TV_CLIENT_VERSION },
      { "Authorization", authorization },
      { "Accept-Encoding", "identity" },
   };
   return fetchHttp("POST", BROWSE_URL, headers, sizeof headers / sizeof headers[0], body, bodyLen, resp, RESP_CAP, respLen, status);
}

static int fetchAccountFeed(AccountFeed feed, const char *continuation, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   char body[MAX_CONTINUATION + 512];
   int bodyLen = buildTvBrowseBody(body, sizeof body, feed, continuation);
   if (bodyLen < 0) return -1;

   char accessToken[OAUTH_ACCESS_TOKEN_MAX];
   if (getAccessToken(accessToken, sizeof accessToken, 0) != 0) return -1;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = postTvBrowse(body, bodyLen, accessToken, resp, &respLen, &status);
   if (rc == 0 && status == 401 && getAccessToken(accessToken, sizeof accessToken, 1) == 0)   // expired early: renew once
      rc = postTvBrowse(body, bodyLen, accessToken, resp, &respLen, &status);
   if (rc < 0 || status != 200) {
      logError("[yt] account feed %d rc=%d status=%d\n", (int)feed, rc, status);
      free(resp);
      return rc < 0 ? rc : -status;
   }

   parseTvTiles(resp, respLen, out);
   free(resp);
   return 0;
}

typedef void (*TvBrowseParser)(const char *resp, int respLen, SearchResults *out);

static int fetchTvCustomBrowse(const char *body, int bodyLen, SearchResults *out, TvBrowseParser parser)
{
   memset(out, 0, sizeof *out);
   char accessToken[OAUTH_ACCESS_TOKEN_MAX];
   if (getAccessToken(accessToken, sizeof accessToken, 0) != 0) return -1;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;
   int respLen = 0, status = 0;
   int rc = postTvBrowse(body, bodyLen, accessToken, resp, &respLen, &status);
   if (rc == 0 && status == 401 && getAccessToken(accessToken, sizeof accessToken, 1) == 0)
      rc = postTvBrowse(body, bodyLen, accessToken, resp, &respLen, &status);
   if (rc < 0 || status != 200) {
      logError("[yt] account library browse rc=%d status=%d\n", rc, status);
      free(resp);
      return rc < 0 ? rc : -status;
   }
   parser(resp, respLen, out);
   free(resp);
   return 0;
}

static int fetchAccountPlaylists(const char *continuation, SearchResults *out)
{
   char body[MAX_CONTINUATION + 512];
   int bodyLen = buildTvLibraryBody(body, sizeof body, continuation);
   if (bodyLen < 0) { memset(out, 0, sizeof *out); return -1; }
   return fetchTvCustomBrowse(body, bodyLen, out, parseTvPlaylists);
}

static int classifyPlaylistError(const char *resp, int status)
{
   if (status == 401 || status == 403) return PLAYLIST_FETCH_RESTRICTED;
   if (status == 404 || status == 410) return PLAYLIST_FETCH_UNAVAILABLE;
   if (resp) {
      if (strstr(resp, "PLAYLIST_PRIVATE") || strstr(resp, "This playlist is private") ||
          strstr(resp, "playlist is private") || strstr(resp, "PRIVATE_PLAYLIST") ||
          strstr(resp, "private playlist"))
         return PLAYLIST_FETCH_PRIVATE;
      if (strstr(resp, "LOGIN_REQUIRED") || strstr(resp, "Members only") ||
          strstr(resp, "members only") || strstr(resp, "AGE_RESTRICTED") ||
          strstr(resp, "UNAUTHENTICATED") || strstr(resp, "UNPLAYABLE") ||
          strstr(resp, "CONTENT_CHECK_REQUIRED") || strstr(resp, "AGE_CHECK_REQUIRED") ||
          strstr(resp, "restricted"))
         return PLAYLIST_FETCH_RESTRICTED;
      if (strstr(resp, "Playlist not found") || strstr(resp, "playlist not found") ||
          strstr(resp, "This playlist is unavailable") || strstr(resp, "playlist is unavailable") ||
          strstr(resp, "not available") || strstr(resp, "Video unavailable"))
         return PLAYLIST_FETCH_UNAVAILABLE;
   }
   return PLAYLIST_FETCH_UNAVAILABLE;
}

static const char *findNextWebPlaylistBlock(const char *scan, const char *end, int *isLockup)
{
   static const char RENDERER[] = "\"playlistVideoRenderer\":{";
   static const char LOCKUP[] = "\"lockupViewModel\":{";
   while (scan < end) {
      const char *quote = memchr(scan, '\"', (size_t)(end - scan));
      if (!quote) return NULL;
      size_t remain = (size_t)(end - quote);
      if (remain >= sizeof RENDERER - 1 &&
          memcmp(quote, RENDERER, sizeof RENDERER - 1) == 0) {
         *isLockup = 0;
         return quote;
      }
      if (remain >= sizeof LOCKUP - 1 &&
          memcmp(quote, LOCKUP, sizeof LOCKUP - 1) == 0) {
         *isLockup = 1;
         return quote;
      }
      scan = quote + 1;
   }
   return NULL;
}

static void parseWebPlaylistVideos(const char *resp, int respLen, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   if (!resp || respLen <= 0) return;

   const char *end = resp + respLen;
   const char *scan = resp;
   while (out->count < MAX_SEARCH_RESULTS) {
      int isLockup = 0;
      const char *block = findNextWebPlaylistBlock(scan, end, &isLockup);
      if (!block) break;

      const char *open = strchr(block, '{');
      const char *blockEnd = open ? jsonObjectEnd(open, end) : NULL;
      if (!open || !blockEnd || blockEnd <= open || blockEnd > end) break;

      SearchResult parsed;
      memset(&parsed, 0, sizeof parsed);
      int ok = isLockup ? parseLockupItem(open, blockEnd, &parsed)
                        : parseRendererItem(open, blockEnd, &parsed);
      if (ok) {
         if (isLockup) {
            strCopy(parsed.thumbnailVideoId, sizeof parsed.thumbnailVideoId,
                    parsed.videoId);
         } else {
            readField(open, blockEnd, "\"shortBylineText\"", "text",
                      parsed.author, sizeof parsed.author);
         }
         if (!isDuplicate(out, parsed.videoId))
            out->items[out->count++] = parsed;
      }
      scan = blockEnd;
   }

   const char *more = strstr(resp, "\"continuationItemRenderer\":{");
   if (more && more < end)
      jsonString(more, end, "token", out->continuation, sizeof out->continuation);

   if (!out->continuation[0]) {
      const char *next = strstr(resp, "\"nextContinuationData\":{");
      if (next && next < end)
         jsonString(next, end, "continuation", out->continuation, sizeof out->continuation);
   }
   if (!out->continuation[0]) {
      const char *command = strstr(resp, "\"continuationCommand\":{");
      if (command && command < end)
         jsonString(command, end, "token", out->continuation, sizeof out->continuation);
   }
}

static void tagPlaylistContinuation(SearchResults *out, char client)
{
   if (!out->continuation[0]) return;
   size_t length = strlen(out->continuation);
   if (length + 3 > sizeof out->continuation) {
      out->continuation[0] = 0;
      return;
   }
   memmove(out->continuation + 2, out->continuation, length + 1);
   out->continuation[0] = client;
   out->continuation[1] = ':';
}

static int fetchWebPlaylistVideos(const char *playlistId, const char *continuation, SearchResults *out)
{
   if (!playlistId || !playlistId[0] || !out) return PLAYLIST_FETCH_UNAVAILABLE;
   memset(out, 0, sizeof *out);
   const char *gl = getContentRegionCode();
   int anonymous = !isSignedIn();
   if (anonymous && ensureVisitorData() != 0) {
      logWarn("[yt] anonymous playlist: visitorData unavailable\n");
      return PLAYLIST_FETCH_UNAVAILABLE;
   }

   char body[MAX_CONTINUATION + 768];
   int bodyLen;
   if (continuation) {
      if (anonymous) {
         bodyLen = snprintf(body, sizeof body,
            "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"%s\",\"visitorData\":\"%s\"}},\"continuation\":\"%s\"}",
            WEB_VERSION, gl, visitorData, continuation);
      } else {
         bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, continuation);
      }
   } else {
      char browseId[72];
      int idLength = snprintf(browseId, sizeof browseId, "VL%s", playlistId);
      if (idLength < 0 || idLength >= (int)sizeof browseId) return PLAYLIST_FETCH_UNAVAILABLE;
      if (anonymous) {
         bodyLen = snprintf(body, sizeof body,
            "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"%s\",\"visitorData\":\"%s\"}},\"browseId\":\"%s\"}",
            WEB_VERSION, gl, visitorData, browseId);
      } else {
         bodyLen = snprintf(body, sizeof body, WEB_CONTEXT_FMT ",\"browseId\":\"%s\"}", gl, browseId);
      }
   }
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return PLAYLIST_FETCH_UNAVAILABLE;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;
   HttpHeader headers[7] = {
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
      { NULL, NULL },
   };
   int headerCount = 6;
   if (anonymous) {
      headers[6].name = "X-Goog-Visitor-Id";
      headers[6].value = visitorData;
      headerCount = 7;
   }
   int respLen = 0, status = 0;
   if (!anonymous)
      logInfo("[yt] playlist web request id=%s page=%s\n",
              playlistId, continuation ? "continuation" : "first");
   int rc = fetchHttp("POST", BROWSE_URL, headers, headerCount,
                      body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0) {
      logWarn("[yt] playlist web transport failed id=%s rc=%d status=%d respLen=%d\n",
              playlistId, rc, status, respLen);
      free(resp);
      return rc;
   }
   if (status != 200) {
      int error = classifyPlaylistError(resp, status);
      logWarn("[yt] playlist browse failed id=%s status=%d error=%d\n", playlistId, status, error);
      free(resp);
      return error;
   }

   parseWebPlaylistVideos(resp, respLen, out);
   tagPlaylistContinuation(out, 'W');
   if (!anonymous)
      logInfo("[yt] playlist web parsed id=%s videos=%d continuation=%s\n",
              playlistId, out->count, out->continuation[0] ? "yes" : "no");
   if (out->count == 0 && !out->continuation[0] && !continuation) {
      // An empty first browse is an actual failure; an empty continuation is simply EOF.
      logWarn("[yt] playlist first page returned no videos id=%s\n", playlistId);
      int error = classifyPlaylistError(resp, status);
      free(resp);
      return error;
   }
   if (out->count == 0 && !out->continuation[0] && continuation)
      logInfo("[yt] playlist reached end id=%s\n", playlistId);
   free(resp);
   return 0;
}

static int fetchAccountPlaylistVideos(const char *playlistId, const char *continuation, SearchResults *out)
{
   if (!playlistId || !playlistId[0] || !out) return PLAYLIST_FETCH_UNAVAILABLE;

   const char *rawContinuation = continuation;
   char client = isSignedIn() ? 'T' : 'W';
   if (continuation && continuation[0] && continuation[1] == ':') {
      client = continuation[0];
      rawContinuation = continuation + 2;
   }

   if (client == 'T') {
      if (!isSignedIn()) return PLAYLIST_FETCH_RESTRICTED;
      char body[MAX_CONTINUATION + 512];
      int bodyLen = buildTvPlaylistBody(body, sizeof body, playlistId, rawContinuation);
      if (bodyLen >= 0) {
         int rc = fetchTvCustomBrowse(body, bodyLen, out, parseTvPlaylistVideos);
         if (rc == 0 && (continuation || out->count > 0)) {
            tagPlaylistContinuation(out, 'T');
            return 0;
         }
      }
      if (continuation) return PLAYLIST_FETCH_RESTRICTED;
   }

   // Public search playlists also open while signed out. WEB browse needs no OAuth; its continuation is
   // tagged so the next page cannot accidentally be sent to the authenticated TV client.
   return fetchWebPlaylistVideos(playlistId, rawContinuation, out);
}


// ---- Live feeds ----

#define LIVE_BROWSE_ID     "UC4R8DWoMoI7CAwX8_LjQHig"
#define LIVE_SEARCH_PARAMS "EgJAAQ=="
#define LIVE_SESSION_CAP   96

typedef enum { LIVE_KIND_BROWSE, LIVE_KIND_SEARCH } LiveKind;

static const struct {
   LiveKind kind;
   const char *name;
   const char *idOrQuery;
   const char *params;
   const char *fallbackQuery;
   int fallbackBelow;
} LIVE_FEEDS[LIVE_COUNT] = {
   { LIVE_KIND_BROWSE, "popular",  LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCKEK", "live",   1 },
   { LIVE_KIND_SEARCH, "music",    "music",        LIVE_SEARCH_PARAMS,     NULL,     0 },
   { LIVE_KIND_BROWSE, "gaming",   LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCJ8K", "gaming", 1 },
   { LIVE_KIND_BROWSE, "sports",   LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCKAK", "sports", 1 },
   { LIVE_KIND_SEARCH, "podcasts", "podcast",      LIVE_SEARCH_PARAMS,     NULL,     0 },
   { LIVE_KIND_BROWSE, "news",     LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCKUK", NULL,     6 },
};

static const struct { const char *codes, *word; } LIVE_NEWS_WORDS[] = {
   { "DE AT CH", "nachrichten" },
   { "FR",       "actualit\xc3\xa9" "s" },
   { "IT",       "notizie" },
   { "ES MX",    "noticias" },
   { "PT BR",    "not\xc3\xad" "cias" },
   { "NL BE",    "nieuws" },
   { "PL",       "wiadomo\xc5\x9b" "ci" },
   { "CZ",       "zpr\xc3\xa1" "vy" },
   { "TR",       "haber" },
   { "RU",       "\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x81\xd1\x82\xd0\xb8" },
   { "UA BG",    "\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd0\xbd\xd0\xb8" },
   { "RS",       "\xd0\xb2\xd0\xb5\xd1\x81\xd1\x82\xd0\xb8" },
   { "HR",       "vijesti" },
   { "GR",       "\xce\xb5\xce\xb9\xce\xb4\xce\xae\xcf\x83\xce\xb5\xce\xb9\xcf\x82" },
   { "SE NO",    "nyheter" },
   { "DK",       "nyheder" },
   { "FI",       "uutiset" },
   { "HU",       "h\xc3\xad" "rek" },
   { "RO",       "\xc8\x99" "tiri" },
   { "JP",       "\xe3\x83\x8b\xe3\x83\xa5\xe3\x83\xbc\xe3\x82\xb9" },
   { "KR",       "\xeb\x89\xb4\xec\x8a\xa4" },
};

static const char *liveNewsWord(const char *gl)
{
   for (unsigned i = 0; i < sizeof LIVE_NEWS_WORDS / sizeof LIVE_NEWS_WORDS[0]; i++) {
      const char *code = LIVE_NEWS_WORDS[i].codes;
      while (code[0] && code[1]) {
         if (code[0] == gl[0] && code[1] == gl[1]) return LIVE_NEWS_WORDS[i].word;
         code += code[2] ? 3 : 2;
      }
   }
   return "news";
}

static int isLiveSessionChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
          c == '+' || c == '/' || c == '=';
}

static int isLiveSessionToken(const char *text)
{
   int length = 0;
   while (isLiveSessionChar(text[length])) length++;
   return length > 0 && text[length] == 0;
}

static void captureLiveSession(const char *resp, const char *end, char *out, int cap)
{
   char raw[128] = "";
   out[0] = 0;
   if (!jsonString(resp, end, "visitorData", raw, sizeof raw) ||
       (int)strlen(raw) >= (int)sizeof raw - 1) return;
   for (const char *c = raw; *c; c++)
      if (!isLiveSessionChar(*c) && *c != '%') return;
   if (urlDecode(raw, out, cap) < 0 ||
       (int)strlen(out) >= cap - 1 || !isLiveSessionToken(out))
      out[0] = 0;
}

// Live shelves need to echo the visitor session returned by YouTube. Search-style
// feeds use the same parser but do not need that header. append preserves shelf
// items when a thin shelf is topped up by a fallback search.
static int postAndParseLive(const char *url, const char *body, int bodyLen,
                            const char *visitorId, char *replyVisitor, int replyVisitorCap,
                            int append, SearchResults *out)
{
   if (!append) memset(out, 0, sizeof *out);
   if (replyVisitor && replyVisitorCap > 0) replyVisitor[0] = 0;

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   HttpHeader headers[7] = {
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
      { NULL, NULL },
   };
   int headerCount = 6;
   if (visitorId && visitorId[0]) {
      headers[6].name = "X-Goog-Visitor-Id";
      headers[6].value = visitorId;
      headerCount = 7;
   }

   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", url, headers, headerCount, body, bodyLen,
                      resp, RESP_CAP, &respLen, &status);
   if (rc < 0 || status != 200) {
      free(resp);
      return rc < 0 ? rc : -status;
   }

   if (append) out->continuation[0] = 0;
   parseVideoResults(resp, respLen, out);
   if (replyVisitor && replyVisitorCap > 0)
      captureLiveSession(resp, resp + respLen, replyVisitor, replyVisitorCap);
   free(resp);
   return 0;
}

static int wrapLiveToken(char *token, int cap, const char *session)
{
   int tokenLength = (int)strlen(token);
   int sessionLength = session ? (int)strlen(session) : 0;
   int prefix = session ? 3 + sessionLength : 2;
   if (strchr(token, '|') || tokenLength + prefix >= cap) {
      token[0] = 0;
      return -1;
   }
   memmove(token + prefix, token, tokenLength + 1);
   token[0] = session ? 'b' : 's';
   token[1] = '|';
   if (session) {
      memcpy(token + 2, session, sessionLength);
      token[2 + sessionLength] = '|';
   }
   return 0;
}

static const char *splitLiveToken(const char *continuation, int *browse, char *session, int cap)
{
   session[0] = 0;
   if (continuation[0] == 's' && continuation[1] == '|') {
      *browse = 0;
      return continuation[2] ? continuation + 2 : NULL;
   }
   if (continuation[0] != 'b' || continuation[1] != '|') return NULL;

   const char *start = continuation + 2;
   const char *bar = strchr(start, '|');
   int length = bar ? (int)(bar - start) : 0;
   if (length <= 0 || length >= cap || !bar[1]) return NULL;
   memcpy(session, start, length);
   session[length] = 0;
   if (!isLiveSessionToken(session)) {
      session[0] = 0;
      return NULL;
   }
   *browse = 1;
   return bar + 1;
}

static int keepLiveOnly(SearchResults *out, int from)
{
   int kept = from;
   for (int i = from; i < out->count; i++) {
      if (!out->items[i].isLive) continue;
      if (kept != i) out->items[kept] = out->items[i];
      kept++;
   }
   out->count = kept;
   return kept - from;
}

static int fetchLive(LiveCategory category, const char *continuation, SearchResults *out, int (*stopped)(void))
{
   if (category < 0 || category >= LIVE_COUNT) {
      memset(out, 0, sizeof *out);
      return -1;
   }

   const char *gl = getContentRegionCode();
   const char *name = LIVE_FEEDS[category].name;
   char body[MAX_CONTINUATION + 512];
   char sent[LIVE_SESSION_CAP] = "";
   char reply[LIVE_SESSION_CAP] = "";
   char escaped[256];
   int browse = LIVE_FEEDS[category].kind == LIVE_KIND_BROWSE;
   int bodyLen;

   if (!continuation) {
      jsonEscape(LIVE_FEEDS[category].idOrQuery, escaped, sizeof escaped);
      if (browse)
         bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, escaped, LIVE_FEEDS[category].params);
      else
         bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, gl, escaped, LIVE_FEEDS[category].params);
   } else {
      const char *token = splitLiveToken(continuation, &browse, sent, sizeof sent);
      if (!token) {
         logError("[yt] live %s: invalid continuation token\n", name);
         memset(out, 0, sizeof *out);
         return 0;
      }
      bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, token);
   }
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) {
      memset(out, 0, sizeof *out);
      return -1;
   }

   int rc = postAndParseLive(browse ? BROWSE_URL : SEARCH_URL, body, bodyLen,
                             continuation && browse ? sent : NULL,
                             browse ? reply : NULL, sizeof reply, 0, out);
   int fallbackBelow = continuation ? 0 : LIVE_FEEDS[category].fallbackBelow;
   if (rc < 0 && (fallbackBelow == 0 || rc > -100)) {
      logError("[yt] live %s %s failed rc=%d\n", name, continuation ? "next page" : "page 1", rc);
      return rc;
   }

   char note[128] = "";
   if (rc < 0) snprintf(note, sizeof note, ", shelf HTTP %d", -rc);
   int parsed = out->count;
   int live = keepLiveOnly(out, 0);
   int tokenIsBrowse = browse;

   if (live < fallbackBelow) {
      const char *word = LIVE_FEEDS[category].fallbackQuery
                       ? LIVE_FEEDS[category].fallbackQuery : liveNewsWord(gl);
      char fallbackQuery[256];
      jsonEscape(word, fallbackQuery, sizeof fallbackQuery);
      int noted = (int)strlen(note);
      bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, gl, fallbackQuery, LIVE_SEARCH_PARAMS);
      if (stopped && stopped()) {
         snprintf(note + noted, sizeof note - noted, ", stopped before fallback search");
      } else if (bodyLen >= 0 && bodyLen < (int)sizeof body) {
         int before = out->count;
         int fallbackRc = postAndParseLive(SEARCH_URL, body, bodyLen, NULL, NULL, 0, 1, out);
         if (fallbackRc == 0) {
            int added = keepLiveOnly(out, before);
            snprintf(note + noted, sizeof note - noted, ", fallback \"%s\": +%d live", word, added);
            tokenIsBrowse = 0;
            rc = 0;
         } else {
            snprintf(note + noted, sizeof note - noted, ", fallback \"%s\" failed rc=%d", word, fallbackRc);
         }
      }
      if (rc < 0) {
         logError("[yt] live %s page 1 failed%s\n", name, note);
         return rc;
      }
   }

   const char *session = reply[0] ? reply : sent;
   if (tokenIsBrowse && !session[0]) out->continuation[0] = 0;
   int tooLong = out->continuation[0] &&
      wrapLiveToken(out->continuation, sizeof out->continuation, tokenIsBrowse ? session : NULL) != 0;

   logInfo("[yt] live %s gl=%s %s: parsed %d, live %d%s, next=%d%s%s\n",
           name, gl, continuation ? "next page" : "page 1", parsed, live, note,
           out->continuation[0] ? 1 : 0,
           tokenIsBrowse ? (session[0] ? ", session=1" : ", session=0") : "",
           tooLong ? ", token too long" : "");
   return 0;
}

const Extractor youtubeExtractor = {
   "youtube", matches, extract, search, fetchTrending, getChannel, fetchAccountFeed,
   youtubeGetVideoChannelId, youtubeGetVideoMetadata, youtubeIsVideoMembersOnly, fetchLive,
   getChannelLive, fetchAccountPlaylists, fetchAccountPlaylistVideos,
   youtubeGetVideoStatus, getChannelCategory
};
