/*
 * crmemory_test — Carrottus Memory's virtual memory calls.
 *
 * Every argument check of each call, and the rules crmemory.h states for
 * every backend: committed pages read zero, a second commit keeps what a
 * page holds, a decommit and commit again reads zero, a decommit may cover
 * pages that aren't committed, and a reservation can be far larger than
 * what is committed in it. Needs nothing but the machine.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <crmemory.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

#define CHECK_RESULT(call, expected)                                                                                         \
	do {                                                                                                                     \
		const CR_RESULT r_ = (call);                                                                                        \
		if (r_ != (expected)) {                                                                                              \
			fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, #call, cr_result_str(r_), #expected); \
			++failures;                                                                                                      \
		}                                                                                                                    \
	} while (0)

/* Whether [size] bytes at [p] are all [value]. */
static int all_bytes_are(
    const uint8_t *p,
    uint64_t size,
    uint8_t value) {
	for (uint64_t i = 0; i < size; ++i) {
		if (p[i] != value)
			return 0;
	}
	return 1;
}

static void test_page_size(void) {
	const uint32_t page = crmem_get_page_size();
	CHECK(page != 0);
	CHECK((page & (page - 1)) == 0);
	CHECK(page == crmem_get_page_size());
	printf("crmemory_test: page size %u\n", (unsigned)page);
}

/* A power of two, a page multiple, and where a reservation starts. */
static void test_reserve_granularity(void) {
	const uint32_t page        = crmem_get_page_size();
	const uint32_t granularity = crmem_get_reserve_granularity();
	CHECK(granularity != 0);
	CHECK((granularity & (granularity - 1)) == 0);
	CHECK(granularity % page == 0);
	CHECK(granularity == crmem_get_reserve_granularity());
	void *address = NULL;
	CHECK_RESULT(crmem_reserve(page, &address), CR_SUCCESS);
	CHECK(address != NULL);
	CHECK((uint64_t)(uintptr_t)address % granularity == 0);
	if (address)
		CHECK_RESULT(crmem_release(address, page), CR_SUCCESS);
	printf("crmemory_test: reserve granularity %u\n", (unsigned)granularity);
}

