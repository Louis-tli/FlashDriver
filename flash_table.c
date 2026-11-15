/******************************************************************************
 * File Name   : flash_table.c
 * Description : Flash Device Profiles and Model Lookup Table for ARM Cortex-M0
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 * Note        : Stored in ROM (.rodata) to conserve Cortex-M0 SRAM.
 *
 ******************************************************************************/

#include "flash_table.h"

/******************************************************************************
 * Shared Read-Mode Config Builders
 * 
 *   READCFG(3B opcode, 4B opcode, dummy_cycles, dummy_bytes, dummy_value,
 *           dummy_last, mode_byte, io_width)
 ******************************************************************************/
#define READCFG(_c,_c4,_dc,_db,_dv,_dl,_mb,_w)                          \
    { .command = (_c), .command_4b = (_c4), .dummy_cycles = (_dc),      \
      .dummy_bytes = (_db), .dummy_value = (_dv), .dummy_last = (_dl),   \
      .mode_byte = (_mb), .io_width = (_w) }

/* Identical on every supported device */
#define RC_SINGLE       READCFG(F_CMD_READ,         F_CMD_READ_4B,          0U, 0U, 0x00U, 0x00U, 0x00U, 1U)
#define RC_FAST         READCFG(F_CMD_FAST_READ,    F_CMD_FAST_READ_4B,     8U, 1U, 0xFFU, 0xFFU, 0x00U, 1U)
#define RC_DUAL         READCFG(F_CMD_DUAL_READ,    F_CMD_DUAL_READ_4B,     8U, 1U, 0xFFU, 0xFFU, 0x00U, 2U)
#define RC_QUAD         READCFG(F_CMD_QUAD_READ,    F_CMD_QUAD_READ_4B,     8U, 1U, 0xFFU, 0xFFU, 0x00U, 4U)

/*    CMD -> ADDR -> DUMMY_LAST (FIRST dummy byte = M7-M0 mode byte slot)
 *        -> DUMMY_VAL -> ... -> DATA
 *   Winbond   : first dummy (M7-M0) MUST be Fxh   -> dummy_last 0xFF
 *   GigaDevice: M7-M0 != Axh keeps normal mode    -> dummy_last 0x00
 *   Macronix  : no mode byte (dummy don't care)   -> dummy_last 0x00
 * Remaining dummy bytes are "don't care" on every vendor -> dummy_value 0x00. */
#define RC_DUAL_IO_WB   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B,  4U, 1U, 0x00U, 0xFFU, 0x00U, 2U) /* Winbond */
#define RC_DUAL_IO_NM   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B,  4U, 1U, 0x00U, 0x00U, 0x00U, 2U) /* Macronix */
#define RC_DUAL_IO_GD   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B,  6U, 1U, 0x00U, 0x00U, 0x00U, 2U) /* GD25LQ64E */
#define RC_QUAD_IO_WB6  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  6U, 3U, 0x00U, 0xFFU, 0x00U, 4U) /* Winbond, dummy 6 */
#define RC_QUAD_IO_WB8  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  8U, 3U, 0x00U, 0xFFU, 0x00U, 4U) /* Winbond, dummy 8 */
#define RC_QUAD_IO_NM6  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  6U, 3U, 0x00U, 0x00U, 0x00U, 4U) /* MX/GD, dummy 6 */
#define RC_QUAD_IO_NM8  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  8U, 3U, 0x00U, 0x00U, 0x00U, 4U) /* Macronix, dummy 8 */



/* read command configurations table */

static const FLASH_READ_CFG s_readcmd_wb6[FLASH_READ_MODE_MAX] = {   /* Winbond, quad dummy 6 */
    [FLASH_READ_SINGLE]  = RC_SINGLE,
    [FLASH_READ_FAST]    = RC_FAST,
    [FLASH_READ_DUAL]    = RC_DUAL,
    [FLASH_READ_DUAL_IO] = RC_DUAL_IO_WB,
    [FLASH_READ_QUAD]    = RC_QUAD,
    [FLASH_READ_QUAD_IO] = RC_QUAD_IO_WB6
};

static const FLASH_READ_CFG s_readcmd_wb8[FLASH_READ_MODE_MAX] = {   /* Winbond, quad dummy 8 */
    [FLASH_READ_SINGLE]  = RC_SINGLE,
    [FLASH_READ_FAST]    = RC_FAST,
    [FLASH_READ_DUAL]    = RC_DUAL,
    [FLASH_READ_DUAL_IO] = RC_DUAL_IO_WB,
    [FLASH_READ_QUAD]    = RC_QUAD,
    [FLASH_READ_QUAD_IO] = RC_QUAD_IO_WB8
};

