/******************************************************************************
 * File Name   : test_flash.c
 * Description : Verification Test Suite for Generic Flash Driver
 * Compiler    : ARMCC / ARMCLANG / GCC (C99 compliant)
 *
 * Covers the SW-013 boot model (ID -> basic-read settings -> configure -> lock),
 * product-family matching / fail-safe, per-die QE config, CRC accumulation and
 * the direct-SPI single-width guard, on top of the original 6 suites.
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>     /* offsetof — SW-021 addresses the crc field by offset */
#include "flash.h"
#include "flash_table.h"
#include "flash_spi.h"
#include "flash_sfc.h"
#include "flash_driver.h"

/******************************************************************************
 * Mock SPI State for Emulation & Testing (per-die status registers)
 ******************************************************************************/
typedef struct
{
    uint32_t mock_jedec_id;
    uint8_t  sr1[2], sr2[2], sr3[2];   /* per-die status registers */
    uint8_t  current_die;
    bool     entered_4byte[2];         /* per-die 4-Byte address mode flag */
    uint8_t  last_die_selected;
    uint32_t die_select_count;
    uint32_t page_program_count;
    uint32_t total_bytes_programmed;
    bool     in_cs;
    uint8_t  current_cmd;
    uint8_t  cmd_byte_index;

    /* Boot settings block served on F_CMD_READ (03h) */
    uint8_t  config_buf[128];
    uint32_t config_len;

    /* Reset (66h/99h) command counter for init-option testing */
    uint32_t reset_cmd_count;

    /* Write-enable variant counters: 06h (WREN) vs 50h (VSR_WREN) */
    uint32_t wren_count;
    uint32_t vsr_wren_count;

    /* SW-013 WEL fix: 50h 는 바로 다음 WRSR 만 무장하고 WEL 은 세우지 않는다
     * (Winbond W25Q / GigaDevice GD25LQ datasheet). 중간에 다른 command 가 오면
     * 대기 중인 volatile enable 이 풀린다. WRSR 은 WEL 이 서 있거나(06h) 50h 가
     * 대기 중일 때만 받아들여진다. */
    bool     vsr_wren_pending;
    bool     wrsr_accepted;

    /* SW-019: 마지막 01h(WRSR) frame 의 byte 수. SR2 를 단독으로 지정할 수 없는
     * 부품에서 driver 가 언제나 16-bit 형식(opcode + SR1 + SR2)을 보내는지 test 가
     * 증명할 수 있게 한다. */
    uint32_t wrsr1_frame_bytes;

    /* SW-043: 가장 최근 status-register write frame(01h/31h/11h)을 나간 그대로 보관한다.
     * test 가 어느 family 경로로 갔는지 증명할 수 있게 하려는 것이다. */
    uint8_t  last_wrsr_opcode;
    uint32_t last_wrsr_tx_len;

    /* SW-043: bus 위의 부품이 GigaDevice 3V(GD25Q) 계열이면 세운다. 이 계열은 01h
     * 를 data byte 하나로만 실행한다. 더 넓은 frame 은 아예 실행되지 않으므로
     * (datasheet 7.4) 여분 byte 가 SR2 에 닿아서는 안 된다 — read-back 이 잡아내야
     * 하는 실패가 바로 그것이다. */
    bool     wrsr_drop_extra_byte;

    /* SW-037: status register write frame(01h/31h/11h)이 bus 에 몇 번 나왔는지.
     * program 과 erase 는 이 수를 늘리면 안 된다 — test 는 그것으로 driver 가 더는
     * 칩을 스스로 unlock 하지 않는다는 것을 증명한다. */
    uint32_t wrsr_frame_count;

    /* SW-020: 앞으로 N 번의 status read 동안 부품은 self-timed 주기 중이다. 그
     * 동안 SR1 은 WIP(BUSY)=1 을 보고하고 WREN 은 버려지므로, WEL 과 BUSY 가 동시에
     * 서 있는 것이 정상이다 — WEL 만 보는 검사가 "준비됨"이라고 거짓말하는 바로 그
     * 구간이다. */
    uint32_t busy_reads_remaining;

    /* SW-020: 부품이 다음 WRSR 을 조용히 버린다(write 거절). 선로에는 아무 표시도
     * 없으므로 read-back 만이 알아챌 수 있다. */
    bool     drop_next_wrsr;

    /* SW-021: driver 가 마지막으로 요청한 SPI clock. boot settings block 이 실제로 bus
     * 속도를 몰아가는지 test 가 증명할 수 있게 하려는 것이다. */
    uint32_t spi_speed_hz;

    /* SW-035: 부품은 status register 를 두 벌로 들고 있다.
     *   sr1/sr2/sr3      = VOLATILE copy (05h/35h/15h 가 읽는 값)
     *   nv_sr1/nv_sr2/.. = NON-VOLATILE copy (power-off 후에도 유지)
     * 06h 로 무장한 01h frame 은 둘 다 쓰고, 50h 로 무장하면 datasheet 설명대로
     * volatile copy 만 쓴다. 이 model 이 없던 시절 mock 은 한 벌만 두었기 때문에,
     * 50h 로 쓴 block protect 가 test 에서는 영구인 것처럼 보이면서 실제 hardware
     * 에서는 사라졌다. */
    uint8_t  nv_sr1[2], nv_sr2[2], nv_sr3[2];
    bool     wrsr_volatile;         /* the 01h being executed is a volatile write */
} MOCK_SPI_STATE;

static MOCK_SPI_STATE s_mock;

/* SW-016: 가장 최근 Flash_SPI_Transact() frame 의 구성. command/address/data 가
 * buffer 하나의 transaction 으로 나간다는 것을 test 가 증명할 수 있게 한다. */
typedef struct
{
    uint32_t frames;            /* Flash_SPI_Transact() calls since reset       */
    uint32_t last_tx_len;       /* header (cmd + address + dummy) length       */
    uint32_t last_data_len;     /* 1-bit payload length                         */
    uint32_t last_quad_len;     /* 4-bit payload length                         */
    uint32_t last_rx_len;       /* read length                                  */
    uint8_t  last_first_tx;     /* first header byte (opcode)                   */
    uint32_t last_payload_len;  /* last non-zero data/quad payload length       */
    uint32_t last_payload_tx_len; /* header length of the frame carrying payload */
    uint32_t last_data_payload_len; /* last 1-bit payload length               */
    uint32_t last_quad_payload_len; /* last 4-bit payload length               */
} MOCK_XFER_LOG;

static MOCK_XFER_LOG s_xfer;

/* mock 을 깨끗한 상태로 되돌린다(die 두 개 모두). driver handle 도 함께 지워서
 * 각 test 가 새롭고 unlock 되지 않은 driver 에서 시작하게 한다. */
static void Mock_Reset(void)
{
    memset(&s_mock, 0, sizeof(s_mock));
    memset(&s_xfer, 0, sizeof(s_xfer));
    s_mock.last_die_selected = 0xFFU;

    g_flash_handle.info          = NULL;
    g_flash_handle.state         = FLASH_STATE_UNINIT;
    g_flash_handle.addr_mode     = FLASH_ADDR_MODE_DEFAULT;
    g_flash_handle.current_die   = 0U;
    g_flash_handle.qe_enabled    = false;
    g_flash_handle.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;
    g_flash_handle.locked        = false;
    g_flash_handle.last_crc      = 0U;
    g_flash_handle.sr3           = 0U;
    g_flash_handle.sr3_valid     = false;
}

/* Set the status registers for both dies (keeps old single-value call sites simple). */
static void Mock_SetStatus(uint8_t sr1, uint8_t sr2, uint8_t sr3)
{
    for (int d = 0; d < 2; d++) {
        s_mock.sr1[d] = sr1;
        s_mock.sr2[d] = sr2;
        s_mock.sr3[d] = sr3;
        s_mock.nv_sr1[d] = sr1;     /* SW-035: power-up image matches */
        s_mock.nv_sr2[d] = sr2;
        s_mock.nv_sr3[d] = sr3;
    }
    s_mock.wrsr_volatile = false;
}

/* SW-035: 전원을 끊었다 다시 넣는다. volatile status register 는 non-volatile
 * image 에서 다시 적재된다 — block protect 가 반드시 견뎌야 하는 바로 그 지점.
 * 나머지(memory 내용)는 실제 hardware 처럼 그대로 둔다. */
static void Mock_PowerCycle(void)
{
    for (int d = 0; d < 2; d++) {
        s_mock.sr1[d] = s_mock.nv_sr1[d];
        s_mock.sr2[d] = s_mock.nv_sr2[d];
        s_mock.sr3[d] = s_mock.nv_sr3[d];
    }
    s_mock.vsr_wren_pending     = false;
    s_mock.wrsr_accepted        = false;
    s_mock.wrsr_volatile        = false;
    s_mock.busy_reads_remaining = 0U;
    s_mock.drop_next_wrsr       = false;
    s_mock.current_die          = 0U;
    s_mock.cmd_byte_index       = 0U;
    g_flash_handle.state        = FLASH_STATE_NO_CONNECT;   /* firmware restarts too */
    g_flash_handle.info         = NULL;
    g_flash_handle.qe_enabled   = false;
    g_flash_handle.locked       = false;
}

/* Build a valid boot settings block and install it as the readable config. */
static void Mock_SetBootConfigHz(uint32_t jedec_expect, uint16_t sfc_page,
                                 uint32_t features, uint32_t spi_hz)
{
    FLASH_BOOT_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic        = FLASH_BOOT_CFG_MAGIC;
    cfg.jedec_expect = jedec_expect;
    cfg.model        = (uint32_t)FLASH_MODEL_UNKNOWN;   /* fall back to JEDEC lookup */
    cfg.features     = features;
    cfg.sfc_page_size = sfc_page;
    cfg.addr_mode    = (uint8_t)FLASH_ADDR_MODE_DEFAULT;
    cfg.spi_hz       = spi_hz;                          /* SW-021 */
    cfg.crc          = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);
}

/* SW-021 이전에 작성된 test 의 block 에는 clock 이 없다. 지워진 Flash 에서
 * 24-byte block 뒤에 오는 값이 0xFFFFFFFF 이므로, 이것이 "field 없음"의 정직한
 * 경우다: driver 는 그래도 부팅해야 하고, 10 MHz bring-up clock 을 유지해야 한다. */
static void Mock_SetBootConfig(uint32_t jedec_expect, uint16_t sfc_page, uint32_t features)
{
    Mock_SetBootConfigHz(jedec_expect, sfc_page, features, 0xFFFFFFFFU);
}

/* Emulation 부품이 고정 16-bit WRSR(01h) frame 을 쓰는가?
 * 특정 부품 번호를 hard-code 하지 않고 model table 에서 찾는다 — GigaDevice
 * 모든 용량(16Mb / 32Mb / 64Mb)이 silicon 과 같이 동작하게 하려는 것이다.
 * (GD25LQ16E/32E/64E 7.4: 8-bit write 는 S15-S8, 즉 QE + CMP 를 지운다.) */
static bool Mock_Uses16BitWrsr(void)
{
    const FLASH_INFO *info = Flash_FindDevice(s_mock.mock_jedec_id);

    return (info != NULL) &&
           ((info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) != 0U);
}

/* SW-043: SILICON 이 어떻게 동작하는지를 driver table 이 아니라 raw ID 로
 * 판단한다 — table 이 틀리면 자기 test 만 통과시켜 버리기 때문이다.
 * GigaDevice memory-type byte 0x60 = GD25LQ(1.8V, 16-bit 01h 필수),
 * 0x40 = GD25Q(3V, 01h/31h/11h frame 마다 register 하나). */
static bool Mock_PartRejectsWideWrsr(void)
{
    const uint32_t id = s_mock.mock_jedec_id & 0x00FFFFFFU;

    return ((id >> 16) == 0xC8U) && (((id >> 8) & 0xFFU) != 0x60U);
}

/* Override weak SPI functions for test verification */
void Flash_SPI_Init(void)
{
    /* Preserve mock data across driver-init; just clear transfer bookkeeping. */
    s_mock.in_cs = false;
    s_mock.current_cmd = 0U;
    s_mock.cmd_byte_index = 0U;
}

void Flash_SPI_SetSpeed(uint32_t speed_hz)
{
    s_mock.spi_speed_hz = speed_hz;     /* SW-021: recorded, not discarded */
}

void Flash_SPI_CS_Low(void)
{
    s_mock.in_cs = true;
    s_mock.cmd_byte_index = 0U;
}

void Flash_SPI_CS_High(void)
{
    s_mock.in_cs = false;
    s_mock.current_cmd = 0U;
    s_mock.cmd_byte_index = 0U;
}

