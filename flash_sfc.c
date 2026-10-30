/******************************************************************************
 * File Name   : flash_sfc.c
 * Description : TCON Serial Flash Controller (SFC) IP Driver Implementation
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_sfc.h"
#include "flash_cmd.h"
#include "flash_xfer.h"

/******************************************************************************
 * Global Pointer to SFC Register Map
 * (Default points to a static fallback instance for simulation / unit test)
 ******************************************************************************/
static SFC_TypeDef s_sfc_sim_regs;
SFC_TypeDef *g_p_sfc_regs = &s_sfc_sim_regs;

static uint16_t s_sfc_transfer_page_size = SFC_DEFAULT_PAGE_SIZE_4K;

/******************************************************************************
 * Implementation
 ******************************************************************************/

void Flash_SFC_Init(uint16_t transfer_page_size)
{
    if ((transfer_page_size == SFC_DEFAULT_PAGE_SIZE_2K) ||
        (transfer_page_size == SFC_DEFAULT_PAGE_SIZE_4K)) {
        s_sfc_transfer_page_size = transfer_page_size;
    }
    else {
        s_sfc_transfer_page_size = SFC_DEFAULT_PAGE_SIZE_4K;
    }

    if (g_p_sfc_regs != NULL) {
        g_p_sfc_regs->CTRL = SFC_CTRL_RESET;
        g_p_sfc_regs->PAGE_SIZE = s_sfc_transfer_page_size;
        g_p_sfc_regs->CTRL = SFC_CTRL_ENABLE;
    }
}

void Flash_SFC_SetSingleMode(void)
{
    if (g_p_sfc_regs != NULL) {
        g_p_sfc_regs->MODE         = 0U;            /* Single Mode */
        g_p_sfc_regs->CMD          = F_CMD_READ;      /* 03h */
        g_p_sfc_regs->DUMMY_CYCLE  = 0U;
        g_p_sfc_regs->DUMMY_BYTE   = 0U;
        g_p_sfc_regs->DUMMY_VAL    = 0x00U;
        g_p_sfc_regs->DUMMY_LAST   = 0x00U;
        g_p_sfc_regs->MODE_BYTE    = 0x00U;
    }
}

void Flash_SFC_SetReadConfig(const FLASH_READ_CFG *cfg, FLASH_ADDR_MODE mode)
{
    if ((g_p_sfc_regs != NULL) && (cfg != NULL)) {
        g_p_sfc_regs->CMD         = (uint32_t)Flash_CmdRead(cfg, mode);
        g_p_sfc_regs->ADDR_SIZE   = (uint32_t)Flash_CmdAddrBytes(mode);
        g_p_sfc_regs->DUMMY_CYCLE = (uint32_t)cfg->dummy_cycles;
        g_p_sfc_regs->DUMMY_BYTE  = (uint32_t)cfg->dummy_bytes;
        g_p_sfc_regs->DUMMY_VAL   = (uint32_t)cfg->dummy_value;
        g_p_sfc_regs->DUMMY_LAST  = (uint32_t)cfg->dummy_last;
        g_p_sfc_regs->MODE_BYTE   = (uint32_t)cfg->mode_byte;
        g_p_sfc_regs->MODE        = (uint32_t)cfg->io_width;
    }
}

