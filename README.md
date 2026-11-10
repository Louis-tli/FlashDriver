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
  - `-Oz` 전체 드라이버 **5.4 KB**, easy API만 쓸 때(`--gc-sections`) 5.0 KB, `-flto`까지 쓰면 **3.6 KB**.
  - 모델명 문자열 제거 + 중복 경로 통합(Boot/Init·Protect/Unprotect·칩 소거)으로 감축. 나눗셈은 가독성을 위해 그대로 사용합니다(프로젝트의 다른 코드가 이미 libgcc 나눗셈기를 링크).
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
| **GD25LQ32E** | GigaDevice | `0xC86016` | 4MB | 3B | 64Mb와 동일 명령·상태 구조 (SW-030) |
| **GD25LQ16E** | GigaDevice | `0xC86015` | 2MB | 3B | 64Mb와 동일 명령·상태 구조 (SW-030) |

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
├── flash_table.c    # 9종 Flash 메모리 ROM 테이블 (FLASH_IDX 순서, C99 Designated Initializer)
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
└── test_flash.c     # 전 기능 시뮬레이션 및 C99 표준 검증 테스트 슈트 (31 suites)
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

### 테스트 통과 항목 (31 suites):
1. **Table Lookup**: 9종 Flash JEDEC ID 및 파라미터 매칭, `FLASH_IDX` 순서 일치
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
24. **Per-Model Dummy Config (SW-013 DUMMY)**: 9종 모델별 dummy_last/dummy_value 검증 — Winbond 0xFF(Fxh), MX/GD 0x00, mode_byte 0x00
25. **Buffered One-Frame Transfer (SW-016)**: 읽기 1프레임(tx_len=cmd+addr+dummy, rx_len=데이터), Fast Read 더미 헤더 포함, 페이지 프로그램 데이터 페이즈 동일 프레임, SPI/SFC 공용 지오메트리(정렬·오버플로) 일치, 실하드웨어 SFC 경로에서 DMA 버퍼/CRC_RES 무변경
26. **NO_CONNECT Data-Path Gate (SW-018)**: 동작하던 칩이 사라진 뒤 재초기화로 NO_CONNECT가 되면 Read/Write/Erase/Protect가 모두 `FLASH_ERR_NO_CONNECT`로 거부되고 **SPI 프레임 0개** (버스에 아무것도 나가지 않음)
27. **GD Coupled Status Write (SW-019)**: GD25LQ64E는 `01h`가 고정 16비트 명령 — SR1을 바꾸는 모든 경로가 SR2를 읽어 그대로 되돌려 보내는 3바이트 프레임을 쓰고, 쓰기 후 SR1·SR2를 **둘 다 read-back 검증**. QE가 조용히 꺼지지 않음
28. **BP4 vs QE Shared Bit (SW-019)**: GD25LQ64E의 S6=BP4(다섯 번째 보호 비트)를 `Flash_Protect(0x10)`으로 설정/해제하고, MX25L 계열에서는 같은 비트(QE)를 **건드리지 않음**
29. **BUSY Gate + WRSR Read-Back (SW-020)**: WEL=1·BUSY=1이 동시에 보이는 구간에서 `Flash_WriteEnable()`이 기다렸다가 성공하는지, 그리고 조용히 사라진 WRSR을 read-back이 `FLASH_ERR_WRITE_FAIL`로 잡는지
30. **부팅 클럭은 설정 블록이 정한다 (SW-021)**: `spi_hz`=24 MHz면 정확히 그 값으로 상승, 24 B 구 블록(0xFFFF_FFFF)이나 200 MHz 같은 비정상 값이면 **10 MHz 유지 + 정상 부팅**
31. **GigaDevice 밀도 3종 (SW-030)**: GD25LQ**16E/32E/64E**를 같은 테스트로 돌려 — 선택된 행·`flash_size`·`addr_bytes`=3, SR2.QE / SR1.BP4 / 16비트 `01h`(3바이트 프레임) / 3바이트 주소(총 4바이트 송신)까지 확인하고, 표에 없는 `0xC86018`은 여전히 거부

---

## 🛡 상태 게이트 (SW-018)

