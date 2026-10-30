/******************************************************************************
 * File Name   : flash_spi.c
 * Description : Low-Level SPI Controller Hardware Interface Implementation
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_spi.h"

/******************************************************************************
 * Hardware Abstraction Hooks
 * (In target TCON SoC firmware, replace these stubs with actual register access)
 ******************************************************************************/

#ifndef __WEAK
#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
#define __WEAK __weak
#elif defined(__GNUC__)
#define __WEAK __attribute__((weak))
#else
#define __WEAK
#endif
#endif

__WEAK void Flash_SPI_Init(void)
{
    /* Initialize SPI controller registers, baud rate, CPOL=0, CPHA=0 */
}

__WEAK void Flash_SPI_SetSpeed(uint32_t speed_hz)
{
    /* Configure SPI clock divider to match requested frequency */
    (void)speed_hz;
}

__WEAK void Flash_SPI_CS_Low(void)
{
    /* Drive /CS GPIO or hardware SPI CS pin LOW */
}

__WEAK void Flash_SPI_CS_High(void)
{
    /* Drive /CS GPIO or hardware SPI CS pin HIGH */
}

__WEAK uint8_t Flash_SPI_TransferByte(uint8_t tx_byte)
{
    /* Write tx_byte to SPI data register, poll TXE/RXNE, return received byte */
    (void)tx_byte;
    return 0xFFU;
}

/******************************************************************************
 * Buffered transfer engine (SW-016)
 *
 * Target BSP replacement (one register kick, no per-byte polling):
 *   SPI->TX_LEN   = xfer->tx_len;
 *   SPI->DATA_LEN = xfer->data_len;
 *   SPI->QUAD_LEN = xfer->quad_len;
 *   SPI->RX_LEN   = xfer->rx_len;
 *   SPI->TX_ADDR  = (uint32_t)xfer->tx_buf;
 *   SPI->DATA_ADDR= (uint32_t)xfer->data_buf;
 *   SPI->QUAD_ADDR= (uint32_t)xfer->quad_buf;
 *   SPI->RX_ADDR  = (uint32_t)xfer->rx_buf;
 *   SPI->CTRL    |= SPI_CTRL_START;        // CS auto-asserted/released by HW
 *   while ((SPI->STATUS & SPI_STATUS_DONE) == 0U) { ... }
 ******************************************************************************/
__WEAK void Flash_SPI_Transact(const FLASH_SPI_XFER *xfer)
{
    if (xfer == NULL) {
        return;
    }

    Flash_SPI_CS_Low();

    /* Phase 1: command + address + dummy (1 data line) */
    if ((xfer->tx_buf != NULL) && (xfer->tx_len > 0U)) {
        for (uint32_t i = 0U; i < xfer->tx_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->tx_buf[i]);
        }
    }

    /* Phase 2: 1-bit data payload (Page Program 02h) */
    if ((xfer->data_buf != NULL) && (xfer->data_len > 0U)) {
        Flash_SPI_WriteData(xfer->data_buf, xfer->data_len);
    }

    /* Phase 3: 4-bit data payload (Quad Page Program 32h) */
    if ((xfer->quad_buf != NULL) && (xfer->quad_len > 0U)) {
        Flash_SPI_WriteQuadData(xfer->quad_buf, xfer->quad_len);
    }

    /* Phase 4: read data back (1 data line) */
    if ((xfer->rx_buf != NULL) && (xfer->rx_len > 0U)) {
        for (uint32_t i = 0U; i < xfer->rx_len; i++) {
            xfer->rx_buf[i] = Flash_SPI_TransferByte(0xFFU);
        }
    }

    Flash_SPI_CS_High();
}

void Flash_SPI_SendCmd(uint8_t cmd)
{
    const FLASH_SPI_XFER xfer = { .tx_buf = &cmd, .tx_len = 1U };
    Flash_SPI_Transact(&xfer);
}

void Flash_SPI_SendAddr(uint32_t addr, uint8_t addr_bytes)
{
    if (addr_bytes == 4U) {
        Flash_SPI_TransferByte((uint8_t)((addr >> 24) & 0xFFU));
    }
    Flash_SPI_TransferByte((uint8_t)((addr >> 16) & 0xFFU));
    Flash_SPI_TransferByte((uint8_t)((addr >> 8) & 0xFFU));
    Flash_SPI_TransferByte((uint8_t)(addr & 0xFFU));
}

void Flash_SPI_ReadData(uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        buf[i] = Flash_SPI_TransferByte(0xFFU);
    }
}

void Flash_SPI_WriteData(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        (void)Flash_SPI_TransferByte(buf[i]);
    }
}

__WEAK void Flash_SPI_WriteQuadData(const uint8_t *buf, uint32_t len)
{
    /* Hardware Quad SPI FIFO write or multi-bit transfer */
    Flash_SPI_WriteData(buf, len);
}
