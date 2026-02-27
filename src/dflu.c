#include "dflu/dflu.h"
#include "dflu/dflu_crc32.h"
#include <string.h>

#define DFLU_MAGIC_HEADER  (0x44464C55u) // "DFLU"
#define DFLU_MAGIC_CTRL    (0x434F4E54u) // "CONT"
// Control area: append-only records in one erase block (no in-place overwrite).
// Slots: A/B. Header is written once after slot erase.
typedef struct
{
    uint32_t magic;        // DFLU_MAGIC_CTRL
    uint32_t seq;          // monotonically increasing
    uint32_t active_slot;  // 0=A, 1=B, 0xFFFFFFFF=none
    uint32_t pending_slot; // 0=A, 1=B, 0xFFFFFFFF=none
    uint32_t crc32;        // CRC of fields above (excluding crc32)
} dflu_ctrl_record_t;

// Slot header at the start of each slot
typedef struct
{
    uint32_t magic;        // DFLU_MAGIC_HEADER
    uint32_t header_size;  // sizeof(dflu_slot_header_t)
    uint32_t image_size;   // payload bytes
    uint32_t image_crc32;  // CRC32(payload)
    uint32_t version;
    uint32_t build_id;
    uint32_t reserved[2];
} dflu_slot_header_t;

static uint32_t align_down(uint32_t v, uint32_t a) { return (v / a) * a; }
static bool is_aligned(uint32_t v, uint32_t a) { return (a != 0u) && ((v % a) == 0u); }

static uint32_t slot_addr(const dflu_ctx_t *ctx, dflu_slot_t slot)
{
    return (slot == DFLU_SLOT_A) ? ctx->slot_a_addr : ctx->slot_b_addr;
}

static dflu_status_t port_read(const dflu_ctx_t *ctx, uint32_t addr, void *dst, size_t len)
{
    return ctx->port.read(addr, dst, len) ? DFLU_OK : DFLU_ERR_PORT;
}

static dflu_status_t port_erase(const dflu_ctx_t *ctx, uint32_t addr, size_t len)
{
    if (!is_aligned(addr, ctx->port.info.erase_block_size) || !is_aligned((uint32_t)len, ctx->port.info.erase_block_size))
        return DFLU_ERR_NOT_ALIGNED;
    return ctx->port.erase(addr, len) ? DFLU_OK : DFLU_ERR_PORT;
}

static dflu_status_t port_program(const dflu_ctx_t *ctx, uint32_t addr, const void *src, size_t len)
{
    if (!is_aligned(addr, ctx->port.info.program_unit) || !is_aligned((uint32_t)len, ctx->port.info.program_unit))
        return DFLU_ERR_NOT_ALIGNED;
    return ctx->port.program(addr, src, len) ? DFLU_OK : DFLU_ERR_PORT;
}

static uint32_t crc32_struct(const void *data, size_t len)
{
    uint32_t c = dflu_crc32_init();
    c = dflu_crc32_update(c, data, len);
    return dflu_crc32_finalize(c);
}

static dflu_status_t read_slot_header(dflu_ctx_t *ctx, dflu_slot_t slot, dflu_slot_header_t *out)
{
    uint32_t addr = slot_addr(ctx, slot);
    dflu_status_t st = port_read(ctx, addr, out, sizeof(*out));
    if (st != DFLU_OK) return st;
    if (out->magic != DFLU_MAGIC_HEADER) return DFLU_ERR_NO_VALID_IMAGE;
    if (out->header_size != sizeof(dflu_slot_header_t)) return DFLU_ERR_NO_VALID_IMAGE;
    if (out->image_size > ctx->layout.max_payload_size) return DFLU_ERR_NO_VALID_IMAGE;
    return DFLU_OK;
}

static dflu_status_t verify_slot_internal(dflu_ctx_t *ctx, dflu_slot_t slot)
{
    dflu_slot_header_t hdr;
    dflu_status_t st = read_slot_header(ctx, slot, &hdr);
    if (st != DFLU_OK) return st;

    uint32_t payload_addr = slot_addr(ctx, slot) + (uint32_t)sizeof(dflu_slot_header_t);
    uint32_t remaining = hdr.image_size;

    uint8_t buf[256]; // fixed-size buffer, deterministic
    uint32_t crc = dflu_crc32_init();

    while (remaining > 0u)
    {
        uint32_t chunk = (remaining > sizeof(buf)) ? (uint32_t)sizeof(buf) : remaining;
        st = port_read(ctx, payload_addr, buf, chunk);
        if (st != DFLU_OK) return st;

        crc = dflu_crc32_update(crc, buf, chunk);
        payload_addr += chunk;
        remaining -= chunk;
    }

    crc = dflu_crc32_finalize(crc);
    return (crc == hdr.image_crc32) ? DFLU_OK : DFLU_ERR_VERIFY_FAILED;
}

