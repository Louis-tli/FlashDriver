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
/* SW-041: "model" 은 곧 24-bit JEDEC ID 다.
 *
 * 예전 enum 은 제품 하나당 table 행 하나를 강요해서, 용량이 늘 때마다 data 도
 * 늘었다. 값 자체를 ID 로 두면 Flash_FindDevice() 가 vendor + density 로 부품을
 * 해석하므로 이 type 은 table 비용을 전혀 만들지 않는다.
 *
 * 저장된 부팅 설정과의 호환: 예전 enum 을 쓰던 firmware 가 쓴 설정에는 "model"
 * field 에 0..7 이 들어 있다. Flash_Init() 은 0x01000000 미만 값을 "힌트 없음"
 * 으로 보고 JEDEC probe 로 넘어가는데, 그 부품들이 FLASH_MODEL_UNKNOWN 으로
 * 동작하던 것과 같은 동작이다. */
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
 *   16MB 를 넘는 주소를 쓸 때 Enter-4-Byte-Mode(B7h) 후 3-Byte opcode 를 재사용
 *   하는 대신, 전용 opcode 를 쓰는 부품이 있다.
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
#define FLASH_SR1_BP4                   (1U << 6)   /* GD25LQ64E 의 Block Protect 4(S6). 주의: Macronix 부품에서는 같은 bit 가 quad-enable(QE) bit 이므로 FLASH_FEAT_BP4 가 있을 때만 건드려야 한다. */
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
 *     ADS (S16, read only)  - 지금 이 die 가 들어가 있는 address mode.
 *                             "When ADS=0, the device is in the 3-Byte Address
 *                              Mode, when ADS=1 ... 4-Byte Address Mode."
 *     ADP (S17, NV writable) - 다음 power-up 때 die 가 진입할 mode.
 *                             "only writable by the non-volatile Write Status
 *                              sequence (06h + 11h)". 출하 기본값은 0.
 *
 *   driver 는 이 bit 를 바꿀 필요가 없다(항상 전용 4-Byte opcode 를 쓰고, 그
 *   opcode 는 어느 mode 에서든 동작한다 - 6.1.4). 다만 가정하지 말고 확인해야
 *   하는데, SW-042 가 그 일을 한다.
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
/* (bit 3 retired - SW-044) 예전에 이 자리에 FLASH_FEAT_MULTI_DIE flag 가 있었다.
 * 그것은 table 이 이미 채워 주는 숫자를 두 번째로 복사한 것이었다:
 * FLASH_INFO.die_count / die_size. 같은 사실의 출처가 둘이면 어긋날 수 있다 —
 * 새 2-die 부품이 die_count = 2 를 넣고 bit 는 잊으면, write 가 clamp 없이 die
 * 경계를 넘어간다. bit 번호를 다시 매기면(4..7) table 만 소란스러워지므로, 그
 * bit 는 미사용으로 남긴다. */
#define FLASH_FEAT_STATUS_2BYTE_WRITE   (1U << 4)   /* 01h frame 하나가 SR1 과 SR2 를 함께 쓴다(GigaDevice GD25LQ 계열만: 16 bit 필수, 8 bit 면 SR2 가 소거됨 - SW-043) */
#define FLASH_FEAT_VSR_WREN             (1U << 5)   /* Supports 50h Volatile SR Write Enable */
#define FLASH_FEAT_BP4                  (1U << 6)   /* SR1 bit6 은 5번째 block-protect bit 이고 quad-enable bit 이 아니다 */
#define FLASH_FEAT_SR3_ADDR_MODE        (1U << 7)   /* SR3 가 ADS/ADP 를 담고 있으므로 address mode 를 가정하지 않고 확인할 수 있다(SW-042) */

/******************************************************************************
 * JEDEC ID / Family Matching
 *   JEDEC ID = Manufacturer(8) | Memory Type(8) | Capacity(8).
 *   "같은 제품군"은 capacity byte 를 무시한다.
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

/* SW-021: SPI clock 을 boot settings block 안에 함께 넣는다.
 *
 * 24 byte block 에는 clock field 를 넣을 자리가 없어서 spi_hz 를 crc 뒤에
 * 덧붙였다. 그 결과는 두 가지이고, 둘 다 의도한 것이다:
 *
 *   1. 기존 field 는 모두 offset 이 그대로다(magic 0, jedec 4, model 8,
 *      features 12, sfc_page_size 16, addr_mode 18, crc_algo 19, crc 20).
 *      Flash_ConfigCrc() 도 여전히 앞 20 byte 만 합산하므로, 예전 firmware 가
 *      쓴 설정 block 도 byte 단위로 그대로 검증된다.
 *
 *   2. 아래 speed window 가 호환용 안전판 역할을 겸한다. 예전 block 에는
 *      spi_hz field 자체가 없어서 그 4 byte 는 0xFF(erased) 또는 0x00 으로
 *      읽히고, 둘 다 window 밖이다. 이때 driver 는 부팅을 실패시키는 대신
 *      안전한 10 MHz bring-up clock 을 그대로 유지한다.
 *
 * 상한은 Flash_InitOpt() 가 평상시 동작 clock 으로 쓰는 50 MHz 와 같게 두어,
 * 두 init 경로가 똑같이 동작하게 했다. */
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

