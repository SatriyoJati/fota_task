#ifndef BOARDLED_H
#define BOARDLED_H
#include "driver/gpio.h"

void setup_pin();

void turn_on();

void turn_off();

void toggle_led();

void toggle_fast();

void normal_blink();

void ota_led_indicator_start();

void recovery_blink();
#endif /* BOARDLED_H */