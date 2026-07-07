#include "app_fall_detector.h"

#define FALL_BASELINE_SAMPLE_COUNT      20
#define FALL_CONFIRM_SAMPLE_COUNT       3
#define FALL_STARTUP_SETTLE_MS          2000

#define FALL_DELTA_ROLL_THRESHOLD       90.0f
#define FALL_COMBINED_ROLL_THRESHOLD    55.0f
#define FALL_COMBINED_PITCH_THRESHOLD   35.0f
#define FALL_ABS_SIDE_ROLL_THRESHOLD    105.0f
#define FALL_ABS_FLAT_PITCH_MAX         35.0f
#define FALL_DELTA_FLAT_PITCH_MAX       45.0f
#define FALL_FORWARD_ROLL_DELTA_MIN     10.0f
#define FALL_FORWARD_PITCH_DELTA_MIN    6.0f
#define FALL_FORWARD_PITCH_MAX          70.0f

typedef struct
{
    float baseline_roll;
    float baseline_pitch;
    float roll_sum;
    float pitch_sum;
    uint8_t baseline_count;
    uint8_t calibrated;
    uint8_t confirm_count;
    rt_tick_t calibrated_tick;
} FallDetectorState_t;

static FallDetectorState_t s_fall_state = {0};

static float abs_float(float value)
{
    return (value < 0.0f) ? -value : value;
}

static float normalize_angle(float angle)
{
    while (angle > 180.0f)
    {
        angle -= 360.0f;
    }

    while (angle <= -180.0f)
    {
        angle += 360.0f;
    }

    return angle;
}

void app_fall_detector_init(void)
{
    s_fall_state.baseline_roll = 0.0f;
    s_fall_state.baseline_pitch = 0.0f;
    s_fall_state.roll_sum = 0.0f;
    s_fall_state.pitch_sum = 0.0f;
    s_fall_state.baseline_count = 0;
    s_fall_state.calibrated = 0;
    s_fall_state.confirm_count = 0;
    s_fall_state.calibrated_tick = 0;
}

void app_fall_detector_update(float raw_roll,
                              float raw_pitch,
                              float raw_yaw,
                              rt_tick_t now_tick,
                              AppFallDetectorResult_t *result)
{
    float roll = normalize_angle(raw_roll);
    float pitch = normalize_angle(raw_pitch);
    float yaw = normalize_angle(raw_yaw);
    float delta_roll = 0.0f;
    float delta_pitch = 0.0f;
    uint8_t posture_trigger = 0;

    if (s_fall_state.calibrated)
    {
        delta_roll = normalize_angle(roll - s_fall_state.baseline_roll);
        delta_pitch = normalize_angle(pitch - s_fall_state.baseline_pitch);
    }

    if (result != RT_NULL)
    {
        result->roll = roll;
        result->pitch = pitch;
        result->yaw = yaw;
        result->delta_roll = delta_roll;
        result->delta_pitch = delta_pitch;
        result->calibrated = s_fall_state.calibrated;
        result->posture_trigger = 0;
        result->confirm_count = s_fall_state.confirm_count;
        result->fall_alarm = 0;
    }

    if (!s_fall_state.calibrated)
    {
        s_fall_state.roll_sum += roll;
        s_fall_state.pitch_sum += pitch;
        s_fall_state.baseline_count++;

        if (s_fall_state.baseline_count >= FALL_BASELINE_SAMPLE_COUNT)
        {
            s_fall_state.baseline_roll = s_fall_state.roll_sum / s_fall_state.baseline_count;
            s_fall_state.baseline_pitch = s_fall_state.pitch_sum / s_fall_state.baseline_count;
            s_fall_state.calibrated = 1;
            s_fall_state.calibrated_tick = now_tick;
        }

        if (result != RT_NULL)
        {
            result->calibrated = s_fall_state.calibrated;
            result->delta_roll = 0.0f;
            result->delta_pitch = 0.0f;
            result->posture_trigger = 0;
            result->confirm_count = 0;
            result->fall_alarm = 0;
        }
        return;
    }

    if ((now_tick - s_fall_state.calibrated_tick) < rt_tick_from_millisecond(FALL_STARTUP_SETTLE_MS))
    {
        s_fall_state.confirm_count = 0;

        if (result != RT_NULL)
        {
            result->calibrated = s_fall_state.calibrated;
            result->delta_roll = delta_roll;
            result->delta_pitch = delta_pitch;
            result->posture_trigger = 0;
            result->confirm_count = 0;
            result->fall_alarm = 0;
        }
        return;
    }

    if ((abs_float(roll) >= FALL_ABS_SIDE_ROLL_THRESHOLD &&
         abs_float(pitch) <= FALL_ABS_FLAT_PITCH_MAX) ||
        (abs_float(delta_roll) >= FALL_DELTA_ROLL_THRESHOLD &&
         abs_float(pitch) <= FALL_DELTA_FLAT_PITCH_MAX) ||
        (abs_float(delta_roll) >= FALL_COMBINED_ROLL_THRESHOLD &&
         abs_float(delta_pitch) >= FALL_COMBINED_PITCH_THRESHOLD &&
         abs_float(pitch) <= FALL_DELTA_FLAT_PITCH_MAX) ||
        (delta_roll >= FALL_FORWARD_ROLL_DELTA_MIN &&
         delta_pitch <= -FALL_FORWARD_PITCH_DELTA_MIN &&
         pitch <= FALL_FORWARD_PITCH_MAX))
    {
        posture_trigger = 1;
    }

    if (posture_trigger)
    {
        if (s_fall_state.confirm_count < FALL_CONFIRM_SAMPLE_COUNT)
        {
            s_fall_state.confirm_count++;
        }
    }
    else
    {
        s_fall_state.confirm_count = 0;
    }

    if (s_fall_state.confirm_count >= FALL_CONFIRM_SAMPLE_COUNT)
    {
        result->calibrated = s_fall_state.calibrated;
        result->delta_roll = delta_roll;
        result->delta_pitch = delta_pitch;
        result->posture_trigger = posture_trigger;
        result->confirm_count = s_fall_state.confirm_count;
        result->fall_alarm = 1;
    }
    else if (result != RT_NULL)
    {
        result->calibrated = s_fall_state.calibrated;
        result->delta_roll = delta_roll;
        result->delta_pitch = delta_pitch;
        result->posture_trigger = posture_trigger;
        result->confirm_count = s_fall_state.confirm_count;
        result->fall_alarm = 0;
    }
}
