/******************************************************************************
 * File Name   : test_flash.c
 * Description : Verification Test Suite for Generic Flash Driver
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 compliant)
 *
 * Covers the SW-013 boot model (ID -> basic-read settings -> configure -> lock),
 * product-family matching / fail-safe, per-die QE config, CRC accumulation and
 * the direct-SPI single-width guard, on top of the original 6 suites.
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include "flash.h"
#include "flash_table.h"
#include "flash_spi.h"
#include "flash_sfc.h"
#include "flash_easy.h"

/******************************************************************************
 * Mock SPI State for Emulation & Testing (per-die status registers)
 ******************************************************************************/
typedef struct
{
    uint32_t mock_jedec_id;
    uint8_t  sr1[2], sr2[2], sr3[2];   /* per-die status registers */
    uint8_t  current_die;
    bool     entered_4byte[2];         /* per-die 4-Byte address mode flag */
    uint8_t  last_die_selected;
    uint32_t die_select_count;
    uint32_t page_program_count;
    uint32_t total_bytes_programmed;
    bool     in_cs;
    uint8_t  current_cmd;
    uint8_t  cmd_byte_index;

    /* Boot settings block served on F_CMD_READ (03h) */
    uint8_t  config_buf[128];
    uint32_t config_len;

    /* Reset (66h/99h) command counter for init-option testing */
    uint32_t reset_cmd_count;

    /* Write-enable variant counters: 06h (WREN) vs 50h (VSR_WREN) */
    uint32_t wren_count;
    uint32_t vsr_wren_count;

    /* SW-013 WEL fix: 50h arms ONLY the next WRSR and does NOT set WEL
     * (Winbond W25Q / GigaDevice GD25LQ datasheets). Intervening commands
     * clear the pending volatile enable. WRSR is accepted only when WEL is
     * set (06h) or a 50h is pending. */
    bool     vsr_wren_pending;
    bool     wrsr_accepted;

    /* SW-019: byte count of the last 01h (WRSR) frame, so a test can prove the
     * driver always sends the full 16-bit form (opcode + SR1 + SR2) on parts
     * that cannot address SR2 on its own. */
    uint32_t wrsr1_frame_bytes;

    /* SW-020: the device is in the middle of a self-timed cycle for the next N
     * status reads. While it is, SR1 reports WIP(BUSY)=1 and WREN is dropped,
     * so WEL and BUSY can legitimately be seen set at the same time - the
     * exact window where a WEL-only check lies about being ready. */
    uint32_t busy_reads_remaining;

    /* SW-020: the device silently drops the next WRSR (a rejected write).
     * Nothing on the wire indicates it, so only a read-back can notice. */
    bool     drop_next_wrsr;
} MOCK_SPI_STATE;

static MOCK_SPI_STATE s_mock;

/* SW-016: geometry of the most recent Flash_SPI_Transact() frame, so the tests
 * can prove that command/address/data travel as ONE buffered transaction. */
typedef struct
{
    uint32_t frames;            /* Flash_SPI_Transact() calls since reset       */
    uint32_t last_tx_len;       /* header (cmd + address + dummy) length       */
    uint32_t last_data_len;     /* 1-bit payload length                         */
    uint32_t last_quad_len;     /* 4-bit payload length                         */
    uint32_t last_rx_len;       /* read length                                  */
    uint8_t  last_first_tx;     /* first header byte (opcode)                   */
    uint32_t last_payload_len;  /* last non-zero data/quad payload length       */
    uint32_t last_payload_tx_len; /* header length of the frame carrying payload */
    uint32_t last_data_payload_len; /* last 1-bit payload length               */
    uint32_t last_quad_payload_len; /* last 4-bit payload length               */
} MOCK_XFER_LOG;

static MOCK_XFER_LOG s_xfer;

/* Reset mock to a clean state (both dies). Also clear the driver handle so
 * each test starts from a fresh, unlocked driver. */
static void Mock_Reset(void)
{
    memset(&s_mock, 0, sizeof(s_mock));
    memset(&s_xfer, 0, sizeof(s_xfer));
    s_mock.last_die_selected = 0xFFU;

    g_flash_handle.info          = NULL;
    g_flash_handle.state         = FLASH_STATE_UNINIT;
    g_flash_handle.addr_mode     = FLASH_ADDR_MODE_DEFAULT;
    g_flash_handle.current_die   = 0U;
    g_flash_handle.qe_enabled    = false;
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;
    g_flash_handle.locked        = false;
    g_flash_handle.last_crc      = 0U;
}

/* Set the status registers for both dies (keeps old single-value call sites simple). */
static void Mock_SetStatus(uint8_t sr1, uint8_t sr2, uint8_t sr3)
{
    for (int d = 0; d < 2; d++) {
        s_mock.sr1[d] = sr1;
        s_mock.sr2[d] = sr2;
        s_mock.sr3[d] = sr3;
    }
}

/* Build a valid boot settings block and install it as the readable config. */
static void Mock_SetBootConfig(uint32_t jedec_expect, uint16_t sfc_page, uint32_t features)
{
    FLASH_BOOT_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic        = FLASH_BOOT_CFG_MAGIC;
    cfg.jedec_expect = jedec_expect;
    cfg.model        = (uint32_t)FLASH_MODEL_UNKNOWN;   /* fall back to JEDEC lookup */
    cfg.features     = features;
    cfg.sfc_page_size = sfc_page;
    cfg.addr_mode    = (uint8_t)FLASH_ADDR_MODE_DEFAULT;
    cfg.crc          = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);
}

/* Override weak SPI functions for test verification */
void Flash_SPI_Init(void)
{
    /* Preserve mock data across driver-init; just clear transfer bookkeeping. */
    s_mock.in_cs = false;
    s_mock.current_cmd = 0U;
    s_mock.cmd_byte_index = 0U;
}

void Flash_SPI_SetSpeed(uint32_t speed_hz)
{
    (void)speed_hz;
}

void Flash_SPI_CS_Low(void)
{
    s_mock.in_cs = true;
    s_mock.cmd_byte_index = 0U;
}

void Flash_SPI_CS_High(void)
{
    s_mock.in_cs = false;
    s_mock.current_cmd = 0U;
    s_mock.cmd_byte_index = 0U;
}

uint8_t Flash_SPI_TransferByte(uint8_t tx_byte)
{
    uint8_t rx_byte = 0xFFU;

    if (s_mock.cmd_byte_index == 0U) {
        s_mock.current_cmd = tx_byte;
        s_mock.cmd_byte_index = 1U;

        /* SW-013 WEL fix: model datasheet semantics — WRSR is accepted only
         * with WEL (06h) or a pending 50h; anything else clears the 50h. */
        if ((tx_byte == F_CMD_WRSR1) || (tx_byte == F_CMD_WRSR2) || (tx_byte == F_CMD_WRSR3)) {
            s_mock.wrsr_accepted = (((s_mock.sr1[s_mock.current_die] & FLASH_SR1_WEL) != 0U) ||
                                    s_mock.vsr_wren_pending);
            s_mock.vsr_wren_pending = false;
        }
        else if (tx_byte != F_CMD_VSR_WREN) {
            s_mock.vsr_wren_pending = false;
        }

        if (tx_byte == F_CMD_WREN) {
            s_mock.wren_count++;
            /* SW-020: like the silicon, WREN is ignored during an embedded
             * operation (WIP=1), so it must not set WEL then. */
            if (s_mock.busy_reads_remaining == 0U) {
                s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
            }
        } else if (tx_byte == F_CMD_VSR_WREN) {
            s_mock.vsr_wren_pending = true;   /* WEL stays 0 (datasheet) */
            s_mock.vsr_wren_count++;
        }
        else if (tx_byte == F_CMD_WRDI) {
            s_mock.sr1[s_mock.current_die] &= (uint8_t)(~FLASH_SR1_WEL);
        }
        else if (tx_byte == F_CMD_ENTER_4BYTE_ADDR) {
            s_mock.entered_4byte[s_mock.current_die] = true;
        }
        else if ((tx_byte == F_CMD_RESET_ENABLE) || (tx_byte == F_CMD_RESET_DEVICE)) {
            s_mock.reset_cmd_count++;
        }
        return 0x00U;
    }

    switch (s_mock.current_cmd) {
        case F_CMD_RDID: /* 9Fh */
            if (s_mock.cmd_byte_index == 1U) {
                rx_byte = (uint8_t)((s_mock.mock_jedec_id >> 16) & 0xFFU);
            }
            else if (s_mock.cmd_byte_index == 2U) {
                rx_byte = (uint8_t)((s_mock.mock_jedec_id >> 8) & 0xFFU);
            }
            else if (s_mock.cmd_byte_index == 3U) {
                rx_byte = (uint8_t)(s_mock.mock_jedec_id & 0xFFU);
            }
            s_mock.cmd_byte_index++;
            break;

        case F_CMD_READ: /* 03h : serve the boot settings block after cmd + 3 addr */
            if (s_mock.cmd_byte_index >= 4U) {
                const uint32_t idx = s_mock.cmd_byte_index - 4U;
                rx_byte = (idx < s_mock.config_len) ? s_mock.config_buf[idx] : 0xFFU;
            }
            else {
                rx_byte = 0x00U; /* address phase */
            }
            s_mock.cmd_byte_index++;
            break;

        case F_CMD_RDSR1: /* 05h */
            rx_byte = s_mock.sr1[s_mock.current_die];
            if (s_mock.busy_reads_remaining > 0U) {   /* SW-020: WIP=1 window */
                s_mock.busy_reads_remaining--;
                rx_byte |= FLASH_SR1_BUSY;
            }
            break;
        case F_CMD_RDSR2: /* 35h */
            rx_byte = s_mock.sr2[s_mock.current_die];
            break;
        case F_CMD_RDSR3: /* 15h */
            rx_byte = s_mock.sr3[s_mock.current_die];
            break;

        case F_CMD_WRSR1: /* 01h : 1-byte (SR1) or 2-byte (SR1+SR2) */
            if (!s_mock.wrsr_accepted) {
                break;                      /* chip rejects: no WEL, no pending 50h */
            }
            if (s_mock.drop_next_wrsr) {    /* SW-020: write silently dropped */
                s_mock.drop_next_wrsr = false;
                s_mock.cmd_byte_index++;
                break;
            }
            if (s_mock.cmd_byte_index == 1U) {
                s_mock.sr1[s_mock.current_die] = tx_byte;
                s_mock.cmd_byte_index++;
            }
            else if (s_mock.cmd_byte_index == 2U) {
                s_mock.sr2[s_mock.current_die] = tx_byte;
                s_mock.cmd_byte_index++;
            }
            break;

        case F_CMD_WRSR2: /* 31h */
            if (!s_mock.wrsr_accepted) {
                break;                      /* chip rejects: no WEL, no pending 50h */
            }
            s_mock.sr2[s_mock.current_die] = tx_byte;
            break;

        case F_CMD_WREN: /* 06h */
            s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
            break;

        case F_CMD_WRDI: /* 04h */
            s_mock.sr1[s_mock.current_die] &= (uint8_t)(~FLASH_SR1_WEL);
            break;

        case F_CMD_WINBOND_DIE_SELECT: /* C2h : next byte selects the die */
            s_mock.current_die = tx_byte;
            s_mock.last_die_selected = tx_byte;
            s_mock.die_select_count++;
            break;

        case F_CMD_PAGE_PROGRAM:       /* 02h */
        case F_CMD_QUAD_PAGE_PROGRAM:  /* 32h */
        case F_CMD_PAGE_PROGRAM_4B:    /* 12h */
        case F_CMD_QUAD_PAGE_PROGRAM_4B: /* 34h */
            if (s_mock.cmd_byte_index == 1U) {
                s_mock.page_program_count++;
            }
            if (s_mock.cmd_byte_index >= 4U) {
                s_mock.total_bytes_programmed++;
            }
            s_mock.cmd_byte_index++;
            break;

        default:
            rx_byte = 0xFFU;
            s_mock.cmd_byte_index++;
            break;
    }

    return rx_byte;
}

/* SW-016: instrumented stand-in for the target BSP's buffered transfer.
 * Records the requested phase geometry, then replays the bytes through the
 * byte-level mock so the emulated device still sees the same wire order. */
void Flash_SPI_Transact(const FLASH_SPI_XFER *xfer)
{
    s_xfer.frames++;
    s_xfer.last_tx_len   = xfer->tx_len;
    s_xfer.last_data_len = xfer->data_len;
    s_xfer.last_quad_len = xfer->quad_len;
    s_xfer.last_rx_len   = xfer->rx_len;
    s_xfer.last_first_tx = ((xfer->tx_buf != NULL) && (xfer->tx_len > 0U))
                           ? xfer->tx_buf[0] : 0U;
    if (xfer->quad_len > 0U) {
        s_xfer.last_payload_len = xfer->quad_len;
        s_xfer.last_payload_tx_len = xfer->tx_len;
        s_xfer.last_quad_payload_len = xfer->quad_len;
    }
    else if (xfer->data_len > 0U) {
        s_xfer.last_payload_len = xfer->data_len;
        s_xfer.last_payload_tx_len = xfer->tx_len;
        s_xfer.last_data_payload_len = xfer->data_len;
    }

    Flash_SPI_CS_Low();

    if (xfer->tx_buf != NULL) {
        for (uint32_t i = 0U; i < xfer->tx_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->tx_buf[i]);
        }
    }
    if (xfer->data_buf != NULL) {
        for (uint32_t i = 0U; i < xfer->data_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->data_buf[i]);
        }
    }
    if (xfer->quad_buf != NULL) {
        /* The emulated device has a single data line: quad length still counts. */
        for (uint32_t i = 0U; i < xfer->quad_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->quad_buf[i]);
        }
    }
    if (xfer->rx_buf != NULL) {
        for (uint32_t i = 0U; i < xfer->rx_len; i++) {
            xfer->rx_buf[i] = Flash_SPI_TransferByte(0xFFU);
        }
    }

    /* ---------------------------------------------------------------------
     * SW-019 mock fidelity: GD25LQ64E WRSR is a FIXED 16-bit command.
     * Datasheet 7.4 - "If CS# is driven high after eighth bit of the data
     * byte, the command is not executed" in QSPI, but in SPI mode the write
     * IS executed and "the QE and CMP bits will be cleared to 0".
     * The earlier mock only wrote SR1 and left QE alone, which is exactly why
     * the one-byte SR1 update looked harmless in the test suite while a real
     * part would silently drop out of quad mode. Model the silicon here.
     * ------------------------------------------------------------------- */
    if ((xfer->tx_buf != NULL) && (xfer->tx_buf[0] == F_CMD_WRSR1)) {
        s_mock.wrsr1_frame_bytes = xfer->tx_len;
        if ((xfer->tx_len == 2U) && (s_mock.mock_jedec_id == 0xC86017U)) {  /* GD25LQ64E */
            s_mock.sr2[s_mock.current_die] &= (uint8_t)~0x42U;   /* clear QE (S9) + CMP (S14) */
        }
    }

    Flash_SPI_CS_High();
}

