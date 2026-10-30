/******************************************************************************
 * File Name   : flash_cmd.c
 * Description : SPI NOR Flash Command Opcode Resolution & Addressing Mode
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_cmd.h"

/******************************************************************************
 * Addressing-mode attribute helpers
 ******************************************************************************/
uint8_t Flash_CmdAddrBytes(FLASH_ADDR_MODE mode)
{
    return (mode == FLASH_ADDR_3BYTE) ? 3U : 4U;
}

bool Flash_CmdUse4bOpcode(FLASH_ADDR_MODE mode)
{
    /* Only Method A uses the dedicated 4-Byte opcodes. */
    return (mode == FLASH_ADDR_4BYTE_OPCODE);
}

bool Flash_CmdNeedsEnter4b(FLASH_ADDR_MODE mode)
{
    /* Only Method B needs an Enter-4-Byte (B7h) sequence. */
    return (mode == FLASH_ADDR_4BYTE_ENTER);
}

/******************************************************************************
 * Read opcode: chosen by addressing mode (dedicated 4B only for Method A).
 ******************************************************************************/
uint8_t Flash_CmdRead(const FLASH_READ_CFG *cfg, FLASH_ADDR_MODE mode)
{
    if (cfg == NULL) {
        return 0x00U;
    }
    return Flash_CmdUse4bOpcode(mode) ? cfg->command_4b : cfg->command;
}

/******************************************************************************
 * Program opcode: standard 3-Byte / 4-Byte variants.
 ******************************************************************************/
uint8_t Flash_CmdProgram(FLASH_PROGRAM_MODE prog_mode, FLASH_ADDR_MODE addr_mode)
{
    const bool is_quad = (prog_mode == FLASH_PROGRAM_QUAD);

    if (Flash_CmdUse4bOpcode(addr_mode)) {
        return is_quad ? F_CMD_QUAD_PAGE_PROGRAM_4B : F_CMD_PAGE_PROGRAM_4B;
    }
    return is_quad ? F_CMD_QUAD_PAGE_PROGRAM : F_CMD_PAGE_PROGRAM;
}

/******************************************************************************
 * Command header builder (SW-016): opcode -> address -> dummy in one buffer.
 ******************************************************************************/
uint32_t Flash_CmdBuildHeader(uint8_t *buf, uint8_t cmd, uint32_t addr,
                              uint8_t addr_bytes, const FLASH_READ_CFG *read_cfg)
{
    if (buf == NULL) {
        return 0U;
    }
    if ((addr_bytes != 0U) && (addr_bytes != 3U) && (addr_bytes != 4U)) {
        return 0U;
    }

    uint32_t n = 0U;
    buf[n++] = cmd;

    if (addr_bytes != 0U) {
        if (addr_bytes == 4U) {
            buf[n++] = (uint8_t)((addr >> 24) & 0xFFU);
        }
        buf[n++] = (uint8_t)((addr >> 16) & 0xFFU);
        buf[n++] = (uint8_t)((addr >> 8) & 0xFFU);
        buf[n++] = (uint8_t)(addr & 0xFFU);
    }

    /* Dummy bytes: DUMMY_LAST first (M7-M0 slot), then DUMMY_VAL for the rest. */
    if (read_cfg != NULL) {
        for (uint32_t i = 0U; i < (uint32_t)read_cfg->dummy_bytes; i++) {
            if (n >= FLASH_CMD_HDR_MAX) {
                return 0U;
            }
            buf[n++] = (i == 0U) ? read_cfg->dummy_last : read_cfg->dummy_value;
        }
    }

    return n;
}

/******************************************************************************
 * Erase opcode: standard 3-Byte / 4-Byte variants (chip erase has no address).
 ******************************************************************************/
uint8_t Flash_CmdErase(FLASH_ERASE_MODE erase_mode, FLASH_ADDR_MODE addr_mode)
{
    const bool use_4b = Flash_CmdUse4bOpcode(addr_mode);

    switch (erase_mode) {
        case FLASH_ERASE_SECTOR:
            return use_4b ? F_CMD_SECTOR_ERASE_4B : F_CMD_SECTOR_ERASE;
        case FLASH_ERASE_BLOCK_32K:
            return use_4b ? F_CMD_BLOCK_ERASE_32K_4B : F_CMD_BLOCK_ERASE_32K;
        case FLASH_ERASE_BLOCK_64K:
            return use_4b ? F_CMD_BLOCK_ERASE_64K_4B : F_CMD_BLOCK_ERASE_64K;
        case FLASH_ERASE_CHIP:
            return F_CMD_CHIP_ERASE;
        default:
            return 0x00U;
    }
}
