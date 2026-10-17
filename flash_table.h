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
 * Supported Flash Device Count & Index Order (SW-013: fixed table order)
 ******************************************************************************/
typedef enum {
    FLASH_IDX_W25Q128JW,    /* 0xEF6018U */
    FLASH_IDX_W25Q512JV,    /* 0xEF4020U */
    FLASH_IDX_W25Q256JV,    /* 0xEF4019U */
    FLASH_IDX_W25M512,      /* 0xEF6119U */
    FLASH_IDX_MX25L512,     /* 0xC22010U */
    FLASH_IDX_MX25L512MB,   /* 0xC2201AU */
    FLASH_IDX_GD25LQ64E,    /* 0xC86017U */
    FLASH_IDX_MAX
} FLASH_IDX;

#define FLASH_TABLE_ENTRIES_COUNT   (FLASH_IDX_MAX)

/******************************************************************************
 * ROM Model Table
 ******************************************************************************/
extern const FLASH_INFO g_flash_table[FLASH_TABLE_ENTRIES_COUNT];

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
 * @brief  Find Flash model descriptor by FLASH_MODEL (selects an exact variant
 *         when several devices could share a JEDEC ID).
 * @param  model_id: FLASH_MODEL enum value.
 * @return Pointer to const FLASH_INFO if found, or NULL.
 */
const FLASH_INFO *Flash_FindDeviceByModel(FLASH_MODEL model_id);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_TABLE_H__ */