/******************************************************************************
 * Test Cases
 ******************************************************************************/
static void Test_TableLookup(void)
{
    printf("[TEST 1] Testing JEDEC ID Table Lookups...\n");

    assert(Flash_FindDevice(0xC22010U) != NULL); /* MX25L512 (512Kb) */
    assert(Flash_FindDevice(0xC2201AU) != NULL); /* MX25L512 (512Mb) */
    assert(Flash_FindDevice(0xEF6119U) != NULL); /* W25M512 */
    assert(Flash_FindDevice(0xEF6018U) != NULL); /* W25Q128JW */
    assert(Flash_FindDevice(0xEF4020U) != NULL); /* W25Q512JV */
    assert(Flash_FindDevice(0xEF4019U) != NULL); /* W25Q256JV */
    assert(Flash_FindDevice(0xC86017U) != NULL); /* GD25LQ64E */

    assert(Flash_FindDevice(0x123456U) == NULL);

    for (uint32_t i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++) {
        const FLASH_INFO *p_info = &g_flash_table[i];
        printf("  - Model [%u]: JEDEC 0x%06X | ModelId %u | Size: %u KB | Addr: %uB\n",
               i, p_info->jedec_id, (uint32_t)p_info->model_id,
               (p_info->flash_size / 1024U), p_info->addr_bytes);
        /* Geometry is shared by every part (constants, not table fields) */
        assert(FLASH_PAGE_SIZE == 256U);
        assert(FLASH_SECTOR_SIZE == 4096U);
    }
    printf("  --> Table lookups PASSED.\n\n");
}

static void Test_InitAndProbe(void)
{
    printf("[TEST 2] Testing Flash_Init with various bus conditions...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0x00000000U;
    int status = Flash_Init();
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    printf("  - Floating bus 0x000000 correctly reported NO_CONNECT.\n");

    s_mock.mock_jedec_id = 0x00FFFFFFU;
    status = Flash_Init();
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    printf("  - Floating bus 0xFFFFFF correctly reported NO_CONNECT.\n");

    s_mock.mock_jedec_id = 0xAABBCCU;
    status = Flash_Init();
    assert(status == FLASH_ERR_UNSUPPORTED);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    printf("  - Unsupported ID 0xAABBCC correctly reported UNSUPPORTED.\n");

    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U); /* QE not set initially */
    status = Flash_Init();
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25Q256JV);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U); /* QE enabled! */
    assert(g_flash_handle.qe_enabled == true);
    printf("  - Valid W25Q256JV initialized, QE automatically enabled. PASSED.\n\n");
}

static void Test_GD25LQ64E_StatusWrite(void)
{
    printf("[TEST 3] Testing GD25LQ64E 2-Byte Status Register Write...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x1CU, 0x00U, 0x00U); /* SR1 has BP bits, QE=0 */

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_GD25LQ64E);
    assert(s_mock.sr1[0] == 0x1CU);                         /* SR1 preserved */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);   /* QE in SR2 set */
    printf("  - GD25LQ64E 2-byte WRSR handled properly. PASSED.\n\n");
}

static void Test_PageBoundarySplitting(void)
{
    uint8_t write_buffer[300];

    printf("[TEST 4] Testing 256-Byte Page Boundary Splitting in Flash_Write...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    for (uint32_t i = 0U; i < 300U; i++) {
        write_buffer[i] = (uint8_t)(i & 0xFFU);
    }

    s_mock.page_program_count = 0U;
    s_mock.total_bytes_programmed = 0U;

    status = Flash_Write(0x000000F0U, write_buffer, 300U, FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count == 3U);
    printf("  - 300 bytes cross-page write successfully split into %u chunks. PASSED.\n\n",
           s_mock.page_program_count);
}

static void Test_MultiDieHandling(void)
{
    uint8_t test_buf[64];

    printf("[TEST 5] Testing Winbond SpiStack Multi-Die (W25M512)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6119U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    s_mock.last_die_selected = 0xFFU;
    s_mock.die_select_count = 0U;

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25M512);
    assert((g_flash_handle.info->features & FLASH_FEAT_MULTI_DIE) != 0U);

    g_flash_handle.current_die = 0xFFU;

    memset(test_buf, 0xA5, sizeof(test_buf));
    status = Flash_Write(0x01000000U, test_buf, sizeof(test_buf), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.last_die_selected == 0U);
    printf("  - Address 16MB mapped to Die 0 (C2h sent with Die 0).\n");

    status = Flash_Write(0x02500000U, test_buf, sizeof(test_buf), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.last_die_selected == 1U);
    printf("  - Address 37MB automatically switched to Die 1 via C2h opcode.\n");
    printf("  - Multi-Die switching verified. PASSED.\n\n");
}

static void Test_SFCDMAReadSequence(void)
{
    uint32_t aligned_buf[1024]; /* 4096 bytes, 4-byte aligned */

    printf("[TEST 6] Testing Flash Read Units and SFC DMA Read...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* Alignment rejections */
    uint8_t *p_unaligned = ((uint8_t *)aligned_buf) + 1;
    status = Flash_Read(0x00001000U, p_unaligned, 32U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);

    status = Flash_Read(0x00001002U, aligned_buf, 32U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);
    printf("  - Word alignment checks verified with FLASH_ERR_ALIGNMENT.\n");

    /* Default unit resolves to WORD */
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_DEFAULT,
                        FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ_4B);  /* W25Q256JV is 4-Byte -> 0xEC */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    assert(g_p_sfc_regs->MODE == 4U);
    assert(g_p_sfc_regs->ACCESS_UNIT == 4U);
    printf("  - Default unit (0) resolved to Word (4 bytes) in SFC ACCESS_UNIT.\n");

    status = Flash_Read(0x00001000U, aligned_buf, 2048U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_HALFWORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->ACCESS_UNIT == 2U);

    status = Flash_Read(0x00001001U, aligned_buf, 10U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_HALFWORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);
    printf("  - Halfword unit and 2-byte alignment verified.\n");

    status = Flash_Read(0x00001001U, p_unaligned, 128U, FLASH_READ_FAST,
                        FLASH_READ_UNIT_BYTE, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->ACCESS_UNIT == 1U);
    printf("  - Byte unit tested.\n");

    /* Direct SPI with all units */
    status = Flash_Read(0x00001000U, aligned_buf, 64U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, aligned_buf, 128U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_HALFWORD, 0U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001001U, p_unaligned, 256U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_BYTE, 0U);
    assert(status == FLASH_OK);
    printf("  - Direct SPI Read with Word, Halfword, Byte verified.\n");

    status = Flash_Read(0x00001000U, aligned_buf, 10U, FLASH_READ_QUAD_IO,
                        (FLASH_READ_UNIT)3, 0U);
    assert(status == FLASH_ERR_PARAM);
    printf("  - Invalid unit rejected with FLASH_ERR_PARAM.\n");

    printf("  - Read unit and SFC DMA sequence test PASSED.\n\n");
}