uint8_t Flash_SPI_TransferByte(uint8_t tx_byte)
{
    uint8_t rx_byte = 0xFFU;

    if (s_mock.cmd_byte_index == 0U) {
        s_mock.current_cmd = tx_byte;
        s_mock.cmd_byte_index = 1U;

        /* SW-013 WEL fix: datasheet 의미를 흉내낸다 — WRSR 은 WEL(06h)이 서 있거나
         * 대기 중인 50h 가 있을 때만 받아들여지고, 그 외에는 대기 중인 50h 가 풀린다. */
        if ((tx_byte == F_CMD_WRSR1) || (tx_byte == F_CMD_WRSR2) || (tx_byte == F_CMD_WRSR3)) {
            s_mock.wrsr_accepted = (((s_mock.sr1[s_mock.current_die] & FLASH_SR1_WEL) != 0U) ||
                                    s_mock.vsr_wren_pending);
            /* SW-035: 이 frame 이 어느 copy 를 건드릴 수 있는지 기억한다. 50h 는 volatile
             * copy 만, 06h 는 non-volatile copy 를 무장한다. */
            s_mock.wrsr_volatile    = s_mock.vsr_wren_pending;
            s_mock.vsr_wren_pending = false;
        }
        else if (tx_byte != F_CMD_VSR_WREN) {
            s_mock.vsr_wren_pending = false;
        }

        if (tx_byte == F_CMD_WREN) {
            s_mock.wren_count++;
            /* SW-020: 실제 silicon 처럼, embedded 주기(WIP=1) 중에는 WREN 이 무시되므로 그때
             * WEL 이 서서는 안 된다. */
            if (s_mock.busy_reads_remaining == 0U) {
                s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
            }
        } else if (tx_byte == F_CMD_VSR_WREN) {
            s_mock.vsr_wren_pending = true;   /* WEL stays 0 (datasheet) */
            s_mock.vsr_wren_count++;
        }
        else if (tx_byte == F_CMD_WRDI) {
            s_mock.sr1[s_mock.current_die] &= (uint8_t)(~FLASH_SR1_WEL);
        }
        else if (tx_byte == F_CMD_ENTER_4BYTE_ADDR) {
            s_mock.entered_4byte[s_mock.current_die] = true;
        }
        else if ((tx_byte == F_CMD_RESET_ENABLE) || (tx_byte == F_CMD_RESET_DEVICE)) {
            s_mock.reset_cmd_count++;
        }
        return 0x00U;
    }

    switch (s_mock.current_cmd) {
        case F_CMD_RDID: /* 9Fh */
            if (s_mock.cmd_byte_index == 1U) {
                rx_byte = (uint8_t)((s_mock.mock_jedec_id >> 16) & 0xFFU);
            }
            else if (s_mock.cmd_byte_index == 2U) {
                rx_byte = (uint8_t)((s_mock.mock_jedec_id >> 8) & 0xFFU);
            }
            else if (s_mock.cmd_byte_index == 3U) {
                rx_byte = (uint8_t)(s_mock.mock_jedec_id & 0xFFU);
            }
            s_mock.cmd_byte_index++;
            break;

        case F_CMD_READ: /* 03h : serve the boot settings block after cmd + 3 addr */
            if (s_mock.cmd_byte_index >= 4U) {
                const uint32_t idx = s_mock.cmd_byte_index - 4U;
                rx_byte = (idx < s_mock.config_len) ? s_mock.config_buf[idx] : 0xFFU;
            }
            else {
                rx_byte = 0x00U; /* address phase */
            }
            s_mock.cmd_byte_index++;
            break;

        case F_CMD_RDSR1: /* 05h */
            rx_byte = s_mock.sr1[s_mock.current_die];
            if (s_mock.busy_reads_remaining > 0U) {   /* SW-020: WIP=1 window */
                s_mock.busy_reads_remaining--;
                rx_byte |= FLASH_SR1_BUSY;
            }
            break;
        case F_CMD_RDSR2: /* 35h */
            rx_byte = s_mock.sr2[s_mock.current_die];
            break;
        case F_CMD_RDSR3: /* 15h */
            rx_byte = s_mock.sr3[s_mock.current_die];
            break;

        case F_CMD_WRSR1: /* 01h : 1-byte (SR1) or 2-byte (SR1+SR2) */
            if (!s_mock.wrsr_accepted) {
                break;                      /* chip rejects: no WEL, no pending 50h */
            }
            if (s_mock.drop_next_wrsr) {    /* SW-020: write silently dropped */
                s_mock.drop_next_wrsr = false;
                s_mock.cmd_byte_index++;
                break;
            }
            if (s_mock.cmd_byte_index == 1U) {
                s_mock.sr1[s_mock.current_die] = tx_byte;
                if (!s_mock.wrsr_volatile) {
                    s_mock.nv_sr1[s_mock.current_die] = tx_byte;   /* SW-035 */
                }
                s_mock.cmd_byte_index++;
            }
            else if (s_mock.cmd_byte_index == 2U) {
                if (s_mock.wrsr_drop_extra_byte) {
                    /* SW-043: GigaDevice 3V 부품. 여기서 01h frame 은 data byte 하나 폭이므로 그
                     * command 는 실행되지 않는다 — SR2 는 예전 값을 유지하고 read-back 이 실패를
                     * 보고한다. */
                    s_mock.cmd_byte_index++;
                    break;
                }
                s_mock.sr2[s_mock.current_die] = tx_byte;
                if (!s_mock.wrsr_volatile) {
                    s_mock.nv_sr2[s_mock.current_die] = tx_byte;   /* SW-035 */
                }
                s_mock.cmd_byte_index++;
            }
            break;

        case F_CMD_WRSR2: /* 31h */
            if (!s_mock.wrsr_accepted) {
                break;                      /* chip rejects: no WEL, no pending 50h */
            }
            s_mock.sr2[s_mock.current_die] = tx_byte;
            if (!s_mock.wrsr_volatile) {
                s_mock.nv_sr2[s_mock.current_die] = tx_byte;       /* SW-035 */
            }
            break;

        case F_CMD_WREN: /* 06h */
            s_mock.sr1[s_mock.current_die] |= FLASH_SR1_WEL;
            break;

        case F_CMD_WRDI: /* 04h */
            s_mock.sr1[s_mock.current_die] &= (uint8_t)(~FLASH_SR1_WEL);
            break;

        case F_CMD_WINBOND_DIE_SELECT: /* C2h : next byte selects the die */
            s_mock.current_die = tx_byte;
            s_mock.last_die_selected = tx_byte;
            s_mock.die_select_count++;
            break;

        case F_CMD_PAGE_PROGRAM:       /* 02h */
        case F_CMD_QUAD_PAGE_PROGRAM:  /* 32h */
        case F_CMD_PAGE_PROGRAM_4B:    /* 12h */
        case F_CMD_QUAD_PAGE_PROGRAM_4B: /* 34h */
            if (s_mock.cmd_byte_index == 1U) {
                s_mock.page_program_count++;
            }
            if (s_mock.cmd_byte_index >= 4U) {
                s_mock.total_bytes_programmed++;
            }
            s_mock.cmd_byte_index++;
            break;

        default:
            rx_byte = 0xFFU;
            s_mock.cmd_byte_index++;
            break;
    }

    return rx_byte;
}

/* SW-016: 목표 BSP 의 buffered transfer 를 계측하도록 대신 세운 것. 요청된
 * phase 구성을 기록한 뒤, byte 단위 mock 으로 같은 byte 를 다시 흘려보내
 * emulation 부품이 같은 선로 순서를 보게 한다. */
void Flash_SPI_Transact(const FLASH_SPI_XFER *xfer)
{
    s_xfer.frames++;
    s_xfer.last_tx_len   = xfer->tx_len;
    s_xfer.last_data_len = xfer->data_len;
    s_xfer.last_quad_len = xfer->quad_len;
    s_xfer.last_rx_len   = xfer->rx_len;
    s_xfer.last_first_tx = ((xfer->tx_buf != NULL) && (xfer->tx_len > 0U))
                           ? xfer->tx_buf[0] : 0U;

    if (xfer->quad_len > 0U) {
        s_xfer.last_payload_len         = xfer->quad_len;
        s_xfer.last_payload_tx_len      = xfer->tx_len;
        s_xfer.last_quad_payload_len    = xfer->quad_len;
    }
    else if (xfer->data_len > 0U) {
        s_xfer.last_payload_len         = xfer->data_len;
        s_xfer.last_payload_tx_len      = xfer->tx_len;
        s_xfer.last_data_payload_len    = xfer->data_len;
    }

    if ((xfer->tx_buf != NULL) && (xfer->tx_len > 0U) &&
        ((xfer->tx_buf[0] == F_CMD_WRSR1) || (xfer->tx_buf[0] == F_CMD_WRSR2) ||
         (xfer->tx_buf[0] == F_CMD_WRSR3))) {
        s_mock.last_wrsr_opcode = xfer->tx_buf[0];
        s_mock.last_wrsr_tx_len = xfer->tx_len;
        s_mock.wrsr_drop_extra_byte = (xfer->tx_buf[0] == F_CMD_WRSR1) &&
                                      (xfer->tx_len > 2U) &&
                                      Mock_PartRejectsWideWrsr();
    }

    Flash_SPI_CS_Low();

    if (xfer->tx_buf != NULL) {
        for (uint32_t i = 0U; i < xfer->tx_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->tx_buf[i]);
        }
    }
    if (xfer->data_buf != NULL) {
        for (uint32_t i = 0U; i < xfer->data_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->data_buf[i]);
        }
    }
    if (xfer->quad_buf != NULL) {
        /* The emulated device has a single data line: quad length still counts. */
        for (uint32_t i = 0U; i < xfer->quad_len; i++) {
            (void)Flash_SPI_TransferByte(xfer->quad_buf[i]);
        }
    }
    if (xfer->rx_buf != NULL) {
        for (uint32_t i = 0U; i < xfer->rx_len; i++) {
            xfer->rx_buf[i] = Flash_SPI_TransferByte(0xFFU);
        }
    }

    /* ---------------------------------------------------------------------
     * SW-019 mock fidelity: GD25LQ64E 의 WRSR 은 고정 16-bit command 다.
     * Datasheet 7.4 - "If CS# is driven high after eighth bit of the data
     * byte, the command is not executed" 는 QSPI 의 경우이고, SPI mode 에서는
     * write 가 실행되며 "the QE and CMP bits will be cleared to 0" 이다.
     * 예전 mock 은 SR1 만 쓰고 QE 는 건드리지 않았는데, 그래서 1-byte SR1 갱신이
     * test suite 에서는 무해해 보였지만 실제 부품은 조용히 quad mode 를 벗어났다.
     * 여기서 silicon 을 흉내낸다.
     * ------------------------------------------------------------------- */
    if ((xfer->tx_buf != NULL) && (xfer->tx_buf[0] == F_CMD_WRSR1)) {
        s_mock.wrsr1_frame_bytes = xfer->tx_len;
        if ((xfer->tx_len == 2U) && Mock_Uses16BitWrsr()) {
            s_mock.sr2[s_mock.current_die] &= (uint8_t)~0x42U;   /* clear QE (S9) + CMP (S14) */
        }
    }

    /* SW-037: count every status-register write that reaches the bus. */
    if ((xfer->tx_buf != NULL) &&
        ((xfer->tx_buf[0] == F_CMD_WRSR1) ||
         (xfer->tx_buf[0] == F_CMD_WRSR2) ||
         (xfer->tx_buf[0] == F_CMD_WRSR3))) {
        s_mock.wrsr_frame_count++;
    }

    Flash_SPI_CS_High();
}

/******************************************************************************
 * Test Cases
 ******************************************************************************/
static void Test_TableLookup(void)
{
    printf("[TEST 1] Testing JEDEC ID Table Lookups...\n");

    assert(Flash_FindDevice(0xC22010U) != NULL); /* MX25L512 (512Kb) */
    assert(Flash_FindDevice(0xC2201AU) != NULL); /* MX25L512 (512Mb) */
    assert(Flash_FindDevice(0xEF6119U) != NULL); /* W25M512 */
    assert(Flash_FindDevice(0xEF6018U) != NULL); /* W25Q128JW */
    assert(Flash_FindDevice(0xEF4020U) != NULL); /* W25Q512JV */
    assert(Flash_FindDevice(0xEF4019U) != NULL); /* W25Q256JV */
    assert(Flash_FindDevice(0xC86017U) != NULL); /* GD25LQ64E */
    assert(Flash_FindDevice(0xC86016U) != NULL); /* GD25LQ32E */
    assert(Flash_FindDevice(0xC86015U) != NULL); /* GD25LQ16E */

    assert(Flash_FindDevice(0x123456U) == NULL);

    /* SW-041: 이제 훑을 flat table 이 없다. 예전에 행이던 ID 들을 쓸어 보며,
     * vendor + density resolver 가 그것들을 어떻게 해석하는지 출력한다. */
    static const struct { uint32_t id; const char *name; } known[] = {
        { 0xEF6018U, "W25Q128JW"  }, { 0xEF4020U, "W25Q512JV"  },
        { 0xEF4019U, "W25Q256JV"  }, { 0xEF6119U, "W25M512"    },
        { 0xC22010U, "MX25L512"   }, { 0xC2201AU, "MX25L51245G"},
        { 0xC86017U, "GD25LQ64E"  }, { 0xC86016U, "GD25LQ32E"  },
        { 0xC86015U, "GD25LQ16E"  }
    };
    for (uint32_t i = 0U; i < (uint32_t)(sizeof(known) / sizeof(known[0])); i++) {
        const FLASH_INFO *p_info = Flash_FindDevice(known[i].id);
        assert(p_info != NULL);
        printf("  - %-13s JEDEC 0x%06X | Size: %6u KB | Addr: %uB | QE: SR%u b%u\n",
               known[i].name, p_info->jedec_id, (p_info->flash_size / 1024U),
               p_info->addr_bytes, p_info->qe_reg, p_info->qe_bit);
        /* Geometry is shared by every part (constants, not table fields) */
        assert(FLASH_PAGE_SIZE == 256U);
        assert(FLASH_SECTOR_SIZE == 4096U);
    }
        printf("  --> Table lookups PASSED.\n\n");
}

