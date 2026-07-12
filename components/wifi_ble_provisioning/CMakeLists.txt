idf_component_register(SRCS "wifi_ble_provisioning.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bt esp_wifi protocomm nvs_flash wifi_provisioning esp_system )
