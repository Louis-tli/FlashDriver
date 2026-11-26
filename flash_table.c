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
    { .command = (_c), .command_4b = (_c4),     \
      .dummy_cycles = (_dc), .dummy_bytes = (_db), .dummy_value = (_dv), .dummy_last = (_dl),   \
      .mode_byte = (_mb), .io_width = (_w) }


/* Identical on every supported device */
#define RC_SINGLE       READCFG(F_CMD_READ,         F_CMD_READ_4B,          0U, 0U, 0x00U, 0x00U,   0x00U, 1U)
#define RC_FAST         READCFG(F_CMD_FAST_READ,    F_CMD_FAST_READ_4B,     8U, 1U, 0xFFU, 0xFFU,   0x00U, 1U)
#define RC_DUAL         READCFG(F_CMD_DUAL_READ,    F_CMD_DUAL_READ_4B,     8U, 1U, 0xFFU, 0xFFU,   0x00U, 2U)
#define RC_QUAD         READCFG(F_CMD_QUAD_READ,    F_CMD_QUAD_READ_4B,     8U, 1U, 0xFFU, 0xFFU,   0x00U, 4U)

/*    CMD -> ADDR -> DUMMY_LAST (첫 dummy byte 가 M7-M0 mode byte slot)
 *        -> DUMMY_VAL -> ... -> DATA
 *   Winbond   : 첫 dummy(M7-M0)가 반드시 Fxh 여야 한다  -> dummy_last 0xFF
 *   GigaDevice: M7-M0 이 Axh 가 아니면 normal mode 유지  -> dummy_last 0x00
 *   Macronix  : mode byte 없음(dummy 값 무관)            -> dummy_last 0x00
 * 남는 dummy byte 는 모든 vendor 에서 "don't care" 이다 -> dummy_value 0x00. */
#define RC_DUAL_IO_WB   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B,  4U, 1U, 0x00U, 0xFFU,   0x00U, 2U) /* Winbond */
#define RC_DUAL_IO_NM   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B,  4U, 1U, 0x00U, 0x00U,   0x00U, 2U) /* Macronix */
#define RC_DUAL_IO_GD   READCFG(F_CMD_DUAL_IO_READ, F_CMD_DUAL_IO_READ_4B,  6U, 1U, 0x00U, 0x00U,   0x00U, 2U) /* GD25LQ64E */
#define RC_QUAD_IO_WB6  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  6U, 3U, 0x00U, 0xFFU,   0x00U, 4U) /* Winbond, dummy 6 */
#define RC_QUAD_IO_WB8  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  8U, 3U, 0x00U, 0xFFU,   0x00U, 4U) /* Winbond, dummy 8 */
#define RC_QUAD_IO_NM6  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  6U, 3U, 0x00U, 0x00U,   0x00U, 4U) /* MX/GD, dummy 6 */
#define RC_QUAD_IO_NM8  READCFG(F_CMD_QUAD_IO_READ, F_CMD_QUAD_IO_READ_4B,  8U, 3U, 0x00U, 0x00U,   0x00U, 4U) /* Macronix, dummy 8 */



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

/* GigaDevice GD25LQ16E / 32E / 64E: BBh 는 dummy 6 cycle, EBh 는 M7-M0
 * 뒤에 dummy 6 cycle 을 받는다 — 세 용량이 모두 같다. */
static const FLASH_READ_CFG s_readcmd_gd6[FLASH_READ_MODE_MAX] = {
    [FLASH_READ_SINGLE]  = RC_SINGLE,
    [FLASH_READ_FAST]    = RC_FAST,
    [FLASH_READ_DUAL]    = RC_DUAL,
    [FLASH_READ_DUAL_IO] = RC_DUAL_IO_GD,
    [FLASH_READ_QUAD]    = RC_QUAD,
    [FLASH_READ_QUAD_IO] = RC_QUAD_IO_NM6
};


