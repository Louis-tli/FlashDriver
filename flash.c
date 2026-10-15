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
 * Helper: Send Short Delay (Cortex-M0 loop)
 ******************************************************************************/
static void Flash_DelayLoops(uint32_t count)
{
    for (volatile uint32_t i = 0U; i < count; i++)
    {
        /* No-op loop */
    }
}

/******************************************************************************
 * Helper: Resolve Multi-Die Address & Die Index (W25M512)
 ******************************************************************************/
static int Flash_ResolveDie(uint32_t addr, uint8_t *p_die, uint32_t *p_die_addr)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
    {
        if (info->die_size == 0U)
        {
            return FLASH_ERR_PARAM;
        }

        const uint8_t target_die = (uint8_t)(addr / info->die_size);
        if (target_die >= info->die_count)
        {
            return FLASH_ERR_PARAM;
        }

        *p_die = target_die;
        *p_die_addr = addr % info->die_size;
    }
    else
    {
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
    if (cfg_mode == (uint8_t)FLASH_ADDR_MODE_DEFAULT)
    {
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
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    /* Enter-4-Byte mode (B7h) is only needed for Method B (FLASH_ADDR_4BYTE_ENTER). */
    if (Flash_CmdNeedsEnter4b(g_flash_handle.addr_mode))
    {
        Flash_SPI_SendCmd(CMD_ENTER_4BYTE_ADDR);
        (void)Flash_WaitBusy(10000U);
    }

    /* Verify / enable Quad Mode on this die */
    if ((info->features & FLASH_FEAT_QUAD) != 0U)
    {
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
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    if (((info->features & FLASH_FEAT_MULTI_DIE) != 0U) && (info->die_count > 1U))
    {
        for (uint8_t d = 0U; d < info->die_count; d++)
        {
            int status = Flash_SelectDie(d);
            if (status != FLASH_OK)
            {
                return status;
            }
            status = Flash_ApplyDieConfig();
            if (status != FLASH_OK)
            {
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
    if (cfg == NULL)
    {
        return 0U;
    }

    const uint8_t *p = (const uint8_t *)cfg;
    const uint32_t n = (uint32_t)(sizeof(FLASH_BOOT_CONFIG) - sizeof(uint32_t));
    uint32_t sum = 0x5A5A5A5AU;
    for (uint32_t i = 0U; i < n; i++)
    {
        sum += (uint32_t)p[i];
    }
    return sum;
}

/******************************************************************************
 * Public API: Flash_BasicRead (safe single-line 03h read, pre-configuration)
 ******************************************************************************/
int Flash_BasicRead(uint32_t addr, void *buf, uint32_t len)
{
    if ((buf == NULL) || (len == 0U))
    {
        return FLASH_ERR_PARAM;
    }

    /* 03h + 3-byte address works on every supported device in low memory. */
    Flash_SPI_CS_Low();
    (void)Flash_SPI_TransferByte(CMD_READ);
    Flash_SPI_SendAddr(addr, 3U);
    Flash_SPI_ReadData((uint8_t *)buf, len);
    Flash_SPI_CS_High();

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_BootInit (ID check -> settings -> configure -> lock)
 ******************************************************************************/
int Flash_BootInitOpt(uint32_t config_addr, uint32_t options)
{
    /* Once the selection is locked it is immutable for the rest of the boot. */
    if (g_flash_handle.locked)
    {
        return FLASH_ERR_BUSY;
    }

    /* 1. Initialize SPI controller (safe) + SFC with default page size */
    Flash_SPI_Init();
    Flash_SPI_SetSpeed(10000000U);
    Flash_SFC_Init(SFC_DEFAULT_PAGE_SIZE_4K);
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;

    /* 2. Software Reset to clear any stale bus state (skippable for fast boot) */
    if ((options & FLASH_INIT_OPT_NO_RESET) == 0U)
    {
        (void)Flash_Reset();
        Flash_DelayLoops(50000U);
    }

    /* 3. Read JEDEC ID and confirm the device responds */
    uint32_t jedec_id = 0U;
    int status = Flash_ReadJEDECID(&jedec_id);
    if (status != FLASH_OK)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }
    jedec_id &= FLASH_JEDEC_ID_MASK;
    if ((jedec_id == 0x000000U) || (jedec_id == 0xFFFFFFU))
    {
        g_flash_handle.state = FLASH_STATE_NO_CONNECT;
        return FLASH_ERR_NO_CONNECT;
    }

    /* 4. Basic-read the boot settings block from Flash */
    FLASH_BOOT_CONFIG cfg;
    status = Flash_BasicRead(config_addr, &cfg, (uint32_t)sizeof(cfg));
    if (status != FLASH_OK)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    /* 5. Validate settings block (magic + checksum) */
    if ((cfg.magic != FLASH_BOOT_CFG_MAGIC) ||
        (cfg.crc != Flash_ConfigCrc(&cfg)))
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_PARAM;
    }

    /* 6. Look up the selected product. The config's model field selects an exact
     *    variant (e.g. GD25LQ64E_PLUTO_SFC when it shares a JEDEC ID); otherwise
     *    fall back to matching by the expected JEDEC ID. */
    const FLASH_INFO *p_info = NULL;
    if (cfg.model != (uint32_t)FLASH_MODEL_UNKNOWN)
    {
        p_info = Flash_FindDeviceByModel((FLASH_MODEL)cfg.model);
    }
    if (p_info == NULL)
    {
        p_info = Flash_FindDevice(cfg.jedec_expect);
    }
    if (p_info == NULL)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* 7. Fail-safe validation: the actual device must match the selection
     *    (exact or same product family). A wrong / unknown ID is refused. */
    if (!Flash_FamilyMatch(jedec_id, cfg.jedec_expect))
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* 8. Configure from the settings block + product descriptor */
    g_flash_handle.info         = p_info;
    g_flash_handle.addr_mode    = Flash_ResolveAddrMode(cfg.addr_mode, p_info);
    g_flash_handle.current_die  = 0U;
    g_flash_handle.qe_enabled   = false;
    g_flash_handle.last_crc     = 0U;
    if ((cfg.sfc_page_size == SFC_DEFAULT_PAGE_SIZE_2K) ||
        (cfg.sfc_page_size == SFC_DEFAULT_PAGE_SIZE_4K))
    {
        g_flash_handle.sfc_page_size = cfg.sfc_page_size;
        Flash_SFC_Init(cfg.sfc_page_size);
    }

    /* 9. Apply non-common config to every die (QE + 4-Byte mode) */
    status = Flash_ApplyProductInit();
    if (status != FLASH_OK)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    /* 10. Lock the selection (immutable) and go READY */
    g_flash_handle.locked = true;
    g_flash_handle.state  = FLASH_STATE_READY;
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
    /* 1. Initialize low-level SPI controller hardware */
    Flash_SPI_Init();

    /* 2. Configure safe SPI clock (e.g., 10 MHz) for device probing */
    Flash_SPI_SetSpeed(10000000U);

    /* 3. Initialize SFC IP with default 4KB transfer page size */
    Flash_SFC_Init(SFC_DEFAULT_PAGE_SIZE_4K);
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;

    /* 4. Issue Software Reset to clear any prior bus states (skippable) */
    if ((options & FLASH_INIT_OPT_NO_RESET) == 0U)
    {
        (void)Flash_Reset();
        Flash_DelayLoops(50000U);
    }

    /* 5. Read JEDEC Identification (9Fh) */
    uint32_t jedec_id = 0U;
    int status = Flash_ReadJEDECID(&jedec_id);
    if (status != FLASH_OK)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    /* Check for floating/unconnected bus (all 0s or all 1s) */
    if ((jedec_id == 0x00000000U) || (jedec_id == 0x00FFFFFFU))
    {
        g_flash_handle.state = FLASH_STATE_NO_CONNECT;
        return FLASH_ERR_NO_CONNECT;
    }

    /* 6. Lookup device configuration from ROM table */
    const FLASH_INFO *p_info = Flash_FindDevice(jedec_id);
    if (p_info == NULL)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* Populate runtime handle */
    g_flash_handle.info         = p_info;
    g_flash_handle.addr_mode    = Flash_ResolveAddrMode((uint8_t)FLASH_ADDR_MODE_DEFAULT, p_info);
    g_flash_handle.current_die  = 0U;
    g_flash_handle.qe_enabled   = false;
    g_flash_handle.last_crc     = 0U;

    /* 7. Apply non-common config to every die (4-Byte mode + QE) */
    status = Flash_ApplyProductInit();
    if (status != FLASH_OK)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return status;
    }

    /* 8. Set high-speed operational clock (e.g. 50MHz for standard operations) */
    Flash_SPI_SetSpeed(50000000U);

    g_flash_handle.state = FLASH_STATE_READY;
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
    Flash_SPI_SendCmd(CMD_RESET_ENABLE);
    Flash_DelayLoops(500U);

    /* Send Reset Device (99h) */
    Flash_SPI_SendCmd(CMD_RESET_DEVICE);
    Flash_DelayLoops(20000U);

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_ReadJEDECID
 ******************************************************************************/
int Flash_ReadJEDECID(uint32_t *p_jedec_id)
{
    uint8_t id_buf[3];

    if (p_jedec_id == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    Flash_SPI_CS_Low();
    (void)Flash_SPI_TransferByte(CMD_RDID);
    Flash_SPI_ReadData(id_buf, 3U);
    Flash_SPI_CS_High();

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

    if (p_val == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    switch (reg_idx)
    {
        case 1U:
            cmd = CMD_RDSR1;
            break;
        case 2U:
            cmd = CMD_RDSR2;
            break;
        case 3U:
            cmd = CMD_RDSR3;
            break;
        default:
            return FLASH_ERR_PARAM;
    }

    Flash_SPI_CS_Low();
    (void)Flash_SPI_TransferByte(cmd);
    *p_val = Flash_SPI_TransferByte(0xFFU);
    Flash_SPI_CS_High();

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_WriteEnable
 ******************************************************************************/
int Flash_WriteEnable(void)
{
    for (uint32_t retry = 0U; retry < 5U; retry++)
    {
        Flash_SPI_SendCmd(CMD_WREN);

        uint8_t sr1 = 0U;
        if (Flash_ReadStatus(1U, &sr1) == FLASH_OK)
        {
            if ((sr1 & FLASH_SR1_WEL) != 0U)
            {
                return FLASH_OK;
            }
        }
        Flash_DelayLoops(100U);
    }

    return FLASH_ERR_WRITE_FAIL;
}

/******************************************************************************
 * Public API: Flash_WriteDisable
 ******************************************************************************/
int Flash_WriteDisable(void)
{
    Flash_SPI_SendCmd(CMD_WRDI);
    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_WaitBusy
 ******************************************************************************/
int Flash_WaitBusy(uint32_t timeout_loops)
{
    for (uint32_t i = 0U; i < timeout_loops; i++)
    {
        uint8_t sr1 = 0U;
        if (Flash_ReadStatus(1U, &sr1) == FLASH_OK)
        {
            if ((sr1 & FLASH_SR1_BUSY) == 0U)
            {
                return FLASH_OK;
            }
        }
        Flash_DelayLoops(50U);
    }

    return FLASH_ERR_TIMEOUT;
}

/******************************************************************************
 * Public API: Flash_WriteStatus
 ******************************************************************************/
int Flash_WriteStatus(uint8_t reg_idx, uint8_t val)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    /* GigaDevice (GD25LQ64E) uses 2-byte WRSR (01h) writing SR1 and SR2 simultaneously */
    if (((info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) != 0U) && (reg_idx == 2U))
    {
        uint8_t sr1 = 0U;
        int status = Flash_ReadStatus(1U, &sr1);
        if (status != FLASH_OK)
        {
            return status;
        }

        status = Flash_WriteEnable();
        if (status != FLASH_OK)
        {
            return status;
        }

        Flash_SPI_CS_Low();
        (void)Flash_SPI_TransferByte(CMD_WRSR1);
        (void)Flash_SPI_TransferByte(sr1);
        (void)Flash_SPI_TransferByte(val); /* SR2 */
        Flash_SPI_CS_High();

        return Flash_WaitBusy(200000U);
    }

    /* Standard Status Register Write */
    int status = Flash_WriteEnable();
    if (status != FLASH_OK)
    {
        return status;
    }

    Flash_SPI_CS_Low();
    if (reg_idx == 1U)
    {
        (void)Flash_SPI_TransferByte(CMD_WRSR1);
        (void)Flash_SPI_TransferByte(val);
    }
    else if (reg_idx == 2U)
    {
        (void)Flash_SPI_TransferByte(CMD_WRSR2);
        (void)Flash_SPI_TransferByte(val);
    }
    else if (reg_idx == 3U)
    {
        (void)Flash_SPI_TransferByte(CMD_WRSR3);
        (void)Flash_SPI_TransferByte(val);
    }
    else
    {
        Flash_SPI_CS_High();
        return FLASH_ERR_PARAM;
    }
    Flash_SPI_CS_High();

    return Flash_WaitBusy(200000U);
}

/******************************************************************************
 * Public API: Flash_Unprotect (SW-013 / G2)
 ******************************************************************************/
int Flash_Unprotect(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    uint8_t sr1 = 0U;
    int status = Flash_ReadStatus(1U, &sr1);
    if (status != FLASH_OK)
    {
        return status;
    }

    /* Clear BP0..BP3 (block protect), preserve QE and all other bits. */
    const uint8_t clear_mask = (uint8_t)(FLASH_SR1_BP0 | FLASH_SR1_BP1 |
                                         FLASH_SR1_BP2 | FLASH_SR1_BP3);
    const uint8_t new_sr1 = (uint8_t)(sr1 & (uint8_t)(~clear_mask));
    if (new_sr1 == sr1)
    {
        return FLASH_OK;   /* already unprotected */
    }

    return Flash_WriteStatus(1U, new_sr1);
}

/******************************************************************************
 * Public API: Flash_Protect (SW-013 / G2)
 ******************************************************************************/
int Flash_Protect(uint8_t bp_mask)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    uint8_t sr1 = 0U;
    int status = Flash_ReadStatus(1U, &sr1);
    if (status != FLASH_OK)
    {
        return status;
    }

    /* Replace BP0..BP3 with the requested 4-bit pattern, preserve the rest. */
    const uint8_t bp_mask_bits = (uint8_t)((bp_mask & 0x0FU) << 2);
    const uint8_t clear_mask   = (uint8_t)(FLASH_SR1_BP0 | FLASH_SR1_BP1 |
                                           FLASH_SR1_BP2 | FLASH_SR1_BP3);
    const uint8_t new_sr1 = (uint8_t)((sr1 & (uint8_t)(~clear_mask)) | bp_mask_bits);

    return Flash_WriteStatus(1U, new_sr1);
}

/******************************************************************************
 * Public API: Flash_EnableQuad
 ******************************************************************************/
int Flash_EnableQuad(void)
{
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    if (info->qe_reg == 0U)
    {
        g_flash_handle.qe_enabled = true;
        return FLASH_OK;
    }

    uint8_t qe_val = 0U;
    int status = Flash_ReadStatus(info->qe_reg, &qe_val);
    if (status != FLASH_OK)
    {
        return status;
    }

    const uint8_t qe_mask = (uint8_t)(1U << info->qe_bit);
    if ((qe_val & qe_mask) != 0U)
    {
        /* QE is already enabled */
        g_flash_handle.qe_enabled = true;
        return FLASH_OK;
    }

    /* Set QE bit and write back */
    qe_val |= qe_mask;
    status = Flash_WriteStatus(info->qe_reg, qe_val);
    if (status != FLASH_OK)
    {
        return status;
    }

    /* Read back to verify */
    status = Flash_ReadStatus(info->qe_reg, &qe_val);
    if (status != FLASH_OK)
    {
        return status;
    }

    if ((qe_val & qe_mask) == 0U)
    {
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
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }
    if ((info->features & FLASH_FEAT_MULTI_DIE) == 0U)
    {
        return FLASH_OK;
    }

    if (die_idx >= info->die_count)
    {
        return FLASH_ERR_PARAM;
    }

    if (g_flash_handle.current_die == die_idx)
    {
        return FLASH_OK;
    }

    /* Winbond Software Die Select (C2h + die_id) */
    Flash_SPI_CS_Low();
    (void)Flash_SPI_TransferByte(CMD_WINBOND_DIE_SELECT);
    (void)Flash_SPI_TransferByte(die_idx);
    Flash_SPI_CS_High();

    Flash_DelayLoops(500U);
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
    if (status != FLASH_OK)
    {
        return status;
    }

    Flash_SFC_SetReadConfig(read_cfg, g_flash_handle.addr_mode);

    const uint32_t chunk_count = chunk_len / effective_unit;
    uint32_t crc_val = 0U;
    return Flash_SFC_DMARead(die_addr, p_buf, chunk_count, unit, use_crc, &crc_val);
}

static int Flash_ReadChunkDirect(uint32_t die_addr, uint8_t *p_buf, uint32_t chunk_len,
                                 const FLASH_READ_CFG *read_cfg)
{
    /* Direct SPI path is single-width only (Dual/Quad require SFC). */
    if (read_cfg->io_width > 1U)
    {
        return FLASH_ERR_PARAM;
    }

    int status = Flash_WaitBusy(100000U);
    if (status != FLASH_OK)
    {
        return status;
    }

    Flash_SPI_CS_Low();
    (void)Flash_SPI_TransferByte(Flash_CmdRead(read_cfg, g_flash_handle.addr_mode));
    Flash_SPI_SendAddr(die_addr, Flash_CmdAddrBytes(g_flash_handle.addr_mode));

    if (read_cfg->mode_byte != 0x00U)
    {
        (void)Flash_SPI_TransferByte(read_cfg->mode_byte);
    }
    for (uint32_t i = 0U; i < read_cfg->dummy_bytes; i++)
    {
        (void)Flash_SPI_TransferByte(read_cfg->dummy_value);
    }

    Flash_SPI_ReadData(p_buf, chunk_len);
    Flash_SPI_CS_High();

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_Read
 ******************************************************************************/
int Flash_Read(uint32_t addr, void *buf, uint32_t count,
               FLASH_READ_MODE mode, FLASH_READ_UNIT unit,
               uint32_t options)
{
    /* Unit Resolution: default (0) or WORD (4) -> 4 bytes, HALFWORD (2) -> 2 bytes, BYTE (1) -> 1 byte */
    uint8_t effective_unit;
    if ((unit == FLASH_READ_UNIT_DEFAULT) || (unit == FLASH_READ_UNIT_WORD))
    {
        effective_unit = 4U;
    }
    else if (unit == FLASH_READ_UNIT_HALFWORD)
    {
        effective_unit = 2U;
    }
    else if (unit == FLASH_READ_UNIT_BYTE)
    {
        effective_unit = 1U;
    }
    else
    {
        return FLASH_ERR_PARAM;
    }

    if ((buf == NULL) || (count == 0U) || (mode >= FLASH_READ_MODE_MAX))
    {
        return FLASH_ERR_PARAM;
    }

    /* Cortex-M0 Alignment check based on effective unit */
    if (!Flash_IsAligned((uintptr_t)buf, (uint32_t)effective_unit) ||
        !Flash_IsAligned(addr, (uint32_t)effective_unit))
    {
        return FLASH_ERR_ALIGNMENT;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    const uint32_t total_bytes = count * (uint32_t)effective_unit;
    const FLASH_READ_CFG *read_cfg = &info->read_cfg[mode];
    uint8_t *p_buf = (uint8_t *)buf;
    uint32_t remaining = total_bytes;
    uint32_t current_addr = addr;

    const bool use_crc = ((options & FLASH_READ_OPT_CRC) != 0U);

    /* Reset the CRC engine once for the whole transfer (accumulate across chunks) */
    if (use_crc)
    {
        Flash_SFC_CrcReset();
        g_flash_handle.last_crc = 0U;
    }

    /* Loop over transfer to handle Multi-Die boundary crossings if any */
    while (remaining > 0U)
    {
        uint8_t target_die = 0U;
        uint32_t die_addr = 0U;
        int status = Flash_ResolveDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK)
        {
            return status;
        }

        status = Flash_SelectDie(target_die);
        if (status != FLASH_OK)
        {
            return status;
        }

        /* Calculate contiguous chunk size within active die */
        uint32_t chunk_len = remaining;
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
        {
            const uint32_t die_remaining = info->die_size - die_addr;
            if (chunk_len > die_remaining)
            {
                chunk_len = die_remaining;
            }
        }

        /* Execute the transfer via the appropriate per-chunk read path */
        if ((options & FLASH_READ_OPT_SFC) != 0U)
        {
            status = Flash_ReadChunkSFC(die_addr, p_buf, chunk_len, (uint32_t)effective_unit,
                                        unit, read_cfg, use_crc);
        }
        else
        {
            status = Flash_ReadChunkDirect(die_addr, p_buf, chunk_len, read_cfg);
        }
        if (status != FLASH_OK)
        {
            return status;
        }

        current_addr += chunk_len;
        p_buf        += chunk_len;
        remaining    -= chunk_len;
    }

    /* Expose the accumulated CRC (all chunks combined) */
    if (use_crc)
    {
        g_flash_handle.last_crc = Flash_SFC_CrcGet();
    }

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_Write (Page Program with 256-Byte Page Boundary Splitting)
 ******************************************************************************/
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode)
{
    if ((buf == NULL) || (len == 0U))
    {
        return FLASH_ERR_PARAM;
    }

    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    const uint8_t prog_cmd = Flash_CmdProgram(mode, g_flash_handle.addr_mode);
    const uint8_t *p_src = (const uint8_t *)buf;
    uint32_t remaining = len;
    uint32_t current_addr = addr;

    while (remaining > 0U)
    {
        uint8_t target_die = 0U;
        uint32_t die_addr = 0U;
        int status = Flash_ResolveDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK)
        {
            return status;
        }

        status = Flash_SelectDie(target_die);
        if (status != FLASH_OK)
        {
            return status;
        }

        /* SW-013 / G2: clear block-protect on this die so the program cannot
         * silently fail on a protected region. */
        status = Flash_Unprotect();
        if (status != FLASH_OK)
        {
            return status;
        }

        /* Compute remaining capacity in current 256-byte Flash page */
        const uint32_t bytes_to_boundary = info->page_size - (die_addr % info->page_size);
        uint32_t chunk_len = (remaining > bytes_to_boundary) ? bytes_to_boundary : remaining;

        /* Also clamp to die boundary if multi-die */
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
        {
            const uint32_t die_remaining = info->die_size - die_addr;
            if (chunk_len > die_remaining)
            {
                chunk_len = die_remaining;
            }
        }

        /* 1. Wait until device is ready */
        status = Flash_WaitBusy(100000U);
        if (status != FLASH_OK)
        {
            return status;
        }

        /* 2. Issue Write Enable */
        status = Flash_WriteEnable();
        if (status != FLASH_OK)
        {
            return status;
        }

        /* 3. Send Program Command + Address + Data */
        Flash_SPI_CS_Low();
        (void)Flash_SPI_TransferByte(prog_cmd);
        Flash_SPI_SendAddr(die_addr, Flash_CmdAddrBytes(g_flash_handle.addr_mode));

        if (mode == FLASH_PROGRAM_QUAD)
        {
            Flash_SPI_WriteQuadData(p_src, chunk_len);
        }
        else
        {
            Flash_SPI_WriteData(p_src, chunk_len);
        }
        Flash_SPI_CS_High();

        /* 4. Wait for Program cycle completion */
        status = Flash_WaitBusy(200000U);
        if (status != FLASH_OK)
        {
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
    const FLASH_INFO *info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    /* Full Chip Erase handling */
    if (mode == FLASH_ERASE_CHIP)
    {
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
        {
            /* Erase each die sequentially for W25M512 */
            for (uint8_t d = 0U; d < info->die_count; d++)
            {
                int status = Flash_SelectDie(d);
                if (status != FLASH_OK)
                {
                    return status;
                }

                status = Flash_WaitBusy(200000U);
                if (status != FLASH_OK)
                {
                    return status;
                }

                status = Flash_WriteEnable();
                if (status != FLASH_OK)
                {
                    return status;
                }

                Flash_SPI_SendCmd(CMD_CHIP_ERASE);

                /* Chip erase can take multiple seconds */
                status = Flash_WaitBusy(50000000U);
                if (status != FLASH_OK)
                {
                    return status;
                }
            }
            return FLASH_OK;
        }
        else
        {
            int status = Flash_WaitBusy(200000U);
            if (status != FLASH_OK)
            {
                return status;
            }

            status = Flash_WriteEnable();
            if (status != FLASH_OK)
            {
                return status;
            }

            Flash_SPI_SendCmd(CMD_CHIP_ERASE);
            return Flash_WaitBusy(50000000U);
        }
    }

    /* Sector / Block Erase handling */
    uint8_t target_die = 0U;
    uint32_t die_addr = 0U;
    int status = Flash_ResolveDie(addr, &target_die, &die_addr);
    if (status != FLASH_OK)
    {
        return status;
    }

    status = Flash_SelectDie(target_die);
    if (status != FLASH_OK)
    {
        return status;
    }

    /* SW-013 / G2: clear block-protect on this die so the erase cannot fail silently. */
    status = Flash_Unprotect();
    if (status != FLASH_OK)
    {
        return status;
    }

    const uint8_t erase_cmd = Flash_CmdErase(mode, g_flash_handle.addr_mode);
    if (erase_cmd == 0x00U)
    {
        return FLASH_ERR_PARAM;
    }

    status = Flash_WaitBusy(200000U);
    if (status != FLASH_OK)
    {
        return status;
    }

    status = Flash_WriteEnable();
    if (status != FLASH_OK)
    {
        return status;
    }

    Flash_SPI_CS_Low();
    (void)Flash_SPI_TransferByte(erase_cmd);
    Flash_SPI_SendAddr(die_addr, Flash_CmdAddrBytes(g_flash_handle.addr_mode));
    Flash_SPI_CS_High();

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
    return g_flash_handle.last_crc;
}

bool Flash_IsLocked(void)
{
    return g_flash_handle.locked;
}
