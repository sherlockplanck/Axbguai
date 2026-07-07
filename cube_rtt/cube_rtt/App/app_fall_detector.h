#ifndef __APP_FALL_DETECTOR_H
#define __APP_FALL_DETECTOR_H

#include <rtthread.h>
#include "main.h"

typedef struct
{
    float roll;
    float pitch;
    float yaw;
    float delta_roll;
    float delta_pitch;
    uint8_t calibrated;
    uint8_t posture_trigger;
    uint8_t confirm_count;
    uint8_t fall_alarm;
} AppFallDetectorResult_t;

void app_fall_detector_init(void);
void app_fall_detector_update(float raw_roll,
                              float raw_pitch,
                              float raw_yaw,
                              rt_tick_t now_tick,
                              AppFallDetectorResult_t *result);

#endif
