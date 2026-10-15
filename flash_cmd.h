/******************************************************************************
 * File Name   : flash_cmd.h
 * Description : SPI NOR Flash Command Opcode Resolution & Addressing Mode
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * SW-013 / req 2 & 3: command selection is factored into its own module. The
 *   active FLASH_ADDR_MODE decides the address width, which opcode set to use
 *   (3-Byte vs dedicated 4-Byte), and whether Enter-4-Byte (B7h) is needed.
 ******************************************************************************/

#ifndef __FLASH_CMD_H__
#define __FLASH_CMD_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Address width (3 or 4 bytes) for the given addressing mode.
 */
uint8_t Flash_CmdAddrBytes(FLASH_ADDR_MODE mode);

/**
 * @brief  True when the mode uses dedicated 4-Byte opcodes (Method A).
 */
bool Flash_CmdUse4bOpcode(FLASH_ADDR_MODE mode);

/**
 * @brief  True when the mode requires an Enter-4-Byte (B7h) sequence (Method B).
 */
bool Flash_CmdNeedsEnter4b(FLASH_ADDR_MODE mode);

/**
 * @brief  Resolve the read opcode for a read config + addressing mode.
 */
uint8_t Flash_CmdRead(const FLASH_READ_CFG *cfg, FLASH_ADDR_MODE mode);

/**
 * @brief  Resolve the page-program opcode for the given program + addressing mode.
 */
uint8_t Flash_CmdProgram(FLASH_PROGRAM_MODE prog_mode, FLASH_ADDR_MODE addr_mode);

/**
 * @brief  Resolve the erase opcode for the given erase + addressing mode.
 */
uint8_t Flash_CmdErase(FLASH_ERASE_MODE erase_mode, FLASH_ADDR_MODE addr_mode);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_CMD_H__ */