static void Test_InitAndProbe(void)
{
    printf("[TEST 2] Testing Flash_Init with various bus conditions...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0x00000000U;
    int status = Flash_Init();
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    printf("  - Floating bus 0x000000 correctly reported NO_CONNECT.\n");

    s_mock.mock_jedec_id = 0x00FFFFFFU;
    status = Flash_Init();
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    printf("  - Floating bus 0xFFFFFF correctly reported NO_CONNECT.\n");

    s_mock.mock_jedec_id = 0xAABBCCU;
    status = Flash_Init();
    assert(status == FLASH_ERR_UNSUPPORTED);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    printf("  - Unsupported ID 0xAABBCC correctly reported UNSUPPORTED.\n");

    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U); /* QE not set initially */
    status = Flash_Init();
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25Q256JV);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U); /* QE enabled! */
    assert(g_flash_handle.qe_enabled == true);
    printf("  - Valid W25Q256JV initialized, QE automatically enabled. PASSED.\n\n");
}

static void Test_GD25LQ64E_StatusWrite(void)
{
    printf("[TEST 3] Testing GD25LQ64E 2-Byte Status Register Write...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x1CU, 0x00U, 0x00U); /* SR1 has BP bits, QE=0 */

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_GD25LQ64E);
    assert(s_mock.sr1[0] == 0x1CU);                         /* SR1 preserved */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);   /* QE in SR2 set */
    printf("  - GD25LQ64E 2-byte WRSR handled properly. PASSED.\n\n");
}

static void Test_PageBoundarySplitting(void)
{
    uint8_t write_buffer[300];

    printf("[TEST 4] Testing 256-Byte Page Boundary Splitting in Flash_Write...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    for (uint32_t i = 0U; i < 300U; i++) {
        write_buffer[i] = (uint8_t)(i & 0xFFU);
    }

    s_mock.page_program_count = 0U;
    s_mock.total_bytes_programmed = 0U;

    status = Flash_Write(0x000000F0U, write_buffer, 300U, FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count == 3U);
    printf("  - 300 bytes cross-page write successfully split into %u chunks. PASSED.\n\n",
           s_mock.page_program_count);
}

static void Test_MultiDieHandling(void)
{
    uint8_t test_buf[64];

    printf("[TEST 5] Testing Winbond SpiStack Multi-Die (W25M512)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6119U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    s_mock.last_die_selected = 0xFFU;
    s_mock.die_select_count = 0U;

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25M512);
    assert(g_flash_handle.info->die_count == 2U);   /* SW-044: die_count is the flag */

    g_flash_handle.current_die = 0xFFU;

    memset(test_buf, 0xA5, sizeof(test_buf));
    status = Flash_Write(0x01000000U, test_buf, sizeof(test_buf), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.last_die_selected == 0U);
    printf("  - Address 16MB mapped to Die 0 (C2h sent with Die 0).\n");

    status = Flash_Write(0x02500000U, test_buf, sizeof(test_buf), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.last_die_selected == 1U);
    printf("  - Address 37MB automatically switched to Die 1 via C2h opcode.\n");
    printf("  - Multi-Die switching verified. PASSED.\n\n");
}

static void Test_SFCDMAReadSequence(void)
{
    uint32_t aligned_buf[1024]; /* 4096 bytes, 4-byte aligned */

    printf("[TEST 6] Testing Flash Read Units and SFC DMA Read...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* Alignment rejections */
    uint8_t *p_unaligned = ((uint8_t *)aligned_buf) + 1;
    status = Flash_Read(0x00001000U, p_unaligned, 32U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);

    status = Flash_Read(0x00001002U, aligned_buf, 32U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);
    printf("  - Word alignment checks verified with FLASH_ERR_ALIGNMENT.\n");

    /* Default unit resolves to WORD */
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_DEFAULT,
                        FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ_4B);  /* W25Q256JV is 4-Byte -> 0xEC */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    assert(g_p_sfc_regs->MODE == 4U);
    assert(g_p_sfc_regs->ACCESS_UNIT == 4U);
    printf("  - Default unit (0) resolved to Word (4 bytes) in SFC ACCESS_UNIT.\n");

    status = Flash_Read(0x00001000U, aligned_buf, 2048U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_HALF, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->ACCESS_UNIT == 2U);

    status = Flash_Read(0x00001001U, aligned_buf, 10U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_HALF, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_ERR_ALIGNMENT);
    printf("  - Halfword unit and 2-byte alignment verified.\n");

    status = Flash_Read(0x00001001U, p_unaligned, 128U, FLASH_READ_FAST,
                        FLASH_READ_UNIT_BYTE, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->ACCESS_UNIT == 1U);
    printf("  - Byte unit tested.\n");

    /* Direct SPI with all units */
    status = Flash_Read(0x00001000U, aligned_buf, 64U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, aligned_buf, 128U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_HALF, 0U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001001U, p_unaligned, 256U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_BYTE, 0U);
    assert(status == FLASH_OK);
    printf("  - Direct SPI Read with Word, Halfword, Byte verified.\n");

    status = Flash_Read(0x00001000U, aligned_buf, 10U, FLASH_READ_QUAD_IO,
                        (FLASH_READ_UNIT)3, 0U);
    assert(status == FLASH_ERR_PARAM);
    printf("  - Invalid unit rejected with FLASH_ERR_PARAM.\n");

    printf("  - Read unit and SFC DMA sequence test PASSED.\n\n");
}

/******************************************************************************
 * NEW TESTS (SW-013): boot model, family match, fail-safe, per-die QE,
 *                      CRC 누적, direct-SPI guard.
 ******************************************************************************/
static void Test_BootConfigAndLock(void)
{
    printf("[TEST 7] Boot: ID check -> basic-read settings -> configure -> lock...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV present */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD); /* selects W25Q256JV */

    assert(Flash_IsLocked() == false);
    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    assert(Flash_IsLocked() == true);                 /* immutable */
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25Q256JV);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U); /* QE applied */

    /* Re-boot must be refused (selection is immutable) */
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_BUSY);
    printf("  - Boot configured from flash settings and locked. PASSED.\n\n");
}

static void Test_BootFamilyMatch(void)
{
    printf("[TEST 8] Boot: same product family accepted, foreign family refused...\n");

    /* 같은 제품군(Winbond EF40xx), 용량만 다름: W25Q512JV 를 선택했지만 실제로는
     * W25Q256JV 가 붙어 있음 -> 둘 다 family EF+0x40 -> 허용. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV present */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4020U, 4096U, FLASH_FEAT_QUAD); /* selects W25Q512JV */
    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_READY);
    printf("  - Same-family device (W25Q256JV for W25Q512JV selection) ACCEPTED.\n");

    /* Foreign family: W25Q256JV selected, MX25L512 present -> FAIL-SAFE. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;                 /* MX25L512 present */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD); /* selects W25Q256JV */
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_UNSUPPORTED);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    assert(Flash_IsLocked() == false);
    printf("  - Foreign-family device (MX25L512 for W25Q256JV selection) REFUSED.\n");

    /* Corrupt settings block (bad CRC) -> fail-safe. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD);
    /* SW-021: crc 가 더는 block 의 마지막 field 가 아니다(spi_hz 를 뒤에 덧붙였다).
     * 그래서 offset 으로 접근한다. "마지막 byte" 로 쓰면 이제 clock field 를
     * 건드리게 되는데, 그 field 는 일부러 checksum window 밖에 있다. */
    s_mock.config_buf[offsetof(FLASH_BOOT_CONFIG, crc)] ^= 0xFFU;
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_PARAM);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    printf("  - Corrupt settings block (bad CRC) REFUSED.\n");
    printf("  - Family matching / fail-safe verified. PASSED.\n\n");
}

static void Test_BootNoConnect(void)
{
    printf("[TEST 9] Boot: no ID response -> NO_CONNECT fail-safe...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0x00000000U;
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD);
    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);
    assert(Flash_IsLocked() == false);
    printf("  - Absent device reported NO_CONNECT. PASSED.\n\n");
}

static void Test_MultiDiePerDieConfig(void)
{
    printf("[TEST 10] G1: per-die QE applied on BOTH dies...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6119U;                 /* W25M512 (2-die) */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);              /* QE off on both dies */

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert(g_flash_handle.info->model_id == FLASH_MODEL_W25M512);

    /* Both dies must now have QE set (each die has its own status regs). */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);
    assert((s_mock.sr2[1] & FLASH_SR2_QE_WINBOND) != 0U);
    printf("  - Die 0 and Die 1 both: QE enabled. PASSED.\n\n");
}

static void Test_CRCAccumulate(void)
{
    uint32_t aligned_buf[2048]; /* 8192 bytes */

    printf("[TEST 11] G3: CRC start/continue chaining + algorithm selection...\n");

    /* --- Chaining: (start N1) + (continue N2) == (start N1+N2) --- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* One-shot: 4096 bytes with CRC start. */
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_full = Flash_GetLastCRC();

    /* Chained: 2048 (start) then 2048 (continue). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    status = Flash_Init();
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, aligned_buf, 512U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_first = Flash_GetLastCRC();
    status = Flash_Read(0x00002000U, aligned_buf, 512U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC_CONT);
    assert(status == FLASH_OK);
    const uint32_t crc_chain = Flash_GetLastCRC();

    printf("  - one-shot=%08X first=%08X chained=%08X\n", crc_full, crc_first, crc_chain);
    assert(crc_full == crc_chain);      /* chaining == one-shot */
    assert(crc_first != crc_chain);     /* continue extended the CRC */
    printf("  - CRC chaining (start/continue) verified. ");

    /* --- Algorithm selection changes the result --- */
    g_flash_handle.crc_algo = FLASH_CRC_CHECKSUM;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_sum = Flash_GetLastCRC();

    g_flash_handle.crc_algo = FLASH_CRC32;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_32 = Flash_GetLastCRC();

    g_flash_handle.crc_algo = FLASH_CRC8;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_8 = Flash_GetLastCRC();

    printf("crc8=%08X crc32=%08X checksum=%08X\n", crc_8, crc_32, crc_sum);
    assert(crc_32 != crc_sum);
    assert(crc_8 != crc_32);
    printf("  - Algorithm selection (crc8/crc32/checksum) verified. PASSED.\n\n");
}

static void Test_DirectSPIQuadGuard(void)
{
    uint32_t aligned_buf[16];

    printf("[TEST 12] G4: Direct SPI refuses Dual/Quad (single-width only)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    /* Quad I/O over the direct SPI path must be refused (no silent wrong data). */
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_ERR_PARAM);

    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_DUAL,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_ERR_PARAM);

    /* Single / Fast on direct SPI remain allowed. */
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_SINGLE,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_FAST,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_OK);
    /* QPI(4-4-4)는 read-mode enum 에 있지만 일부러 구현하지 않았다. 두 전송 경로
     * 모두 전용 error(FLASH_ERR_PARAM 이 아님)로 거절해야, 호출자가 "모르는 mode"
     * 와 "알지만 미지원"을 구분할 수 있다. */
    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_QPI,
                        FLASH_READ_UNIT_WORD, 0U);
    assert(status == FLASH_ERR_UNSUPPORTED);

    status = Flash_Read(0x00001000U, aligned_buf, 16U, FLASH_READ_QPI,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC);
    assert(status == FLASH_ERR_UNSUPPORTED);
    printf("  - QPI (4-4-4) listed but refused with FLASH_ERR_UNSUPPORTED.\n");
    printf("  - Direct SPI guard verified. PASSED.\n\n");
}

