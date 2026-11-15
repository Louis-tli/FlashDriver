/******************************************************************************
 * File Name   : flash_driver.h
 * Description : Simple "it just works" Flash API for end users (SW-013)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * This is the RECOMMENDED entry point. It hides every mode / unit / option
 * detail behind automatic choices. A complete example:
 *
 *     #include "flash_driver.h"
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

#ifndef __FLASH_DRV_H__
#define __FLASH_DRV_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  `addr` 에서 `buf` 로 `len` byte 를 읽는다.
 *         칩이 지원하는 가장 빠른 read 를 자동으로 고르고, 주소·길이·buffer
 *         정렬에 관계없이 동작한다.
 */
int Flash_ReadBytes(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief  `data` 의 `len` byte 를 `addr` 에 program 한다.
 *         Page 경계 분할을 처리하고 write protection 도 자동으로 해제한다.
 *         (대상 영역은 먼저 erase 되어 있어야 한다.)
 */
int Flash_WriteBytes(uint32_t addr, const void *data, uint32_t len);

/**
 * @brief  [addr, addr+len) 을 덮는 4KB sector 를 모두 erase 한다.
 *         주소·길이는 자유롭게 줄 수 있다(sector 경계로 반올림).
 */
int Flash_EraseRange(uint32_t addr, uint32_t len);

/**
 * @brief  `addr` 이 속한 4KB sector 를 erase 한다.
 */
int Flash_EraseSector(uint32_t addr);

/**
 * @brief  Erase the entire chip.
 */
int Flash_EraseChip(void);

/**
 * @brief  전체 flash 용량(byte). 초기화 전이면 0.
 */
uint32_t Flash_SizeBytes(void);

/**
 * @brief  sector(erase) 크기(byte). 초기화 전이면 0.
 */
uint32_t Flash_SectorSize(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_DRV_H__ */