/******************************************************************************
 * Vendor information
 *
 *     0x15 -> 1<<21 = 2 MB      0x18 -> 1<<24 = 16 MB
 *     0x16 -> 1<<22 = 4 MB      0x19 -> 1<<25 = 32 MB
 *     0x17 -> 1<<23 = 8 MB
 *
 * 이 driver 의 구조는 이렇다:
 *
 *   tier 1   VENDOR 당 한 행      (Winbond, Macronix, GigaDevice 3행)
 *   tier 2   크기는 계산          (1UL << density byte)
 *   plus     작은 예외 목록       (공식이 거짓말하는 부품 2개)
 *
 * 따라서 아는 계열에 새 용량이 추가되어도 table 은 0 byte 든다 — 같은 3행이
 * 그대로 해석한다. data 가 필요한 것은 새 vendor(1행) 또는 density 공식이
 * 깨지는 부품(예외 1행)뿐이다.
 *
 * 16 MB 경계는 두 가지 일을 동시에 한다: 16 MB 이하 부품은 address 3 byte +
 * vendor 의 6-dummy-cycle read profile 을 쓰고, 그보다 큰 부품은 address
 * 4 byte + dummy 8 cycle 이 필요하다. 이 규칙 하나로 예전 9행이 그대로
 * 재현된다(TEST 19 에서 부품별로 확인).
 *
 * 예외 1 - W25Q512JV 는 density byte 로 0x20 을 보고한다. 1<<32 는 32비트
 *   word 에 들어가지 않고 "4 GB" 라는 뜻이 되어 버리지만, 실제 package 는
 *   64 MB 다.
 * 예외 2 - W25M512 는 32 MB die 2개를 묶은 SpiStack 이라, 0x19 는 package 가
 *   아니라 die 하나를 뜻한다.
 ******************************************************************************/
typedef struct
{
    uint8_t  manufacture_id;        /* JEDEC manufacturer byte (byte 0 of 9Fh) */
    uint8_t  qe_reg;                /* QE 를 담은 status register: 1, 2, 또는 0 = 없음 */
    uint8_t  qe_bit;                /* QE bit position inside that register */
    uint16_t features;              /* FLASH_FEAT_* true for the whole family */
    const FLASH_READ_CFG *rc_6;     /* read profile for parts <= 16 MB */
    const FLASH_READ_CFG *rc_8;     /* read profile for parts >  16 MB */
} FLASH_VENDOR_CFG;

static const FLASH_VENDOR_CFG flash_vender[] =
{
    /* Winbond */
    /* QE는 SR2 bit 1이고, Quad read의 첫 번째 dummy byte는 Fxh(모드 바이트, RC_QUAD_IO_WB* 참조)여야 한다. 
     * 256MB 이상 크기의 부품은 8개의 dummy cycles이 필요하고 128MB 이하 크기의 부품은 6개의 dummy cycles가 필요하다.  */
    {   0xEFU, 2U, 1U,
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_VSR_WREN,
        s_readcmd_wb6, s_readcmd_wb8 },

    /* GigaDevice */
    /* 두 제품군 모두에서 QE는 SR2의 1번 비트이고, BP4는 SR1의 6번 비트다. 
     * ( GD25Q32E Rev1.5의 표 6/7 및 GD25LQ32E Rev1.4의 표 5/6 )
     * 두 제품군 간에 차이가 있는 부분은 WRSR(Write status register) 프레임 구성이다. 따라서 해당 설정은 이곳이 아니라, 
     * 아래에 명시된 메모리 유형 바이트(memory-type byte)를 바탕으로 각 부품별로 결정된다. 
     *  휘발성 SR 쓰기 활성화(50h) 및 읽기 프로파일은 두 제품군에서 공통으로 적용된다. */
    {   0xC8U, 2U, 1U,
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD | FLASH_FEAT_VSR_WREN | FLASH_FEAT_BP4,
        s_readcmd_gd6, s_readcmd_gd6 },

    /* Macronix */
    /* QE는 SR1의 6번 비트이며, 별도로 전송해야 할 모드 바이트는 없다. 
     * 512Mb 제품은 8개의 dummy cycles이 필요한 반면, 512Kb 제품은 6개의 dummy cycles이 필요하다. */
    {   0xC2U, 1U, 6U,
        FLASH_FEAT_DUAL | FLASH_FEAT_QUAD,
        s_readcmd_nm6, s_readcmd_nm8 }
};