static void test_reserve_checks(void) {
	const uint64_t page = crmem_get_page_size();
	void *address       = (void *)&address;

	CHECK_RESULT(crmem_reserve(0, &address), CR_RESULT_ZERO_SIZE);
	CHECK(address == NULL);
	address = (void *)&address;
	CHECK_RESULT(crmem_reserve(page + 1, &address), CR_RESULT_MEM_NOT_PAGE_ALIGNED);
	CHECK(address == NULL);
	CHECK_RESULT(crmem_reserve(page - 1, &address), CR_RESULT_MEM_NOT_PAGE_ALIGNED);
	CHECK_RESULT(crmem_reserve(page, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	/* Checked in parameter order: the size before the output. */
	CHECK_RESULT(crmem_reserve(0, NULL), CR_RESULT_ZERO_SIZE);
}

/* The same four checks, in the same order, for each call that takes an
 * address and a size. */
static void test_range_checks(
    const char *name,
    CR_RESULT (*call)(void *, uint64_t),
    uint8_t *base,
    uint64_t page) {
	printf("crmemory_test: %s's checks\n", name);
	CHECK_RESULT(call(NULL, page), CR_RESULT_MEM_NULL_ADDRESS);
	CHECK_RESULT(call(base + 1, page), CR_RESULT_MEM_NOT_PAGE_ALIGNED);
	CHECK_RESULT(call(base, 0), CR_RESULT_ZERO_SIZE);
	CHECK_RESULT(call(base, page + 1), CR_RESULT_MEM_NOT_PAGE_ALIGNED);
	CHECK_RESULT(call(NULL, 0), CR_RESULT_MEM_NULL_ADDRESS);
	CHECK_RESULT(call(base + 1, 0), CR_RESULT_MEM_NOT_PAGE_ALIGNED);
}

static void test_reserve_commit_release(void) {
	const uint64_t page = crmem_get_page_size();
	/* A gigabyte of address space and a few pages of memory in it. */
	const uint64_t reserved = CRMEM_GiB(1);
	void *address           = NULL;
	CHECK_RESULT(crmem_reserve(reserved, &address), CR_SUCCESS);
	if (!address)
		return;
	CHECK(((uintptr_t)address & (page - 1)) == 0);
	uint8_t *base = (uint8_t *)address;

	test_range_checks("crmem_commit", crmem_commit, base, page);
	test_range_checks("crmem_decommit", crmem_decommit, base, page);

	/* Committed pages read zero, and can be written. */
	CHECK_RESULT(crmem_commit(base, 4 * page), CR_SUCCESS);
	CHECK(all_bytes_are(base, 4 * page, 0));
	memset(base, 0xA5, 4 * page);
	CHECK(all_bytes_are(base, 4 * page, 0xA5));

	/* A second commit over committed pages, and one more, keeps what they
	 * hold and zeroes only the new page. */
	CHECK_RESULT(crmem_commit(base, 5 * page), CR_SUCCESS);
	CHECK(all_bytes_are(base, 4 * page, 0xA5));
	CHECK(all_bytes_are(base + 4 * page, page, 0));

	/* Decommitted and committed again, a page reads zero; its neighbours
	 * are untouched. */
	CHECK_RESULT(crmem_decommit(base + page, page), CR_SUCCESS);
	CHECK_RESULT(crmem_commit(base + page, page), CR_SUCCESS);
	CHECK(all_bytes_are(base + page, page, 0));
	CHECK(all_bytes_are(base, page, 0xA5));
	CHECK(all_bytes_are(base + 2 * page, 2 * page, 0xA5));

	/* A decommit may cover pages that were never committed. */
	CHECK_RESULT(crmem_decommit(base, 64 * page), CR_SUCCESS);
	CHECK_RESULT(crmem_commit(base, page), CR_SUCCESS);
	CHECK(all_bytes_are(base, page, 0));

	/* Pages far into the reservation, and its last one. */
	uint8_t *middle = base + reserved / 2;
	uint8_t *last   = base + reserved - page;
	CHECK_RESULT(crmem_commit(middle, page), CR_SUCCESS);
	CHECK_RESULT(crmem_commit(last, page), CR_SUCCESS);
	middle[0]        = 1;
	last[page - 1]   = 2;
	CHECK(middle[0] == 1);
	CHECK(last[page - 1] == 2);

	test_range_checks("crmem_release", crmem_release, base, page);
	CHECK_RESULT(crmem_release(base, reserved), CR_SUCCESS);
}

/* A reservation larger than this machine's memory is address space only. */
static void test_large_reservation(void) {
	const uint64_t reserved = CRMEM_GiB(64);
	void *address           = NULL;
	const CR_RESULT r      = crmem_reserve(reserved, &address);
	if (r != CR_SUCCESS) {
		/* A 32-bit process, or a system that limits address space, can
		 * refuse; that is an answer, not a failure of the call. */
		printf("crmemory_test: 64 GiB not reserved: %s\n", cr_result_str(r));
		CHECK(r == CR_RESULT_OUT_OF_MEMORY);
		return;
	}
	const uint64_t page = crmem_get_page_size();
	CHECK_RESULT(crmem_commit(address, page), CR_SUCCESS);
	((uint8_t *)address)[0] = 7;
	CHECK(((uint8_t *)address)[0] == 7);
	CHECK_RESULT(crmem_release(address, reserved), CR_SUCCESS);
}

int main(void) {
	test_page_size();
	test_reserve_granularity();
	test_reserve_checks();
	test_reserve_commit_release();
	test_large_reservation();
	if (failures) {
		fprintf(stderr, "crmemory_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("crmemory_test: all passed\n");
	return 0;
}
