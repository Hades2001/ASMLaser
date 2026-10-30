#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs_flash.h"
#include "driver.h"
#include "grbl/settings.h"
#include "grbl/nvs_buffer.h"
#include "nvs.h"
#include "factory_init.h"
#include "factory_settings.h"

bool factory_initialize (bool (*sound)(void))
{
    nvs_handle_t handle;
    uint32_t initialized = 0;
    esp_err_t err = nvs_open("machine_factory", NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        printf("FACTORY: marker open failed %d\n", err);
        return false;
    }
    err = nvs_get_u32(handle, "initialized", &initialized);
    if(err == ESP_OK && initialized == 2) {
        nvs_close(handle);
        printf("FACTORY: already initialized; preserving settings\n");
        return true;
    }
    // Revision 1 was a development profile with truncated steps/mm inputs.
    bool ok = err == ESP_ERR_NVS_NOT_FOUND || (err == ESP_OK && initialized == 1);
    if(!ok) {
        printf("FACTORY: invalid marker %d/%lu\n", err, (unsigned long)initialized);
        nvs_close(handle);
        return false;
    }
    printf("FACTORY: first initialization; 3 second tone\n");
    if(!(ok = sound()))
        goto done;

    for(unsigned i = 0; i < sizeof(factory_settings) / sizeof(factory_settings[0]); i++) {
        char value[32];
        snprintf(value, sizeof(value), "%s", factory_settings[i].value);
        status_code_t status = settings_store_setting(factory_settings[i].id, value);
        if(status != Status_OK) {
            printf("FACTORY: setting $%u failed: %u\n", factory_settings[i].id, status);
            ok = false;
            goto done;
        }
    }
    for(unsigned i = 0; i < N_STARTUP_LINE; i++) {
        char empty[] = "";
        settings_write_startup_line(i, empty);
    }
    // Commit Grbl data before the separate completion marker. A power loss
    // before marker commit leaves initialization retryable on the next boot.
    nvs_buffer_sync_physical();
    uint8_t *stored = malloc(hal.nvs.size), *expected = malloc(hal.nvs.size);
    ok = stored && expected && !settings_dirty.is_dirty && nvsRead(stored) &&
         hal.nvs.memcpy_from_nvs(expected, 0, hal.nvs.size, false) == NVS_TransferResult_OK &&
         memcmp(stored, expected, hal.nvs.size) == 0;
    free(stored);
    free(expected);
    if(ok)
        ok = nvs_set_u32(handle, "initialized", 2) == ESP_OK && nvs_commit(handle) == ESP_OK;
done:
    nvs_close(handle);
    printf("FACTORY: %s\n", ok ? "parameters verified; initialization complete" : "failed; startup blocked, retry next boot");
    return ok;
}