/******************************************************************************
 * NEW TESTS (SW-013): boot model, family match, fail-safe, per-die QE,
 *                      CRC accumulation, direct-SPI guard.
 ******************************************************************************/
static void Test_BootConfigAndLock(void)
{
    printf("[TEST 7] Boot: ID check -> basic-read settings -> configure -> lock...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV present */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD); /* selects W25Q256JV */

    assert(Flash_IsLocked() == false);
    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    assert(Flash_IsLocked() == true);                 /* immutable */
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25Q256JV);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U); /* QE applied */

    /* Re-boot must be refused (selection is immutable) */
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_BUSY);
    printf("  - Boot configured from flash settings and locked. PASSED.\n\n");
}

static void Test_BootFamilyMatch(void)
{
    printf("[TEST 8] Boot: same product family accepted, foreign family refused...\n");

    /* Same family (Winbond EF40xx), different capacity: W25Q512JV selected,
     * W25Q256JV actually present -> both are family EF+0x40 -> ACCEPTED. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV present */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4020U, 4096U, FLASH_FEAT_QUAD); /* selects W25Q512JV */
    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    printf("  - Same-family device (W25Q256JV for W25Q512JV selection) ACCEPTED.\n");

    /* Foreign family: W25Q256JV selected, MX25L512 present -> FAIL-SAFE. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;                 /* MX25L512 present */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD); /* selects W25Q256JV */
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_UNSUPPORTED);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    assert(Flash_IsLocked() == false);
    printf("  - Foreign-family device (MX25L512 for W25Q256JV selection) REFUSED.\n");

    /* Corrupt settings block (bad CRC) -> fail-safe. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD);
    s_mock.config_buf[s_mock.config_len - 1U] ^= 0xFFU; /* flip CRC byte */
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_PARAM);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    printf("  - Corrupt settings block (bad CRC) REFUSED.\n");
    printf("  - Family matching / fail-safe verified. PASSED.\n\n");
}

static void Test_BootNoConnect(void)
{
    printf("[TEST 9] Boot: no ID response -> NO_CONNECT fail-safe...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0x00000000U;
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD);
    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    assert(Flash_IsLocked() == false);
    printf("  - Absent device reported NO_CONNECT. PASSED.\n\n");
}

static void Test_MultiDiePerDieConfig(void)
{
    printf("[TEST 10] G1: per-die QE applied on BOTH dies...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6119U;                 /* W25M512 (2-die) */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);              /* QE off on both dies */

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25M512);

    /* Both dies must now have QE set (each die has its own status regs). */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);
    assert((s_mock.sr2[1] & FLASH_SR2_QE_WINBOND) != 0U);
    printf("  - Die 0 and Die 1 both: QE enabled. PASSED.\n\n");
}

static void Test_CRCAccumulate(void)
{
    uint32_t aligned_buf[2048]; /* 8192 bytes */

    printf("[TEST 11] G3: CRC start/continue chaining + algorithm selection...\n");

    /* --- Chaining: (start N1) + (continue N2) == (start N1+N2) --- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* One-shot: 4096 bytes with CRC start. */
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_full = Flash_GetLastCRC();

    /* Chained: 2048 (start) then 2048 (continue). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    status = Flash_Init();
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, aligned_buf, 512U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_first = Flash_GetLastCRC();
    status = Flash_Read(0x00002000U, aligned_buf, 512U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC_CONT);
    assert(status == FLASH_OK);
    const uint32_t crc_chain = Flash_GetLastCRC();

    printf("  - one-shot=%08X first=%08X chained=%08X\n", crc_full, crc_first, crc_chain);
    assert(crc_full == crc_chain);      /* chaining == one-shot */
    assert(crc_first != crc_chain);     /* continue extended the CRC */
    printf("  - CRC chaining (start/continue) verified. ");

    /* --- Algorithm selection changes the result --- */
    g_flash_handle.crc_algo = FLASH_CRC_CHECKSUM;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_sum = Flash_GetLastCRC();

    g_flash_handle.crc_algo = FLASH_CRC32;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_32 = Flash_GetLastCRC();

    g_flash_handle.crc_algo = FLASH_CRC8;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_8 = Flash_GetLastCRC();

    printf("crc8=%08X crc32=%08X checksum=%08X\n", crc_8, crc_32, crc_sum);
    assert(crc_32 != crc_sum);
    assert(crc_8 != crc_32);
    printf("  - Algorithm selection (crc8/crc32/checksum) verified. PASSED.\n\n");
}

static void Test_DirectSPIQuadGuard(void)
{
    uint32_t aligned_buf[16];

    printf("[TEST 12] G4: Direct SPI refuses Dual/Quad (single-width only)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* Quad I/O over the direct SPI path must be refused (no silent wrong data). */
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_ERR_PARAM);

    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_DUAL,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_ERR_PARAM);

    /* Single / Fast on direct SPI remain allowed. */
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_FAST,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_OK);
    /* QPI (4-4-4) is listed in the read-mode enum but deliberately NOT
     * implemented. Both transfer paths must refuse it with a dedicated error
     * (not FLASH_ERR_PARAM) so a caller can tell "unknown mode" from
     * "known but unsupported". */
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_QPI,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_ERR_UNSUPPORTED);

    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_QPI,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC);
    assert(status == FLASH_ERR_UNSUPPORTED);
    printf("  - QPI (4-4-4) listed but refused with FLASH_ERR_UNSUPPORTED.\n");
    printf("  - Direct SPI guard verified. PASSED.\n\n");
}

static void Test_ProtectUnprotect(void)
{
    printf("[TEST 13] G2: Protect / Unprotect (block-protect bits)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    status = Flash_Protect(0x0FU);
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x3CU);          /* BP0..BP3 set */

    status = Flash_Unprotect();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);          /* BP cleared */
    printf("  - Protect sets BP bits, Unprotect clears them. PASSED.\n\n");
}

static void Test_WriteClearsBP(void)
{
    uint8_t write_buffer[8];
    printf("[TEST 14] G2: Write auto-clears block-protect (no silent failure)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x1CU, 0x02U, 0x00U);              /* SR1 has BP0..BP2 set */
    int status = Flash_Init();
    assert(status == FLASH_OK);

    memset(write_buffer, 0x5A, sizeof(write_buffer));
    s_mock.page_program_count = 0U;
    status = Flash_Write(0x00000000U, write_buffer, sizeof(write_buffer), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count >= 1U);          /* program happened */
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);         /* BP cleared before write */
    printf("  - Write succeeded and cleared BP bits. PASSED.\n\n");
}

