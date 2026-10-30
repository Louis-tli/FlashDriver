/******************************************************************************
 * File Name   : flash_table.c
 * Description : Flash Device Profiles and Model Lookup Table for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 * Note        : Stored in ROM (.rodata) to conserve Cortex-M0 SRAM.
 *
 * SW-013: entries are ordered by the FLASH_IDX enum (see flash_table.h).
 *   Shared read rows use the READCFG() builders; each carries a 3-Byte and a
 *   4-Byte opcode (req 2) since the instruction differs with address width.
 ******************************************************************************/

#include "flash_table.h"

/******************************************************************************
 * Shared Read-Mode Config Builders
 *   READCFG(3B opcode, 4B opcode, dummy_cycles, dummy_bytes, dummy_value,
 *           dummy_last, mode_byte, io_width)
 ******************************************************************************/
#define READCFG(_c,_c4,_dc,_db,_dv,_dl,_mb,_w)                          \
    { .command = (_c), .command_4b = (_c4), .dummy_cycles = (_dc),      \
      .dummy_bytes = (_db), .dummy_value = (_dv), .dummy_last = (_dl),   \
      .mode_byte = (_mb), .io_width = (_w) }

/* Identical on every supported device */
#define RC_SINGLE       READCFG(F_CMD_READ,      F_CMD_READ_4B,      0U, 0U, 0x00U, 0x00U, 0x00U, 1U)
#define RC_FAST         READCFG(F_CMD_FAST_READ, F_CMD_FAST_READ_4B, 8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U)
#define RC_DUAL         READCFG(F_CMD_DUAL_READ, F_CMD_DUAL_READ_4B, 8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U)
#define RC_QUAD         READCFG(F_CMD_QUAD_READ, F_CMD_QUAD_READ_4B, 8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U)

/* Per-device timing variations */
/* SW-013 DUMMY (owner-confirmed TCON wire order):
 *   CMD -> ADDR -> DUMMY_LAST (FIRST dummy byte = M7-M0 mode byte slot)
 *        -> DUMMY_VAL -> ... -> DATA
 *   Winbond   : first dummy (M7-M0) MUST be Fxh   -> dummy_last 0xFF
 *   GigaDevice: M7-M0 != Axh keeps normal mode    -> dummy_last 0x00
 *   Macronix  : no mode byte (dummy don't care)   -> dummy_last 0x00
 * Remaining dummy bytes are "don't care" on every vendor -> dummy_value 0x00. */
#define RC_DUAL_IO_WB   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B, 4U, 1U, 0x00U, 0xFFU, 0x00U, 2U) /* Winbond */
#define RC_DUAL_IO_NM   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B, 4U, 1U, 0x00U, 0x00U, 0x00U, 2U) /* Macronix */
#define RC_DUAL_IO_GD   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B, 6U, 1U, 0x00U, 0x00U, 0x00U, 2U) /* GD25LQ64E */
#define RC_QUAD_IO_WB6  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B, 6U, 3U, 0x00U, 0xFFU, 0x00U, 4U) /* Winbond, dummy 6 */
#define RC_QUAD_IO_WB8  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B, 8U, 3U, 0x00U, 0xFFU, 0x00U, 4U) /* Winbond, dummy 8 */
#define RC_QUAD_IO_NM6  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B, 6U, 3U, 0x00U, 0x00U, 0x00U, 4U) /* MX/GD, dummy 6 */
#define RC_QUAD_IO_NM8  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B, 8U, 3U, 0x00U, 0x00U, 0x00U, 4U) /* Macronix, dummy 8 */

/******************************************************************************
 * ROM Table (indexed by FLASH_IDX)
 ******************************************************************************/
