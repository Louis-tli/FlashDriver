# Generic SPI NOR Flash Driver for ARM Cortex-M0

ARM Cortex-M0 (`arm-cm0`) 및 `armcc` (ARM Compiler 5 / RVCT) 환경에 최적화된 초경량 고성능 SPI NOR Flash Driver입니다.
TCON SoC의 **SFC (Serial Flash Controller) Memory-Mapped DMA** 고속 읽기와 **SPI Command** 제어 경로를 결합한 하이브리드 아키텍처를 지원합니다.

---

## 🚀 빠른 시작 — 최대한 단순한 사용법

**최종 사용자는 `flash_easy.h`만 포함하고 아래 함수만 쓰면 됩니다.**
읽기 모드·단위·옵션·정렬은 모두 자동 처리됩니다.

```c
#include "flash_easy.h"

void app_start(void)
{
    /* 0) MCU 클록 설정 (기본 108MHz — 50MHz 보드는 50000000U 지정) */
    Flash_SysClockSet(108000000U);

    /* 1) 부팅 시 한 번: 칩 자동 감지 + 구성 + 잠금 */
    if (Flash_BootInit(0x00000000U) != FLASH_OK) {
        return;                          /* 칩 없음/불일치 → 안전 정지 */
    }

    /* 2) 이후엔 이렇게 간단하게 */
    uint8_t buf[256];
    Flash_ReadBytes(0x1000U, buf, sizeof(buf));     /* 읽기 (주소·길이 제약 없음) */

    Flash_EraseRange(0x2000U, 4096U);               /* 영역 지우기 */
    Flash_WriteBytes(0x2000U, buf, sizeof(buf));    /* 쓰기 (페이지 분할 자동) */
}
```

| 함수 | 역할 |
|------|------|
| `Flash_BootInit(addr)` | 부팅 1회 초기화 (자동 감지·구성·잠금) |
| `Flash_ReadBytes(addr, buf, len)` | `len`바이트 읽기 — 임의 주소/길이/정렬 |
| `Flash_WriteBytes(addr, data, len)` | `len`바이트 쓰기 — 페이지 분할·보호 해제 자동 |
| `Flash_EraseRange(addr, len)` | 구간 내 모든 섹터 지우기 |
| `Flash_EraseChip()` | 전체 칩 지우기 |
| `Flash_SizeBytes()` / `Flash_SectorSize()` | 용량·섹터 크기 조회 |
| `Flash_SysClockSet(hz)` / `Flash_DelayUs(us)` | MCU 클록 설정(108/50MHz) + 시간 기반 지연 |

> 고급 제어(특정 읽기 모드, CRC, SFC 튜닝, 블록 보호)가 필요하면 `flash.h`의 전체 API를 사용하세요.

---

## 🌟 주요 특징 (Key Features)

- **하이브리드 듀얼 패스 (Hybrid Dual-Path Architecture)**:
  - **SPI Command Path**: JEDEC ID (0x9F), Status Register 읽기/쓰기 (0x05/0x35/0x15/0x01), WREN (0x06), Page Program (0x02/0x32), Erase (0x20/0x52/0xD8/0xC7), Software Die Select (0xC2) 제어.
  - **SFC Data Path**: 대용량 고속 Read 전용. Memory-Mapped 주소를 통해 2KB 또는 4KB 단위의 DMA Transfer로 고속 데이터 수신 및 하드웨어 CRC32 연산 옵션 지원.
- **SFC 안전 Read 시퀀스 내장**:
  - Quad I/O Read 전 `SFC Single 모드 설정` ➔ `SPI Status BUSY=0 폴링 완료` ➔ `SFC Quad I/O 모드 재설정` ➔ `DMA 전송` 시퀀스 자동화.
- **작은 ROM 풋프린트 (SW-017 실측)**:
  - `-Oz` 전체 드라이버 **5.4 KB** (text 4.5 + rodata 0.76 + data/bss 0.1), easy API만 쓸 때 `--gc-sections` 4.9 KB, `-flto`까지 쓰면 **3.6 KB**.
  - Cortex-M0 나눗셈 명령 부재를 고려해 `/`·`%`를 시프트/마스크로 대체 → libgcc 소프트웨어 나눗셈기(`__aeabi_uidiv`) 미링크.
  - 모델 정보는 ROM(`.rodata`) 상주, SRAM은 가벼운 핸들 1개만 사용. 동적 메모리 할당(`malloc`) 완전 배제.