static void Test_AddrModeOpcodes(void)
{
    uint32_t buf[4];
    printf("[TEST 15] Req2: 3-Byte vs 4-Byte opcode selection...\n");

    /* 4-Byte device (W25Q256JV): Quad I/O read must use the 4B opcode (0xEC). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, buf, 4U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ_4B);   /* 0xEC */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    printf("  - 4-Byte device used Quad I/O 4B opcode 0x%02X.\n", g_p_sfc_regs->CMD);

    /* 3-Byte device (W25Q128JW): Quad I/O read must use the 3B opcode (0xEB). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6018U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    status = Flash_Init();
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, buf, 4U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ);       /* 0xEB */
    assert(g_p_sfc_regs->ADDR_SIZE == 3U);
    printf("  - 3-Byte device used Quad I/O 3B opcode 0x%02X.\n", g_p_sfc_regs->CMD);

    /* Method B (Enter-4-Byte via B7h): same 3B opcode + 4B address + B7h sent. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    FLASH_BOOT_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic         = FLASH_BOOT_CFG_MAGIC;
    cfg.jedec_expect  = 0xEF4019U;
    cfg.model         = (uint32_t)FLASH_MODEL_UNKNOWN;
    cfg.features      = FLASH_FEAT_QUAD;
    cfg.sfc_page_size = 4096U;
    cfg.addr_mode     = (uint8_t)FLASH_ADDR_4BYTE_ENTER;
    cfg.crc           = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, buf, 4U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ);       /* 0xEB (3B opcode) */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    assert(s_mock.entered_4byte[0] == true);             /* B7h sent */
    printf("  - Method B (Enter-4B) used 3B opcode 0x%02X + sent B7h. PASSED.\n\n", g_p_sfc_regs->CMD);
}

static void Test_InitNoReset(void)
{
    printf("[TEST 16] G7: Init option skips software reset (fast boot)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    s_mock.reset_cmd_count = 0U;
    int status = Flash_InitOpt(FLASH_INIT_OPT_NO_RESET);
    assert(status == FLASH_OK);
    assert(s_mock.reset_cmd_count == 0U);
    printf("  - NO_RESET: no reset commands sent.\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    s_mock.reset_cmd_count = 0U;
    status = Flash_InitOpt(FLASH_INIT_OPT_NONE);
    assert(status == FLASH_OK);
    assert(s_mock.reset_cmd_count >= 2U);             /* 66h + 99h */
    printf("  - default: reset commands (66h/99h) sent. PASSED.\n\n");
}

static void Test_TableReadCfgGuard(void)
{
    printf("[TEST 17] G6: shared read_cfg values preserved after dedup...\n");

    const FLASH_INFO *w25q256 = Flash_FindDevice(0xEF4019U);
    assert(w25q256 != NULL);
    assert(w25q256->read_cfg[FLASH_READ_QUAD_IO].dummy_cycles == 8U);
    assert(w25q256->read_cfg[FLASH_READ_SINGLE].command == F_CMD_READ);
    assert(w25q256->read_cfg[FLASH_READ_SINGLE].command_4b == F_CMD_READ_4B);
    assert(w25q256->read_cfg[FLASH_READ_QUAD_IO].command == F_CMD_QUAD_IO_READ);
    assert(w25q256->read_cfg[FLASH_READ_QUAD_IO].command_4b == F_CMD_QUAD_IO_READ_4B);

    const FLASH_INFO *gd = Flash_FindDevice(0xC86017U);
    assert(gd != NULL);
    assert(gd->model_id == FLASH_MODEL_GD25LQ64E);
    assert(gd->read_cfg[FLASH_READ_DUAL_IO].dummy_cycles == 6U);
    assert(gd->read_cfg[FLASH_READ_QUAD_IO].dummy_cycles == 6U);

    const FLASH_INFO *mx = Flash_FindDevice(0xC22010U);
    assert(mx != NULL);
    assert(mx->read_cfg[FLASH_READ_QUAD_IO].dummy_cycles == 6U);
    printf("  - Per-device DUAL_IO / QUAD_IO dummy values intact. PASSED.\n\n");
}

static void Test_VsrWriteEnable(void)
{
    printf("[TEST 18] WREN 50h (volatile) vs 06h selection...\n");

    /* W25Q256JV has FLASH_FEAT_VSR_WREN -> status writes use 50h */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteEnable() == FLASH_OK);
    assert(s_mock.wren_count == 1U && s_mock.vsr_wren_count == 0U);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteEnableVolatile() == FLASH_OK);
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteStatus(1U, 0x00U) == FLASH_OK);
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);
    printf("  - W25Q256JV (VSR_WREN): WriteEnable=06h, Volatile=50h, StatusWrite=50h.\n");

    /* MX25L512 has no VSR flag -> status writes fall back to 06h */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteStatus(1U, 0x00U) == FLASH_OK);
    assert(s_mock.wren_count == 1U && s_mock.vsr_wren_count == 0U);
    printf("  - MX25L512 (no flag): StatusWrite=06h fallback. PASSED.\n\n");
}

static void Test_FlashIdxAccess(void)
{
    printf("[TEST 19] FLASH_IDX table access pattern...\n");

    /* Owner's usage: Flash_FindDevice(g_flash_table[FLASH_IDX_W25Q256JV].jedec_id) */
    const FLASH_INFO *w25q256 = Flash_FindDevice(g_flash_table[FLASH_IDX_W25Q256JV].jedec_id);
    assert(w25q256 != NULL);
    assert(w25q256->jedec_id == 0xEF4019U);
    assert(w25q256->model_id == FLASH_MODEL_W25Q256JV);

    /* Index order must match the FLASH_IDX enum exactly */
    assert(g_flash_table[FLASH_IDX_W25Q128JW].jedec_id == 0xEF6018U);
    assert(g_flash_table[FLASH_IDX_W25Q512JV].jedec_id == 0xEF4020U);
    assert(g_flash_table[FLASH_IDX_W25Q256JV].jedec_id == 0xEF4019U);
    assert(g_flash_table[FLASH_IDX_W25M512].jedec_id == 0xEF6119U);
    assert(g_flash_table[FLASH_IDX_MX25L512].jedec_id == 0xC22010U);
    assert(g_flash_table[FLASH_IDX_MX25L512MB].jedec_id == 0xC2201AU);
    assert(g_flash_table[FLASH_IDX_GD25LQ64E].jedec_id == 0xC86017U);
    printf("  - FLASH_IDX order + FindDevice access verified. PASSED.\n\n");
}

static void Test_CrcAlgoBootSelect(void)
{
    uint32_t aligned_buf[1024];

    printf("[TEST 20] CRC algorithm selected from boot config...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD);

    /* Override crc_algo = CHECKSUM in the settings block and fix its CRC */
    FLASH_BOOT_CONFIG cfg;
    memcpy(&cfg, s_mock.config_buf, sizeof(cfg));
    cfg.crc_algo = (uint8_t)FLASH_CRC_CHECKSUM;
    cfg.crc = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));

    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(g_flash_handle.crc_algo == FLASH_CRC_CHECKSUM);

    /* Read with CRC: the selected CHECKSUM algorithm is used ... */
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_sum = Flash_GetLastCRC();

    /* ... and it differs from the CRC32 default */
    g_flash_handle.crc_algo = FLASH_CRC32;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_32 = Flash_GetLastCRC();

    printf("  - crc_algo=CHECKSUM from config: checksum=%08X vs crc32=%08X\n", crc_sum, crc_32);
    assert(crc_sum != crc_32);
    printf("  - Boot-config CRC algorithm selection verified. PASSED.\n\n");
}

