#include <rtthread.h>
#include "usart.h"
#include "jy61p.h"
#include "gps.h"
#include "../App/app_esp32_protocol.h"
#include "../App/app_fall_detector.h"
#include "../App/app_buzzer.h"
#include "../App/app_sim900a.h"
#include "../App/app_emergency.h"
#include <string.h>

#define GYRO_THREAD_PRIORITY    7     
#define GYRO_THREAD_STACK_SIZE  1024  

#define GPS_THREAD_PRIORITY     8      
#define GPS_THREAD_STACK_SIZE   2048   

#define MAIN_THREAD_PRIORITY    15    
#define MAIN_THREAD_STACK_SIZE  2048  

#define ESP32_THREAD_PRIORITY   9
#define ESP32_THREAD_STACK_SIZE 1024

#define SIM900A_THREAD_PRIORITY 5
#define SIM900A_THREAD_STACK_SIZE 1024
#define SIM900A_CALL_HOLD_MS    20000
#define SIM900A_AFTER_HANGUP_MS 2000

#define THREAD_TIMESLICE        10
#define MAIN_LOGIC_PERIOD_MS    100
#define STM32_DATA_PERIOD_MS    5000
#define ANGLE_DEBUG_PERIOD_MS   0
#define APP_DEBUG_ANGLE_ENABLE  0
#define APP_DEBUG_GPS_ENABLE    0
#define APP_DEBUG_ESP32_ENABLE  0
#define GPS_SIGNAL_TIMEOUT_MS   6000

typedef struct {
    float roll;
    float pitch;
    float yaw;
    uint8_t fall_alarm;
    
    double latitude; 
    double longitude;
    uint8_t gps_fix; 
    rt_tick_t gps_last_tick;
} System_Data_t;

static System_Data_t g_sys_data = {0}; 

rt_mutex_t  dynamic_data_mutex = RT_NULL;
rt_sem_t    gyro_rx_sem        = RT_NULL;
rt_sem_t    gps_rx_sem         = RT_NULL;

static rt_thread_t gyro_thread       = RT_NULL;
static rt_thread_t gps_thread        = RT_NULL;
static rt_thread_t esp32_thread      = RT_NULL;
static rt_thread_t main_logic_thread = RT_NULL;
static rt_thread_t sim900a_thread     = RT_NULL;

extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3; 
extern UART_HandleTypeDef huart4; 
extern UART_HandleTypeDef huart6;

uint8_t g_usart3_rx_buf[33];

uint8_t g_usart4_rx_buf[512];
uint16_t gps_rx_len = 0;

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

static rt_size_t append_ascii_as_ucs2_hex(char *buf, rt_size_t size, const char *text)
{
    rt_size_t len;
    unsigned char ch;

    if (buf == RT_NULL || text == RT_NULL || size == 0)
    {
        return 0;
    }

    len = rt_strlen(buf);
    while (*text != '\0' && (len + 4) < size)
    {
        ch = (unsigned char)*text++;
        rt_snprintf(&buf[len], size - len, "%04X", (unsigned int)ch);
        len += 4;
    }

    return len;
}

static void build_fall_sms_ucs2(char *buf,
                                rt_size_t size,
                                double latitude,
                                double longitude,
                                uint8_t gps_fix,
                                rt_tick_t gps_last_tick)
{
    char lat_buf[24];
    char lon_buf[24];
    rt_tick_t now_tick = rt_tick_get();
    uint8_t gps_valid = 0;

    if (buf == RT_NULL || size == 0)
    {
        return;
    }

    buf[0] = '\0';
    rt_strncpy(buf, "6211645450124E86", size - 1);
    buf[size - 1] = '\0';

    gps_valid = (gps_fix &&
                 ((now_tick - gps_last_tick) <= rt_tick_from_millisecond(GPS_SIGNAL_TIMEOUT_MS))) ? 1 : 0;

    if (gps_valid)
    {
        format_coordinate(lat_buf, sizeof(lat_buf), latitude);
        format_coordinate(lon_buf, sizeof(lon_buf), longitude);
        append_ascii_as_ucs2_hex(buf, size, " LON:");
        append_ascii_as_ucs2_hex(buf, size, lon_buf);
        append_ascii_as_ucs2_hex(buf, size, " LAT:");
        append_ascii_as_ucs2_hex(buf, size, lat_buf);
    }
    else
    {
        append_ascii_as_ucs2_hex(buf, size, " GPS:NO SIGNAL");
    }
}