static void Test_ProtectUnprotect(void)
{
    printf("[TEST 13] G2: Protect / Unprotect (block-protect bits)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);

    status = Flash_Protect(0x0FU);
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x3CU);          /* BP0..BP3 set */

    status = Flash_Unprotect();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);          /* BP cleared */

    /* SW-046: Winbond 128 Mbit 이하(W25Q128JV, 0xEF4018)는 BP2..BP0 만 있고
     * SR1 bit5 는 TB 다. 4-bit mask 를 그대로 쓰면 BP3 대신 TB 를 뒤집어
     * "보호했다고 믿는데 아무것도 안 걸린" 상태가 된다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4018U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    status = Flash_Init();
    assert(status == FLASH_OK);

    status = Flash_Protect(0x08U);                      /* BP3 요청 = 이 부품엔 없음 */
    assert(status == FLASH_ERR_PARAM);
    assert(s_mock.sr1[0] == 0x00U);                     /* BP 도 TB 도 그대로 */

    status = Flash_Protect(0x07U);                      /* BP2..BP0 */
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x1CU) == 0x1CU);
    assert((s_mock.sr1[0] & 0x20U) == 0x00U);           /* TB 는 건드리지 않는다 */

    /* TB=1 로 이미 설정된 부품: Unprotect 는 BP 만 지우고 TB 는 남긴다. */
    Mock_SetStatus(0x20U, 0x02U, 0x00U);
    status = Flash_Unprotect();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x1CU) == 0x00U);
    assert((s_mock.sr1[0] & 0x20U) == 0x20U);           /* TB 보존 */
    printf("  - Protect sets BP bits, Unprotect clears them. PASSED.\n");
    printf("  - SW-046: 3-bit BP part refuses BP3 mask, keeps TB. PASSED.\n\n");
}

/* SW-037: driver 는 더는 칩을 스스로 unlock 하지 않는다.
 * 예전 TEST 14 는 반대("write 가 BP bit 를 지운다")를 주장했으므로, 계약이
 * 바뀐 것과 함께 test 도 뒤집는다: 아직 보호된 부품을 향한 program/erase 는
 * SR1 을 그대로 두어야 하고, status write frame 을 단 하나도 bus 에 올려서는
 * 안 된다. 대신 호출자가 Flash_Unprotect() 를 부른다. */
static void Test_WriteKeepsProtect(void)
{
    uint8_t write_buffer[8];
    printf("[TEST 14] SW-037: Write/Erase never clear block-protect...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x1CU, 0x02U, 0x00U);              /* SR1: BP0..BP2 already set */
    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x1CU);         /* Init leaves BP alone */

    /* (1) program while protected: the 02h frame goes out, the SR is untouched */
    memset(write_buffer, 0x5A, sizeof(write_buffer));
    s_mock.page_program_count = 0U;
    s_mock.wrsr_frame_count   = 0U;
    status = Flash_Write(0x00000000U, write_buffer, sizeof(write_buffer), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count >= 1U);           /* program frame was sent */
    assert(s_mock.wrsr_frame_count == 0U);             /* no 01h / 31h / 11h at all */
    assert((s_mock.sr1[0] & 0x3CU) == 0x1CU);          /* protection still in force */

    /* (2) erase while protected: same rule, no unlock, no restore */
    s_mock.wrsr_frame_count = 0U;
    status = Flash_Erase(0x00000000U, FLASH_ERASE_SECTOR);
    assert(status == FLASH_OK);
    assert(s_mock.wrsr_frame_count == 0U);
    assert((s_mock.sr1[0] & 0x3CU) == 0x1CU);

    /* (3) the caller decides: explicit Unprotect, then the write is allowed */
    status = Flash_Unprotect();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);

    s_mock.page_program_count = 0U;
    status = Flash_Write(0x00000000U, write_buffer, sizeof(write_buffer), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count >= 1U);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);          /* still clear, not re-set */

    printf("  - Write/Erase leave SR untouched; caller unprotects. PASSED.\n\n");
}

static void Test_AddrModeOpcodes(void)
{
    uint32_t buf[4];
    printf("[TEST 15] Req2: 3-Byte vs 4-Byte opcode selection...\n");

    /* 4-Byte device (W25Q256JV): Quad I/O read must use the 4B opcode (0xEC). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    int status = Flash_Init();
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, buf, 4U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ_4B);   /* 0xEC */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    printf("  - 4-Byte device used Quad I/O 4B opcode 0x%02X.\n", g_p_sfc_regs->CMD);

    /* 3-Byte device (W25Q128JW): Quad I/O read must use the 3B opcode (0xEB). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6018U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    status = Flash_Init();
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, buf, 4U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ);       /* 0xEB */
    assert(g_p_sfc_regs->ADDR_SIZE == 3U);
    printf("  - 3-Byte device used Quad I/O 3B opcode 0x%02X.\n", g_p_sfc_regs->CMD);

    /* Method B (Enter-4-Byte via B7h): same 3B opcode + 4B address + B7h sent. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    FLASH_BOOT_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic         = FLASH_BOOT_CFG_MAGIC;
    cfg.jedec_expect  = 0xEF4019U;
    cfg.model         = (uint32_t)FLASH_MODEL_UNKNOWN;
    cfg.features      = FLASH_FEAT_QUAD;
    cfg.sfc_page_size = 4096U;
    cfg.addr_mode     = (uint8_t)FLASH_ADDR_4BYTE_ENTER;
    cfg.crc           = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);
    status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    status = Flash_Read(0x00001000U, buf, 4U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_DMA);
    assert(status == FLASH_OK);
    assert(g_p_sfc_regs->CMD == F_CMD_QUAD_IO_READ);       /* 0xEB (3B opcode) */
    assert(g_p_sfc_regs->ADDR_SIZE == 4U);
    assert(s_mock.entered_4byte[0] == true);             /* B7h sent */
    printf("  - Method B (Enter-4B) used 3B opcode 0x%02X + sent B7h. PASSED.\n\n", g_p_sfc_regs->CMD);

    /* (4) addr_mode 가 0..3 이 아닌데 CRC 는 CORRECT 인 boot block 은 거절해야 한다.
     * 범위 가드가 없으면 그 값이 그대로 cast 되어 Method A 도 B 도 아니게 되고,
     * 따라서 3-byte opcode + 4-byte address + B7h 없음이 된다 — 칩은 남는 address
     * byte 를 data 로 밀어낸다. 조용한 오독, 그리고 FLASH_OK 반환. CRC 로는 잡을 수
     * 없다: byte 는 온전히 도착했고, 다만 의미가 없을 뿐이다. */
    {
        FLASH_BOOT_CONFIG bad;
        Mock_Reset();
        s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV, 4-byte part */
        Mock_SetStatus(0x00U, 0x02U, 0x00U);
        memset(&bad, 0, sizeof(bad));
        bad.magic         = FLASH_BOOT_CFG_MAGIC;
        bad.jedec_expect  = 0xEF4019U;
        bad.model         = (uint32_t)FLASH_MODEL_UNKNOWN;
        bad.features      = FLASH_FEAT_QUAD;
        bad.sfc_page_size = 4096U;
        bad.addr_mode     = 9U;                           /* out of range */
        bad.crc           = Flash_ConfigCrc(&bad);       /* ...but CRC is valid */
        memcpy(s_mock.config_buf, &bad, sizeof(bad));
        s_mock.config_len = (uint32_t)sizeof(bad);
        status = Flash_BootInit(0x00000000U);
        assert(status == FLASH_ERR_PARAM);
        assert(Flash_GetState() == FLASH_STATE_ERROR);
        printf("  - addr_mode 9 (out of range, valid CRC) refused with PARAM. PASSED.\n\n");
    }
}

static void Test_InitNoReset(void)
{
    printf("[TEST 16] G7: Init option skips software reset (fast boot)...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    s_mock.reset_cmd_count = 0U;
    int status = Flash_InitOpt(FLASH_INIT_OPT_NO_RESET);
    assert(status == FLASH_OK);
    assert(s_mock.reset_cmd_count == 0U);
    printf("  - NO_RESET: no reset commands sent.\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    s_mock.reset_cmd_count = 0U;
    status = Flash_InitOpt(FLASH_INIT_OPT_NONE);
    assert(status == FLASH_OK);
    assert(s_mock.reset_cmd_count >= 2U);             /* 66h + 99h */
    printf("  - default: reset commands (66h/99h) sent. PASSED.\n\n");
}

static void Test_TableReadCfgGuard(void)
{
    printf("[TEST 17] G6: shared read_cfg values preserved after dedup...\n");

    const FLASH_INFO *w25q256 = Flash_FindDevice(0xEF4019U);
    assert(w25q256 != NULL);
    assert(w25q256->read_cfg[FLASH_READ_QUAD_IO].dummy_cycles == 8U);
    assert(w25q256->read_cfg[FLASH_READ_SINGLE].command == F_CMD_READ);
    assert(w25q256->read_cfg[FLASH_READ_SINGLE].command_4b == F_CMD_READ_4B);
    assert(w25q256->read_cfg[FLASH_READ_QUAD_IO].command == F_CMD_QUAD_IO_READ);
    assert(w25q256->read_cfg[FLASH_READ_QUAD_IO].command_4b == F_CMD_QUAD_IO_READ_4B);

    const FLASH_INFO *gd = Flash_FindDevice(0xC86017U);
    assert(gd != NULL);
    assert(gd->model_id == FLASH_MODEL_GD25LQ64E);
    assert(gd->read_cfg[FLASH_READ_DUAL_IO].dummy_cycles == 6U);
    assert(gd->read_cfg[FLASH_READ_QUAD_IO].dummy_cycles == 6U);

    const FLASH_INFO *mx = Flash_FindDevice(0xC22010U);
    assert(mx != NULL);
    assert(mx->read_cfg[FLASH_READ_QUAD_IO].dummy_cycles == 6U);
    printf("  - Per-device DUAL_IO / QUAD_IO dummy values intact. PASSED.\n\n");
}

static void Test_VsrWriteEnable(void)
{
    printf("[TEST 18] WREN 50h (volatile) vs 06h selection...\n");

    /* W25Q256JV has FLASH_FEAT_VSR_WREN -> status writes use 50h */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteEnable() == FLASH_OK);
    assert(s_mock.wren_count == 1U && s_mock.vsr_wren_count == 0U);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteEnableVolatile() == FLASH_OK);
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteStatus(1U, 0x00U) == FLASH_OK);
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);
    printf("  - W25Q256JV (VSR_WREN): WriteEnable=06h, Volatile=50h, StatusWrite=50h.\n");

    /* MX25L512 has no VSR flag -> status writes fall back to 06h */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteStatus(1U, 0x00U) == FLASH_OK);
    assert(s_mock.wren_count == 1U && s_mock.vsr_wren_count == 0U);
    printf("  - MX25L512 (no flag): StatusWrite=06h fallback. PASSED.\n\n");
}

static void Test_VendorDensityResolution(void)
{

    /* (a) Every ID that used to be a table row must resolve to the same facts. */
    static const struct {
        uint32_t id; uint32_t size; uint8_t addr; const char *name;
    } rows[] = {
        { 0xEF6018U, 16UL*1024UL*1024UL, 3U, "W25Q128JW"   },
        { 0xEF4020U, 64UL*1024UL*1024UL, 4U, "W25Q512JV"   },
        { 0xEF4019U, 32UL*1024UL*1024UL, 4U, "W25Q256JV"   },
        { 0xEF6119U, 64UL*1024UL*1024UL, 4U, "W25M512"     },
        { 0xC22010U, 64UL*1024UL,        3U, "MX25L512"    },
        { 0xC2201AU, 64UL*1024UL*1024UL, 4U, "MX25L51245G" },
        { 0xC86017U,  8UL*1024UL*1024UL, 3U, "GD25LQ64E"   },
        { 0xC86016U,  4UL*1024UL*1024UL, 3U, "GD25LQ32E"   },
        { 0xC86015U,  2UL*1024UL*1024UL, 3U, "GD25LQ16E"   }
    };

    printf("[TEST 19] Vendor + density resolution (SW-041)...\n");
    for (uint32_t i = 0U; i < (uint32_t)(sizeof(rows) / sizeof(rows[0])); i++) {
        const FLASH_INFO *p = Flash_FindDevice(rows[i].id);
        assert(p != NULL);
        assert(p->jedec_id == rows[i].id);
        assert(p->flash_size == rows[i].size);
        assert(p->addr_bytes == rows[i].addr);
        assert(p->model_id == rows[i].id);          /* model == JEDEC ID */
        printf("  - %-13s %7u KB, %uB addr. PASSED.\n",
               rows[i].name, p->flash_size / 1024U, p->addr_bytes);
    }

    /* (b) 예전 table 에 행이 없던 것들이 이제 자유롭다: density byte 하나로 설명된다.
     * 이것이 SW-041 의 요점 전체다. */
    assert(Flash_FindDevice(0xEF4015U)->flash_size ==  2UL*1024UL*1024UL); /* W25Q16JV */
    assert(Flash_FindDevice(0xEF4016U)->flash_size ==  4UL*1024UL*1024UL); /* W25Q32JV */
    assert(Flash_FindDevice(0xEF4017U)->flash_size ==  8UL*1024UL*1024UL); /* W25Q64JV */
    assert(Flash_FindDevice(0xEF4015U)->addr_bytes == 3U);
    assert(Flash_FindDevice(0xEF4015U)->qe_reg == 2U);
    assert(Flash_FindDevice(0xEF4015U)->qe_bit == 1U);

    /* (c) Same for other vendors and densities that never had a row. */
    assert(Flash_FindDevice(0xC22018U)->flash_size == 16UL*1024UL*1024UL); /* MX25L128 */
    assert(Flash_FindDevice(0xC84018U)->flash_size == 16UL*1024UL*1024UL); /* GD25Q128 */
    assert(Flash_FindDevice(0xC84017U)->flash_size ==  8UL*1024UL*1024UL); /* GD25Q64  */

    /* (d) The 16 MB boundary: 16 MB stays 3-byte, 32 MB flips to 4-byte. */
    assert(Flash_FindDevice(0xEF6018U)->addr_bytes == 3U);
    assert(Flash_FindDevice(0xEF4019U)->addr_bytes == 4U);
    assert(Flash_FindDevice(0xEF6018U)->read_cfg != Flash_FindDevice(0xEF4019U)->read_cfg);

    /* (e) Exceptions still win over the formula. */
    assert(Flash_FindDevice(0xEF4020U)->flash_size == 64UL*1024UL*1024UL); /* byte 0x20 lies */
    assert(Flash_FindDevice(0xEF6119U)->die_count == 2U);
    assert(Flash_FindDevice(0xEF6119U)->die_size == 32UL*1024UL*1024UL);

    /* (f) Unknown vendor / nonsense density / unknown model are refused. */
    assert(Flash_FindDevice(0x9D4018U) == NULL);   /* ISSI is not supported */
    assert(Flash_FindDevice(0xEF4000U) == NULL);   /* density 0 */
    assert(Flash_FindDevice(0xEF40FFU) == NULL);   /* density 255 */
    assert(Flash_FindDeviceByModel(FLASH_MODEL_UNKNOWN) == NULL);

    /* (g) ByModel is the same lookup: FLASH_MODEL is a JEDEC ID. */
    assert(Flash_FindDeviceByModel(FLASH_MODEL_W25Q256JV) == Flash_FindDevice(0xEF4019U));

    printf("  - 9 legacy parts reproduced, 6 free densities resolved. PASSED.\n\n");
}

