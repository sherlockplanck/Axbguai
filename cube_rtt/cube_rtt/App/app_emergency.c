#include "app_emergency.h"
#include "main.h"
#include <string.h>

#define APP_EMERGENCY_FLASH_ADDR        0x080E0000U
#define APP_EMERGENCY_FLASH_SECTOR      FLASH_SECTOR_11
#define APP_EMERGENCY_FLASH_MAGIC       0x45524759U
#define APP_EMERGENCY_FLASH_VERSION     1U
#define APP_EMERGENCY_FALL_COOLDOWN_MS  60000

typedef struct
{
    rt_mutex_t mutex;
    AppEmergencyContact_t contacts[APP_EMERGENCY_CONTACT_MAX];
    uint8_t last_fall_alarm;
    uint8_t call_pending;
    uint8_t sms_head;
    uint8_t sms_tail;
    uint8_t sms_count;
    AppEmergencyCall_t pending_call;
    AppEmergencySms_t pending_sms[APP_EMERGENCY_CONTACT_MAX];
    rt_tick_t last_fall_event_tick;
} AppEmergencyState_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    AppEmergencyContact_t contacts[APP_EMERGENCY_CONTACT_MAX];
    uint32_t checksum;
} AppEmergencyFlashData_t;

static AppEmergencyState_t s_emergency = {0};

static uint32_t calc_checksum(const uint8_t *data, rt_size_t len)
{
    uint32_t hash = 2166136261U;
    rt_size_t i;

    for (i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= 16777619U;
    }

    return hash;
}

static void safe_copy(char *dst, rt_size_t dst_size, const char *src)
{
    if (dst == RT_NULL || dst_size == 0)
    {
        return;
    }

    if (src == RT_NULL)
    {
        dst[0] = '\0';
        return;
    }

    rt_strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static uint8_t contact_is_same(const AppEmergencyContact_t *contact,
                               const char *phone,
                               int priority)
{
    if (contact == RT_NULL || phone == RT_NULL)
    {
        return 0;
    }

    return (contact->valid &&
            contact->priority == priority &&
            strcmp(contact->phone, phone) == 0) ? 1 : 0;
}

static int normalize_priority(int priority)
{
    return (priority <= 0) ? 0 : 1;
}

static uint8_t flash_data_is_valid(const AppEmergencyFlashData_t *data)
{
    uint32_t checksum;

    if (data == RT_NULL ||
        data->magic != APP_EMERGENCY_FLASH_MAGIC ||
        data->version != APP_EMERGENCY_FLASH_VERSION)
    {
        return 0;
    }

    checksum = calc_checksum((const uint8_t *)data,
                             sizeof(AppEmergencyFlashData_t) - sizeof(uint32_t));

    return (checksum == data->checksum) ? 1 : 0;
}

static uint8_t load_contacts_from_flash_locked(void)
{
    const AppEmergencyFlashData_t *flash_data =
        (const AppEmergencyFlashData_t *)APP_EMERGENCY_FLASH_ADDR;
    uint8_t count = 0;
    uint8_t i;

    if (!flash_data_is_valid(flash_data))
    {
        return 0;
    }

    for (i = 0; i < APP_EMERGENCY_CONTACT_MAX; i++)
    {
        s_emergency.contacts[i] = flash_data->contacts[i];
        if (s_emergency.contacts[i].valid)
        {
            s_emergency.contacts[i].phone[APP_EMERGENCY_CONTACT_SIZE - 1] = '\0';
            count++;
        }
    }

    return count;
}

static int save_contacts_to_flash_locked(void)
{
    AppEmergencyFlashData_t flash_data = {0};
    FLASH_EraseInitTypeDef erase_init = {0};
    uint32_t sector_error = 0;
    uint32_t addr = APP_EMERGENCY_FLASH_ADDR;
    const uint32_t *word_data = (const uint32_t *)&flash_data;
    rt_size_t word_count = sizeof(AppEmergencyFlashData_t) / sizeof(uint32_t);
    rt_size_t i;
    HAL_StatusTypeDef status;

    flash_data.magic = APP_EMERGENCY_FLASH_MAGIC;
    flash_data.version = APP_EMERGENCY_FLASH_VERSION;
    for (i = 0; i < APP_EMERGENCY_CONTACT_MAX; i++)
    {
        flash_data.contacts[i] = s_emergency.contacts[i];
    }
    flash_data.checksum = calc_checksum((const uint8_t *)&flash_data,
                                        sizeof(AppEmergencyFlashData_t) - sizeof(uint32_t));

    HAL_FLASH_Unlock();

    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector = APP_EMERGENCY_FLASH_SECTOR;
    erase_init.NbSectors = 1;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    status = HAL_FLASHEx_Erase(&erase_init, &sector_error);
    if (status != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -1;
    }

    for (i = 0; i < word_count; i++)
    {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word_data[i]);
        if (status != HAL_OK)
        {
            HAL_FLASH_Lock();
            return -1;
        }
        addr += sizeof(uint32_t);
    }

    HAL_FLASH_Lock();
    return 0;
}

