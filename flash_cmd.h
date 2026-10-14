/******************************************************************************
 * File Name   : flash_cmd.h
 * Description : SPI NOR Flash Command Opcode Resolution (3-Byte / 4-Byte)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * SW-013 / req 2 & 3: command selection is factored into its own module so the
 *   3-Byte vs 4-Byte opcode choice lives in one place. Read opcodes come from
 *   the per-device FLASH_READ_CFG; program/erase opcodes use the standard set.
 ******************************************************************************/

#ifndef __FLASH_CMD_H__
#define __FLASH_CMD_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Resolve the read opcode for a read mode given the active address width.
 * @param  cfg: Per-device read configuration (holds 3B and 4B opcodes).
 * @param  addr_bytes: 3 or 4.
 * @return The opcode for the current address width.
 */
uint8_t Flash_CmdRead(const FLASH_READ_CFG *cfg, uint8_t addr_bytes);

/**
 * @brief  Resolve the page-program opcode for the given program mode + width.
 */
uint8_t Flash_CmdProgram(FLASH_PROGRAM_MODE mode, uint8_t addr_bytes);

/**
 * @brief  Resolve the erase opcode for the given erase mode + width.
 */
uint8_t Flash_CmdErase(FLASH_ERASE_MODE mode, uint8_t addr_bytes);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_CMD_H__ */
