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
 * (기본값은 simulation / unit test 용 static fallback instance)
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
        g_p_sfc_regs->CMD         = (uint32_t)Flash_GetCmdRead(cfg, mode);
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
    /* Unit 매핑(DEFAULT/WORD -> 4, HALFWORD -> 2, BYTE -> 1), NULL/count 검사,
         * 32비트 overflow 방어, Cortex-M0 정렬 검사는 모두 공용 helper(SW-016)가
         * 처리한다 — SPI 경로도 같은 code 를 쓴다. */
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

    /* Simulation register block — 뒤에 SFC IP 가 없는 unit test / bring-up 용.
         * Simulation 전용 동작은 반드시 이 flag 로 막아야 한다. 그렇지 않으면 실제
         * silicon 에서도 실행되어 DMA 결과를 망친다(SW-015). */
    const bool is_sim = (g_p_sfc_regs == &s_sfc_sim_regs);

    g_p_sfc_regs->ACCESS_UNIT = unit_bytes;

    /* CRC engine 이 켜져 있는지 확인한다. reset 은 Flash_SFC_CrcReset() 이
         * 전송당 한 번만 걸기 때문에 결과는 모든 chunk 에 걸쳐 누적된다. */
    if (use_crc) {
        g_p_sfc_regs->CRC_CTRL |= SFC_CRC_CTRL_ENABLE;
    }

    uint8_t *p_dst = (uint8_t *)dst_buf;
    uint32_t remaining = total_bytes;
    uint32_t current_addr = flash_addr;

    /* SFC page size(2KB/4KB) 단위로 나눠 전송하는 loop */
    while (remaining > 0U) {
        const uint32_t chunk_len = (remaining > s_sfc_transfer_page_size) ?
                                   (uint32_t)s_sfc_transfer_page_size : remaining;

        g_p_sfc_regs->DMA_SRC = current_addr;
        g_p_sfc_regs->DMA_DST = (uint32_t)(uintptr_t)p_dst;
        g_p_sfc_regs->DMA_LEN = chunk_len;

        /* 실제 hardware CRC 는 전송 전체에 걸쳐 스스로 누적된다. 길이를
                 * accumulator 에 더하는 것은 simulation 뿐이다. 실제 silicon 에서는
                 * SFC IP 가 CRC_RES 를 소유하므로 software 가 더하면 결과가 깨진다. */
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

            /* Simulation 전용: DMA_DONE 을 올려 줄 DMA engine 이 없다. */
            if (is_sim) {
                g_p_sfc_regs->STATUS |= SFC_STATUS_DMA_DONE;
            }
        }

        /* Clear DMA completion flag */
        g_p_sfc_regs->STATUS &= ~SFC_STATUS_DMA_DONE;

        /* Simulation 전용: DMA 가 read data 를 목적지에 쓴 상황을 흉내낸다.
                 * 실제 hardware 는 SFC IP 가 flash array 에서 p_dst 를 이미 채웠으므로,
                 * 여기서 건드리면 모든 byte 가 0xFF 로 덮인다(SW-015). */
        if (is_sim) {
            for (uint32_t i = 0U; i < chunk_len; i++) {
                p_dst[i] = 0xFFU;
            }
        }

        current_addr    += chunk_len;
        p_dst           += chunk_len;
        remaining       -= chunk_len;
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
