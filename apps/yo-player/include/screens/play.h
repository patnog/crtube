#pragma once

// play screen - resolves a test video and plays it full-screen. Pushed on top of
// the debug screen (Start), popped with Circle.

#include "screen.h"

extern Screen playScreen;

// resolves and plays `input` (a youtube link or 11-char video id): stores it and
// pushes the play screen on top of the current one.
void playVideo(const char *input);
void playLocalVideo(const char *path);
// Starts a downloaded video with a snapshot of the Downloads grid order (16-byte IDs).
void playLocalVideoWithQueue(const char *path, const char videoIds[][16], int count, int index);