static void Test_EasyApi(void)
{
    uint8_t buf[256];

    printf("[TEST 21] Easy API: ReadBytes / WriteBytes / EraseRange / EraseChip...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV (quad) */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    /* Convenience accessors */
    assert(Flash_SizeBytes() == 32U * 1024U * 1024U);
    assert(Flash_SectorSize() == 4096U);

    /* Byte-granular read at any address (auto mode, any alignment) */
    assert(Flash_ReadBytes(0x00001000U, buf, sizeof(buf)) == FLASH_OK);

    /* Erase a region then program it */
    assert(Flash_EraseRange(0x00002000U, 4096U) == FLASH_OK);   /* 1 sector */
    assert(Flash_WriteBytes(0x00002000U, buf, sizeof(buf)) == FLASH_OK);

    /* Unaligned address / odd length are fine (auto rounding + byte unit) */
    assert(Flash_EraseRange(0x00003123U, 100U) == FLASH_OK);
    assert(Flash_ReadBytes(0x00003123U, buf, 100U) == FLASH_OK);
    assert(Flash_WriteBytes(0x00003123U, buf, 100U) == FLASH_OK);

    /* Whole-chip erase */
    assert(Flash_EraseChip() == FLASH_OK);
    printf("  - Easy API round-trip (any addr/len, auto mode) verified. PASSED.\n\n");
}

static void Test_DelayUsConversion(void)
{
    printf("[TEST 22] Time-based delay: us -> loop conversion (SW-014)...\n");

    /* Defaults: FLASH_SYS_CLK_HZ = 108MHz, CYCLES_PER_LOOP = 3 -> 36 loops/us */
    assert(Flash_SysClockGet() == FLASH_SYS_CLK_HZ);
    assert(Flash_DelayUsToLoops(0U) == 0U);
    assert(Flash_DelayUsToLoops(1U) == 36U);
    assert(Flash_DelayUsToLoops(10U) == 360U);
    assert(Flash_DelayUsToLoops(1000U) == 36000U);
    printf("  - 108MHz default: 1us->36 loops, 1ms->36000 loops. PASSED.\n");

    /* 50MHz target: ceil(50/3) = 17 loops/us (round-up = never under-delays) */
    Flash_SysClockSet(50000000U);
    assert(Flash_SysClockGet() == 50000000U);
    assert(Flash_DelayUsToLoops(1U) == 17U);
    assert(Flash_DelayUsToLoops(1000U) == 17000U);
    printf("  - 50MHz: 1us->17 loops (ceil rounding). PASSED.\n");

    /* Invalid clock (0) is ignored */
    Flash_SysClockSet(0U);
    assert(Flash_SysClockGet() == 50000000U);
    printf("  - Clock guard: hz=0 ignored. PASSED.\n");

    /* Smoke: delays execute and return */
    Flash_DelayUs(1U);
    Flash_DelayUs(1000U);
    Flash_DelayLoops(10U);

    /* Restore default for any later suites */
    Flash_SysClockSet(FLASH_SYS_CLK_HZ);
    assert(Flash_DelayUsToLoops(1U) == 36U);

    printf("  - Time-based delay API verified. PASSED.\n");
}

static void Test_VsrWrenNoWel(void)
{
    uint8_t sr = 0U;

    printf("[TEST 23] 50h does NOT set WEL - volatile SR write (datasheet)...\n");

    /* GD25LQ64E datasheet 7.5: "The Write Enable for Volatile Status Register
     * command will not set the Write Enable Latch bit, it is only valid for
     * the Write Status Register command". Winbond W25Q series is identical. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;   /* init may enable QE */
    assert(Flash_WriteEnableVolatile() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) == 0U);      /* WEL must stay 0 */
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);
    printf("  - GD25LQ64E: 50h issued, WEL remains 0. PASSED.\n");

    /* Volatile SR2 write must succeed with WEL=0 via the pending 50h */
    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteStatus(2U, 0x02U) == FLASH_OK);
    assert(s_mock.sr2[0] == 0x02U);
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) == 0U);
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);
    printf("  - Volatile WRSR accepted with WEL=0 (pending 50h). PASSED.\n");

    /* GD rule: an intervening command clears the pending 50h */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_WriteEnableVolatile() == FLASH_OK);
    assert(Flash_ReadStatus(1U, &sr) == FLASH_OK);
    assert(s_mock.vsr_wren_pending == false);
    printf("  - Intervening command clears pending 50h. PASSED.\n\n");
}

static void Test_DummyCfgPerModel(void)
{
    static const struct { uint32_t jedec; uint8_t dummy_last; } models[] = {
        { 0xEF6018U, 0xFFU },   /* W25Q128JW  - Winbond: first dummy Fxh */
        { 0xEF4020U, 0xFFU },   /* W25Q512JV  - Winbond: first dummy Fxh */
        { 0xEF4019U, 0xFFU },   /* W25Q256JV  - Winbond: first dummy Fxh */
        { 0xEF6119U, 0xFFU },   /* W25M512    - Winbond: first dummy Fxh */
        { 0xC22010U, 0x00U },   /* MX25L512   - no mode byte */
        { 0xC2201AU, 0x00U },   /* MX25L51245G- no mode byte */
        { 0xC86017U, 0x00U }    /* GD25LQ64E  - M7-0 != Axh */
    };

    printf("[TEST 24] Per-model dummy data / dummy last (SW-013 DUMMY)...\n");

    for (uint32_t i = 0U; i < sizeof(models) / sizeof(models[0]); i++) {
        const FLASH_INFO *info = Flash_FindDevice(models[i].jedec);
        assert(info != NULL);

        /* Wire order: CMD -> ADDR -> DUMMY_LAST (first dummy = M7-M0)
         * -> DUMMY_VAL -> DATA. Trailing dummy bytes are don't care. */
        assert(info->read_cfg[FLASH_READ_DUAL_IO].dummy_last  == models[i].dummy_last);
        assert(info->read_cfg[FLASH_READ_DUAL_IO].dummy_value == 0x00U);
        assert(info->read_cfg[FLASH_READ_QUAD_IO].dummy_last  == models[i].dummy_last);
        assert(info->read_cfg[FLASH_READ_QUAD_IO].dummy_value == 0x00U);

        /* mode_byte must stay 0x00: M7-M0 rides on DUMMY_LAST.
         * (0x20 would enter Winbond Read Command Bypass / GD AXh rules) */
        assert(info->read_cfg[FLASH_READ_DUAL_IO].mode_byte == 0x00U);
        assert(info->read_cfg[FLASH_READ_QUAD_IO].mode_byte == 0x00U);
    }
    printf("  - 7 models: dummy_last 0xFF (Winbond Fxh) / 0x00 (MX, GD). PASSED.\n");
    printf("  - mode_byte 0x00 everywhere (M7-M0 on DUMMY_LAST). PASSED.\n\n");
}

