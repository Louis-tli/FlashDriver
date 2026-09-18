/******************************************************************************
 * File Name   : flash_spi.c
 * Description : Low-Level SPI Controller Hardware Interface Implementation
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / C90 Compatible
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

void Flash_SPI_SendCmd(uint8_t cmd)
{
    Flash_SPI_CS_Low();
    Flash_SPI_TransferByte(cmd);
    Flash_SPI_CS_High();
}

void Flash_SPI_SendAddr(uint32_t addr, uint8_t addr_bytes)
{
    if (addr_bytes == 4U)
    {
        Flash_SPI_TransferByte((uint8_t)((addr >> 24) & 0xFFU));
    }
    Flash_SPI_TransferByte((uint8_t)((addr >> 16) & 0xFFU));
    Flash_SPI_TransferByte((uint8_t)((addr >> 8) & 0xFFU));
    Flash_SPI_TransferByte((uint8_t)(addr & 0xFFU));
}

void Flash_SPI_ReadData(uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0U; i < len; i++)
    {
        buf[i] = Flash_SPI_TransferByte(0xFFU);
    }
}

void Flash_SPI_WriteData(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0U; i < len; i++)
    {
        (void)Flash_SPI_TransferByte(buf[i]);
    }
}

__WEAK void Flash_SPI_WriteQuadData(const uint8_t *buf, uint32_t len)
{
    /* Hardware Quad SPI FIFO write or multi-bit transfer */
    Flash_SPI_WriteData(buf, len);
}
