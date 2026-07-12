#ifndef OTA_APP_H
#define OTA_APP_H
#include "esp_err.h"

typedef struct {
    char version[32];       // Embedded Firmware Version
    char partition[16];     // Active Partition Label (e.g., "factory", "ota_0")
    char last_boot[48];   // Self-diagnostic boot result or rollback status
} my_ota_status_t;

void check_active_firmware(void);

void ota_perform_task(void);

void trigger_factory_reset(void);

esp_err_t my_ota_get_system_status(my_ota_status_t *status);

#endif /* OTA_APP_H */