static void Test_BufferedXferAndSharedGeometry(void)
{
    uint32_t buf[64];       /* 256 bytes, 4-byte aligned */
    uint8_t  pg[16];

    printf("[TEST 25] SW-016: one-frame buffered SPI transfer + shared geometry...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;      /* GD25LQ64E: 3-byte address */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(g_flash_handle.addr_mode == FLASH_ADDR_3BYTE);

    /* --- Direct read: ONE transaction carries header AND data --- */
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Read(0x00001000U, buf, 64U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_OK);
    assert(s_xfer.frames == 2U);           /* 1 BUSY poll + 1 data frame */
    assert(s_xfer.last_first_tx == F_CMD_READ);
    assert(s_xfer.last_tx_len == 4U);      /* 03h + 3-byte address, no dummy */
    assert(s_xfer.last_rx_len == 256U);    /* 64 words */
    assert(s_xfer.last_data_len == 0U);
    printf("  - Read: header+data in 1 frame (tx_len=%u, rx_len=%u).\n",
           s_xfer.last_tx_len, s_xfer.last_rx_len);

    /* --- Fast read: the dummy byte rides inside the same header --- */
    const uint32_t fast_dummy = (uint32_t)g_flash_handle.info->read_cfg[FLASH_READ_FAST].dummy_bytes;
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Read(0x00001000U, buf, 8U, FLASH_READ_FAST,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_OK);
    assert(s_xfer.frames == 2U);           /* 1 BUSY poll + 1 data frame */
    assert(s_xfer.last_tx_len == (1U + 3U + fast_dummy));
    printf("  - Fast read: %u dummy byte(s) packed into tx_len=%u.\n",
           fast_dummy, s_xfer.last_tx_len);

    /* --- Page program: header + separate data phase, still one CS frame --- */
    memset(pg, 0xA5, sizeof(pg));
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Write(0x00002000U, pg, sizeof(pg), FLASH_PROGRAM_SINGLE) == FLASH_OK);
    assert(s_xfer.last_payload_len == (uint32_t)sizeof(pg));
    assert(s_xfer.last_payload_tx_len == 4U);   /* 02h + 3-byte address */
    assert(s_xfer.last_data_payload_len == (uint32_t)sizeof(pg));
    printf("  - Program: payload of %u bytes in the same frame (tx_len=%u).\n",
           s_xfer.last_payload_len, s_xfer.last_payload_tx_len);

    /* --- Quad page program: same frame, payload on the 4-bit phase --- */
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Write(0x00003000U, pg, sizeof(pg), FLASH_PROGRAM_QUAD) == FLASH_OK);
    assert(s_xfer.last_quad_payload_len == (uint32_t)sizeof(pg));
    assert(s_xfer.last_data_payload_len == 0U);
    printf("  - Quad program: %u bytes on the 4-bit phase (tx_len=%u).\n",
           s_xfer.last_quad_payload_len, s_xfer.last_payload_tx_len);

    /* --- Shared geometry helper: both paths reject identical bad input --- */
    uint8_t *misaligned = ((uint8_t *)buf) + 1U;
    assert(Flash_Read(0x00001000U, misaligned, 4U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_ERR_ALIGNMENT);
    assert(Flash_SFC_DMARead(0x00001000U, misaligned, 4U,
                             FLASH_READ_UNIT_WORD, false, NULL) == FLASH_ERR_ALIGNMENT);

    /* count * unit must not wrap a 32-bit size (0x40000001 * 4) */
    assert(Flash_Read(0x00001000U, buf, 0x40000001U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_ERR_PARAM);
    assert(Flash_SFC_DMARead(0x00001000U, buf, 0x40000001U,
                             FLASH_READ_UNIT_WORD, false, NULL) == FLASH_ERR_PARAM);
    printf("  - Unit/alignment/overflow checks are shared by SPI and SFC paths.\n");

    /* --- Real-hardware SFC path: software must not edit DMA data or CRC --- */
    static SFC_TypeDef hw_regs;
    SFC_TypeDef *saved_regs = g_p_sfc_regs;
    uint32_t hw_buf[4];                    /* 16 bytes, 4-byte aligned */

    memset(&hw_regs, 0, sizeof(hw_regs));
    hw_regs.STATUS = SFC_STATUS_DMA_DONE;  /* DMA already completed */
    g_p_sfc_regs = &hw_regs;

    for (uint32_t i = 0U; i < 4U; i++) {
        hw_buf[i] = 0x5A5A5A5AU;
    }
    uint32_t crc_out = 0xDEADBEEFU;
    assert(Flash_SFC_DMARead(0x00001000U, hw_buf, 4U,
                             FLASH_READ_UNIT_WORD, true, &crc_out) == FLASH_OK);
    for (uint32_t i = 0U; i < 4U; i++) {
        assert(hw_buf[i] == 0x5A5A5A5AU);  /* HW-owned payload untouched */
    }
    assert(hw_regs.CRC_RES == 0U);         /* SW did not edit the CRC register */
    assert(crc_out == 0U);

    g_p_sfc_regs = saved_regs;
    printf("  - Real-HW SFC path: DMA buffer and CRC_RES untouched by software. PASSED.\n\n");
}

static void Test_NoConnectRefusesDataPath(void)
{
    uint8_t buf[16];

    printf("[TEST 26] NO_CONNECT: data path refuses and issues no SPI frames...\n");

    /* 1. A working device fills the handle */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    /* 2. The device disappears; re-init detects it and enters NO_CONNECT.
     *    The handle still holds the previous model, which is exactly why the
     *    data path must look at the driver state, not only at info != NULL. */
    s_mock.mock_jedec_id = 0x000000U;
    assert(Flash_Init() == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);

    /* 3. Nothing may reach the bus from here on */
    memset(&s_xfer, 0, sizeof(s_xfer));
    memset(buf, 0x00, sizeof(buf));

    assert(Flash_Read(0x00001000U, buf, 4U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Write(0x00001000U, buf, sizeof(buf),
                       FLASH_PROGRAM_SINGLE) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Erase(0x00001000U, FLASH_ERASE_SECTOR) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Protect(0x0FU) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Unprotect() == FLASH_ERR_NO_CONNECT);

    assert(s_xfer.frames == 0U);          /* no SPI transaction was started */

    printf("  - Read/Write/Erase/Protect refused, %u SPI frames. PASSED.\n\n",
           s_xfer.frames);
}

/* SW-019: GD25LQ64E는 01h가 "고정 16비트" 명령이다. 8비트에서 CS를 올리면
 * SR1은 써지지만 QE/CMP가 0으로 클리어된다(데이터시트 7.4). 따라서 SR1을
 * 바꾸는 모든 경로(Protect/Unprotect/Write 내부 Unprotect)는 반드시
 * "SR2를 읽어 그대로 되돌려 보내는 3바이트 프레임"이어야 한다. */
static void Test_GdCoupledWriteKeepsQe(void)
{
    uint8_t data[8];

    printf("[TEST 27] SW-019: GD 16-bit WRSR keeps QE alive on SR1 changes...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);   /* Init이 QE를 켠다 */

    /* (1) BP0 설정 - SR1이 바뀌어도 QE는 살아 있어야 한다. */
    status = Flash_Protect(0x01U);
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x04U);                        /* BP0만 set */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);            /* QE 유지 */
    assert(s_mock.wrsr1_frame_bytes == 3U);                          /* opcode+SR1+SR2 */

    /* (2) 실제 쓰기 경로 - Flash_Write는 페이지마다 Unprotect를 부른다.
     *     여기서 QE가 꺼지면 이후 quad 읽기가 전부 쓰레기가 된다. */
    memset(data, 0x5AU, sizeof(data));
    s_mock.page_program_count = 0U;
    status = Flash_Write(0x00000000U, data, sizeof(data), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count >= 1U);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);            /* QE 유지 */
    assert(g_flash_handle.qe_enabled);                               /* 핸들도 일치 */

    /* (3) Unprotect도 같은 프레임을 써야 한다. */
    status = Flash_Unprotect();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);
    assert(s_mock.wrsr1_frame_bytes == 3U);

    /* (4) 이 부품에는 11h(SR3 단독 쓰기)가 없다 - 무음 처리하지 않고 거부. */
    assert(Flash_WriteStatus(3U, 0x00U) == FLASH_ERR_UNSUPPORTED);

    printf("  - SR1 changed, QE survived, every 01h frame was 3 bytes. PASSED.\n\n");
}

