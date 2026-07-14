idf_component_register(SRCS "wifi_stack.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi nvs_flash)