static void Test_CrcAlgoBootSelect(void)
{
    uint32_t aligned_buf[1024];

    printf("[TEST 20] CRC algorithm selected from boot config...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    Mock_SetBootConfig(0xEF4019U, 4096U, FLASH_FEAT_QUAD);

    /* Override crc_algo = CHECKSUM in the settings block and fix its CRC */
    FLASH_BOOT_CONFIG cfg;
    memcpy(&cfg, s_mock.config_buf, sizeof(cfg));
    cfg.crc_algo = (uint8_t)FLASH_CRC_CHECKSUM;
    cfg.crc = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));

    int status = Flash_BootInit(0x00000000U);
    assert(status == FLASH_OK);
    assert(g_flash_handle.crc_algo == FLASH_CRC_CHECKSUM);

    /* Read with CRC: the selected CHECKSUM algorithm is used ... */
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_sum = Flash_GetLastCRC();

    /* ... and it differs from the CRC32 default */
    g_flash_handle.crc_algo = FLASH_CRC32;
    status = Flash_Read(0x00001000U, aligned_buf, 1024U, FLASH_READ_QUAD_IO,
                        FLASH_READ_UNIT_WORD, FLASH_READ_OPT_SFC | FLASH_READ_OPT_CRC);
    assert(status == FLASH_OK);
    const uint32_t crc_32 = Flash_GetLastCRC();

    printf("  - crc_algo=CHECKSUM from config: checksum=%08X vs crc32=%08X\n", crc_sum, crc_32);
    assert(crc_sum != crc_32);
    printf("  - Boot-config CRC algorithm selection verified. PASSED.\n\n");
}

static void Test_EasyApi(void)
{
    uint8_t buf[256];

    printf("[TEST 21] Easy API: ReadBytes / WriteBytes / EraseSector / EraseRange / EraseChip...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q256JV (quad) */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    /* Convenience accessors */
    assert(Flash_SizeBytes() == 32U * 1024U * 1024U);
    assert(Flash_SectorSize() == 4096U);

    /* Byte-granular read at any address (auto mode, any alignment) */
    assert(Flash_ReadBytes(0x00001000U, buf, sizeof(buf)) == FLASH_OK);

    /* Erase a region then program it */
    assert(Flash_EraseRange(0x00002000U, 4096U) == FLASH_OK);   /* 1 sector */
    assert(Flash_EraseSector(0x00004123U) == FLASH_OK);         /* containing sector */
    assert(Flash_EraseSector(Flash_SizeBytes()) == FLASH_ERR_PARAM);
    assert(Flash_WriteBytes(0x00002000U, buf, sizeof(buf)) == FLASH_OK);

    /* Unaligned address / odd length are fine (auto rounding + byte unit) */
    assert(Flash_EraseRange(0x00003123U, 100U) == FLASH_OK);
    assert(Flash_ReadBytes(0x00003123U, buf, 100U) == FLASH_OK);
    assert(Flash_WriteBytes(0x00003123U, buf, 100U) == FLASH_OK);

    /* Whole-chip erase */
    assert(Flash_EraseChip() == FLASH_OK);
    printf("  - Easy API round-trip (any addr/len, auto mode) verified. PASSED.\n\n");
}

static void Test_DelayUsConversion(void)
{
    printf("[TEST 22] Time-based delay: us -> loop conversion (SW-014)...\n");

    /* Defaults: FLASH_SYS_CLK_HZ = 108MHz, CYCLES_PER_LOOP = 3 -> 36 loops/us */
    assert(Flash_SysClockGet() == FLASH_SYS_CLK_HZ);
    assert(Flash_DelayUsToLoops(0U) == 0U);
    assert(Flash_DelayUsToLoops(1U) == 36U);
    assert(Flash_DelayUsToLoops(10U) == 360U);
    assert(Flash_DelayUsToLoops(1000U) == 36000U);
    printf("  - 108MHz default: 1us->36 loops, 1ms->36000 loops. PASSED.\n");

    /* 50MHz target: ceil(50/3) = 17 loops/us (round-up = never under-delays) */
    Flash_SysClockSet(50000000U);
    assert(Flash_SysClockGet() == 50000000U);
    assert(Flash_DelayUsToLoops(1U) == 17U);
    assert(Flash_DelayUsToLoops(1000U) == 17000U);
    printf("  - 50MHz: 1us->17 loops (ceil rounding). PASSED.\n");

    /* Invalid clock (0) is ignored */
    Flash_SysClockSet(0U);
    assert(Flash_SysClockGet() == 50000000U);
    printf("  - Clock guard: hz=0 ignored. PASSED.\n");

    /* Smoke: delays execute and return */
    Flash_DelayUs(1U);
    Flash_DelayUs(1000U);
    Flash_DelayLoops(10U);

    /* Restore default for any later suites */
    Flash_SysClockSet(FLASH_SYS_CLK_HZ);
    assert(Flash_DelayUsToLoops(1U) == 36U);

    printf("  - Time-based delay API verified. PASSED.\n");
}

static void Test_VsrWrenNoWel(void)
{
    uint8_t sr = 0U;

    printf("[TEST 23] 50h does NOT set WEL - volatile SR write (datasheet)...\n");

    /* GD25LQ64E datasheet 7.5: "The Write Enable for Volatile Status Register
     * command will not set the Write Enable Latch bit, it is only valid for
     * the Write Status Register command". Winbond W25Q 계열도 같다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;   /* init may enable QE */
    assert(Flash_WriteEnableVolatile() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) == 0U);      /* WEL must stay 0 */
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);
    printf("  - GD25LQ64E: 50h issued, WEL remains 0. PASSED.\n");

    /* Volatile SR2 write must succeed with WEL=0 via the pending 50h */
    s_mock.wren_count = 0U; s_mock.vsr_wren_count = 0U;
    assert(Flash_WriteStatus(2U, 0x02U) == FLASH_OK);
    assert(s_mock.sr2[0] == 0x02U);
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) == 0U);
    assert(s_mock.vsr_wren_count == 1U && s_mock.wren_count == 0U);
    printf("  - Volatile WRSR accepted with WEL=0 (pending 50h). PASSED.\n");

    /* GD rule: an intervening command clears the pending 50h */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_WriteEnableVolatile() == FLASH_OK);
    assert(Flash_ReadStatus(1U, &sr) == FLASH_OK);
    assert(s_mock.vsr_wren_pending == false);
    printf("  - Intervening command clears pending 50h. PASSED.\n\n");
}

static void Test_DummyCfgPerModel(void)
{
    static const struct { uint32_t jedec; uint8_t dummy_last; } models[] = {
        { 0xEF6018U, 0xFFU },   /* W25Q128JW  - Winbond: first dummy Fxh */
        { 0xEF4020U, 0xFFU },   /* W25Q512JV  - Winbond: first dummy Fxh */
        { 0xEF4019U, 0xFFU },   /* W25Q256JV  - Winbond: first dummy Fxh */
        { 0xEF6119U, 0xFFU },   /* W25M512    - Winbond: first dummy Fxh */
        { 0xC22010U, 0x00U },   /* MX25L512   - no mode byte */
        { 0xC2201AU, 0x00U },   /* MX25L51245G- no mode byte */
        { 0xC86017U, 0x00U }    /* GD25LQ64E  - M7-0 != Axh */
    };

    printf("[TEST 24] Per-model dummy data / dummy last (SW-013 DUMMY)...\n");

    for (uint32_t i = 0U; i < sizeof(models) / sizeof(models[0]); i++) {
        const FLASH_INFO *info = Flash_FindDevice(models[i].jedec);
        assert(info != NULL);

        /* 선로 순서: CMD -> ADDR -> DUMMY_LAST (첫 dummy = M7-M0)
         * -> DUMMY_VAL -> DATA. 뒤에 붙는 dummy byte 는 don't care. */
        assert(info->read_cfg[FLASH_READ_DUAL_IO].dummy_last  == models[i].dummy_last);
        assert(info->read_cfg[FLASH_READ_DUAL_IO].dummy_value == 0x00U);
        assert(info->read_cfg[FLASH_READ_QUAD_IO].dummy_last  == models[i].dummy_last);
        assert(info->read_cfg[FLASH_READ_QUAD_IO].dummy_value == 0x00U);

        /* mode_byte 는 0x00 이어야 한다: M7-M0 은 DUMMY_LAST 로 실린다.
         * (0x20 이면 Winbond Read Command Bypass / GD AXh 규칙으로 들어간다) */
        assert(info->read_cfg[FLASH_READ_DUAL_IO].mode_byte == 0x00U);
        assert(info->read_cfg[FLASH_READ_QUAD_IO].mode_byte == 0x00U);
    }
    printf("  - 7 models: dummy_last 0xFF (Winbond Fxh) / 0x00 (MX, GD). PASSED.\n");
    printf("  - mode_byte 0x00 everywhere (M7-M0 on DUMMY_LAST). PASSED.\n\n");
}

