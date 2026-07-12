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

#define NVS_OTA_NAMESPACE "stored_ota"
#define NVS_KEY_OFFSET    "bytes_written"

static int total_file_size = 0;
static bool is_ota_handler_registered = false;

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

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                int incoming_size = atoi(evt->header_value);
                if (incoming_size > total_file_size) {
                        total_file_size = incoming_size;
                    }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void ota_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == ESP_HTTPS_OTA_EVENT) {
        switch (event_id) {
            case ESP_HTTPS_OTA_START:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] OTA Transaction Initialized.");
                break;
            case ESP_HTTPS_OTA_CONNECTED:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Connected successfully to remote secure server.");
                break;
            case ESP_HTTPS_OTA_GET_IMG_DESC:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Reading internal raw binary descriptor tags...");
                break;
            case ESP_HTTPS_OTA_VERIFY_CHIP_ID:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Validating target hardware chip identification matches.");
                break;
            case ESP_HTTPS_OTA_VERIFY_CHIP_REVISION:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Verifying silicon chip revision.");
                break;
            case ESP_HTTPS_OTA_DECRYPT_CB: // <-- Corrected from DECRYPT_BOOTLOADER
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Running secure firmware decryption callback hooks.");
                break;
            case ESP_HTTPS_OTA_WRITE_FLASH:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Beginning binary stream deployment to target partition.");
                break;
            case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Success! Redirecting main target partition pointer flags.");
                break;
            case ESP_HTTPS_OTA_FINISH:
                ESP_LOGI(TAG, "[PROCESS INDICATOR] Stream transaction completed perfectly.");
                break;
            case ESP_HTTPS_OTA_ABORT:
                ESP_LOGE(TAG, "[PROCESS INDICATOR] FATAL: Stream transaction failed or aborted prematurely.");
                break;
            default:
                break;
        }
    }
}

static void resolve_rollback_reason(char *dest_buffer, size_t max_len) {
    const esp_partition_t *failed_part = esp_ota_get_last_invalid_partition();
    
    if (failed_part == NULL) {
        strncpy(dest_buffer, "None (System Healthy)", max_len - 1);
        dest_buffer[max_len - 1] = '\0';
        return;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(failed_part, &ota_state) == ESP_OK) {
        switch (ota_state) {
            case ESP_OTA_IMG_ABORTED:
                snprintf(dest_buffer, max_len, "Aborted: %s failed to validate", failed_part->label);
                break;
            case ESP_OTA_IMG_INVALID:
                snprintf(dest_buffer, max_len, "Invalid: %s explicitly rejected", failed_part->label);
                break;
            default:
                snprintf(dest_buffer, max_len, "Rollback occurred on slot %s", failed_part->label);
                break;
        }
    } else {
        snprintf(dest_buffer, max_len, "Rollback detected on slot %s (unknown state)", failed_part->label);
    }
}

