#if CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE

#include "../../crmemoryshared.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

uint32_t crmem_backend_page_size(void) { return (uint32_t)sysconf(_SC_PAGESIZE); }

/* mmap places a mapping on any page boundary. */
uint32_t crmem_backend_reserve_granularity(void) { return crmem_backend_page_size(); }

CR_RESULT crmem_backend_reserve(
    uint64_t size,
    void **out_address) {
	if (size > (uint64_t)SIZE_MAX)
		return CR_RESULT_OUT_OF_MEMORY;
	/* PROT_NONE: address space only. Linux counts no memory against the
	 * commit limit for it until a commit makes it writable. */
	void *address = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED)
		return errno == ENOMEM ? CR_RESULT_OUT_OF_MEMORY : CR_RESULT_API_SPECIFIC_FAILURE;
	*out_address = address;
	return CR_SUCCESS;
}

CR_RESULT crmem_backend_commit(
    void *address,
    uint64_t size) {
	if (mprotect(address, (size_t)size, PROT_READ | PROT_WRITE) != 0)
		return errno == ENOMEM ? CR_RESULT_OUT_OF_MEMORY : CR_RESULT_API_SPECIFIC_FAILURE;
	return CR_SUCCESS;
}

CR_RESULT crmem_backend_decommit(
    void *address,
    uint64_t size) {
	/* A fresh PROT_NONE mapping over the range: the old pages go back to
	 * the system and the next commit reads zero, on Linux and Apple alike.
	 * madvise(MADV_DONTNEED) would be neither on Apple, where it is only a
	 * hint and the pages may keep what they held. */
	void *replaced = mmap(address, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (replaced == MAP_FAILED)
		return CR_RESULT_API_SPECIFIC_FAILURE;
	return CR_SUCCESS;
}

CR_RESULT crmem_backend_release(
    void *address,
    uint64_t size) {
	if (munmap(address, (size_t)size) != 0)
		return CR_RESULT_API_SPECIFIC_FAILURE;
	return CR_SUCCESS;
}

#endif // CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE
