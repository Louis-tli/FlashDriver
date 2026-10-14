/******************************************************************************
 * File Name   : flash.h
 * Description : Generic SPI NOR Flash Driver Header for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compliant)
 *
 * Boot model (SW-013):
 *   1) Read JEDEC ID (9Fh) and confirm the device responds.
 *   2) Basic-read (03h, single) the boot settings block from Flash.
 *   3) Configure the driver from those settings (Flash_BootInit).
 *   4) The selection is then locked (immutable for the rest of the boot).
 ******************************************************************************/


#ifndef __FLASH_H__
#define __FLASH_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * Return Codes & Driver Status
 ******************************************************************************/
typedef enum
{
    FLASH_OK                =  0,
    FLASH_ERR_NO_CONNECT    = -1,   /* Device ID unreadable (0x000000 or 0xFFFFFF) */
    FLASH_ERR_UNSUPPORTED   = -2,   /* JEDEC ID not found / not matching selection */
    FLASH_ERR_TIMEOUT       = -3,   /* Operation timed out (e.g., BUSY polling) */
    FLASH_ERR_PARAM         = -4,   /* Invalid argument passed */
    FLASH_ERR_ALIGNMENT     = -5,   /* Buffer or address alignment fault */
    FLASH_ERR_BUSY          = -6,   /* Flash is currently busy / already locked */
    FLASH_ERR_WRITE_FAIL    = -7,   /* WEL was not set or program verify failed */
    FLASH_ERR_HARDWARE      = -8    /* SFC / SPI controller hardware error */
} FLASH_STATUS;

/******************************************************************************
 * Driver State
 ******************************************************************************/
typedef enum
{
    FLASH_STATE_UNINIT      = 0,
    FLASH_STATE_NO_CONNECT,
    FLASH_STATE_READY,
    FLASH_STATE_ERROR
} FLASH_STATE;

/******************************************************************************
 * Supported Flash Models
 ******************************************************************************/
typedef enum
{
    FLASH_MODEL_MX25L512        = 0,    /* Macronix (512Kb or 512Mb) */
    FLASH_MODEL_W25M512,                /* Winbond SpiStack Multi-Die (2x256Mb) */
    FLASH_MODEL_W25Q128JW,              /* Winbond 128Mb (1.8V) */
    FLASH_MODEL_W25Q512JV,              /* Winbond 512Mb */
    FLASH_MODEL_W25Q256JV,              /* Winbond 256Mb */
    FLASH_MODEL_GD25LQ64E,              /* GigaDevice 64Mb (1.8V) */
    FLASH_MODEL_UNKNOWN
} FLASH_MODEL;

/******************************************************************************
 * Read Modes
 ******************************************************************************/
typedef enum
{
    FLASH_READ_SINGLE           = 0,    /* 03h: Normal Read (1-bit, up to ~50MHz) */
    FLASH_READ_FAST,                    /* 0Bh: Fast Read (1-bit with dummy) */
    FLASH_READ_DUAL,                    /* 3Bh: Dual Output Read (1-1-2) */
    FLASH_READ_DUAL_IO,                 /* BBh: Dual I/O Read (1-2-2) */
    FLASH_READ_QUAD,                    /* 6Bh: Quad Output Read (1-1-4) */
    FLASH_READ_QUAD_IO,                 /* EBh: Quad I/O Read (1-4-4) */
    FLASH_READ_MODE_MAX
} FLASH_READ_MODE;

/******************************************************************************
 * Read Access Units (Transfer Unit: Byte, Halfword, Word - Default: Word)
 ******************************************************************************/
typedef enum
{
    FLASH_READ_UNIT_DEFAULT  = 0,   /* Default: 32-bit Word access */
    FLASH_READ_UNIT_BYTE     = 1,   /* 8-bit Byte access */
    FLASH_READ_UNIT_HALFWORD = 2,   /* 16-bit Halfword access */
    FLASH_READ_UNIT_WORD     = 4    /* 32-bit Word access */
} FLASH_READ_UNIT;

/******************************************************************************
 * Program Modes
 ******************************************************************************/
typedef enum
{
    FLASH_PROGRAM_SINGLE        = 0,    /* 02h: Standard Page Program */
    FLASH_PROGRAM_QUAD                  /* 32h: Quad Input Page Program */
} FLASH_PROGRAM_MODE;

/******************************************************************************
 * Erase Types
 ******************************************************************************/
typedef enum
{
    FLASH_ERASE_SECTOR          = 0,    /* 20h: 4KB Sector Erase */
    FLASH_ERASE_BLOCK_32K,              /* 52h: 32KB Block Erase */
    FLASH_ERASE_BLOCK_64K,              /* D8h: 64KB Block Erase */
    FLASH_ERASE_CHIP                    /* C7h / 60h: Entire Chip Erase */
} FLASH_ERASE_MODE;

/******************************************************************************
 * Read Options (Bitmask)
 ******************************************************************************/
#define FLASH_READ_OPT_NONE             (0x00000000U)
#define FLASH_READ_OPT_SFC              (0x00000001U)   /* Use SFC Memory-Mapped DMA */
#define FLASH_READ_OPT_DMA              (0x00000002U)   /* Enable DMA controller */
#define FLASH_READ_OPT_CRC              (0x00000004U)   /* Calculate & verify CRC */

/******************************************************************************
 * Init Options (Bitmask) - SW-013 / G7
 ******************************************************************************/
#define FLASH_INIT_OPT_NONE             (0x00000000U)
#define FLASH_INIT_OPT_NO_RESET         (0x00000001U)   /* Skip SW reset (fast boot) */

/******************************************************************************
 * Common Standard SPI NOR Flash Command Opcodes
 ******************************************************************************/
#define CMD_WREN                        (0x06U) /* Write Enable */
#define CMD_WRDI                        (0x04U) /* Write Disable */
#define CMD_RDSR1                       (0x05U) /* Read Status Register 1 */
#define CMD_RDSR2                       (0x35U) /* Read Status Register 2 */
#define CMD_RDSR3                       (0x15U) /* Read Status Register 3 */
#define CMD_WRSR1                       (0x01U) /* Write Status Register 1 (or SR1+SR2) */
#define CMD_WRSR2                       (0x31U) /* Write Status Register 2 */
#define CMD_WRSR3                       (0x11U) /* Write Status Register 3 */
#define CMD_RDID                        (0x9FU) /* Read JEDEC Identification */
#define CMD_READ                        (0x03U) /* Read Data */
#define CMD_FAST_READ                   (0x0BU) /* Fast Read */
#define CMD_DUAL_READ                   (0x3BU) /* Dual Output Read */
#define CMD_DUAL_IO_READ                (0xBBU) /* Dual I/O Read */
#define CMD_QUAD_READ                   (0x6BU) /* Quad Output Read */
#define CMD_QUAD_IO_READ                (0xEBU) /* Quad I/O Read */
#define CMD_PAGE_PROGRAM                (0x02U) /* Page Program */
#define CMD_QUAD_PAGE_PROGRAM           (0x32U) /* Quad Page Program */
#define CMD_SECTOR_ERASE                (0x20U) /* 4KB Sector Erase */
#define CMD_BLOCK_ERASE_32K             (0x52U) /* 32KB Block Erase */
#define CMD_BLOCK_ERASE_64K             (0xD8U) /* 64KB Block Erase */
#define CMD_CHIP_ERASE                  (0xC7U) /* Chip Erase */
#define CMD_CHIP_ERASE_ALT              (0x60U) /* Alternative Chip Erase */
#define CMD_RESET_ENABLE                (0x66U) /* Software Reset Enable */
#define CMD_RESET_DEVICE                (0x99U) /* Software Reset Device */
#define CMD_ENTER_4BYTE_ADDR            (0xB7U) /* Enter 4-Byte Address Mode */
#define CMD_EXIT_4BYTE_ADDR             (0xE9U) /* Exit 4-Byte Address Mode */
#define CMD_WINBOND_DIE_SELECT          (0xC2U) /* Winbond SpiStack Software Die Select */

/******************************************************************************
 * 4-Byte Address Command Variants (SW-013 / req 2)
 *   Some devices use dedicated opcodes when addressing above 16MB (4-Byte
 *   mode) instead of reusing the 3-Byte opcode after Enter-4-Byte-Mode (B7h).
 ******************************************************************************/