static void print_angle_debug(const AppFallDetectorResult_t *fall_result)
{
#if APP_DEBUG_ANGLE_ENABLE
    rt_kprintf("ANGLE,tick=%d,roll=%d,pitch=%d,yaw=%d,droll=%d,dpitch=%d,cal=%d,trigger=%d,cnt=%d,fall=%d\n",
               (int)rt_tick_get(),
               (int)fall_result->roll,
               (int)fall_result->pitch,
               (int)fall_result->yaw,
               (int)fall_result->delta_roll,
               (int)fall_result->delta_pitch,
               fall_result->calibrated,
               fall_result->posture_trigger,
               fall_result->confirm_count,
               fall_result->fall_alarm);
#else
    (void)fall_result;
#endif
}

static void gyro_thread_entry(void *parameter)
{
    rt_tick_t last_angle_debug_tick = 0;
    AppFallDetectorResult_t fall_result = {0};
    double local_lat = 0.0;
    double local_lon = 0.0;
    uint8_t local_fix = 0;
    rt_tick_t local_gps_tick = 0;
    AppEsp32Data_t esp32_data = {0};
    char sms_ucs2[APP_EMERGENCY_SMS_SIZE];

    while (1)
    {
        if (rt_sem_take(gyro_rx_sem, RT_WAITING_FOREVER) == RT_EOK)
        {
            for(int i = 0; i < 33; i++)
            {
                jy61p_ReceiveData(g_usart3_rx_buf[i]);
            }

            app_fall_detector_update((float)Roll,
                                     (float)Pitch,
                                     (float)Yaw,
                                     rt_tick_get(),
                                     &fall_result);
            app_buzzer_set(fall_result.fall_alarm);
            
            if (rt_mutex_take(dynamic_data_mutex, RT_WAITING_FOREVER) == RT_EOK)
            {
                g_sys_data.roll  = fall_result.roll;
                g_sys_data.pitch = fall_result.pitch;
                g_sys_data.yaw   = fall_result.yaw;
                g_sys_data.fall_alarm = fall_result.fall_alarm;
                local_lat = g_sys_data.latitude;
                local_lon = g_sys_data.longitude;
                local_fix = g_sys_data.gps_fix;
                local_gps_tick = g_sys_data.gps_last_tick;
                rt_mutex_release(dynamic_data_mutex);
            }
            if (app_emergency_update_fall(fall_result.fall_alarm) > 0)
            {
                rt_kprintf("[FALL] alarm=1\n");
                esp32_data.latitude = local_lat;
                esp32_data.longitude = local_lon;
                esp32_data.gps_fix = local_fix;
                esp32_data.gps_last_tick = local_gps_tick;
                esp32_data.fall_alarm = 1;
                app_esp32_protocol_send_data(&esp32_data);

                build_fall_sms_ucs2(sms_ucs2,
                                    sizeof(sms_ucs2),
                                    local_lat,
                                    local_lon,
                                    local_fix,
                                    local_gps_tick);
                if (app_emergency_queue_all_sms(sms_ucs2) > 0)
                {
                    rt_kprintf("[SIM900A] fall sms queued for all contacts\n");
                }
                else
                {
                    rt_kprintf("[SIM900A] fall sms queue failed\n");
                }
            }

            if ((rt_tick_get() - last_angle_debug_tick) >= rt_tick_from_millisecond(ANGLE_DEBUG_PERIOD_MS))
            {
                print_angle_debug(&fall_result);
                last_angle_debug_tick = rt_tick_get();
            }
        }
    }
}

static void gps_thread_entry(void *parameter)
{
    GPS_Info_t temp_gps = {0};

    while (1)
    {
        if (rt_sem_take(gps_rx_sem, RT_WAITING_FOREVER) == RT_EOK)
        {
            memset(&temp_gps, 0, sizeof(temp_gps));
            if (gps_rx_len >= sizeof(g_usart4_rx_buf))
            {
                gps_rx_len = sizeof(g_usart4_rx_buf) - 1;
            }
            g_usart4_rx_buf[gps_rx_len] = '\0';
//            rt_kprintf("RAW GPS DATA: %s\n", g_usart4_rx_buf);
            GPS_ParseData(g_usart4_rx_buf, &temp_gps);
            
            if (temp_gps.sentence_parsed &&
                rt_mutex_take(dynamic_data_mutex, RT_WAITING_FOREVER) == RT_EOK)
            {
                if (temp_gps.fix_status)
                {
                    g_sys_data.latitude  = temp_gps.latitude;
                    g_sys_data.longitude = temp_gps.longitude;
                    g_sys_data.gps_last_tick = rt_tick_get();
                }
                g_sys_data.gps_fix   = temp_gps.fix_status;
                rt_mutex_release(dynamic_data_mutex);
            }
            
            HAL_UART_Receive_DMA(&huart4, g_usart4_rx_buf, sizeof(g_usart4_rx_buf));
        }
    }
}

