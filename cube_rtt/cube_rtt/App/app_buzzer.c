#include "app_buzzer.h"
#include "gpio.h"

#define APP_BUZZER_GPIO_PORT    GPIOB
#define APP_BUZZER_GPIO_PIN     GPIO_PIN_0
#define APP_BUZZER_ON_LEVEL     GPIO_PIN_RESET
#define APP_BUZZER_OFF_LEVEL    GPIO_PIN_SET

void app_buzzer_init(void)
{
    app_buzzer_set(0);
}

void app_buzzer_set(uint8_t enabled)
{
    HAL_GPIO_WritePin(APP_BUZZER_GPIO_PORT,
                      APP_BUZZER_GPIO_PIN,
                      enabled ? APP_BUZZER_ON_LEVEL : APP_BUZZER_OFF_LEVEL);
}