/* Compile-time guard: 이 block 은 생산 도구가 Flash 에 써 넣으므로 크기가 조용히
 * 변하면 안 된다. field 가 늘거나 줄면 이 줄이 compile 을 실패시켜, 예전 block
 * 을 어떻게 다룰지 반드시 결정하게 만든다. */
typedef char flash_boot_config_size_must_be_28[
    (sizeof(FLASH_BOOT_CONFIG) == 28U) ? 1 : -1];

/******************************************************************************
 * Addressing Mode (SW-013: boot-config selectable) — req 2
 ******************************************************************************/
typedef enum
{
    FLASH_ADDR_MODE_DEFAULT     = 0,  /* derive from the device table addr_bytes */
    FLASH_ADDR_3BYTE,                 /* 3-Byte opcodes + 3-Byte address */
    FLASH_ADDR_4BYTE_OPCODE,          /* 전용 4-Byte opcode + 4B address (Method A) */
    FLASH_ADDR_4BYTE_ENTER            /* Enter-4B(B7h) + 3-Byte opcode + 4B address (Method B) */
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
 * 예전에는 FLASH_INFO 의 field 였지만, 지원하는 모든 부품이 같은 값을 써서
 * 항목별로 저장하면 table 만 커지고 "부품마다 다를 수 있다"는 오해만 생겼다.
 * 지금은 상수다.
 ******************************************************************************/
#define FLASH_PAGE_SIZE                 (256U)      /* page program granularity */
#define FLASH_SECTOR_SIZE               (4096U)     /* smallest erasable unit  */
#define FLASH_BLOCK_SIZE                (65536U)    /* 64 KiB block erase      */

/******************************************************************************
 * Flash Device Information (one live record in RAM, SW-041)
 ******************************************************************************/
typedef struct
{
    /* model 이름 문자열은 없다: 항목당 4 byte 에 문자열 table 이 더 붙었다.
     * model 은 jedec_id / model_id 로 식별되고, 실제로 code 가 쓰는 것도 그것이다
     * (SW-017: ROM 이 작아지고, 맞춰야 할 것이 하나 줄었다).
     * SW-041: 이 record 는 정적 table 의 한 행이 아니라 지금 bus 에 있는 부품을
     * 설명한다. model_id 는 곧 JEDEC ID 다. */
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
     * SW-035: 여섯 개 8-byte 항목(행당 56 byte)을 private copy 로 들고 있는 대신
     * flash_table.c 의 공유 read-only profile 하나를 가리킨다. 예전 array 와 똑같이
     * FLASH_READ_MODE 로 index 하므로, 모든 사용처는 info->read_cfg[mode] 를 그대로
     * 읽으면 된다. */
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
 * @brief 주소/pointer 가 주어진 byte 경계에 정렬되어 있는지 확인한다.
 */
static inline bool Flash_IsAligned(uintptr_t val, uint32_t alignment_bytes)
{
    return ((val & (alignment_bytes - 1U)) == 0U);
}

/**
 * @brief 두 JEDEC ID 가 같은 제품군인지 반환한다
 *        (manufacturer + memory type 이 같고 capacity byte 는 무시).
 */
static inline bool Flash_FamilyMatch(uint32_t jedec_a, uint32_t jedec_b)
{
    return ((jedec_a & FLASH_JEDEC_FAMILY_MASK) == (jedec_b & FLASH_JEDEC_FAMILY_MASK));
}

/******************************************************************************
 * Public Driver APIs
 ******************************************************************************/

/**
 * @brief  boot settings block 의 checksum 을 계산한다.
 */
uint32_t Flash_ConfigCrc(const FLASH_BOOT_CONFIG *cfg);

/**
 * @brief  모든 부품에서 동작하는 기본(안전) single-line Read Data(03h).
 *         QE / address-mode 설정이 하나도 적용되기 전에도 쓸 수 있다.
 *         부팅할 때 settings block 을 읽는 데 사용한다.
 * @param  addr: 32-bit Flash address (config block 은 low memory 에 있다).
 * @param  buf:  결과를 받을 buffer.
 * @param  len:  읽을 byte 수.
 */
int Flash_BasicRead(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief  Boot-time initialization: ID 응답 확인 -> basic-read 로 settings
 *         block 을 읽음 -> settings 로 configure -> 선택을 lock.
 *         ID 가 틀리거나 불일치하면 driver 는 fail-safe error state 로 들어간다.
 * @param  config_addr: FLASH_BOOT_CONFIG block 의 Flash address.
 * @return FLASH_OK on success, or negative error code (fail-safe on failure).
 */
int Flash_BootInit(uint32_t config_addr);

/**
 * @brief  Boot-time initialization with options (e.g. FLASH_INIT_OPT_NO_RESET).
 */
int Flash_BootInitOpt(uint32_t config_addr, uint32_t options);

/**
 * @brief  Flash Driver 초기화(auto-detect 방식).
 *         안전한 Single SPI mode 로 JEDEC ID 를 probe 하고, model 을 찾고,
 *         address mode 를 설정하고, die 마다 QE bit 를 확인/활성화한다.
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
 * @brief  active die 의 block-protect(BP) bit 를 모두 지워서, program / erase 가
 *         조용히 실패하는 일이 없게 한다. QE 와 나머지 bit 는 보존한다.
 *         (SW-013 / G2)
 *
 *         SW-037: Flash_Write / Flash_Erase 는 더 이상 이 함수를 대신 불러 주지
 *         않는다. 보호는 이제 전적으로 호출자의 결정이다: 첫 program/erase 전에
 *         한 번 부르면, driver 가 뒤에서 몰래 칩을 unlock 하는 일은 없다.
 */
int Flash_Unprotect(void);

/**
 * @brief  mask 로 active die 의 block-protect(BP) bit 를 세워 메모리 영역을
 *         보호한다. QE 와 나머지 bit 는 보존한다. (SW-013 / G2)
 *         bits 3..0 = BP3..BP0, bit 4 = BP4 (FLASH_FEAT_BP4 를 가진 부품만, 예:
 *         GD25LQ64E).
 *         SW-046: BP3 가 없는 부품에서는 mask bit3 이상을 넘기면
 *         FLASH_ERR_PARAM 을 돌려준다. Winbond W25Q 는 128 Mbit 이하에서 SR1
 *         bit5 가 BP3 가 아니라 TB(top/bottom) 이므로(W25Q32JV §7.1,
 *         W25Q256JV §7.1.4), 그 mask 를 그대로 쓰면 보호가 걸리지 않았는데도
 *         OK 가 나오거나 보호 방향이 뒤집힌다. bit4 는 FLASH_FEAT_BP4 부품에서만
 *         유효하다 — Macronix 의 같은 자리는 quad-enable bit 이므로 건드리지
 *         않는다.
 */
int Flash_Protect(uint8_t bp_mask);

/**
 * @brief  Send Write Enable (06h) and verify WEL bit is set.
 */
int Flash_WriteEnable(void);

/**
 * @brief  Volatile Status-Register Write Enable(50h)을 보낸다.
 *         Datasheet: 50h 는 WEL 을 세우지 않고, 바로 뒤따르는 WRSR 만 무장한다 —
 *         그 사이에 어떤 command 도 끼우면 안 된다.
 *         부품이 지원할 때 volatile SR write(예: QE) 에 쓴다.
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
 * @brief  Status Register-3 에서 해석한 address-mode 상태를 알려준다(SW-042).
 *
 *         FLASH_FEAT_SR3_ADDR_MODE 를 가진 부품(Winbond 4-Byte 계열: W25Q256JV,
 *         W25Q512JV, W25M512 stack 의 die 각각)만 이 bit 를 갖는다. 나머지 부품에서는
 *         정의되지 않은 register 값으로 추측하지 않고 FLASH_ERR_UNSUPPORTED 를
 *         돌려주며 아무것도 쓰지 않는다.
 *
 * @param  p_ads:  1 = 이 die 는 지금 4-Byte Address Mode 상태다 (NULL ok)
 * @param  p_adp:  1 = 이 die 는 power-up 시 4-Byte Address Mode 로 뜬다 (NULL ok)
 */
int Flash_GetAddrModeState(uint8_t *p_ads, uint8_t *p_adp);

/**
 * @brief  Flash 에서 data 를 읽는다(직접 SPI 또는 SFC Memory-Mapped DMA).
 *         FLASH_READ_OPT_CRC 가 설정되어 있으면, CRC 를 모든 transfer chunk 에
 *         걸쳐 누적하며 Flash_GetLastCRC() 로 꺼낼 수 있다.
 */
int Flash_Read(uint32_t addr, void *buf, uint32_t count,
               FLASH_READ_MODE mode, FLASH_READ_UNIT unit,
               uint32_t options);

/**
 * @brief  Flash 에 data 를 program 한다(Single 02h 또는 Quad 32h).
 *         SW-037: block protect 를 자동으로 해제하지 않는다. 대상 영역이 보호되어
 *         있으면 부품이 program 을 무시하는데 이 함수는 그래도 FLASH_OK 를 돌려주므로,
 *         먼저 Flash_Unprotect() 를 부를 것.
 */
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode);

/**
 * @brief  Sector(4KB), Block(32KB/64KB) 또는 Chip 을 erase 한다.
 *         SW-037: block protect 를 자동으로 해제하지 않는다. Flash_Write 참조.
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
