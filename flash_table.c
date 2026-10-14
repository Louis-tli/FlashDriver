/******************************************************************************
 * File Name   : flash_table.c
 * Description : Flash Device Profiles and Model Lookup Table for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 * Note        : Stored in ROM (.rodata) to conserve Cortex-M0 SRAM.
 *
 * SW-013 / G6: read-mode configs that are identical across devices are built
 *              from shared READCFG() rows. Each read mode carries both a
 *              3-Byte and a 4-Byte opcode (req 2), since the instruction
 *              differs with the active address width.
 ******************************************************************************/

#include "flash_table.h"

/******************************************************************************
 * Shared Read-Mode Config Builders (G6: common values defined once)
 *   READCFG(3B opcode, 4B opcode, dummy_cycles, dummy_bytes, dummy_value,
 *           dummy_last, mode_byte, io_width)
 ******************************************************************************/
#define READCFG(_c,_c4,_dc,_db,_dv,_dl,_mb,_w)                          \
    { .command = (_c), .command_4b = (_c4), .dummy_cycles = (_dc),      \
      .dummy_bytes = (_db), .dummy_value = (_dv), .dummy_last = (_dl),   \
      .mode_byte = (_mb), .io_width = (_w) }

/* Identical on every supported device (opcodes: 3B / 4B) */
#define RC_SINGLE       READCFG(CMD_READ,      CMD_READ_4B,      0U, 0U, 0x00U, 0x00U, 0x00U, 1U)
#define RC_FAST         READCFG(CMD_FAST_READ, CMD_FAST_READ_4B, 8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U)
#define RC_DUAL         READCFG(CMD_DUAL_READ, CMD_DUAL_READ_4B, 8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U)
#define RC_QUAD         READCFG(CMD_QUAD_READ, CMD_QUAD_READ_4B, 8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U)

/* Per-device timing variations */
#define RC_DUAL_IO      READCFG(CMD_DUAL_IO_READ, CMD_DUAL_IO_READ_4B, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U) /* default */
#define RC_DUAL_IO_GD   READCFG(CMD_DUAL_IO_READ, CMD_DUAL_IO_READ_4B, 6U, 1U, 0xFFU, 0xFFU, 0x00U, 2U) /* GD25LQ64E */
#define RC_QUAD_IO_8    READCFG(CMD_QUAD_IO_READ, CMD_QUAD_IO_READ_4B, 8U, 3U, 0xFFU, 0xFFU, 0x20U, 4U) /* dummy 8 */
#define RC_QUAD_IO_6    READCFG(CMD_QUAD_IO_READ, CMD_QUAD_IO_READ_4B, 6U, 3U, 0xFFU, 0xFFU, 0x20U, 4U) /* dummy 6 */

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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_6
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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_8
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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_8
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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_6
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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_8
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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_8
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
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_GD,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_6
        }
    }
};

/******************************************************************************
 * Model Lookup Functions
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

const FLASH_INFO *Flash_FindDeviceByModel(FLASH_MODEL model_id)
{
    /* First entry with the requested model */
    for (uint32_t i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++)
    {
        if (g_flash_table[i].model_id == model_id)
        {
            return &g_flash_table[i];
        }
    }

    return NULL;
}
