#ifndef APP_EMERGENCY_H_
#define APP_EMERGENCY_H_

#include <rtthread.h>
#include <stdint.h>

#define APP_EMERGENCY_CONTACT_SIZE  24
#define APP_EMERGENCY_CONTACT_MAX   2
#define APP_EMERGENCY_SMS_SIZE      256

typedef struct
{
    char phone[APP_EMERGENCY_CONTACT_SIZE];
    int priority;
    uint8_t valid;
} AppEmergencyContact_t;

typedef struct
{
    char phone[APP_EMERGENCY_CONTACT_SIZE];
} AppEmergencyCall_t;

typedef struct
{
    char phone[APP_EMERGENCY_CONTACT_SIZE];
    char message[APP_EMERGENCY_SMS_SIZE];
} AppEmergencySms_t;

int app_emergency_init(void);
int app_emergency_set_contact(uint8_t index, const char *phone, int priority);
uint8_t app_emergency_has_contact(void);
uint8_t app_emergency_get_contact_count(void);
int app_emergency_update_fall(uint8_t fall_alarm);
int app_emergency_queue_fall_call(void);
int app_emergency_queue_contact_call(uint8_t index);
int app_emergency_take_call(AppEmergencyCall_t *call);
int app_emergency_queue_all_sms(const char *message);
int app_emergency_take_sms(AppEmergencySms_t *sms);

#endif
