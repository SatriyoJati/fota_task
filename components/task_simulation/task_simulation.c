#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "ota_app.h"
#include "wifi_stack.h"
#include "cJSON.h"

static const char *TAG = "MAIN";

/* MQTT Topic Definitions */
#define TOPIC_OTA_TRIGGER    "device/ota/trigger"
#define TOPIC_SYS_STATUS     "device/status/boot"
#define TOPIC_DATA_STREAM    "device/data/stream"
extern char global_ota_url_buffer[256];
/* Simple Queue Packet for passing the Trigger String */
typedef struct {
    char command[32];  // Command string (e.g., "START_UPDATE")
    char url[256];
} OtaMessage_t;

static QueueHandle_t xOtaQueue = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

/* Helper to decode boot reasons */
const char* get_reset_reason_string(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SOFTWARE_RESET";
        case ESP_RST_PANIC:     return "EXCEPTION_PANIC";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        default:                return "OTHER_REASON";
    }
}

/* MQTT Event Callback Manager */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Connected. Subscribing to: %s", TOPIC_OTA_TRIGGER);
            mqtt_connected = true;
            esp_mqtt_client_subscribe(event->client, TOPIC_OTA_TRIGGER, 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA:
            /* Route target message payloads to the OTA Task Queue */
            if (strncmp(event->topic, TOPIC_OTA_TRIGGER, event->topic_len) == 0) {
                OtaMessage_t msg;
                char *payload = malloc(event->data_len + 1);
                strncpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';

                cJSON *root = cJSON_Parse(payload);
            if (root != NULL) {
                // 3. Extract a specific key (e.g., "temperature")
                cJSON *url = cJSON_GetObjectItem(root, "url");
                
                if (url != NULL && cJSON_IsString(url)) {
                    const char *url_string = url->valuestring;
                    ESP_LOGI("MQTT", "Received URL: %s", url_string);
                    strncpy(msg.url, url_string, sizeof(msg.url) - 1);
                    msg.url[sizeof(msg.url) - 1] = '\0';
                }

                cJSON *command = cJSON_GetObjectItem(root, "command");
                if (command != NULL && cJSON_IsString(command)) {   
                    const char *command_string = command->valuestring;
                    ESP_LOGI("MQTT", "Received Command: %s", command_string);
                    strncpy(msg.command, command_string, sizeof(msg.command) - 1);
                    msg.command[sizeof(msg.command) - 1] = '\0'; // Ensure null-termination
                    strncpy(global_ota_url_buffer, msg.url, sizeof(global_ota_url_buffer) - 1);
                    global_ota_url_buffer[sizeof(global_ota_url_buffer) - 1] = '\0'; // Ensure null-termination
                } else {
                    ESP_LOGE("MQTT", "Command key missing or not a string.");
                }

                // 4. Clean up the cJSON object
                cJSON_Delete(root);
                } else {
                    ESP_LOGE("MQTT", "Failed to parse JSON. Error at: %s", cJSON_GetErrorPtr());
                }

                free(payload);
                if (xOtaQueue != NULL) {
                    xQueueSend(xOtaQueue, &msg, 0);
                }
                break;
                
            }
            break;
        default:
            break;
    }
}

/* ---- TASK 1: OTA & Configuration Manager Task ---- */
static void vOtaConfigTask(void *pvParameters) {
    ESP_LOGI("OTA_TASK", "Task initialized and listening for queue triggers...");
    OtaMessage_t received_msg;

    for (;;) {
        /* Topic 1 Trigger: Blocks indefinitely until a payload hits the queue */
        if (xQueueReceive(xOtaQueue, &received_msg, portMAX_DELAY) == pdPASS) {
            ESP_LOGW("OTA_TASK", "Received trigger command string: [%s]", received_msg.command);
            
            if (strcmp(received_msg.command, "START_UPDATE") == 0) {
                ESP_LOGE("OTA_TASK", "--- CRITICAL ACTION REQUIRED: Triggering actual OTA sequence now! ---");
                
                ota_perform_task();
            } else {
                ESP_LOGI("OTA_TASK", "Ignored unexpected payload value.");
            }
        }
    }
}

/* ---- TASK 2: Telemetry / Status Reporting Task ---- */
static void vTelemetryTask(void *pvParameters) {
    int data_counter = 0;
    char buffer[48];

    for (;;) {
        if (mqtt_connected) {
            data_counter++;
            snprintf(buffer, sizeof(buffer), "{\"sensor_data\": %d}", data_counter);
            
            /* Topic 3: Publish standard data streams */
            esp_mqtt_client_publish(mqtt_client, TOPIC_DATA_STREAM, buffer, 0, 1, 0);
            ESP_LOGI("TELEMETRY", "Published data stream payload.");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---- TASK 3: System Monitor Task ---- */
static void vMonitorTask(void *pvParameters) {
    char status_buffer[96];

    /* Fetch current partition details and reset maps */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char* reason_str = get_reset_reason_string(esp_reset_reason());

    snprintf(status_buffer, sizeof(status_buffer), 
             "{\"running_partition\": \"%s\", \"boot_reason\": \"%s\"}", 
             running->label, reason_str);

    for (;;) {
        if (mqtt_connected) {
            /* Topic 2: Periodically emit critical metadata variables */
            esp_mqtt_client_publish(mqtt_client, TOPIC_SYS_STATUS, status_buffer, 0, 1, 1);
            ESP_LOGI("MONITOR", "Published firmware status summary block.");
            vTaskDelay(pdMS_TO_TICKS(30000)); 
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}


void run_simulation_tasks(void) {
   ESP_LOGI(TAG, "Initializing Task Infrastructure...");

    xOtaQueue = xQueueCreate(2, sizeof(OtaMessage_t));
    if (xOtaQueue == NULL) {
        ESP_LOGE(TAG, "Queue creation failed!");
        return;
    }
    wifi_stack_connect();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.hivemq.com:1883", 
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    /* Spawn parallel execution kernels */
    xTaskCreatePinnedToCore(vMonitorTask,   "MonitorTask",   3072, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(vTelemetryTask, "TelemetryTask", 3072, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(vOtaConfigTask, "OtaConfigTask", 8192, NULL, 4, NULL, 0); // Stack bumped to 8KB for actual network handling
}