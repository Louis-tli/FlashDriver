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
#include "flash_delay.h"

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
/* SW-041: a "model" IS its 24-bit JEDEC ID.
 *
 * The old enum forced one table row per product, so every new density was
 * new data. With the value equal to the ID, Flash_FindDevice() resolves the
 * part from vendor + density and this type carries no table cost at all.
 *
 * Compatibility note for saved boot settings: settings written by firmware
 * that used the old enum hold 0..7 in the "model" field. Flash_Init() treats
 * any value below 0x01000000 as "no hint" and falls back to the JEDEC probe,
 * which is the same behaviour those products had with FLASH_MODEL_UNKNOWN. */
typedef uint32_t FLASH_MODEL;

#define FLASH_MODEL_MX25L512        0xC22010U   /* Macronix 512 Kb / 64 KB */
#define FLASH_MODEL_MX25L512MB      0xC2201AU   /* Macronix 512 Mb / 64 MB */
#define FLASH_MODEL_W25M512         0xEF6119U   /* Winbond SpiStack 2 x 32 MB */
#define FLASH_MODEL_W25Q128JW       0xEF6018U   /* Winbond 128 Mb / 16 MB, 1.8V */
#define FLASH_MODEL_W25Q512JV       0xEF4020U   /* Winbond 512 Mb / 64 MB */
#define FLASH_MODEL_W25Q256JV       0xEF4019U   /* Winbond 256 Mb / 32 MB */
#define FLASH_MODEL_GD25LQ64E       0xC86017U   /* GigaDevice 64 Mb / 8 MB, 1.8V */
#define FLASH_MODEL_GD25LQ32E       0xC86016U   /* GigaDevice 32 Mb / 4 MB, 1.8V */
#define FLASH_MODEL_GD25LQ16E       0xC86015U   /* GigaDevice 16 Mb / 2 MB, 1.8V */
#define FLASH_MODEL_UNKNOWN         0xFFFFFFFFU /* "no hint" */

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
    FLASH_READ_QPI,                     /* 4-4-4 Quad Peripheral Interface.
                                         * 목록에만 있는 모드입니다 — 이 드라이버와 TCON SFC는
                                         * opcode와 주소를 항상 1선(1-bit)으로 보내므로 QPI는
                                         * 미지원입니다. Flash_Read()가 FLASH_ERR_UNSUPPORTED로
                                         * 거절하며, 호출자가 이름으로 지정할 수 있도록 남겨 둡니다. */
    FLASH_READ_MODE_MAX
} FLASH_READ_MODE;

/******************************************************************************
 * Read Access Units (Transfer Unit: Byte, Halfword, Word - Default: Word)
 ******************************************************************************/