static const FLASH_READ_CFG s_readcmd_nm6[FLASH_READ_MODE_MAX] = {   /* Macronix, quad dummy 6 */
    [FLASH_READ_SINGLE]  = RC_SINGLE,
    [FLASH_READ_FAST]    = RC_FAST,
    [FLASH_READ_DUAL]    = RC_DUAL,
    [FLASH_READ_DUAL_IO] = RC_DUAL_IO_NM,
    [FLASH_READ_QUAD]    = RC_QUAD,
    [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM6
};

static const FLASH_READ_CFG s_readcmd_nm8[FLASH_READ_MODE_MAX] = {   /* Macronix, quad dummy 8 */
    [FLASH_READ_SINGLE]  = RC_SINGLE,
    [FLASH_READ_FAST]    = RC_FAST,
    [FLASH_READ_DUAL]    = RC_DUAL,
    [FLASH_READ_DUAL_IO] = RC_DUAL_IO_NM,
    [FLASH_READ_QUAD]    = RC_QUAD,
    [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM8
};

/* GigaDevice GD25LQ16E / 32E / 64E: BBh takes 6 dummy cycles and EBh takes
 * M7-M0 followed by 6 dummy cycles - identical across the three densities. */
static const FLASH_READ_CFG s_readcmd_gd6[FLASH_READ_MODE_MAX] = {
    [FLASH_READ_SINGLE]  = RC_SINGLE,
    [FLASH_READ_FAST]    = RC_FAST,
    [FLASH_READ_DUAL]    = RC_DUAL,
    [FLASH_READ_DUAL_IO] = RC_DUAL_IO_GD,
    [FLASH_READ_QUAD]    = RC_QUAD,
    [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM6
};


/******************************************************************************
 * Two-tier device description (SW-041)
 *
 * Why the flat table is gone
 * --------------------------
 * The old table repeated every vendor fact once per density: nine rows of
 * 28 bytes (252 bytes of .rodata), and adding a W25Q64JV meant adding a row.
 * Yet nothing in a row is density-specific except the size - and the JEDEC ID
 * already carries the size. Byte 2 of the 9Fh answer is log2(size in bytes):
 *
 *     0x15 -> 1<<21 = 2 MB      0x18 -> 1<<24 = 16 MB
 *     0x16 -> 1<<22 = 4 MB      0x19 -> 1<<25 = 32 MB
 *     0x17 -> 1<<23 = 8 MB
 *
 * So the driver now keeps:
 *
 *   tier 1   one row per VENDOR      (3 rows: Winbond, Macronix, GigaDevice)
 *   tier 2   the size, computed      (1UL << density byte)
 *   plus     a tiny exception list   (2 parts where the formula lies)
 *
 * A new density of a known family therefore costs ZERO table bytes - it is
 * resolved by the same three rows. Only a new vendor (1 row) or a part that
 * breaks the density formula (1 exception row) needs data.
 *
 * The 16 MB boundary does double duty: parts up to 16 MB are addressed with
 * 3 bytes and use the vendor's 6-dummy-cycle read profile, bigger parts need
 * 4 address bytes and 8 dummy cycles. That single rule reproduces all nine
 * old rows exactly (verified part by part in TEST 19).
 *
 * Exception 1 - W25Q512JV reports density byte 0x20. 1<<32 does not fit in a
 *   32-bit word and would mean "4 GB"; the package is 64 MB.
 * Exception 2 - W25M512 is a SpiStack of two 32 MB dies, so its 0x19 byte
 *   describes ONE die, not the package.
 ******************************************************************************/
typedef struct
{
    uint8_t  manufacture_id;        /* JEDEC manufacturer byte (byte 0 of 9Fh) */
    uint8_t  qe_reg;                /* Status register holding QE: 1, 2, or 0 = none */
    uint8_t  qe_bit;                /* QE bit position inside that register */
    uint16_t features;              /* FLASH_FEAT_* true for the whole family */
    const FLASH_READ_CFG *rc_6;     /* read profile for parts <= 16 MB */
    const FLASH_READ_CFG *rc_8;     /* read profile for parts >  16 MB */
} FLASH_VENDOR_CFG;

static const FLASH_VENDOR_CFG flash_vender[] =
{
    /* Winbond: QE는 SR2 bit 1이고, Quad read의 첫 번째 dummy byte는 Fxh(모드 바이트, RC_QUAD_IO_WB* 참조)여야 한다. 
     * 256MB 이상 크기의 부품은 8개의 dummy cycles이 필요하고 128MB 이하 크기의 부품은 6개의 dummy cycles가 필요하다.  */
    {   0xEFU, 2U, 1U,
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_VSR_WREN,
        s_readcmd_wb6, s_readcmd_wb8 },

    /* Macronix: QE는 SR1의 6번 비트이며, 별도로 전송해야 할 모드 바이트는 없다. 
     * 512Mb 제품은 8개의 dummy cycles이 필요한 반면, 512Kb 제품은 6개의 dummy cycles이 필요하다. */
    {   0xC2U, 1U, 6U,
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD,
        s_readcmd_nm6, s_readcmd_nm8 },

    /* GigaDevice: 두 제품군 모두에서 QE는 SR2의 1번 비트이고, BP4는 SR1의 6번 비트다. 
     * ( GD25Q32E Rev1.5의 표 6/7 및 GD25LQ32E Rev1.4의 표 5/6 )
     * 두 제품군 간에 차이가 있는 부분은 WRSR(Write status register) 프레임 구성이다. 따라서 해당 설정은 이곳이 아니라, 
     * 아래에 명시된 메모리 유형 바이트(memory-type byte)를 바탕으로 각 부품별로 결정된다. 
     *  휘발성 SR 쓰기 활성화(50h) 및 읽기 프로파일은 두 제품군에서 공통으로 적용된다. */
    {   0xC8U, 2U, 1U,
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_VSR_WREN | FLASH_FEAT_BP4,
        s_readcmd_gd6, s_readcmd_gd6 }
};

#define FLASH_VENDOR_COUNT      (sizeof(flash_vender) / sizeof(flash_vender[0]))

/* Size at which 3-byte addressing stops working (0xFFFFFF is the last
 * 3-byte address, so exactly 16 MB is still addressable). */
#define FLASH_SIZE_1MB          (1024UL * 1024UL)
#define FLASH_SIZE_16MB         (16UL * 1024UL * 1024UL)
#define FLASH_JEDEC_MASK        (0x00FFFFFFUL)

typedef struct
{
    uint32_t jedec_id;              /* exact 24-bit ID */
    uint32_t flash_size;            /* real package size in bytes */
    uint8_t  die_count;             /* 1 = single die, 2 = stacked */
    uint16_t features;              /* extra FLASH_FEAT_* on top of the vendor row */
} FLASH_EXCEPTION_CFG;


static const FLASH_EXCEPTION_CFG s_exception[] =
{
    /* W25Q512JV: density byte 0x20 would mean 2^32 bytes. Real size: 64 MB. */
    { 0xEF4020U, 64UL*FLASH_SIZE_1MB, 1U, 0U },

    /* W25M512: SpiStack, density byte 0x19 describes one 32 MB die. */
    { 0xEF6119U, 64UL*FLASH_SIZE_1MB, 2U, FLASH_FEAT_MULTI_DIE }
};

#define FLASH_EXCEPTION_COUNT   (sizeof(s_exception) / sizeof(s_exception[0]))

/* Description of the ONE part that is on the bus. 28 bytes of RAM instead of
 * 252 bytes of ROM describing nine parts the board does not have. Every call
 * to Flash_FindDevice() refills it, so the returned pointer stays valid only
 * until the next lookup - exactly what Flash_Init() and Flash_GetInfo() need.
 */
static FLASH_INFO s_device;

/******************************************************************************
 * Vendor Lookup
 ******************************************************************************/
static const FLASH_VENDOR_CFG *Flash_FindVendor(uint8_t manufacture_id)
{
    for (uint32_t i = 0U; i < FLASH_VENDOR_COUNT; i++) {
        if (flash_vender[i].manufacture_id == manufacture_id) {
            return &flash_vender[i];
        }
    }
    return NULL;
}

/******************************************************************************
 * Model Lookup Functions
 ******************************************************************************/

const FLASH_INFO *Flash_FindDevice(uint32_t jedec_id)
{
    const FLASH_VENDOR_CFG    *p_vendor;
    const FLASH_EXCEPTION_CFG *p_ex = NULL;
    uint8_t  density;
    uint8_t  die_count = 1U;
    uint16_t extra = 0U;
    uint32_t size;

    jedec_id &= FLASH_JEDEC_MASK;

    /* Tier 1: does this manufacturer exist at all? */
    p_vendor = Flash_FindVendor((uint8_t)(jedec_id >> 16));
    if (p_vendor == NULL) {
        return NULL;                        /* unknown vendor: unsupported */
    }

    /* Exceptions come first: they exist precisely because the byte lies. */
    for (uint32_t i = 0U; i < FLASH_EXCEPTION_COUNT; i++) {
        if (s_exception[i].jedec_id == jedec_id) {
            p_ex = &s_exception[i];
            break;
        }
    }

    if (p_ex != NULL) {
        size      = p_ex->flash_size;
        die_count = p_ex->die_count;
        extra     = p_ex->features;
    }
    else {
        /* Tier 2: the density byte is log2(bytes). 0 means "no dice"; 32 and
         * above cannot be shifted into a 32-bit size and always need an
         * exception row instead. */
        density = (uint8_t)(jedec_id & 0xFFU);
        if ((density == 0U) || (density > 31U)) {
            return NULL;
        }
        size = 1UL << density;
    }

    /* Fill the one live description. Order follows the struct in flash.h so a
     * reader can check the two side by side. */
    s_device.jedec_id   = jedec_id;
    s_device.model_id   = jedec_id;         /* SW-041: model == JEDEC ID */
    s_device.flash_size = size;
    s_device.addr_bytes = (size > FLASH_SIZE_16MB) ? 4U : 3U;
    s_device.qe_reg     = p_vendor->qe_reg;
    s_device.qe_bit     = p_vendor->qe_bit;
    s_device.features   = (uint16_t)(p_vendor->features | extra |
                                     ((size > FLASH_SIZE_16MB) ? FLASH_FEAT_4BYTE_ADDR : 0U));

    /* SW-042: Winbond's 4-Byte parts report the live and the power-up address
     * mode in SR3 (W25Q256JV datasheet 7.1.5/7.1.6; likewise W25Q512JV and each
     * W25Q256JV die inside the W25M512 stack). The same two bits mean something
     * else on 16 MB parts, so the flag is tied to the 4-Byte boundary rather
     * than to the manufacturer alone. */
    if (((jedec_id >> 16) == 0xEFU) && (size > FLASH_SIZE_16MB)) {
        s_device.features = (uint16_t)(s_device.features | FLASH_FEAT_SR3_ADDR_MODE);
    }

    /* SW-043: GigaDevice's 3V (GD25Q, type byte 0x40) and 1.8V (GD25LQ, type
     * byte 0x60) families share density codes, QE position and BP4, but not the
     * WRSR frame:
     *   GD25Q32E Rev1.5 7.4  - "For command code of 01H / 31H / 11H, the Status
     *     Register bits S7~S0 / S15~S8 / S23~S16 would be written. CS# must be
     *     driven high after the eighth bit of the data byte has been latched
     *     in." One register per frame, 8 bits each.
     *   GD25LQ32E Rev1.4 7.4 - "CS# must be driven high after the eighth or
     *     sixteen bit ... If CS# is driven high after eighth bit of the data
     *     byte, the alterable bits in Status Register-2 (S15~S8) will be
     *     cleared to 0." 16 bits are mandatory; an 8-bit frame silently eats QE.
     * The type byte is the only thing that tells the two apart, so the coupled
     * 16-bit flag goes to the LQ type alone. Every other GigaDevice type keeps
     * the standard one-byte-per-register path, which is what its datasheet
     * family documents. */
    if ((jedec_id >> 16) == 0xC8U) {
        const uint8_t mem_type = (uint8_t)((jedec_id >> 8) & 0xFFU);
        if (mem_type == 0x60U) {
            s_device.features = (uint16_t)(s_device.features |
                                           FLASH_FEAT_STATUS_2BYTE_WRITE);
        }
    }

    s_device.die_count  = die_count;
    /* A SpiStack today is always two identical dies (W25M512), so shift
     * instead of dividing: keeps __aeabi_uidiv out of the image. */
    s_device.die_size   = (die_count == 2U) ? (size >> 1) : 0U;
    s_device.read_cfg   = (size > FLASH_SIZE_16MB) ? p_vendor->rc_8 : p_vendor->rc_6;

    return &s_device;
}

const FLASH_INFO *Flash_FindDeviceByModel(FLASH_MODEL model_id)
{
    /* SW-041: a model is its JEDEC ID, so this is the same lookup. The old
     * enum existed only to name nine rows that no longer exist. */
    if (model_id == FLASH_MODEL_UNKNOWN) {
        return NULL;
    }
    return Flash_FindDevice((uint32_t)model_id);
}