// Control page record scan: find best valid record by highest seq.
static dflu_status_t ctrl_find_latest(dflu_ctx_t *ctx, dflu_ctrl_record_t *out_rec, bool *out_found)
{
    *out_found = false;
    dflu_ctrl_record_t best = {0};
    uint32_t best_seq = 0;

    const uint32_t ctrl_addr = ctx->control_addr;
    const uint32_t block = ctx->port.info.erase_block_size;

    // We store records sequentially inside one erase block.
    // Each record is fixed size; we scan until we hit 0xFFFFFFFF (erased).
    const uint32_t max_records = block / (uint32_t)sizeof(dflu_ctrl_record_t);

    for (uint32_t i = 0; i < max_records; i++)
    {
        dflu_ctrl_record_t r;
        uint32_t addr = ctrl_addr + i * (uint32_t)sizeof(dflu_ctrl_record_t);
        dflu_status_t st = port_read(ctx, addr, &r, sizeof(r));
        if (st != DFLU_OK) return st;

        // Erased record?
        if (r.magic == 0xFFFFFFFFu && r.seq == 0xFFFFFFFFu) break;

        if (r.magic != DFLU_MAGIC_CTRL) continue;

        uint32_t crc_expected = r.crc32;
        r.crc32 = 0u;
        uint32_t crc_calc = crc32_struct(&r, sizeof(r));
        if (crc_calc != crc_expected) continue;

        if (!(*out_found) || r.seq > best_seq)
        {
            best = r;
            best_seq = r.seq;
            *out_found = true;
        }
    }

    if (*out_found) *out_rec = best;
    return DFLU_OK;
}

static dflu_status_t ctrl_append_record(dflu_ctx_t *ctx, const dflu_ctrl_record_t *rec)
{
    // Find next free slot in control block; if full, erase control block then write record at start.
    const uint32_t ctrl_addr = ctx->control_addr;
    const uint32_t block = ctx->port.info.erase_block_size;
    const uint32_t max_records = block / (uint32_t)sizeof(dflu_ctrl_record_t);

    for (uint32_t i = 0; i < max_records; i++)
    {
        dflu_ctrl_record_t r;
        uint32_t addr = ctrl_addr + i * (uint32_t)sizeof(dflu_ctrl_record_t);
        dflu_status_t st = port_read(ctx, addr, &r, sizeof(r));
        if (st != DFLU_OK) return st;

        if (r.magic == 0xFFFFFFFFu && r.seq == 0xFFFFFFFFu)
        {
            // free spot
            return port_program(ctx, addr, rec, sizeof(*rec));
        }
    }

    // No free record: erase control block and write at start
    dflu_status_t st = port_erase(ctx, ctrl_addr, block);
    if (st != DFLU_OK) return st;
    return port_program(ctx, ctrl_addr, rec, sizeof(*rec));
}

static dflu_status_t ctrl_write(dflu_ctx_t *ctx, uint32_t active_slot, uint32_t pending_slot)
{
    dflu_ctrl_record_t latest;
    bool found = false;
    dflu_status_t st = ctrl_find_latest(ctx, &latest, &found);
    if (st != DFLU_OK) return st;

    dflu_ctrl_record_t rec;
    memset(&rec, 0xFF, sizeof(rec)); // helps if fields are not written fully by platform
    rec.magic = DFLU_MAGIC_CTRL;
    rec.seq = found ? (latest.seq + 1u) : 1u;
    rec.active_slot = active_slot;
    rec.pending_slot = pending_slot;
    rec.crc32 = 0u;
    rec.crc32 = crc32_struct(&rec, sizeof(rec));

    return ctrl_append_record(ctx, &rec);
}

static dflu_status_t ctrl_read_state(dflu_ctx_t *ctx, uint32_t *active, uint32_t *pending, bool *found)
{
    dflu_ctrl_record_t rec;
    dflu_status_t st = ctrl_find_latest(ctx, &rec, found);
    if (st != DFLU_OK) return st;

    if (!(*found))
    {
        *active = 0xFFFFFFFFu;
        *pending = 0xFFFFFFFFu;
        return DFLU_OK;
    }

    *active = rec.active_slot;
    *pending = rec.pending_slot;
    return DFLU_OK;
}

