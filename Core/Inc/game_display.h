#ifndef GAME_DISPLAY_H
#define GAME_DISPLAY_H

#include "external_io.h"

/* Pure, deterministic renderer. Bytes use SSD1306 horizontal page layout:
 * buffer[x + 128 * (y / 8)], bit (y % 8). No hardware or persistent state. */
void GameDisplay_Render(uint8_t buffer[1024], const ExternalDisplayFrame *frame);

#endif
