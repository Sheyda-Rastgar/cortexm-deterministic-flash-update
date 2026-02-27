#include "dflu/dflu_crc32.h"
#include <stdint.h>

static uint32_t crc32_table[256];
static int table_ready = 0;

static void crc32_make_table(void)
{
    const uint32_t poly = 0xEDB88320u; // reflected 0x04C11DB7
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (uint32_t j = 0; j < 8; j++)
        {
            c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    table_ready = 1;
}

uint32_t dflu_crc32_init(void)
{
    if (!table_ready) crc32_make_table();
    return 0xFFFFFFFFu;
}

uint32_t dflu_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
    {
        uint8_t idx = (uint8_t)((crc ^ p[i]) & 0xFFu);
        crc = crc32_table[idx] ^ (crc >> 8);
    }
    return crc;
}

uint32_t dflu_crc32_finalize(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}