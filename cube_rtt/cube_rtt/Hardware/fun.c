#include "fun.h" 
#include <rtthread.h>

extern UART_HandleTypeDef huart3;
extern uint8_t g_usart3_rx_buf[33];
extern rt_sem_t gyro_rx_sem;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart == &huart3)
    {
        rt_interrupt_enter();
        if (gyro_rx_sem != RT_NULL)
        {
            rt_sem_release(gyro_rx_sem);
        }
        rt_interrupt_leave();
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if(huart == &huart3)
    {
        HAL_UART_DMAStop(&huart3);
        __HAL_UART_CLEAR_OREFLAG(&huart3);
        HAL_UART_Receive_DMA(&huart3, g_usart3_rx_buf, 33);
    }
}

extern UART_HandleTypeDef huart4;
extern uint8_t g_usart4_rx_buf[512];
extern uint16_t gps_rx_len;
extern rt_sem_t gps_rx_sem;

void FUN_UART4_IDLE_Callback(void)
{
    if(__HAL_UART_GET_FLAG(&huart4, UART_FLAG_IDLE) != RESET)
    {
        __HAL_UART_CLEAR_IDLEFLAG(&huart4); 
        
        HAL_UART_DMAStop(&huart4);
        gps_rx_len = sizeof(g_usart4_rx_buf) - __HAL_DMA_GET_COUNTER(huart4.hdmarx);
        
        rt_interrupt_enter();
        if (gps_rx_sem != RT_NULL)
        {
            rt_sem_release(gps_rx_sem);
        }
        rt_interrupt_leave();
    }
}
