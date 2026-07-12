#include "ota_app.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "boardled.h"

static const char *TAG = "PORTAL_ENGINE";

#define NVS_WIFI_NAMESPACE "wifi_store"
#define NVS_KEY_SSID       "wifi_ssid"
#define NVS_KEY_PASS       "wifi_pass"

// Embedded HTML UI template mappings
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Retry wifi counter
uint8_t s_retry_num = 0;
#define MAXIMUM_RETRY 5

// Global buffer passed safely to your external module
char global_ota_url_buffer[256] = {0};

// forwared declareation 
static void load_credentials_and_connect(void);


static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    // === STATION MODE EVENTS ===
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi Station layer initialized.");
        load_credentials_and_connect();
        // esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "Station disconnected from router! Reason code: %d", event->reason);
        
        // Handle password or association failures gracefully
        if (event->reason == WIFI_REASON_AUTH_EXPIRE || 
            event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            event->reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGE(TAG, "Failed to join network. Please check credentials via the portal dashboard.");
        }

        if (s_retry_num < MAXIMUM_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Attempting connection to local AP (%d/%d)", s_retry_num, MAXIMUM_RETRY);
            } else {
                ESP_LOGE(TAG, "Connection attempts exhausted. Keeping Access Portal open for corrections...");
            } 
        // Optional: Trigger a reconnection retry attempt if desired
        // esp_wifi_connect();
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Station successfully assigned IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Device internet link active. Safe to trigger OTA download actions.");
        s_retry_num = 0;
    }
    
    // === ACCESS POINT (AP) MODE EVENTS ===
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "User device connected to portal hotspot! MAC: " MACSTR " (AID: %d)", 
                 MAC2STR(event->mac), event->aid);
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "User device disconnected from portal hotspot. MAC: " MACSTR " (AID: %d)", 
                 MAC2STR(event->mac), event->aid);
    }
}

/* Helper: Automatically checks NVS key storage profile definitions and connects */
static void load_credentials_and_connect(void)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No pre-existing Wi-Fi key parameters defined in NVS yet.");
        return;
    }

    wifi_config_t wifi_sta_config = {0};
    size_t ssid_size = sizeof(wifi_sta_config.sta.ssid);
    size_t pass_size = sizeof(wifi_sta_config.sta.password);

    esp_err_t err_ssid = nvs_get_str(my_handle, NVS_KEY_SSID, (char *)wifi_sta_config.sta.ssid, &ssid_size);
    esp_err_t err_pass = nvs_get_str(my_handle, NVS_KEY_PASS, (char *)wifi_sta_config.sta.password, &pass_size);
    nvs_close(my_handle);

    if (err_ssid == ESP_OK && err_pass == ESP_OK) {
        ESP_LOGI(TAG, "Found stored keys. Initiating auto-connection to SSID: %s", wifi_sta_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
}

/* HTTP GET Handler: Serves the control console index page */
static esp_err_t root_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, (index_html_end - index_html_start));
}

/* HTTP POST Handler: Parses router configurations and caches variables to keys */
static esp_err_t wifi_provision_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "pass");

    if (ssid && pass) {
        nvs_handle_t my_handle;
        if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_str(my_handle, NVS_KEY_SSID, ssid->valuestring);
            nvs_set_str(my_handle, NVS_KEY_PASS, pass->valuestring);
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGI(TAG, "Saved network properties to NVS registry namespaces.");
        }

        wifi_config_t wifi_sta_config = {0};
        strlcpy((char *)wifi_sta_config.sta.ssid, ssid->valuestring, sizeof(wifi_sta_config.sta.ssid));
        strlcpy((char *)wifi_sta_config.sta.password, pass->valuestring, sizeof(wifi_sta_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

        s_retry_num = 0;

        esp_wifi_connect();
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"status\":\"saved_and_connecting\"}");
    return ESP_OK;
}

/* HTTP POST Handler: Intercepts target URLs and triggers your external task loop */
static esp_err_t trigger_ota_post_handler(httpd_req_t *req)
{
    int ret = httpd_req_recv(req, global_ota_url_buffer, MIN(req->content_len, sizeof(global_ota_url_buffer) - 1));
    if (ret <= 0) return ESP_FAIL;
    global_ota_url_buffer[ret] = '\0';

    // Enforcement Check: Require .bin file target structure
    if (!strstr(global_ota_url_buffer, ".bin")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid parameter tracking layout. Target missing file suffix extension .bin");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web UI triggered action button. Invoking external module firmware pipeline link: %s", global_ota_url_buffer);
    httpd_resp_sendstr(req, "{\"status\":\"ota_task_spawned\"}");

    /* 
     * 🚀 FORWARD THREAD ASSIGNMENT:
     * Dispatches runtime process ownership straight to your provided task handling logic.
     */
    ota_perform_task();

    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    my_ota_status_t sys_status;
    
    if (my_ota_get_system_status(&sys_status) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Telemetry Error");
        return ESP_FAIL;
    }

    char json_response[512]; 
    snprintf(json_response, sizeof(json_response),
             "{\"version\":\"%s\",\"partition\":\"%s\",\"boot_reason\":\"%s\",\"rollback_reason\":\"%s\"}",
             sys_status.version, sys_status.partition, sys_status.last_boot, sys_status.roll_back_reason);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

/* Component Core Activation Portal Sequence */
void ota_engine_init_portal(void)
{
    /* 
     * NVS HEALTH CHECK: 
     * Verify that app_main successfully deployed NVS flash pools before initializing routes.
     */
    nvs_stats_t nvs_stats;
    esp_err_t nvs_check = nvs_get_stats(NULL, &nvs_stats);
    
    if (nvs_check == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGE(TAG, "FATAL: NVS Flash is not initialized inside the main application layer!");
        return;
    } else if (nvs_check != ESP_OK) {
        ESP_LOGE(TAG, "NVS status verification query error code: %s", esp_err_to_name(nvs_check));
        return;
    }

    ESP_LOGI(TAG, "NVS integration validation healthy. Launching tracking interfaces...");
    ESP_ERROR_CHECK(esp_netif_init()); 
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    
    check_active_firmware();


    /* 
     * EARLY DIAGNOSTIC INTEGRATION:
     * Invokes your external verification utility to audit health metrics.
     */

    // // 1. Establish structural network attachments using robust AP+STA configuration
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        wifi_country_t country_config = {
        .cc = "ID",                          // Indonesia country code
        .schan = 1,                          // Start at channel 1
        .nchan = 13,                         // Support up to channel 13
        .max_tx_power = 20,                  
        .policy = WIFI_COUNTRY_POLICY_AUTO   // Auto-hop SoftAP channel to match STA
    };
    
    esp_err_t err = esp_wifi_set_country(&country_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Indonesian country config: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "ESP32C3_Portal",
            .ssid_len = strlen("ESP32C3_Portal"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // used to prevent SoftAP gone after connected to STA
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // 2. Start internal web service routes
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_html_get_handler };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t wifi_uri = { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_provision_post_handler };
        httpd_register_uri_handler(server, &wifi_uri);

        httpd_uri_t ota_uri = { .uri = "/api/ota", .method = HTTP_POST, .handler = trigger_ota_post_handler };
        httpd_register_uri_handler(server, &ota_uri);

        httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_register_uri_handler(server, &status_uri);
        ESP_LOGI(TAG, "HTTP Server active on http://192.168.4.1");
    }

    // 3. Process checking structural key database configurations on bootup 
    
}