#define FLASH_VENDOR_COUNT      (sizeof(flash_vender) / sizeof(flash_vender[0]))

/* 3-byte addressing 이 더는 통하지 않는 크기(0xFFFFFF 이 마지막 3-byte
 * address 이므로 정확히 16 MB 까지는 addressed 된다). */
#define FLASH_SIZE_1MB          (1024UL * 1024UL)
#define FLASH_SIZE_16MB         (16UL * 1024UL * 1024UL)
#define FLASH_JEDEC_MASK        (0x00FFFFFFUL)

typedef struct
{
    uint32_t jedec_id;              /* exact 24-bit ID */
    uint32_t flash_size;            /* real package size in bytes */
    uint8_t  die_count;             /* 1 = single die, 2 = stacked */
    uint16_t features;              /* vendor 행 위에 추가로 붙는 FLASH_FEAT_* */
} FLASH_EXCEPTION_CFG;


static const FLASH_EXCEPTION_CFG s_exception[] =
{
    /* W25Q512JV: density byte 0x20 은 2^32 byte 를 뜻한다. 실제 크기: 64 MB. */
    { 0xEF4020U, 64UL*FLASH_SIZE_1MB, 1U, FLASH_FEAT_NONE },

    /* W25M512: SpiStack 이라 density byte 0x19 는 32 MB die 하나를 뜻한다. */
    { 0xEF6119U, 64UL*FLASH_SIZE_1MB, 2U, FLASH_FEAT_NONE }
};

#define FLASH_EXCEPTION_COUNT   (sizeof(s_exception) / sizeof(s_exception[0]))

