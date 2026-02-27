#pragma once
#include <stdint.h>
#include <stddef.h>

// Standard CRC-32 (Ethernet/ZIP): poly 0x04C11DB7, init 0xFFFFFFFF, xorout 0xFFFFFFFF, reflected.
uint32_t dflu_crc32_init(void);
uint32_t dflu_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t dflu_crc32_finalize(uint32_t crc);