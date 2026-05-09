#ifndef _Ft6336U_DRIVER_H
#define _Ft6336U_DRIVER_H

#include "driver/gpio.h"

esp_err_t ft6336u_driver(gpio_num_t sda, gpio_num_t scl);
void ft6336u_read(int16_t *x, int16_t *y,int *state);
void ft6336u_int_info(bool flag);

#endif
