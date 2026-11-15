/******************************************************************************
 * File Name   : flash.c
 * Description : Generic SPI NOR Flash Driver Core Implementation
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * Boot flow (SW-013): ID check -> basic-read settings -> configure -> lock.
 ******************************************************************************/


#include "flash.h"
#include "flash_table.h"
#include "flash_spi.h"
#include "flash_sfc.h"
#include "flash_cmd.h"
#include "flash_crc.h"
#include "flash_delay.h"
#include "flash_xfer.h"

#include <stddef.h>     /* offsetof — SW-021 keeps the config checksum window fixed */

/******************************************************************************
 * Global Runtime Handle Instance (Allocated in SRAM)
 ******************************************************************************/
FLASH_HANDLE g_flash_handle =
{
    .info          = NULL,
    .state         = FLASH_STATE_UNINIT,
    .addr_mode     = FLASH_ADDR_MODE_DEFAULT,
    .current_die   = 0U,
    .qe_enabled    = false,
    .sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K,
    .locked        = false,
    .last_crc      = 0U
};

/******************************************************************************
 * Helpers: one-frame SPI transfers (SW-016)
 *
 *   opcode, address, dummy byte 를 작은 header buffer 에 모으고, 동작 전체를
 *   Flash_SPI_Transact() 에 /CS frame 하나로 넘긴다(tx_len / data phase / rx_len).
 *   호출부마다 SPI byte 를 하나씩 찔러 넣던 방식은 쓰지 않는다.
 ******************************************************************************/
static int Flash_SpiReadFrame(uint8_t cmd, uint32_t addr, uint8_t addr_bytes,
                              const FLASH_READ_CFG *read_cfg,
                              uint8_t *rx_buf, uint32_t rx_len)
{
    uint8_t hdr[FLASH_CMD_HDR_MAX];
    const uint32_t hdr_len = Flash_CmdBuildHeader(hdr, cmd, addr, addr_bytes, read_cfg);
    if (hdr_len == 0U) {
        return FLASH_ERR_PARAM;
    }

    const FLASH_SPI_XFER xfer = {
        .tx_buf = hdr, .tx_len = hdr_len,
        .rx_buf = rx_buf, .rx_len = rx_len
    };
    Flash_SPI_Transact(&xfer);
    return FLASH_OK;
}

static int Flash_SpiWriteFrame(uint8_t cmd, uint32_t addr, uint8_t addr_bytes,
                               const uint8_t *data, uint32_t data_len, bool quad)
{
    uint8_t hdr[FLASH_CMD_HDR_MAX];
    const uint32_t hdr_len = Flash_CmdBuildHeader(hdr, cmd, addr, addr_bytes, NULL);
    if (hdr_len == 0U) {
        return FLASH_ERR_PARAM;
    }

    FLASH_SPI_XFER xfer = { .tx_buf = hdr, .tx_len = hdr_len };
    if ((data != NULL) && (data_len > 0U)) {
        if (quad) {
            xfer.quad_buf = data;
            xfer.quad_len = data_len;
        }
        else {
            xfer.data_buf = data;
            xfer.data_len = data_len;
        }
    }
    Flash_SPI_Transact(&xfer);
    return FLASH_OK;
}

/******************************************************************************
 * Helper: 임의의 opcode + payload 로 raw command frame 을 보낸다(address 없음).
 ******************************************************************************/
static void Flash_SpiRawFrame(const uint8_t *bytes, uint32_t len)
{
    const FLASH_SPI_XFER xfer = { .tx_buf = bytes, .tx_len = len };
    Flash_SPI_Transact(&xfer);
}


/******************************************************************************
 * Public API: Flash_SelectDie (W25M512)
 ******************************************************************************/
int Flash_SelectDie(uint8_t die_idx)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }
    /* Single die: nothing to select, and the caller may pass any index */
    if (info->die_count < 2U) {
        return FLASH_OK;
    }

    if (die_idx >= info->die_count) {
        return FLASH_ERR_PARAM;
    }

    if (g_flash_handle.current_die == die_idx) {
        return FLASH_OK;
    }

    /* Winbond Software Die Select (C2h + die_id) — one CS frame */
    const uint8_t frame[2] = { F_CMD_WINBOND_DIE_SELECT, die_idx };
    Flash_SpiRawFrame(frame, (uint32_t)sizeof(frame));

    Flash_DelayUs(50U);         /* die-select settle (SW-014: was 500 loops) */
    g_flash_handle.current_die = die_idx;

    return FLASH_OK;
}


/******************************************************************************
 * Helper: Resolve Multi-Die Address & Die Index (W25M512)
 * (짧은 지연은 flash_delay.c 의 Flash_DelayUs 를 쓴다 — SW-014)
 ******************************************************************************/
static int Flash_CheckDie(uint32_t addr, uint8_t *p_die, uint32_t *p_die_addr)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* die_size == 0 은 "die 하나"라는 뜻이다 — table 은 stack 인 경우에만 이 값을
     * 채우므로, 이것만 검사하면 된다(SW-044). */
    if (info->die_size != 0U) {
        /* Which die does this address land on, and where inside that die? */
        const uint8_t target_die = (uint8_t)(addr / info->die_size);
        if (target_die >= info->die_count) {
            return FLASH_ERR_PARAM;
        }

        *p_die = target_die;
        *p_die_addr = addr % info->die_size;
    }
    else {
        *p_die = 0U;
        *p_die_addr = addr;
    }

    int status = Flash_SelectDie(*p_die);
    if (status != FLASH_OK) {
        return status;
    } 

    return FLASH_OK;
}

/******************************************************************************
 * Helper: Reject a transfer that leaves the device (SW-036)
 *
 *   칩은 주소를 검사하지 않는다. 용량을 넘는 address bit 는 그냥 decode 되지
 *   않아서, 끝을 넘어선 offset 은 배열의 시작으로 되돌아간다(wrap). 실제
 *   board 에서 이는 호출자의 offset/len 실수가 첫 sector(보통 bootloader)를
 *   조용히 덮어쓰고도 모든 호출이 FLASH_OK 를 돌려준다는 뜻이다 — 가장 나쁜
 *   종류의 고장: error 없이 data 가 깨진다.
 *
 *   여기서 비교 두 번으로 그것을 FLASH_ERR_PARAM 으로 바꾼다. 뺄셈 형태를 쓰는
 *   것은 의도적이다: "addr + len" 은 0xFFFFFFFF 근처 주소에서 overflow 되어
 *   순진한 검사를 통과해 버린다.
 ******************************************************************************/
static int Flash_CheckRange(uint32_t addr, uint32_t len)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    if (len == 0U) {
        /* Nothing moves, so no address can be out of range. */
        return FLASH_OK;
    }

    if (addr >= info->flash_size) {
        return FLASH_ERR_PARAM;
    }

    if (len > (info->flash_size - addr)) {
        return FLASH_ERR_PARAM;
    }

    return FLASH_OK;
}

/******************************************************************************
 * Helper: 지금 쓸 addressing mode 를 결정한다(boot-config 우선, 없으면 기본값)
 ******************************************************************************/
