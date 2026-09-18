/******************************************************************************
 * File Name   : test_flash.c
 * Description : Verification Test Suite for Generic Flash Driver
 * Compiler    : C90 / armcc / GCC compliant
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "flash.h"
#include "flash_table.h"
#include "flash_spi.h"
#include "flash_sfc.h"

/******************************************************************************
 * Mock SPI State for Emulation & Testing
 ******************************************************************************/
typedef struct
{
    uint32_t mock_jedec_id;
    uint8_t  mock_sr1;
    uint8_t  mock_sr2;
    uint8_t  mock_sr3;
    uint8_t  last_die_selected;
    uint32_t die_select_count;
    uint32_t page_program_count;
    uint32_t total_bytes_programmed;
    uint8_t  in_cs;
    uint8_t  current_cmd;
    uint8_t  cmd_byte_index;
} MOCK_SPI_STATE;

static MOCK_SPI_STATE s_mock;

/* Override weak SPI functions for test verification */
void Flash_SPI_Init(void)
{
    /* Reset mock state */
    s_mock.in_cs = 0U;
    s_mock.current_cmd = 0U;
    s_mock.cmd_byte_index = 0U;
}

void Flash_SPI_SetSpeed(uint32_t speed_hz)
{
    (void)speed_hz;
}

void Flash_SPI_CS_Low(void)
{
    s_mock.in_cs = 1U;
    s_mock.cmd_byte_index = 0U;
}

void Flash_SPI_CS_High(void)
{
    s_mock.in_cs = 0U;
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
            s_mock.mock_sr1 |= FLASH_SR1_WEL;
        }
        else if (tx_byte == CMD_WRDI)
        {
            s_mock.mock_sr1 &= ~FLASH_SR1_WEL;
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

        case CMD_RDSR1: /* 05h */
            rx_byte = s_mock.mock_sr1;
            break;

        case CMD_RDSR2: /* 35h */
            rx_byte = s_mock.mock_sr2;
            break;

        case CMD_RDSR3: /* 15h */
            rx_byte = s_mock.mock_sr3;
            break;

        case CMD_WRSR1: /* 01h */
            if (s_mock.cmd_byte_index == 1U)
            {
                s_mock.mock_sr1 = tx_byte;
                s_mock.cmd_byte_index++;
            }
            else if (s_mock.cmd_byte_index == 2U)
            {
                /* 2-byte write (GD25LQ64E): second byte is SR2 */
                s_mock.mock_sr2 = tx_byte;
                s_mock.cmd_byte_index++;
            }
            break;

        case CMD_WRSR2: /* 31h */
            s_mock.mock_sr2 = tx_byte;
            break;

        case CMD_WREN: /* 06h */
            s_mock.mock_sr1 |= FLASH_SR1_WEL;
            break;

        case CMD_WRDI: /* 04h */
            s_mock.mock_sr1 &= ~FLASH_SR1_WEL;
            break;

        case CMD_WINBOND_DIE_SELECT: /* C2h */
            s_mock.last_die_selected = tx_byte;
            s_mock.die_select_count++;
            break;

        case CMD_PAGE_PROGRAM:       /* 02h */
        case CMD_QUAD_PAGE_PROGRAM:  /* 32h */
            /* Count page programs and payload */
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
            break;
    }

    return rx_byte;
}

/******************************************************************************
 * Test Cases
 ******************************************************************************/

