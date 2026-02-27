#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
{
    // Inclusive start address of the flash region used by this library.
    uint32_t flash_base_addr;

    // Total bytes available in the region.
    uint32_t flash_region_size;

    // Erase page/sector size in bytes.
    uint32_t erase_block_size;

    // Minimum programmable unit in bytes (e.g., 4 or 8). If unknown, use 4.
    uint32_t program_unit;
} dflu_flash_info_t;

typedef struct
{
    dflu_flash_info_t info;

    // Read from absolute flash address into dst.
    bool (*read)(uint32_t addr, void *dst, size_t len);

    // Erase one or more erase blocks starting at absolute addr.
    // addr and len must be aligned to erase_block_size.
    bool (*erase)(uint32_t addr, size_t len);

    // Program bytes to absolute addr.
    // addr and len must respect program_unit requirements.
    bool (*program)(uint32_t addr, const void *src, size_t len);
} dflu_port_t;