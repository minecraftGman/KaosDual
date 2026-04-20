/*
 * SkylanderCrypt.c
 * Sector key derivation + AES-128 encrypt/decrypt for Skylander NFC dumps.
 * Pure C, no platform dependencies.
 */
#include "SkylanderCrypt.h"
#include "rijndael.h"
#include <string.h>

/*
 * The key derivation mixes the 4-byte tag UID with a per-sector constant.
 * Sector 0 uses the factory MIFARE key; sectors 1-15 use the Skylander key.
 */
static const uint8_t g_key_const_a[6] = { 0x4b, 0x0b, 0x20, 0x10, 0x7c, 0xcb };
static const uint8_t g_sector0_key[6] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 };

void generate_sector_key(const uint8_t uid[4], uint8_t sector, uint8_t key_out[16]) {
    const uint8_t *base = (sector == 0) ? g_sector0_key : g_key_const_a;
    for (int i = 0; i < 6; i++)
        key_out[i] = base[i] ^ uid[i % 4];
    key_out[6]  = uid[0];
    key_out[7]  = uid[1];
    key_out[8]  = uid[2];
    key_out[9]  = uid[3];
    key_out[10] = (uint8_t)(sector | (sector << 4));
    key_out[11] = uid[0];
    key_out[12] = uid[1];
    key_out[13] = uid[2];
    key_out[14] = uid[3];
    key_out[15] = (uint8_t)(sector | (sector << 4));
}

static void aes_decrypt_block(uint8_t *block, const uint8_t *key) {
    uint32_t rk[4 * 11];
    int Nr = rijndaelKeySetupDec(rk, key, 128);
    uint8_t out[16];
    rijndaelDecrypt(rk, Nr, block, out);
    memcpy(block, out, 16);
}

static void aes_encrypt_block(uint8_t *block, const uint8_t *key) {
    uint32_t rk[4 * 11];
    int Nr = rijndaelKeySetupEnc(rk, key, 128);
    uint8_t out[16];
    rijndaelEncrypt(rk, Nr, block, out);
    memcpy(block, out, 16);
}

void decrypt_skylander(uint8_t *data, const uint8_t uid[4]) {
    for (int sector = 0; sector < SKYLANDER_SECTOR_COUNT; sector++) {
        uint8_t key[16];
        generate_sector_key(uid, (uint8_t)sector, key);
        for (int blk = 0; blk < 3; blk++) {       /* skip trailer block (blk 3) */
            int abs = sector * SKYLANDER_BLOCKS_PER_SECTOR + blk;
            if (abs == 0) continue;                /* manufacturer block, skip */
            aes_decrypt_block(&data[abs * SKYLANDER_BLOCK_SIZE], key);
        }
    }
}

void encrypt_skylander(uint8_t *data, const uint8_t uid[4]) {
    for (int sector = 0; sector < SKYLANDER_SECTOR_COUNT; sector++) {
        uint8_t key[16];
        generate_sector_key(uid, (uint8_t)sector, key);
        for (int blk = 0; blk < 3; blk++) {
            int abs = sector * SKYLANDER_BLOCKS_PER_SECTOR + blk;
            if (abs == 0) continue;
            aes_encrypt_block(&data[abs * SKYLANDER_BLOCK_SIZE], key);
        }
    }
}
