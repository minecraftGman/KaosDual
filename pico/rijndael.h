#pragma once
#ifndef RIJNDAEL_H
#define RIJNDAEL_H

#include <stdint.h>

int  rijndaelKeySetupEnc(uint32_t rk[], const uint8_t cipherKey[], int keyBits);
int  rijndaelKeySetupDec(uint32_t rk[], const uint8_t cipherKey[], int keyBits);
void rijndaelEncrypt(const uint32_t rk[], int Nr, const uint8_t pt[16], uint8_t ct[16]);
void rijndaelDecrypt(const uint32_t rk[], int Nr, const uint8_t ct[16], uint8_t pt[16]);

#endif
