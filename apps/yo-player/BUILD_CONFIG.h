#pragma once

// Central build-time controls for yo-player diagnostics.
// Set BUILD_DIAG_ALL to 0 for a quiet build, then enable only the categories needed.
#ifndef BUILD_DIAG_ALL
#define BUILD_DIAG_ALL 0
#endif

#ifndef BUILD_DIAG_MAIN_LOOP
#define BUILD_DIAG_MAIN_LOOP BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_PLAY_RENDER
#define BUILD_DIAG_PLAY_RENDER BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_LIVE_RECOVERY
#define BUILD_DIAG_LIVE_RECOVERY BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_GFX
#define BUILD_DIAG_GFX BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_GFX_END_FRAME
#define BUILD_DIAG_GFX_END_FRAME BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_VIDEO_PLAYER
#define BUILD_DIAG_VIDEO_PLAYER BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_POST_UNLOCK
#define BUILD_DIAG_POST_UNLOCK BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_LIVE_SOURCE
#define BUILD_DIAG_LIVE_SOURCE BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_LIVE_URL_DETAILS
#define BUILD_DIAG_LIVE_URL_DETAILS BUILD_DIAG_ALL
#endif
#ifndef BUILD_DIAG_LOG_TIMING
#define BUILD_DIAG_LOG_TIMING BUILD_DIAG_ALL
#endif

// Subtitle render profiling is independent from the general playback diagnostics.
// Set to 1 to measure subtitle clock lookup, cue selection, label update and drawing.
#ifndef BUILD_DIAG_SUBTITLE_RENDER
#define BUILD_DIAG_SUBTITLE_RENDER 1
#endif

// Additional network/frame diagnostics retain their previous conservative defaults.
#ifndef BUILD_DIAG_FRAME_DETAILS
#define BUILD_DIAG_FRAME_DETAILS 0
#endif
#ifndef BUILD_DIAG_LIVE_NETWORK_DETAILS
#define BUILD_DIAG_LIVE_NETWORK_DETAILS 0
#endif
#ifndef BUILD_DIAG_TLS_DETAILS
#define BUILD_DIAG_TLS_DETAILS BUILD_DIAG_LIVE_NETWORK_DETAILS
#endif
#ifndef BUILD_DIAG_YOUTUBE_RESOLVE
#define BUILD_DIAG_YOUTUBE_RESOLVE BUILD_DIAG_LIVE_NETWORK_DETAILS
#endif
