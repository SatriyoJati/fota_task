#include "boardled.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#define TAG "led_task"

#define LED_GPIO 8


static TaskHandle_t xTaskLedHandle = NULL;
static QueueHandle_t  xVariableQueue = NULL;

static void led_task(void *pvParameters) {
    uint8_t led_state = 0;
    uint8_t mode = 0;
    uint8_t current_mode = 1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (xQueueReceive(xVariableQueue, &mode, 0) == pdPASS){
            current_mode = mode;
        }
   
        switch (current_mode)
        {
            case 0:
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(LED_GPIO,1);
                /* code */
                break;
            
            case 1:
                gpio_set_level(LED_GPIO,1);
                break;
            
            case 2:
                gpio_set_level(LED_GPIO,0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;

            case 3:
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_GPIO,1);
                break;

            case 4:
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(LED_GPIO,1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            
            case 5:
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(LED_GPIO,1);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;

            case 6:
                for(int i = 0; i < 2; i++) {
                    gpio_set_level(LED_GPIO, 0);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    gpio_set_level(LED_GPIO,1);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }   
                vTaskDelay(pdMS_TO_TICKS(2000));
                break;

            default:
                break;
        
        }
    }
}

void setup_pin()
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    xVariableQueue = xQueueCreate(5, sizeof(uint8_t));
    xTaskCreate(
        led_task,
        "led_task",
        4096,
        NULL,
        1,
        &xTaskLedHandle
    );
}

void turn_on(void)
{
    uint8_t turn_on_mode = 2;
    xQueueSend(xVariableQueue, &turn_on_mode, portMAX_DELAY);
}

void turn_off()
{
    uint8_t turn_off_mode = 1;
    xQueueSend(xVariableQueue, &turn_off_mode, portMAX_DELAY);
}

void toggle_led()
{
    uint8_t toggle_led_mode = 0;
    xQueueSend(xVariableQueue, &toggle_led_mode, portMAX_DELAY);
}

void toggle_fast()
{
    uint8_t toggle_led_mode = 3;
    xQueueSend(xVariableQueue, &toggle_led_mode, portMAX_DELAY);
}

void normal_blink()
{
    uint8_t normal_blink_mode = 4;
    xQueueSend(xVariableQueue, &normal_blink_mode, portMAX_DELAY);
}

void ota_led_indicator_start()
{
    uint8_t ota_mode = 5;
    xQueueSend(xVariableQueue, &ota_mode, portMAX_DELAY);
}

void recovery_blink()
{
    uint8_t recovery_mode = 6;
    xQueueSend(xVariableQueue, &recovery_mode, portMAX_DELAY);
}
