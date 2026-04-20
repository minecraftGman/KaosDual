#pragma once
#ifndef SKYLANDER_CRYPT_H
#define SKYLANDER_CRYPT_H

#include <stdint.h>

#define SKYLANDER_BLOCK_SIZE      16
#define SKYLANDER_SECTOR_COUNT    16
#define SKYLANDER_BLOCKS_PER_SECTOR 4
#define SKYLANDER_DUMP_SIZE       1024   /* 64 blocks * 16 bytes */

void generate_sector_key(const uint8_t uid[4], uint8_t sector, uint8_t key_out[16]);
void decrypt_skylander(uint8_t *data, const uint8_t uid[4]);
void encrypt_skylander(uint8_t *data, const uint8_t uid[4]);

#endif
