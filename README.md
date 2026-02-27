# Deterministic Flash Update & Integrity Framework for ARM Cortex-M (DFLU)

A small, portable, deterministic A/B-slot flash update library for Cortex-M systems.

---

## Design Goals

- Deterministic behavior (no dynamic allocation)
- Portable via minimal flash port abstraction
- Power-fail tolerant update flow
- Append-only control record scheme (no in-place overwrite)
- CRC32 integrity verification of payload stored in flash

---

## Architecture Overview

- One erase block reserved as control area
- Two equal-sized slots (A / B)
- Each slot contains:
  - Fixed-size header
  - Payload region

Control records are written append-only within a single erase block to avoid partial overwrite corruption.

---

## Update Flow

1. Erase inactive slot completely  
2. Stream payload into slot  
3. Finalize CRC32  
4. Write slot header (single write after erase)  
5. Verify payload from flash  
6. Mark slot as `pending`  
7. On next boot:
   - If pending verifies → promote to `active`
   - Otherwise → keep previous active image  

---

## Image Metadata

Each slot header stores:

- `image_size`
- `image_crc32`
- `version`
- `build_id`

Metadata is written once after payload programming.

---

## Porting Interface

Platform must implement:

```c
bool read(uint32_t addr, void *dst, size_t len);
bool erase(uint32_t addr, size_t len);
bool program(uint32_t addr, const void *src, size_t len);
```

### Constraints

- `erase()` must operate on erase-block aligned regions  
- `program()` must follow platform program unit alignment  
- Flash region must contain:
  - 1 control block  
  - 2 equal-sized slots  

See: `include/dflu/dflu_port.h`

---

## Properties

- No heap usage  
- Fixed memory usage  
- Deterministic execution  
- Flash verification before activation  
- Safe recovery from interrupted updates  