- **다양한 읽기 단위 지원 (Read Access Units - Byte / Halfword / Word)**:
  - `FLASH_READ_UNIT_BYTE` (8-bit), `FLASH_READ_UNIT_HALFWORD` (16-bit), `FLASH_READ_UNIT_WORD` (32-bit, 기본값) 지원.
  - 기본값(0 지정 시)은 32-bit Word 단위로 자동 해석되어 4바이트 정렬 검사 및 SFC ACCESS_UNIT 설정 수행.
- **Cortex-M0 (`arm-cm0`) & C99 표준 호환**:
  - 현대적 C99 표준 준수 (`<stdbool.h>`, 지정 초기화자 `Designated Initializers`, 루프 및 로컬 변수 스코프 국소화, `static inline` 헬퍼 함수).
  - ARM Compiler 5 (`armcc --c99`), ARM Compiler 6 (`armclang`), GCC (`gcc -std=c99`) 완벽 호환.
  - Word(4-Byte)/Halfword(2-Byte) 단위별 정렬 검사로 Cortex-M0 Unaligned Access HardFault 원천 차단.
- **256-Byte Page Boundary 자동 분할**:
  - 페이지 경계를 걸치는 Program 요청 시 256바이트 단위로 자동 분할 전송.

---

## 💾 지원 Flash 메모리 모델 (Supported Models)

| 모델명 | 제조사 | JEDEC ID | 용량 | 주소 모드 | 특이사항 |
|---|---|---|---|---|---|
| **MX25L512** | Macronix | `0xC22010` / `0xC2201A` | 64KB / 64MB | 3B / 4B | SR1 bit 6 QE, 1-byte WRSR |
| **W25M512** | Winbond | `0xEF6119` / `0xEF7119` | 64MB (2x32MB) | 4B | SpiStack 2-Die, Software Die Select (`0xC2`) 자동 제어 |
| **W25Q128JW** | Winbond | `0xEF6018` | 16MB | 3B | 1.8V 저전력 규격, Single / Quad Page Program |
| **W25Q256JV** | Winbond | `0xEF4019` | 32MB | 4B | 4-Byte Address Mode (`0xB7`) 자동 진입 |
| **W25Q512JV** | Winbond | `0xEF4020` | 64MB | 4B | 4-Byte Address Mode (`0xB7`) 자동 진입 |
| **GD25LQ64E** | GigaDevice | `0xC86017` | 8MB | 3B | SR1+SR2 2-Byte 동시 쓰기 (`0x01`) 지원 |

---

## 📁 파일 구성 (File Structure)

```text
├── flash_easy.h     # ⭐ 간편 API (최종 사용자용 — ReadBytes/WriteBytes/EraseRange 등)
├── flash_easy.c     # 간편 API 구현 (모드·단위·옵션 자동 선택)
├── flash_delay.h    # 시간 기반 지연 (Flash_DelayUs — MCU 클록 108/50MHz 대응)
├── flash_delay.c    # µs→루프 환산 + 바쁜 대기 구현 (SW-014)
├── flash.h          # 공개 드라이버 API, FLASH_READ_UNIT, FLASH_INFO 및 FLASH_HANDLE 정의
├── flash.c          # 드라이버 코어 엔진 (Init, Read, Program, Erase, Die Select 등)
├── flash_table.h    # Flash 모델 검색 인터페이스, FLASH_IDX 순서 enum 및 테이블 선언
├── flash_table.c    # 7종 Flash 메모리 ROM 테이블 (FLASH_IDX 순서, C99 Designated Initializer)
├── flash_cmd.h      # Read/Program/Erase 명령 + 3B/4B 주소폭별 opcode 선택 인터페이스
├── flash_cmd.c      # 명령 엔진 (주소폭별 opcode 분기, Enter-4B Method B 지원)
├── flash_crc.h      # 소프트웨어 CRC/체크섬 알고리즘 인터페이스
├── flash_crc.c      # CRC8 / CRC16 / CRC16-CCITT / CRC32 / CHECKSUM 구현
├── flash_sfc.h      # TCON SFC IP 하드웨어 레지스터 맵(ACCESS_UNIT 포함) 및 DMA API 헤더
├── flash_sfc.c      # TCON SFC IP 드라이버 구현 (Single/Quad 모드 전환, 단위별 2K/4K DMA Read)
├── flash_spi.h      # Low-Level SPI 인터페이스 + 버퍼 전송(FLASH_SPI_XFER) 정의
├── flash_spi.c      # Low-Level SPI 드라이버 (Transact 엔진 + HW 추상화 스텁)
├── flash_xfer.h     # 전송 지오메트리(단위/크기/정렬) 공용 인터페이스 (SW-016)
├── flash_xfer.c     # ReadUnit 해석 + 32bit 오버플로 가드 + 정렬 검사 (공용)
└── test_flash.c     # 전 기능 시뮬레이션 및 C99 표준 검증 테스트 슈트 (25 suites)
```

