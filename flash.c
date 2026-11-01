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
 *   The opcode, address and dummy bytes are packed into one small header buffer
 *   and the whole operation is handed to Flash_SPI_Transact() as a single /CS
 *   frame (tx_len / data phase / rx_len), instead of poking SPI bytes one at a
 *   time from every call site.
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
 * Helper: raw command frame with an arbitrary opcode + payload (no address).
 ******************************************************************************/
static void Flash_SpiRawFrame(const uint8_t *bytes, uint32_t len)
{
    const FLASH_SPI_XFER xfer = { .tx_buf = bytes, .tx_len = len };
    Flash_SPI_Transact(&xfer);
}

/******************************************************************************
 * Helper: Resolve Multi-Die Address & Die Index (W25M512)
 * (Short delays now use Flash_DelayUs from flash_delay.c — SW-014)
 ******************************************************************************/
static int Flash_ResolveDie(uint32_t addr, uint8_t *p_die, uint32_t *p_die_addr)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U) {
        if (info->die_size == 0U) {
            return FLASH_ERR_PARAM;
        }

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

    return FLASH_OK;
}

/******************************************************************************
 * Helper: Resolve the active addressing mode (boot-config override or default)
 ******************************************************************************/
static FLASH_ADDR_MODE Flash_ResolveAddrMode(uint8_t cfg_mode, const FLASH_INFO *info)
{
    if (cfg_mode == (uint8_t)FLASH_ADDR_MODE_DEFAULT) {
        /* Default: derive from the device's native width (Method A for 4-Byte). */
        return (info->addr_bytes == 4U) ? FLASH_ADDR_4BYTE_OPCODE : FLASH_ADDR_3BYTE;
    }
    return (FLASH_ADDR_MODE)cfg_mode;
}

/******************************************************************************
 * Helper: Apply non-common config to the ACTIVE die (4-Byte mode + QE).
 *   Every die of a multi-die stack has its own status registers, so this must
 *   run once per die (SW-013 / G1).
 ******************************************************************************/
static int Flash_ApplyDieConfig(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* Enter-4-Byte mode (B7h) is only needed for Method B (FLASH_ADDR_4BYTE_ENTER). */
    if (Flash_CmdNeedsEnter4b(g_flash_handle.addr_mode)) {
        Flash_SPI_SendCmd(F_CMD_ENTER_4BYTE_ADDR);
        (void)Flash_WaitBusy(10000U);
    }

    /* Verify / enable Quad Mode on this die */
    if ((info->features & FLASH_FEAT_QUAD) != 0U) {
        return Flash_EnableQuad();
    }

    return FLASH_OK;
}

/******************************************************************************
 * Helper: Apply non-common init across ALL dies of the active device.
 ******************************************************************************/
