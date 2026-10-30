/******************************************************************************
 * File Name   : flash_xfer.c
 * Description : Shared transfer geometry (unit resolution, size math, alignment)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_xfer.h"

int Flash_XferResolve(FLASH_READ_UNIT unit, uint32_t count,
                      const void *buf, uint32_t addr,
                      uint32_t *p_unit_bytes, uint32_t *p_total_bytes)
{
    if ((p_unit_bytes == NULL) || (p_total_bytes == NULL)) {
        return FLASH_ERR_PARAM;
    }

    /* 1. Read-unit mapping (single source of truth for both transfer paths) */
    uint32_t unit_bytes;
    switch (unit) {
        case FLASH_READ_UNIT_DEFAULT:
        case FLASH_READ_UNIT_WORD:
            unit_bytes = 4U;
            break;
        case FLASH_READ_UNIT_HALFWORD:
            unit_bytes = 2U;
            break;
        case FLASH_READ_UNIT_BYTE:
            unit_bytes = 1U;
            break;
        default:
            return FLASH_ERR_PARAM;
    }

    /* 2. Argument sanity */
    if ((buf == NULL) || (count == 0U)) {
        return FLASH_ERR_PARAM;
    }

    /* 3. count * unit must not wrap a 32-bit size */
    if (count > (UINT32_MAX / unit_bytes)) {
        return FLASH_ERR_PARAM;
    }

    /* 4. Cortex-M0 hard-fault guard: unaligned access is not recoverable */
    if (!Flash_IsAligned((uintptr_t)buf, unit_bytes) ||
        !Flash_IsAligned(addr, unit_bytes)) {
        return FLASH_ERR_ALIGNMENT;
    }

    *p_unit_bytes  = unit_bytes;
    *p_total_bytes = count * unit_bytes;
    return FLASH_OK;
}