static FLASH_ADDR_MODE Flash_ResolveAddrMode(uint8_t cfg_mode, const FLASH_INFO *info)
{
    /* boot block 에는 enum 네 값 중 어느 것도 아닌 addr_mode 가 들어 있을 수 있다
     * (write 가 깨졌거나, 새 도구가 쓴 값을 예전 code 가 읽은 경우). CRC 는 byte
     * 가 온전히 도착했다는 것만 증명하고 그 값의 의미까지 보증하지 않으므로,
     * cast 하기 전에 범위를 검사해야 한다.
     *
     * 모르는 값은 의도적으로 3-Byte 로 떨어뜨린다. 4-byte 부품이면
     * Flash_ApplyHandleConfig() 의 가드가 부팅을 거절하고, 이 줄이 없으면 칩은
     * 아직 3-byte mode 인데 4번째 address byte 를 받아 그것을 data 로 밀어내며
     * 틀린 data 를 FLASH_OK 와 함께 돌려준다. 3-byte 부품에서는 결과가 DEFAULT
     * 와 같으므로, 이 fallback 이 멀쩡한 board 를 나쁘게 만들 수는 없다. */
    if (cfg_mode > (uint8_t)FLASH_ADDR_4BYTE_ENTER) {
        return FLASH_ADDR_3BYTE;
    }

    if (cfg_mode == (uint8_t)FLASH_ADDR_MODE_DEFAULT) {
        /* 기본값: 부품의 native width 에서 유도한다(4-Byte 부품은 Method A). */
        return (info->addr_bytes == 4U) ? FLASH_ADDR_4BYTE_OPCODE : FLASH_ADDR_3BYTE;
    }
    return (FLASH_ADDR_MODE)cfg_mode;
}

/******************************************************************************
 * Helper: ACTIVE die 에 공통이 아닌 설정(4-Byte mode + QE)을 적용한다.
 *   multi-die stack 은 die 마다 status register 가 따로 있으므로, 이 작업은
 *   die 당 한 번씩 실행해야 한다(SW-013 / G1).
 ******************************************************************************/
static int Flash_ApplyDieConfig(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* Enter-4-Byte mode (B7h) is only needed for Method B (FLASH_ADDR_4BYTE_ENTER). */
    if ( g_flash_handle.addr_mode == FLASH_ADDR_4BYTE_ENTER) {
        Flash_SPI_SendCmd(F_CMD_ENTER_4BYTE_ADDR);
        (void)Flash_WaitBusy(10000U);
    }

    /* SW-042: address mode 를 가정하지 말고 확인한다.
     *
     *   W25Q256JV 는 현재 mode 를 SR3.ADS 에, power-up mode 를 SR3.ADP 에 둔다
     *   (datasheet 7.1.5 / 7.1.6). 선택적인 B7h 뒤에 읽으므로 ADS 는 이 die 가
     *   지금 실제로 들어가 있는 mode 를 뜻한다.
     *
     *   stack 의 die 마다 status register 가 따로 있어서, 이 동작은
     *   Flash_ApplyProductInit() 에서 die 당 한 번 실행된다. handle 은 마지막
     *   으로 읽은 값, 즉 active die 의 답을 보관한다.
     *
     *   해당 bit 가 없는 부품은 건너뛴다 — 추측하지 않는다. */
    if ((info->features & FLASH_FEAT_SR3_ADDR_MODE) != 0U) {
        const int sr_status = Flash_ReadStatus(3U, &g_flash_handle.sr3);
        if (sr_status != FLASH_OK) {
            return sr_status;
        }
        g_flash_handle.sr3_valid = true;
    }

    /* Verify / enable Quad Mode on this die */
    if ((info->features & FLASH_FEAT_QUAD) != 0U) {
        return Flash_EnableQuad();
    }

    return FLASH_OK;
}

/******************************************************************************
 * Helper: active device 의 모든 die 에 공통이 아닌 init 을 적용한다.
 ******************************************************************************/
static int Flash_ApplyProductInit(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* die_count is 1 on a single-die part, so no separate "is it a stack" bit */
    if (info->die_count > 1U) {
        for (uint8_t d = 0U; d < info->die_count; d++) {
            int status = Flash_SelectDie(d);
            if (status != FLASH_OK) {
                return status;
            }
            status = Flash_ApplyDieConfig();
            if (status != FLASH_OK) {
                return status;
            }
        }
        return Flash_SelectDie(0U);
    }

    return Flash_ApplyDieConfig();
}

/******************************************************************************
 * Public API: Flash_ConfigCrc
 ******************************************************************************/
uint32_t Flash_ConfigCrc(const FLASH_BOOT_CONFIG *cfg)
{
    if (cfg == NULL) {
        return 0U;
    }

    const uint8_t *p = (const uint8_t *)cfg;
    /* SW-021: checksum window 는 crc 가 시작하는 곳에서 끝난다 — "sizeof - 4" 가
     * 아니다. spi_hz 를 crc 뒤에 덧붙였기 때문에, 이렇게 해야 window 가 원래의
     * 20 byte 그대로이고 예전 firmware 가 쓴 block 도 여전히 검증된다. */
    const uint32_t n = (uint32_t)offsetof(FLASH_BOOT_CONFIG, crc);
    uint32_t sum = 0x5A5A5A5AU;
    for (uint32_t i = 0U; i < n; i++) {
        sum += (uint32_t)p[i];
    }
    return sum;
}

/******************************************************************************
 * Public API: Flash_BasicRead (safe single-line 03h read, pre-configuration)
 ******************************************************************************/
int Flash_BasicRead(uint32_t addr, void *buf, uint32_t len)
{
    if ((buf == NULL) || (len == 0U)) {
        return FLASH_ERR_PARAM;
    }

    /* 03h + 3-byte address 는 지원하는 모든 부품의 low memory 에서 동작한다.
     * header 내보내고 data 받는 frame 하나. */
    return Flash_SpiReadFrame(F_CMD_READ, addr, 3U, NULL, (uint8_t *)buf, len);
}

/******************************************************************************
 * Helper (SW-018): data-path gate.
 *
 * Read / Write / Erase / Protect 는 driver 가 READY 일 때만 허용한다.
 *
 * "info != NULL" 만으로는 부족한 이유: 답하던 부품이 사라지면 re-init 이
 * NO_CONNECT 를 세우지만, handle 은 앞서 찾은 model 을 그대로 가리킨다. 이
 * 검사가 없으면 driver 는 죽은 bus 에 command 와 address byte 를 밀어 넣고
 * 떠다니는 값(0xFF)을 받아 온다 — 분명한 error 대신 조용한 garbage read 다.
 *
 * probe/bring-up helper(Flash_BasicRead, Flash_Reset, status polling)는 이
 * gate 를 일부러 쓰지 않는다: driver 가 READY 되기 전에도 동작해야 한다.
 ******************************************************************************/
static int Flash_CheckConnect(void)
{
    /* 일부러 if-chain 으로 둔다: 여기를 switch 로 두면 compiler 가 .rodata 에
     * 12-byte jump table 을 만든다. 값 네 개에 그건 과하다. */
    if (g_flash_handle.state == FLASH_STATE_READY) {
        return FLASH_OK;
    }
    if (g_flash_handle.state == FLASH_STATE_NO_CONNECT) {
        return FLASH_ERR_NO_CONNECT;
    }
    if (g_flash_handle.state == FLASH_STATE_ERROR) {
        return FLASH_ERR_HARDWARE;
    }
    return FLASH_ERR_PARAM;      /* UNINIT: driver was never initialised */
}

