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

        if (tx_byte == F_CMD_WREN) {
            s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
            s_mock.wren_count++;
        } else if (tx_byte == F_CMD_VSR_WREN) {
            s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
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
            break;
        case F_CMD_RDSR2: /* 35h */
            rx_byte = s_mock.sr2[s_mock.current_die];
            break;
        case F_CMD_RDSR3: /* 15h */
            rx_byte = s_mock.sr3[s_mock.current_die];
            break;

        case F_CMD_WRSR1: /* 01h : 1-byte (SR1) or 2-byte (SR1+SR2) */
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

    printf("================================================================\n");
    printf(" ALL 22 VERIFICATION TEST SUITES PASSED SUCCESSFULLY (100%%)!\n");
    printf("================================================================\n");

    return 0;
}
