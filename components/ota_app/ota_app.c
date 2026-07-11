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