typedef enum
{
    FLASH_READ_UNIT_DEFAULT  = 0,   /* Default: 32-bit Word access */
    FLASH_READ_UNIT_BYTE     = 1,   /* 8-bit Byte access */
    FLASH_READ_UNIT_HALF     = 2,   /* 16-bit Halfword access */
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
#define FLASH_READ_OPT_CRC              (0x00000004U)   /* Start a new CRC over the read data */
#define FLASH_READ_OPT_CRC_CONT         (0x00000008U)   /* Continue CRC from the previous value */

/******************************************************************************
 * Init Options (Bitmask) - SW-013 / G7
 ******************************************************************************/
#define FLASH_INIT_OPT_NONE             (0x00000000U)
#define FLASH_INIT_OPT_NO_RESET         (0x00000001U)   /* Skip SW reset (fast boot) */

/******************************************************************************
 * Common Standard SPI NOR Flash Command Opcodes
 ******************************************************************************/
#define F_CMD_WREN                        (0x06U) /* Write Enable */
#define F_CMD_VSR_WREN                    (0x50U) /* Volatile Status Register Write Enable */
#define F_CMD_WRDI                        (0x04U) /* Write Disable */
#define F_CMD_RDSR1                       (0x05U) /* Read Status Register 1 */
#define F_CMD_RDSR2                       (0x35U) /* Read Status Register 2 */
#define F_CMD_RDSR3                       (0x15U) /* Read Status Register 3 */
#define F_CMD_WRSR1                       (0x01U) /* Write Status Register 1 (or SR1+SR2) */
#define F_CMD_WRSR2                       (0x31U) /* Write Status Register 2 */
#define F_CMD_WRSR3                       (0x11U) /* Write Status Register 3 */
#define F_CMD_RDID                        (0x9FU) /* Read JEDEC Identification */
#define F_CMD_READ                        (0x03U) /* Read Data */
#define F_CMD_FAST_READ                   (0x0BU) /* Fast Read */
#define F_CMD_DUAL_READ                   (0x3BU) /* Dual Output Read */
#define F_CMD_DUAL_IO_READ                (0xBBU) /* Dual I/O Read */
#define F_CMD_QUAD_READ                   (0x6BU) /* Quad Output Read */
#define F_CMD_QUAD_IO_READ                (0xEBU) /* Quad I/O Read */
#define F_CMD_PAGE_PROGRAM                (0x02U) /* Page Program */
#define F_CMD_QUAD_PAGE_PROGRAM           (0x32U) /* Quad Page Program */
#define F_CMD_SECTOR_ERASE                (0x20U) /* 4KB Sector Erase */
#define F_CMD_BLOCK_ERASE_32K             (0x52U) /* 32KB Block Erase */
#define F_CMD_BLOCK_ERASE_64K             (0xD8U) /* 64KB Block Erase */
#define F_CMD_CHIP_ERASE                  (0xC7U) /* Chip Erase */
#define F_CMD_CHIP_ERASE_ALT              (0x60U) /* Alternative Chip Erase */
#define F_CMD_RESET_ENABLE                (0x66U) /* Software Reset Enable */
#define F_CMD_RESET_DEVICE                (0x99U) /* Software Reset Device */
#define F_CMD_ENTER_4BYTE_ADDR            (0xB7U) /* Enter 4-Byte Address Mode */
#define F_CMD_EXIT_4BYTE_ADDR             (0xE9U) /* Exit 4-Byte Address Mode */
#define F_CMD_WINBOND_DIE_SELECT          (0xC2U) /* Winbond SpiStack Software Die Select */

/******************************************************************************
 * 4-Byte Address Command Variants (SW-013 / req 2)
 *   Some devices use dedicated opcodes when addressing above 16MB (4-Byte
 *   mode) instead of reusing the 3-Byte opcode after Enter-4-Byte-Mode (B7h).
 ******************************************************************************/
#define F_CMD_READ_4B                     (0x13U) /* Read Data (4-Byte) */
#define F_CMD_FAST_READ_4B                (0x0CU) /* Fast Read (4-Byte) */
#define F_CMD_DUAL_READ_4B                (0x3CU) /* Dual Output Read (4-Byte) */
#define F_CMD_DUAL_IO_READ_4B             (0xBCU) /* Dual I/O Read (4-Byte) */
#define F_CMD_QUAD_READ_4B                (0x6CU) /* Quad Output Read (4-Byte) */
#define F_CMD_QUAD_IO_READ_4B             (0xECU) /* Quad I/O Read (4-Byte) */
#define F_CMD_PAGE_PROGRAM_4B             (0x12U) /* Page Program (4-Byte) */
#define F_CMD_QUAD_PAGE_PROGRAM_4B        (0x34U) /* Quad Page Program (4-Byte) */
#define F_CMD_SECTOR_ERASE_4B             (0x21U) /* 4KB Sector Erase (4-Byte) */
#define F_CMD_BLOCK_ERASE_32K_4B          (0x5CU) /* 32KB Block Erase (4-Byte) */
#define F_CMD_BLOCK_ERASE_64K_4B          (0xDCU) /* 64KB Block Erase (4-Byte) */

/******************************************************************************
 * Status Register 1 Bit Masks
 ******************************************************************************/
#define FLASH_SR1_BUSY                  (1U << 0)   /* Erase/Write in progress */
#define FLASH_SR1_WEL                   (1U << 1)   /* Write Enable Latch */
#define FLASH_SR1_BP0                   (1U << 2)   /* Block Protect 0 */
#define FLASH_SR1_BP1                   (1U << 3)   /* Block Protect 1 */
#define FLASH_SR1_BP2                   (1U << 4)   /* Block Protect 2 */
#define FLASH_SR1_BP3                   (1U << 5)   /* Block Protect 3 (or TB) */
#define FLASH_SR1_BP4                   (1U << 6)   /* Block Protect 4 (S6) on GD25LQ64E.
                                                         NOTE: on Macronix parts this very
                                                         bit is the quad-enable (QE) bit,
                                                         so it may only be touched when
                                                         FLASH_FEAT_BP4 is present. */
#define FLASH_SR1_QE_MX25L              (1U << 6)   /* Quad Enable for Macronix */
#define FLASH_SR1_SRP0                  (1U << 7)   /* Status Register Protect 0 */

/******************************************************************************
 * Status Register 2 Bit Masks (Winbond / GigaDevice)
 ******************************************************************************/
#define FLASH_SR2_QE_WINBOND            (1U << 1)   /* Quad Enable for Winbond/GD */

/******************************************************************************
 * Status Register 3 Bit Masks (Winbond 4-Byte parts: W25Q256JV / W25Q512JV)
 *
 *   W25Q256JV datasheet 7.1.5 / 7.1.6:
 *     ADS (S16, read only)  - the address mode the die is in RIGHT NOW.
 *                             "When ADS=0, the device is in the 3-Byte Address
 *                              Mode, when ADS=1 ... 4-Byte Address Mode."
 *     ADP (S17, NV writable) - the mode the die enters at the NEXT power-up,
 *                             "only writable by the non-volatile Write Status
 *                              sequence (06h + 11h)". Factory default is 0.
 *
 *   The driver does not have to change them (it always uses the dedicated
 *   4-Byte opcodes, which work in either mode - 6.1.4), but it should CONFIRM
 *   them instead of assuming, which is what SW-042 does.
 ******************************************************************************/
#define FLASH_SR3_ADS                   (1U << 0)   /* Current address mode (read only) */
#define FLASH_SR3_ADP                   (1U << 1)   /* Power-up address mode (NV) */

/******************************************************************************
 * Feature Flags (Bitmask)
 ******************************************************************************/
#define FLASH_FEAT_NONE                 (0x00U)
#define FLASH_FEAT_DUAL                 (1U << 0)
#define FLASH_FEAT_QUAD                 (1U << 1)
#define FLASH_FEAT_4BYTE_ADDR           (1U << 2)
/* (bit 3 retired - SW-044) There used to be a FLASH_FEAT_MULTI_DIE flag here.
 * It was a second copy of a number the table already fills in:
 * FLASH_INFO.die_count / die_size. Two sources for one fact can drift apart -
 * a new 2-die part could get die_count = 2 and forget the bit, and the writes
 * would then cross the die boundary without a clamp. The bit stays unused
 * instead of renumbering 4..7, which only churns the table. */
#define FLASH_FEAT_STATUS_2BYTE_WRITE   (1U << 4)   /* One 01h frame carries SR1 and SR2
                                                         (GigaDevice GD25LQ family only:
                                                         16 bits mandatory, 8 bits clears
                                                         SR2 - SW-043) */
#define FLASH_FEAT_VSR_WREN             (1U << 5)   /* Supports 50h Volatile SR Write Enable */
#define FLASH_FEAT_BP4                  (1U << 6)   /* SR1 bit6 is a 5th block-protect bit,
                                                         NOT the quad-enable bit */
#define FLASH_FEAT_SR3_ADDR_MODE        (1U << 7)   /* SR3 carries ADS/ADP, so the
                                                         address mode can be confirmed
                                                         instead of assumed (SW-042) */

/******************************************************************************
 * JEDEC ID / Family Matching
 *   JEDEC ID = Manufacturer(8) | Memory Type(8) | Capacity(8).
 *   "Same product family" ignores the capacity byte.
 ******************************************************************************/
#define FLASH_JEDEC_ID_MASK             (0x00FFFFFFU)
#define FLASH_JEDEC_FAMILY_MASK         (0x00FFFF00U)

/******************************************************************************
 * CRC Algorithms (SW-013: selectable at init)
 ******************************************************************************/
typedef enum
{
    FLASH_CRC8 = 0,
    FLASH_CRC16,
    FLASH_CRC16_CCITT,
    FLASH_CRC32,
    FLASH_CRC_CHECKSUM,
    FLASH_CRC_ALGO_MAX
} FLASH_CRC_ALGO;

/******************************************************************************
 * Boot Settings Block (stored in Flash, loaded via basic read at boot)
 ******************************************************************************/
#define FLASH_BOOT_CFG_MAGIC            (0x46424F54UL)  /* "FBOT" */

/* SW-021: SPI clock carried inside the boot settings block.
 *
 * The 24-byte block has no room for a clock field, so spi_hz is appended AFTER
 * crc. Two consequences, both intended:
 *
 *   1. Every existing field keeps its offset (magic 0, jedec 4, model 8,
 *      features 12, sfc_page_size 16, addr_mode 18, crc_algo 19, crc 20), and
 *      Flash_ConfigCrc() still sums exactly the first 20 bytes, so a settings
 *      block written by an older firmware still validates byte for byte.
 *
 *   2. The speed window below doubles as the compatibility valve. An old block
 *      has no spi_hz field at all: those 4 bytes read back as 0xFF (erased) or
 *      0x00, both outside the window, and the driver simply keeps its safe
 *      10 MHz bring-up clock instead of failing the boot.
 *
 * The upper bound is the same 50 MHz that Flash_InitOpt() uses as its normal
 * operational clock, so both init paths behave identically. */
#define FLASH_BOOT_SPI_HZ_MIN           (1000000UL)     /* 1 MHz  */
#define FLASH_BOOT_SPI_HZ_MAX           (50000000UL)    /* 50 MHz */

typedef struct
{
    uint32_t magic;             /* FLASH_BOOT_CFG_MAGIC */
    uint32_t jedec_expect;      /* Expected JEDEC ID (family match allowed) */
    uint32_t model;             /* FLASH_MODEL hint (optional lookup key) */
    uint32_t features;          /* FLASH_FEAT_* operational flags */
    uint16_t sfc_page_size;     /* SFC transfer page size: 2048 or 4096 */
    uint8_t  addr_mode;         /* FLASH_ADDR_MODE (0 = default from table) */
    uint8_t  crc_algo;          /* FLASH_CRC_ALGO (CRC/checksum selection) */
    uint32_t crc;               /* Checksum of the fields above (first 20 bytes) */
    uint32_t spi_hz;            /* SW-021: SPI clock applied after boot; 0/unset = keep 10 MHz */
} FLASH_BOOT_CONFIG;

/* Compile-time guard: the block is provisioned into Flash by production tools,
 * so its size must never drift silently. If a field is added or removed, this
 * line fails to compile and forces a deliberate decision about old blocks. */
typedef char flash_boot_config_size_must_be_28[
    (sizeof(FLASH_BOOT_CONFIG) == 28U) ? 1 : -1];

/******************************************************************************
 * Addressing Mode (SW-013: boot-config selectable) — req 2
 ******************************************************************************/
typedef enum
{
    FLASH_ADDR_MODE_DEFAULT     = 0,  /* derive from the device table addr_bytes */
    FLASH_ADDR_3BYTE,                 /* 3-Byte opcodes + 3-Byte address */
    FLASH_ADDR_4BYTE_OPCODE,          /* dedicated 4-Byte opcodes + 4B address (Method A) */
    FLASH_ADDR_4BYTE_ENTER            /* Enter-4B (B7h) + 3-Byte opcodes + 4B address (Method B) */
} FLASH_ADDR_MODE;

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
 * Common Geometry (identical on every supported device)
 *
 * These used to be fields of FLASH_INFO, but all seven parts use the same
 * values, so storing them per entry only made the table bigger and gave the
 * false impression that they could differ. They are constants now.
 ******************************************************************************/
#define FLASH_PAGE_SIZE                 (256U)      /* page program granularity */
#define FLASH_SECTOR_SIZE               (4096U)     /* smallest erasable unit  */
#define FLASH_BLOCK_SIZE                (65536U)    /* 64 KiB block erase      */

/******************************************************************************
 * Flash Device Information (one live record in RAM, SW-041)
 ******************************************************************************/
typedef struct
{
    /* No model-name string: it was 4 bytes per entry plus the string table.
     * The model is identified by jedec_id / model_id, which the code actually
     * uses (SW-017: smaller ROM, one less thing to keep in sync).
     * SW-041: the record describes the part that is on the bus right now, not
     * a row of a static table; model_id is simply the JEDEC ID. */
    uint32_t            jedec_id;           /* 24-bit JEDEC ID (RDID 9Fh) */
    FLASH_MODEL         model_id;           /* SW-041: equals jedec_id */
    uint32_t            flash_size;         /* Total density in bytes */
    uint8_t             addr_bytes;         /* 3 or 4 */

    /* Quad Enable (QE) Information */
    uint8_t             qe_reg;             /* Status register index: 1, 2, or 0 (none) */
    uint8_t             qe_bit;             /* QE bit position (0..7) */
    uint8_t             features;           /* FLASH_FEAT_* bitmask */

    /* Multi-Die Stack Configuration (W25M512) */
    uint8_t             die_count;          /* 1 for single die, 2 for dual-die */
    uint32_t            die_size;           /* Size of single die in bytes */

    /* Read mode configuration table.
     * SW-035: points at one shared, read-only profile (see flash_table.c)
     * instead of storing a private copy of six 8-byte entries (56 bytes per
     * row). Indexed by FLASH_READ_MODE exactly like the old array, so every
     * consumer keeps reading info->read_cfg[mode] unchanged. */
    const FLASH_READ_CFG *read_cfg;
} FLASH_INFO;

/******************************************************************************
 * Flash Runtime Handle (Stored in RAM)
 ******************************************************************************/
typedef struct
{
    const FLASH_INFO   *info;               /* Pointer to active flash model in ROM */
    FLASH_STATE         state;              /* Current driver state */
    FLASH_ADDR_MODE     addr_mode;          /* Active addressing mode (resolved) */
    FLASH_CRC_ALGO      crc_algo;           /* Active CRC algorithm */
    uint8_t             current_die;        /* Currently selected die index */
    bool                qe_enabled;         /* True if QE is verified enabled */
    uint16_t            sfc_page_size;      /* SFC DMA page size: 2048 or 4096 */
    bool                locked;             /* Selection locked after boot (immutable) */
    uint32_t            last_crc;           /* Accumulated CRC of last Flash_Read */
    uint8_t             sr3;                /* Last Status Register-3 read (SW-042) */
    bool                sr3_valid;          /* True once a die has answered 15h */
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
 *
 *         SW-037: Flash_Write / Flash_Erase no longer call this for you.
 *         Protection is now entirely the caller's decision: call it once
 *         before the first program/erase, and the driver will never quietly
 *         unlock the device behind your back.
 */
int Flash_Unprotect(void);

/**
 * @brief  Set block-protect (BP) bits on the active die from a mask to protect
 *         memory regions. Preserves QE and other bits. (SW-013 / G2)
 *         bits 3..0 = BP3..BP0. Bit 4 = BP4 (only on parts that carry
 *         FLASH_FEAT_BP4, e.g. GD25LQ64E); it is ignored elsewhere so that the
 *         Macronix quad-enable bit (same position) is never disturbed.
 */
int Flash_Protect(uint8_t bp_mask);

/**
 * @brief  Send Write Enable (06h) and verify WEL bit is set.
 */
int Flash_WriteEnable(void);

/**
 * @brief  Send Volatile Status-Register Write Enable (50h).
 *         Datasheet: 50h does NOT set WEL and is valid ONLY for the
 *         immediately following WRSR — no commands may be inserted between.
 *         Used for volatile SR writes (e.g. QE) when the device supports it.
 */
int Flash_WriteEnableVolatile(void);

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
 * @brief  Report the address-mode state decoded from Status Register-3 (SW-042).
 *
 *         Only parts with FLASH_FEAT_SR3_ADDR_MODE (Winbond 4-Byte devices:
 *         W25Q256JV, W25Q512JV, each die of the W25M512 stack) have these bits;
 *         everywhere else the call returns FLASH_ERR_UNSUPPORTED and writes
 *         nothing, rather than inventing a value from an undefined register.
 *
 * @param  p_ads:  1 = the die is in 4-Byte Address Mode right now (NULL ok)
 * @param  p_adp:  1 = the die will power up in 4-Byte Address Mode (NULL ok)
 */
int Flash_GetAddrModeState(uint8_t *p_ads, uint8_t *p_adp);

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
 *         SW-037: block protection is NOT cleared automatically. If the target
 *         region is protected the device ignores the program and this function
 *         still returns FLASH_OK, so call Flash_Unprotect() first.
 */
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode);

/**
 * @brief  Erase Sector (4KB), Block (32KB/64KB), or Chip.
 *         SW-037: block protection is NOT cleared automatically; see Flash_Write.
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
