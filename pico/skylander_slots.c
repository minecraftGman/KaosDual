#include "skylander_slots.h"
#include "SkylanderCrypt.h"
#include <string.h>

slot_t g_slots[MAX_SLOTS] = {0};

void slots_load(uint8_t slot, const uint8_t *dump) {
    if (slot >= MAX_SLOTS) return;
    slot_t *s = &g_slots[slot];
    memcpy(s->data, dump, SKYLANDER_DUMP_SIZE);
    memcpy(s->uid, dump, 4);          /* UID is first 4 bytes of block 0 */
    decrypt_skylander(s->data, s->uid);
    s->loaded = true;
    s->active = true;
    s->dirty  = false;
}

void slots_unload(uint8_t slot) {
    if (slot >= MAX_SLOTS) return;
    memset(&g_slots[slot], 0, sizeof(slot_t));
}

uint8_t slots_portal_status(void) {
    uint8_t st = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].loaded && g_slots[i].active) st |= (1 << i);
    return st;
}

uint8_t *slots_get_block(uint8_t slot, uint8_t block) {
    if (slot >= MAX_SLOTS || !g_slots[slot].loaded) return NULL;
    if (block >= SKYLANDER_DUMP_SIZE / 16) return NULL;
    return &g_slots[slot].data[block * 16];
}

void slots_write_block(uint8_t slot, uint8_t block, const uint8_t *data) {
    if (slot >= MAX_SLOTS || !g_slots[slot].loaded) return;
    if (block >= SKYLANDER_DUMP_SIZE / 16) return;
    memcpy(&g_slots[slot].data[block * 16], data, 16);
    g_slots[slot].dirty = true;
}
