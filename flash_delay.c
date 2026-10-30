/******************************************************************************
 * File Name   : flash_delay.c
 * Description : Time-based busy-wait delay implementation (SW-014)
 * Target Core : ARM Cortex-M0 (ARMv6-M)
 * Compiler    : ARMCC (ARM Compiler 5) / ARMCLANG / GCC (C99 Compatible)
 ******************************************************************************/

#include "flash_delay.h"

/******************************************************************************
 * Internal State (SW-017)
 *
 * Two values are kept on purpose:
 *   s_sys_clk_hz    : what the caller asked for (read back by Flash_SysClockGet)
 *   s_loops_per_us  : the converted "loops per microsecond", so the hot path is
 *                     one multiply instead of a divide.
 *
 * RAM cost: 4 extra bytes. ROM saving: no software divider in the image.
 ******************************************************************************/
#define FLASH_DELAY_DENOM   (1000000UL * FLASH_DELAY_CYCLES_PER_LOOP)

/* ceil(clk / denom) evaluated at COMPILE time (an initializer is a constant
 * expression, so this never becomes a runtime divide). */
#define FLASH_DELAY_CEIL_DIV(clk)   (((clk) / FLASH_DELAY_DENOM) + \
                                     ((((clk) % FLASH_DELAY_DENOM) != 0UL) ? 1UL : 0UL))

#define FLASH_LOOPS_PER_US_108M     ((uint32_t)FLASH_DELAY_CEIL_DIV(108000000UL))  /* 36 */
#define FLASH_LOOPS_PER_US_50M      ((uint32_t)FLASH_DELAY_CEIL_DIV( 50000000UL))  /* 17 */

static uint32_t s_sys_clk_hz   = FLASH_SYS_CLK_HZ;
static uint32_t s_loops_per_us = (uint32_t)FLASH_DELAY_CEIL_DIV(FLASH_SYS_CLK_HZ);

/******************************************************************************
 * Helper: loops-per-microsecond for a supported core clock.
 *
 * Why a switch and not "hz / denom" (SW-017):
 *   Cortex-M0 has no divide instruction, and clang turns even a division by a
 *   compile-time constant into a call to __aeabi_uidiv. That call pulls
 *   libgcc's software divider (~100-200 bytes of ROM) into the image. The
 *   driver only documents two clocks, so the mapping lives in code instead.
 *
 *   Anything else keeps the 108 MHz timing. On a slower core the same loop
 *   count takes longer, so the delay can only get longer, never shorter — the
 *   conservative direction this driver promises.
 ******************************************************************************/
static uint32_t Flash_LoopsPerUs(uint32_t hz)
{
    switch (hz) {
        case 108000000UL: return FLASH_LOOPS_PER_US_108M;
        case  50000000UL: return FLASH_LOOPS_PER_US_50M;
        default:          return FLASH_LOOPS_PER_US_108M;
    }
}

/******************************************************************************
 * Public API: Flash_SysClockSet
 ******************************************************************************/
void Flash_SysClockSet(uint32_t hz)
{
    if (hz == 0U) {
        return;                     /* invalid clock: keep current setting */
    }
    s_sys_clk_hz   = hz;
    s_loops_per_us = Flash_LoopsPerUs(hz);
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
    /* Clamp instead of dividing to detect overflow.
     *
     * The old check "us > 0xFFFFFFFF / loops_per_us" needed a runtime divide.
     * Clamping to FLASH_DELAY_MAX_US (3 s) keeps "us * loops_per_us" inside
     * 32 bits for every clock the driver accepts, costs one compare, and no
     * longer pulls libgcc's divider into the image (SW-017). */
    if (us > FLASH_DELAY_MAX_US) {
        us = FLASH_DELAY_MAX_US;
    }

    return us * s_loops_per_us;
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
