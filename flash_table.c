/******************************************************************************
 * File Name   : flash_table.c
 * Description : Flash Device Profiles and Model Lookup Table for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 Compatible)
 * Note        : Stored in ROM (.rodata) to conserve Cortex-M0 SRAM.
 ******************************************************************************/

#include "flash_table.h"

/******************************************************************************
 * ROM Table containing configurations for supported SPI NOR Flash models
 ******************************************************************************/
const FLASH_INFO g_flash_table[FLASH_TABLE_ENTRIES_COUNT] =
{
    /* ------------------------------------------------------------------------
     * [0] MX25L512 (Macronix 512K-bit / 64KB)
     * ------------------------------------------------------------------------ */
    [0] = {
        .name         = "MX25L512 (512Kb)",
        .jedec_id     = 0xC22010U,
        .model_id     = FLASH_MODEL_MX25L512,
        .flash_size   = 64U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 3U,
        .qe_reg       = 1U,
        .qe_bit       = 6U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 6U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [1] MX25L51245G / MX25L512 (Macronix 512M-bit / 64MB)
     * ------------------------------------------------------------------------ */
    [1] = {
        .name         = "MX25L51245G (512Mb)",
        .jedec_id     = 0xC2201AU,
        .model_id     = FLASH_MODEL_MX25L512,
        .flash_size   = 64U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 1U,
        .qe_bit       = 6U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 8U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [2] W25M512 (Winbond SpiStack 2 x 256Mb = 64MB Multi-Die)
     * ------------------------------------------------------------------------ */
    [2] = {
        .name         = "W25M512 (2xDie 64MB)",
        .jedec_id     = 0xEF6119U,
        .model_id     = FLASH_MODEL_W25M512,
        .flash_size   = 64U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR | FLASH_FEAT_MULTI_DIE,
        .die_count    = 2U,
        .die_size     = 32U * 1024U * 1024U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 8U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [3] W25Q128JW (Winbond 128M-bit / 16MB, 1.8V)
     * ------------------------------------------------------------------------ */
    [3] = {
        .name         = "W25Q128JW (16MB)",
        .jedec_id     = 0xEF6018U,
        .model_id     = FLASH_MODEL_W25Q128JW,
        .flash_size   = 16U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 3U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 6U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [4] W25Q512JV (Winbond 512M-bit / 64MB)
     * ------------------------------------------------------------------------ */
    [4] = {
        .name         = "W25Q512JV (64MB)",
        .jedec_id     = 0xEF4020U,
        .model_id     = FLASH_MODEL_W25Q512JV,
        .flash_size   = 64U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 8U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [5] W25Q256JV (Winbond 256M-bit / 32MB)
     * ------------------------------------------------------------------------ */
    [5] = {
        .name         = "W25Q256JV (32MB)",
        .jedec_id     = 0xEF4019U,
        .model_id     = FLASH_MODEL_W25Q256JV,
        .flash_size   = 32U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 8U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [6] GD25LQ64E (GigaDevice 64M-bit / 8MB, 1.8V)
     * ------------------------------------------------------------------------ */
    [6] = {
        .name         = "GD25LQ64E (8MB)",
        .jedec_id     = 0xC86017U,
        .model_id     = FLASH_MODEL_GD25LQ64E,
        .flash_size   = 8U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 3U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
        .features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_STATUS_2BYTE_WRITE,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = { .command = CMD_READ,         .dummy_cycles = 0U, .dummy_bytes = 0U, .dummy_value = 0x00U, .dummy_last = 0x00U, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_FAST]    = { .command = CMD_FAST_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 1U },
            [FLASH_READ_DUAL]    = { .command = CMD_DUAL_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_DUAL_IO] = { .command = CMD_DUAL_IO_READ, .dummy_cycles = 4U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 2U },
            [FLASH_READ_QUAD]    = { .command = CMD_QUAD_READ,    .dummy_cycles = 8U, .dummy_bytes = 1U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x00U, .io_width = 4U },
            [FLASH_READ_QUAD_IO] = { .command = CMD_QUAD_IO_READ, .dummy_cycles = 6U, .dummy_bytes = 3U, .dummy_value = 0xFFU, .dummy_last = 0xFFU, .mode_byte = 0x20U, .io_width = 4U }
        }
    }
};

/******************************************************************************
 * Model Lookup Function
 ******************************************************************************/
const FLASH_INFO *Flash_FindDevice(uint32_t jedec_id)
{
    /* Search through table for exact matching JEDEC ID */
    for (uint32_t i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++)
    {
        if (g_flash_table[i].jedec_id == jedec_id)
        {
            return &g_flash_table[i];
        }
    }

    return NULL;
}