---

## 🚀 빌드 및 검증 (Build & Test)

GCC (MinGW / LLVM) 또는 ARM Compiler (ARMCC/ARMCLANG)를 통해 C99 표준으로 빌드하고 테스트를 실행할 수 있습니다:

```bash
# GCC 환경에서 C99 엄격 모드로 컴파일
gcc -std=c99 -Wall -Wextra -pedantic -I. flash.c flash_table.c flash_spi.c flash_sfc.c flash_cmd.c flash_crc.c flash_delay.c flash_easy.c flash_xfer.c test_flash.c -o test_flash.exe

# 검증 테스트 실행
./test_flash.exe
```

> **ARMCC (ARM Compiler 5) 컴파일 시 플래그**: `armcc --c99 -c flash.c`
> **ARMCLANG (ARM Compiler 6) 컴파일 시 플래그**: `armclang --target=arm-arm-none-eabi -mcpu=cortex-m0 -std=c99 -c flash.c`

### 테스트 통과 항목 (25 suites):
1. **Table Lookup**: 7종 Flash JEDEC ID 및 파라미터 매칭, `FLASH_IDX` 순서 일치
2. **Init & Bus Probing**: 플로팅 버스 (0x000000 / 0xFFFFFF) NO_CONNECT 판별 및 자동 QE 활성화
3. **GD25LQ64E Status Write**: 2-Byte Status Register 동시 쓰기 시퀀스
4. **Page Boundary Splitting**: 256바이트 경계 걸침 시 3회 분할 쓰기
5. **Multi-Die Switching**: W25M512 32MB 경계 주소 접근 시 `0xC2` Software Die Select 자동 전환
6. **Flash Read Units & SFC DMA Read**: Byte/Halfword/Word 단위별 읽기·정렬 검증, SFC DMA 시퀀스
7. **Boot Config & Lock**: 부팅 설정블록 → 구성 → 잠금(불변) + CRC 변조 거부
8. **Family Matching**: 제품군(제조사+Type) 일치 허용 / 이종 거부
9. **No-Connect Fail-Safe**: ID 무응답 시 `NO_CONNECT` 안전 상태
10. **Per-Die QE**: 멀티 다이(QE) 모든 다이에 적용
11. **CRC Start/Continue + Algorithms**: 시작/계속 체이닝, crc8/crc32/checksum 선택
12. **Direct SPI Guard**: Direct SPI는 Single 강제, Dual/Quad 거부
13. **Protect/Unprotect**: 블록 보호 BP 비트 설정/해제
14. **Write Auto-Clears BP**: 쓰기 시 BP 자동 해제(무음 실패 방지)
15. **3B/4B Opcode Selection**: 주소폭별 opcode 자동 선택 + Method B(0xB7)
16. **Init Option (NO_RESET)**: 소프트웨어 리셋 생략(fast boot) 옵션
17. **Read Config Guard**: 제품별 DUAL_IO/QUAD_IO dummy 값 보존
18. **WREN 50h vs 06h**: 지원 칩은 50h(VSR), 미지원은 06h 폴백 — 단, 50h는 WEL 미설정(데이터시트 규칙)
19. **FLASH_IDX Access**: `g_flash_table[FLASH_IDX_x]` 인덱스 접근 패턴
20. **CRC Algorithm from Boot Config**: 설정블록 `crc_algo`로 알고리즘 선택
21. **Easy API Round-Trip**: `Flash_ReadBytes/WriteBytes/EraseRange/EraseChip` 간편 API 검증
22. **Time-based Delay (SW-014)**: µs→루프 환산(108/50MHz), 올림 처리, 무효 클록 가드
23. **VSR WREN 50h WEL-Free (SW-013 WEL)**: 50h는 WEL을 올리지 않음(데이터시트 준수) — WEL=0 상태의 휘발성 WRSR 성공 + 명령 삽입 시 50h 무효화 규칙
24. **Per-Model Dummy Config (SW-013 DUMMY)**: 7종 모델별 dummy_last/dummy_value 검증 — Winbond 0xFF(Fxh), MX/GD 0x00, mode_byte 0x00
25. **Buffered One-Frame Transfer (SW-016)**: 읽기 1프레임(tx_len=cmd+addr+dummy, rx_len=데이터), Fast Read 더미 헤더 포함, 페이지 프로그램 데이터 페이즈 동일 프레임, SPI/SFC 공용 지오메트리(정렬·오버플로) 일치, 실하드웨어 SFC 경로에서 DMA 버퍼/CRC_RES 무변경