int app_emergency_init(void)
{
    uint8_t loaded_count = 0;

    if (s_emergency.mutex == RT_NULL)
    {
        s_emergency.mutex = rt_mutex_create("emer_mut", RT_IPC_FLAG_PRIO);
    }

    if (s_emergency.mutex == RT_NULL)
    {
        return -1;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        loaded_count = load_contacts_from_flash_locked();
        rt_mutex_release(s_emergency.mutex);
    }

    if (loaded_count > 0)
    {
        rt_kprintf("[EMERGENCY] loaded contact_count=%d\n", loaded_count);
    }

    return 0;
}

int app_emergency_set_contact(uint8_t index, const char *phone, int priority)
{
    int save_result = 0;
    int normalized_priority = normalize_priority(priority);

    if (s_emergency.mutex == RT_NULL ||
        index >= APP_EMERGENCY_CONTACT_MAX ||
        phone == RT_NULL ||
        phone[0] == '\0')
    {
        return -1;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -1;
    }

    if (contact_is_same(&s_emergency.contacts[index], phone, normalized_priority))
    {
        rt_mutex_release(s_emergency.mutex);
        return 0;
    }

    safe_copy(s_emergency.contacts[index].phone,
              sizeof(s_emergency.contacts[index].phone),
              phone);
    s_emergency.contacts[index].priority = normalized_priority;
    s_emergency.contacts[index].valid = 1;
    save_result = save_contacts_to_flash_locked();

    rt_mutex_release(s_emergency.mutex);
    rt_kprintf("[EMERGENCY] contact%d=%s,priority=%d\n",
               (int)index + 1,
               phone,
               normalized_priority);
    if (save_result != 0)
    {
        rt_kprintf("[EMERGENCY] flash save failed\n");
        return -1;
    }

    return 0;
}

uint8_t app_emergency_has_contact(void)
{
    uint8_t count = app_emergency_get_contact_count();

    return (count > 0) ? 1 : 0;
}

uint8_t app_emergency_get_contact_count(void)
{
    uint8_t count = 0;
    uint8_t i;

    if (s_emergency.mutex == RT_NULL)
    {
        return 0;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        for (i = 0; i < APP_EMERGENCY_CONTACT_MAX; i++)
        {
            if (s_emergency.contacts[i].valid)
            {
                count++;
            }
        }
        rt_mutex_release(s_emergency.mutex);
    }

    return count;
}

static int find_best_contact_index(void)
{
    int best_index = -1;
    uint8_t i;

    for (i = 0; i < APP_EMERGENCY_CONTACT_MAX; i++)
    {
        if (!s_emergency.contacts[i].valid)
        {
            continue;
        }

        if (best_index < 0 ||
            s_emergency.contacts[i].priority < s_emergency.contacts[best_index].priority)
        {
            best_index = i;
        }
    }

    return best_index;
}

int app_emergency_queue_fall_call(void)
{
    int best_index;

    if (s_emergency.mutex == RT_NULL)
    {
        return -1;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -1;
    }

    best_index = find_best_contact_index();
    if (best_index < 0 || s_emergency.call_pending)
    {
        rt_mutex_release(s_emergency.mutex);
        return -1;
    }

    safe_copy(s_emergency.pending_call.phone,
              sizeof(s_emergency.pending_call.phone),
              s_emergency.contacts[best_index].phone);

    s_emergency.call_pending = 1;
    rt_kprintf("[EMERGENCY] fall call queued: %s\n", s_emergency.pending_call.phone);
    rt_mutex_release(s_emergency.mutex);
    return 0;
}