int Flash_SFC_DMARead(uint32_t flash_addr, void *dst_buf, uint32_t count,
                      FLASH_READ_UNIT unit, bool use_crc, uint32_t *p_crc_out)
{
    /* Unit mapping (DEFAULT/WORD -> 4, HALFWORD -> 2, BYTE -> 1), NULL/count
     * checks, 32-bit overflow guard and the Cortex-M0 alignment check all come
     * from the shared helper (SW-016) — the SPI path uses the very same code. */
    uint32_t unit_bytes = 0U;
    uint32_t total_bytes = 0U;
    int status = Flash_XferResolve(unit, count, dst_buf, flash_addr,
                                   &unit_bytes, &total_bytes);
    if (status != FLASH_OK) {
        return status;
    }

    if (g_p_sfc_regs == NULL) {
        return FLASH_ERR_HARDWARE;
    }

    /* Simulation register block = unit-test / bring-up with no SFC IP behind it.
     * Everything simulation-only must be gated on this flag, otherwise it runs
     * on real silicon and destroys the DMA result (SW-015). */
    const bool is_sim = (g_p_sfc_regs == &s_sfc_sim_regs);

    g_p_sfc_regs->ACCESS_UNIT = unit_bytes;

    /* Ensure the CRC engine is enabled. The reset is issued once per transfer
     * by Flash_SFC_CrcReset() so the result accumulates across all chunks. */
    if (use_crc) {
        g_p_sfc_regs->CRC_CTRL |= SFC_CRC_CTRL_ENABLE;
    }

    uint8_t *p_dst = (uint8_t *)dst_buf;
    uint32_t remaining = total_bytes;
    uint32_t current_addr = flash_addr;

    /* Loop through transfers split by SFC page size (2KB or 4KB) */
    while (remaining > 0U) {
        const uint32_t chunk_len = (remaining > s_sfc_transfer_page_size) ?
                                   (uint32_t)s_sfc_transfer_page_size : remaining;

        g_p_sfc_regs->DMA_SRC = current_addr;
        g_p_sfc_regs->DMA_DST = (uint32_t)(uintptr_t)p_dst;
        g_p_sfc_regs->DMA_LEN = chunk_len;

        /* Hardware CRC accumulates over the whole transfer by itself. Only the
         * simulation folds the length into the accumulator; on real silicon the
         * SFC IP owns CRC_RES and a software add would corrupt the result. */
        if (use_crc && is_sim) {
            g_p_sfc_regs->CRC_RES += chunk_len;
        }

        /* Trigger DMA read */
        g_p_sfc_regs->CTRL |= SFC_CTRL_DMA_START;

        /* Wait for DMA transfer completion */
        uint32_t timeout = 500000U;
        while ((g_p_sfc_regs->STATUS & SFC_STATUS_DMA_DONE) == 0U) {
            if ((g_p_sfc_regs->STATUS & SFC_STATUS_ERROR) != 0U) {
                return FLASH_ERR_HARDWARE;
            }

            timeout--;
            if (timeout == 0U) {
                return FLASH_ERR_TIMEOUT;
            }

            /* Simulation only: no DMA engine exists to raise DMA_DONE. */
            if (is_sim) {
                g_p_sfc_regs->STATUS |= SFC_STATUS_DMA_DONE;
            }
        }

        /* Clear DMA completion flag */
        g_p_sfc_regs->STATUS &= ~SFC_STATUS_DMA_DONE;

        /* Simulation only: model the DMA writing read data into the destination.
         * On real hardware the SFC IP already filled p_dst from the flash array,
         * so touching it here would overwrite every byte with 0xFF (SW-015). */
        if (is_sim) {
            for (uint32_t i = 0U; i < chunk_len; i++) {
                p_dst[i] = 0xFFU;
            }
        }

        current_addr += chunk_len;
        p_dst += chunk_len;
        remaining -= chunk_len;
    }

    /* Fetch CRC result if requested */
    if (use_crc && (p_crc_out != NULL)) {
        *p_crc_out = g_p_sfc_regs->CRC_RES;
    }

    return FLASH_OK;
}

uint16_t Flash_SFC_GetPageSize(void)
{
    return s_sfc_transfer_page_size;
}

void Flash_SFC_CrcReset(void)
{
    if (g_p_sfc_regs != NULL) {
        g_p_sfc_regs->CRC_RES  = 0U;
        g_p_sfc_regs->CRC_CTRL = SFC_CRC_CTRL_RESET | SFC_CRC_CTRL_ENABLE;
    }
}

uint32_t Flash_SFC_CrcGet(void)
{
    return (g_p_sfc_regs != NULL) ? g_p_sfc_regs->CRC_RES : 0U;
}
