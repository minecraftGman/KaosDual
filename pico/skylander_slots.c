#include "skylander_slots.h"
#include "SkylanderCrypt.h"
#include <string.h>

slot_t g_slots[MAX_SLOTS] = {0};

void slots_load(uint8_t slot, const uint8_t *dump) {
    if (slot >= MAX_SLOTS) return;
    slot_t *s = &g_slots[slot];
    memcpy(s->data, dump, SKYLANDER_DUMP_SIZE);
    /* Cache the 7 actual UID bytes (skipping BCC0 at byte 3) */
    s->uid[0] = s->data[0];
    s->uid[1] = s->data[1];
    s->uid[2] = s->data[2];
    /* data[3] is BCC0 — skip */
    s->uid[3] = s->data[4];
    s->uid[4] = s->data[5];
    s->uid[5] = s->data[6];
    s->uid[6] = s->data[7];
    s->loaded = true;
    s->active = true;
    s->dirty  = false;

    /* If another loaded slot has the same UID, patch this slot's UID
     * in-memory only (never written back to SPIFFS).
     *
     * MIFARE Ultralight 7-byte UID layout in block 0 (16 bytes):
     *   byte 0: UID0
     *   byte 1: UID1
     *   byte 2: UID2
     *   byte 3: BCC0 = 0x88 ^ UID0 ^ UID1 ^ UID2
     *   byte 4: UID3
     *   byte 5: UID4
     *   byte 6: UID5
     *   byte 7: UID6  ← patch this
     *   byte 8: BCC1 = UID3 ^ UID4 ^ UID5 ^ UID6
     */
    for (int other = 0; other < MAX_SLOTS; other++) {
        if (other == slot || !g_slots[other].loaded) continue;
        if (memcmp(g_slots[other].uid, s->uid, 7) == 0) {
            s->data[7]++;
            s->uid[6] = s->data[7];
            s->data[8] = s->data[4] ^ s->data[5] ^ s->data[6] ^ s->data[7];
        }
    }
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
