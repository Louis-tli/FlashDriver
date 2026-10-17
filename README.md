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

> 고급 제어(특정 읽기 모드, CRC, SFC 튜닝, 블록 보호)가 필요하면 `flash.h`의 전체 API를 사용하세요.

---

## 🌟 주요 특징 (Key Features)

- **하이브리드 듀얼 패스 (Hybrid Dual-Path Architecture)**:
  - **SPI Command Path**: JEDEC ID (0x9F), Status Register 읽기/쓰기 (0x05/0x35/0x15/0x01), WREN (0x06), Page Program (0x02/0x32), Erase (0x20/0x52/0xD8/0xC7), Software Die Select (0xC2) 제어.
  - **SFC Data Path**: 대용량 고속 Read 전용. Memory-Mapped 주소를 통해 2KB 또는 4KB 단위의 DMA Transfer로 고속 데이터 수신 및 하드웨어 CRC32 연산 옵션 지원.
- **SFC 안전 Read 시퀀스 내장**:
  - Quad I/O Read 전 `SFC Single 모드 설정` ➔ `SPI Status BUSY=0 폴링 완료` ➔ `SFC Quad I/O 모드 재설정` ➔ `DMA 전송` 시퀀스 자동화.
- **초경량 메모리 풋프린트 (< 3KB)**:
  - `flash.o` 코드 크기 약 2.2 KB.
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
├── flash_spi.h      # Low-Level SPI 컨트롤러 인터페이스 헤더
├── flash_spi.c      # Low-Level SPI 컨트롤러 드라이버 및 HW 추상화 스텁
└── test_flash.c     # 전 기능 시뮬레이션 및 C99 표준 검증 테스트 슈트 (20 suites)
```

---

## 🚀 빌드 및 검증 (Build & Test)

GCC (MinGW / LLVM) 또는 ARM Compiler (ARMCC/ARMCLANG)를 통해 C99 표준으로 빌드하고 테스트를 실행할 수 있습니다:

```bash
# GCC 환경에서 C99 엄격 모드로 컴파일
gcc -std=c99 -Wall -Wextra -pedantic -I. flash.c flash_table.c flash_spi.c flash_sfc.c flash_cmd.c flash_crc.c flash_easy.c test_flash.c -o test_flash.exe

# 검증 테스트 실행
./test_flash.exe
```

> **ARMCC (ARM Compiler 5) 컴파일 시 플래그**: `armcc --c99 -c flash.c`
> **ARMCLANG (ARM Compiler 6) 컴파일 시 플래그**: `armclang --target=arm-arm-none-eabi -mcpu=cortex-m0 -std=c99 -c flash.c`

### 테스트 통과 항목 (20 suites):
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
18. **WREN 50h vs 06h**: 지원 칩은 50h(VSR), 미지원은 06h 폴백
19. **FLASH_IDX Access**: `g_flash_table[FLASH_IDX_x]` 인덱스 접근 패턴
20. **CRC Algorithm from Boot Config**: 설정블록 `crc_algo`로 알고리즘 선택