dflu_status_t dflu_init(dflu_ctx_t *ctx, const dflu_port_t *port, uint32_t slot_size)
{
    if (!ctx || !port || !port->read || !port->erase || !port->program) return DFLU_ERR_INVALID_ARG;
    if (slot_size == 0u) return DFLU_ERR_INVALID_ARG;

    memset(ctx, 0, sizeof(*ctx));
    ctx->port = *port;

    const uint32_t block = ctx->port.info.erase_block_size;
    if (block == 0u) return DFLU_ERR_LAYOUT;

    // Region must hold: control block + 2 slots
    const uint32_t required = block + 2u * slot_size;
    if (ctx->port.info.flash_region_size < required) return DFLU_ERR_LAYOUT;

    // Slot must fit header + payload and be aligned to erase block for clean erase.
    if (!is_aligned(slot_size, block)) return DFLU_ERR_LAYOUT;

    ctx->layout.slot_size = slot_size;
    ctx->layout.max_payload_size = slot_size - (uint32_t)sizeof(dflu_slot_header_t);

    ctx->control_addr = ctx->port.info.flash_base_addr;
    ctx->slot_a_addr = ctx->control_addr + block;
    ctx->slot_b_addr = ctx->slot_a_addr + slot_size;

    ctx->write_in_progress = false;
    return DFLU_OK;
}

dflu_status_t dflu_get_active_slot(dflu_ctx_t *ctx, dflu_slot_t *out_slot)
{
    if (!ctx || !out_slot) return DFLU_ERR_INVALID_ARG;

    uint32_t active, pending;
    bool found = false;
    dflu_status_t st = ctrl_read_state(ctx, &active, &pending, &found);
    if (st != DFLU_OK) return st;

    // If control not set, choose any valid image with preference A then B.
    if (!found || active == 0xFFFFFFFFu)
    {
        if (verify_slot_internal(ctx, DFLU_SLOT_A) == DFLU_OK) { *out_slot = DFLU_SLOT_A; return DFLU_OK; }
        if (verify_slot_internal(ctx, DFLU_SLOT_B) == DFLU_OK) { *out_slot = DFLU_SLOT_B; return DFLU_OK; }
        return DFLU_ERR_NO_VALID_IMAGE;
    }

    if (active > 1u) return DFLU_ERR_NO_VALID_IMAGE;
    if (verify_slot_internal(ctx, (dflu_slot_t)active) != DFLU_OK) return DFLU_ERR_NO_VALID_IMAGE;

    *out_slot = (dflu_slot_t)active;
    return DFLU_OK;
}

dflu_status_t dflu_get_active_image_info(dflu_ctx_t *ctx, dflu_image_info_t *out_info)
{
    if (!ctx || !out_info) return DFLU_ERR_INVALID_ARG;

    dflu_slot_t slot;
    dflu_status_t st = dflu_get_active_slot(ctx, &slot);
    if (st != DFLU_OK) return st;

    dflu_slot_header_t hdr;
    st = read_slot_header(ctx, slot, &hdr);
    if (st != DFLU_OK) return st;

    out_info->image_size = hdr.image_size;
    out_info->image_crc32 = hdr.image_crc32;
    out_info->version = hdr.version;
    out_info->build_id = hdr.build_id;
    return DFLU_OK;
}

dflu_status_t dflu_boot_process(dflu_ctx_t *ctx)
{
    if (!ctx) return DFLU_ERR_INVALID_ARG;

    uint32_t active, pending;
    bool found = false;
    dflu_status_t st = ctrl_read_state(ctx, &active, &pending, &found);
    if (st != DFLU_OK) return st;

    if (!found || pending == 0xFFFFFFFFu) return DFLU_OK;
    if (pending > 1u) return DFLU_OK;

    // If pending verifies, make it active and clear pending.
    if (verify_slot_internal(ctx, (dflu_slot_t)pending) == DFLU_OK)
    {
        return ctrl_write(ctx, pending, 0xFFFFFFFFu);
    }

    // Pending invalid -> clear pending, keep active unchanged.
    return ctrl_write(ctx, active, 0xFFFFFFFFu);
}

