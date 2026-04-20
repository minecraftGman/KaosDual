#pragma once
#ifndef SKYLANDER_H
#define SKYLANDER_H

#include <stdint.h>
#include <stdbool.h>
#include "SkylanderCrypt.h"

#define MAX_SKYLANDERS  2

typedef struct {
    bool    loaded;
    bool    active;
    uint8_t data[SKYLANDER_DUMP_SIZE];
    uint8_t uid[4];
    char    filename[256];
} skylander_slot_t;

extern skylander_slot_t g_skylanders[MAX_SKYLANDERS];

bool    skylander_load(uint8_t slot, const char *path);
void    skylander_unload(uint8_t slot);
uint8_t skylander_get_portal_status(void);
uint8_t *skylander_get_block(uint8_t slot, uint8_t block);
void    skylander_write_block(uint8_t slot, uint8_t block, const uint8_t *data);

#endif