int app_emergency_queue_contact_call(uint8_t index)
{
    if (s_emergency.mutex == RT_NULL || index >= APP_EMERGENCY_CONTACT_MAX)
    {
        rt_kprintf("[EMERGENCY] contact%d call failed: invalid index or mutex\n", (int)index + 1);
        return -1;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        rt_kprintf("[EMERGENCY] contact%d call failed: mutex busy\n", (int)index + 1);
        return -1;
    }

    if (!s_emergency.contacts[index].valid)
    {
        rt_kprintf("[EMERGENCY] contact%d call failed: contact not set\n", (int)index + 1);
        rt_mutex_release(s_emergency.mutex);
        return -1;
    }

    if (s_emergency.call_pending)
    {
        rt_kprintf("[EMERGENCY] contact%d call failed: call busy\n", (int)index + 1);
        rt_mutex_release(s_emergency.mutex);
        return -1;
    }

    safe_copy(s_emergency.pending_call.phone,
              sizeof(s_emergency.pending_call.phone),
              s_emergency.contacts[index].phone);

    s_emergency.call_pending = 1;
    rt_kprintf("[EMERGENCY] contact call queued: %s\n", s_emergency.pending_call.phone);
    rt_mutex_release(s_emergency.mutex);
    return 0;
}

int app_emergency_update_fall(uint8_t fall_alarm)
{
    uint8_t should_queue = 0;
    rt_tick_t now_tick = rt_tick_get();
    rt_tick_t cooldown_tick = rt_tick_from_millisecond(APP_EMERGENCY_FALL_COOLDOWN_MS);

    if (s_emergency.mutex == RT_NULL)
    {
        return -1;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -1;
    }

    if (fall_alarm && !s_emergency.last_fall_alarm)
    {
        if (s_emergency.last_fall_event_tick == 0 ||
            ((now_tick - s_emergency.last_fall_event_tick) >= cooldown_tick))
        {
            should_queue = 1;
            s_emergency.last_fall_event_tick = now_tick;
        }
        else
        {
            rt_kprintf("[EMERGENCY] fall event ignored: cooldown\n");
        }
    }
    s_emergency.last_fall_alarm = fall_alarm;

    rt_mutex_release(s_emergency.mutex);

    if (should_queue)
    {
        app_emergency_queue_fall_call();
        return 1;
    }

    return 0;
}

int app_emergency_take_call(AppEmergencyCall_t *call)
{
    if (s_emergency.mutex == RT_NULL || call == RT_NULL)
    {
        return 0;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return 0;
    }

    if (!s_emergency.call_pending)
    {
        rt_mutex_release(s_emergency.mutex);
        return 0;
    }

    *call = s_emergency.pending_call;
    s_emergency.call_pending = 0;
    rt_mutex_release(s_emergency.mutex);
    return 1;
}

int app_emergency_queue_all_sms(const char *message)
{
    uint8_t queued_count = 0;
    uint8_t i;

    if (s_emergency.mutex == RT_NULL || message == RT_NULL || message[0] == '\0')
    {
        return -1;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -1;
    }

    for (i = 0; i < APP_EMERGENCY_CONTACT_MAX; i++)
    {
        if (!s_emergency.contacts[i].valid ||
            s_emergency.sms_count >= APP_EMERGENCY_CONTACT_MAX)
        {
            continue;
        }

        safe_copy(s_emergency.pending_sms[s_emergency.sms_tail].phone,
                  sizeof(s_emergency.pending_sms[s_emergency.sms_tail].phone),
                  s_emergency.contacts[i].phone);
        safe_copy(s_emergency.pending_sms[s_emergency.sms_tail].message,
                  sizeof(s_emergency.pending_sms[s_emergency.sms_tail].message),
                  message);

        s_emergency.sms_tail = (uint8_t)((s_emergency.sms_tail + 1) % APP_EMERGENCY_CONTACT_MAX);
        s_emergency.sms_count++;
        queued_count++;
    }

    rt_mutex_release(s_emergency.mutex);
    return (queued_count > 0) ? queued_count : -1;
}

int app_emergency_take_sms(AppEmergencySms_t *sms)
{
    if (s_emergency.mutex == RT_NULL || sms == RT_NULL)
    {
        return 0;
    }

    if (rt_mutex_take(s_emergency.mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return 0;
    }

    if (s_emergency.sms_count == 0)
    {
        rt_mutex_release(s_emergency.mutex);
        return 0;
    }

    *sms = s_emergency.pending_sms[s_emergency.sms_head];
    s_emergency.sms_head = (uint8_t)((s_emergency.sms_head + 1) % APP_EMERGENCY_CONTACT_MAX);
    s_emergency.sms_count--;
    rt_mutex_release(s_emergency.mutex);
    return 1;
}
