/******************************************************************************
 * File Name   : flash_crc.c
 * Description : Software CRC / Checksum algorithms (SW-013: selectable at init)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * Bitwise implementations (small code, no lookup tables).
 ******************************************************************************/

#include "flash_crc.h"

uint32_t Flash_CrcInit(FLASH_CRC_ALGO algo)
{
    switch (algo) {
        case FLASH_CRC8:         return 0x00000000U;
        case FLASH_CRC16:        return 0x00000000U;
        case FLASH_CRC16_CCITT:  return 0x0000FFFFU;
        case FLASH_CRC32:        return 0xFFFFFFFFU;
        case FLASH_CRC_CHECKSUM: return 0x00000000U;
        default:                 return 0x00000000U;
    }
}

uint32_t Flash_CrcUpdate(FLASH_CRC_ALGO algo, uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (data == NULL) {
        return crc;
    }

    for (uint32_t i = 0U; i < len; i++) {
        const uint8_t b = data[i];

        switch (algo) {
            case FLASH_CRC8:
                crc ^= (uint32_t)b;
                for (uint32_t bit = 0U; bit < 8U; bit++) {
                    crc = (crc & 0x80U) ? ((crc << 1) ^ 0x07U) : (crc << 1);
                }
                crc &= 0xFFU;
                break;

            case FLASH_CRC16:
                crc ^= ((uint32_t)b << 8);
                for (uint32_t bit = 0U; bit < 8U; bit++) {
                    crc = (crc & 0x8000U) ? ((crc << 1) ^ 0x8005U) : (crc << 1);
                }
                crc &= 0xFFFFU;
                break;

            case FLASH_CRC16_CCITT:
                crc ^= ((uint32_t)b << 8);
                for (uint32_t bit = 0U; bit < 8U; bit++) {
                    crc = (crc & 0x8000U) ? ((crc << 1) ^ 0x1021U) : (crc << 1);
                }
                crc &= 0xFFFFU;
                break;

            case FLASH_CRC32:
                crc ^= (uint32_t)b;
                for (uint32_t bit = 0U; bit < 8U; bit++) {
                    crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
                }
                break;

            case FLASH_CRC_CHECKSUM:
            default:
                crc = (crc + (uint32_t)b) & 0xFFU;
                break;
        }
    }

    return crc;
}

uint32_t Flash_CrcFinal(FLASH_CRC_ALGO algo, uint32_t crc)
{
    if (algo == FLASH_CRC32) {
        return crc ^ 0xFFFFFFFFU;
    }
    return crc;
}
