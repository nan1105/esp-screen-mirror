#ifndef _AVI_PLAYER_H_
#define _AVI_PLAYER_H_

#include "esp_err.h"

esp_err_t avi_player_play(const char *filename);
esp_err_t avi_player_find_and_play(void);

#endif