/* 지금 bus 에 붙어 있는 단 한 개 부품의 설명. board 에 없는 부품 9개를
 * 252 byte ROM 으로 설명하는 대신 RAM 28 byte 만 쓴다. Flash_FindDevice()
 * 를 부를 때마다 다시 채워지므로, 돌려받은 pointer 는 다음 lookup 전까지만
 * 유효하다 — Flash_Init() 과 Flash_GetInfo() 가 필요로 하는 만큼과 같다.
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

    /* Tier 1: 이 manufacturer 가 아예 존재하는가? */
    p_vendor = Flash_FindVendor((uint8_t)(jedec_id >> 16));
    if (p_vendor == NULL) {
        return NULL;                        /* unknown vendor: unsupported */
    }

    /* 예외 행을 먼저 본다 — 그 행들은 바로 byte 가 거짓말을 하기 때문에 있다. */
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
        /* Tier 2: density byte 는 log2(bytes) 다. 0 은 "해당 없음"이고, 32 이상은
         * 32비트 size 로 shift 할 수 없으므로 언제나 예외 행이 필요하다. */
        density = (uint8_t)(jedec_id & 0xFFU);

        /* Macronix 는 density byte 의 상위 3 bit 를 "계열 표시"로 쓴다. 1.8V
         * MX25U1632F 는 RDID 로 C2 25 35 를 답하는데(MX25U1632F Rev 1.2 표 6),
         * 0x35 는 2^53 이 아니라 "2^21 byte = 16 Mb" 라는 뜻이다 — 즉 0x30 +
         * log2(byte). 같은 방식이 MX25U3232F(0x36) / 6432F(0x37) /
         * 12832F(0x38) 에 그대로 이어지고, 3V MX25L 계열(0x15, 0x18 …)은
         * 상위 bit 가 0 이라 이 mask 에 걸리지 않는다.
         * 그래서 예외 행 4개 대신 이 한 줄이 MX25U 4종을 전부 해석한다. */
        if (((jedec_id >> 16) == 0xC2U) && ((density & 0xE0U) == 0x20U)) {
            density &= 0x1FU;
        }

        if ((density == 0U) || (density > 31U)) {
            return NULL;
        }
        size = 1UL << density;
    }

    /* 살아 있는 description 하나를 채운다. 채우는 순서는 flash.h 의 struct
     * 순서를 따른다 — 나란히 놓고 대조할 수 있게 하기 위해서다. */
    s_device.jedec_id   = jedec_id;
    s_device.model_id   = jedec_id;         /* SW-041: model == JEDEC ID */
    s_device.flash_size = size;
    s_device.addr_bytes = (size > FLASH_SIZE_16MB) ? 4U : 3U;
    s_device.qe_reg     = p_vendor->qe_reg;
    s_device.qe_bit     = p_vendor->qe_bit;
    s_device.features   = (uint16_t)(p_vendor->features | extra |
                                     ((size > FLASH_SIZE_16MB) ? FLASH_FEAT_4BYTE_ADDR : 0U));

    /* SW-042: Winbond 의 4-Byte 부품은 현재·power-up address mode 를 SR3 에
     * 보고한다(W25Q256JV datasheet 7.1.5/7.1.6, W25Q512JV 와 W25M512 stack 안의
     * 각 W25Q256JV die 도 같다). 같은 두 bit 가 16 MB 부품에서는 다른 의미이므로,
     * 이 flag 는 manufacturer 만이 아니라 4-Byte 경계에 묶는다. */
    if (((jedec_id >> 16) == 0xEFU) && (size > FLASH_SIZE_16MB)) {
        s_device.features = (uint16_t)(s_device.features | FLASH_FEAT_SR3_ADDR_MODE);
    }

    /* SW-043: GigaDevice 3V(GD25Q, type byte 0x40) 계열과 1.8V(GD25LQ, type
     * byte 0x60) 계열은 density code, QE 위치, BP4 가 같지만 WRSR frame 은 다르다:
     *   GD25Q32E Rev1.5 7.4  - "For command code of 01H / 31H / 11H, the Status
     *     Register bits S7~S0 / S15~S8 / S23~S16 would be written. CS# must be
     *     driven high after the eighth bit of the data byte has been latched
     *     in." Register 하나가 frame 하나이고 각 8 bit 다.
     *   GD25LQ32E Rev1.4 7.4 - "CS# must be driven high after the eighth or
     *     sixteen bit ... If CS# is driven high after eighth bit of the data
     *     byte, the alterable bits in Status Register-2 (S15~S8) will be
     *     cleared to 0." 16 bit 가 필수이고, 8 bit frame 은 QE 를 조용히 먹는다.
     * 두 계열을 구분해 주는 것은 type byte 뿐이므로, 16 bit 로 묶는 flag 는 LQ
     * type 에만 준다. 나머지 GigaDevice type 은 그 datasheet family 가 문서화한
     * 대로 register 당 1 byte 인 표준 경로를 그대로 쓴다. */
    if ((jedec_id >> 16) == 0xC8U) {
        const uint8_t mem_type = (uint8_t)((jedec_id >> 8) & 0xFFU);
        if (mem_type == 0x60U) {
            s_device.features = (uint16_t)(s_device.features |
                                           FLASH_FEAT_STATUS_2BYTE_WRITE);
        }
    }

    s_device.die_count  = die_count;
    /* 지금의 SpiStack 은 언제나 동일한 die 2개(W25M512)이므로 나눗셈 대신
     * shift 를 쓴다 — image 에 __aeabi_uidiv 가 들어오지 않게 하려는 것이다. */
    s_device.die_size   = (die_count == 2U) ? (size >> 1) : 0U;
    s_device.read_cfg   = (size > FLASH_SIZE_16MB) ? p_vendor->rc_8 : p_vendor->rc_6;

    return &s_device;
}

const FLASH_INFO *Flash_FindDeviceByModel(FLASH_MODEL model_id)
{
    /* SW-041: model 은 곧 JEDEC ID 이므로 이 lookup 은 같은 일이다. 예전
     * enum 은 더는 존재하지 않는 9행에 이름을 붙이기 위해서만 있었다. */
    if (model_id == FLASH_MODEL_UNKNOWN) {
        return NULL;
    }
    return Flash_FindDevice((uint32_t)model_id);
}
