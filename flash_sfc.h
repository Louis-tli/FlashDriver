/******************************************************************************
 * File Name   : flash_sfc.h
 * Description : TCON Serial Flash Controller (SFC) IP Driver Header
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / C90 Compatible
 ******************************************************************************/

#ifndef __FLASH_SFC_H__
#define __FLASH_SFC_H__

#include "flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * Default SFC Configuration Parameters
 ******************************************************************************/
#define SFC_DEFAULT_PAGE_SIZE_2K        (2048U)
#define SFC_DEFAULT_PAGE_SIZE_4K        (4096U)

/******************************************************************************
 * SFC Controller Register Map (Mapped to TCON SoC Address Space)
 ******************************************************************************/
typedef struct
{
    volatile uint32_t CTRL;             /* 0x00: Control Register (Enable, DMA Start, Reset) */
    volatile uint32_t MODE;             /* 0x04: Read Mode (Single, Dual, Quad, Quad IO) */
    volatile uint32_t CMD;              /* 0x08: Read Opcode (0x03, 0x0B, 0xBB, 0xEB) */
    volatile uint32_t ADDR_SIZE;        /* 0x0C: Address Size in Bytes (3 or 4) */
    volatile uint32_t DUMMY_CYCLE;      /* 0x10: Dummy Clock Cycles */
    volatile uint32_t DUMMY_BYTE;       /* 0x14: Dummy Byte Count */
    volatile uint32_t DUMMY_VAL;        /* 0x18: Dummy Byte Value (0x00 or 0xFF) */
    volatile uint32_t DUMMY_LAST;       /* 0x1C: Dummy Last Byte Value */
    volatile uint32_t MODE_BYTE;        /* 0x20: Mode Cycle Byte (Continuous Read) */
    volatile uint32_t PAGE_SIZE;        /* 0x24: Transfer Page Size (2048 or 4096) */
    volatile uint32_t ACCESS_UNIT;      /* 0x28: Access Unit (1: Byte, 2: Halfword, 4: Word - Default: Word) */
    volatile uint32_t DMA_SRC;          /* 0x2C: Flash Source Address */
    volatile uint32_t DMA_DST;          /* 0x30: Internal SRAM Destination Address */
    volatile uint32_t DMA_LEN;          /* 0x34: Transfer Length in Bytes */
    volatile uint32_t STATUS;           /* 0x38: SFC / DMA Status (Done, Busy, Error) */
    volatile uint32_t CRC_CTRL;         /* 0x3C: Hardware CRC Calculation Control */
    volatile uint32_t CRC_RES;          /* 0x40: Hardware CRC Result Register */
} SFC_TypeDef;

/******************************************************************************
 * SFC Register Bitfields
 ******************************************************************************/
#define SFC_CTRL_ENABLE                 (1U << 0)
#define SFC_CTRL_DMA_START              (1U << 1)
#define SFC_CTRL_RESET                  (1U << 2)

#define SFC_STATUS_BUSY                 (1U << 0)
#define SFC_STATUS_DMA_DONE             (1U << 1)
#define SFC_STATUS_ERROR                (1U << 2)

#define SFC_CRC_CTRL_ENABLE             (1U << 0)
#define SFC_CRC_CTRL_RESET              (1U << 1)

/******************************************************************************
 * Base Address Pointer
 ******************************************************************************/
extern SFC_TypeDef *g_p_sfc_regs;

/******************************************************************************
 * SFC Hardware Interface Functions
 ******************************************************************************/

/**
 * @brief Initialize SFC Controller IP with specified transfer unit size.
 * @param transfer_page_size: 2048 or 4096 bytes.
 */
void Flash_SFC_Init(uint16_t transfer_page_size);

/**
 * @brief Configure SFC IP registers to Single Mode before polling Flash status.
 */
void Flash_SFC_SetSingleMode(void);

/**
 * @brief Configure SFC IP registers for target high-speed Read Mode.
 * @param cfg: Pointer to FLASH_READ_CFG.
 * @param addr_bytes: 3 or 4.
 */
void Flash_SFC_SetReadConfig(const FLASH_READ_CFG *cfg, uint8_t addr_bytes);

/**
 * @brief Perform Memory-Mapped DMA Read through SFC IP.
 *        Automatically splits transfer into 2KB / 4KB chunks and optionally computes CRC.
 * @param flash_addr: Linear Flash start address (must be aligned to unit size).
 * @param dst_buf: Destination buffer (must be aligned to unit size).
 * @param count: Number of units to read.
 * @param unit: FLASH_READ_UNIT (BYTE=1, HALFWORD=2, WORD=4, or DEFAULT=0 for WORD).
 * @param use_crc: 1 to enable hardware CRC calculation.
 * @param p_crc_out: Pointer to receive 32-bit CRC result (can be NULL).
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_SFC_DMARead(uint32_t flash_addr, void *dst_buf, uint32_t count,
                      FLASH_READ_UNIT unit, uint8_t use_crc, uint32_t *p_crc_out);

/**
 * @brief Get configured SFC transfer page size.
 * @return 2048 or 4096.
 */
uint16_t Flash_SFC_GetPageSize(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_SFC_H__ */
