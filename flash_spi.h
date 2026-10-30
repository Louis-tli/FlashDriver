/******************************************************************************
 * File Name   : flash_spi.h
 * Description : Low-Level SPI Controller Hardware Interface Header
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
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
 * @brief One complete SPI transaction (SW-016: one /CS frame, one hardware kick).
 *
 * The controller is programmed with the phase lengths and started once; it is
 * NOT a byte-at-a-time loop on real silicon. Phases run in this order inside a
 * single /CS assertion (identical to a QSPI controller's phase registers):
 *
 *   1. tx_buf   / tx_len    : instruction + address + dummy (1 data line)
 *   2. data_buf / data_len  : payload, 1 data line   (Page Program 02h)
 *   3. quad_buf / quad_len  : payload, 4 data lines  (Quad Page Program 32h)
 *   4. rx_buf   / rx_len    : read data (1 data line)
 *
 * Any phase may be empty (NULL pointer / zero length) and buffers are never
 * copied or concatenated. A weak default implementation drives the byte-level
 * primitives so unit tests and bring-up boards work unchanged; a target BSP
 * overrides it with the real buffered-register path.
 */
typedef struct
{
    const uint8_t *tx_buf;      /* Phase 1 source (cmd/address/dummy), or NULL  */
    uint32_t       tx_len;      /* Phase 1 length in bytes (0 = no phase)       */
    const uint8_t *data_buf;    /* Phase 2 source (1-bit payload), or NULL      */
    uint32_t       data_len;    /* Phase 2 length in bytes (0 = no phase)       */
    const uint8_t *quad_buf;    /* Phase 3 source (4-bit payload), or NULL      */
    uint32_t       quad_len;    /* Phase 3 length in bytes (0 = no phase)       */
    uint8_t       *rx_buf;      /* Phase 4 destination, or NULL                 */
    uint32_t       rx_len;      /* Phase 4 length in bytes (0 = no phase)       */
} FLASH_SPI_XFER;

/**
 * @brief Execute one FLASH_SPI_XFER as a single /CS frame.
 */
void Flash_SPI_Transact(const FLASH_SPI_XFER *xfer);

/**
 * @brief Transmit command opcode with /CS assertion (one-byte frame).
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