dflu_status_t dflu_begin_update(dflu_ctx_t *ctx, uint32_t image_size, uint32_t version, uint32_t build_id)
{
    if (!ctx) return DFLU_ERR_INVALID_ARG;
    if (ctx->write_in_progress) return DFLU_ERR_STATE;
    if (image_size == 0u || image_size > ctx->layout.max_payload_size) return DFLU_ERR_INVALID_ARG;

    // Determine inactive slot based on current active slot.
    dflu_slot_t active_slot;
    dflu_status_t st = dflu_get_active_slot(ctx, &active_slot);
    dflu_slot_t target = DFLU_SLOT_A;

    if (st == DFLU_OK)
        target = (active_slot == DFLU_SLOT_A) ? DFLU_SLOT_B : DFLU_SLOT_A;
    else
        target = DFLU_SLOT_A; // if none valid, write A first

    // Erase target slot fully.
    uint32_t addr = slot_addr(ctx, target);
    st = port_erase(ctx, addr, ctx->layout.slot_size);
    if (st != DFLU_OK) return st;

    // Start session.
    ctx->write_in_progress = true;
    ctx->write_slot = target;
    ctx->write_offset = 0u;
    ctx->running_crc = dflu_crc32_init();
    ctx->declared_size = image_size;

    // Store version/build_id later in header at end (after CRC known).
    ctx->declared_version = version;
    ctx->declared_build_id = build_id;

    return DFLU_OK;
}

dflu_status_t dflu_write_chunk(dflu_ctx_t *ctx, const void *data, size_t len)
{
    if (!ctx || (!data && len > 0u)) return DFLU_ERR_INVALID_ARG;
    if (!ctx->write_in_progress) return DFLU_ERR_STATE;
    if ((uint32_t)len > (ctx->declared_size - ctx->write_offset)) return DFLU_ERR_INVALID_ARG;

    // Program into payload region (after header).
    uint32_t base = slot_addr(ctx, ctx->write_slot) + (uint32_t)sizeof(dflu_slot_header_t);
    uint32_t addr = base + ctx->write_offset;

    // Program must follow program_unit. If len isn't aligned, we require caller to chunk properly.
    dflu_status_t st = port_program(ctx, addr, data, len);
    if (st != DFLU_OK) return st;

    ctx->running_crc = dflu_crc32_update(ctx->running_crc, data, len);
    ctx->write_offset += (uint32_t)len;
    return DFLU_OK;
}

dflu_status_t dflu_end_update(dflu_ctx_t *ctx)
{
    if (!ctx) return DFLU_ERR_INVALID_ARG;
    if (!ctx->write_in_progress) return DFLU_ERR_STATE;
    if (ctx->write_offset != ctx->declared_size) return DFLU_ERR_STATE;

    // Finalize CRC.
    uint32_t crc = dflu_crc32_finalize(ctx->running_crc);

    // Write header at beginning of slot (one-shot after erase).
    dflu_slot_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = DFLU_MAGIC_HEADER;
    hdr.header_size = sizeof(dflu_slot_header_t);
    hdr.image_size = ctx->declared_size;
    hdr.image_crc32 = crc;
    hdr.version = ctx->declared_version;
    hdr.build_id = ctx->declared_build_id;

    dflu_status_t st = port_program(ctx, slot_addr(ctx, ctx->write_slot), &hdr, sizeof(hdr));
    if (st != DFLU_OK) return st;

    // Verify written slot by reading back + CRC over flash.
    st = verify_slot_internal(ctx, ctx->write_slot);
    if (st != DFLU_OK) return st;

    // Mark as pending in control page. Activation happens at next boot_process.
    uint32_t active, pending;
    bool found = false;
    st = ctrl_read_state(ctx, &active, &pending, &found);
    if (st != DFLU_OK) return st;

    st = ctrl_write(ctx, active, (uint32_t)ctx->write_slot);
    if (st != DFLU_OK) return st;

    // End session.
    ctx->write_in_progress = false;
    ctx->write_offset = 0u;
    ctx->declared_size = 0u;
    ctx->running_crc = 0u;

    return DFLU_OK;
}

dflu_status_t dflu_verify_slot(dflu_ctx_t *ctx, dflu_slot_t slot)
{
    if (!ctx) return DFLU_ERR_INVALID_ARG;
    if (slot != DFLU_SLOT_A && slot != DFLU_SLOT_B) return DFLU_ERR_INVALID_ARG;
    return verify_slot_internal(ctx, slot);
}