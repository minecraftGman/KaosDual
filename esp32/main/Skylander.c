#include "Skylander.h"
#include "SkylanderCrypt.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "Skylander";

skylander_slot_t g_skylanders[MAX_SKYLANDERS] = {0};

bool skylander_load(uint8_t slot, const char *path) {
    if (slot >= MAX_SKYLANDERS) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open: %s", path); return false; }

    skylander_slot_t *sky = &g_skylanders[slot];
    memset(sky->data, 0, SKYLANDER_DUMP_SIZE);
    size_t n = fread(sky->data, 1, SKYLANDER_DUMP_SIZE, f);
    fclose(f);
    if (n == 0) { ESP_LOGE(TAG, "Empty file: %s", path); return false; }

    memcpy(sky->uid, sky->data, 4);
    decrypt_skylander(sky->data, sky->uid);
    strncpy(sky->filename, path, sizeof(sky->filename)-1);
    sky->loaded = true;
    sky->active = true;

    ESP_LOGI(TAG, "Slot %d loaded: %s  UID=%02X:%02X:%02X:%02X",
             slot, path, sky->uid[0], sky->uid[1], sky->uid[2], sky->uid[3]);
    return true;
}

void skylander_unload(uint8_t slot) {
    if (slot >= MAX_SKYLANDERS) return;
    memset(&g_skylanders[slot], 0, sizeof(skylander_slot_t));
    ESP_LOGI(TAG, "Slot %d unloaded", slot);
}

uint8_t skylander_get_portal_status(void) {
    uint8_t s = 0;
    for (int i = 0; i < MAX_SKYLANDERS; i++)
        if (g_skylanders[i].loaded && g_skylanders[i].active) s |= (1 << i);
    return s;
}

uint8_t *skylander_get_block(uint8_t slot, uint8_t block) {
    if (slot >= MAX_SKYLANDERS || !g_skylanders[slot].loaded) return NULL;
    if (block >= SKYLANDER_DUMP_SIZE / 16) return NULL;
    return &g_skylanders[slot].data[block * 16];
}

void skylander_write_block(uint8_t slot, uint8_t block, const uint8_t *data) {
    if (slot >= MAX_SKYLANDERS || !g_skylanders[slot].loaded) return;
    if (block >= SKYLANDER_DUMP_SIZE / 16) return;
    memcpy(&g_skylanders[slot].data[block * 16], data, 16);
}