static void Test_BufferedXferAndSharedGeometry(void)
{
    uint32_t buf[64];       /* 256 bytes, 4-byte aligned */
    uint8_t  pg[16];

    printf("[TEST 25] SW-016: one-frame buffered SPI transfer + shared geometry...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;      /* GD25LQ64E: 3-byte address */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(g_flash_handle.addr_mode == FLASH_ADDR_3BYTE);

    /* --- Direct read: ONE transaction carries header AND data --- */
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Read(0x00001000U, buf, 64U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_OK);
    assert(s_xfer.frames == 2U);           /* 1 BUSY poll + 1 data frame */
    assert(s_xfer.last_first_tx == F_CMD_READ);
    assert(s_xfer.last_tx_len == 4U);      /* 03h + 3-byte address, no dummy */
    assert(s_xfer.last_rx_len == 256U);    /* 64 words */
    assert(s_xfer.last_data_len == 0U);
    printf("  - Read: header+data in 1 frame (tx_len=%u, rx_len=%u).\n",
           s_xfer.last_tx_len, s_xfer.last_rx_len);

    /* --- Fast read: the dummy byte rides inside the same header --- */
    const uint32_t fast_dummy = (uint32_t)g_flash_handle.info->read_cfg[FLASH_READ_FAST].dummy_bytes;
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Read(0x00001000U, buf, 8U, FLASH_READ_FAST,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_OK);
    assert(s_xfer.frames == 2U);           /* 1 BUSY poll + 1 data frame */
    assert(s_xfer.last_tx_len == (1U + 3U + fast_dummy));
    printf("  - Fast read: %u dummy byte(s) packed into tx_len=%u.\n",
           fast_dummy, s_xfer.last_tx_len);

    /* --- Page program: header + separate data phase, still one CS frame --- */
    memset(pg, 0xA5, sizeof(pg));
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Write(0x00002000U, pg, sizeof(pg), FLASH_PROGRAM_SINGLE) == FLASH_OK);
    assert(s_xfer.last_payload_len == (uint32_t)sizeof(pg));
    assert(s_xfer.last_payload_tx_len == 4U);   /* 02h + 3-byte address */
    assert(s_xfer.last_data_payload_len == (uint32_t)sizeof(pg));
    printf("  - Program: payload of %u bytes in the same frame (tx_len=%u).\n",
           s_xfer.last_payload_len, s_xfer.last_payload_tx_len);

    /* --- Quad page program: same frame, payload on the 4-bit phase --- */
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Write(0x00003000U, pg, sizeof(pg), FLASH_PROGRAM_QUAD) == FLASH_OK);
    assert(s_xfer.last_quad_payload_len == (uint32_t)sizeof(pg));
    assert(s_xfer.last_data_payload_len == 0U);
    printf("  - Quad program: %u bytes on the 4-bit phase (tx_len=%u).\n",
           s_xfer.last_quad_payload_len, s_xfer.last_payload_tx_len);

    /* --- Shared geometry helper: both paths reject identical bad input --- */
    uint8_t *misaligned = ((uint8_t *)buf) + 1U;
    assert(Flash_Read(0x00001000U, misaligned, 4U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_ERR_ALIGNMENT);
    assert(Flash_SFC_DMARead(0x00001000U, misaligned, 4U,
                             FLASH_READ_UNIT_WORD, false, NULL) == FLASH_ERR_ALIGNMENT);

    /* count * unit must not wrap a 32-bit size (0x40000001 * 4) */
    assert(Flash_Read(0x00001000U, buf, 0x40000001U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_ERR_PARAM);
    assert(Flash_SFC_DMARead(0x00001000U, buf, 0x40000001U,
                             FLASH_READ_UNIT_WORD, false, NULL) == FLASH_ERR_PARAM);
    printf("  - Unit/alignment/overflow checks are shared by SPI and SFC paths.\n");

    /* --- Real-hardware SFC path: software must not edit DMA data or CRC --- */
    static SFC_TypeDef hw_regs;
    SFC_TypeDef *saved_regs = g_p_sfc_regs;
    uint32_t hw_buf[4];                    /* 16 bytes, 4-byte aligned */

    memset(&hw_regs, 0, sizeof(hw_regs));
    hw_regs.STATUS = SFC_STATUS_DMA_DONE;  /* DMA already completed */
    g_p_sfc_regs = &hw_regs;

    for (uint32_t i = 0U; i < 4U; i++) {
        hw_buf[i] = 0x5A5A5A5AU;
    }
    uint32_t crc_out = 0xDEADBEEFU;
    assert(Flash_SFC_DMARead(0x00001000U, hw_buf, 4U,
                             FLASH_READ_UNIT_WORD, true, &crc_out) == FLASH_OK);
    for (uint32_t i = 0U; i < 4U; i++) {
        assert(hw_buf[i] == 0x5A5A5A5AU);  /* HW-owned payload untouched */
    }
    assert(hw_regs.CRC_RES == 0U);         /* SW did not edit the CRC register */
    assert(crc_out == 0U);

    g_p_sfc_regs = saved_regs;
    printf("  - Real-HW SFC path: DMA buffer and CRC_RES untouched by software. PASSED.\n\n");
}

static void Test_NoConnectRefusesDataPath(void)
{
    uint8_t buf[16];

    printf("[TEST 26] NO_CONNECT: data path refuses and issues no SPI frames...\n");

    /* 1. A working device fills the handle */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    /* 2. 부품이 사라진다. re-init 이 그것을 감지해 NO_CONNECT 로 들어간다.
     *    handle 은 앞서의 model 을 그대로 들고 있는데, data path 가 info != NULL
     *    만이 아니라 driver state 까지 봐야 하는 이유가 바로 이것이다. */
    s_mock.mock_jedec_id = 0x000000U;
    assert(Flash_Init() == FLASH_ERR_NO_CONNECT);
    assert(Flash_GetState() == FLASH_STATE_NO_CONNECT);

    /* 3. Nothing may reach the bus from here on */
    memset(&s_xfer, 0, sizeof(s_xfer));
    memset(buf, 0x00, sizeof(buf));

    assert(Flash_Read(0x00001000U, buf, 4U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Write(0x00001000U, buf, sizeof(buf),
                       FLASH_PROGRAM_SINGLE) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Erase(0x00001000U, FLASH_ERASE_SECTOR) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Protect(0x0FU) == FLASH_ERR_NO_CONNECT);
    assert(Flash_Unprotect() == FLASH_ERR_NO_CONNECT);

    assert(s_xfer.frames == 0U);          /* no SPI transaction was started */

    printf("  - Read/Write/Erase/Protect refused, %u SPI frames. PASSED.\n\n",
           s_xfer.frames);
}

/* SW-019: GD25LQ64E는 01h가 "고정 16비트" 명령이다. 8비트에서 CS를 올리면
 * SR1은 써지지만 QE/CMP가 0으로 클리어된다(데이터시트 7.4). 따라서 SR1을
 * 바꾸는 모든 경로(Protect/Unprotect/Write 내부 Unprotect)는 반드시
 * "SR2를 읽어 그대로 되돌려 보내는 3바이트 프레임"이어야 한다. */
static void Test_GdCoupledWriteKeepsQe(void)
{
    uint8_t data[8];

    printf("[TEST 27] SW-019: GD 16-bit WRSR keeps QE alive on SR1 changes...\n");

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);

    int status = Flash_Init();
    assert(status == FLASH_OK);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);   /* Init이 QE를 켠다 */

    /* (1) BP0 설정 - SR1이 바뀌어도 QE는 살아 있어야 한다. */
    status = Flash_Protect(0x01U);
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x04U);                        /* BP0만 set */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);            /* QE 유지 */
    assert(s_mock.wrsr1_frame_bytes == 3U);                          /* opcode+SR1+SR2 */

    /* (2) 실제 쓰기 경로 - SW-037 이후 Write는 SR을 아예 건드리지 않는다.
     *     QE가 꺼지면 이후 quad 읽기가 전부 쓰레기가 되므로 프레임 0을 확인. */
    memset(data, 0x5AU, sizeof(data));
    s_mock.page_program_count = 0U;
    s_mock.wrsr_frame_count   = 0U;                                  /* 카운터 리셋 */
    status = Flash_Write(0x00000000U, data, sizeof(data), FLASH_PROGRAM_SINGLE);
    assert(status == FLASH_OK);
    assert(s_mock.page_program_count >= 1U);
    assert(s_mock.wrsr_frame_count == 0U);                           /* SR 무변경 */
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);            /* QE 유지 */
    assert(g_flash_handle.qe_enabled);                               /* 핸들도 일치 */

    /* (3) Unprotect도 같은 프레임을 써야 한다. */
    status = Flash_Unprotect();
    assert(status == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);
    assert((s_mock.sr2[0] & FLASH_SR2_QE_WINBOND) != 0U);
    assert(s_mock.wrsr1_frame_bytes == 3U);

    /* (4) 이 부품에는 11h(SR3 단독 쓰기)가 없다 - 무음 처리하지 않고 거부. */
    assert(Flash_WriteStatus(3U, 0x00U) == FLASH_ERR_UNSUPPORTED);

    printf("  - SR1 changed, QE survived, every 01h frame was 3 bytes. PASSED.\n\n");
}

/* SW-019: GD25LQ64E SR1의 S6은 BP4(다섯 번째 블록 보호 비트)다.
 * MX25L 계열에서 같은 비트는 QE이므로, 모델 플래그 없이 지우면 quad 모드가
 * 죽는다. 두 부품을 나란히 검증한다. */
static void Test_Bp4VersusQeBit(void)
{
    printf("[TEST 28] SW-019: S6 = BP4 on GD, but QE on Macronix...\n");

    /* (1) GD25LQ64E - BP4가 걸린 부품을 Unprotect하면 실제로 풀려야 한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x40U, 0x00U, 0x00U);                    /* BP4=1, 전체 보호 */
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_Unprotect() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_BP4) == 0U);          /* BP4 해제됨 */
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);

    assert(Flash_Protect(0x10U) == FLASH_OK);               /* mask bit4 -> BP4 */
    assert((s_mock.sr1[0] & FLASH_SR1_BP4) != 0U);

    /* (2) MX25L512 - 같은 비트가 QE다. Protect/Unprotect가 건드리면 안 된다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_QE_MX25L) != 0U);     /* Init이 QE(S6)를 켠다 */

    assert(Flash_Protect(0x0FU) == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_QE_MX25L) != 0U);     /* QE 유지 */
    assert((s_mock.sr1[0] & 0x3CU) == 0x3CU);               /* BP는 설정됨 */

    assert(Flash_Unprotect() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_QE_MX25L) != 0U);     /* QE 여전히 유지 */
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);

    printf("  - BP4 clears on GD and S6=QE survives on Macronix. PASSED.\n\n");
}

/* SW-020: WEL(S1)과 BUSY(S0)는 같은 05h 응답의 서로 다른 비트다.
 * WEL만 보고 돌아가면 칩이 아직 쓰기 중일 때도 "준비됨"으로 판정하고,
 * 뒤따르는 program 명령은 조용히 사라진다. 또한 상태 레지스터 쓰기는
 * read-back으로 실제 반영 여부를 확인해야 한다. */
static void Test_BusyGateAndStatusReadBack(void)
{
    printf("[TEST 29] SW-020: WEL+BUSY from one 05h read, WRSR read-back...\n");

    /* (1) WEL=1 + WIP=1 동시 상태. 옛 코드는 이 첫 읽기에서 OK를 돌려줬다
     *     (WEL만 검사했으므로). 지금은 BUSY가 풀릴 때까지 기다려야 한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;              /* W25Q64: 표준 1바이트 경로 */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    assert(Flash_WriteEnable() == FLASH_OK);       /* 먼저 정상적으로 WEL을 세운다 */
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) != 0U);

    s_mock.busy_reads_remaining = 3U;              /* 자체 타이밍 사이클 진행 중 */
    s_mock.wren_count = 0U;
    assert(Flash_WriteEnable() == FLASH_OK);
    assert(s_mock.busy_reads_remaining == 0U);     /* BUSY가 풀릴 때까지 기다렸다 */
    assert(s_mock.wren_count == 4U);               /* 3회는 busy로 무시, 4회째 성공 */

    /* (2) WEL=0 + busy 구간. WREN이 무시되므로 재시도해야만 성공한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.busy_reads_remaining = 2U;
    s_mock.wren_count = 0U;
    assert(Flash_WriteEnable() == FLASH_OK);
    assert((s_mock.sr1[0] & FLASH_SR1_WEL) != 0U);
    assert(s_mock.busy_reads_remaining == 0U);
    assert(s_mock.wren_count == 3U);

    /* (3) 조용히 사라진 WRSR은 read-back이 잡아낸다. 옛 코드는 OK를 돌려줬다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);

    s_mock.drop_next_wrsr = true;
    assert(Flash_WriteStatus(1U, 0x04U) == FLASH_ERR_WRITE_FAIL);
    assert((s_mock.sr1[0] & 0x04U) == 0x00U);      /* 실제로 반영되지 않았다 */

    /* (4) 정상 쓰기는 그대로 통과하고 값이 반영된다. */
    assert(Flash_WriteStatus(1U, 0x04U) == FLASH_OK);
    assert((s_mock.sr1[0] & 0x04U) != 0U);

    printf("  - Waited out BUSY, then proved the dropped WRSR. PASSED.\n\n");
}

/* ---------------------------------------------------------------------------
 * [TEST 30] SW-021: boot settings block 이 SPI clock 을 함께 나른다
 *
 * clock 은 더 이상 compile-time 상수가 아니다: Flash_BasicRead() 가 10 MHz 로
 * 가져오는 block 안에 실려 오고, 그 block 이 magic + checksum + family 검사를
 * 통과한 뒤에야 bus 에 적용된다. 흥미로운 부분은 호환용 안전판이다 — 예전
 * 24-byte block 에는 clock field 가 아예 없고, 그것이 부팅 실패가 되어서는 안 된다.
 * ------------------------------------------------------------------------- */
