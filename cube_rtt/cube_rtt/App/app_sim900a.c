#include "app_sim900a.h"
#include <string.h>

#define SIM900A_AT_TIMEOUT_MS   5000
#define SIM900A_SMS_TIMEOUT_MS  60000
#define SIM900A_RX_BUF_SIZE     128
#define SIM900A_CMD_BUF_SIZE    96
#define SIM900A_PHONE_UCS2_BUF_SIZE 80
#define SIM900A_SMS_CTRL_Z      0x1A
#define SIM900A_WAIT_RESPONSE_ENABLE 0

static UART_HandleTypeDef *s_sim900a_uart = RT_NULL;

static int sim900a_bytes_contains(const uint8_t *buf, rt_size_t len, const char *pattern)
{
    rt_size_t pattern_len = rt_strlen(pattern);
    rt_size_t i;
    rt_size_t j;

    if (pattern_len == 0 || len < pattern_len)
    {
        return 0;
    }

    for (i = 0; i <= (len - pattern_len); i++)
    {
        for (j = 0; j < pattern_len; j++)
        {
            if (buf[i + j] != (uint8_t)pattern[j])
            {
                break;
            }
        }

        if (j == pattern_len)
        {
            return 1;
        }
    }

    return 0;
}

static char sim900a_printable_char(uint8_t byte)
{
    if (byte >= 0x20 && byte <= 0x7E)
    {
        return (char)byte;
    }

    return '.';
}

static void sim900a_clear_error_flags(void)
{
    if (__HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(s_sim900a_uart);
    }
    if (__HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_FE) != RESET)
    {
        __HAL_UART_CLEAR_FEFLAG(s_sim900a_uart);
    }
    if (__HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_NE) != RESET)
    {
        __HAL_UART_CLEAR_NEFLAG(s_sim900a_uart);
    }
    if (__HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_PE) != RESET)
    {
        __HAL_UART_CLEAR_PEFLAG(s_sim900a_uart);
    }

    s_sim900a_uart->ErrorCode = HAL_UART_ERROR_NONE;
    s_sim900a_uart->RxState = HAL_UART_STATE_READY;
}

static int sim900a_read_byte(uint8_t *byte, uint32_t timeout_ms)
{
    rt_tick_t start_tick = rt_tick_get();
    rt_tick_t timeout_tick = rt_tick_from_millisecond(timeout_ms);

    while ((rt_tick_get() - start_tick) < timeout_tick)
    {
        if (__HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_RXNE) != RESET)
        {
            *byte = (uint8_t)(s_sim900a_uart->Instance->DR & 0xFF);
            return 0;
        }

        if (__HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_ORE) != RESET ||
            __HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_FE) != RESET ||
            __HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_NE) != RESET ||
            __HAL_UART_GET_FLAG(s_sim900a_uart, UART_FLAG_PE) != RESET)
        {
            sim900a_clear_error_flags();
        }

        /* Do not sleep here: at 115200 bps, one byte is only about 87 us. */
    }

    return -1;
}

static void sim900a_flush_rx(void)
{
    uint8_t rx_byte;

    if (s_sim900a_uart == RT_NULL)
    {
        return;
    }

    sim900a_clear_error_flags();

    while (sim900a_read_byte(&rx_byte, 1) == 0)
    {
    }
}

static int sim900a_write_bytes(const uint8_t *data, rt_size_t len)
{
    if (s_sim900a_uart == RT_NULL || data == RT_NULL || len == 0)
    {
        return -1;
    }

    return (HAL_UART_Transmit(s_sim900a_uart, (uint8_t *)data, len, 1000) == HAL_OK) ? 0 : -1;
}

static int sim900a_write_text(const char *text)
{
    if (text == RT_NULL)
    {
        return -1;
    }

    return sim900a_write_bytes((const uint8_t *)text, rt_strlen(text));
}

