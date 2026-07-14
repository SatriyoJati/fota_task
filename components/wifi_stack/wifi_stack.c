#include <stdio.h>
#include "wifi_stack.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"

static int s_retry_num = 0;
#define MAX_RETRY_CONNECT 5

#define NVS_WIFI_NAMESPACE "wifi_store"
#define NVS_KEY_SSID       "wifi_ssid"
#define NVS_KEY_PASS       "wifi_pass"

#define TAG "WIFI_STACK"
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY_CONNECT) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


static void load_creds_and_connect(void)
{
    esp_err_t ret = ESP_OK;
    nvs_handle_t my_handle;

    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No pre-existing Wi-Fi key parameters defined in NVS yet.");
    }

    wifi_config_t wifi_sta_config = {0};
    size_t ssid_size = sizeof(wifi_sta_config.sta.ssid);
    size_t pass_size = sizeof(wifi_sta_config.sta.password);

    esp_err_t err_ssid = nvs_get_str(my_handle, NVS_KEY_SSID, (char *)wifi_sta_config.sta.ssid, &ssid_size);
    esp_err_t err_pass = nvs_get_str(my_handle, NVS_KEY_PASS, (char *)wifi_sta_config.sta.password, &pass_size);
    nvs_close(my_handle);

    if (err_ssid == ESP_OK && err_pass == ESP_OK) {
        ESP_LOGI(TAG, "Found stored keys. Initiating auto-connection to SSID: %s", wifi_sta_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 (const char *)wifi_sta_config.sta.ssid, (const char *)wifi_sta_config.sta.password);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 (const char *)wifi_sta_config.sta.ssid, (const char *)wifi_sta_config.sta.password);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

esp_err_t wifi_stack_connect(void)
{
    esp_err_t ret = ESP_OK;
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init()); 
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

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

    load_creds_and_connect();

    return ret;
}
