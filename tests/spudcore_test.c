/*
 * spudcore_test — SpudCore's version, result names and system allocator.
 *
 * The version is written once, in spudcore.h. The test checks that CMake
 * read the same three numbers from it (SPUDCORE_TEST_CMAKE_VERSION, passed
 * in by CMakeLists.txt), that the library returns the version it was built
 * with, and that the packing macros round-trip and order versions. It also
 * checks that spud_allocator_system() keeps SPUD_ALLOCATOR's rules for
 * alignments below and above max_align_t's.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <spudcore.h>

#include <stddef.h>
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

static void test_version(void) {
	/* Built together, the library and its caller have one version. */
	CHECK(spud_get_version() == SPUDLIB_VERSION);
	CHECK(spud_get_version() != 0);

	char header[32];
	snprintf(header, sizeof(header), "%u.%u.%u", (unsigned)SPUDLIB_VERSION_MAJOR, (unsigned)SPUDLIB_VERSION_MINOR, (unsigned)SPUDLIB_VERSION_PATCH);
	CHECK(strcmp(header, SPUDCORE_TEST_CMAKE_VERSION) == 0);

	CHECK(SPUDLIB_VERSION_GET_MAJOR(SPUDLIB_VERSION) == SPUDLIB_VERSION_MAJOR);
	CHECK(SPUDLIB_VERSION_GET_MINOR(SPUDLIB_VERSION) == SPUDLIB_VERSION_MINOR);
	CHECK(SPUDLIB_VERSION_GET_PATCH(SPUDLIB_VERSION) == SPUDLIB_VERSION_PATCH);

	/* Each part at its largest, and next to its neighbours. */
	const uint32_t top = SPUDLIB_MAKE_VERSION(1023, 1023, 4095);
	CHECK(top == 0xFFFFFFFFu);
	CHECK(SPUDLIB_VERSION_GET_MAJOR(top) == 1023);
	CHECK(SPUDLIB_VERSION_GET_MINOR(top) == 1023);
	CHECK(SPUDLIB_VERSION_GET_PATCH(top) == 4095);
	CHECK(SPUDLIB_VERSION_GET_MINOR(SPUDLIB_MAKE_VERSION(1, 0, 4095)) == 0);
	CHECK(SPUDLIB_VERSION_GET_MAJOR(SPUDLIB_MAKE_VERSION(0, 1023, 4095)) == 0);

	/* A later version is a larger number, whichever part grows. */
	CHECK(SPUDLIB_MAKE_VERSION(0, 1, 0) < SPUDLIB_MAKE_VERSION(0, 1, 1));
	CHECK(SPUDLIB_MAKE_VERSION(0, 1, 4095) < SPUDLIB_MAKE_VERSION(0, 2, 0));
	CHECK(SPUDLIB_MAKE_VERSION(0, 1023, 4095) < SPUDLIB_MAKE_VERSION(1, 0, 0));

	printf("spudcore_test: spudlib %s\n", header);
}

static void test_allocator_system(void) {
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	CHECK(allocator.allocate != NULL);
	CHECK(allocator.free != NULL);
	CHECK(allocator.user_data == NULL);

	/* Every power of two from 1 to 64 KiB, at sizes that are and aren't
	 * multiples of it. Each block is written whole, then freed with the size
	 * and alignment it was allocated with. */
	const size_t sizes[] = {1, 3, 64, 1000, 65536 + 7};
	for (size_t alignment = 1; alignment <= 65536; alignment <<= 1) {
		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
			void *block = allocator.allocate(allocator.user_data, sizes[i], alignment);
			CHECK(block != NULL);
			if (!block)
				continue;
			CHECK(((uintptr_t)block & (alignment - 1)) == 0);
			memset(block, 0xA5, sizes[i]);
			allocator.free(allocator.user_data, block, sizes[i], alignment);
		}
	}

	/* A size no rounding can hold is refused, not wrapped to a small block. */
	CHECK(allocator.allocate(allocator.user_data, SIZE_MAX - 8, 4096) == NULL);

	CHECK(strcmp(spudresult_str(SPUDRESULT_NULL_ALLOCATOR), "SPUDRESULT_NULL_ALLOCATOR") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_INVALID_ALLOCATOR), "SPUDRESULT_INVALID_ALLOCATOR") == 0);
}

static void test_result_names(void) {
	CHECK(strcmp(spudresult_str(SPUD_SUCCESS), "SPUD_SUCCESS") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_GPU_INVALID_HEAP), "SPUDRESULT_GPU_INVALID_HEAP") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED), "SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_SPUDFILE_INVALID_LIBRARY), "SPUDRESULT_SPUDFILE_INVALID_LIBRARY") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED), "SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_SPUDFILE_SYMBOL_NOT_FOUND), "SPUDRESULT_SPUDFILE_SYMBOL_NOT_FOUND") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED), "SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED") == 0);
	CHECK(strcmp(spudresult_str((SPUDRESULT)123456), "SPUDRESULT_UNKNOWN") == 0);
}

int main(void) {
	test_version();
	test_result_names();
	test_allocator_system();

	if (failures)
		fprintf(stderr, "spudcore_test: %d failure(s)\n", failures);
	else
		printf("spudcore_test: all passed\n");
	return failures ? 1 : 0;
}
