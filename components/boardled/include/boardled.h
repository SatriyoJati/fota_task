#ifndef BOARDLED_H
#define BOARDLED_H
#include "driver/gpio.h"

void setup_pin();

void turn_on();

void turn_off();

void toggle_led();

void toggle_fast();
#endif /* BOARDLEAD*/