const FLASH_INFO g_flash_table[FLASH_TABLE_ENTRIES_COUNT] =
{
    /* ------------------------------------------------------------------------
     * [FLASH_IDX_W25Q128JW] W25Q128JW (Winbond 128M-bit / 16MB, 1.8V)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_W25Q128JW] = {
        .jedec_id     = 0xEF6018U,
        .model_id     = FLASH_MODEL_W25Q128JW,
        .flash_size   = 16U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 3U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
.features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_VSR_WREN,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_WB,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_WB6
        }
    },

    /* ------------------------------------------------------------------------
     * [FLASH_IDX_W25Q512JV] W25Q512JV (Winbond 512M-bit / 64MB)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_W25Q512JV] = {
        .jedec_id     = 0xEF4020U,
        .model_id     = FLASH_MODEL_W25Q512JV,
        .flash_size   = 64U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
.features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR | FLASH_FEAT_VSR_WREN,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_WB,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_WB8
        }
    },

    /* ------------------------------------------------------------------------
     * [FLASH_IDX_W25Q256JV] W25Q256JV (Winbond 256M-bit / 32MB)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_W25Q256JV] = {
        .jedec_id     = 0xEF4019U,
        .model_id     = FLASH_MODEL_W25Q256JV,
        .flash_size   = 32U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
.features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR | FLASH_FEAT_VSR_WREN,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_WB,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_WB8
        }
    },

    /* ------------------------------------------------------------------------
     * [FLASH_IDX_W25M512] W25M512 (Winbond SpiStack 2 x 256Mb = 64MB Multi-Die)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_W25M512] = {
        .jedec_id     = 0xEF6119U,
        .model_id     = FLASH_MODEL_W25M512,
        .flash_size   = 64U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 4U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
.features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR | FLASH_FEAT_MULTI_DIE | FLASH_FEAT_VSR_WREN,
        .die_count    = 2U,
        .die_size     = 32U * 1024U * 1024U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_WB,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_WB8
        }
    },

    /* ------------------------------------------------------------------------
     * [FLASH_IDX_MX25L512] MX25L512 (Macronix 512K-bit / 64KB)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_MX25L512] = {
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
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_NM,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM6
        }
    },

    /* ------------------------------------------------------------------------
     * [FLASH_IDX_MX25L512MB] MX25L51245G (Macronix 512M-bit / 64MB)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_MX25L512MB] = {
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
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_NM,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM8
        }
    },

    /* ------------------------------------------------------------------------
     * [FLASH_IDX_GD25LQ64E] GD25LQ64E (GigaDevice 64M-bit / 8MB, 1.8V)
     * ------------------------------------------------------------------------ */
    [FLASH_IDX_GD25LQ64E] = {
        .jedec_id     = 0xC86017U,
        .model_id     = FLASH_MODEL_GD25LQ64E,
        .flash_size   = 8U * 1024U * 1024U,
        .page_size    = 256U,
        .sector_size  = 4096U,
        .block_size   = 65536U,
        .addr_bytes   = 3U,
        .qe_reg       = 2U,
        .qe_bit       = 1U,
.features     = FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_STATUS_2BYTE_WRITE | FLASH_FEAT_VSR_WREN,
        .die_count    = 1U,
        .die_size     = 0U,
        .read_cfg     = {
            [FLASH_READ_SINGLE]  = RC_SINGLE,
            [FLASH_READ_FAST]    = RC_FAST,
            [FLASH_READ_DUAL]    = RC_DUAL,
            [FLASH_READ_DUAL_IO] = RC_DUAL_IO_GD,
            [FLASH_READ_QUAD]    = RC_QUAD,
            [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM6
        }
    }
};

/******************************************************************************
 * Model Lookup Functions
 ******************************************************************************/
const FLASH_INFO *Flash_FindDevice(uint32_t jedec_id)
{
    for (uint32_t i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++) {
        if (g_flash_table[i].jedec_id == jedec_id) {
            return &g_flash_table[i];
        }
    }
    return NULL;
}

const FLASH_INFO *Flash_FindDeviceByModel(FLASH_MODEL model_id)
{
    for (uint32_t i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++) {
        if (g_flash_table[i].model_id == model_id) {
            return &g_flash_table[i];
        }
    }
    return NULL;
}
