/******************************************************************************
 * File Name   : flash_driver.c
 * Description : Simple "it just works" Flash API (SW-013)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * Thin, robust wrappers over the full API in flash.c. They choose the best
 * read / program mode automatically and never require the caller to think
 * about read units, alignment, or option bitmasks.
 ******************************************************************************/

#include "flash_driver.h"

/******************************************************************************
 * Internal: pick the fastest read mode the active device supports.
 ******************************************************************************/
static FLASH_READ_MODE Flash_GetReadMode(const FLASH_INFO *info)
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

    const FLASH_READ_MODE mode = Flash_GetReadMode(info);

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

    /* Flash_Write가 페이지 경계(256B)마다 나누어 쓴다.
     * Block protect(BP) 해제는 자동이 아니므로, 사용하려면 Unprotect가 선행 되어야 한다.
     * 안 하면 기기는 쓰기를 무시하고 FLASH_OK가 돌아온다 */
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

    /* Erase whole sectors that cover [addr, addr+len) */
    const uint32_t sector = FLASH_SECTOR_SIZE;
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

int Flash_EraseSector(uint32_t addr)
{
    const FLASH_INFO *info = Flash_GetInfo();
    uint32_t sector_addr = 0U;

    /* 용량 밖 주소는 버스 트래픽 없이 거절 (SW-036 규칙과 동일) */
    if ((info == NULL) || (addr >= info->flash_size)) {
        return FLASH_ERR_PARAM;
    }

    /* 섹터 경계로 내림: 같은 섹터 안의 어떤 주소를 주어도 그 섹터를 지운다.
     * FLASH_SECTOR_SIZE는 2의 거듭제곱이라 나눗셈이 AND로 바뀐다. */
    sector_addr = addr - (addr % FLASH_SECTOR_SIZE);
    return Flash_Erase(sector_addr, FLASH_ERASE_SECTOR);
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
    /* 지원하는 모든 부품에서 같은 값이므로 상수로 둔다. init 전에는 0 을 돌려주어
     * "not initialized" 계약을 그대로 유지한다. */
    return (Flash_GetInfo() != NULL) ? FLASH_SECTOR_SIZE : 0U;
}
