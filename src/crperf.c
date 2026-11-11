
#include "crperf.h"

#if CARROTTUS_PLATFORM_WIN32
#include <windows.h>
#include <Psapi.h>
#elif CARROTTUS_PLATFORM_LINUX
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#elif CARROTTUS_PLATFORM_APPLE
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <time.h>
#endif

#if __cplusplus
extern "C" {
#endif

uint64_t crperf_catch_current_clock_cycle() {
#if defined(_MSC_VER) && !defined(_M_ARM64)
	return __rdtsc();
#elif defined(__aarch64__) || defined(__arm64__)
	// x86's TSC and ARM's generic timer are both just free-running,
	// fixed-frequency hardware counters usable for relative timing (not
	// literal core-clock cycles on either architecture) -- CNTVCT_EL0 is
	// ARM's direct equivalent and, unlike PMCCNTR_EL0, is readable from
	// userspace without special privileges.
	uint64_t val;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
	return val;
#else
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#endif
}

void crperf_get_ram_usage(
    uint64_t *current_ram_usage,
    uint64_t *peak_ram_usage) {
#if CARROTTUS_PLATFORM_WIN32
	PROCESS_MEMORY_COUNTERS pmc = {0};
	pmc.cb                      = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
	if (current_ram_usage)
		*current_ram_usage = (uint64_t)pmc.WorkingSetSize;
	if (peak_ram_usage)
		*peak_ram_usage = (uint64_t)pmc.PeakWorkingSetSize;
#elif CARROTTUS_PLATFORM_LINUX
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) {
		if (current_ram_usage)
			*current_ram_usage = 0;
		if (peak_ram_usage)
			*peak_ram_usage = 0;
		return;
	}

	uint64_t rss = 0, hwm = 0;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		unsigned long kb;
		if (sscanf(line, "VmRSS: %lu kB", &kb) == 1)
			rss = (uint64_t)kb * 1024;
		else if (sscanf(line, "VmHWM: %lu kB", &kb) == 1)
			hwm = (uint64_t)kb * 1024;
	}
	fclose(f);

	if (current_ram_usage)
		*current_ram_usage = rss;
	if (peak_ram_usage)
		*peak_ram_usage = hwm;
#elif CARROTTUS_PLATFORM_APPLE
	struct task_vm_info info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	kern_return_t kr             = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count);
	if (kr != KERN_SUCCESS) {
		if (current_ram_usage)
			*current_ram_usage = 0;
		if (peak_ram_usage)
			*peak_ram_usage = 0;
		return;
	}

	if (current_ram_usage)
		*current_ram_usage = (uint64_t)info.phys_footprint;
	if (peak_ram_usage)
		*peak_ram_usage = (uint64_t)info.resident_size_peak;
#endif
}

uint64_t crperf_get_current_time_milliseconds() {
#if CARROTTUS_PLATFORM_WIN32
	static LARGE_INTEGER frequency;
	if (frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64_t)((counter.QuadPart * 1000) / frequency.QuadPart);
#elif CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#else
#error "Unsupported platform"
	return 0;
#endif
}

uint64_t crperf_get_monotonic_time_ns(void) {
#if CARROTTUS_PLATFORM_WIN32
	// QueryPerformanceFrequency just reads a value fixed at boot, so it's
	// queried each call rather than cached in a static.
	LARGE_INTEGER frequency, counter;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&counter);
	// Split into whole seconds + remainder: a plain counter * 1e9 overflows
	// uint64 after ~30 minutes of uptime at the usual 10 MHz frequency.
	uint64_t freq  = (uint64_t)frequency.QuadPart;
	uint64_t ticks = (uint64_t)counter.QuadPart;
	return (ticks / freq) * 1000000000ULL + ((ticks % freq) * 1000000000ULL) / freq;
