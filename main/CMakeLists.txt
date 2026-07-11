idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash wifi_ble_provisioning app_update config_portal_ap esp_driver_gpio esp_timer boardled
                       INCLUDE_DIRS "")