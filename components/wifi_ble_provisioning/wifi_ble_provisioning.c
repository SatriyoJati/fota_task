#include <stdio.h>
#include "wifi_ble_provisioning.h"
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
// #include "esp_app_trace.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
// #include "esp_app_trace.h"
#include "esp_timer.h"
#include "esp_log.h"


static const char *TAG = "WIFIPROVISIONING";

const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

void wifi_prov_eb (void *user_data, wifi_prov_cb_event_t event, void *event_data) {
    switch (event)
    {
    case WIFI_PROV_SET_STA_CONFIG:
        ESP_LOGI(TAG,"WIFI PROVISION SET STA CONFIG");
        /* code */
        break;
    
    default:
        break;
    }
}


static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id)
        {
        case WIFI_PROV_START:
            /* code */
            ESP_LOGI(TAG, "Provisioning started");
            break;
        
        case WIFI_PROV_CRED_RECV :
            ESP_LOGI(TAG, "Received Credential");
            break;

        case WIFI_PROV_CRED_FAIL :
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning fail!\n\tReason: %s""\n\tDo something!", reason);
            // esp_err_t  ret = wifi_prov_mgr_reset_sm_state_on_failure();
            // ESP_LOGI(TAG, "Restart Provision after failing: %d", ret);
            // esp_err_t  ret = wifi_prov_mgr_reset_sm_state_on_failure();
            // wifi_prov_mgr_reset_provisioning();
            break;

        case WIFI_PROV_CRED_SUCCESS :
            ESP_LOGI(TAG, "Provisioning success");
            // wifi_prov_mgr_stop_provisioning();
            break;
        
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "End of Provisioning");
            // wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                /* code */
                ESP_LOGI(TAG, "Connecting to the AP...");
                esp_wifi_connect();
                break;
            
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
                esp_wifi_connect();
                break;
                
            default:
                break;
        }
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        switch (event_id) 
        {
            case PROTOCOMM_TRANSPORT_BLE_CONNECTED :
                ESP_LOGI(TAG, "BLE Transport : Connected!");
                break;
            
            case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED :
                ESP_LOGI(TAG, "BLE Transport : Disconnected!");
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
}

wifi_prov_event_handler_t wifi_prov_event_handler = {
    .event_cb = wifi_prov_eb,
    .user_data = NULL
};

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}


void start_wifi_provisioning(void)
{
    uint8_t custom_service_uuid[] = {
    /* LSB <---------------------------------------
        * ---------------------------------------> MSB */
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };

    esp_err_t ret = nvs_flash_init();


    if (ret ==  ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());

        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_prov_conn_cfg_t wifi_prov_conn_cfg = {
        .wifi_conn_attempts = 1
    };

    wifi_prov_mgr_config_t init_confg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM ,
        .app_event_handler = wifi_prov_event_handler,
        .wifi_prov_conn_cfg = wifi_prov_conn_cfg
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(init_confg));
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {

        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
        const char *pop = "abcd1234";
        wifi_prov_security1_params_t *sec_params = pop;
        const char *service_key = NULL;

        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));

        wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);

        // wifi_prov_mgr_disable_auto_stop(4000);

        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, (const void *) sec_params, service_name, service_key));

    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
        /* Start Wi-Fi station */
        wifi_init_sta();
    }
    // xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

    while (1) {
        // blink_led();
        /* Toggle the LED state */
        
        // ESP_LOGI(TAG, "Main Task Running!");
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
