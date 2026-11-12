/******************************************************************************
 * File Name   : flash_table.h
 * Description : Flash Device Profiles and Model Lookup Table Header
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#ifndef __FLASH_TABLE_H__
#define __FLASH_TABLE_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * Device Resolution (SW-041)
 *
 * There is no flat table any more. A part is described by a vendor row
 * (manufacturer ID -> QE location, features, read profiles) plus the size
 * carried in the JEDEC ID's density byte. See the long comment in
 * flash_table.c for the full reasoning.
 ******************************************************************************/

/* The active part is described by one RAM record (28 bytes) that every
 * Flash_FindDevice() call refills. The pointer returned by Flash_FindDevice()
 * or Flash_GetInfo() is therefore valid only until the next lookup. */

/******************************************************************************
 * Lookup API
 ******************************************************************************/

/**
 * @brief  Find Flash model descriptor by 24-bit JEDEC ID.
 * @param  jedec_id: 24-bit ID from 9Fh command (e.g. 0xEF4019).
 * @return Pointer to const FLASH_INFO if found, or NULL if unsupported.
 */
const FLASH_INFO *Flash_FindDevice(uint32_t jedec_id);

/**
 * @brief  Find Flash model descriptor by FLASH_MODEL.
 *         SW-041: FLASH_MODEL is a JEDEC ID, so this is Flash_FindDevice()
 *         with a readability alias. FLASH_MODEL_UNKNOWN returns NULL.
 * @return Pointer to const FLASH_INFO if found, or NULL.
 */
const FLASH_INFO *Flash_FindDeviceByModel(FLASH_MODEL model_id);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_TABLE_H__ */