---

## 📦 코드 크기 (ROM) — SW-017

Cortex-M0에는 **나눗셈 명령이 없습니다.** `uint32_t / 값` 한 줄이 libgcc의 소프트웨어 나눗셈기(`__aeabi_uidiv`)를 이미지에 통째로 끌어옵니다.
SW-017은 **ROM을 줄이고 RAM(스택·전역 몇 바이트)은 조금 더 쓰는** 방향으로 정리했습니다.

| 적용 | 내용 |
|------|------|
| 나눗셈 제거 | page(256)/sector(4K)/die(32M)가 모두 2의 거듭제곱 → `>>`·`&` 로 대체 |
| libgcc 제거 | `__aeabi_uidiv`·`__aeabi_uidivmod`·`__aeabi_lmul` 참조 0 (실측) |
| 중복 통합 | Boot/Init 공통 경로, Protect/Unprotect, 칩 소거 분기 |
| 지연 클램프 | `us × loops_per_us` 오버플로를 **나눗셈 없이** 3초 클램프로 처리 |

측정 (Cortex-M0 Thumb, lld, libgcc 제외):

| 빌드 | 이전 | 이후 |
|------|------|------|
| 전체 함수 유지 (`-Os`) | 5,860 B | **5,608 B** |
| 전체 함수 유지 (`-Oz`) | 5,558 B | **5,366 B** |
| easy API만 + `--gc-sections` (`-Oz`) | 5,074 B | **4,920 B** |
| 위 + `-flto` | — | **3,552 B** |

크기 우선 빌드:

```bash
clang --target=arm-none-eabi -mcpu=cortex-m0 -mthumb \
      -Oz -flto -std=c99 -ffunction-sections -fdata-sections -I. -c <각 파일>.c
# 링크 단계에서 -Wl,--gc-sections 추가
```

> `-Oz`·`-flto`는 동작을 바꾸지 않습니다(25/25 테스트 통과).  
> 더 줄이려면 **기능을 줄이는** 선택이 필요합니다: 모델명 문자열 제거(−125 B) · CRC 알고리즘 고정(−170 B) · `flash_easy` 제외(−296 B) · 지원 모델 축소(모델당 −96 B).

---

## 🔌 SPI 전송 방식 (SW-016)

명령·주소·데이터를 바이트 단위로 각각 호출하지 않고, **한 번의 /CS 프레임**으로 묶어 전송합니다.
`Flash_SPI_XFER`에 단계별 버퍼와 길이만 채워 `Flash_SPI_Transact()`에 넘기면 됩니다.

```c
uint8_t hdr[FLASH_CMD_HDR_MAX];
const uint32_t hdr_len = Flash_CmdBuildHeader(hdr, F_CMD_READ, 0x1000U, 3U, NULL);

const FLASH_SPI_XFER xfer = {
    .tx_buf = hdr,  .tx_len = hdr_len,      /* 1단계: cmd + addr (+ dummy) */
    .rx_buf = buf,  .rx_len = 256U          /* 4단계: 데이터 수신            */
};
Flash_SPI_Transact(&xfer);                  /* CS Low → TX → RX → CS High  */
```