#define CMD_READ_4B                     (0x13U) /* Read Data (4-Byte) */
#define CMD_FAST_READ_4B                (0x0CU) /* Fast Read (4-Byte) */
#define CMD_DUAL_READ_4B                (0x3CU) /* Dual Output Read (4-Byte) */
#define CMD_DUAL_IO_READ_4B             (0xBCU) /* Dual I/O Read (4-Byte) */
#define CMD_QUAD_READ_4B                (0x6CU) /* Quad Output Read (4-Byte) */
#define CMD_QUAD_IO_READ_4B             (0xECU) /* Quad I/O Read (4-Byte) */
#define CMD_PAGE_PROGRAM_4B             (0x12U) /* Page Program (4-Byte) */
#define CMD_QUAD_PAGE_PROGRAM_4B        (0x34U) /* Quad Page Program (4-Byte) */
#define CMD_SECTOR_ERASE_4B             (0x21U) /* 4KB Sector Erase (4-Byte) */
#define CMD_BLOCK_ERASE_32K_4B          (0x5CU) /* 32KB Block Erase (4-Byte) */
#define CMD_BLOCK_ERASE_64K_4B          (0xDCU) /* 64KB Block Erase (4-Byte) */

/******************************************************************************
 * Status Register 1 Bit Masks
 ******************************************************************************/
#define FLASH_SR1_BUSY                  (1U << 0)   /* Erase/Write in progress */
#define FLASH_SR1_WEL                   (1U << 1)   /* Write Enable Latch */
#define FLASH_SR1_BP0                   (1U << 2)   /* Block Protect 0 */
#define FLASH_SR1_BP1                   (1U << 3)   /* Block Protect 1 */
#define FLASH_SR1_BP2                   (1U << 4)   /* Block Protect 2 */
#define FLASH_SR1_BP3                   (1U << 5)   /* Block Protect 3 (or TB) */
#define FLASH_SR1_QE_MX25L              (1U << 6)   /* Quad Enable for Macronix */
#define FLASH_SR1_SRP0                  (1U << 7)   /* Status Register Protect 0 */

/******************************************************************************
 * Status Register 2 Bit Masks (Winbond / GigaDevice)
 ******************************************************************************/
#define FLASH_SR2_QE_WINBOND            (1U << 1)   /* Quad Enable for Winbond/GD */

/******************************************************************************
 * Feature Flags (Bitmask)
 ******************************************************************************/
#define FLASH_FEAT_NONE                 (0x00U)
#define FLASH_FEAT_DUAL                 (1U << 0)
#define FLASH_FEAT_QUAD                 (1U << 1)
#define FLASH_FEAT_4BYTE_ADDR           (1U << 2)
#define FLASH_FEAT_MULTI_DIE            (1U << 3)
#define FLASH_FEAT_STATUS_2BYTE_WRITE   (1U << 4)   /* Writes SR1 and SR2 together (GD25LQ64E) */

/******************************************************************************
 * JEDEC ID / Family Matching
 *   JEDEC ID = Manufacturer(8) | Memory Type(8) | Capacity(8).
 *   "Same product family" ignores the capacity byte.
 ******************************************************************************/
#define FLASH_JEDEC_ID_MASK             (0x00FFFFFFU)
#define FLASH_JEDEC_FAMILY_MASK         (0x00FFFF00U)

/******************************************************************************
 * Boot Settings Block (stored in Flash, loaded via basic read at boot)
 ******************************************************************************/
#define FLASH_BOOT_CFG_MAGIC            (0x46424F54UL)  /* "FBOT" */

typedef struct
{
    uint32_t magic;             /* FLASH_BOOT_CFG_MAGIC */
    uint32_t jedec_expect;      /* Expected JEDEC ID (family match allowed) */
    uint32_t model;             /* FLASH_MODEL hint (optional lookup key) */
    uint32_t features;          /* FLASH_FEAT_* operational flags */
    uint16_t sfc_page_size;     /* SFC transfer page size: 2048 or 4096 */
    uint16_t reserved;
    uint32_t crc;               /* Checksum of the fields above */
} FLASH_BOOT_CONFIG;