`Flash_Read` / `Flash_Write` / `Flash_Erase` / `Flash_Protect` / `Flash_Unprotect`는 **`FLASH_STATE_READY`일 때만** 버스를 건드립니다.

| 상태 | 반환 | 의미 |
|------|------|------|
| `READY` | 정상 수행 | 초기화 완료 |
| `NO_CONNECT` | `FLASH_ERR_NO_CONNECT` | 칩 무응답 (재초기화로 판별) |
| `ERROR` | `FLASH_ERR_HARDWARE` | ID 불일치·설정 오류 등 |
| `UNINIT` | `FLASH_ERR_PARAM` | 초기화 미수행 |

> `info != NULL`만 검사하면 **부족합니다**: 한 번 초기화에 성공한 뒤 칩이 사라지면 핸들에는 이전 모델이 남아 있어, 예전 코드는 죽은 버스로 명령을 내보내고 0xFF를 정상 데이터처럼 돌려줬습니다. 이제는 상태를 먼저 확인하고 즉시 거부합니다.
> 저수준 함수(`Flash_BasicRead`, `Flash_Reset`, 상태 폴링)는 초기화 과정에도 필요하므로 게이트를 적용하지 않습니다.

---

## 🔐 상태 레지스터 결합 쓰기 (SW-019)

GD25LQ64E에는 **`31h`(SR2 단독 쓰기)가 없습니다.** SR2에 닿는 유일한 명령이 `01h`이고, 이 명령은
**항상 16비트 고정**입니다 — 첫 데이터 바이트가 SR1, 두 번째가 SR2입니다.

데이터시트 7.4의 경고:

> *"If CS# is driven high after the eighth bit of the data byte, the QE and CMP bits will be cleared to 0."*

즉 **1바이트로 `01h`를 보내면 SR1은 써지지만 QE가 꺼집니다.** 이때 소프트웨어는 아무 오류도 못 봅니다
(핸들의 `qe_enabled`는 `true`로 남아 있고, 다음 quad 읽기부터 쓰레기 데이터가 나옵니다).
`Flash_Protect()`를 한 번 호출하거나, BP가 걸린 부품에 `Flash_Write()`를 하면 그 순간 발생합니다
(공장 기본값 BP=0에서는 "변경 없음"으로 조기 반환되어 쓰기가 일어나지 않으므로 증상이 숨습니다).

수정 후 동작:

| 호출 | 실제 전송 | 비고 |
|------|-----------|------|
| `Flash_WriteStatus(1, v)` | `01h + v + (읽은 SR2)` | SR1만 교체, QE는 그대로 반영 |
| `Flash_WriteStatus(2, v)` | `01h + (읽은 SR1) + v` | 기존 동작 유지 |
| `Flash_WriteStatus(3, v)` | 없음 → `FLASH_ERR_UNSUPPORTED` | 이 부품에 `11h` 없음 |

쓰기 후에는 **SR1과 SR2를 둘 다 다시 읽어** 쓰기 가능 비트(SR1 `0xFC`, SR2 `0x7B`)를 비교합니다.
한 바이트라도 어긋나면 `FLASH_ERR_WRITE_FAIL`입니다 — 조용히 넘어가지 않습니다.

### S6은 부품마다 의미가 다릅니다

| 부품 | SR1 bit6 | 처리 |
|------|----------|------|
| GD25LQ64E | **BP4** (다섯 번째 블록 보호 비트) | `FLASH_FEAT_BP4` — `Flash_Protect(0x10)`로 설정, `Flash_Unprotect()`로 해제 |
| MX25L 계열 | **QE** (Quad Enable) | 플래그 없음 — Protect/Unprotect가 절대 건드리지 않음 |

플래그 없이 비트6을 마스크에 넣으면 MX 계열에서 quad 모드가 죽고, 반대로 BP4를 무시하면
`Flash_Unprotect()`가 성공을 반환했는데도 쓰기가 막히는 무음 실패가 됩니다.

> 비용: ROM **+226 B** (`-Oz` 5,294 → 5,520 B). 테스트 2개 추가로 28 suites.
> 시뮬레이터도 실제 실리콘처럼 "8비트에서 CS 상승 → QE/CMP 클리어"를 모사하도록 고쳤기 때문에,
> 수정 전 코드에서는 TEST 27/28이 실제로 실패합니다.