/******************************************************************************
 * Helper (SW-017): 공통 bring-up + identification 순서.
 *
 * Flash_BootInitOpt() 와 Flash_InitOpt() 가 이 순서를 각자 복사해 들고 있었다
 * (SPI/SFC init -> 선택적 reset -> JEDEC ID -> floating-bus 검사). 한 벌만
 * 두면 ROM 이 약 150 byte 줄어든다.
 *
 * 성공하면 *p_jedec_id 에 24-bit ID 가 들어가고 driver state 는 건드리지 않는다.
 * 실패하면 fail-safe state(ERROR / NO_CONNECT)가 이미 설정되어 있다.
 ******************************************************************************/
static int Flash_ProbeBus(uint32_t options, uint32_t *p_jedec_id)
{
    /* 1. Controller bring-up: SPI at a safe 10 MHz, SFC with a 4 KiB page */
    Flash_SPI_Init();
    Flash_SPI_SetSpeed(10000000U);
    Flash_SFC_Init(SFC_DEFAULT_PAGE_SIZE_4K);
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;

    /* 2. Software reset clears any stale bus state (skippable for fast boot) */
    if ((options & FLASH_INIT_OPT_NO_RESET) == 0U) {
        (void)Flash_Reset();
        Flash_DelayUs(2000U);   /* ~2ms reset settle (SW-014: was 50000 loops) */
    }

    /* 3. Read the JEDEC ID (9Fh) */
    uint32_t id = 0U;
    int status = Flash_ReadJEDECID(&id);
    if (status != FLASH_OK) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    /* 4. An all-0 or all-1 answer means nothing is driving the bus */
    id &= FLASH_JEDEC_ID_MASK;
    if ((id == 0x000000U) || (id == 0xFFFFFFU)) {
        g_flash_handle.state = FLASH_STATE_NO_CONNECT;
        return FLASH_ERR_NO_CONNECT;
    }

    *p_jedec_id = id;
    return FLASH_OK;
}

/******************************************************************************
 * Helper (SW-017): 선택한 부품에 맞게 runtime handle 을 채우고, die 별 설정
 * (4-byte mode + QE)을 적용한다. 두 init 경로가 함께 쓴다.
 ******************************************************************************/
static int Flash_ApplyHandleConfig(const FLASH_INFO *p_info,
                                   uint8_t cfg_addr_mode,
                                   FLASH_CRC_ALGO crc_algo)
{
    g_flash_handle.info        = p_info;
    g_flash_handle.addr_mode   = Flash_ResolveAddrMode(cfg_addr_mode, p_info);
    g_flash_handle.crc_algo    = crc_algo;
    g_flash_handle.current_die = 0U;
    g_flash_handle.qe_enabled  = false;
    g_flash_handle.last_crc    = 0U;
    g_flash_handle.sr3         = 0U;
    g_flash_handle.sr3_valid   = false;

    /* SW-042: 4-Byte 부품은 3-Byte mode 로 구동할 수 없다.
     *
     *   이 driver 에는 Extended Address Register(C8h) 경로가 없어서, 3-Byte mode
     *   에서는 16 MB 를 넘는 주소가 칩에서 조용히 버려지고 배열 아래로 wrap 된다.
     *   그런데 Flash_CheckRange() 는 flash_size 와만 비교하므로 그 범위를 여전히
     *   합법이라고 판정한다. 즉 FLASH_OK 를 돌려주면서 data 가 깨진다.
     *
     *   이 조합을 거절하는 데는 비교 하나가 들고, 최악의 고장 mode 를 통합자가
     *   볼 수 있는 boot-time error 로 바꾼다. 이 판단을 config validator 가 아니라
     *   여기서 하는 이유는, 부품을 JEDEC probe 이후에야 알 수 있기 때문이다. */
    if ((g_flash_handle.addr_mode == FLASH_ADDR_3BYTE) && (p_info->addr_bytes == 4U)) {
        g_flash_handle.info  = NULL;   /* leave nothing half-configured behind */
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_PARAM;
    }

    /* Non-common configuration has to run once per die (QE + 4-byte mode) */
    const int status = Flash_ApplyProductInit();
    if (status != FLASH_OK) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    g_flash_handle.state = FLASH_STATE_READY;
    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_BootInit (ID check -> settings -> configure -> lock)
 ******************************************************************************/
int Flash_BootInitOpt(uint32_t config_addr, uint32_t options)
{
    /* Once the selection is locked it is immutable for the rest of the boot. */
    if (g_flash_handle.locked) {
        return FLASH_ERR_BUSY;
    }

    /* 1-2. Bring the bus up and read the JEDEC ID */
    uint32_t jedec_id = 0U;
    int status = Flash_ProbeBus(options, &jedec_id);
    if (status != FLASH_OK) {
        return status;
    }

    /* 3. Basic-read the boot settings block from Flash */
    FLASH_BOOT_CONFIG cfg;
    status = Flash_BasicRead(config_addr, &cfg, (uint32_t)sizeof(cfg));
    if (status != FLASH_OK) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    /* 4. Validate the settings block (magic + checksum) */
    if ((cfg.magic != FLASH_BOOT_CFG_MAGIC) ||
        (cfg.crc != Flash_ConfigCrc(&cfg))) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_PARAM;
    }

    /* 5. 선택된 제품을 찾는다. config 의 model field 가 필요한 경우 정확한 variant
     *    를 지정하고, 그렇지 않으면 기대 JEDEC ID 로 넘어간다. */
    const FLASH_INFO *p_info = NULL;
    /* SW-041: "model" field 는 이제 JEDEC ID 다. 예전 firmware 가 쓴 설정에는 그
     * 자리에 옛 enum(0..7)이 들어 있는데 그것은 ID 가 아니므로 "힌트 없음"으로
     * 보고, 아래 jedec_expect probe 가 판단하게 한다. */
    if ((cfg.model != (uint32_t)FLASH_MODEL_UNKNOWN) && (cfg.model >= 0x01000000UL)) {
        p_info = Flash_FindDeviceByModel((FLASH_MODEL)cfg.model);
    }
    if (p_info == NULL) {
        p_info = Flash_FindDevice(cfg.jedec_expect);
    }
    if (p_info == NULL) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* 6. Fail-safe 검증: 실제 부품이 선택과 일치해야 한다(정확히 또는 같은 제품군).
     *    틀리거나 모르는 ID 는 거절한다. */
    if (!Flash_FamilyMatch(jedec_id, cfg.jedec_expect)) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* 7. Optional SFC page size override from the settings block */
    if ((cfg.sfc_page_size == SFC_DEFAULT_PAGE_SIZE_2K) ||
        (cfg.sfc_page_size == SFC_DEFAULT_PAGE_SIZE_4K)) {
        g_flash_handle.sfc_page_size = cfg.sfc_page_size;
        Flash_SFC_Init(cfg.sfc_page_size);
    }

    /* 8. Configure the handle + every die (QE / 4-byte mode) */
    const FLASH_CRC_ALGO crc_algo = (cfg.crc_algo < (uint8_t)FLASH_CRC_ALGO_MAX)
                                    ? (FLASH_CRC_ALGO)cfg.crc_algo : FLASH_CRC32;
    status = Flash_ApplyHandleConfig(p_info, cfg.addr_mode, crc_algo);
    if (status != FLASH_OK) {
        return status;
    }

    /* 9. 설정 block 에서 동작 clock 을 정한다(SW-021).
     *
     *    위의 모든 단계는 10 MHz bring-up clock 으로 돌았다: probing, 이 block 의
     *    basic read, QE 와 4-byte-mode 변경까지. 이제 block 검증이 끝나고 handle
     *    설정도 마쳤으므로 bus 를 올려도 안전하다.
     *
     *    범위 밖 값은 error 가 아니다: 그 field 가 없다는 뜻이기 때문이다(예전
     *    24-byte block 은 그 자리가 0xFF 로 읽힌다). 이때 driver 는 실패할 이유가
     *    없는 부팅을 실패시키지 않고 안전한 10 MHz 를 유지한다. */
    const uint32_t spi_hz = cfg.spi_hz;
    if ((spi_hz >= FLASH_BOOT_SPI_HZ_MIN) && (spi_hz <= FLASH_BOOT_SPI_HZ_MAX)) {
        Flash_SPI_SetSpeed(spi_hz);
    }

    /* 10. Lock the selection (immutable for the rest of the boot) */
    g_flash_handle.locked = true;
    return FLASH_OK;
}