/******************************************************************************
 * Read Configuration Descriptor (SFC & SPI)
 ******************************************************************************/
typedef struct
{
    uint8_t command;            /* Read instruction opcode (3-Byte address) */
    uint8_t command_4b;         /* Read instruction opcode (4-Byte address) */
    uint8_t dummy_cycles;       /* Dummy clocks required by Flash spec */
    uint8_t dummy_bytes;        /* Dummy bytes for SFC controller */
    uint8_t dummy_value;        /* Dummy byte value (0x00 or 0xFF) */
    uint8_t dummy_last;         /* Dummy last byte value (0x00 or 0xFF) */
    uint8_t mode_byte;          /* Continuous mode byte (e.g. 0x20 or 0x00) */
    uint8_t io_width;           /* Data bus width: 1, 2, or 4 */
} FLASH_READ_CFG;

/******************************************************************************
 * Flash Model Information (Stored in ROM as const)
 ******************************************************************************/
typedef struct
{
    const char         *name;               /* Human-readable model name */
    uint32_t            jedec_id;           /* 24-bit JEDEC ID (RDID 9Fh) */
    FLASH_MODEL         model_id;           /* Internal model enum */
    uint32_t            flash_size;         /* Total density in bytes */
    uint16_t            page_size;          /* Typically 256 bytes */
    uint32_t            sector_size;        /* Typically 4096 bytes (4KB) */
    uint32_t            block_size;         /* Typically 65536 bytes (64KB) */
    uint8_t             addr_bytes;         /* 3 or 4 */

    /* Quad Enable (QE) Information */
    uint8_t             qe_reg;             /* Status register index: 1, 2, or 0 (none) */
    uint8_t             qe_bit;             /* QE bit position (0..7) */
    uint8_t             features;           /* FLASH_FEAT_* bitmask */

    /* Multi-Die Stack Configuration (W25M512) */
    uint8_t             die_count;          /* 1 for single die, 2 for dual-die */
    uint32_t            die_size;           /* Size of single die in bytes */

    /* Read mode configuration table */
    FLASH_READ_CFG      read_cfg[FLASH_READ_MODE_MAX];
} FLASH_INFO;

/******************************************************************************
 * Flash Runtime Handle (Stored in RAM)
 ******************************************************************************/
typedef struct
{
    const FLASH_INFO   *info;               /* Pointer to active flash model in ROM */
    FLASH_STATE         state;              /* Current driver state */
    uint8_t             current_die;        /* Currently selected die index */
    bool                qe_enabled;         /* True if QE is verified enabled */
    uint16_t            sfc_page_size;      /* SFC DMA page size: 2048 or 4096 */
    bool                locked;             /* Selection locked after boot (immutable) */
    uint32_t            last_crc;           /* Accumulated CRC of last Flash_Read */
} FLASH_HANDLE;

/******************************************************************************
 * Global Handle Access & Inline Helpers
 ******************************************************************************/
extern FLASH_HANDLE g_flash_handle;

/**
 * @brief Check if address/pointer is aligned to the given byte boundary.
 */
static inline bool Flash_IsAligned(uintptr_t val, uint32_t alignment_bytes)
{
    return ((val & (alignment_bytes - 1U)) == 0U);
}

/**
 * @brief Return true if two JEDEC IDs belong to the same product family
 *        (same manufacturer + memory type; capacity byte ignored).
 */
static inline bool Flash_FamilyMatch(uint32_t jedec_a, uint32_t jedec_b)
{
    return ((jedec_a & FLASH_JEDEC_FAMILY_MASK) == (jedec_b & FLASH_JEDEC_FAMILY_MASK));
}

/******************************************************************************
 * Public Driver APIs
 ******************************************************************************/

/**
 * @brief  Compute the checksum for a boot settings block.
 */
uint32_t Flash_ConfigCrc(const FLASH_BOOT_CONFIG *cfg);

/**
 * @brief  Basic (safe) single-line Read Data (03h) that works on every device
 *         before any QE / address-mode configuration is applied.
 *         Used at boot to load the settings block.
 * @param  addr: 32-bit Flash address (config block lives in low memory).
 * @param  buf:  Destination buffer.
 * @param  len:  Number of bytes to read.
 */
