#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "ota_app.h"


#define TAG "OTA_APP"

extern char global_ota_url_buffer[256];
char app_version_str[32] = {0};
char partition_type_str[32] = {0};

extern const uint8_t ota_chaos_cert_pem_start[] asm("_binary_ota_chaos_cert_pem_start");
extern const uint8_t ota_chaos_cert_pem_end[]   asm("_binary_ota_chaos_cert_pem_end");

void check_active_firmware(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        strlcpy(app_version_str, app_desc.version, sizeof(app_version_str));
    }
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        strcpy(partition_type_str, "Factory Partition");
    } else if (running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        snprintf(partition_type_str, sizeof(partition_type_str), "OTA Slot %d", running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    }
}



static void ota_run_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting firmware upgrade transaction...");

    char *dynamic_url = (char *)pvParameter;
    if (dynamic_url == NULL || strlen(dynamic_url) == 0) {
        ESP_LOGE(TAG, "Invalid or empty OTA URL target pointer passed.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Targeting server bin endpoint location: %s", dynamic_url);

    esp_http_client_config_t client_cfg = {
        .url = dynamic_url,
        .skip_cert_common_name_check = true,
        // .transport_type = HTTP_TRANSPORT_OVER_TCP,
        
        .cert_pem = (const char *)ota_chaos_cert_pem_start,          
        .skip_cert_common_name_check = true,
        .timeout_ms = 15000, 
    };
    esp_https_ota_config_t ota_config = { .http_config = &client_cfg };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Firmware updated! Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed with error: %d", ret);
    }
    vTaskDelete(NULL);
}

void ota_perform_task(){
    xTaskCreate(&ota_run_task, "module_ota_worker", 8192, (void*)global_ota_url_buffer, 5, NULL);
}

void trigger_factory_reset(void) 
{
    ESP_LOGI(TAG, "Starting factory reset process...");

    // 1. Find the factory partition
    const esp_partition_t *factory_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, 
        ESP_PARTITION_SUBTYPE_APP_FACTORY, 
        NULL
    );

    if (factory_part == NULL) {
        ESP_LOGE(TAG, "Factory partition not found!");
        return;
    }

    // 2. Point the bootloader back to the factory partition
    esp_err_t err = esp_ota_set_boot_partition(factory_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition to factory: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Boot partition successfully directed to Factory.");

    // 3. Find and completely erase the OTA data partition
    const esp_partition_t *ota_data_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_OTA, 
        NULL
    );

    if (ota_data_part != NULL) {
        ESP_LOGI(TAG, "Erasing OTA data partition...");
        err = esp_partition_erase_range(ota_data_part, 0, ota_data_part->size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase OTA data: %s", esp_err_to_name(err));
        }
    }
    nvs_flash_deinit(); 

    // 4. Wipe the NVS flash to delete saved WiFi credentials and tokens
    ESP_LOGI(TAG, "Wiping NVS Flash...");
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    }

    // 5. Reboot the chip
    ESP_LOGI(TAG, "Reset complete. Rebooting into factory mode in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static const char* resolve_last_boot_result(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON:   return "Power On / Cable Plugged";
        case ESP_RST_EXT:       return "External Reset Button Pin";
        case ESP_RST_SW:        return "Software Restart (e.g. OTA Apply)";
        case ESP_RST_PANIC:     return "Software Crash Panic";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog Reset";
        case ESP_RST_TASK_WDT:  return "Task Watchdog Freeze Reset";
        case ESP_RST_DEEPSLEEP: return "Deep Sleep Wakeup";
        case ESP_RST_BROWNOUT:  return "Brownout Voltage Drop Reset";
        default:                return "Unknown Initialization Reason";
    }
}


esp_err_t my_ota_get_system_status(my_ota_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Clear memory block safely
    memset(status, 0, sizeof(my_ota_status_t));

    // 2. Fetch Version Data
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc != NULL) {
        strncpy(status->version, app_desc->version, sizeof(status->version) - 1);
    } else {
        strncpy(status->version, "0.0.0-dev", sizeof(status->version) - 1);
    }

    // 3. Fetch Partition Information
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    if (running_part != NULL) {
        strncpy(status->partition, running_part->label, sizeof(status->partition) - 1);
    } else {
        strncpy(status->partition, "Unknown", sizeof(status->partition) - 1);
    }

    // 4. Resolve Boot Health State Machine
    const char *boot_msg = resolve_last_boot_result();
    strncpy(status->last_boot, boot_msg, sizeof(status->last_boot) - 1);

    return ESP_OK;
}
