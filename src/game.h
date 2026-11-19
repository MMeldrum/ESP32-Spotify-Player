#pragma once
#include "board.h"
#include <lvgl.h>

#if BOARD_HAS_GAME
// Groove Breaker, the hidden game: bounce a ball into a wall of bricks coloured from the album cover.
// art is the player's background (RGB565, artW x artH) or nullptr; accent colours the paddle. The game
// builds its own screen, and calls onExit when the player leaves it (back button).
void game_open(const lv_color_t *art, int artW, int artH, lv_color_t accent, void (*onExit)());
#endif
