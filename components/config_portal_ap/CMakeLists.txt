idf_component_register(
    SRCS "config_portal_ap.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi 
            esp_event 
            nvs_flash 
            lwip 
            esp_http_server 
            json
            ota_app
            boardled
    EMBED_FILES
        "${PROJECT_DIR}/web/index.html" # Traverses out to fetch and embed the static HTML binary
)