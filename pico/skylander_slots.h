#pragma once
#ifndef SKYLANDER_SLOTS_H
#define SKYLANDER_SLOTS_H

#include <stdint.h>
#include <stdbool.h>
#include "kaos_protocol.h"

#define MAX_SLOTS 2

typedef struct {
    bool    loaded;
    bool    active;
    bool    dirty;          /* game has written to this slot since last sync */
    uint8_t data[SKYLANDER_DUMP_SIZE];
    uint8_t uid[4];
} slot_t;

extern slot_t g_slots[MAX_SLOTS];

void    slots_load(uint8_t slot, const uint8_t *dump_1024);
void    slots_unload(uint8_t slot);
uint8_t slots_portal_status(void);
uint8_t *slots_get_block(uint8_t slot, uint8_t block);
void    slots_write_block(uint8_t slot, uint8_t block, const uint8_t *data);

#endif
