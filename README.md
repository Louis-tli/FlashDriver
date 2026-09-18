# Generic SPI NOR Flash Driver for ARM Cortex-M0

ARM Cortex-M0 (`arm-cm0`) 및 `armcc` (ARM Compiler 5 / RVCT) 환경에 최적화된 초경량 고성능 SPI NOR Flash Driver입니다.
TCON SoC의 **SFC (Serial Flash Controller) Memory-Mapped DMA** 고속 읽기와 **SPI Command** 제어 경로를 결합한 하이브리드 아키텍처를 지원합니다.

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
- **Cortex-M0 (`arm-cm0`) & `armcc` 호환**:
  - Strict C90 표준 준수.
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
├── flash.h          # 공개 드라이버 API, FLASH_READ_UNIT, FLASH_INFO 및 FLASH_HANDLE 정의
├── flash.c          # 드라이버 코어 엔진 (Init, Read, Program, Erase, Die Select 등)
├── flash_table.h    # Flash 모델 검색 인터페이스 및 테이블 선언
├── flash_table.c    # 7종 Flash 메모리 ROM 테이블 (const FLASH_INFO)
├── flash_sfc.h      # TCON SFC IP 하드웨어 레지스터 맵(ACCESS_UNIT 포함) 및 DMA API 헤더
├── flash_sfc.c      # TCON SFC IP 드라이버 구현 (Single/Quad 모드 전환, 단위별 2K/4K DMA Read)
├── flash_spi.h      # Low-Level SPI 컨트롤러 인터페이스 헤더
├── flash_spi.c      # Low-Level SPI 컨트롤러 드라이버 및 HW 추상화 스텁
└── test_flash.c     # 전 기능 시뮬레이션 및 C90 표준 검증 테스트 슈트
```

---

## 🚀 빌드 및 검증 (Build & Test)

GCC (MinGW / LLVM) 또는 ARMCC를 통해 C90 엄격 모드로 빌드하고 테스트를 실행할 수 있습니다:

```bash
# C90 엄격 모드로 컴파일
gcc -std=c90 -Wall -Wextra -pedantic -I. flash.c flash_table.c flash_spi.c flash_sfc.c test_flash.c -o test_flash.exe

# 검증 테스트 실행
./test_flash.exe
```

### 테스트 통과 항목:
1. **Table Lookup**: 7종 Flash JEDEC ID 및 파라미터 매칭
2. **Init & Bus Probing**: 플로팅 버스 (0x000000 / 0xFFFFFF) NO_CONNECT 판별 및 자동 QE 활성화
3. **GD25LQ64E Status Write**: 2-Byte Status Register 동시 쓰기 시퀀스
4. **Page Boundary Splitting**: 256바이트 경계 걸침 시 3회 분할 쓰기
5. **Multi-Die Switching**: W25M512 32MB 경계 주소 접근 시 `0xC2` Software Die Select 자동 전환
6. **Flash Read Units & SFC DMA Read**: Byte(1B), Halfword(2B), Word(4B, Default) 단위별 읽기 및 정렬 검증, SFC DMA 시퀀스 검증