int Flash_BasicRead(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief  Boot-time initialization: ID response check -> basic-read settings
 *         block -> configure from settings -> lock the selection.
 *         On any mismatch / bad ID the driver enters a fail-safe error state.
 * @param  config_addr: Flash address of the FLASH_BOOT_CONFIG block.
 * @return FLASH_OK on success, or negative error code (fail-safe on failure).
 */
int Flash_BootInit(uint32_t config_addr);

/**
 * @brief  Boot-time initialization with options (e.g. FLASH_INIT_OPT_NO_RESET).
 */
int Flash_BootInitOpt(uint32_t config_addr, uint32_t options);

/**
 * @brief  Initialize the Flash Driver (auto-detect variant).
 *         Probes JEDEC ID via safe Single SPI mode, searches model table,
 *         configures address mode, verifies/enables QE bit on every die.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_Init(void);

/**
 * @brief  Auto-detect initialization with options (e.g. FLASH_INIT_OPT_NO_RESET).
 */
int Flash_InitOpt(uint32_t options);

/**
 * @brief  Issue Software Reset to Flash (66h followed by 99h).
 */
int Flash_Reset(void);

/**
 * @brief  Read 24-bit JEDEC ID using 9Fh opcode.
 */
int Flash_ReadJEDECID(uint32_t *p_jedec_id);

/**
 * @brief  Read Status Register 1, 2, or 3.
 */
int Flash_ReadStatus(uint8_t reg_idx, uint8_t *p_val);

/**
 * @brief  Write Status Register (handles 1-byte vs 2-byte SR1+SR2 automatically).
 */
int Flash_WriteStatus(uint8_t reg_idx, uint8_t val);

/**
 * @brief  Clear all block-protect (BP) bits on the active die so that
 *         program / erase cannot silently fail. Preserves QE and other bits.
 *         (SW-013 / G2)
 */
int Flash_Unprotect(void);

/**
 * @brief  Set block-protect (BP) bits on the active die from a 4-bit mask
 *         (BP3..BP0) to protect memory regions. Preserves QE and other bits.
 *         (SW-013 / G2)
 */
int Flash_Protect(uint8_t bp_mask);

/**
 * @brief  Send Write Enable (06h) and verify WEL bit is set.
 */
int Flash_WriteEnable(void);

/**
 * @brief  Send Write Disable (04h).
 */
int Flash_WriteDisable(void);

/**
 * @brief  Poll BUSY bit in Status Register 1 until clear or timeout.
 */
int Flash_WaitBusy(uint32_t timeout_loops);

/**
 * @brief  Verify and enable Quad Enable (QE) bit on the active die.
 */
int Flash_EnableQuad(void);

/**
 * @brief  Select active die on multi-die stacked chips (W25M512).
 */
int Flash_SelectDie(uint8_t die_idx);

/**
 * @brief  Read data from Flash (direct SPI or SFC Memory-Mapped DMA).
 *         When FLASH_READ_OPT_CRC is set, the CRC is accumulated across all
 *         transfer chunks and can be retrieved with Flash_GetLastCRC().
 */
int Flash_Read(uint32_t addr, void *buf, uint32_t count,
               FLASH_READ_MODE mode, FLASH_READ_UNIT unit,
               uint32_t options);

/**
 * @brief  Program data into Flash (Single 02h or Quad 32h).
 */
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode);

/**
 * @brief  Erase Sector (4KB), Block (32KB/64KB), or Chip.
 */
int Flash_Erase(uint32_t addr, FLASH_ERASE_MODE mode);

/**
 * @brief  Get current driver state.
 */
FLASH_STATE Flash_GetState(void);

/**
 * @brief  Get pointer to active Flash model info (or NULL if not initialized).
 */
const FLASH_INFO *Flash_GetInfo(void);

/**
 * @brief  Get the accumulated CRC of the last Flash_Read (with FLASH_READ_OPT_CRC).
 */
uint32_t Flash_GetLastCRC(void);

/**
 * @brief  Return true once the boot selection has been locked (immutable).
 */
bool Flash_IsLocked(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_H__ */
