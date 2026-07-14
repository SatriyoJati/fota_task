idf_component_register(SRCS "task_simulation.c"
                    INCLUDE_DIRS "include"
                    REQUIRES 
                    mqtt            # Required for esp_mqtt_client APIs
                    app_update      # Required for esp_ota_ops APIs
                    esp_partition   # Required for reading active boot slots
                    ota_app
                    wifi_stack
                    json
                    )
