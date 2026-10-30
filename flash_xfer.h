/******************************************************************************
 * File Name   : flash_xfer.h
 * Description : Shared transfer geometry (unit resolution, size math, alignment)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
 *
 * SW-016: Flash_Read (SPI path) and Flash_SFC_DMARead (SFC path) had identical
 *   copies of the read-unit mapping, the NULL/count checks and the Cortex-M0
 *   alignment check. Both now resolve their geometry through this one module.
 ******************************************************************************/

#ifndef __FLASH_XFER_H__
#define __FLASH_XFER_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Resolve a read request into (unit size, total bytes) and validate it.
 *         Performs, in this order:
 *           1. read-unit mapping (DEFAULT/WORD -> 4, HALFWORD -> 2, BYTE -> 1)
 *           2. NULL buffer / zero count rejection
 *           3. count * unit overflow rejection (32-bit)
 *           4. Cortex-M0 alignment check of both the buffer and the address
 * @param  unit:         FLASH_READ_UNIT requested by the caller.
 * @param  count:        Number of access units.
 * @param  buf:          Destination buffer (NULL is rejected).
 * @param  addr:         Flash address (must be aligned to the unit size).
 * @param  p_unit_bytes: Out: resolved unit size (1, 2 or 4).
 * @param  p_total_bytes:Out: count * unit size (overflow checked).
 * @return FLASH_OK, FLASH_ERR_PARAM or FLASH_ERR_ALIGNMENT.
 */
int Flash_XferResolve(FLASH_READ_UNIT unit, uint32_t count,
                      const void *buf, uint32_t addr,
                      uint32_t *p_unit_bytes, uint32_t *p_total_bytes);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_XFER_H__ */
