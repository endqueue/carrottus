/**
 * @file crperf.h
 * @brief Carrottus Perf: the platform's clocks, its wait on one of them, and the
 * process's memory counters.
 *
 * Each call reads one value from the operating system or the CPU and returns
 * it, or, for crperf_sleep_until_monotonic_ns() and
 * crperf_sleep_until_monotonic_ms(), hands the calling thread to the
 * operating system until a time. Nothing is measured, averaged or
 * kept between calls, and no call takes a handle: there is no state to own.
 *
 * @par Threads
 * No call takes an object, so under the library rule (crcore.h, "Threads")
 * any call may overlap any other.
 *
 * No function here returns a CR_RESULT. The clocks cannot fail, and
 * crperf_get_ram_usage() reports a failed read as 0.
 */

#ifndef CRPERF_H
#define CRPERF_H

#include "stdint.h"

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Reads the CPU's free-running hardware counter.
 *
 * - x86 and x86-64: the time stamp counter (`rdtsc`).
 * - 64-bit ARM: the generic timer's virtual count (`CNTVCT_EL0`).
 *
 * Both are fixed-frequency counters, not a count of core clock cycles, and
 * the frequency is not reported here and differs between machines. The value
 * is good for comparing two readings taken on the same machine, and nothing
 * else: it is not a time in any unit. For a time, use
 * crperf_get_monotonic_time_ns().
 *
 * @return The counter's current value, in ticks of that counter.
 */
uint64_t crperf_catch_current_clock_cycle();

/**
 * @brief Reads how much physical memory the calling process is using.
 *
 * What the platform counts differs:
 *
 * | Platform | Current | Peak |
 * |---|---|---|
 * | Windows | Working set size | Peak working set size |
 * | Linux | `VmRSS` in `/proc/self/status` | `VmHWM` in `/proc/self/status` |
 * | Apple | Physical footprint (`phys_footprint`) | Peak resident size (`resident_size_peak`) |
 *
 * On Apple platforms the two are different measures, so the peak is not
 * necessarily the largest value the current figure has had.
 *
 * Either parameter may be NULL to skip that value.
 *
 * @param[out] current_ram_usage Receives the memory in use now, in bytes, or
 *                               0 if the platform's query failed. May be
 *                               NULL.
 * @param[out] peak_ram_usage    Receives the most memory the process has
 *                               used, in bytes, or 0 if the platform's query
 *                               failed. May be NULL.
 */
void crperf_get_ram_usage(
    uint64_t *current_ram_usage,
    uint64_t *peak_ram_usage);

/**
 * @brief Reads a monotonic clock, in milliseconds.
 *
 * The epoch is arbitrary, so only the difference between two readings means
 * anything. The clock never goes backwards and is not the time of day.
 *
 * - Windows: `QueryPerformanceCounter`.
 * - Linux and Apple: `CLOCK_MONOTONIC`.
 *
 * On Apple platforms this is not the clock
 * crperf_get_monotonic_time_ns() reads: `CLOCK_MONOTONIC` keeps counting
 * while the system sleeps and `CLOCK_UPTIME_RAW` does not, so the two drift
 * apart across a sleep. Take both readings of a difference from the same
 * function.
 *
 * @return Milliseconds since the clock's epoch.
 */
uint64_t crperf_get_current_time_milliseconds();

/**
 * @brief Reads the host's monotonic clock, in nanoseconds.
 *
 * The epoch is arbitrary, so only the difference between two readings means
 * anything. This is the clock the platform audio APIs timestamp against, so
 * a reading is directly comparable with
 * CRAUDIO_CALLBACK_INFO::host_time_ns:
 *
 * | Platform | Clock | The audio timestamp on the same clock |
 * |---|---|---|
 * | Windows | `QueryPerformanceCounter` | WASAPI `u64QPCPosition` |
 * | Apple | `CLOCK_UPTIME_RAW` (`mach_absolute_time`) | CoreAudio `AudioTimeStamp.mHostTime` |
 * | Linux | `CLOCK_MONOTONIC` | PipeWire `pw_time.now`, ALSA `SND_PCM_TSTAMP_TYPE_MONOTONIC` |
 *
 * On Apple platforms this clock stops while the system sleeps.
 *
 * @return Nanoseconds since the clock's epoch.
 */
uint64_t crperf_get_monotonic_time_ns(void);

/**
 * @brief Blocks the calling thread until the monotonic clock reaches a time.
 *
 * The time is a reading of crperf_get_monotonic_time_ns(), or one worked
 * out from a reading: the wait is on that clock on every platform. It is a
 * point in time, not a length of time, so a caller that wakes late and waits
 * for its next point does not fall further behind.
 *
 * | Platform | The wait |
 * |---|---|
 * | Windows | A waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` where the system has it), set for what is left on `QueryPerformanceCounter` |
 * | Apple | `mach_wait_until` |
 * | Linux | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` |
 *
 * It never returns before the clock has reached @p deadline_ns: a wait the
 * system ends early (a signal, a spurious wake) is taken up again. It
 * returns at once if the clock is already there. How long after the
 * deadline it returns is the operating system's scheduler's to say, and no
 * bound is promised. On macOS it has been measured at a quarter of a
 * millisecond for a wait of one millisecond and at five milliseconds for
 * waits of twenty and more, which the system's timer coalescing allows
 * itself; on Windows expect the system's timer resolution where the
 * high-resolution timer is missing.
 *
 * On Apple platforms the clock stops while the system sleeps, so a wait
 * that spans a sleep is that much longer in time of day.
 *
 * Nothing wakes the thread early: there is no cancel. A caller that has to
 * stop a waiting thread waits in pieces short enough for it.
 *
 * @param[in] deadline_ns When to return, in nanoseconds since the epoch of
 *                        crperf_get_monotonic_time_ns().
 */
void crperf_sleep_until_monotonic_ns(uint64_t deadline_ns);

/**
 * @brief Blocks the calling thread until the millisecond clock reaches a
 * time.
 *
 * As crperf_sleep_until_monotonic_ns(), for a time on the clock
 * crperf_get_current_time_milliseconds() reads: @p deadline_ms is a
 * reading of that function, or one worked out from a reading. It is not a
 * reading of crperf_get_monotonic_time_ns() divided down: on Apple
 * platforms the two are different clocks.
 *
 * It waits for what is left on the millisecond clock, by
 * crperf_sleep_until_monotonic_ns(), and reads the millisecond clock
 * again, until that clock has reached @p deadline_ms. So it never returns
 * before then, returns at once if the clock is already there, and is as
 * late as crperf_sleep_until_monotonic_ns() is, plus up to the
 * millisecond the clock counts in.
 *
 * On Apple platforms the millisecond clock keeps counting while the system
 * sleeps and the clock the wait is made on does not, so a wait that spans a
 * system sleep returns after the deadline by up to the time left on it when
 * the system went to sleep.
 *
 * @param[in] deadline_ms When to return, in milliseconds since the epoch of
 *                        crperf_get_current_time_milliseconds().
 */
void crperf_sleep_until_monotonic_ms(uint64_t deadline_ms);

#if __cplusplus
}
#endif

#endif // CRPERF_H
