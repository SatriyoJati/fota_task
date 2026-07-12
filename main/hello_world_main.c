/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "wifi_ble_provisioning.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "boardled.h"
#include "ota_app.h"
#include "task_simulation.h"

#include "config_portal_ap.h"

// Forward declarations of your custom functions
void run_factory_logic(void);
void run_ota_logic(void);

static const char *TAG = "MAIN_APP";
#define BOOT_BUTTON_PIN   9
#define ESP_INTR_FLAG_DEFAULT 0
#define BLINK_GPIO 2

static QueueHandle_t gpio_evt_queue = NULL;
static volatile uint8_t is_reset_button_pressed = 0;
static volatile int64_t button_press_start_time = 0;

// Watchdog simulation variable
volatile bool critical_loop_heartbeat = true;

void simple_task_feeding_watchdog(void *pvParameters) {
    while (1) {        
        // Feeding
        critical_loop_heartbeat = true;

        vTaskDelay(pdMS_TO_TICKS(500)); // Run twice a second
    }
}

typedef struct {
    uint32_t pin;
    int level;
    int64_t timestamp;
} gpio_evt_t;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    // ESP_LOGI(TAG, "GPIO %d Interrupt Triggered!", gpio_num);

    // xQueueSendFromISR(gpio_evt_queue, &evt, NULL);
    if (!is_reset_button_pressed) {
        is_reset_button_pressed = 1;
        button_press_start_time = esp_timer_get_time();
    }
}

void verify_system_partitions_and_ota_data(void) {
    ESP_LOGI("VERIFY", "==================================================");
    ESP_LOGI("VERIFY", "     RUNNING RUNTIME PARTITION MAP DIAGNOSTICS      ");
    ESP_LOGI("VERIFY", "==================================================");

    // 1. Check if the 'otadata' control sector exists in Flash
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_OTA, 
        NULL
    );

    if (otadata == NULL) {
        ESP_LOGE("VERIFY", "CRITICAL ERROR: 'otadata' partition is MISSING from your table layout!");
        ESP_LOGE("VERIFY", "Without an 'otadata' slot, the ESP32 can NEVER switch boot apps.");
        ESP_LOGW("VERIFY", "Fix: Run 'idf.py menuconfig' -> 'Partition Table' -> select 'Factory app, two OTA definitions'.");
    } else {
        ESP_LOGI("VERIFY", "✅ Checked 'otadata': Found at address 0x%08X (Size: %d bytes)", 
                 (unsigned int)otadata->address, (int)otadata->size);
    }

    // 2. Scan and print all available App Slots
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    int app_slots_found = 0;
    
    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        ESP_LOGI("VERIFY", "Found App Slot -> Label: '%s' | Subtype: 0x%02X | Size: %d bytes", 
                 part->label, part->subtype, (int)part->size);
        app_slots_found++;
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    if (app_slots_found < 3) {
        ESP_LOGW("VERIFY", "WARNING: You only have %d app slots. Standard OTA requires at least 3 (factory, ota_0, ota_1).", app_slots_found);
    }

    // 3. Track Current Execution Environment vs Targets
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running    = esp_ota_get_running_partition();
    const esp_partition_t *next_target= esp_ota_get_next_update_partition(NULL);

    ESP_LOGI("VERIFY", "--------------------------------------------------");
    ESP_LOGI("VERIFY", "Active Partition Layout Tracker Status:");
    ESP_LOGI("VERIFY", " - Currently Executing From Slot: %s", running ? running->label : "NULL");
    ESP_LOGI("VERIFY", " - Stored Target in Bootloader:   %s", configured ? configured->label : "NULL");
    ESP_LOGI("VERIFY", " - Next Staging Download Slot:    %s", next_target ? next_target->label : "NULL");
    ESP_LOGI("VERIFY", "==================================================");
}

