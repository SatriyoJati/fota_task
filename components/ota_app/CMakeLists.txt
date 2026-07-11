idf_component_register(SRCS "ota_app.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update 
                        esp_http_client 
                        esp_https_ota 
                        nvs_flash
                        esp_wifi)