static void Test_BootConfigCarriesSpiClock(void)
{
    printf("[TEST 30] SW-021: boot settings block carries the SPI clock...\n");

    /* (1) Block that asks for a real clock: the bus ends up on exactly that. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;                 /* W25Q64 present */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    Mock_SetBootConfigHz(0xEF4019U, 4096U, FLASH_FEAT_QUAD, 24000000U);
    assert(Flash_BootInit(0x00000000U) == FLASH_OK);
    assert(s_mock.spi_speed_hz == 24000000U);

    /* (2) 예전 24-byte block: crc 뒤 4 byte 는 지워진 Flash 에서 0xFF 로 읽힌다.
     *     그래도 block 은 검증되어야 하고, 부팅은 성공해야 하며, driver 는 안전한
     *     10 MHz bring-up clock 을 유지해야 한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    Mock_SetBootConfigHz(0xEF4019U, 4096U, FLASH_FEAT_QUAD, 0xFFFFFFFFU);
    assert(Flash_BootInit(0x00000000U) == FLASH_OK);
    assert(s_mock.spi_speed_hz == 10000000U);         /* unchanged */

    /* (3) 말도 안 되는 값(bit flip 이지만 u32 로는 그럴듯한 값): 같은 안전판.
     *     Flash 부품이 200 MHz 로 도는 일은 없으므로, 믿지 않고 무시한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    Mock_SetBootConfigHz(0xEF4019U, 4096U, FLASH_FEAT_QUAD, 200000000U);
    assert(Flash_BootInit(0x00000000U) == FLASH_OK);
    assert(s_mock.spi_speed_hz == 10000000U);

    /* (4) 이 field 는 일부러 checksum window 밖에 있다(덧붙이기만 해서 예전 block 을
     *     유효하게 유지했다). garbage clock 이 깨진 block 인 척하지 않는다는 것을
     *     증명한다: handle 은 여전히 READY 로 올라온다. */
    assert(Flash_GetState() == FLASH_STATE_READY);
    assert(Flash_IsLocked() == true);

    printf("  - 24 MHz applied; absent/absurd values keep 10 MHz and still boot. PASSED.\n\n");
}

/******************************************************************************
 * [TEST 31] SW-030: GigaDevice 용량 커버리지 (16Mb / 32Mb / 64Mb)
 *
 * GD25LQ 계열은 용량이 달라도 command set 이 하나다 — 바뀌는 것은 JEDEC capacity
 * byte 와 배열 크기뿐이다. 이 test 는 table 이 세 용량을 모두 담고 있고, 각각이
 * 부팅하며 SR2 의 QE 를 켜고 선로에는 여전히 3 byte address 로 접근한다는 것을
 * 증명한다.
 ******************************************************************************/
static void Test_GigaDeviceDensities(void)
{
    typedef struct
    {
        uint32_t    jedec;
        FLASH_MODEL model;
        uint32_t    size;
        const char *name;
    } GD_PART;

    static const GD_PART parts[] = {
        { 0xC86015U, FLASH_MODEL_GD25LQ16E, 2U * 1024U * 1024U, "GD25LQ16E" },
        { 0xC86016U, FLASH_MODEL_GD25LQ32E, 4U * 1024U * 1024U, "GD25LQ32E" },
        { 0xC86017U, FLASH_MODEL_GD25LQ64E, 8U * 1024U * 1024U, "GD25LQ64E" }
    };

    printf("[TEST 31] SW-030: GigaDevice 16Mb/32Mb/64Mb density coverage...\n");

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(parts) / sizeof(parts[0])); i++)
    {
        uint8_t buf[16];

        Mock_Reset();
        s_mock.mock_jedec_id = parts[i].jedec;
        Mock_SetStatus(0x1CU, 0x00U, 0x00U);   /* SR1 has BP bits set, QE off */

        assert(Flash_Init() == FLASH_OK);
        assert(g_flash_handle.info != NULL);

        /* 1) the right row was selected, with the right geometry */
        assert(g_flash_handle.info->model_id   == parts[i].model);
        assert(g_flash_handle.info->jedec_id   == parts[i].jedec);
        assert(g_flash_handle.info->flash_size == parts[i].size);
        assert(g_flash_handle.info->addr_bytes == 3U);   /* all three < 16 MB */

        /* 2) same status layout as the 64Mb part: QE = SR2 S9, BP4 = SR1 S6 */
        assert(g_flash_handle.info->qe_reg == 2U);
        assert(g_flash_handle.info->qe_bit == 1U);
        assert((g_flash_handle.info->features & FLASH_FEAT_BP4) != 0U);
        assert((g_flash_handle.info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) != 0U);

        /* 3) QE came up in SR2 while SR1 survived -> the 16-bit 01h ran */
        assert(s_mock.sr1[0] == 0x1CU);
        assert((s_mock.sr2[0] & 0x02U) != 0U);
        assert(s_mock.wrsr1_frame_bytes == 3U);   /* 01h + SR1 + SR2 */

        /* 4) a plain read still puts THREE address bytes on the wire */
        memset(buf, 0, sizeof(buf));
        assert(Flash_Read(0x00000010U, buf, 16U, FLASH_READ_SINGLE,
                          FLASH_READ_UNIT_BYTE, 0U) == FLASH_OK);
        assert(s_xfer.last_first_tx == F_CMD_READ);
        assert(s_xfer.last_tx_len == 4U);         /* 1 opcode + 3 address */

        printf("  - %s: %u KB, 3-byte address, SR2.QE, 16-bit WRSR. PASSED.\n",
               parts[i].name, (unsigned)(parts[i].size / 1024U));
    }

    /* 5) SW-041 계약 변경: table 행이 없던 용량도 이제 "미지원"이 아니다 —
     *    vendor 행 + density byte 가 그것을 설명한다. GD25LQ128E(0xC86018)는
     *    예전에는 거절되었지만 지금은 그냥 동작한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86018U;
    Mock_SetStatus(0x1CU, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_GetInfo()->flash_size == 16UL * 1024UL * 1024UL);
    assert(Flash_GetInfo()->addr_bytes == 3U);    /* exactly 16 MB is still 3-byte */

    /* 6) an unknown VENDOR is still refused - that is the real limit now. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0x9D4018U;             /* ISSI: no vendor row */
    assert(Flash_Init() != FLASH_OK);
    assert(Flash_GetState() == FLASH_STATE_ERROR);
    printf("  - Free density 0xC86018 resolves, unknown vendor 0x9D4018 refused. PASSED.\n\n");
}

/******************************************************************************
 * [TEST 32] SW-035: block protect 는 power cycle 을 견뎌야 한다
 *
 * status register 는 부품 안에 두 벌로 존재한다: power-up 때 적재되는
 * non-volatile copy 와, 05h/35h/15h 가 읽는 volatile copy. 01h frame 은 06h 로
 * 무장했을 때만 non-volatile copy 를 쓰고, 50h 로 무장하면 volatile copy 만 쓴다.
 *
 * SW-035 이전에는 FLASH_FEAT_VSR_WREN 부품의 모든 status write 가 50h 로
 * 무장되었기 때문에, Flash_Protect() 가 test 에서는 맞아 보였지만(mock 이 한 벌만
 * 두었다) 실제 hardware 에서는 다음 power-up 에서 보호가 사라졌다. 이 test 는 두
 * 벌을 모두 model 로 두고 전원을 끊는다.
 ******************************************************************************/
static void Test_ProtectSurvivesPowerCycle(void)
{
    uint32_t vsr_before_protect = 0U;

    printf("[TEST 32] SW-035: block protect persists, QE stays volatile...\n");

    /* (1) GD25LQ64E boots: QE is enabled, but only in the volatile copy. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(s_mock.vsr_wren_count > 0U);          /* QE used the 50h path */
    assert((s_mock.sr2[0] & 0x02U) != 0U);       /* live copy: QE on */
    assert((s_mock.nv_sr2[0] & 0x02U) == 0U);    /* durable copy: untouched */

    Mock_PowerCycle();
    assert((s_mock.sr2[0] & 0x02U) == 0U);       /* QE is volatile by design */

    /* (2) Reboot, then protect BP1 and cut the power again. */
    assert(Flash_Init() == FLASH_OK);
    assert((s_mock.sr2[0] & 0x02U) != 0U);       /* QE re-armed at boot */

    vsr_before_protect = s_mock.vsr_wren_count;
    assert(Flash_Protect(0x02U) == FLASH_OK);    /* mask bit1 -> BP1 */
    assert((s_mock.sr1[0] & 0x08U) != 0U);
    assert(s_mock.vsr_wren_count == vsr_before_protect);  /* no 50h here */
    assert((s_mock.nv_sr1[0] & 0x08U) != 0U);    /* durable copy written */

    /* 이 부품들의 01h frame 은 16-bit 이므로 SR2 도 반드시 program 된다 — 그 순간
     * 살아 있는 QE bit 도 함께 영구화된다. 무해하지만(QE 는 quad command 에만
     * 의미가 있다) 발견이 아니라 이해가 되어야 하므로, 이 assertion 이 그것을
     * 문서로 남긴다. */
    assert((s_mock.nv_sr2[0] & 0x02U) != 0U);

    Mock_PowerCycle();
    assert((s_mock.sr1[0] & 0x08U) != 0U);       /* survives: reloaded from NV */

    /* (3) The release must be durable too. */
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_Unprotect() == FLASH_OK);
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);
    Mock_PowerCycle();
    assert((s_mock.sr1[0] & 0x3CU) == 0x00U);

    /* (4) Winbond W25Q128JW behaves identically (it also has VSR_WREN). */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6018U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_Protect(0x07U) == FLASH_OK);    /* BP0..BP2 */
    assert((s_mock.nv_sr1[0] & 0x1CU) == 0x1CU);
    Mock_PowerCycle();
    assert((s_mock.sr1[0] & 0x1CU) == 0x1CU);

    /* (5) Macronix 에는 volatile 무장 자체가 없다: 06h 가 유일한 선택지이므로,
     * 이 경로는 FLASH_FEAT_VSR_WREN 없이도 계속 동작해야 한다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC22010U;            /* MX25L512 */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_Protect(0x02U) == FLASH_OK);
    assert(s_mock.vsr_wren_count == 0U);
    Mock_PowerCycle();
    assert((s_mock.sr1[0] & 0x08U) != 0U);

    /* (6) 순서: 앞선 주기를 아직 끝내는 중인 부품에 protect 를 걸면 먼저 기다려야
     *     한다. idle 검사가 없으면 WIP=1 동안 06h 무장이 버려지고 01h frame 도
     *     함께 사라진다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    s_mock.busy_reads_remaining = 3U;
    assert(Flash_Protect(0x02U) == FLASH_OK);
    assert((s_mock.sr1[0] & 0x08U) != 0U);

    printf("  - BP survives power-off (GD/Winbond/Macronix); QE re-arms. PASSED.\n\n");
}

/* SW-036: 주소 범위 검사.
 *   칩은 범위 밖 주소를 거절하지 않는다 — 용량을 넘는 상위 주소 비트가 디코드되지
 *   않아 배열 앞쪽으로 되돌아가고(wrap), 호출자는 FLASH_OK를 받는다. 실제 보드에서는
 *   부트로더가 조용히 덮어써지는 경로다. 드라이버가 입구에서 막아야 한다. */