void reset_factory_task () {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    gpio_isr_handler_add(BOOT_BUTTON_PIN, gpio_isr_handler, (void*) BOOT_BUTTON_PIN);
    int64_t current_time = 0;
    int64_t end_time = 0;
    gpio_evt_t evt;
    while (1) {
        if (is_reset_button_pressed) {
            current_time = esp_timer_get_time();
            if(gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                if ((current_time - button_press_start_time) >= 5000000) {
                    ESP_LOGI(TAG, "Factory Reset Button Pressed!");
                    ESP_LOGI(TAG, "Starting Factory Reset...");
                    trigger_factory_reset();
                }
            } else {
                vTaskDelay(100 / portTICK_PERIOD_MS);

                if (gpio_get_level(BOOT_BUTTON_PIN) == 1) {
                    if((current_time - button_press_start_time) >= 100000) {
                        ESP_LOGI(TAG, "Factory Reset Button Released after %lld seconds!", (current_time - button_press_start_time)/1000000);
                    }
                    is_reset_button_pressed = 0;
                } 
            }
        } 

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    fflush(stdout);

    // 1. Get the currently running partition details
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running from partition: %s", running_partition->label);

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    ESP_LOGI("BOOT_CHECK", "Configured Boot Slot: %s", configured ? configured->label : "NULL");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    verify_system_partitions_and_ota_data();

    // Task feeding watchdog to ensure that the OTA validation loop can check for system health
    xTaskCreatePinnedToCore(
        &simple_task_feeding_watchdog, 
        "simple_task_feeding_watchdog", 
        2048,
        NULL, 
        6, 
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        &reset_factory_task, 
        "reset_factory_task", 
        4096 , 
        NULL, 
        5, 
        NULL,
        0
    );

    setup_pin(); // Initialize LED GPIO for status indication

    // 2. Differentiate behavior based on subtype
    if (running_partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        ESP_LOGW(TAG, "SYSTEM STARTED IN FACTORY MODE");
        run_factory_logic();
    } 
    else if (running_partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || 
             running_partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        ESP_LOGI(TAG, "SYSTEM STARTED IN PRODUCTION (OTA) MODE");
        run_ota_logic();
    } 
    else {
        ESP_LOGE(TAG, "Unexpected partition subtype: %d", running_partition->subtype);
    }

}

void run_factory_logic(void) {
    ESP_LOGI(TAG, "Initializing softAP / Provisioning mode...");
    // 1. Start Wi-Fi in Access Point (SoftAP) mode
    // 2. Launch an HTTP server to receive Wi-Fi credentials and new OTA firmware
    // 3. Keep a GPIO interrupt active for factory exit/reset conditions
    recovery_blink(); // Blink LED to indicate factory mode
    ota_engine_init_portal();

    // while(1) {
    //     // Factory loop logic
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }
}

void run_ota_logic(void) {
    ESP_LOGI(TAG, "Running OTA logic...");
    // 1. Check for available OTA updates
    // 2. Download and install updates if available
    // 3. Reboot if necessary
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {

            // Validation logic: If the app is running correctly, mark it as valid to cancel rollback
            ESP_LOGI(TAG, "App is in PENDING_VERIFY state. Starting stability checks...");
            // 1. Simulate stability period 10 seconds.
            vTaskDelay(pdMS_TO_TICKS(10000)); // Simulate some validation checks
            bool system_checks_passed = true; 

            // 2. Simulate a watchdog check . There will be simple task that will set critical_loop_heartbeat 
            // to true to prevent the watchdog from triggering a rollback. If the task fails to set it, we will force a rollback.
            for (int i = 0; i < 4; i++) {
                vTaskDelay(pdMS_TO_TICKS(2500)); // Sleep for 2.5 second at a time
        
                    // Check if our critical loops are still alive and reporting
                    if (!critical_loop_heartbeat) {
                        ESP_LOGE(TAG, "CRITICAL WATCHDOG ERROR: Loop is frozen inside the 10s stability window!");
                        
                        // POINT 3: Application explicitly marks itself invalid and reboots!
                        esp_ota_mark_app_invalid_rollback_and_reboot(); 
                        
                    }
                    critical_loop_heartbeat = false; 
            }

            if (system_checks_passed) {
                // CRITICAL STEP: Permanently validate the app and cancel the pending rollback!
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "SUCCESS: Stability period met! New firmware locked in permanently.");
                } else {
                    ESP_LOGE(TAG, "Failed to confirm validity flag: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGE(TAG, "Self-checks failed during stability period! Forcing manual rollback...");
                // If your test fails, you can force an instant rollback instead of waiting
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }

    normal_blink(); // Blink LED to indicate normal operation
    run_simulation_tasks(); // Start the simulation tasks for telemetry and monitoring
    // while(1) {
    //     // OTA loop logic
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }
}
