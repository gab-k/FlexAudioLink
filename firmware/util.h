#ifndef UTIL_H_
#define UTIL_H_
#include "FreeRTOS.h"
#include "pin_mux.h"
#include "audio.h"

void led_task(void *pvParameters);
void get_unique_id(uint8_t id[], uint32_t * len);

#endif /* UTIL_H_ */