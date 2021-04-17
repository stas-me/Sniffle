/*
 * Written by Sultan Qasim Khan
 * Copyright (c) 2018-2019, NCC Group plc
 * Released as open source under GPLv3
 */

#include <stdio.h>

#include "debug.h"
#include "PacketTask.h"

void dprintf(const char *fmt, ...)
{
    BLE_Frame frame;
    char buf[128];
    va_list args;

    frame.timestamp = 0;
    frame.rssi = 0;
    frame.channel = 40; // indicates debug message
    frame.phy = PHY_1M;
    frame.direction = 0;
    frame.pData = (uint8_t *)buf;

    va_start (args, fmt);
    frame.length = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Does thread safe copying into queue
    indicatePacket(&frame);
}


char* convertIntegerToChar(uint64_t N)
{
    if (N == 0) {
        return "0";
    }
    uint64_t m = N;
    uint16_t digit = 0;
    while (m) {
        digit++;
        m /= 10;
    }

    static char convertedInteger[15];

    char arr1[digit];

    uint16_t index = 0;
    while (N) {
        arr1[++index] = N % 10 + '0';
        N /= 10;
    }

    uint16_t i;
    for (i = 0; i < 15; i++) {
        convertedInteger[i] = 0;
    }
    for (i = 0; i < index; i++) {
        convertedInteger[i] = arr1[index - i];
    }

    convertedInteger[i] = '\0';

    return (char*)convertedInteger;
}