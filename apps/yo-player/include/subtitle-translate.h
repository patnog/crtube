#pragma once

// Local translation for synthesized subtitle tracks. YouTube's tlang endpoint can
// reject translated-caption requests, so CRTube fetches the original track and
// translates its cues through Google Translate instead.

#include "subtitles.h"

// State shared by the subtitle worker and playback UI while translation runs.
typedef struct {
   volatile float position;    // playback position in seconds, updated by the UI thread
   volatile int   cancel;      // set when another track is selected or playback closes
   volatile int   published;   // first translated chunk is ready for display
   volatile int   stopped;     // translation service stopped responding; remaining cues stay original
} SubtitleTranslation;

// Group and translate subtitle cues into `language` (for example "pt-BR").
// Returns 0 once the first translated chunk is published; negative if none was available.
int translateSubtitles(SubtitleTrack *track, const char *language, SubtitleTranslation *control);
