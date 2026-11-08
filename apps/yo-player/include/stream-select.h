#pragma once

// stream-select - picks which of a resolved video's formats to use. Shared by playback and download so a
// downloaded file is the exact stream the player would show (same PS3-decodable H.264 + AAC pair).

#include "extractor.h"   // StreamInfo / StreamFormat

// best decodable H.264/mp4 video stream: highest resolution the PS3 decoder can keep up with, and at a
// given resolution the higher frame rate. favours the adaptive (video-only) streams over the 360p muxed
// one. NULL if none is usable.
const StreamFormat *pickBestVideo(const StreamInfo *info);

// Playback selector: additionally allows 1080p50/60 only when TEE initializes.
const StreamFormat *pickBestPlaybackVideo(const StreamInfo *info);

// True when a format is playable by the currently enabled PS3 decoder path.
int isPlaybackVideoFormatDecodable(const StreamFormat *format);

// Best format in a resolution profile: 0 <=480p60, 1 481-720p60, 2 721-1080p30.
const StreamFormat *pickVideoForProfile(const StreamInfo *info, int profile);

// the AAC/mp4 audio-only stream to pair with a video-only pick, preferring itag 140. NULL if none.
const StreamFormat *pickBestAudio(const StreamInfo *info);