---

## ⏳ BUSY 게이트와 상태 레지스터 read-back (SW-020)

### BUSY와 WEL은 같은 응답, 다른 보증

`05h`(RDSR) 한 프레임이 S7~S0을 모두 돌려줍니다. 그중 **S0 = BUSY(WIP)**, **S1 = WEL**입니다.
한 번의 읽기로 둘 다 볼 수 있지만, 둘이 같은 값이라는 뜻은 아닙니다.

| 읽은 SR1 | 의미 | 해야 할 일 |
|----------|------|-----------|
| WEL=1, BUSY=0 | 래치가 걸렸고 칩이 놀고 있다 | 진행 |
| WEL=1, BUSY=1 | 래치는 걸렸지만 아직 쓰기/소거 중 | **대기** — 이때 보낸 program은 장치가 무시 |
| WEL=0 | WREN이 안 먹었다 | WREN 재전송 |

### 무엇을 고쳤나

`Flash_WriteEnable()`은 이전에 WEL만 확인하고 5회 × 10 µs(총 50 µs) 만에 포기했습니다.

1. **같은 `05h` 응답에서 BUSY까지 함께 검사** — 추가 SPI 프레임은 0개입니다.
2. **재시도 예산을 200000 × 5 µs ≈ 1초**로 확대(`Flash_WaitBusy`와 동일). 페이지 프로그램 0.4~3 ms, WRSR 15~25 ms, 섹터 소거 45 ms를 흡수합니다. 50 µs로는 어떤 실제 사이클도 기다릴 수 없었습니다.
3. **상태 읽기 자체가 실패하면 즉시 반환** — 칩이 없을 때 1초를 버리지 않습니다.
4. **표준 WRSR 경로에 사전 BUSY 검사 + 쓰기 후 read-back** — 이전에는 `Flash_WriteStatus(1/2, v)`가 프레임을 보내고 BUSY만 기다린 뒤 무조건 `FLASH_OK`를 반환했습니다. 이제 SR1(`0xFC`)·SR2(`0x7B`) 쓰기 가능 비트를 다시 읽어 비교하고, 어긋나면 `FLASH_ERR_WRITE_FAIL`입니다.

정상 경로 비용은 늘지 않습니다: WREN 1회 + RDSR 1회는 이전과 같고, WRSR 경로만 확인용 읽기 1회가 늘어납니다. 실제로 기다리는 것은 BUSY일 때뿐입니다.

> 비용: ROM **+88 B**(`-Oz`), **+76 B**(`-Os`) — 같은 툴체인·같은 플래그로 수정 전/후를 각각 링크해 비교한 값입니다.
> 테스트 1개 추가로 **29 suites**. 시뮬레이터는 WIP=1 구간에서 WREN을 무시하고 SR1에 BUSY를 세우며,
> 장치가 WRSR을 조용히 버리는 경우까지 모사합니다.
> 수정 전 코드에서는 TEST 29가 **line 1400**(BUSY를 보지 않음)과 **line 1394**(read-back 없음)에서 각각 실패합니다.

---

## ⏱ 부팅 클럭은 설정 블록에서 온다 (SW-021)

`Flash_BootInitOpt()`는 지금까지 10 MHz로 부팅하고 **그대로** 남았습니다. 50 MHz로 올리는 단계는 `Flash_InitOpt()`에만 있었기 때문에, 부팅 경로를 쓰는 제품은 런타임 내내 5배 느린 버스로 동작했습니다.

이제 클럭은 **플래시에서 읽은 설정 블록의 값**입니다. `Flash_BasicRead()`가 28 B 블록을 그대로 받아오고, magic·체크섬·패밀리 검증을 모두 통과한 뒤에야 버스를 올립니다.

| 시점 | 버스 | 이유 |
|------|------|------|
| 프로브 · 설정 블록 읽기 · QE/4바이트 모드 변경 | 10 MHz | 아직 무엇이 꽂혔는지 모르는 구간 |
| 설정 검증 완료 후 | `cfg.spi_hz` | 보드와 부품을 이제 신뢰할 수 있음 |

### 하위 호환 — 24 B 블록만 써 둔 보드도 그대로 부팅

`spi_hz`를 `crc` **뒤에** 덧붙였습니다. 그래서

