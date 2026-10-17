/******************************************************************************
 * File Name   : flash_crc.h
 * Description : Software CRC / Checksum algorithms (SW-013: selectable at init)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#ifndef __FLASH_CRC_H__
#define __FLASH_CRC_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initial accumulator value for the given algorithm. */
uint32_t Flash_CrcInit(FLASH_CRC_ALGO algo);

/** @brief Fold len bytes of data into the running CRC. */
uint32_t Flash_CrcUpdate(FLASH_CRC_ALGO algo, uint32_t crc, const uint8_t *data, uint32_t len);

/** @brief Finalize the CRC (final xor / reflection post-processing). */
uint32_t Flash_CrcFinal(FLASH_CRC_ALGO algo, uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_CRC_H__ */
