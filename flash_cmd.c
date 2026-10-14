/******************************************************************************
 * File Name   : flash_cmd.c
 * Description : SPI NOR Flash Command Opcode Resolution (3-Byte / 4-Byte)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_cmd.h"

/******************************************************************************
 * Read opcode: taken from the per-device config, chosen by address width.
 ******************************************************************************/
uint8_t Flash_CmdRead(const FLASH_READ_CFG *cfg, uint8_t addr_bytes)
{
    if (cfg == NULL)
    {
        return 0x00U;
    }
    return (addr_bytes == 4U) ? cfg->command_4b : cfg->command;
}

/******************************************************************************
 * Program opcode: standard 3-Byte / 4-Byte variants.
 ******************************************************************************/
uint8_t Flash_CmdProgram(FLASH_PROGRAM_MODE mode, uint8_t addr_bytes)
{
    const bool is_quad = (mode == FLASH_PROGRAM_QUAD);

    if (addr_bytes == 4U)
    {
        return is_quad ? CMD_QUAD_PAGE_PROGRAM_4B : CMD_PAGE_PROGRAM_4B;
    }
    return is_quad ? CMD_QUAD_PAGE_PROGRAM : CMD_PAGE_PROGRAM;
}

/******************************************************************************
 * Erase opcode: standard 3-Byte / 4-Byte variants (chip erase has no address).
 ******************************************************************************/
uint8_t Flash_CmdErase(FLASH_ERASE_MODE mode, uint8_t addr_bytes)
{
    const bool is_4b = (addr_bytes == 4U);

    switch (mode)
    {
        case FLASH_ERASE_SECTOR:
            return is_4b ? CMD_SECTOR_ERASE_4B : CMD_SECTOR_ERASE;
        case FLASH_ERASE_BLOCK_32K:
            return is_4b ? CMD_BLOCK_ERASE_32K_4B : CMD_BLOCK_ERASE_32K;
        case FLASH_ERASE_BLOCK_64K:
            return is_4b ? CMD_BLOCK_ERASE_64K_4B : CMD_BLOCK_ERASE_64K;
        case FLASH_ERASE_CHIP:
            return CMD_CHIP_ERASE;
        default:
            return 0x00U;
    }
}
