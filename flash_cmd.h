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
 * @brief  Maximum size of a command header: opcode(1) + address(4) + dummy(7).
 *         Used to size the on-stack buffer handed to Flash_SPI_Transact().
 */
#define FLASH_CMD_HDR_MAX               (12U)

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

/**
 * @brief  Build the on-wire command header for a buffered SPI transaction
 *         (opcode -> address MSB first -> dummy bytes) in a single buffer, so
 *         the whole header goes out in one FLASH_SPI_XFER phase (SW-016).
 *
 *         Dummy wire order (owner-confirmed TCON behaviour):
 *           DUMMY_LAST (first dummy = M7-M0 mode byte slot) -> DUMMY_VAL ...
 *
 * @param  buf:        Destination buffer of at least FLASH_CMD_HDR_MAX bytes.
 * @param  cmd:        Opcode byte.
 * @param  addr:       Flash address (ignored when addr_bytes == 0).
 * @param  addr_bytes: 0 (no address phase), 3 or 4.
 * @param  read_cfg:   Optional read config supplying dummy bytes (NULL = none).
 * @return Header length in bytes, or 0 when the arguments cannot be encoded.
 */
uint32_t Flash_CmdBuildHeader(uint8_t *buf, uint8_t cmd, uint32_t addr,
                              uint8_t addr_bytes, const FLASH_READ_CFG *read_cfg);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_CMD_H__ */
