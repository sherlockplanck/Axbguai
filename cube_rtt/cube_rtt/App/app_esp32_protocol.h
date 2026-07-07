#ifndef __APP_ESP32_PROTOCOL_H
#define __APP_ESP32_PROTOCOL_H

#include <rtthread.h>
#include "main.h"

#define APP_ESP32_CONTACT_MAX       2
#define APP_ESP32_CONTACT_SIZE      24

typedef struct
{
    char phone[APP_ESP32_CONTACT_SIZE];
    int priority;
    uint8_t valid;
} AppEsp32Contact_t;

typedef struct
{
    uint8_t call_cmd;
    char text_msg[64];
    char emergency_contact[APP_ESP32_CONTACT_SIZE];
    uint8_t emergency_contact_valid;
    AppEsp32Contact_t contacts[APP_ESP32_CONTACT_MAX];
    uint8_t contact_update_mask;
} AppEsp32Cmd_t;

typedef struct
{
    double latitude;
    double longitude;
    uint8_t gps_fix;
    rt_tick_t gps_last_tick;
    uint8_t fall_alarm;
} AppEsp32Data_t;

int app_esp32_protocol_init(UART_HandleTypeDef *huart);
void app_esp32_protocol_rx_thread_entry(void *parameter);
void app_esp32_protocol_send_data(const AppEsp32Data_t *data);
void app_esp32_protocol_get_cmd(AppEsp32Cmd_t *cmd);
void app_esp32_protocol_take_cmd(AppEsp32Cmd_t *cmd);

#endif
