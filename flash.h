/******************************************************************************
 * File Name   : flash.h
 * Description : Generic SPI NOR Flash Driver Header for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compliant)
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
    FLASH_ERR_UNSUPPORTED   = -2,   /* JEDEC ID not found in supported table */
    FLASH_ERR_TIMEOUT       = -3,   /* Operation timed out (e.g., BUSY polling) */
    FLASH_ERR_PARAM         = -4,   /* Invalid argument passed */
    FLASH_ERR_ALIGNMENT     = -5,   /* Buffer or address alignment fault */
    FLASH_ERR_BUSY          = -6,   /* Flash is currently busy with another operation */
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
 * Read Configuration Descriptor (SFC & SPI)
 ******************************************************************************/
typedef struct
{
    uint8_t command;            /* Read instruction opcode */
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

/******************************************************************************
 * Public Driver APIs
 ******************************************************************************/

/**
 * @brief  Initialize the Flash Driver.
 *         Probes JEDEC ID via safe Single SPI mode, searches model table,
 *         configures address mode, verifies/enables QE bit, and readies SFC.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_Init(void);

/**
 * @brief  Issue Software Reset to Flash (66h followed by 99h).
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_Reset(void);

/**
 * @brief  Read 24-bit JEDEC ID using 9Fh opcode.
 * @param  p_jedec_id: Pointer to 32-bit variable receiving the 24-bit ID.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_ReadJEDECID(uint32_t *p_jedec_id);

/**
 * @brief  Read Status Register 1, 2, or 3.
 * @param  reg_idx: 1 (05h), 2 (35h), or 3 (15h).
 * @param  p_val: Pointer to receive the register byte.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_ReadStatus(uint8_t reg_idx, uint8_t *p_val);

/**
 * @brief  Write Status Register.
 *         Handles 1-byte vs 2-byte (SR1+SR2) differences automatically.
 * @param  reg_idx: Status register index to write.
 * @param  val: New value for register.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_WriteStatus(uint8_t reg_idx, uint8_t val);

/**
 * @brief  Send Write Enable instruction (06h) and verify WEL bit is set.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_WriteEnable(void);

/**
 * @brief  Send Write Disable instruction (04h).
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_WriteDisable(void);

/**
 * @brief  Poll BUSY bit in Status Register 1 until clear or timeout.
 * @param  timeout_loops: Max iteration count for polling loop.
 * @return FLASH_OK if ready, FLASH_ERR_TIMEOUT if timed out.
 */
int Flash_WaitBusy(uint32_t timeout_loops);

/**
 * @brief  Verify and enable Quad Enable (QE) bit in status register if required.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_EnableQuad(void);

/**
 * @brief  Select active die on multi-die stacked chips (W25M512).
 * @param  die_idx: Die index (0 or 1).
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_SelectDie(uint8_t die_idx);

/**
 * @brief  Read data from Flash.
 *         Supports both direct SPI and TCON SFC Memory-Mapped DMA transfer.
 *         Access unit can be BYTE (1), HALFWORD (2), or WORD (4). Default is WORD (0 or 4).
 * @param  addr: 32-bit linear Flash address (must be aligned to unit size).
 * @param  buf: Destination buffer (must be aligned to unit size).
 * @param  count: Number of units to read (e.g. if unit is WORD, count=10 reads 10 words = 40 bytes).
 * @param  mode: Read mode (Single, Fast, Dual, Quad, etc.).
 * @param  unit: FLASH_READ_UNIT_BYTE, FLASH_READ_UNIT_HALFWORD, or FLASH_READ_UNIT_WORD (0 for default WORD).
 * @param  options: Bitmask of FLASH_READ_OPT_* flags.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_Read(uint32_t addr, void *buf, uint32_t count,
               FLASH_READ_MODE mode, FLASH_READ_UNIT unit,
               uint32_t options);

/**
 * @brief  Program data into Flash (Single 02h or Quad 32h).
 *         Automatically splits cross-boundary writes at 256-byte page boundaries
 *         and handles multi-die switching for W25M512.
 * @param  addr: 32-bit linear Flash address.
 * @param  buf: Source buffer containing data to write.
 * @param  len: Length in bytes.
 * @param  mode: FLASH_PROGRAM_SINGLE or FLASH_PROGRAM_QUAD.
 * @return FLASH_OK on success, or negative error code.
 */
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode);

/**
 * @brief  Erase Sector (4KB), Block (32KB/64KB), or Chip.
 *         Handles multi-die operations automatically.
 * @param  addr: Linear Flash address (ignored for CHIP erase).
 * @param  mode: FLASH_ERASE_SECTOR, BLOCK_32K, BLOCK_64K, or CHIP.
 * @return FLASH_OK on success, or negative error code.
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

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_H__ */