static void Test_TableLookup(void)
{
    const FLASH_INFO *p_info;
    uint32_t i;

    printf("[TEST 1] Testing JEDEC ID Table Lookups...\n");

    /* Test all 7 target IDs */
    assert(Flash_FindDevice(0xC22010U) != NULL); /* MX25L512 (512Kb) */
    assert(Flash_FindDevice(0xC2201AU) != NULL); /* MX25L512 (512Mb) */
    assert(Flash_FindDevice(0xEF6119U) != NULL); /* W25M512 */
    assert(Flash_FindDevice(0xEF6018U) != NULL); /* W25Q128JW */
    assert(Flash_FindDevice(0xEF4020U) != NULL); /* W25Q512JV */
    assert(Flash_FindDevice(0xEF4019U) != NULL); /* W25Q256JV */
    assert(Flash_FindDevice(0xC86017U) != NULL); /* GD25LQ64E */

    /* Unsupported ID */
    assert(Flash_FindDevice(0x123456U) == NULL);

    /* Verify table descriptors */
    for (i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++)
    {
        p_info = &g_flash_table[i];
        printf("  - Model [%u]: %-22s | JEDEC: 0x%06X | Size: %u KB | Addr: %uB\n",
               i, p_info->name, p_info->jedec_id, (p_info->flash_size / 1024U), p_info->addr_bytes);
        assert(p_info->page_size == 256U);
        assert(p_info->sector_size == 4096U);
    }
    printf("  --> Table lookups PASSED.\n\n");
}

static void Test_InitAndProbe(void)
{
    int status;

    printf("[TEST 2] Testing Flash_Init with various bus conditions...\n");

    /* 1. Unconnected / floating bus (0x000000) */
    s_mock.mock_jedec_id = 0x00000000U;
    status = Flash_Init();
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    printf("  - Floating bus 0x000000 correctly reported NO_CONNECT.\n");

    /* 2. Floating bus pull-up (0xFFFFFF) */
    s_mock.mock_jedec_id = 0x00FFFFFFU;
    status = Flash_Init();
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    printf("  - Floating bus 0xFFFFFF correctly reported NO_CONNECT.\n");

    /* 3. Unsupported ID */
    s_mock.mock_jedec_id = 0xAABBCCU;
    status = Flash_Init();
    assert(status == FLASH_ERR_UNSUPPORTED);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    printf("  - Unsupported ID 0xAABBCC correctly reported UNSUPPORTED.\n");

    /* 4. Valid Winbond W25Q256JV (0xEF4019) */
    s_mock.mock_jedec_id = 0xEF4019U;
    s_mock.mock_sr1 = 0x00U;
    s_mock.mock_sr2 = 0x00U; /* QE not set initially */
    status = Flash_Init();
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25Q256JV);
    assert((s_mock.mock_sr2 & FLASH_SR2_QE_WINBOND) != 0U); /* QE must have been enabled! */
    assert(g_flash_handle.qe_enabled == 1U);
    printf("  - Valid W25Q256JV initialized, QE automatically enabled. PASSED.\n\n");
}

static void Test_GD25LQ64E_StatusWrite(void)
{
    int status;

    printf("[TEST 3] Testing GD25LQ64E 2-Byte Status Register Write...\n");

    s_mock.mock_jedec_id = 0xC86017U;
    s_mock.mock_sr1 = 0x1CU; /* Some BP bits */
    s_mock.mock_sr2 = 0x00U; /* QE = 0 */

    status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_GD25LQ64E);
    /* Verify 2-byte write preserved SR1 and enabled QE bit in SR2 */
    assert(s_mock.mock_sr1 == 0x1CU);
    assert((s_mock.mock_sr2 & FLASH_SR2_QE_WINBOND) != 0U);
    printf("  - GD25LQ64E 2-byte WRSR handled properly. PASSED.\n\n");
}

