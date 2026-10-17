/******************************************************************************
 * File Name   : flash_easy.c
 * Description : Simple "it just works" Flash API (SW-013)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * Thin, robust wrappers over the full API in flash.c. They choose the best
 * read / program mode automatically and never require the caller to think
 * about read units, alignment, or option bitmasks.
 ******************************************************************************/

#include "flash_easy.h"

/******************************************************************************
 * Internal: pick the fastest read mode the active device supports.
 ******************************************************************************/
static FLASH_READ_MODE Flash_EasyBestReadMode(const FLASH_INFO *info)
{
    /* Quad I/O is fastest; fall back through dual to single-width fast read. */
    if ((info->features & FLASH_FEAT_QUAD) != 0U) {
        return FLASH_READ_QUAD_IO;
    }
    if ((info->features & FLASH_FEAT_DUAL) != 0U) {
        return FLASH_READ_DUAL_IO;
    }
    return FLASH_READ_FAST;
}

/******************************************************************************
 * Public Easy API
 ******************************************************************************/
int Flash_ReadBytes(uint32_t addr, void *buf, uint32_t len)
{
    const FLASH_INFO *info = Flash_GetInfo();

    if ((info == NULL) || (buf == NULL)) {
        return FLASH_ERR_PARAM;
    }
    if (len == 0U) {
        return FLASH_OK;
    }

    const FLASH_READ_MODE mode = Flash_EasyBestReadMode(info);

    /* Dual / Quad reads must go through the SFC data path. */
    uint32_t opt = FLASH_READ_OPT_NONE;
    if ((mode != FLASH_READ_SINGLE) && (mode != FLASH_READ_FAST)) {
        opt |= FLASH_READ_OPT_SFC;
    }

    /* Byte read unit => any address / length / buffer alignment works. */
    int status = Flash_Read(addr, buf, len, mode, FLASH_READ_UNIT_BYTE, opt);
    if (status != FLASH_OK) {
        /* Robust fallback: direct single-width fast read always works. */
        status = Flash_Read(addr, buf, len, FLASH_READ_FAST,
                           FLASH_READ_UNIT_BYTE, FLASH_READ_OPT_NONE);
    }
    return status;
}

int Flash_WriteBytes(uint32_t addr, const void *data, uint32_t len)
{
    const FLASH_INFO *info = Flash_GetInfo();

    if ((info == NULL) || (data == NULL)) {
        return FLASH_ERR_PARAM;
    }
    if (len == 0U) {
        return FLASH_OK;
    }

    const FLASH_PROGRAM_MODE mode = ((info->features & FLASH_FEAT_QUAD) != 0U)
                                    ? FLASH_PROGRAM_QUAD : FLASH_PROGRAM_SINGLE;

    /* Flash_Write already splits at page boundaries and clears write-protect. */
    return Flash_Write(addr, data, len, mode);
}

int Flash_EraseRange(uint32_t addr, uint32_t len)
{
    const FLASH_INFO *info = Flash_GetInfo();

    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }
    if (len == 0U) {
        return FLASH_OK;
    }

    const uint32_t sector = info->sector_size;
    uint32_t start = addr - (addr % sector);                 /* round down */
    uint32_t end   = addr + len;
    end = ((end + sector - 1U) / sector) * sector;           /* round up   */

    for (uint32_t a = start; a < end; a += sector) {
        const int status = Flash_Erase(a, FLASH_ERASE_SECTOR);
        if (status != FLASH_OK) {
            return status;
        }
    }
    return FLASH_OK;
}

int Flash_EraseChip(void)
{
    return Flash_Erase(0U, FLASH_ERASE_CHIP);
}

uint32_t Flash_SizeBytes(void)
{
    const FLASH_INFO *info = Flash_GetInfo();
    return (info != NULL) ? info->flash_size : 0U;
}

uint32_t Flash_SectorSize(void)
{
    const FLASH_INFO *info = Flash_GetInfo();
    return (info != NULL) ? info->sector_size : 0U;
}