static void main_logic_thread_entry(void *parameter)
{
    double local_lat = 0.0;
    double local_lon = 0.0;
    uint8_t local_fix = 0;
    uint8_t local_fall_alarm = 0;
    rt_tick_t local_gps_tick = 0;
    uint8_t i;
    AppEsp32Cmd_t esp32_cmd = {0};
    AppEsp32Data_t esp32_data = {0};
    uint8_t contact_updated = 0;
    uint8_t esp32_data_report_enabled = 1;
    rt_tick_t last_stm32_data_tick = 0;

    last_stm32_data_tick = rt_tick_get() - rt_tick_from_millisecond(STM32_DATA_PERIOD_MS);

    while (1)
    {
        if (rt_mutex_take(dynamic_data_mutex, RT_WAITING_FOREVER) == RT_EOK)
        {
            local_lat   = g_sys_data.latitude;
            local_lon   = g_sys_data.longitude;
            local_fix   = g_sys_data.gps_fix;
            local_fall_alarm = g_sys_data.fall_alarm;
            local_gps_tick = g_sys_data.gps_last_tick;
            
            rt_mutex_release(dynamic_data_mutex);
        }

        app_esp32_protocol_take_cmd(&esp32_cmd);
        contact_updated = 0;

        for (i = 0; i < APP_ESP32_CONTACT_MAX; i++)
        {
            if (esp32_cmd.contact_update_mask & (1U << i))
            {
                app_emergency_set_contact(i,
                                          esp32_cmd.contacts[i].phone,
                                          esp32_cmd.contacts[i].priority);
                contact_updated = 1;
            }
        }
        if (contact_updated)
        {
            if (local_fall_alarm)
            {
                app_emergency_queue_fall_call();
            }
        }

#if APP_DEBUG_GPS_ENABLE
        if (local_fix)
        {
            rt_kprintf("[GPS ] FIX OK! Lat: %d, Lon: %d\n",
                      (int)(local_lat * 1000000.0), (int)(local_lon * 1000000.0));
        }
        else
        {
            rt_kprintf("[GPS ] NO FIX (Searching for satellites...)\n");
        }
#endif
        if (strcmp(esp32_cmd.text_msg, "start") == 0)
        {
            esp32_data_report_enabled = 1;
            rt_kprintf("[ESP32] data report start\n");
        }
        else if (strcmp(esp32_cmd.text_msg, "stop") == 0)
        {
            esp32_data_report_enabled = 0;
            rt_kprintf("[ESP32] data report stop\n");
        }
        if (esp32_cmd.call_cmd >= 1 && esp32_cmd.call_cmd <= APP_ESP32_CONTACT_MAX)
        {
#if APP_DEBUG_ESP32_ENABLE
            rt_kprintf("[ESP32] call contact%d\n", esp32_cmd.call_cmd);
#endif
            if (app_emergency_queue_contact_call((uint8_t)(esp32_cmd.call_cmd - 1)) != 0)
            {
                rt_kprintf("[ESP32] contact%d call queue failed\n", esp32_cmd.call_cmd);
            }
        }

        if ((rt_tick_get() - last_stm32_data_tick) >= rt_tick_from_millisecond(STM32_DATA_PERIOD_MS))
        {
            esp32_data.latitude = local_lat;
            esp32_data.longitude = local_lon;
            esp32_data.gps_fix = local_fix;
            esp32_data.gps_last_tick = local_gps_tick;
            esp32_data.fall_alarm = local_fall_alarm;
            if (esp32_data_report_enabled)
            {
                app_esp32_protocol_send_data(&esp32_data);
            }
            last_stm32_data_tick = rt_tick_get();
        }

        rt_thread_mdelay(MAIN_LOGIC_PERIOD_MS);
    }
}