- 기존 필드 오프셋(magic 0, jedec 4, model 8, features 12, sfc_page_size 16, addr_mode 18, crc_algo 19, crc 20)이 전부 그대로이고,
- `Flash_ConfigCrc()`는 `offsetof(crc)`까지만 더하므로 체크섬 구간도 **정확히 20 B 그대로**입니다.

24 B 블록만 써 둔 기존 보드는 그 뒤 4 B가 지워진 플래시(0xFFFF_FFFF)로 읽혀 허용 범위(1~50 MHz)를 벗어나고, 드라이버는 **10 MHz를 유지한 채 정상 부팅**합니다. 필드가 없다고 부팅을 실패시키지 않습니다.

> 필드가 체크섬 밖에 있는 대신 값 자체를 범위로 검증합니다(1~50 MHz). 손상된 값이 그 범위에 우연히 들어오면 클럭만 기본값(10 MHz)으로 남고, 데이터 무결성은 영향받지 않습니다.
> `FLASH_BOOT_CONFIG`가 28 B에서 벗어나면 컴파일이 실패하도록 `typedef char[... == 28]` 가드를 넣었습니다 — 양산 도구가 이미 써 둔 블록 크기가 조용히 바뀌는 일을 막습니다.

> 비용: ROM **+24 B** (`-Os`, `-Oz` 동일), easy API만 링크하는 `--gc-sections` 구성에서는 **+0 B**(이 경로에 `Flash_BootInit`이 링크되지 않음).
> 테스트 1개 추가로 **30 suites**(SW-021 시점). 수정 전 코드에서는 TEST 30이 **line 1470**에서 실패합니다.

---

## 🧩 GigaDevice 16Mb / 32Mb 추가 (SW-030)

GD25LQ 패밀리는 **용량만 다르고 명령 세트·상태 레지스터·타이밍이 같습니다.** 그래서 64Mb 행을 복제하고 밀도에 해당하는 값만 바꿨습니다.

| 부품 | JEDEC (`9Fh`) | 용량 | BP1 단독 보호 구간 (CMP=0) |
|------|---------------|------|-----------------------------|
| GD25LQ16E | `C8 60 15` | 2MB | blocks 30–31, `1E0000h–1FFFFFH`, 128KB (Upper 1/16) |
| GD25LQ32E | `C8 60 16` | 4MB | blocks 62–63, `3E0000h–3FFFFFH`, 128KB (Upper 1/32) |
| GD25LQ64E | `C8 60 17` | 8MB | blocks 124–127, `7C0000h–7FFFFFH`, 256KB (Upper 1/32) |

세 부품이 공유하는 성질(그래서 코드 분기가 필요 없음):

- QE = **SR2 S9** (`qe_reg = 2`, `qe_bit = 1`)
- BP4 = **SR1 S6** → `FLASH_FEAT_BP4` (MX25L의 같은 비트는 QE라서 공용 코드가 조심해야 하는 지점)
- `01h`는 **고정 16비트** — 8비트만 클록하면 S15–S8(QE·CMP)이 0으로 지워짐 → `FLASH_FEAT_STATUS_2BYTE_WRITE`
- `50h` Volatile SR WREN 지원 → `FLASH_FEAT_VSR_WREN`
- 주소 3바이트 (셋 다 16MB 미만)

근거 문헌: GD25LQ32E Rev1.6 / GD25LQ16E — ID Definitions 표(`9Fh → C8 60 1x`), §7.4 WRSR(16비트 고정), 보호 영역 표(CMP=0). 64Mb와 같은 실리콘 패밀리라는 판단은 **명령·비트 위치·프레임 길이가 문서상 동일**하다는 대조에서 나왔습니다.

> 비용: ROM **+160 B** (행당 80 B × 2, 코드 증가 0, `-Os`/`-Oz` 동일).
> 테스트 1개 추가로 **31 suites**. 32Mb 행의 `flash_size`를 1MB로 틀리게 바꾸면 TEST 31이 **line 1556**에서 실패합니다(역검증 완료).

---

## 📦 코드 크기 (ROM) — SW-017

원칙은 **사람이 읽고 고치기 쉬운 코드**입니다. 크기는 구조를 꼬지 않고 줄일 수 있는 곳에서만 줄였습니다.