static void ota_run_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting firmware upgrade transaction...");
    is_ota_handler_registered = false;


    char *dynamic_url = (char *)pvParameter;
    if (dynamic_url == NULL || strlen(dynamic_url) == 0) {
        ESP_LOGE(TAG, "Invalid or empty OTA URL target pointer passed.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Targeting server bin endpoint location: %s", dynamic_url);

    uint32_t saved_offset = 0;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_OTA_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        // Read previous progress offset. If it doesn't exist, saved_offset stays 0
        nvs_get_u32(nvs_handle, NVS_KEY_OFFSET, &saved_offset);
        if (saved_offset > 0) {
            ESP_LOGI(TAG, "Found incomplete download! Resuming from offset: %u bytes", saved_offset);
        }
    }
 

    esp_http_client_config_t client_cfg = {
        .url = dynamic_url,
        .skip_cert_common_name_check = true,
        // .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .event_handler = http_event_handler,
        .cert_pem = (const char *)ota_chaos_cert_pem_start,          
        .skip_cert_common_name_check = true,
        .timeout_ms = 15000, 
    };
    
    esp_https_ota_config_t ota_config = { 
        .http_config = &client_cfg,
        .partial_http_download = true,       // Enable Range Request tracking chunks
        .max_http_request_size = 4 * 1024,   // Download in 4KB chunks to optimize RAM footprint
        .ota_resumption = true,              // Tell core system this is a resumable attempt
        .ota_image_bytes_written = saved_offset // Tell core where to continue from
    };

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err == ESP_OK) {
        ESP_LOGI(TAG, "System default event loop created successfully.");
    } else if (loop_err == ESP_ERR_INVALID_STATE) {
        // This is exactly what happens if main() already initialized it!
        // The code prints this log and moves forward completely safely.
        ESP_LOGI(TAG, "System default event loop already running. Proceeding safely.");
    } else {
        // This only handles actual critical errors (like out-of-memory states)
        ESP_LOGE(TAG, "Failed to instantiate event loop framework: %s", esp_err_to_name(loop_err));
        vTaskDelete(NULL);
        return;
    }

    esp_err_t reg_err = esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, &ota_event_handler, NULL);
    if (reg_err == ESP_OK) {
        is_ota_handler_registered = true;
        ESP_LOGI(TAG, "OTA handler registered successfully.");
    } else {
        ESP_LOGE(TAG, "Critical error registering handler instance: %s", esp_err_to_name(reg_err));
        vTaskDelete(NULL);
        return;
    }

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK || ota_handle == NULL) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed; error code: %d", err);
        goto cleanup;

    }

    esp_app_desc_t app_desc;
    if (esp_https_ota_get_img_desc(ota_handle, &app_desc) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read image descriptor from stream.");
        esp_https_ota_abort(ota_handle);
        goto cleanup;

    }

    ESP_LOGI(TAG, "Processing App: %s | Version: %s", app_desc.project_name, app_desc.version);

    int last_percentage = -1;
    int chunk_counter = 0;

    while(1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break; // Complete or broke off due to error
        }

        // Fetch current cumulative bytes written (including previous attempts)
        int bytes_read = esp_https_ota_get_image_len_read(ota_handle);


        // if (total_file_size > 0 && bytes_read >= total_file_size) {
        //     ESP_LOGI(TAG, "All expected payload bytes downloaded (%d/%d). Breaking loop safely.", bytes_read, total_file_size);
        //     err = ESP_OK; // Force successful break status
        //     break;
        // }

        // Save progress to NVS periodically (e.g., every 10 loop cycles) to protect Flash endurance
        if (chunk_counter++ % 10 == 0) {
            nvs_set_u32(nvs_handle, NVS_KEY_OFFSET, (uint32_t)bytes_read);
            nvs_commit(nvs_handle);
        }

        // Print Progress Indicators bytes read / total file size * 100 = percentage.
        if (total_file_size > 0) {
            int percentage = (bytes_read * 100) / total_file_size;
            if (percentage != last_percentage) {
                ESP_LOGI(TAG, "OTA Progress: %d%% (%d / %d bytes)", percentage, bytes_read, total_file_size);
                last_percentage = percentage;
            }
        }

        // vTaskDelay(pdMS_TO_TICKS(100)); // Avoid tight loop
    }

    // after reason break loop, check if OTA was successful
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA stopped mid-download (Error: %d). Progress cached to NVS.", err);
        // Do NOT call abort here if you want to keep the partition data for next time!
        // Just cleanly exit the handle context
        esp_https_ota_abort(ota_handle); 
    } 
    else if (esp_https_ota_is_complete_data_received(ota_handle) != true) {
        ESP_LOGE(TAG, "File transmission incomplete.");
        esp_https_ota_abort(ota_handle);
    } 
    else {
        // Success path! Wipe NVS tracker token so next update starts cleanly from 0
        nvs_erase_key(nvs_handle, NVS_KEY_OFFSET);
        nvs_commit(nvs_handle);

        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Update completely verified! Rebooting now...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            nvs_close(nvs_handle);
            esp_restart();
        } else {
            ESP_LOGE(TAG, "Failed to apply boot flag. Error: %d", err);
        }
    }

cleanup:
    if (is_ota_handler_registered) {
        esp_event_handler_unregister(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, &ota_event_handler);
        ESP_LOGI(TAG, "OTA handler unregistered safely during cleanup.");
    }
    nvs_close(nvs_handle);
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


    resolve_rollback_reason(status->roll_back_reason, sizeof(status->roll_back_reason));

    return ESP_OK;
}
