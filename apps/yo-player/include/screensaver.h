#pragma once

// Global screensaver animation. This module does not manage activation or input;
// the main application loop owns those responsibilities.

void termScreenSaver(void);
void startScreenSaver(void);
void stopScreenSaver(void);
int isScreenSaverActive(void);
void updateScreenSaver(void);
void drawScreenSaver(void);