| 적용 | 내용 |
|------|------|
| 모델명 문자열 제거 | `FLASH_INFO.name` 필드와 7개 문자열 삭제 (−약 150 B). 모델 식별은 `jedec_id`/`model_id`로 충분 |
| 공통 지오메트리 상수화 | 7종 모두 page 256 / sector 4096 / block 65536 → 모델별 필드 삭제, `FLASH_PAGE_SIZE` 등 상수로 (−약 96 B) |
| 중복 경로 통합 | Boot/Init 공통 경로(`Flash_ProbeBus` + `Flash_ApplyHandleConfig`), Protect/Unprotect(`Flash_WriteBlockProtect`), 칩 소거 다이 루프 하나 |
| QPI 명시 거부 | 모드 목록에는 추가, 호출 시 `FLASH_ERR_UNSUPPORTED` 반환 (아래 참고) |

> 나눗셈(`/`, `%`)은 **가독성을 위해 그대로 사용**합니다. 프로젝트의 다른 코드가 이미 libgcc 나눗셈기를 링크하므로 이미지 크기 차이가 거의 없습니다.

측정 (Cortex-M0 Thumb, lld, libgcc 제외):

| 빌드 | SW-019 기준 (표 기준값) | 직전 (SW-017) |
|------|------|------|
| 전체 함수 유지 (`-Os`) | **5,882 B** | 5,612 B |
| 전체 함수 유지 (`-Oz`) | **5,520 B** | 5,294 B |
| easy API만 + `--gc-sections` (`-Oz`) | **5,160 B** | 4,954 B |
| 위 + `-flto` | **3,680 B** | 3,616 B |

> SW-018(상태 게이트, +90 B)과 SW-019(결합 쓰기 + 검증, +226 B), SW-020(BUSY 게이트 + WRSR read-back, `-Oz` +88 B), SW-021(부팅 클럭 필드, +24 B)는 **정확성 비용**입니다.
> 숫자만 보면 SW-017 시점이 가장 작지만, 그 버전은 칩이 사라져도 0xFF를 정상 데이터로 돌려주고,
> SR1을 한 번 바꾸면 QE가 조용히 꺼지는 상태였습니다. 네 가지 빌드 모두 같은 진입 심볼 목록
> (`Flash_BootInit` + easy API 4종)으로 측정했습니다.
> 표의 절대값은 SW-019 시점 실측이고, SW-020(+88 B)·SW-021(+24 B)·SW-030(+160 B)는 **같은 툴체인으로 이전/현재를 A/B 링크해 잰 증분**입니다.
> 그래서 지금 `-Oz` 전체 유지 빌드는 대략 **5,792 B**(5,520 + 88 + 24 + 160)로 보면 됩니다.

크기 우선 빌드:

```bash
clang --target=arm-none-eabi -mcpu=cortex-m0 -mthumb \
      -Oz -flto -std=c99 -ffunction-sections -fdata-sections -I. -c <각 파일>.c
# 링크 단계에서 -Wl,--gc-sections 추가
```

> `-Oz`·`-flto`는 동작을 바꾸지 않습니다(31/31 테스트 통과).  
> 더 줄이려면 기능을 줄이는 선택이 필요합니다: CRC 알고리즘 고정(−170 B) · `flash_easy` 제외(−296 B) · 지원 모델 축소(모델당 −80 B).

### 읽기 모드와 QPI

| 모드 | 지원 | 비고 |
|------|------|------|
| SINGLE / FAST / DUAL / DUAL_IO / QUAD / QUAD_IO | ✅ | 실제 사용 경로 |
| **QPI (4-4-4)** | ❌ `FLASH_ERR_UNSUPPORTED` | 목록에는 있으나 미구현 |

QPI는 명령·주소까지 4라인으로 보내야 하는데, 현재 SPI 프레임과 TCON SFC는 명령/주소를 항상 1라인으로 보냅니다.
그래서 호출하면 **틀린 opcode로 읽지 않고 명확한 오류로 거부**합니다(enum에 값을 두어 호출자가 이름으로 지정 가능).

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
- 최대 지연: 포화 한계 ≈ 119초 (108MHz 기준, `uint32_t` 루프 수 한계)
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