/* Convenience wrapper: default options (with software reset). */
int Flash_BootInit(uint32_t config_addr)
{
    return Flash_BootInitOpt(config_addr, FLASH_INIT_OPT_NONE);
}

/******************************************************************************
 * Public API: Flash_Init (auto-detect variant)
 ******************************************************************************/
int Flash_InitOpt(uint32_t options)
{
    /* 1. Bring the bus up and read the JEDEC ID */
    uint32_t jedec_id = 0U;
    int status = Flash_ProbeBus(options, &jedec_id);
    if (status != FLASH_OK) {
        return status;
    }

    /* 2. Look the ID up in the model table (exact match only) */
    const FLASH_INFO *p_info = Flash_FindDevice(jedec_id);
    if (p_info == NULL) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* 3. Configure the handle + every die, then go READY */
    status = Flash_ApplyHandleConfig(p_info, (uint8_t)FLASH_ADDR_MODE_DEFAULT, FLASH_CRC32);
    if (status != FLASH_OK) {
        return status;
    }

    /* 4. High-speed operational clock for normal use */
    Flash_SPI_SetSpeed(50000000U);
    return FLASH_OK;
}

/* Convenience wrapper: default options (with software reset). */
int Flash_Init(void)
{
    return Flash_InitOpt(FLASH_INIT_OPT_NONE);
}

/******************************************************************************
 * Public API: Flash_Reset
 ******************************************************************************/