static int Flash_ApplyProductInit(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    if (((info->features & FLASH_FEAT_MULTI_DIE) != 0U) && (info->die_count > 1U)) {
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
    const uint32_t n = (uint32_t)(sizeof(FLASH_BOOT_CONFIG) - sizeof(uint32_t));
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

    /* 03h + 3-byte address works on every supported device in low memory.
     * One frame: header out, data in. */
    return Flash_SpiReadFrame(F_CMD_READ, addr, 3U, NULL, (uint8_t *)buf, len);
}

/******************************************************************************
 * Helper (SW-018): data-path gate.
 *
 * Read / Write / Erase / Protect are only allowed while the driver is READY.
 *
 * Why "info != NULL" alone is not enough: if a device that used to answer goes
 * away, a re-init sets NO_CONNECT, but the handle still points at the model it
 * found earlier. Without this check the driver would happily shift command and
 * address bytes onto a dead bus and return whatever floats in (0xFF), i.e. a
 * silent garbage read instead of a clear error.
 *
 * The probe/bring-up helpers (Flash_BasicRead, Flash_Reset, status polling)
 * deliberately do NOT use this gate: they must work before the driver is READY.
 ******************************************************************************/
static int Flash_EnsureReady(void)
{
    /* Plain if-chain on purpose: a switch here made the compiler emit a
     * 12-byte jump table in .rodata, which is silly for four values. */
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
 * Helper (SW-017): common bring-up + identification sequence.
 *
 * Flash_BootInitOpt() and Flash_InitOpt() used to carry their own copy of this
 * sequence (SPI/SFC init -> optional reset -> JEDEC ID -> floating-bus check).
 * Keeping one copy saves roughly 150 bytes of ROM.
 *
 * On success *p_jedec_id holds the 24-bit ID and the driver state is untouched.
 * On failure the fail-safe state (ERROR / NO_CONNECT) is already set.
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
 * Helper (SW-017): fill the runtime handle for the chosen device and apply its
 * per-die configuration (4-byte mode + QE). Shared by both init paths.
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

    /* 5. Look up the selected product. The config's model field selects an exact
     *    variant when needed; otherwise fall back to the expected JEDEC ID. */
    const FLASH_INFO *p_info = NULL;
    if (cfg.model != (uint32_t)FLASH_MODEL_UNKNOWN) {
        p_info = Flash_FindDeviceByModel((FLASH_MODEL)cfg.model);
    }
    if (p_info == NULL) {
        p_info = Flash_FindDevice(cfg.jedec_expect);
    }
    if (p_info == NULL) {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* 6. Fail-safe validation: the actual device must match the selection
     *    (exact or same product family). A wrong / unknown ID is refused. */
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

    /* 9. Lock the selection (immutable for the rest of the boot) */
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
    /* WEL (S1) and BUSY (S0) are two bits of the SAME 05h response, so one
     * status read decides both and nothing extra goes on the wire (SW-020).
     *
     * Why BUSY has to be part of the test:
     *   WEL only says "the latch is armed". A WREN that lands while a
     *   program/erase cycle is still running leaves WEL=1 and WIP=1 set at
     *   the same time. Returning OK there makes the caller's next program
     *   command get dropped by the device - and the WaitBusy that follows it
     *   only proves the PREVIOUS cycle finished, so the failure is silent and
     *   the caller thinks the page was written.
     *
     * The retry budget is the same ~1 s as Flash_WaitBusy(): a page program
     * is 0.4-3 ms, WRSR 15-25 ms, a sector erase 45 ms typical, so a short
     * 5 x 10 us loop could never absorb a real cycle. */
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
 * Datasheet rule (Winbond W25Q / GigaDevice GD25LQ series):
 *   "The Write Enable for Volatile Status Register command will not set the
 *    Write Enable Latch (WEL) bit, it is only valid for the Write Status
 *    Register command to change the volatile Status Register bit values."
 *   -> 50h arms ONLY the immediately following WRSR; any intervening
 *      command (including a status poll) clears it. Therefore this function
 *      deliberately does NOT poll WEL — it would fail (WEL stays 0) and the
 *      poll itself would consume the volatile enable (SW-013 WEL fix).
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
 * Helper: status-register write-enable (50h when supported, else 06h)
 ******************************************************************************/
static int Flash_SrWriteEnable(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if ((info != NULL) && ((info->features & FLASH_FEAT_VSR_WREN) != 0U)) {
        return Flash_WriteEnableVolatile();
    }
    return Flash_WriteEnable();
}

/******************************************************************************
 * Status Register Write (SW-019)
 *
 * Why the GD25LQ64E needs its own path
 * ------------------------------------
 * The part has no 31h (SR2-only) command. 01h is the ONLY way to reach SR2,
 * and it is a FIXED 16-bit command: one SR1 byte, then one SR2 byte.
 * Datasheet 7.4: if CS# is driven high after the 8th data bit the command is
 * still executed, SR1 is written, but QE and CMP are CLEARED to 0 in SPI mode.
 * A "SR1 only" write therefore drops the device out of quad mode silently,
 * while the handle keeps qe_enabled = true and every later quad read returns
 * garbage. So on parts carrying FLASH_FEAT_STATUS_2BYTE_WRITE every status
 * write is issued as a full 16-bit frame: the register that is not being
 * changed is read and echoed untouched, and BOTH bytes are verified after.
 ******************************************************************************/

/* Bits a WRSR command can actually change.
 * SR1: S7..S2 (SRP0, BP4..BP0). S1 (WEL) and S0 (WIP) are read-only.
 * SR2: S14..S8 except S10. S15 (SUS) and S10 are read-only. */
#define FLASH_SR1_WRITABLE_MASK  (0xFCU)
#define FLASH_SR2_WRITABLE_MASK  (0x7BU)

/**
 * @brief  Send one 16-bit WRSR (01h + SR1 + SR2) and check both bytes.
 *         Reserved for parts that cannot write SR2 on its own (GD25LQ64E).
 */
static int Flash_WriteStatusPair(uint8_t sr1, uint8_t sr2)
{
    int status = Flash_SrWriteEnable();
    if (status != FLASH_OK) {
        return status;
    }

    const uint8_t frame[3] = { F_CMD_WRSR1, sr1, sr2 };  /* SR1 + SR2 in one frame */
    Flash_SpiRawFrame(frame, (uint32_t)sizeof(frame));

    status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    /* Read BOTH bytes back. A truncated frame, a rejected write or a cleared
     * QE bit is reported here instead of surfacing later as bad data. */
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

int Flash_WriteStatus(uint8_t reg_idx, uint8_t val)
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

        /* Swap only the byte the caller asked for; the other byte is echoed
         * exactly as read, which is what keeps QE (and CMP) alive. */
        if (reg_idx == 1U) {
            sr1 = val;
        }
        else {
            sr2 = val;
        }
        return Flash_WriteStatusPair(sr1, sr2);
    }

    /* ---- Standard one-byte status write -------------------------------- */
    /* Idle first. Two reasons: a WRSR that arrives while WIP=1 is dropped by
     * the device, and on parts that arm with 50h any intervening command
     * consumes the volatile enable. */
    int status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK) {
        return status;
    }

    status = Flash_SrWriteEnable();
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

    /* Read back what actually landed (SW-020). Without this a rejected or
     * truncated WRSR still returned FLASH_OK - the same class of silent
     * failure the coupled GD path had. Only writable bits are compared:
     * WEL/WIP on SR1 and SUS/reserved on SR2 are read-only and would make
     * every write look like a mismatch. reg 3 (11h) has no supported part,
     * so there is no writable mask for it. */
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
 * Helper (SW-017): write the 4 block-protect bits of SR1 in one place.
 *
 *   Protect(bp)   -> write BP3..BP0 = bp
 *   Unprotect()   -> write BP3..BP0 = 0
 *
 * Both are the same operation with a different nibble, so they share one body
 * instead of two near-identical ones (~50 bytes of ROM saved). QE and every
 * other SR1 bit are preserved exactly as read.
 *
 * Bits used: BP0=bit2, BP1=bit3, BP2=bit4, BP3=bit5 (see flash.h).
 ******************************************************************************/
static int Flash_WriteBlockProtect(uint8_t bp_mask)
{
    /* Refuse before touching the bus (SW-018): NO_CONNECT / ERROR / UNINIT */
    const int ready = Flash_EnsureReady();
    if (ready != FLASH_OK) {
        return ready;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    uint8_t sr1 = 0U;
    int status = Flash_ReadStatus(1U, &sr1);
    if (status != FLASH_OK) {
        return status;
    }

    /* BP0..BP3 live in SR1 bits 2..5. Parts that carry a fifth block-protect
     * bit (GD25LQ64E: S6 = BP4) additionally take bit 4 of bp_mask.
     * The mask is only widened when FLASH_FEAT_BP4 is present: on Macronix
     * parts bit 6 of SR1 is the quad-enable bit and must never be cleared
     * here. (SW-019) */
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

    return Flash_WriteStatus(1U, new_sr1);
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
 * Public API: Flash_SelectDie (W25M512)
 ******************************************************************************/
int Flash_SelectDie(uint8_t die_idx)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }
    if ((info->features & FLASH_FEAT_MULTI_DIE) == 0U) {
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
 * Helpers: per-chunk read paths (SW-013 / req 3 functionization)
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

    /* SW-013 DUMMY (owner-confirmed TCON wire order) is built into the header:
     * CMD -> ADDR -> DUMMY_LAST (FIRST dummy byte = M7-M0 mode byte slot)
     *      -> DUMMY_VAL -> ... -> DATA
     * SW-016: header and data travel in ONE frame (tx_len + rx_len). */
    return Flash_SpiReadFrame(Flash_CmdRead(read_cfg, g_flash_handle.addr_mode),
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
    int status = Flash_EnsureReady();
    if (status != FLASH_OK) {
        return status;
    }

    /* Unit resolution + NULL/count checks + overflow guard + Cortex-M0 alignment
     * now live in one shared helper (SW-016) used by both the SPI and SFC paths. */
    uint32_t effective_unit = 0U;
    uint32_t total_bytes = 0U;
    status = Flash_XferResolve(unit, count, buf, addr, &effective_unit, &total_bytes);
    if (status != FLASH_OK) {
        return status;
    }

    if (mode >= FLASH_READ_MODE_MAX) {
        return FLASH_ERR_PARAM;
    }

    /* QPI (4-4-4) is in the mode list but not implemented: the opcode and
     * address would have to travel on four data lines, which neither the SPI
     * frame nor the TCON SFC supports. Refuse it by name so a caller gets a
     * clear error instead of a read performed with the wrong opcode. */
    if (mode == FLASH_READ_QPI) {
        return FLASH_ERR_UNSUPPORTED;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
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
        status = Flash_ResolveDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK) {
            return status;
        }

        status = Flash_SelectDie(target_die);
        if (status != FLASH_OK) {
            return status;
        }

        /* Calculate contiguous chunk size within active die */
        uint32_t chunk_len = remaining;
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U) {
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
    const int ready = Flash_EnsureReady();
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

    const uint8_t prog_cmd = Flash_CmdProgram(mode, g_flash_handle.addr_mode);
    const uint8_t *p_src = (const uint8_t *)buf;
    uint32_t remaining = len;
    uint32_t current_addr = addr;

    while (remaining > 0U) {
        uint8_t target_die = 0U;
        uint32_t die_addr = 0U;
        int status = Flash_ResolveDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK) {
            return status;
        }

        status = Flash_SelectDie(target_die);
        if (status != FLASH_OK) {
            return status;
        }

        /* SW-013 / G2: clear block-protect on this die so the program cannot
         * silently fail on a protected region. */
        status = Flash_Unprotect();
        if (status != FLASH_OK) {
            return status;
        }

        /* Bytes left in the current 256-byte page (FLASH_PAGE_SIZE) */
        const uint32_t bytes_to_boundary = FLASH_PAGE_SIZE - (die_addr % FLASH_PAGE_SIZE);
        uint32_t chunk_len = (remaining > bytes_to_boundary) ? bytes_to_boundary : remaining;

        /* Also clamp to die boundary if multi-die */
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U) {
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
    const int ready = Flash_EnsureReady();
    if (ready != FLASH_OK) {
        return ready;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL) {
        return FLASH_ERR_PARAM;
    }

    /* Full Chip Erase handling.
     *
     * A single-die part is simply "one die" here, so the old duplicated
     * single-die branch is gone: the loop below covers both cases and saves
     * ~70 bytes of ROM (SW-017). Flash_SelectDie() is a no-op on a single-die
     * device, so nothing changes on the wire for those parts. */
    if (mode == FLASH_ERASE_CHIP) {
        const uint8_t dies = ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
                             ? info->die_count : 1U;

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

    /* Sector / Block Erase handling */
    uint8_t target_die = 0U;
    uint32_t die_addr = 0U;
    int status = Flash_ResolveDie(addr, &target_die, &die_addr);
    if (status != FLASH_OK) {
        return status;
    }

    status = Flash_SelectDie(target_die);
    if (status != FLASH_OK) {
        return status;
    }

    /* SW-013 / G2: clear block-protect on this die so the erase cannot fail silently. */
    status = Flash_Unprotect();
    if (status != FLASH_OK) {
        return status;
    }

    const uint8_t erase_cmd = Flash_CmdErase(mode, g_flash_handle.addr_mode);
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

    /* Opcode + address in one frame (SW-016); the erase itself is BUSY-polled. */
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