static void Test_AddressRangeRefused(void)
{
    uint8_t buf[16];

    printf("[TEST 33] SW-036: addresses outside the device are refused...\n");

    /* GD25LQ64E = 8 MiB, single die. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86017U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_GetInfo() != NULL);
    assert(Flash_GetInfo()->flash_size == (8U * 1024U * 1024U));

    memset(buf, 0x00, sizeof(buf));

    /* (1) The last byte of the array is inside and must still work. */
    assert(Flash_Read(0x7FFFFFU, buf, 1U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_OK);

    /* (2) First byte past the end, and a read that only *ends* past the end. */
    assert(Flash_Read(0x800000U, buf, 1U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_ERR_PARAM);
    assert(Flash_Read(0x7FFFFEU, buf, 4U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_ERR_PARAM);

    /* (3) write 는 bus 를 건드리기 *전에* 거절된다: address 가 wrap 된 program
     *     command 가 바로 bootloader sector 를 파괴하는 경로다. */
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Write(0x800000U, buf, 4U, FLASH_PROGRAM_SINGLE) == FLASH_ERR_PARAM);
    assert(Flash_Write(0x7FFFFFU, buf, 2U, FLASH_PROGRAM_SINGLE) == FLASH_ERR_PARAM);
    assert(s_xfer.frames == 0U);          /* nothing left the driver */

    /* (4) erase: 대상이 부품 안에서 시작해야 한다. 칩은 erase 주소를 sector 로
     *     내림 정렬하므로 0x7FFFFF 은 정당한 in-range 호출이다. */
    assert(Flash_Erase(0x800000U, FLASH_ERASE_SECTOR) == FLASH_ERR_PARAM);
    assert(Flash_Erase(0x7FFFFFU, FLASH_ERASE_SECTOR) == FLASH_OK);

    /* (5) The check must not be fooled by "addr + len" overflow. */
    memset(&s_xfer, 0, sizeof(s_xfer));
    assert(Flash_Write(0xFFFFFFF0U, buf, 0x20U, FLASH_PROGRAM_SINGLE) == FLASH_ERR_PARAM);
    assert(s_xfer.frames == 0U);
    assert(Flash_Read(0xFFFFFFF0U, buf, 0x20U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_ERR_PARAM);

    /* (6) multi-die: flash_size 는 stack 전체(2 x 32 MiB)이므로, read 는 0x2000000
     *     에서 die 경계를 넘을 수 있지만, stack 전체 다음 byte(0x4000000)는 거절된다. */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6119U;                 /* W25M512JV */
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_GetInfo()->flash_size == (64U * 1024U * 1024U));
    assert(Flash_Read(0x1FFFFF8U, buf, 16U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_OK);   /* across dies */
    assert(Flash_Read(0x3FFFFFFU, buf, 2U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_ERR_PARAM);
    assert(Flash_Read(0x4000000U, buf, 1U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_ERR_PARAM);

    printf("  - 8 MiB part + 2-die 64 MiB stack: ends refused, no SPI frames. PASSED.\n\n");
}

/*============================================================================
 * TEST 34 (SW-042): address mode 는 가정하지 않고 SR3 에서 되읽으며,
 * 4-Byte 부품은 3-Byte mode 로 구동되지 않는다.
 *
 *   W25Q256JV 7.1.5/7.1.6 은 현재 mode 를 SR3.ADS(bit0), power-up mode 를
 *   SR3.ADP(bit1)에 둔다. 중요한 상황은 넷이다:
 *     1. 4-Byte mode 로 power-up (ADP=1, ADS=1)  -> 1/1 로 보고
 *     2. 3-Byte mode 로 power-up (ADP=0, ADS=0)  -> 그래도 쓸 수 있다. driver 가
 *        mode 설정을 무시하는 전용 4-Byte opcode(0x0C/0x12/0x21)를 보내기 때문이다
 *        (6.1.4)
 *     3. 32 MB die 에 누가 3-Byte mode 를 강제 -> 16 MB 위 주소를 조용히 wrap 하는
 *        대신 거절
 *     4. 16 MB 부품에는 ADS/ADP 가 없음       -> 가짜 0/0 이 아니라 UNSUPPORTED
 *============================================================================*/
static void Test_Sr3AddressMode(void)
{
    FLASH_BOOT_CONFIG cfg;
    uint32_t          buf[4];
    uint8_t           ads = 0xFFU;
    uint8_t           adp = 0xFFU;

    printf("[TEST 34] SW-042: SR3 reports ADS/ADP, 3-Byte mode refused on 4-Byte parts\n");

    /* --- 1. W25Q256JV that came up in 4-Byte mode: ADS=1, ADP=1 ------------- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x03U);          /* SR3 = ADP|ADS */
    assert(Flash_Init() == FLASH_OK);
    assert(g_flash_handle.addr_mode == FLASH_ADDR_4BYTE_OPCODE);
    assert(Flash_GetAddrModeState(&ads, &adp) == FLASH_OK);
    assert(ads == 1U);
    assert(adp == 1U);
    printf("  - 4-Byte boot (SR3=0x03): ADS=1 ADP=1, method A opcodes. PASSED.\n");

    /* --- 2. Same part that came up in 3-Byte mode: readable either way ------ */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);          /* SR3 = 0: 3-Byte mode */
    assert(Flash_Init() == FLASH_OK);
    assert(Flash_GetAddrModeState(&ads, &adp) == FLASH_OK);
    assert(ads == 0U);
    assert(adp == 0U);
    /* Above 16 MB the part needs a 4-Byte opcode; the driver sends one anyway. */
    assert(Flash_Read(0x01000010U, buf, 1U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_WORD, 0U) == FLASH_OK);
    assert(s_xfer.last_first_tx == F_CMD_READ_4B);
    assert(s_xfer.last_tx_len == 5U);        /* 1 command + 4 address bytes */
    printf("  - 3-Byte boot (SR3=0x00): still uses 0x%02X + 4 address bytes. PASSED.\n",
           (unsigned)F_CMD_READ_4B);

    /* --- 3. Forced 3-Byte mode on a 32 MB die must be refused --------------- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF4019U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic         = FLASH_BOOT_CFG_MAGIC;
    cfg.jedec_expect  = 0xEF4019U;
    cfg.model         = (uint32_t)FLASH_MODEL_UNKNOWN;
    cfg.features      = (uint8_t)FLASH_FEAT_QUAD;
    cfg.sfc_page_size = SFC_DEFAULT_PAGE_SIZE_4K;
    cfg.addr_mode     = (uint8_t)FLASH_ADDR_3BYTE;
    cfg.crc           = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);
    assert(Flash_BootInit(0x00000000U) == FLASH_ERR_PARAM);
    assert(g_flash_handle.state == FLASH_STATE_ERROR);
    assert(g_flash_handle.info == NULL);
    printf("  - forced 3-Byte on 32 MB: FLASH_ERR_PARAM (no wrap, no FLASH_OK). PASSED.\n");

    /* --- 4. The same forced mode on a 16 MB part stays legal ---------------- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6018U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    cfg.jedec_expect = 0xEF6018U;
    cfg.crc          = Flash_ConfigCrc(&cfg);
    memcpy(s_mock.config_buf, &cfg, sizeof(cfg));
    s_mock.config_len = (uint32_t)sizeof(cfg);
    assert(Flash_BootInit(0x00000000U) == FLASH_OK);
    assert(g_flash_handle.addr_mode == FLASH_ADDR_3BYTE);
    printf("  - forced 3-Byte on 16 MB: accepted, as it must be. PASSED.\n");

    /* --- 5. A part with no ADS/ADP says so instead of inventing bits -------- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xEF6018U;
    Mock_SetStatus(0x00U, 0x02U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    ads = 0xEEU;                          /* sentinels: must survive untouched */
    adp = 0xEEU;
    assert(Flash_GetAddrModeState(&ads, &adp) == FLASH_ERR_UNSUPPORTED);
    assert(ads == 0xEEU);
    assert(adp == 0xEEU);
    printf("  - 16 MB part without SR3 bits: FLASH_ERR_UNSUPPORTED. PASSED.\n\n");
}

/******************************************************************************
 * [TEST 35] SW-043: GigaDevice 3V 와 1.8V 부품은 WRSR frame 을 공유하지 않는다
 *
 * 두 계열 모두 QE 를 SR2 bit 1 에, BP4 를 SR1 bit 6 에 두고 density byte 도
 * 같아서, 예전의 vendor 단위 "16-bit WRSR" flag 는 모든 GigaDevice 부품에 맞아
 * 보였다. 그것이 맞는 것은 GD25LQ 뿐이다:
 *
 *   GD25Q32E  Rev1.5 7.4 - 01H / 31H / 11H 는 각각 status byte 하나를 나르고,
 *                          "CS# must be driven high after the eighth bit ...
 *                          Otherwise, the WRSR command is not executed."
 *   GD25LQ32E Rev1.4 7.4 - 16 bit 가 필수이고, 8 bit 에서 멈추면 SR2 가 지워진다.
 *
 * 그래서 3-byte 01h frame 을 받은 GD25Q32E(0xC84016)는 QE 를 절대 켜지 못한다:
 * init read-back 이 FLASH_ERR_WRITE_FAIL 로 실패하고 부품을 쓸 수 없다.
 * 이 둘을 구분해 주는 것은 memory-type byte(0x40 vs 0x60) 뿐이다.
 ******************************************************************************/
static void Test_GdWrsrFamilySplit(void)
{
    uint8_t buf[8];

    printf("[TEST 35] SW-043: GigaDevice 3V writes one status byte per frame...\n");

    /* --- 1. GD25Q32E (3V): QE must be turned on through 31h, one byte wide --- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC84016U;             /* GD25Q32E: C8 40 16 */
    Mock_SetStatus(0x1CU, 0x00U, 0x00U);          /* BP bits set, QE off */
    assert(Flash_Init() == FLASH_OK);
    assert(g_flash_handle.info != NULL);
    assert(g_flash_handle.info->flash_size == 4UL * 1024UL * 1024UL);
    assert(g_flash_handle.info->addr_bytes == 3U);

    /* The family flags that ARE shared must survive the split. */
    assert((g_flash_handle.info->features & FLASH_FEAT_BP4) != 0U);
    assert((g_flash_handle.info->features & FLASH_FEAT_VSR_WREN) != 0U);
    assert((g_flash_handle.info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) == 0U);

    /* QE went in through 31h with exactly one data byte. */
    assert(s_mock.last_wrsr_opcode == F_CMD_WRSR2);
    assert(s_mock.last_wrsr_tx_len == 2U);        /* opcode + SR2 */
    assert((s_mock.sr2[0] & 0x02U) != 0U);        /* QE now on */
    assert(s_mock.sr1[0] == 0x1CU);               /* SR1 untouched */
    assert(g_flash_handle.qe_enabled);
    printf("  - GD25Q32E boot: QE via %02Xh, 2-byte frame, SR1 untouched. PASSED.\n",
           (unsigned)F_CMD_WRSR2);

    /* --- 2. Protect on the same part: 01h must stay one data byte wide ------ */
    assert(Flash_Protect(0x02U) == FLASH_OK);     /* mask bit1 -> BP1 */
    assert(s_mock.last_wrsr_opcode == F_CMD_WRSR1);
    assert(s_mock.last_wrsr_tx_len == 2U);        /* opcode + SR1 only */
    assert((s_mock.sr1[0] & 0x08U) != 0U);        /* BP1 latched */
    assert((s_mock.nv_sr1[0] & 0x08U) != 0U);     /* durable copy too */
    assert((s_mock.sr2[0] & 0x02U) != 0U);        /* QE survived the SR1 write */
    assert(s_mock.wrsr1_frame_bytes == 2U);
    printf("  - protect: 01h carries SR1 alone, QE untouched. PASSED.\n");

    /* --- 3. Still a 3-byte address part ------------------------------------ */
    memset(buf, 0, sizeof(buf));
    assert(Flash_Read(0x00000010U, buf, 8U, FLASH_READ_SINGLE,
                      FLASH_READ_UNIT_BYTE, 0U) == FLASH_OK);
    assert(s_xfer.last_first_tx == F_CMD_READ);
    assert(s_xfer.last_tx_len == 4U);             /* opcode + 3 address bytes */

    /* --- 4. GD25LQ32E (1.8V) keeps the coupled 16-bit frame ---------------- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86016U;
    Mock_SetStatus(0x1CU, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert((g_flash_handle.info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) != 0U);
    assert(s_mock.last_wrsr_opcode == F_CMD_WRSR1);
    assert(s_mock.last_wrsr_tx_len == 3U);        /* opcode + SR1 + SR2 */
    assert((s_mock.sr2[0] & 0x02U) != 0U);
    assert(s_mock.sr1[0] == 0x1CU);               /* echoed back unchanged */
    printf("  - GD25LQ32E boot: 01h carries SR1+SR2 in one 16-bit frame. PASSED.\n");

    /* --- 5. The split follows the TYPE byte, not the density --------------- */
    Mock_Reset();
    s_mock.mock_jedec_id = 0xC86018U;             /* GD25LQ128E */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);
    assert(s_mock.last_wrsr_tx_len == 3U);        /* still 16-bit at 16 MB */

    Mock_Reset();
    s_mock.mock_jedec_id = 0xC84116U;             /* unknown GD type 0x41 */
    Mock_SetStatus(0x00U, 0x00U, 0x00U);
    assert(Flash_Init() == FLASH_OK);             /* 4 MB, 3-byte, one-byte WRSR */
    assert((g_flash_handle.info->features & FLASH_FEAT_STATUS_2BYTE_WRITE) == 0U);
    assert(s_mock.last_wrsr_tx_len == 2U);
    printf("  - LQ stays 16-bit at 16 MB; unknown type 0x41 defaults to 8-bit. PASSED.\n\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: show progress even on assert */

    printf("================================================================\n");
    printf(" Generic SPI NOR Flash Driver Verification Suite (ARM Cortex-M0)\n");
    printf("================================================================\n\n");

    Test_TableLookup();
    Test_InitAndProbe();
    Test_GD25LQ64E_StatusWrite();
    Test_PageBoundarySplitting();
    Test_MultiDieHandling();
    Test_SFCDMAReadSequence();
    Test_BootConfigAndLock();
    Test_BootFamilyMatch();
    Test_BootNoConnect();
    Test_MultiDiePerDieConfig();
    Test_CRCAccumulate();
    Test_DirectSPIQuadGuard();
    Test_ProtectUnprotect();
    Test_WriteKeepsProtect();
    Test_AddrModeOpcodes();
    Test_InitNoReset();
    Test_TableReadCfgGuard();
    Test_VsrWriteEnable();
    Test_VendorDensityResolution();
    Test_CrcAlgoBootSelect();
    Test_EasyApi();
    Test_DelayUsConversion();
    Test_VsrWrenNoWel();
    Test_DummyCfgPerModel();
    Test_BufferedXferAndSharedGeometry();
    Test_NoConnectRefusesDataPath();
    Test_GdCoupledWriteKeepsQe();
    Test_Bp4VersusQeBit();
    Test_BusyGateAndStatusReadBack();
    Test_BootConfigCarriesSpiClock();
    Test_GigaDeviceDensities();
    Test_ProtectSurvivesPowerCycle();
    Test_AddressRangeRefused();
    Test_Sr3AddressMode();
    Test_GdWrsrFamilySplit();

    printf("================================================================\n");
    printf(" ALL 35 VERIFICATION TEST SUITES PASSED SUCCESSFULLY (100%%)!\n");
    printf("================================================================\n");

    return 0;
}