static int sim900a_phone_to_ucs2_hex(const char *phone, char *buf, rt_size_t size)
{
    rt_size_t i = 0;
    rt_size_t out = 0;
    char ch;

    if (phone == RT_NULL || buf == RT_NULL || size == 0)
    {
        return -1;
    }

    while (phone[i] != '\0')
    {
        ch = phone[i++];
        if ((ch < '0' || ch > '9') && ch != '+')
        {
            continue;
        }

        if ((out + 4) >= size)
        {
            return -1;
        }

        rt_snprintf(&buf[out], size - out, "%04X", (unsigned int)(uint8_t)ch);
        out += 4;
    }

    buf[out] = '\0';
    return (out > 0) ? 0 : -1;
}

int app_sim900a_init(UART_HandleTypeDef *huart)
{
    s_sim900a_uart = huart;
    return (s_sim900a_uart == RT_NULL) ? -1 : 0;
}

int app_sim900a_send_cmd(const char *cmd)
{
    rt_size_t len;

    if (s_sim900a_uart == RT_NULL)
    {
        rt_kprintf("[SIM900A] uart not init\n");
        return -1;
    }
    if (cmd == RT_NULL)
    {
        return -1;
    }

    len = rt_strlen(cmd);
    sim900a_flush_rx();

    if (sim900a_write_bytes((const uint8_t *)cmd, len) == 0)
    {
        return 0;
    }

    return -1;
}

int app_sim900a_send_at(void)
{
    const char at_cmd[] = "AT\r\n";
    uint8_t rx_byte = 0;
    uint8_t rx_buf[SIM900A_RX_BUF_SIZE] = {0};
    char rx_text[SIM900A_RX_BUF_SIZE] = {0};
    rt_size_t rx_len = 0;
    rt_tick_t start_tick;

    if (s_sim900a_uart == RT_NULL)
    {
        rt_kprintf("[SIM900A] uart not init\n");
        return -1;
    }

    app_sim900a_send_cmd(at_cmd);

    start_tick = rt_tick_get();
    while ((rt_tick_get() - start_tick) < rt_tick_from_millisecond(SIM900A_AT_TIMEOUT_MS))
    {
        if (sim900a_read_byte(&rx_byte, 20) == 0)
        {
            if (rx_len < (sizeof(rx_buf) - 1))
            {
                rx_buf[rx_len] = rx_byte;
                rx_text[rx_len] = sim900a_printable_char(rx_byte);
                rx_len++;
                rx_text[rx_len] = '\0';
            }

            if (sim900a_bytes_contains(rx_buf, rx_len, "OK"))
            {
                rt_kprintf("[SIM900A] AT OK\n");
                return 0;
            }

            if (sim900a_bytes_contains(rx_buf, rx_len, "ERROR"))
            {
                rt_kprintf("[SIM900A] AT ERROR\n");
                return -1;
            }
        }
    }

    rt_kprintf("[SIM900A] AT timeout, len=%d, text=%s\n", (int)rx_len, rx_text);
    return -1;
}

int app_sim900a_wait_ready(uint8_t retry_count, uint32_t retry_interval_ms)
{
    uint8_t retry = 0;

    if (s_sim900a_uart == RT_NULL)
    {
        rt_kprintf("[SIM900A] uart not init\n");
        return -1;
    }

    while (retry_count == 0 || retry < retry_count)
    {
        retry++;
        if (app_sim900a_send_at() == 0)
        {
            rt_kprintf("[SIM900A] ready\n");
            return 0;
        }

        rt_thread_mdelay(retry_interval_ms);
    }

    rt_kprintf("[SIM900A] ready check failed\n");
    return -1;
}

int app_sim900a_call(const char *phone)
{
    char cmd[SIM900A_CMD_BUF_SIZE];

    if (phone == RT_NULL || phone[0] == '\0')
    {
        return -1;
    }

    rt_snprintf(cmd, sizeof(cmd), "ATD%s;\r\n", phone);
    if (app_sim900a_send_cmd(cmd) != 0)
    {
        return -1;
    }

    rt_kprintf("[SIM900A] call %s\n", phone);
    return 0;
}

int app_sim900a_hangup(void)
{
    if (app_sim900a_send_cmd("ATH\r\n") != 0)
    {
        rt_kprintf("[SIM900A] hangup failed\n");
        return -1;
    }

    rt_kprintf("[SIM900A] hangup\n");
    return 0;
}

