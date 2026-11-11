#include "crmemory.h"
#include "memory/crmemoryshared.h"

#include <stdbool.h>
#include <stdint.h>

/* Whether [value] is a multiple of the page size, which is a power of two. */
static bool crmem_page_multiple(uint64_t value) { return (value & ((uint64_t)crmem_backend_page_size() - 1)) == 0; }

uint32_t crmem_get_page_size(void) { return crmem_backend_page_size(); }

uint32_t crmem_get_reserve_granularity(void) { return crmem_backend_reserve_granularity(); }

CR_RESULT crmem_reserve(
    uint64_t size,
    void **out_address) {
	if (out_address)
		*out_address = NULL;
	if (size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!crmem_page_multiple(size))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	if (!out_address)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	return crmem_backend_reserve(size, out_address);
}

CR_RESULT crmem_commit(
    void *address,
    uint64_t size) {
	if (!address)
		return CR_RESULT_MEM_NULL_ADDRESS;
	if (!crmem_page_multiple((uint64_t)(uintptr_t)address))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	if (size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!crmem_page_multiple(size))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	return crmem_backend_commit(address, size);
}

CR_RESULT crmem_decommit(
    void *address,
    uint64_t size) {
	if (!address)
		return CR_RESULT_MEM_NULL_ADDRESS;
	if (!crmem_page_multiple((uint64_t)(uintptr_t)address))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	if (size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!crmem_page_multiple(size))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	return crmem_backend_decommit(address, size);
}

CR_RESULT crmem_release(
    void *address,
    uint64_t size) {
	if (!address)
		return CR_RESULT_MEM_NULL_ADDRESS;
	if (!crmem_page_multiple((uint64_t)(uintptr_t)address))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	if (size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!crmem_page_multiple(size))
		return CR_RESULT_MEM_NOT_PAGE_ALIGNED;
	return crmem_backend_release(address, size);
}