int Flash_Reset(void)
{
    /* Send Reset Enable (66h) */
    Flash_SPI_SendCmd(F_CMD_RESET_ENABLE);
    Flash_DelayUs(50U);         /* gap between 66h/99h (SW-014: was 500 loops) */

    /* Send Reset Device (99h) */
    Flash_SPI_SendCmd(F_CMD_RESET_DEVICE);
    Flash_DelayUs(1000U);       /* reset recovery tRST (SW-014: was 20000 loops) */

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_ReadJEDECID
 ******************************************************************************/
int Flash_ReadJEDECID(uint32_t *p_jedec_id)
{
    uint8_t id_buf[3];

    if (p_jedec_id == NULL) {
        return FLASH_ERR_PARAM;
    }

    const int status = Flash_SpiReadFrame(F_CMD_RDID, 0U, 0U, NULL, id_buf, 3U);
    if (status != FLASH_OK) {
        return status;
    }

    *p_jedec_id = ((uint32_t)id_buf[0] << 16) |
                  ((uint32_t)id_buf[1] << 8)  |
                  ((uint32_t)id_buf[2]);

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_ReadStatus
 ******************************************************************************/
int Flash_ReadStatus(uint8_t reg_idx, uint8_t *p_val)
{
    uint8_t cmd;

    if (p_val == NULL) {
        return FLASH_ERR_PARAM;
    }

    switch (reg_idx) {
        case 1U:
            cmd = F_CMD_RDSR1;
            break;
        case 2U:
            cmd = F_CMD_RDSR2;
            break;
        case 3U:
            cmd = F_CMD_RDSR3;
            break;
        default:
            return FLASH_ERR_PARAM;
    }

    /* Status read: one-byte opcode in, one register value out, one CS frame. */
    return Flash_SpiReadFrame(cmd, 0U, 0U, NULL, p_val, 1U);
}

/******************************************************************************
 * Public API: Flash_WriteEnable
 ******************************************************************************/
int Flash_WriteEnable(void)
{
    /* WEL(S1)과 BUSY(S0)는 같은 05h 응답의 두 bit 이므로, status read 한 번으로
     * 둘을 함께 판단하고 선로에는 아무것도 더 보내지 않는다(SW-020).
     *
     * BUSY 까지 봐야 하는 이유:
     *   WEL 은 "latch 가 무장되었다"는 것만 말한다. program/erase 주기가 아직
     *   도는 중에 WREN 이 들어오면 WEL=1 과 WIP=1 이 동시에 선다. 이때 OK 를
     *   돌려주면 호출자의 다음 program command 는 부품이 버려 버리고, 뒤따르는
     *   WaitBusy 는 "이전" 주기가 끝났다는 것만 증명하므로, 실패는 조용히 묻히고
     *   호출자는 page 가 쓰였다고 믿는다.
     *
     * 재시도 예산은 Flash_WaitBusy() 와 같은 ~1 s 다: page program 은 0.4-3 ms,
     * WRSR 은 15-25 ms, sector erase 는 보통 45 ms 이므로, 5 x 10 us 짜리 짧은
     * loop 로는 실제 주기를 절대 흡수할 수 없다. */
    for (uint32_t retry = 0U; retry < 200000U; retry++) {
        Flash_SPI_SendCmd(F_CMD_WREN);

        uint8_t sr1 = 0U;
        const int status = Flash_ReadStatus(1U, &sr1);
        if (status != FLASH_OK) {
            return status;      /* dead bus: no point burning the whole budget */
        }
        if (((sr1 & FLASH_SR1_WEL) != 0U) && ((sr1 & FLASH_SR1_BUSY) == 0U)) {
            return FLASH_OK;
        }
        Flash_DelayUs(5U);      /* same 5 us backoff as Flash_WaitBusy */
    }

    return FLASH_ERR_WRITE_FAIL;
}

/******************************************************************************
 * Public API: Flash_WriteEnableVolatile (50h)
 *
 * Datasheet rule (Winbond W25Q / GigaDevice GD25LQ 계열):
 *   "The Write Enable for Volatile Status Register command will not set the
 *    Write Enable Latch (WEL) bit, it is only valid for the Write Status
 *    Register command to change the volatile Status Register bit values."
 *   -> 50h 는 바로 뒤따르는 WRSR 만 무장시킨다. 중간에 다른 command(status
 *      poll 포함)가 끼면 무장이 풀린다. 그래서 이 함수는 일부러 WEL 을
 *      poll 하지 않는다 — poll 하면 실패하고(WEL 은 0 그대로), 그 poll 자체가
 *      volatile enable 을 소모한다(SW-013 WEL fix).
 ******************************************************************************/
int Flash_WriteEnableVolatile(void)
{
    Flash_SPI_SendCmd(F_CMD_VSR_WREN);
    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_WriteDisable
 ******************************************************************************/
int Flash_WriteDisable(void)
{
    Flash_SPI_SendCmd(F_CMD_WRDI);
    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_WaitBusy
 ******************************************************************************/
int Flash_WaitBusy(uint32_t timeout_loops)
{
    for (uint32_t i = 0U; i < timeout_loops; i++) {
        uint8_t sr1 = 0U;
        if (Flash_ReadStatus(1U, &sr1) == FLASH_OK) {
            if ((sr1 & FLASH_SR1_BUSY) == 0U) {
                return FLASH_OK;
            }
        }
        Flash_DelayUs(5U);      /* BUSY poll backoff (SW-014: was 50 loops) */
    }

    return FLASH_ERR_TIMEOUT;
}

/******************************************************************************
 * Status-register write-enable: 06h 또는 50h (SW-035)
 *
 * 부품은 status register 를 두 벌로 들고 있다: power-off 에서도 살아남는
 * non-volatile copy 와, power-up 때마다 그것에서 다시 적재되는 volatile copy.
 * 둘 다 01h 로 쓰고, 차이는 write 를 무장하는 command 뿐이다:
 *
 *   06h Write Enable           -> NON-VOLATILE copy 무장(영구)
 *   50h Write Enable Volatile  -> VOLATILE copy 만 무장
 *
 * 어느 쪽이 맞는지는 부품이 아니라 호출자가 정한다:
 *   - Block protect 는 power cycle 을 견뎌야 하므로 Flash_Protect() 는
 *     PERSISTENT(06h)를 요청한다. SW-035 이전에는 FLASH_FEAT_VSR_WREN 을
 *     광고하는 부품에서 이 경로가 50h 를 썼기 때문에, 보호 설정이 volatile
 *     copy 에만 써지고 다음 power-up 에서 조용히 사라졌다.
 *   - Quad enable 은 어차피 부팅마다 다시 적용하므로 VOLATILE(50h)로 두어,
 *     다음 reset 에서 또 쓸 값을 non-volatile program cycle 로 낭비하지 않는다.
 ******************************************************************************/
typedef enum
{
    FLASH_SR_WRITE_VOLATILE   = 0,  /* 50h when the part supports it */
    FLASH_SR_WRITE_PERSISTENT = 1   /* always 06h */
} FLASH_SR_WRITE_MODE;

static int Flash_SrWriteEnable(FLASH_SR_WRITE_MODE mode)
{
    const FLASH_INFO *info = g_flash_handle.info;
    const bool can_arm_volatile =
        (info != NULL) && ((info->features & FLASH_FEAT_VSR_WREN) != 0U);

    if ((mode == FLASH_SR_WRITE_VOLATILE) && can_arm_volatile) {
        return Flash_WriteEnableVolatile();
    }
    return Flash_WriteEnable();
}

/******************************************************************************
 * Status Register Write (SW-019)
 *
 * GD25LQ64E 에 전용 경로가 필요한 이유
 * -----------------------------------
 * 이 부품에는 31h(SR2 전용) command 가 없다. SR2 에 닿는 방법은 01h 뿐이고,
 * 그것은 고정 16-bit command 다: SR1 byte 하나, 그다음 SR2 byte 하나.
 * Datasheet 7.4: 8번째 data bit 뒤에 CS# 를 올리면 command 는 그래도 실행되어
 * SR1 은 써지지만, SPI mode 에서 QE 와 CMP 가 0 으로 CLEAR 된다.
 * 따라서 "SR1 만 쓰는" write 는 부품을 조용히 quad mode 밖으로 떨어뜨리는데,
 * handle 은 qe_enabled = true 를 그대로 들고 있어 이후 quad read 가 전부
 * garbage 를 돌려준다. 그래서 FLASH_FEAT_STATUS_2BYTE_WRITE 를 가진 부품에서는
 * 모든 status write 를 16-bit frame 전체로 보낸다: 바꾸지 않는 register 는 읽은
 * 값을 그대로 되돌려 쓰고, 두 byte 모두 write 후 검증한다.
 ******************************************************************************/

/* WRSR 이 실제로 바꿀 수 있는 bit.
 * SR1: S7..S2 (SRP0, BP4..BP0). S1(WEL)과 S0(WIP)는 read-only.
 * SR2: S14..S8 중 S10 제외. S15(SUS)와 S10 은 read-only. */
#define FLASH_SR1_WRITABLE_MASK  (0xFCU)
#define FLASH_SR2_WRITABLE_MASK  (0x7BU)

/**
 * @brief  16-bit WRSR(01h + SR1 + SR2) 하나를 보내고 두 byte 를 모두 검사한다.
 *         SR2 를 단독으로 쓸 수 없는 부품(GD25LQ64E) 전용.
 */
static int Flash_WriteStatusPair(uint8_t sr1, uint8_t sr2, FLASH_SR_WRITE_MODE mode)
{
    /* 무장 전에 먼저 idle 을 확인한다(SW-035). 이 경로는 예전에 아래 일반 경로와
     * 달리 곧바로 무장했는데, persistent 무장(06h)에서는 WIP=1 중에 도착한 WREN
     * 을 부품이 무시해 WEL 이 서지 않고 01h frame 이 버려진다. 그러면 busy timeout
     * 을 다 기다린 뒤 read-back 불일치로 나서야 실패가 드러난다. 05h poll 한 번이
     * 훨씬 싸다. */
    int status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    status = Flash_SrWriteEnable(mode);
    if (status != FLASH_OK) {
        return status;
    }

    const uint8_t frame[3] = { F_CMD_WRSR1, sr1, sr2 };  /* SR1 + SR2 in one frame */
    Flash_SpiRawFrame(frame, (uint32_t)sizeof(frame));

    status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    /* 두 byte 를 모두 되읽는다. frame 이 잘렸거나 write 가 거절되었거나 QE 가
     * 지워진 경우가, 나중에 나쁜 data 로 드러나는 대신 여기서 보고된다. */
    uint8_t rb1 = 0U;
    uint8_t rb2 = 0U;
    status = Flash_ReadStatus(1U, &rb1);
    if (status != FLASH_OK) {
        return status;
    }
    status = Flash_ReadStatus(2U, &rb2);
    if (status != FLASH_OK) {
        return status;
    }

    if (((rb1 ^ sr1) & FLASH_SR1_WRITABLE_MASK) != 0U) {
        return FLASH_ERR_WRITE_FAIL;
    }
    if (((rb2 ^ sr2) & FLASH_SR2_WRITABLE_MASK) != 0U) {
        return FLASH_ERR_WRITE_FAIL;
    }
    return FLASH_OK;
}

/**
 * @brief  status register 하나를 쓴다. durable/volatile 을 골라서 저장한다.
 *
 * @param  reg_idx 1 (SR1), 2 (SR2) 또는 3 (SR3)
 * @param  val     쓸 수 있는 bit 에 넣을 값. read-only bit 는 무시된다
 * @param  mode    FLASH_SR_WRITE_PERSISTENT - power-off 후에도 유지(block
 *                 protect), FLASH_SR_WRITE_VOLATILE - 부팅 때 다시 적재(QE)
 */
static int Flash_WriteStatusEx(uint8_t reg_idx, uint8_t val, FLASH_SR_WRITE_MODE mode)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* ---- GigaDevice GD25LQ64E: coupled 16-bit write --------------------- */
    if ((info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) != 0U) {
        if ((reg_idx != 1U) && (reg_idx != 2U)) {
            return FLASH_ERR_UNSUPPORTED;   /* this part has no 11h */
        }

        uint8_t sr1 = 0U;
        uint8_t sr2 = 0U;
        int status = Flash_ReadStatus(1U, &sr1);
        if (status != FLASH_OK) {
            return status;
        }
        status = Flash_ReadStatus(2U, &sr2);
        if (status != FLASH_OK) {
            return status;
        }

        /* 호출자가 요청한 byte 만 바꾼다. 나머지 byte 는 읽은 그대로 되돌려 쓰며,
         * 그것이 QE(와 CMP)를 살려 두는 방법이다. */
        if (reg_idx == 1U) {
            sr1 = val;
        }
        else {
            sr2 = val;
        }
        return Flash_WriteStatusPair(sr1, sr2, mode);
    }

    /* ---- Standard one-byte status write -------------------------------- */
    /* 먼저 idle 을 확인한다. 이유는 둘이다: WIP=1 중에 도착한 WRSR 은 부품이
     * 버리고, 50h 로 무장하는 부품에서는 중간 command 가 volatile enable 을
     * 소모한다. */
    int status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    status = Flash_SrWriteEnable(mode);
    if (status != FLASH_OK) {
        return status;
    }

    uint8_t cmd;
    if (reg_idx == 1U) {
        cmd = F_CMD_WRSR1;
    }
    else if (reg_idx == 2U) {
        cmd = F_CMD_WRSR2;
    }
    else if (reg_idx == 3U) {
        cmd = F_CMD_WRSR3;
    }
    else {
        return FLASH_ERR_PARAM;
    }

    const uint8_t frame[2] = { cmd, val };   /* opcode + value, one CS frame */
    Flash_SpiRawFrame(frame, (uint32_t)sizeof(frame));

    status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    /* 실제로 무엇이 들어갔는지 되읽는다(SW-020). 이 검사가 없으면 거절되거나
     * 잘린 WRSR 도 FLASH_OK 를 돌려주었다 — GD coupled 경로가 겪은 것과 같은
     * 종류의 조용한 실패다. 쓸 수 있는 bit 만 비교한다: SR1 의 WEL/WIP 과 SR2 의
     * SUS/reserved 는 read-only 라서, 비교하면 모든 write 가 불일치로 보인다.
     * reg 3(11h)은 지원 부품이 없어 writable mask 도 없다. */
    if (reg_idx != 3U) {
        uint8_t rb = 0U;
        status = Flash_ReadStatus(reg_idx, &rb);
        if (status != FLASH_OK) {
            return status;
        }
        const uint8_t mask = (reg_idx == 2U) ? FLASH_SR2_WRITABLE_MASK
                                             : FLASH_SR1_WRITABLE_MASK;
        if (((rb ^ val) & mask) != 0U) {
            return FLASH_ERR_WRITE_FAIL;
        }
    }

    return FLASH_OK;
}

/******************************************************************************
 * Public status write (API/tests 용으로 유지)
 *
 * 예전 동작을 그대로 유지한다: volatile copy 에 쓴다. 그 bit 들은 부팅마다 다시
 * 적용되므로, 영구 저장하면 status register 의 program 예산만 낭비한다. Block
 * protect 는 이 entry point 를 쓰지 않고 PERSISTENT 를 명시한다(SW-035).
 ******************************************************************************/
int Flash_WriteStatus(uint8_t reg_idx, uint8_t val)
{
    return Flash_WriteStatusEx(reg_idx, val, FLASH_SR_WRITE_VOLATILE);
}

/******************************************************************************
 * Helper (SW-017): SR1 의 block-protect 4 bit 를 한 곳에서 쓴다.
 *
 *   Protect(bp)   -> BP3..BP0 = bp 로 write
 *   Unprotect()   -> BP3..BP0 = 0 으로 write
 *
 * 같은 동작에 nibble 만 다르므로, 거의 같은 본문 두 개 대신 하나를 공유한다
 * (ROM 약 50 byte 절약). QE 와 나머지 SR1 bit 는 읽은 값 그대로 보존한다.
 *
 * 사용 bit: BP0=bit2, BP1=bit3, BP2=bit4, BP3=bit5 (flash.h 참조).
 ******************************************************************************/
static int Flash_WriteBlockProtect(uint8_t bp_mask)
{
    /* Refuse before touching the bus (SW-018): NO_CONNECT / ERROR / UNINIT */
    const int ready = Flash_CheckConnect();
    if (ready != FLASH_OK) {
        return ready;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    int status;
    uint8_t sr1 = 0U;

    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* snapshot 전에 부품을 먼저 안정시킨다(SW-035). SR1 을 읽어 고쳐 쓰는 사이에
     * 그 read 가 주기 중간에 걸리면 과도기 WIP bit 가 write 값에 섞여 들어가고,
     * 뒤따르는 06h 무장은 WIP=1 이라 무시된다. 그러면 부품이 멀쩡한데도 protect
     * 가 자기 read-back 에서 timeout 된다. */
    status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    status = Flash_ReadStatus(1U, &sr1);
    if (status != FLASH_OK) {
        return status;
    }

    /* BP0..BP3 은 SR1 의 bit 2..5 에 있다. 다섯 번째 block-protect bit 를 가진
     * 부품(GD25LQ64E: S6 = BP4)은 bp_mask 의 bit 4 를 추가로 받는다.
     * mask 는 FLASH_FEAT_BP4 가 있을 때만 넓힌다: Macronix 부품에서 SR1 bit 6 은
     * quad-enable bit 이므로 여기서 절대 지워서는 안 된다(SW-019). */
    uint8_t bp_field = (uint8_t)(FLASH_SR1_BP0 | FLASH_SR1_BP1 |
                                 FLASH_SR1_BP2 | FLASH_SR1_BP3);
    uint8_t bp_bits  = (uint8_t)((bp_mask & 0x0FU) << 2);    /* nibble -> bits 2..5 */

    if ((info->features & FLASH_FEAT_BP4) != 0U) {
        bp_field = (uint8_t)(bp_field | FLASH_SR1_BP4);
        if ((bp_mask & 0x10U) != 0U) {
            bp_bits = (uint8_t)(bp_bits | FLASH_SR1_BP4);    /* mask bit4 -> BP4 */
        }
    }

    const uint8_t new_sr1  = (uint8_t)((sr1 & (uint8_t)(~bp_field)) | (bp_bits & bp_field));

    if (new_sr1 == sr1) {
        return FLASH_OK;            /* already in the requested state */
    }

    /* PERSISTENT (SW-035): volatile status copy 에만 써진 block protect 는 다음
     * power-up 에서 아무 error 없이 사라진다. 보호는 설정이므로 non-volatile
     * copy 로 보낸다: 50h 대신 06h. */
    return Flash_WriteStatusEx(1U, new_sr1, FLASH_SR_WRITE_PERSISTENT);
}

/******************************************************************************
 * Public API: Flash_Unprotect (SW-013 / G2)
 ******************************************************************************/
int Flash_Unprotect(void)
{
    return Flash_WriteBlockProtect(0U);      /* clear every BP bit */
}

/******************************************************************************
 * Public API: Flash_Protect (SW-013 / G2)
 ******************************************************************************/
int Flash_Protect(uint8_t bp_mask)
{
    return Flash_WriteBlockProtect(bp_mask); /* apply the requested BP pattern */
}

/******************************************************************************
 * Public API: Flash_EnableQuad
 ******************************************************************************/
int Flash_EnableQuad(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    if (info->qe_reg == 0U) {
        g_flash_handle.qe_enabled = true;
        return FLASH_OK;
    }

    uint8_t qe_val = 0U;
    int status = Flash_ReadStatus(info->qe_reg, &qe_val);
    if (status != FLASH_OK) {
        return status;
    }

    const uint8_t qe_mask = (uint8_t)(1U << info->qe_bit);
    if ((qe_val & qe_mask) != 0U) {
        /* QE is already enabled */
        g_flash_handle.qe_enabled = true;
        return FLASH_OK;
    }

    /* Set QE bit and write back */
    qe_val |= qe_mask;
    status = Flash_WriteStatus(info->qe_reg, qe_val);
    if (status != FLASH_OK) {
        return status;
    }

    /* Read back to verify */
    status = Flash_ReadStatus(info->qe_reg, &qe_val);
    if (status != FLASH_OK) {
        return status;
    }

    if ((qe_val & qe_mask) == 0U) {
        return FLASH_ERR_WRITE_FAIL;
    }

    g_flash_handle.qe_enabled = true;
    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_GetAddrModeState (SW-042)
 *
 *   bring-up 중에 잡아 둔 SR3 값에서 address-mode bit 두 개를 해석한다. bit 는
 *   인자 하나당 하나씩 돌려주므로, 호출자는 register 배치를 몰라도 된다. 둘 중
 *   하나에 NULL 을 넘겨도 된다.
 *
 *   FLASH_ERR_UNSUPPORTED(0/0 이 아니라)를 돌려주는 것은 의도적이다: "이 부품에는
 *   ADS/ADP 가 없다"와 "이 부품은 3-Byte mode 다"가 호출자에게 같아 보이면 안 된다.
 ******************************************************************************/
int Flash_GetAddrModeState(uint8_t *p_ads, uint8_t *p_adp)
{
    const FLASH_INFO *info = g_flash_handle.info;

    if ((info == NULL) || (g_flash_handle.sr3_valid == false)) {
        return FLASH_ERR_UNSUPPORTED;
    }

    if (p_ads != NULL) {
        *p_ads = (uint8_t)(((g_flash_handle.sr3 & FLASH_SR3_ADS) != 0U) ? 1U : 0U);
    }
    if (p_adp != NULL) {
        *p_adp = (uint8_t)(((g_flash_handle.sr3 & FLASH_SR3_ADP) != 0U) ? 1U : 0U);
    }

    return FLASH_OK;
}

/******************************************************************************
 * Helpers: chunk 별 read 경로 (SW-013 / req 3 기능화)
 ******************************************************************************/
static int Flash_ReadChunkSFC(uint32_t die_addr, uint8_t *p_buf, uint32_t chunk_len,
                              uint32_t effective_unit, FLASH_READ_UNIT unit,
                              const FLASH_READ_CFG *read_cfg,
                              bool use_crc)
{
    /* SFC safe-read sequence: single mode -> wait ready -> target mode -> DMA */
    Flash_SFC_SetSingleMode();

    int status = Flash_WaitBusy(100000U);
    if (status != FLASH_OK) {
        return status;
    }

    Flash_SFC_SetReadConfig(read_cfg, g_flash_handle.addr_mode);

    /* SFC takes a unit count, not a byte count */
    const uint32_t chunk_count = chunk_len / effective_unit;
    uint32_t crc_val = 0U;
    return Flash_SFC_DMARead(die_addr, p_buf, chunk_count, unit, use_crc, &crc_val);
}

static int Flash_ReadChunkDirect(uint32_t die_addr, uint8_t *p_buf, uint32_t chunk_len,
                                 const FLASH_READ_CFG *read_cfg)
{
    /* Direct SPI path is single-width only (Dual/Quad require SFC). */
    if (read_cfg->io_width > 1U) {
        return FLASH_ERR_PARAM;
    }

    int status = Flash_WaitBusy(100000U);
    if (status != FLASH_OK) {
        return status;
    }

    /* SW-013 DUMMY(오너 확인 TCON 선로 순서)는 header 안에서 만들어진다:
     * CMD -> ADDR -> DUMMY_LAST (첫 dummy byte = M7-M0 mode byte slot)
     *      -> DUMMY_VAL -> ... -> DATA
     * SW-016: header 와 data 가 frame 하나로 나간다(tx_len + rx_len). */
    return Flash_SpiReadFrame(Flash_GetCmdRead(read_cfg, g_flash_handle.addr_mode),
                              die_addr,
                              Flash_CmdAddrBytes(g_flash_handle.addr_mode),
                              read_cfg, p_buf, chunk_len);
}

/******************************************************************************
 * Public API: Flash_Read
 ******************************************************************************/
int Flash_Read(uint32_t addr, void *buf, uint32_t count,
               FLASH_READ_MODE mode, FLASH_READ_UNIT unit,
               uint32_t options)
{
    /* Refuse before touching the bus (SW-018): NO_CONNECT / ERROR / UNINIT */
    int status = Flash_CheckConnect();
    if (status != FLASH_OK) {
        return status;
    }

    /* unit 해석 + NULL/count 검사 + overflow 방어 + Cortex-M0 정렬 검사가 모두
     * 공용 helper(SW-016)로 옮겨졌고, SPI 와 SFC 경로가 함께 쓴다. */
    uint32_t effective_unit = 0U;
    uint32_t total_bytes = 0U;
    status = Flash_XferResolve(unit, count, buf, addr, &effective_unit, &total_bytes);
    if (status != FLASH_OK) {
        return status;
    }

    if (mode >= FLASH_READ_MODE_MAX) {
        return FLASH_ERR_PARAM;
    }

    /* QPI(4-4-4)는 mode 목록에는 있지만 구현되지 않았다: opcode 와 address 를
     * data line 네 개로 보내야 하는데, SPI frame 도 TCON SFC 도 그것을 지원하지
     * 않는다. 이름으로 거절해 두어야 호출자가 틀린 opcode 로 read 를 수행하는
     * 대신 분명한 error 를 받는다. */
    if (mode == FLASH_READ_QPI) {
        return FLASH_ERR_UNSUPPORTED;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* SW-036: 범위를 벗어난 offset 은 칩이 거절하지 않는다 — wrap 되어 엉뚱한
     * sector 를 아무 error 없이 읽는다. 여기서 거절한다. */
    status = Flash_CheckRange(addr, total_bytes);
    if (status != FLASH_OK) {
        return status;
    }

    const FLASH_READ_CFG *read_cfg = &info->read_cfg[mode];
    uint8_t *p_buf = (uint8_t *)buf;
    uint32_t remaining = total_bytes;
    uint32_t current_addr = addr;

    const bool crc_start = ((options & FLASH_READ_OPT_CRC) != 0U);
    const bool crc_cont  = ((options & FLASH_READ_OPT_CRC_CONT) != 0U);
    const bool do_crc    = (crc_start || crc_cont);
    uint32_t crc_run = 0U;

    /* Start fresh or continue from the previous accumulated value */
    if (crc_start) {
        crc_run = Flash_CrcInit(g_flash_handle.crc_algo);
    }
    else if (crc_cont) {
        crc_run = g_flash_handle.last_crc;
    }

    /* Loop over transfer to handle Multi-Die boundary crossings if any */
    while (remaining > 0U) {
        uint8_t target_die = 0U;
        uint32_t die_addr = 0U;
        status = Flash_CheckDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK) {
            return status;
        }

        /* Calculate contiguous chunk size within active die */
        uint32_t chunk_len = remaining;
        if (info->die_size != 0U) {
            const uint32_t die_remaining = info->die_size - die_addr;
            if (chunk_len > die_remaining) {
                chunk_len = die_remaining;
            }
        }

        /* Execute the transfer via the appropriate per-chunk read path */
        if ((options & FLASH_READ_OPT_SFC) != 0U) {
            status = Flash_ReadChunkSFC(die_addr, p_buf, chunk_len, effective_unit,
                                        unit, read_cfg, false);
        }
        else {
            status = Flash_ReadChunkDirect(die_addr, p_buf, chunk_len, read_cfg);
        }
        if (status != FLASH_OK) {
            return status;
        }

        /* Fold this chunk's data into the running software CRC */
        if (do_crc) {
            crc_run = Flash_CrcUpdate(g_flash_handle.crc_algo, crc_run, p_buf, chunk_len);
        }

        current_addr += chunk_len;
        p_buf        += chunk_len;
        remaining    -= chunk_len;
    }

    /* Expose the accumulated CRC (running value; finalized by Flash_GetLastCRC) */
    if (do_crc) {
        g_flash_handle.last_crc = crc_run;
    }

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_Write (Page Program with 256-Byte Page Boundary Splitting)
 ******************************************************************************/
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode)
{
    /* Refuse before touching the bus (SW-018): NO_CONNECT / ERROR / UNINIT */
    const int ready = Flash_CheckConnect();
    if (ready != FLASH_OK) {
        return ready;
    }

    if ((buf == NULL) || (len == 0U)) {
        return FLASH_ERR_PARAM;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* 버스 트래픽이 발생하기 전에 쓰기 작업을 막아야 한다. 
     * 허용 ​​범위를 초과하는 주소를 지정한 프로그래밍 명령은 (주소 래핑 현상에 의해) 배열의 시작 부분에 적용되어 버리는데, 
     * 이것이 바로 부트로더가 파괴되는 원인이다.*/
    const int range_status = Flash_CheckRange(addr, len);
    if (range_status != FLASH_OK) {
        return range_status;
    }

    const uint8_t prog_cmd = Flash_GetCmdProgram(mode, g_flash_handle.addr_mode);
    const uint8_t *p_src = (const uint8_t *)buf;
    uint32_t remaining = len;
    uint32_t current_addr = addr;

    while (remaining > 0U) {
        uint8_t target_die = 0U;
        uint32_t die_addr = 0U;
        int status = Flash_CheckDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK) {
            return status;
        }

        /* Bytes left in the current 256-byte page (FLASH_PAGE_SIZE) */
        const uint32_t bytes_to_boundary = FLASH_PAGE_SIZE - (die_addr % FLASH_PAGE_SIZE);
        uint32_t chunk_len = (remaining > bytes_to_boundary) ? bytes_to_boundary : remaining;

        /* Also clamp to die boundary if multi-die */
        if (info->die_size != 0U) {
            const uint32_t die_remaining = info->die_size - die_addr;
            if (chunk_len > die_remaining) {
                chunk_len = die_remaining;
            }
        }

        /* 1. Wait until device is ready */
        status = Flash_WaitBusy(100000U);
        if (status != FLASH_OK) {
            return status;
        }

        /* 2. Issue Write Enable */
        status = Flash_WriteEnable();
        if (status != FLASH_OK) {
            return status;
        }

        /* 3. Send Program Command + Address + Data in ONE frame (SW-016) */
        status = Flash_SpiWriteFrame(prog_cmd, die_addr,
                                     Flash_CmdAddrBytes(g_flash_handle.addr_mode),
                                     p_src, chunk_len,
                                     (mode == FLASH_PROGRAM_QUAD));
        if (status != FLASH_OK) {
            return status;
        }

        /* 4. Wait for Program cycle completion */
        status = Flash_WaitBusy(200000U);
        if (status != FLASH_OK) {
            return status;
        }

        current_addr += chunk_len;
        p_src        += chunk_len;
        remaining    -= chunk_len;
    }

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_Erase
 ******************************************************************************/
int Flash_Erase(uint32_t addr, FLASH_ERASE_MODE mode)
{
    /* Refuse before touching the bus (SW-018): NO_CONNECT / ERROR / UNINIT */
    const int ready = Flash_CheckConnect();
    if (ready != FLASH_OK) {
        return ready;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* Full Chip Erase 처리.
     *
     * 단일 die 부품도 여기서는 "die 하나"이므로, 예전의 중복된 단일-die 분기를
     * 없앴다: 아래 loop 가 두 경우를 모두 덮고 ROM 이 약 70 byte 줄어든다
     * (SW-017). Flash_SelectDie() 는 단일 die 부품에서 no-op 이므로 그런 부품의
     * 선로 동작은 달라지지 않는다. */
    if (mode == FLASH_ERASE_CHIP) {
        /* table 은 언제나 die_count >= 1 을 채운다. ternary 는 망가진 행이 chip
         * erase 를 조용한 no-op 으로 바꾸지 않게 막는다. */
        const uint8_t dies = (info->die_count != 0U) ? info->die_count : 1U;

        for (uint8_t d = 0U; d < dies; d++) {
            int status = Flash_SelectDie(d);
            if (status != FLASH_OK) {
                return status;
            }

            /* Wait for any previous operation, then arm and issue the erase */
            status = Flash_WaitBusy(200000U);
            if (status != FLASH_OK) {
                return status;
            }

            status = Flash_WriteEnable();
            if (status != FLASH_OK) {
                return status;
            }

            Flash_SPI_SendCmd(F_CMD_CHIP_ERASE);

            /* Chip erase can take multiple seconds */
            status = Flash_WaitBusy(50000000U);
            if (status != FLASH_OK) {
                return status;
            }
        }
        return FLASH_OK;
    }

    /* Sector / Block Erase 처리.
     * SW-036: 대상이 부품 안에서 시작해야 한다. chip erase 는 addr 를 무시하고
     * 위에서 이미 처리했으므로, 여기는 sector/block 만 해당한다. */
    int status = Flash_CheckRange(addr, 1U);
    if (status != FLASH_OK) {
        return status;
    }

    uint8_t target_die = 0U;
    uint32_t die_addr = 0U;
    status = Flash_CheckDie(addr, &target_die, &die_addr);
    if (status != FLASH_OK) {
        return status;
    }

    /* SW-037: 여기서도 block-protect 를 더는 해제하지 않는다.
     * Erase 는 program 과 같은 규칙을 따른다: 호출자가 건 보호는 그대로 유효하고,
     * 보호된 sector 를 향한 erase 는 부품이 거절한다(이 driver 는 read-back 없이
     * FLASH_OK 를 보고하므로, 원한다면 먼저 Flash_Unprotect() 를 부를 것). */

    const uint8_t erase_cmd = Flash_GetCmdErase(mode, g_flash_handle.addr_mode);
    if (erase_cmd == 0x00U) {
        return FLASH_ERR_PARAM;
    }

    status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    status = Flash_WriteEnable();
    if (status != FLASH_OK) {
        return status;
    }

    /* opcode + address 를 frame 하나로(SW-016). erase 자체는 BUSY 로 poll 한다. */
    status = Flash_SpiWriteFrame(erase_cmd, die_addr,
                                 Flash_CmdAddrBytes(g_flash_handle.addr_mode),
                                 NULL, 0U, false);
    if (status != FLASH_OK) {
        return status;
    }

    return Flash_WaitBusy(2000000U);
}

/******************************************************************************
 * Getters
 ******************************************************************************/
FLASH_STATE Flash_GetState(void)
{
    return g_flash_handle.state;
}

const FLASH_INFO *Flash_GetInfo(void)
{
    return g_flash_handle.info;
}

uint32_t Flash_GetLastCRC(void)
{
    return Flash_CrcFinal(g_flash_handle.crc_algo, g_flash_handle.last_crc);
}

bool Flash_IsLocked(void)
{
    return g_flash_handle.locked;
}
