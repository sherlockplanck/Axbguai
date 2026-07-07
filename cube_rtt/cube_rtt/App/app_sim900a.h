#ifndef __APP_SIM900A_H
#define __APP_SIM900A_H

#include <rtthread.h>
#include "main.h"

int app_sim900a_init(UART_HandleTypeDef *huart);
int app_sim900a_send_cmd(const char *cmd);
int app_sim900a_send_at(void);
int app_sim900a_wait_ready(uint8_t retry_count, uint32_t retry_interval_ms);
int app_sim900a_call(const char *phone);
int app_sim900a_hangup(void);
int app_sim900a_send_sms(const char *phone, const char *message);
int app_sim900a_send_sms_ucs2_hex(const char *phone, const char *ucs2_hex_message);

#endif
