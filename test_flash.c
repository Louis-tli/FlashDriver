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

    /* Boot settings block served on CMD_READ (03h) */
    uint8_t  config_buf[128];
    uint32_t config_len;

    /* Reset (66h/99h) command counter for init-option testing */
    uint32_t reset_cmd_count;
} MOCK_SPI_STATE;

static MOCK_SPI_STATE s_mock;

/* Reset mock to a clean state (both dies). Also clear the driver handle so
 * each test starts from a fresh, unlocked driver. */
static void Mock_Reset(void)
{
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.last_die_selected = 0xFFU;

    g_flash_handle.info          = NULL;
    g_flash_handle.state         = FLASH_STATE_UNINIT;
    g_flash_handle.current_die   = 0U;
    g_flash_handle.qe_enabled    = false;
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;
    g_flash_handle.locked        = false;
    g_flash_handle.last_crc      = 0U;
}

/* Set the status registers for both dies (keeps old single-value call sites simple). */
static void Mock_SetStatus(uint8_t sr1, uint8_t sr2, uint8_t sr3)
{
    for (int d = 0; d < 2; d++)
    {
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
    cfg.reserved     = 0U;
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

    if (s_mock.cmd_byte_index == 0U)
    {
        s_mock.current_cmd = tx_byte;
        s_mock.cmd_byte_index = 1U;

        if (tx_byte == CMD_WREN)
        {
            s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
        }
        else if (tx_byte == CMD_WRDI)
        {
            s_mock.sr1[s_mock.current_die] &= (uint8_t)(~FLASH_SR1_WEL);
        }
        else if (tx_byte == CMD_ENTER_4BYTE_ADDR)
        {
            s_mock.entered_4byte[s_mock.current_die] = true;
        }
        else if ((tx_byte == CMD_RESET_ENABLE) || (tx_byte == CMD_RESET_DEVICE))
        {
            s_mock.reset_cmd_count++;
        }
        return 0x00U;
    }

    switch (s_mock.current_cmd)
    {
        case CMD_RDID: /* 9Fh */
            if (s_mock.cmd_byte_index == 1U)
            {
                rx_byte = (uint8_t)((s_mock.mock_jedec_id >> 16) & 0xFFU);
            }
            else if (s_mock.cmd_byte_index == 2U)
            {
                rx_byte = (uint8_t)((s_mock.mock_jedec_id >> 8) & 0xFFU);
            }
            else if (s_mock.cmd_byte_index == 3U)
            {
                rx_byte = (uint8_t)(s_mock.mock_jedec_id & 0xFFU);
            }
            s_mock.cmd_byte_index++;
            break;

        case CMD_READ: /* 03h : serve the boot settings block after cmd + 3 addr */
            if (s_mock.cmd_byte_index >= 4U)
            {
                const uint32_t idx = s_mock.cmd_byte_index - 4U;
                rx_byte = (idx < s_mock.config_len) ? s_mock.config_buf[idx] : 0xFFU;
            }
            else
            {
                rx_byte = 0x00U; /* address phase */
            }
            s_mock.cmd_byte_index++;
            break;

        case CMD_RDSR1: /* 05h */
            rx_byte = s_mock.sr1[s_mock.current_die];
            break;
        case CMD_RDSR2: /* 35h */
            rx_byte = s_mock.sr2[s_mock.current_die];
            break;
        case CMD_RDSR3: /* 15h */
            rx_byte = s_mock.sr3[s_mock.current_die];
            break;

        case CMD_WRSR1: /* 01h : 1-byte (SR1) or 2-byte (SR1+SR2) */
            if (s_mock.cmd_byte_index == 1U)
            {
                s_mock.sr1[s_mock.current_die] = tx_byte;
                s_mock.cmd_byte_index++;
            }
            else if (s_mock.cmd_byte_index == 2U)
            {
                s_mock.sr2[s_mock.current_die] = tx_byte;
                s_mock.cmd_byte_index++;
            }
            break;

        case CMD_WRSR2: /* 31h */
            s_mock.sr2[s_mock.current_die] = tx_byte;
            break;

        case CMD_WREN: /* 06h */
            s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
            break;

        case CMD_WRDI: /* 04h */
            s_mock.sr1[s_mock.current_die] &= (uint8_t)(~FLASH_SR1_WEL);
            break;

        case CMD_WINBOND_DIE_SELECT: /* C2h : next byte selects the die */
            s_mock.current_die = tx_byte;
            s_mock.last_die_selected = tx_byte;
            s_mock.die_select_count++;
            break;

        case CMD_PAGE_PROGRAM:       /* 02h */
        case CMD_QUAD_PAGE_PROGRAM:  /* 32h */
            if (s_mock.cmd_byte_index == 1U)
            {
                s_mock.page_program_count++;
            }
            if (s_mock.cmd_byte_index >= 4U)
            {
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

    for (uint32_t i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++)
    {
        const FLASH_INFO *p_info = &g_flash_table[i];
        printf("  - Model [%u]: %-22s | JEDEC: 0x%06X | Size: %u KB | Addr: %uB\n",
               i, p_info->name, p_info->jedec_id, (p_info->flash_size / 1024U), p_info->addr_bytes);
        assert(p_info->page_size == 256U);
        assert(p_info->sector_size == 4096U);
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

    for (uint32_t i = 0U; i < 300U; i++)
    {
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
    assert(g_p_sfc_regs->CMD == CMD_QUAD_IO_READ);
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
    printf("[TEST 10] G1: per-die QE + 4-Byte mode applied on BOTH dies...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6119U;                 /* W25M512 (2-die) */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);              /* QE off on both dies */

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25M512);

    /* Both dies must now have QE set and 4-Byte mode entered. */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);
    assert((s_mock.sr2[1] & FLASH_SR2_QE_WINBOND) != 0U);
    assert(s_mock.entered_4byte[0] == true);
    assert(s_mock.entered_4byte[1] == true);
    printf("  - Die 0 and Die 1 both: QE enabled, 4-Byte mode entered. PASSED.\n\n");
}

static void Test_CRCAccumulate(void)
{
    uint32_t aligned_buf[2048]; /* 8192 bytes */

    printf("[TEST 11] G3: CRC is returned and accumulated across chunks...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* Read 8192 bytes via SFC with CRC. SFC page = 4096 -> 2 chunks.
     * The simulation folds each transferred length into the accumulator, so
     * the exposed CRC equals the total transferred bytes (not discarded). */
    status = Flash_Read(0x00001000U, aligned_buf, 2048U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);

    const uint32_t crc = Flash_GetLastCRC();
    printf("  - Flash_GetLastCRC() = %u (expected 8192).\n", crc);
    assert(crc == 8192U);   /* 2 x 4096 accumulated across chunks */
    printf("  - CRC accumulated across chunks and exposed. PASSED.\n\n");
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

static void Test_BootSelectsPlutoVariant(void)
{
    printf("[TEST 15] G5: boot config model selects GD25LQ64E_PLUTO_SFC...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);

    FLASH_BOOT_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic        = FLASH_BOOT_CFG_MAGIC;
    cfg.jedec_expect = 0xC86017U;
    cfg.model        = (uint32_t)FLASH_MODEL_GD25LQ64E_PLUTO_SFC;
    cfg.features     = FLASH_FEAT_QUAD;
    cfg.sfc_page_size = 4096U;
    cfg.crc          = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);

    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_GD25LQ64E_PLUTO_SFC);
    printf("  - Config model field selected the PLUTO_SFC variant. PASSED.\n\n");
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
    assert(w25q256->read_cfg[FLASH_READ_SINGLE].command == CMD_READ);

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
    Test_BootSelectsPlutoVariant();
    Test_InitNoReset();
    Test_TableReadCfgGuard();

    printf("================================================================\n");
    printf(" ALL 17 VERIFICATION TEST SUITES PASSED SUCCESSFULLY (100%%)!\n");
    printf("================================================================\n");

    return 0;
}