int app_sim900a_send_sms(const char *phone, const char *message)
{
    char cmd[SIM900A_CMD_BUF_SIZE];
    const uint8_t ctrl_z = SIM900A_SMS_CTRL_Z;

    if (phone == RT_NULL || message == RT_NULL || phone[0] == '\0')
    {
        return -1;
    }

#if !SIM900A_WAIT_RESPONSE_ENABLE
    app_sim900a_send_cmd("AT\r\n");
    rt_thread_mdelay(500);

    app_sim900a_send_cmd("ATE0\r\n");
    rt_thread_mdelay(500);

    app_sim900a_send_cmd("AT+CMGF=1\r\n");
    rt_thread_mdelay(1000);

    app_sim900a_send_cmd("AT+CSCS=\"GSM\"\r\n");
    rt_thread_mdelay(500);

    rt_snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", phone);
    if (app_sim900a_send_cmd(cmd) != 0)
    {
        return -1;
    }
    rt_thread_mdelay(2000);

    if (sim900a_write_text(message) != 0)
    {
        return -1;
    }
    if (sim900a_write_bytes(&ctrl_z, 1) != 0)
    {
        return -1;
    }

    rt_kprintf("[SIM900A] sms command sent to %s\n", phone);
    return 0;
#else
    rt_snprintf(cmd, sizeof(cmd), "AT+CMGF=1\r\n");
    if (app_sim900a_send_cmd(cmd) != 0)
    {
        return -1;
    }
    if (sim900a_wait_for("OK", "ERROR", SIM900A_AT_TIMEOUT_MS, "CMGF") != 0)
    {
        rt_kprintf("[SIM900A] sms text mode failed\n");
        return -1;
    }

    rt_snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", phone);
    if (app_sim900a_send_cmd(cmd) != 0)
    {
        return -1;
    }
    if (sim900a_wait_for(">", "ERROR", SIM900A_AT_TIMEOUT_MS, "CMGS prompt") != 0)
    {
        rt_kprintf("[SIM900A] sms prompt timeout\n");
        return -1;
    }

    if (sim900a_write_text(message) != 0)
    {
        return -1;
    }
    if (sim900a_write_bytes(&ctrl_z, 1) != 0)
    {
        return -1;
    }
    if (sim900a_wait_for("OK", "ERROR", SIM900A_SMS_TIMEOUT_MS, "SMS send") != 0)
    {
        rt_kprintf("[SIM900A] sms send timeout\n");
        return -1;
    }

    rt_kprintf("[SIM900A] sms sent to %s\n", phone);
    return 0;
#endif
}

int app_sim900a_send_sms_ucs2_hex(const char *phone, const char *ucs2_hex_message)
{
    char cmd[SIM900A_CMD_BUF_SIZE];
    char phone_ucs2[SIM900A_PHONE_UCS2_BUF_SIZE];
    const uint8_t ctrl_z = SIM900A_SMS_CTRL_Z;

    if (phone == RT_NULL ||
        ucs2_hex_message == RT_NULL ||
        phone[0] == '\0' ||
        ucs2_hex_message[0] == '\0')
    {
        return -1;
    }

    if (sim900a_phone_to_ucs2_hex(phone, phone_ucs2, sizeof(phone_ucs2)) != 0)
    {
        return -1;
    }

    app_sim900a_send_cmd("AT\r\n");
    rt_thread_mdelay(500);

    app_sim900a_send_cmd("ATE0\r\n");
    rt_thread_mdelay(500);

    app_sim900a_send_cmd("AT+CMGF=1\r\n");
    rt_thread_mdelay(1000);

    app_sim900a_send_cmd("AT+CSCS=\"UCS2\"\r\n");
    rt_thread_mdelay(500);

    app_sim900a_send_cmd("AT+CSMP=17,167,0,8\r\n");
    rt_thread_mdelay(500);

    rt_snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", phone_ucs2);
    if (app_sim900a_send_cmd(cmd) != 0)
    {
        return -1;
    }
    rt_thread_mdelay(2000);

    if (sim900a_write_text(ucs2_hex_message) != 0)
    {
        return -1;
    }
    if (sim900a_write_bytes(&ctrl_z, 1) != 0)
    {
        return -1;
    }

    rt_kprintf("[SIM900A] ucs2 sms command sent to %s\n", phone);
    return 0;
}
