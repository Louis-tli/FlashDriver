/******************************************************************************
 * File Name   : flash_delay.h
 * Description : Time-based busy-wait delay (SW-014)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 *
 * Flash datasheets specify wait times in absolute microseconds (e.g. reset
 * recovery tRST), but a raw loop count scales with the MCU core clock.
 * SW-014 converts microseconds to loop counts using the system clock
 * configured at boot (targets: 108 MHz / 50 MHz), so the same call
 * `Flash_DelayUs(n)` keeps its meaning on either clock.
 *
 * Conversion is deliberately conservative (round-up + low cycles-per-loop
 * estimate), so the actual delay is never shorter than requested.
 ******************************************************************************/

#ifndef __FLASH_DELAY_H__
#define __FLASH_DELAY_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------
 * Compile-time defaults (override via compiler -D if needed)
 *--------------------------------------------------------------------------*/

/* MCU core clock (HCLK) in Hz used when Flash_SysClockSet() is never called */
#ifndef FLASH_SYS_CLK_HZ
#define FLASH_SYS_CLK_HZ            (108000000UL)
#endif

/* Estimated CPU cycles per no-op loop iteration (Cortex-M0, volatile counter).
 * Kept LOW on purpose: fewer assumed cycles -> more loops -> never under-delays. */
#ifndef FLASH_DELAY_CYCLES_PER_LOOP
#define FLASH_DELAY_CYCLES_PER_LOOP (3U)
#endif

/*----------------------------------------------------------------------------
 * Runtime clock configuration
 *--------------------------------------------------------------------------*/

/**
 * @brief  Set the MCU core clock used for us->loop conversion.
 * @param  hz: Core clock in Hz (e.g. 108000000U or 50000000U).
 *             0 is ignored (keeps the current setting).
 */
void Flash_SysClockSet(uint32_t hz);

/**
 * @brief  Get the currently configured core clock in Hz.
 */
uint32_t Flash_SysClockGet(void);

/*----------------------------------------------------------------------------
 * Delay API
 *--------------------------------------------------------------------------*/

/**
 * @brief  Convert microseconds to raw loop count (pure function, testable).
 *         Rounds UP: result * loops never under-delays the requested time.
 *         Saturates at 0xFFFFFFFF (~119 s @ 108 MHz).
 */
uint32_t Flash_DelayUsToLoops(uint32_t us);

/**
 * @brief  Busy-wait for approximately `us` microseconds (minimum guarantee
 *         with the configured clock; longer if the loop costs more cycles).
 */
void Flash_DelayUs(uint32_t us);

/**
 * @brief  Raw busy-wait loop (clock-dependent). Kept for low-level tuning.
 */
void Flash_DelayLoops(uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_DELAY_H__ */
