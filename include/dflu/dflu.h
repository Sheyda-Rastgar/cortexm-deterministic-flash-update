#ifndef DFLU_DFLU_H_
#define DFLU_DFLU_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dflu_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DFLU — Deterministic Flash Update Library
 *
 * Dual-slot (A/B) update mechanism for Cortex-M systems.
 * No dynamic allocation. Designed for memory-constrained targets.
 */

typedef enum
{
    DFLU_OK = 0,
    DFLU_ERR_INVALID_ARG,
    DFLU_ERR_PORT,
    DFLU_ERR_LAYOUT,
    DFLU_ERR_NO_VALID_IMAGE,
    DFLU_ERR_VERIFY_FAILED,
    DFLU_ERR_NOT_ALIGNED,
    DFLU_ERR_STATE
} dflu_status_t;

typedef enum
{
    DFLU_SLOT_A = 0,
    DFLU_SLOT_B = 1
} dflu_slot_t;

/* Slot layout information */
typedef struct
{
    uint32_t slot_size;         /* Total slot bytes including header */
    uint32_t max_payload_size;  /* slot_size - header */
} dflu_layout_t;

/* Image metadata (from slot header) */
typedef struct
{
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t version;
    uint32_t build_id;
} dflu_image_info_t;

/* Runtime context (no heap usage) */
typedef struct
{
    dflu_port_t port;
    dflu_layout_t layout;

    uint32_t control_addr;
    uint32_t slot_a_addr;
    uint32_t slot_b_addr;

    bool write_in_progress;
    dflu_slot_t write_slot;
    uint32_t write_offset;
    uint32_t running_crc;
    uint32_t declared_size;

    uint32_t declared_version;
    uint32_t declared_build_id;
} dflu_ctx_t;

/* Initialization */
dflu_status_t dflu_init(dflu_ctx_t *ctx, const dflu_port_t *port, uint32_t slot_size);
dflu_status_t dflu_boot_process(dflu_ctx_t *ctx);

/* Active image */
dflu_status_t dflu_get_active_slot(dflu_ctx_t *ctx, dflu_slot_t *out_slot);
dflu_status_t dflu_get_active_image_info(dflu_ctx_t *ctx, dflu_image_info_t *out_info);

/* Update flow */
dflu_status_t dflu_begin_update(dflu_ctx_t *ctx,
                                uint32_t image_size,
                                uint32_t version,
                                uint32_t build_id);

dflu_status_t dflu_write_chunk(dflu_ctx_t *ctx,
                               const void *data,
                               size_t len);

dflu_status_t dflu_end_update(dflu_ctx_t *ctx);

/* Verification */
dflu_status_t dflu_verify_slot(dflu_ctx_t *ctx, dflu_slot_t slot);

#ifdef __cplusplus
}
#endif

#endif /* DFLU_DFLU_H_ */