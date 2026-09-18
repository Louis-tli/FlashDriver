/******************************************************************************
 * File Name   : flash.c
 * Description : Generic SPI NOR Flash Driver Core Implementation
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / C90 Compatible
 ******************************************************************************/

#include "flash.h"
#include "flash_table.h"
#include "flash_spi.h"
#include "flash_sfc.h"

/******************************************************************************
 * Global Runtime Handle Instance (Allocated in SRAM)
 ******************************************************************************/
FLASH_HANDLE g_flash_handle =
{
    NULL,
    FLASH_STATE_UNINIT,
    0U,
    0U,
    SFC_DEFAULT_PAGE_SIZE_4K
};

/******************************************************************************
 * Helper: Send Short Delay (Cortex-M0 loop)
 ******************************************************************************/
static void Flash_DelayLoops(uint32_t count)
{
    volatile uint32_t i;
    for (i = 0U; i < count; i++)
    {
        /* No-op loop */
    }
}

/******************************************************************************
 * Helper: Resolve Multi-Die Address & Die Index (W25M512)
 ******************************************************************************/
static int Flash_ResolveDie(uint32_t addr, uint8_t *p_die, uint32_t *p_die_addr)
{
    const FLASH_INFO *info;
    uint8_t target_die;

    info = g_flash_handle.info;
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

        target_die = (uint8_t)(addr / info->die_size);
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
 * Public API: Flash_Init
 ******************************************************************************/
int Flash_Init(void)
{
    uint32_t jedec_id;
    int status;
    const FLASH_INFO *p_info;

    /* 1. Initialize low-level SPI controller hardware */
    Flash_SPI_Init();

    /* 2. Configure safe SPI clock (e.g., 10 MHz) for device probing */
    Flash_SPI_SetSpeed(10000000U);

    /* 3. Initialize SFC IP with default 4KB transfer page size */
    Flash_SFC_Init(SFC_DEFAULT_PAGE_SIZE_4K);
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;

    /* 4. Issue Software Reset to clear any prior bus states */
    (void)Flash_Reset();
    Flash_DelayLoops(50000U);

    /* 5. Read JEDEC Identification (9Fh) */
    status = Flash_ReadJEDECID(&jedec_id);
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
    p_info = Flash_FindDevice(jedec_id);
    if (p_info == NULL)
    {
        g_flash_handle.state = FLASH_STATE_ERROR;
        return FLASH_ERR_UNSUPPORTED;
    }

    /* Populate runtime handle */
    g_flash_handle.info         = p_info;
    g_flash_handle.current_die  = 0U;
    g_flash_handle.qe_enabled   = 0U;

    /* 7. Configure 4-Byte Address Mode if required by Flash density (>= 256Mb) */
    if (p_info->addr_bytes == 4U)
    {
        Flash_SPI_SendCmd(CMD_ENTER_4BYTE_ADDR);
        (void)Flash_WaitBusy(10000U);
    }

    /* 8. Verify and Enable Quad Mode (QE bit) if supported */
    if ((p_info->features & FLASH_FEAT_QUAD) != 0U)
    {
        status = Flash_EnableQuad();
        if (status != FLASH_OK)
        {
            g_flash_handle.state = FLASH_STATE_ERROR;
            return status;
        }
    }

    /* 9. Set high-speed operational clock (e.g. 50MHz for standard operations) */
    Flash_SPI_SetSpeed(50000000U);

    g_flash_handle.state = FLASH_STATE_READY;
    return FLASH_OK;
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
    uint8_t sr1;
    uint32_t retry;

    for (retry = 0U; retry < 5U; retry++)
    {
        Flash_SPI_SendCmd(CMD_WREN);

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
    uint8_t sr1;
    uint32_t i;

    for (i = 0U; i < timeout_loops; i++)
    {
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
    const FLASH_INFO *info;
    uint8_t sr1;
    int status;

    info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    /* GigaDevice (GD25LQ64E) uses 2-byte WRSR (01h) writing SR1 and SR2 simultaneously */
    if (((info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) != 0U) && (reg_idx == 2U))
    {
        status = Flash_ReadStatus(1U, &sr1);
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
    status = Flash_WriteEnable();
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
 * Public API: Flash_EnableQuad
 ******************************************************************************/
int Flash_EnableQuad(void)
{
    const FLASH_INFO *info;
    uint8_t qe_val;
    uint8_t qe_mask;
    int status;

    info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    if (info->qe_reg == 0U)
    {
        g_flash_handle.qe_enabled = 1U;
        return FLASH_OK;
    }

    status = Flash_ReadStatus(info->qe_reg, &qe_val);
    if (status != FLASH_OK)
    {
        return status;
    }

    qe_mask = (uint8_t)(1U << info->qe_bit);
    if ((qe_val & qe_mask) != 0U)
    {
        /* QE is already enabled */
        g_flash_handle.qe_enabled = 1U;
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

    g_flash_handle.qe_enabled = 1U;
    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_SelectDie (W25M512)
 ******************************************************************************/
int Flash_SelectDie(uint8_t die_idx)
{
    const FLASH_INFO *info;

    info = g_flash_handle.info;
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
 * Public API: Flash_Read
 ******************************************************************************/
int Flash_Read(uint32_t addr, void *buf, uint32_t len,
               FLASH_READ_MODE mode, uint32_t options)
{
    const FLASH_INFO *info;
    const FLASH_READ_CFG *read_cfg;
    uint8_t *p_buf;
    uint32_t remaining;
    uint32_t current_addr;
    uint32_t chunk_len;
    uint8_t target_die;
    uint32_t die_addr;
    uint32_t crc_val;
    uint32_t i;
    int status;

    if ((buf == NULL) || (len == 0U) || (mode >= FLASH_READ_MODE_MAX))
    {
        return FLASH_ERR_PARAM;
    }

    info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    read_cfg = &info->read_cfg[mode];
    p_buf = (uint8_t *)buf;
    remaining = len;
    current_addr = addr;

    /* Loop over transfer to handle Multi-Die boundary crossings if any */
    while (remaining > 0U)
    {
        status = Flash_ResolveDie(current_addr, &target_die, &die_addr);
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
        chunk_len = remaining;
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
        {
            uint32_t die_remaining = info->die_size - die_addr;
            if (chunk_len > die_remaining)
            {
                chunk_len = die_remaining;
            }
        }

        /* Branch: TCON SFC Memory-Mapped DMA Read Path */
        if ((options & FLASH_READ_OPT_SFC) != 0U)
        {
            /* Step 1: Configure SFC to Single Mode */
            Flash_SFC_SetSingleMode();

            /* Step 2: Poll Flash Status via SPI until BUSY=0 */
            status = Flash_WaitBusy(100000U);
            if (status != FLASH_OK)
            {
                return status;
            }

            /* Step 3: Reconfigure SFC to target high-speed Read Mode */
            Flash_SFC_SetReadConfig(read_cfg, info->addr_bytes);

            /* Step 4: Execute Memory-Mapped DMA transfer */
            status = Flash_SFC_DMARead(die_addr, p_buf, chunk_len,
                                       ((options & FLASH_READ_OPT_CRC) != 0U) ? 1U : 0U,
                                       &crc_val);
            if (status != FLASH_OK)
            {
                return status;
            }
        }
        else
        {
            /* Branch: Direct SPI Read Path */
            status = Flash_WaitBusy(100000U);
            if (status != FLASH_OK)
            {
                return status;
            }

            Flash_SPI_CS_Low();
            (void)Flash_SPI_TransferByte(read_cfg->command);
            Flash_SPI_SendAddr(die_addr, info->addr_bytes);

            if (read_cfg->mode_byte != 0x00U)
            {
                (void)Flash_SPI_TransferByte(read_cfg->mode_byte);
            }

            for (i = 0U; i < read_cfg->dummy_bytes; i++)
            {
                (void)Flash_SPI_TransferByte(read_cfg->dummy_value);
            }

            Flash_SPI_ReadData(p_buf, chunk_len);
            Flash_SPI_CS_High();
        }

        current_addr += chunk_len;
        p_buf        += chunk_len;
        remaining    -= chunk_len;
    }

    return FLASH_OK;
}

/******************************************************************************
 * Public API: Flash_Write (Page Program with 256-Byte Page Boundary Splitting)
 ******************************************************************************/
int Flash_Write(uint32_t addr, const void *buf, uint32_t len,
                FLASH_PROGRAM_MODE mode)
{
    const FLASH_INFO *info;
    const uint8_t *p_src;
    uint32_t remaining;
    uint32_t current_addr;
    uint32_t chunk_len;
    uint32_t bytes_to_boundary;
    uint8_t target_die;
    uint32_t die_addr;
    uint8_t prog_cmd;
    int status;

    if ((buf == NULL) || (len == 0U))
    {
        return FLASH_ERR_PARAM;
    }

    info = g_flash_handle.info;
    if (info == NULL)
    {
        return FLASH_ERR_PARAM;
    }

    prog_cmd = (mode == FLASH_PROGRAM_QUAD) ? CMD_QUAD_PAGE_PROGRAM : CMD_PAGE_PROGRAM;
    p_src = (const uint8_t *)buf;
    remaining = len;
    current_addr = addr;

    while (remaining > 0U)
    {
        status = Flash_ResolveDie(current_addr, &target_die, &die_addr);
        if (status != FLASH_OK)
        {
            return status;
        }

        status = Flash_SelectDie(target_die);
        if (status != FLASH_OK)
        {
            return status;
        }

        /* Compute remaining capacity in current 256-byte Flash page */
        bytes_to_boundary = info->page_size - (die_addr % info->page_size);
        chunk_len = (remaining > bytes_to_boundary) ? bytes_to_boundary : remaining;

        /* Also clamp to die boundary if multi-die */
        if ((info->features & FLASH_FEAT_MULTI_DIE) != 0U)
        {
            uint32_t die_remaining = info->die_size - die_addr;
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
        Flash_SPI_SendAddr(die_addr, info->addr_bytes);

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
    const FLASH_INFO *info;
    uint8_t erase_cmd;
    uint8_t target_die;
    uint32_t die_addr;
    uint8_t d;
    int status;

    info = g_flash_handle.info;
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
            for (d = 0U; d < info->die_count; d++)
            {
                status = Flash_SelectDie(d);
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
            return Flash_WaitBusy(50000000U);
        }
    }

    /* Sector / Block Erase handling */
    status = Flash_ResolveDie(addr, &target_die, &die_addr);
    if (status != FLASH_OK)
    {
        return status;
    }

    status = Flash_SelectDie(target_die);
    if (status != FLASH_OK)
    {
        return status;
    }

    switch (mode)
    {
        case FLASH_ERASE_SECTOR:
            erase_cmd = CMD_SECTOR_ERASE;       /* 20h */
            break;
        case FLASH_ERASE_BLOCK_32K:
            erase_cmd = CMD_BLOCK_ERASE_32K;    /* 52h */
            break;
        case FLASH_ERASE_BLOCK_64K:
            erase_cmd = CMD_BLOCK_ERASE_64K;    /* D8h */
            break;
        default:
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
    Flash_SPI_SendAddr(die_addr, info->addr_bytes);
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