static void sim900a_test_thread_entry(void *parameter)
{
    AppEmergencyCall_t call = {0};
    AppEmergencySms_t sms = {0};

    (void)parameter;

    while (1)
    {
        if (app_emergency_take_call(&call))
        {
            rt_kprintf("[SIM900A] dialing queued phone: %s\n", call.phone);
            if (app_sim900a_call(call.phone) != 0)
            {
                rt_kprintf("[SIM900A] call command failed\n");
            }
            rt_thread_mdelay(SIM900A_CALL_HOLD_MS);
            app_sim900a_hangup();
            rt_thread_mdelay(SIM900A_AFTER_HANGUP_MS);

            while (app_emergency_take_sms(&sms))
            {
                rt_kprintf("[SIM900A] sending fall sms to: %s\n", sms.phone);
                if (app_sim900a_send_sms_ucs2_hex(sms.phone, sms.message) != 0)
                {
                    rt_kprintf("[SIM900A] fall sms send failed\n");
                }
                rt_thread_mdelay(500);
            }
        }
        else if (app_emergency_take_sms(&sms))
        {
            rt_kprintf("[SIM900A] sending fall sms to: %s\n", sms.phone);
            if (app_sim900a_send_sms_ucs2_hex(sms.phone, sms.message) != 0)
            {
                rt_kprintf("[SIM900A] fall sms send failed\n");
            }
        }

        rt_thread_mdelay(500);
    }
}

int app_init(void)
{
    dynamic_data_mutex = rt_mutex_create("data_mut", RT_IPC_FLAG_PRIO);
    gyro_rx_sem        = rt_sem_create("gyro_sem", 0, RT_IPC_FLAG_FIFO);
    gps_rx_sem         = rt_sem_create("gps_sem", 0, RT_IPC_FLAG_FIFO); 

    if (dynamic_data_mutex == RT_NULL ||
        gyro_rx_sem == RT_NULL || gps_rx_sem == RT_NULL)
    {
        rt_kprintf("IPC objects creation failed!\n");
        return -1;
    }
    if (app_emergency_init() != 0)
    {
        rt_kprintf("Emergency app init failed!\n");
        return -1;
    }

    if (app_esp32_protocol_init(&huart2) != 0)
    {
        rt_kprintf("ESP32 protocol init failed!\n");
        return -1;
    }
    if (app_sim900a_init(&huart6) != 0)
    {
        rt_kprintf("SIM900A protocol init failed!\n");
        return -1;
    }
    app_fall_detector_init();
    app_buzzer_init();

    gyro_thread = rt_thread_create("gyro_tsk", gyro_thread_entry, RT_NULL, GYRO_THREAD_STACK_SIZE, GYRO_THREAD_PRIORITY, THREAD_TIMESLICE);
    if (gyro_thread != RT_NULL) rt_thread_startup(gyro_thread);

    gps_thread = rt_thread_create("gps_tsk", gps_thread_entry, RT_NULL, GPS_THREAD_STACK_SIZE, GPS_THREAD_PRIORITY, THREAD_TIMESLICE);
    if (gps_thread != RT_NULL) rt_thread_startup(gps_thread);

    esp32_thread = rt_thread_create("esp32_tsk", app_esp32_protocol_rx_thread_entry, RT_NULL, ESP32_THREAD_STACK_SIZE, ESP32_THREAD_PRIORITY, THREAD_TIMESLICE);
    if (esp32_thread != RT_NULL) rt_thread_startup(esp32_thread);

    main_logic_thread = rt_thread_create("main_tsk", main_logic_thread_entry, RT_NULL, MAIN_THREAD_STACK_SIZE, MAIN_THREAD_PRIORITY, THREAD_TIMESLICE);
    if (main_logic_thread != RT_NULL) rt_thread_startup(main_logic_thread);

    sim900a_thread = rt_thread_create("gsm_tsk", sim900a_test_thread_entry, RT_NULL, SIM900A_THREAD_STACK_SIZE, SIM900A_THREAD_PRIORITY, THREAD_TIMESLICE);
    if (sim900a_thread != RT_NULL) rt_thread_startup(sim900a_thread);

    __HAL_UART_CLEAR_FLAG(&huart3, UART_FLAG_RXNE);
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    HAL_UART_Receive_DMA(&huart3, g_usart3_rx_buf, 33);

    __HAL_UART_CLEAR_IDLEFLAG(&huart4);
    __HAL_UART_ENABLE_IT(&huart4, UART_IT_IDLE);
    HAL_UART_Receive_DMA(&huart4, g_usart4_rx_buf, sizeof(g_usart4_rx_buf));

    rt_kprintf("Project framework started successfully!\n");
    return 0;
}

MSH_CMD_EXPORT(app_init, initialize and run project framework);
