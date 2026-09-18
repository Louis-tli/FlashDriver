/******************************************************************************
 * File Name   : flash_table.c
 * Description : Flash Device Profiles and Model Lookup Table for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC / C90 Compatible
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
    {
        "MX25L512 (512Kb)",
        0xC22010U,                          /* JEDEC ID: C2h (Macronix), 20h, 10h */
        FLASH_MODEL_MX25L512,
        64U * 1024U,                        /* Total Size: 64 KB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        3U,                                 /* 3-byte address */
        1U,                                 /* QE in Status Register 1 */
        6U,                                 /* QE is Bit 6 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD,
        1U,                                 /* Single Die */
        0U,                                 /* Die Size (N/A) */
        {
            /* SINGLE */   { CMD_READ,         0U, 0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 6U, 3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [1] MX25L51245G / MX25L512 (Macronix 512M-bit / 64MB)
     * ------------------------------------------------------------------------ */
    {
        "MX25L51245G (512Mb)",
        0xC2201AU,                          /* JEDEC ID: C2h (Macronix), 20h, 1Ah */
        FLASH_MODEL_MX25L512,
        64U * 1024U * 1024U,                /* Total Size: 64 MB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        4U,                                 /* 4-byte address */
        1U,                                 /* QE in Status Register 1 */
        6U,                                 /* QE is Bit 6 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR,
        1U,                                 /* Single Die */
        0U,
        {
            /* SINGLE */   { CMD_READ,         0U,  0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U,  1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U,  1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U,  1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U,  1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 8U,  3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [2] W25M512 (Winbond SpiStack 2 x 256Mb = 64MB Multi-Die)
     * ------------------------------------------------------------------------ */
    {
        "W25M512 (2xDie 64MB)",
        0xEF6119U,                          /* JEDEC ID: EFh (Winbond), 61h, 19h (or EF7119) */
        FLASH_MODEL_W25M512,
        64U * 1024U * 1024U,                /* Total Size: 64 MB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        4U,                                 /* 4-byte address */
        2U,                                 /* QE in Status Register 2 */
        1U,                                 /* QE is Bit 1 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR | FLASH_FEAT_MULTI_DIE,
        2U,                                 /* 2 Dies */
        32U * 1024U * 1024U,                /* 32 MB per Die */
        {
            /* SINGLE */   { CMD_READ,         0U, 0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 8U, 3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [3] W25Q128JW (Winbond 128M-bit / 16MB, 1.8V)
     * ------------------------------------------------------------------------ */
    {
        "W25Q128JW (16MB)",
        0xEF6018U,                          /* JEDEC ID: EFh (Winbond), 60h, 18h */
        FLASH_MODEL_W25Q128JW,
        16U * 1024U * 1024U,                /* Total Size: 16 MB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        3U,                                 /* 3-byte address */
        2U,                                 /* QE in Status Register 2 */
        1U,                                 /* QE is Bit 1 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD,
        1U,                                 /* Single Die */
        0U,
        {
            /* SINGLE */   { CMD_READ,         0U, 0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 6U, 3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [4] W25Q512JV (Winbond 512M-bit / 64MB)
     * ------------------------------------------------------------------------ */
    {
        "W25Q512JV (64MB)",
        0xEF4020U,                          /* JEDEC ID: EFh (Winbond), 40h, 20h */
        FLASH_MODEL_W25Q512JV,
        64U * 1024U * 1024U,                /* Total Size: 64 MB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        4U,                                 /* 4-byte address */
        2U,                                 /* QE in Status Register 2 */
        1U,                                 /* QE is Bit 1 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR,
        1U,                                 /* Single Die */
        0U,
        {
            /* SINGLE */   { CMD_READ,         0U, 0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 8U, 3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [5] W25Q256JV (Winbond 256M-bit / 32MB)
     * ------------------------------------------------------------------------ */
    {
        "W25Q256JV (32MB)",
        0xEF4019U,                          /* JEDEC ID: EFh (Winbond), 40h, 19h */
        FLASH_MODEL_W25Q256JV,
        32U * 1024U * 1024U,                /* Total Size: 32 MB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        4U,                                 /* 4-byte address */
        2U,                                 /* QE in Status Register 2 */
        1U,                                 /* QE is Bit 1 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_4BYTE_ADDR,
        1U,                                 /* Single Die */
        0U,
        {
            /* SINGLE */   { CMD_READ,         0U, 0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 8U, 3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    },

    /* ------------------------------------------------------------------------
     * [6] GD25LQ64E (GigaDevice 64M-bit / 8MB, 1.8V)
     * ------------------------------------------------------------------------ */
    {
        "GD25LQ64E (8MB)",
        0xC86017U,                          /* JEDEC ID: C8h (GigaDevice), 60h, 17h */
        FLASH_MODEL_GD25LQ64E,
        8U * 1024U * 1024U,                 /* Total Size: 8 MB */
        256U,                               /* Page Size: 256 B */
        4096U,                              /* Sector Size: 4 KB */
        65536U,                             /* Block Size: 64 KB */
        3U,                                 /* 3-byte address */
        2U,                                 /* QE in Status Register 2 */
        1U,                                 /* QE is Bit 1 */
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_STATUS_2BYTE_WRITE,
        1U,                                 /* Single Die */
        0U,
        {
            /* SINGLE */   { CMD_READ,         0U, 0U, 0x00U, 0x00U, 0x00U, 1U },
            /* FAST */     { CMD_FAST_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U },
            /* DUAL */     { CMD_DUAL_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* DUAL_IO */  { CMD_DUAL_IO_READ, 4U, 1U, 0xFFU, 0xFFU, 0x00U, 2U },
            /* QUAD */     { CMD_QUAD_READ,    8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U },
            /* QUAD_IO */  { CMD_QUAD_IO_READ, 6U, 3U, 0xFFU, 0xFFU, 0x20U, 4U }
        }
    }
};

/******************************************************************************
 * Model Lookup Function
 ******************************************************************************/
const FLASH_INFO *Flash_FindDevice(uint32_t jedec_id)
{
    uint32_t i;

    /* Search through table for exact matching JEDEC ID */
    for (i = 0U; i < FLASH_TABLE_ENTRIES_COUNT; i++)
    {
        if (g_flash_table[i].jedec_id == jedec_id)
        {
            return &g_flash_table[i];
        }
    }

    return NULL;
}
