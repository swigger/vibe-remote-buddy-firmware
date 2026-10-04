#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void buddy_led_init(void);
uint16_t buddy_led_command(const cJSON *request, cJSON *response,
                           bool allow_save);
#ifdef __cplusplus
}
#endif
