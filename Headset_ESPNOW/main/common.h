#ifndef COMMON_H
#define COMMON_H

#include "stdint.h"

// Define a simple data structure for the packet
typedef struct {
    uint32_t count;
    uint8_t  payload[1344]; // 7ms audio 48kHz Stereo 16 Bit
} audio_packet_t;


#endif