#elif CARROTTUS_PLATFORM_APPLE
	// Not CLOCK_MONOTONIC: on macOS that keeps counting through sleep, while
	// CoreAudio's host time is mach_absolute_time, which doesn't.
	// CLOCK_UPTIME_RAW is mach_absolute_time already converted to ns.
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#elif CARROTTUS_PLATFORM_LINUX
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#else
#error "Unsupported platform"
	return 0;
#endif
}

void crperf_sleep_until_monotonic_ns(uint64_t deadline_ns) {
#if CARROTTUS_PLATFORM_WIN32
	// Windows has no wait on an absolute QueryPerformanceCounter time, so
	// each pass waits for what is left and the loop reads the clock again.
	// The high-resolution flag is Windows 10 1803 and later; without it the
	// timer fires on the system's timer interrupt.
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!timer)
		timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);

	for (;;) {
		uint64_t now = crperf_get_monotonic_time_ns();
		if (now >= deadline_ns)
			break;

		uint64_t remaining_ns = deadline_ns - now;
		if (!timer) {
			// No timer to be had: the coarse wait, a millisecond at least.
			Sleep((DWORD)(remaining_ns / 1000000ULL) + 1);
			continue;
		}

		// Negative is relative, in 100 ns units; rounded up so that one
		// pass is usually enough.
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)((remaining_ns + 99ULL) / 100ULL);
		if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
			Sleep((DWORD)(remaining_ns / 1000000ULL) + 1);
			continue;
		}
		WaitForSingleObject(timer, INFINITE);
	}

	if (timer)
		CloseHandle(timer);
#elif CARROTTUS_PLATFORM_APPLE
	// mach_wait_until waits on mach_absolute_time, which is the clock
	// CLOCK_UPTIME_RAW reports in nanoseconds, but takes its own ticks:
	// nanoseconds * denom / numer, rounded up so the wait doesn't end a tick
	// short. The timebase is fixed for the life of the system; it is read
	// each call, as the Windows frequency is, to keep no state here.
	mach_timebase_info_data_t timebase;
	if (mach_timebase_info(&timebase) != KERN_SUCCESS)
		return;
	if (timebase.numer == 0)
		return;

	const unsigned __int128 scaled = (unsigned __int128)deadline_ns * timebase.denom + (timebase.numer - 1);
	const uint64_t deadline_ticks  = (uint64_t)(scaled / timebase.numer);

	// It can come back early (KERN_ABORTED), so go by the clock, not by
	// what it returns.
	while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline_ns)
		mach_wait_until(deadline_ticks);
#elif CARROTTUS_PLATFORM_LINUX
	struct timespec deadline;
	deadline.tv_sec  = (time_t)(deadline_ns / 1000000000ULL);
	deadline.tv_nsec = (long)(deadline_ns % 1000000000ULL);

	// Returns the error, not -1: EINTR is a signal that ended the wait
	// early, and the deadline being absolute, it is simply waited for again.
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {
	}
#else
#error "Unsupported platform"
#endif
}

void crperf_sleep_until_monotonic_ms(uint64_t deadline_ms) {
	// The same on every platform: what is left on the millisecond clock,
	// waited for on the nanosecond one, and the millisecond clock read
	// again. On Windows and Linux the two are one clock and a single pass
	// does it; on Apple they are two (see the header), which is why the
	// deadline isn't simply multiplied up.
	for (;;) {
		uint64_t now_ms = crperf_get_current_time_milliseconds();
		if (now_ms >= deadline_ms)
			return;

		// Kept to what fits in nanoseconds: a deadline further off than
		// that is waited for in more than one pass.
		uint64_t remaining_ms = deadline_ms - now_ms;
		if (remaining_ms > UINT64_MAX / 2000000ULL)
			remaining_ms = UINT64_MAX / 2000000ULL;

		crperf_sleep_until_monotonic_ns(crperf_get_monotonic_time_ns() + remaining_ms * 1000000ULL);
	}
}

#if __cplusplus
}
#endif
