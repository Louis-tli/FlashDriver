/******************************************************************************
 * File Name   : flash_easy.h
 * Description : Simple "it just works" Flash API for end users (SW-013)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * This is the RECOMMENDED entry point. It hides every mode / unit / option
 * detail behind automatic choices. A complete example:
 *
 *     #include "flash_easy.h"
 *
 *     void app_start(void)
 *     {
 *         if (Flash_BootInit(0x00000000U) != FLASH_OK) {
 *             return;                       // no / wrong chip -> safe stop
 *         }
 *
 *         uint8_t buf[256];
 *         Flash_ReadBytes(0x1000U, buf, sizeof(buf));      // read any range
 *
 *         Flash_EraseRange(0x2000U, 4096U);                // erase a region
 *         Flash_WriteBytes(0x2000U, buf, sizeof(buf));     // program data
 *     }
 *
 * For advanced control (specific read mode, CRC, SFC tuning, protection),
 * use the full API in flash.h.
 ******************************************************************************/

#ifndef __FLASH_EASY_H__
#define __FLASH_EASY_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Read `len` bytes from `addr` into `buf`.
 *         Automatically picks the fastest read the chip supports and works
 *         with any address / length / buffer alignment.
 */
int Flash_ReadBytes(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief  Program `len` bytes from `data` to `addr`.
 *         Handles page-boundary splitting and clears write-protection
 *         automatically. (Flash must be erased at the target first.)
 */
int Flash_WriteBytes(uint32_t addr, const void *data, uint32_t len);

/**
 * @brief  Erase every 4KB sector covering [addr, addr+len).
 *         Accepts any address / length (rounded to sector boundaries).
 */
int Flash_EraseRange(uint32_t addr, uint32_t len);

/**
 * @brief  Erase the entire chip.
 */
int Flash_EraseChip(void);

/**
 * @brief  Total flash density in bytes (0 if not initialized).
 */
uint32_t Flash_SizeBytes(void);

/**
 * @brief  Sector (erase) size in bytes (0 if not initialized).
 */
uint32_t Flash_SectorSize(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_EASY_H__ */