static void Test_PageBoundarySplitting(void)
{
    uint8_t write_buffer[300];
    int status;
    uint32_t i;

    printf("[TEST 4] Testing 256-Byte Page Boundary Splitting in Flash_Write...\n");

    s_mock.mock_jedec_id = 0xEF4019U; /* W25Q256JV */
    s_mock.mock_sr1 = 0x00U;
    s_mock.mock_sr2 = 0x02U;
    status = Flash_Init();
    assert(status == FLASH_OK);

    for (i = 0U; i < 300U; i++)
    {
        write_buffer[i] = (uint8_t)(i & 0xFFU);
    }

    /* Start write at address 0x000000F0, length = 300 bytes:
     * - Chunk 1: 0x00F0 ~ 0x00FF (16 bytes, fills page 0)
     * - Chunk 2: 0x0100 ~ 0x01FF (256 bytes, full page 1)
     * - Chunk 3: 0x0200 ~ 0x021B (28 bytes, page 2)
     * Total = 3 Page Program operations! */
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
    int status;
    uint8_t test_buf[64];

    printf("[TEST 5] Testing Winbond SpiStack Multi-Die (W25M512)...\n");

    s_mock.mock_jedec_id = 0xEF6119U; /* W25M512 */
    s_mock.mock_sr1 = 0x00U;
    s_mock.mock_sr2 = 0x02U;
    s_mock.last_die_selected = 0xFFU;
    s_mock.die_select_count = 0U;

    status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25M512);
    assert((g_flash_handle.info->features & FLASH_FEAT_MULTI_DIE) != 0U);

    /* Force die select to verify C2h command generation */
    g_flash_handle.current_die = 0xFFU;

    /* Write to Die 0 (address = 0x01000000 = 16MB) */
    memset(test_buf, 0xA5, sizeof(test_buf));
    status = Flash_Write(0x01000000U, test_buf, sizeof(test_buf), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.last_die_selected == 0U);
    printf("  - Address 16MB mapped to Die 0 (C2h sent with Die 0).\n");

    /* Write to Die 1 (address = 0x02500000 = 37MB > 32MB) */
    status = Flash_Write(0x02500000U, test_buf, sizeof(test_buf), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.last_die_selected == 1U);
    printf("  - Address 37MB automatically switched to Die 1 via C2h opcode.\n");
    printf("  - Multi-Die switching verified. PASSED.\n\n");
}

static void Test_SFCDMAReadSequence(void)
{
    uint32_t aligned_buf[1024]; /* 4096 bytes, 4-byte aligned on 32-bit stack */
    uint8_t *p_unaligned;
    uint32_t crc_val = 0U;
    int status;

    printf("[TEST 6] Testing TCON SFC Memory-Mapped DMA Read Sequence...\n");

    s_mock.mock_jedec_id = 0xEF4019U; /* W25Q256JV */
    status = Flash_Init();
    assert(status == FLASH_OK);

    /* 1. Alignment check: Unaligned destination buffer must be rejected */
    p_unaligned = ((uint8_t *)aligned_buf) + 1;
    status = Flash_Read(0x00001000U, p_unaligned, 128U, FLASH_READ_QUAD_IO,
                        FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);
    printf("  - Unaligned DMA buffer properly rejected with FLASH_ERR_ALIGNMENT.\n");

    /* 2. Aligned DMA Read with SFC Sequence:
     *    SFC Single Mode -> Poll Busy -> SFC Quad I/O Mode -> DMA Chunks */
    status = Flash_Read(0x00001000U, aligned_buf, 4096U, FLASH_READ_QUAD_IO,
                        FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == CMD_QUAD_IO_READ); /* 0xEB */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    assert(g_p_sfc_regs->MODE == 4U);              /* Quad width */
    printf("  - SFC Quad I/O DMA read and sequence successfully validated. PASSED.\n\n");
    (void)crc_val;
}

int main(void)
{
    printf("================================================================\n");
    printf(" Generic SPI NOR Flash Driver Verification Suite (ARM Cortex-M0)\n");
    printf("================================================================\n\n");

    Test_TableLookup();
    Test_InitAndProbe();
    Test_GD25LQ64E_StatusWrite();
    Test_PageBoundarySplitting();
    Test_MultiDieHandling();
    Test_SFCDMAReadSequence();

    printf("================================================================\n");
    printf(" ALL 6 VERIFICATION TEST SUITES PASSED SUCCESSFULLY (100%%)!\n");
    printf("================================================================\n");

    return 0;
}