| 단계 | 필드 | 용도 |
|------|------|------|
| 1 | `tx_buf` / `tx_len` | opcode + 주소 + 더미 (1라인) |
| 2 | `data_buf` / `data_len` | Page Program 데이터 (1라인, 02h) |
| 3 | `quad_buf` / `quad_len` | Quad Page Program 데이터 (4라인, 32h) |
| 4 | `rx_buf` / `rx_len` | 읽기 데이터 수신 (1라인) |

- 펌웨어 BSP는 `Flash_SPI_Transact()`만 재정의하면 됩니다(weak 기본 구현은 바이트 루프로 폴백).
- 버퍼를 이어붙이거나 복사하지 않습니다 — 단계별 포인터를 그대로 레지스터에 넣습니다.
- 읽기 단위 해석·정렬 검사·32bit 오버플로 가드는 `flash_xfer.c` 한 곳에서 처리하며, SPI 경로와 SFC 경로가 동일한 코드를 사용합니다.

---

## 🧪 시뮬레이션 / 실하드웨어 구분 (SW-015)

`g_p_sfc_regs`가 내장 시뮬레이션 블록(`s_sfc_sim_regs`)을 가리킬 때만 시뮬레이션 동작(DMA_DONE 강제, 수신 버퍼 0xFF 채움, CRC 누산)이 실행됩니다.
실제 SFC IP 레지스터를 지정하면 하드웨어가 DMA·CRC를 담당하고 소프트웨어는 레지스터 설정과 완료 대기만 수행합니다.

---

## ⏱ 시간 기반 지연 (SW-014)

플래시 데이터시트의 대기 시간은 **절대 시간(µs)** 으로 규정되지만, 기존 루프 대기는 MCU 클록에 따라 실제 시간이 변했습니다. SW-014부터 `Flash_DelayUs(µs)`로 마이크로초 단위 지연을 지정하면, 설정된 MCU 클록에 맞춰 루프 수를 자동 환산합니다.

| 설정 | 값 |
|------|-----|
| 기본 클록 | `FLASH_SYS_CLK_HZ` = 108MHz (컴파일 시 `-D`로 변경 가능) |
| 런타임 변경 | `Flash_SysClockSet(108000000U)` 또는 `Flash_SysClockSet(50000000U)` |
| 환산 기준 | 루프 1회 ≈ `FLASH_DELAY_CYCLES_PER_LOOP`(기본 3) 사이클 — **올림 처리**로 실제 지연이 요청보다 짧아지지 않음 |

```c
Flash_SysClockSet(50000000U);   /* 50MHz 보드인 경우 */
Flash_DelayUs(100);             /* 최소 100µs 대기 */
```

- 108MHz: 1µs = 36 loops / 50MHz: 1µs = 17 loops (올림)
- 최대 지연: `FLASH_DELAY_MAX_US` = **3초** (초과 요청은 클램프 — 나눗셈 없는 오버플로 방지, SW-017)
- 그 외 클록: 108MHz 기준 타이밍으로 폴백 — 느린 코어에서는 지연이 더 길어질 뿐, 짧아지지 않음
- `Flash_DelayLoops(count)`는 기존 루프 대기 API (하위 호환 유지)

---

## 🔧 모델별 더미 설정 (SW-013 DUMMY)

TCON SFC의 전송 순서는 **CMD → ADDR → DUMMY_LAST(첫 더미 = M7-0 모드 바이트 자리) → DUMMY_VAL... → DATA**입니다 (오너 확인). 이에 따라 모델별로 첫 더미 값을 다르게 설정합니다:

| 계열 | dummy_last (첫 더미) | 근거 |
|------|-----|------|
| Winbond (W25Q128JW/256JV/512JV, W25M512) | `0xFF` | 데이터시트 주석 "The first dummy is M7-M0 should be set to Fxh" |
| GigaDevice (GD25LQ64E) | `0x00` | M7-0 ≠ Axh면 정상 동작 |
| Macronix (MX25L512/51245G) | `0x00` | 모드 바이트 없음 (더미 don't care) |

- 나머지 더미 바이트(dummy_value)는 전 계열 `0x00` (don't care)
- `mode_byte`는 전 모델 `0x00` — M7-0은 DUMMY_LAST가 전송 (과거 0x20은 Winbond Read Command Bypass 진입 위험)
