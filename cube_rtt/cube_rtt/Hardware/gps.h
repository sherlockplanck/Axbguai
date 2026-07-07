#ifndef __GPS_H
#define __GPS_H

#include <stdint.h>

typedef struct {
    double latitude;   
    double longitude;  
    uint8_t fix_status;
    uint8_t sentence_parsed;
} GPS_Info_t;

void GPS_ParseData(uint8_t *nmea_buffer, GPS_Info_t *out_gps_data);

#endif
