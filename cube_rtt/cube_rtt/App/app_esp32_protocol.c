#include "app_esp32_protocol.h"
#include <string.h>

#define ESP32_RX_LINE_SIZE      192
#define ESP32_RX_TIMEOUT_MS     10
#define GPS_SIGNAL_TIMEOUT_MS   6000

static UART_HandleTypeDef *s_esp32_uart = RT_NULL;
static AppEsp32Cmd_t s_last_cmd = {0};
static rt_mutex_t s_cmd_mutex = RT_NULL;
static rt_mutex_t s_tx_mutex = RT_NULL;

static void parse_esp32_cmd(const char *line);

static int abs_int(int value)
{
    return (value < 0) ? -value : value;
}

static void format_coordinate(char *buf, rt_size_t size, double value)
{
    int scaled = (int)(value * 1000000.0);
    int integer = scaled / 1000000;
    int decimal = abs_int(scaled % 1000000);

    rt_snprintf(buf, size, "%d.%06d", integer, decimal);
}

static void esp32_send_text(const char *text)
{
    if (text == RT_NULL || s_esp32_uart == RT_NULL || s_tx_mutex == RT_NULL)
    {
        return;
    }

    if (rt_mutex_take(s_tx_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        HAL_UART_Transmit(s_esp32_uart, (uint8_t *)text, rt_strlen(text), 100);
        rt_mutex_release(s_tx_mutex);
    }
}

static void esp32_send_contact_received_ack(void)
{
    esp32_send_text("ESP32_ACK,cmd=contact_update,received=1\r\n");
}

static void esp32_send_call_received_ack(void)
{
    esp32_send_text("ESP32_ACK,cmd=call,received=1\r\n");
}

static void esp32_send_serial_received_ack(void)
{
    esp32_send_text("ESP32_ACK,cmd=serial,received=1\r\n");
}

static void esp32_send_command_ack(const char *line)
{
    if (line == RT_NULL)
    {
        return;
    }

    if (strstr(line, "contact1=") != RT_NULL ||
        strstr(line, "contact2=") != RT_NULL ||
        strstr(line, "emergency_contact=") != RT_NULL)
    {
        esp32_send_contact_received_ack();
    }

    if (strstr(line, "text_msg=start") != RT_NULL ||
        strstr(line, "text_msg=stop") != RT_NULL)
    {
        esp32_send_serial_received_ack();
        return;
    }

    if (strstr(line, "call_cmd=1") != RT_NULL ||
        strstr(line, "call_cmd=2") != RT_NULL)
    {
        esp32_send_call_received_ack();
    }
}

static void esp32_process_line(char *line, rt_size_t *line_len)
{
    if (line == RT_NULL || line_len == RT_NULL)
    {
        return;
    }

    line[*line_len] = '\0';
    if (strncmp(line, "ESP32_CMD", 9) == 0)
    {
        esp32_send_command_ack(line);
    }
    parse_esp32_cmd(line);
    memset(line, 0, ESP32_RX_LINE_SIZE);
    *line_len = 0;
}

static int parse_int_field(const char *line, const char *key, int default_value)
{
    const char *ptr = strstr(line, key);
    int value = 0;
    int sign = 1;

    if (ptr == RT_NULL)
    {
        return default_value;
    }

    ptr += rt_strlen(key);
    if (*ptr == '-')
    {
        sign = -1;
        ptr++;
    }

    while (*ptr >= '0' && *ptr <= '9')
    {
        value = value * 10 + (*ptr - '0');
        ptr++;
    }

    return value * sign;
}

static int normalize_priority(int priority)
{
    return (priority <= 0) ? 0 : 1;
}

static uint8_t parse_string_field(const char *line, const char *key, char *buf, rt_size_t size)
{
    const char *ptr = strstr(line, key);
    rt_size_t len = 0;

    if (ptr == RT_NULL || buf == RT_NULL || size == 0)
    {
        return 0;
    }

    ptr += rt_strlen(key);
    while (ptr[len] != '\0' &&
           ptr[len] != ',' &&
           ptr[len] != '\\' &&
           ptr[len] != '\r' &&
           ptr[len] != '\n' &&
           len < (size - 1))
    {
        len++;
    }

    memcpy(buf, ptr, len);
    buf[len] = '\0';
    return (len > 0) ? 1 : 0;
}

static uint8_t phone_is_valid(const char *phone)
{
    rt_size_t i = 0;
    rt_size_t digit_count = 0;

    if (phone == RT_NULL || phone[0] == '\0')
    {
        return 0;
    }

    while (phone[i] != '\0')
    {
        if (phone[i] >= '0' && phone[i] <= '9')
        {
            digit_count++;
        }
        else if (!(phone[i] == '+' && i == 0))
        {
            return 0;
        }
        i++;
    }

    return (digit_count >= 5 && digit_count < APP_ESP32_CONTACT_SIZE) ? 1 : 0;
}

static uint8_t parse_contact_field(const char *line, int index, AppEsp32Contact_t *contact)
{
    char phone_key[16];
    char priority_key[24];

    rt_snprintf(phone_key, sizeof(phone_key), "contact%d=", index);
    rt_snprintf(priority_key, sizeof(priority_key), "contact%d_priority=", index);

    if (!parse_string_field(line, phone_key, contact->phone, sizeof(contact->phone)))
    {
        return 0;
    }
    if (!phone_is_valid(contact->phone))
    {
        rt_kprintf("[ESP32] contact%d rejected: invalid phone=%s\n", index, contact->phone);
        return 0;
    }

    contact->priority = normalize_priority(parse_int_field(line, priority_key, index - 1));
    contact->valid = 1;
    return 1;
}

static uint8_t is_serial_control_text(const char *text)
{
    if (text == RT_NULL)
    {
        return 0;
    }

    return (strcmp(text, "start") == 0 || strcmp(text, "stop") == 0) ? 1 : 0;
}

static void parse_esp32_cmd(const char *line)
{
    const char *call_ptr = RT_NULL;
    AppEsp32Cmd_t cmd = {0};
    uint8_t i;
    uint8_t serial_control = 0;

    if (strncmp(line, "ESP32_CMD", 9) != 0)
    {
        return;
    }

    call_ptr = strstr(line, "call_cmd=");
    if (call_ptr != RT_NULL)
    {
        cmd.call_cmd = (uint8_t)parse_int_field(line, "call_cmd=", 0);
        if (cmd.call_cmd > APP_ESP32_CONTACT_MAX)
        {
            cmd.call_cmd = 0;
        }
    }

    parse_string_field(line, "text_msg=", cmd.text_msg, sizeof(cmd.text_msg));
    serial_control = is_serial_control_text(cmd.text_msg);

    if (cmd.call_cmd >= 1 &&
        cmd.call_cmd <= APP_ESP32_CONTACT_MAX &&
        !serial_control)
    {
        rt_kprintf("[ESP32] call contact%d cmd received\n", cmd.call_cmd);
    }

    if (parse_string_field(line,
                           "emergency_contact=",
                           cmd.emergency_contact,
                           sizeof(cmd.emergency_contact)) &&
        phone_is_valid(cmd.emergency_contact))
    {
        cmd.emergency_contact_valid = 1;
        memcpy(cmd.contacts[0].phone,
               cmd.emergency_contact,
               sizeof(cmd.contacts[0].phone));
        cmd.contacts[0].priority = normalize_priority(parse_int_field(line, "priority=", 0));
        cmd.contacts[0].valid = 1;
        cmd.contact_update_mask |= 0x01;
    }

    for (i = 0; i < APP_ESP32_CONTACT_MAX; i++)
    {
        if (parse_contact_field(line, (int)i + 1, &cmd.contacts[i]))
        {
            cmd.contact_update_mask |= (uint8_t)(1U << i);
        }
    }

    if (cmd.contact_update_mask != 0)
    {
        rt_kprintf("[ESP32] contact update cmd received\n");
    }

    if (rt_mutex_take(s_cmd_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        if (serial_control)
        {
            memcpy(s_last_cmd.text_msg, cmd.text_msg, sizeof(s_last_cmd.text_msg));
        }
        else if (cmd.call_cmd > 0)
        {
            s_last_cmd.call_cmd = cmd.call_cmd;
        }
        else if (cmd.text_msg[0] != '\0')
        {
            memcpy(s_last_cmd.text_msg, cmd.text_msg, sizeof(s_last_cmd.text_msg));
        }
        if (cmd.emergency_contact_valid)
        {
            memcpy(s_last_cmd.emergency_contact,
                   cmd.emergency_contact,
                   sizeof(s_last_cmd.emergency_contact));
            s_last_cmd.emergency_contact_valid = 1;
        }
        for (i = 0; i < APP_ESP32_CONTACT_MAX; i++)
        {
            if (cmd.contact_update_mask & (1U << i))
            {
                s_last_cmd.contacts[i] = cmd.contacts[i];
                s_last_cmd.contact_update_mask |= (uint8_t)(1U << i);
            }
        }
        rt_mutex_release(s_cmd_mutex);
    }

    for (i = 0; i < APP_ESP32_CONTACT_MAX; i++)
    {
        if (cmd.contact_update_mask & (1U << i))
        {
            rt_kprintf("[ESP32] contact%d=%s,priority=%d\n",
                       (int)i + 1,
                       cmd.contacts[i].phone,
                       cmd.contacts[i].priority);
        }
    }

}

int app_esp32_protocol_init(UART_HandleTypeDef *huart)
{
    s_esp32_uart = huart;

    if (s_cmd_mutex == RT_NULL)
    {
        s_cmd_mutex = rt_mutex_create("esp32_cmd", RT_IPC_FLAG_PRIO);
    }
    if (s_tx_mutex == RT_NULL)
    {
        s_tx_mutex = rt_mutex_create("esp32_tx", RT_IPC_FLAG_PRIO);
    }

    return (s_cmd_mutex == RT_NULL || s_tx_mutex == RT_NULL || s_esp32_uart == RT_NULL) ? -1 : 0;
}

void app_esp32_protocol_rx_thread_entry(void *parameter)
{
    uint8_t rx_byte = 0;
    char line[ESP32_RX_LINE_SIZE];
    rt_size_t line_len = 0;
    uint8_t escape_pending = 0;

    (void)parameter;
    memset(line, 0, sizeof(line));

    while (1)
    {
        if (HAL_UART_Receive(s_esp32_uart, &rx_byte, 1, ESP32_RX_TIMEOUT_MS) == HAL_OK)
        {
            if (escape_pending)
            {
                if (rx_byte == 'n')
                {
                    esp32_process_line(line, &line_len);
                }
                else if (rx_byte != 'r')
                {
                    if (line_len < (sizeof(line) - 1))
                    {
                        line[line_len++] = '\\';
                    }
                    if (line_len < (sizeof(line) - 1))
                    {
                        line[line_len++] = (char)rx_byte;
                    }
                }
                escape_pending = 0;
            }
            else if (rx_byte == '\n')
            {
                esp32_process_line(line, &line_len);
            }
            else if (rx_byte != '\r')
            {
                if (rx_byte == '\\')
                {
                    escape_pending = 1;
                }
                else if (line_len < (sizeof(line) - 1))
                {
                    line[line_len++] = (char)rx_byte;
                }
                else
                {
                    memset(line, 0, sizeof(line));
                    line_len = 0;
                }
            }
        }
        else
        {
            rt_thread_mdelay(5);
        }
    }
}

void app_esp32_protocol_send_data(const AppEsp32Data_t *data)
{
    char tx_buf[192];
    char lat_buf[24];
    char lon_buf[24];
    rt_tick_t now_tick = rt_tick_get();

    if (data == RT_NULL || s_esp32_uart == RT_NULL || s_tx_mutex == RT_NULL)
    {
        return;
    }

    if (rt_mutex_take(s_tx_mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }

    if (data->gps_fix &&
        ((now_tick - data->gps_last_tick) <= rt_tick_from_millisecond(GPS_SIGNAL_TIMEOUT_MS)))
    {
        format_coordinate(lat_buf, sizeof(lat_buf), data->latitude);
        format_coordinate(lon_buf, sizeof(lon_buf), data->longitude);
        rt_snprintf(tx_buf, sizeof(tx_buf),
                    "STM32_DATA,gps_signal=1,longitude=%s,latitude=%s,fall_alarm=%d\r\n",
                    lon_buf,
                    lat_buf,
                    data->fall_alarm);
    }
    else
    {
        rt_snprintf(tx_buf, sizeof(tx_buf),
                    "STM32_DATA,gps_signal=0,gps_status=no_signal,fall_alarm=%d\r\n",
                    data->fall_alarm);
    }

    HAL_UART_Transmit(s_esp32_uart, (uint8_t *)tx_buf, rt_strlen(tx_buf), 100);
    rt_mutex_release(s_tx_mutex);
}

void app_esp32_protocol_get_cmd(AppEsp32Cmd_t *cmd)
{
    if (cmd == RT_NULL || s_cmd_mutex == RT_NULL)
    {
        return;
    }

    if (rt_mutex_take(s_cmd_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        *cmd = s_last_cmd;
        rt_mutex_release(s_cmd_mutex);
    }
}

void app_esp32_protocol_take_cmd(AppEsp32Cmd_t *cmd)
{
    if (cmd == RT_NULL || s_cmd_mutex == RT_NULL)
    {
        return;
    }

    if (rt_mutex_take(s_cmd_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        *cmd = s_last_cmd;
        s_last_cmd.call_cmd = 0;
        s_last_cmd.text_msg[0] = '\0';
        s_last_cmd.emergency_contact_valid = 0;
        s_last_cmd.contact_update_mask = 0;
        rt_mutex_release(s_cmd_mutex);
    }
}
