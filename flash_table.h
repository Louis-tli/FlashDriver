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

/**
 * @brief  SW-048 (3단 폴백 tier 2): table 에 없는 부품의 임시 profile 을 만든다.
 *         JEDEC ID 의 밀도 byte 만 신뢰한다(log2(byte), 64 KB ~ 2 GB).
 *         16 MB 를 넘는 부품은 앞 16 MB 까지만 노출한다(3-byte 주소 전용).
 *         돌려받는 FLASH_INFO 에는 FLASH_FEAT_GENERIC 이 서 있고, read 는
 *         03h single 하나뿐이며 QE 는 없다.
 * @param  jedec_id: 9Fh 로 읽은 24-bit ID (vendor 는 아무 값이나 된다).
 * @return Pointer to const FLASH_INFO, or NULL if the density byte is not
 *         a meaningful 25-series density (0x10..0x1F 밖).
 */
const FLASH_INFO *Flash_FindGenericDevice(uint32_t jedec_id);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_TABLE_H__ */
