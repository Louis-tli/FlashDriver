/******************************************************************************
 * File Name   : flash_sfc.c
 * Description : TCON Serial Flash Controller (SFC) IP Driver Implementation
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / C90 Compatible
 ******************************************************************************/

#include "flash_sfc.h"

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
        (transfer_page_size == SFC_DEFAULT_PAGE_SIZE_4K))
    {
        s_sfc_transfer_page_size = transfer_page_size;
    }
    else
    {
        s_sfc_transfer_page_size = SFC_DEFAULT_PAGE_SIZE_4K;
    }

    if (g_p_sfc_regs != NULL)
    {
        g_p_sfc_regs->CTRL = SFC_CTRL_RESET;
        g_p_sfc_regs->PAGE_SIZE = s_sfc_transfer_page_size;
        g_p_sfc_regs->CTRL = SFC_CTRL_ENABLE;
    }
}

void Flash_SFC_SetSingleMode(void)
{
    if (g_p_sfc_regs != NULL)
    {
        g_p_sfc_regs->MODE         = 0U;            /* Single Mode */
        g_p_sfc_regs->CMD          = CMD_READ;      /* 03h */
        g_p_sfc_regs->DUMMY_CYCLE  = 0U;
        g_p_sfc_regs->DUMMY_BYTE   = 0U;
        g_p_sfc_regs->DUMMY_VAL    = 0x00U;
        g_p_sfc_regs->DUMMY_LAST   = 0x00U;
        g_p_sfc_regs->MODE_BYTE    = 0x00U;
    }
}

void Flash_SFC_SetReadConfig(const FLASH_READ_CFG *cfg, uint8_t addr_bytes)
{
    if ((g_p_sfc_regs != NULL) && (cfg != NULL))
    {
        g_p_sfc_regs->CMD         = (uint32_t)cfg->command;
        g_p_sfc_regs->ADDR_SIZE   = (uint32_t)addr_bytes;
        g_p_sfc_regs->DUMMY_CYCLE = (uint32_t)cfg->dummy_cycles;
        g_p_sfc_regs->DUMMY_BYTE  = (uint32_t)cfg->dummy_bytes;
        g_p_sfc_regs->DUMMY_VAL   = (uint32_t)cfg->dummy_value;
        g_p_sfc_regs->DUMMY_LAST  = (uint32_t)cfg->dummy_last;
        g_p_sfc_regs->MODE_BYTE   = (uint32_t)cfg->mode_byte;
        g_p_sfc_regs->MODE        = (uint32_t)cfg->io_width;
    }
}

int Flash_SFC_DMARead(uint32_t flash_addr, void *dst_buf, uint32_t len,
                      uint8_t use_crc, uint32_t *p_crc_out)
{
    uint8_t *p_dst;
    uint32_t remaining;
    uint32_t current_addr;
    uint32_t chunk_len;
    uint32_t timeout;

    if ((dst_buf == NULL) || (len == 0U))
    {
        return FLASH_ERR_PARAM;
    }

    /* Cortex-M0 strictly requires 4-byte aligned addresses for 32-bit DMA transfers */
    if (((uint32_t)(uintptr_t)dst_buf & 3U) != 0U)
    {
        return FLASH_ERR_ALIGNMENT;
    }

    if (g_p_sfc_regs == NULL)
    {
        return FLASH_ERR_HARDWARE;
    }

    /* Initialize hardware CRC engine if requested */
    if (use_crc != 0U)
    {
        g_p_sfc_regs->CRC_CTRL = SFC_CRC_CTRL_RESET | SFC_CRC_CTRL_ENABLE;
    }

    p_dst = (uint8_t *)dst_buf;
    remaining = len;
    current_addr = flash_addr;

    /* Loop through transfers split by SFC page size (2KB or 4KB) */
    while (remaining > 0U)
    {
        chunk_len = (remaining > s_sfc_transfer_page_size) ?
                    (uint32_t)s_sfc_transfer_page_size : remaining;

        g_p_sfc_regs->DMA_SRC = current_addr;
        g_p_sfc_regs->DMA_DST = (uint32_t)(uintptr_t)p_dst;
        g_p_sfc_regs->DMA_LEN = chunk_len;

        /* Trigger DMA read */
        g_p_sfc_regs->CTRL |= SFC_CTRL_DMA_START;

        /* Wait for DMA transfer completion */
        timeout = 500000U;
        while ((g_p_sfc_regs->STATUS & SFC_STATUS_DMA_DONE) == 0U)
        {
            if ((g_p_sfc_regs->STATUS & SFC_STATUS_ERROR) != 0U)
            {
                return FLASH_ERR_HARDWARE;
            }

            timeout--;
            if (timeout == 0U)
            {
                return FLASH_ERR_TIMEOUT;
            }

            /* For simulation/stubs: set DMA_DONE if hardware loop is inactive */
            if (g_p_sfc_regs == &s_sfc_sim_regs)
            {
                g_p_sfc_regs->STATUS |= SFC_STATUS_DMA_DONE;
            }
        }

        /* Clear DMA completion flag */
        g_p_sfc_regs->STATUS &= ~SFC_STATUS_DMA_DONE;

        current_addr += chunk_len;
        p_dst += chunk_len;
        remaining -= chunk_len;
    }

    /* Fetch CRC result if requested */
    if ((use_crc != 0U) && (p_crc_out != NULL))
    {
        *p_crc_out = g_p_sfc_regs->CRC_RES;
    }

    return FLASH_OK;
}

uint16_t Flash_SFC_GetPageSize(void)
{
    return s_sfc_transfer_page_size;
}
