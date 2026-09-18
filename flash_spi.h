/******************************************************************************
 * File Name   : flash_spi.h
 * Description : Low-Level SPI Controller Hardware Interface Header
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / C90 Compatible
 ******************************************************************************/

#ifndef __FLASH_SPI_H__
#define __FLASH_SPI_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * SPI Low-Level Control Interface
 ******************************************************************************/

/**
 * @brief Initialize SPI peripheral hardware (GPIOs, SPI clock, Master Mode 0).
 */
void Flash_SPI_Init(void);

/**
 * @brief Set SPI clock frequency.
 * @param speed_hz: Clock frequency in Hz (e.g. 10000000 for 10MHz safe mode).
 */
void Flash_SPI_SetSpeed(uint32_t speed_hz);

/**
 * @brief Assert Chip Select (drive /CS low).
 */
void Flash_SPI_CS_Low(void);

/**
 * @brief Deassert Chip Select (drive /CS high).
 */
void Flash_SPI_CS_High(void);

/**
 * @brief Full-duplex single byte transfer via SPI.
 * @param tx_byte: Byte to transmit.
 * @return Byte received from slave.
 */
uint8_t Flash_SPI_TransferByte(uint8_t tx_byte);

/**
 * @brief Transmit command opcode with /CS assertion.
 * @param cmd: Command byte.
 */
void Flash_SPI_SendCmd(uint8_t cmd);

/**
 * @brief Transmit address bytes (3 or 4 bytes, MSB first).
 * @param addr: 32-bit address.
 * @param addr_bytes: 3 or 4.
 */
void Flash_SPI_SendAddr(uint32_t addr, uint8_t addr_bytes);

/**
 * @brief Read buffer from SPI in single mode.
 * @param buf: Destination buffer.
 * @param len: Number of bytes to read.
 */
void Flash_SPI_ReadData(uint8_t *buf, uint32_t len);

/**
 * @brief Write buffer to SPI in single mode.
 * @param buf: Source buffer.
 * @param len: Number of bytes to write.
 */
void Flash_SPI_WriteData(const uint8_t *buf, uint32_t len);

/**
 * @brief Write buffer in Quad Page Program (32h) mode.
 * @param buf: Source buffer.
 * @param len: Number of bytes to write.
 */
void Flash_SPI_WriteQuadData(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_SPI_H__ */