/* SW-019: GD25LQ64E SR1의 S6은 BP4(다섯 번째 블록 보호 비트)다.
 * MX25L 계열에서 같은 비트는 QE이므로, 모델 플래그 없이 지우면 quad 모드가
 * 죽는다. 두 부품을 나란히 검증한다. */
static void Test_Bp4VersusQeBit(void)
{
    printf("[TEST 28] SW-019: S6 = BP4 on GD, but QE on Macronix...\n");

    /* (1) GD25LQ64E - BP4가 걸린 부품을 Unprotect하면 실제로 풀려야 한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x40U, 0x00U, 0x00U);                    /* BP4=1, 전체 보호 */
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_Unprotect() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_BP4) == 0U);          /* BP4 해제됨 */
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);

    assert(Flash_Protect(0x10U) == FLASH_OK);               /* mask bit4 -> BP4 */
    assert((s_mock.sr1[0] & FLASH_SR1_BP4) != 0U);

    /* (2) MX25L512 - 같은 비트가 QE다. Protect/Unprotect가 건드리면 안 된다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_QE_MX25L) != 0U);     /* Init이 QE(S6)를 켠다 */

    assert(Flash_Protect(0x0FU) == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_QE_MX25L) != 0U);     /* QE 유지 */
    assert((s_mock.sr1[0] & 0x3CU) == 0x3CU);               /* BP는 설정됨 */

    assert(Flash_Unprotect() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_QE_MX25L) != 0U);     /* QE 여전히 유지 */
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);

    printf("  - BP4 clears on GD and S6=QE survives on Macronix. PASSED.\n\n");
}

/* SW-020: WEL(S1)과 BUSY(S0)는 같은 05h 응답의 서로 다른 비트다.
 * WEL만 보고 돌아가면 칩이 아직 쓰기 중일 때도 "준비됨"으로 판정하고,
 * 뒤따르는 program 명령은 조용히 사라진다. 또한 상태 레지스터 쓰기는
 * read-back으로 실제 반영 여부를 확인해야 한다. */
static void Test_BusyGateAndStatusReadBack(void)
{
    printf("[TEST 29] SW-020: WEL+BUSY from one 05h read, WRSR read-back...\n");

    /* (1) WEL=1 + WIP=1 동시 상태. 옛 코드는 이 첫 읽기에서 OK를 돌려줬다
     *     (WEL만 검사했으므로). 지금은 BUSY가 풀릴 때까지 기다려야 한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;              /* W25Q64: 표준 1바이트 경로 */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    assert(Flash_WriteEnable() == FLASH_OK);       /* 먼저 정상적으로 WEL을 세운다 */
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) != 0U);

    s_mock.busy_reads_remaining = 3U;              /* 자체 타이밍 사이클 진행 중 */
    s_mock.wren_count = 0U;
    assert(Flash_WriteEnable() == FLASH_OK);
    assert(s_mock.busy_reads_remaining == 0U);     /* BUSY가 풀릴 때까지 기다렸다 */
    assert(s_mock.wren_count == 4U);               /* 3회는 busy로 무시, 4회째 성공 */

    /* (2) WEL=0 + busy 구간. WREN이 무시되므로 재시도해야만 성공한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.busy_reads_remaining = 2U;
    s_mock.wren_count = 0U;
    assert(Flash_WriteEnable() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) != 0U);
    assert(s_mock.busy_reads_remaining == 0U);
    assert(s_mock.wren_count == 3U);

    /* (3) 조용히 사라진 WRSR은 read-back이 잡아낸다. 옛 코드는 OK를 돌려줬다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.drop_next_wrsr = true;
    assert(Flash_WriteStatus(1U, 0x04U) == FLASH_ERR_WRITE_FAIL);
    assert((s_mock.sr1[0] & 0x04U) == 0x00U);      /* 실제로 반영되지 않았다 */

    /* (4) 정상 쓰기는 그대로 통과하고 값이 반영된다. */
    assert(Flash_WriteStatus(1U, 0x04U) == FLASH_OK);
    assert((s_mock.sr1[0] & 0x04U) != 0U);

    printf("  - Waited out BUSY, then proved the dropped WRSR. PASSED.\n\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: show progress even on assert */

    printf("================================================================\n");
    printf(" Generic SPI NOR Flash Driver Verification Suite (ARM Cortex-M0)\n");
    printf("================================================================\n\n");

    Test_TableLookup();
    Test_InitAndProbe();
    Test_GD25LQ64E_StatusWrite();
    Test_PageBoundarySplitting();
    Test_MultiDieHandling();
    Test_SFCDMAReadSequence();
    Test_BootConfigAndLock();
    Test_BootFamilyMatch();
    Test_BootNoConnect();
    Test_MultiDiePerDieConfig();
    Test_CRCAccumulate();
    Test_DirectSPIQuadGuard();
    Test_ProtectUnprotect();
    Test_WriteClearsBP();
    Test_AddrModeOpcodes();
    Test_InitNoReset();
    Test_TableReadCfgGuard();
    Test_VsrWriteEnable();
    Test_FlashIdxAccess();
    Test_CrcAlgoBootSelect();
    Test_EasyApi();
    Test_DelayUsConversion();
    Test_VsrWrenNoWel();
    Test_DummyCfgPerModel();
    Test_BufferedXferAndSharedGeometry();
    Test_NoConnectRefusesDataPath();
    Test_GdCoupledWriteKeepsQe();
    Test_Bp4VersusQeBit();
    Test_BusyGateAndStatusReadBack();

    printf("================================================================\n");
    printf(" ALL 29 VERIFICATION TEST SUITES PASSED SUCCESSFULLY (100%%)!\n");
    printf("================================================================\n");

    return 0;
}
