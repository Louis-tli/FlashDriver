/******************************************************************************
 * File Name   : flash_delay.c
 * Description : Time-based busy-wait delay implementation (SW-014)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_delay.h"

/******************************************************************************
 * Internal State
 ******************************************************************************/
static uint32_t s_sys_clk_hz = FLASH_SYS_CLK_HZ;

/******************************************************************************
 * Public API: Flash_SysClockSet
 ******************************************************************************/
void Flash_SysClockSet(uint32_t hz)
{
    if (hz == 0U) {
        return;                     /* invalid clock: keep current setting */
    }
    s_sys_clk_hz = hz;
}

/******************************************************************************
 * Public API: Flash_SysClockGet
 ******************************************************************************/
uint32_t Flash_SysClockGet(void)
{
    return s_sys_clk_hz;
}

/******************************************************************************
 * Public API: Flash_DelayUsToLoops
 *
 * loops = ceil(us * clk_hz / (1e6 * CYCLES_PER_LOOP))
 *       = us * ceil(clk_hz / (1e6 * CYCLES_PER_LOOP))     [per-us rounding up]
 *
 * Only 32-bit math is used (no 64-bit division) to keep the M0 footprint
 * small. Per-microsecond rounding adds at most one loop per us, which is
 * safe: it can only make the delay longer, never shorter.
 ******************************************************************************/
uint32_t Flash_DelayUsToLoops(uint32_t us)
{
    const uint32_t denom = 1000000UL * FLASH_DELAY_CYCLES_PER_LOOP;
    const uint32_t loops_per_us = (s_sys_clk_hz + denom - 1UL) / denom;
    uint32_t loops;

    if (us == 0U || loops_per_us == 0U) {
        return 0U;
    }

    /* Saturate instead of wrapping on very long requests */
    if (us > (0xFFFFFFFFUL / loops_per_us)) {
        return 0xFFFFFFFFUL;
    }

    loops = us * loops_per_us;
    return loops;
}

/******************************************************************************
 * Public API: Flash_DelayUs
 ******************************************************************************/
void Flash_DelayUs(uint32_t us)
{
    Flash_DelayLoops(Flash_DelayUsToLoops(us));
}

/******************************************************************************
 * Public API: Flash_DelayLoops (raw, clock-dependent)
 ******************************************************************************/
void Flash_DelayLoops(uint32_t count)
{
    volatile uint32_t i;

    for (i = 0U; i < count; i++) {
        /* No-op loop */
    }
}
