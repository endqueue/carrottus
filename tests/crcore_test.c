/*
 * crcore_test — Carrottus Core's version, result names and system allocator.
 *
 * The version is written once, in crcore.h. The test checks that CMake
 * read the same three numbers from it (CRCORE_TEST_CMAKE_VERSION, passed
 * in by CMakeLists.txt), that the library returns the version it was built
 * with, and that the packing macros round-trip and order versions. It also
 * checks that cr_allocator_system() keeps CR_ALLOCATOR's rules for
 * alignments below and above max_align_t's.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <crcore.h>

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
	CHECK(cr_get_version() == CARROTTUS_VERSION);
	CHECK(cr_get_version() != 0);

	char header[32];
	snprintf(header, sizeof(header), "%u.%u.%u", (unsigned)CARROTTUS_VERSION_MAJOR, (unsigned)CARROTTUS_VERSION_MINOR, (unsigned)CARROTTUS_VERSION_PATCH);
	CHECK(strcmp(header, CRCORE_TEST_CMAKE_VERSION) == 0);

	CHECK(CARROTTUS_VERSION_GET_MAJOR(CARROTTUS_VERSION) == CARROTTUS_VERSION_MAJOR);
	CHECK(CARROTTUS_VERSION_GET_MINOR(CARROTTUS_VERSION) == CARROTTUS_VERSION_MINOR);
	CHECK(CARROTTUS_VERSION_GET_PATCH(CARROTTUS_VERSION) == CARROTTUS_VERSION_PATCH);

	/* Each part at its largest, and next to its neighbours. */
	const uint32_t top = CARROTTUS_MAKE_VERSION(1023, 1023, 4095);
	CHECK(top == 0xFFFFFFFFu);
	CHECK(CARROTTUS_VERSION_GET_MAJOR(top) == 1023);
	CHECK(CARROTTUS_VERSION_GET_MINOR(top) == 1023);
	CHECK(CARROTTUS_VERSION_GET_PATCH(top) == 4095);
	CHECK(CARROTTUS_VERSION_GET_MINOR(CARROTTUS_MAKE_VERSION(1, 0, 4095)) == 0);
	CHECK(CARROTTUS_VERSION_GET_MAJOR(CARROTTUS_MAKE_VERSION(0, 1023, 4095)) == 0);

	/* A later version is a larger number, whichever part grows. */
	CHECK(CARROTTUS_MAKE_VERSION(0, 1, 0) < CARROTTUS_MAKE_VERSION(0, 1, 1));
	CHECK(CARROTTUS_MAKE_VERSION(0, 1, 4095) < CARROTTUS_MAKE_VERSION(0, 2, 0));
	CHECK(CARROTTUS_MAKE_VERSION(0, 1023, 4095) < CARROTTUS_MAKE_VERSION(1, 0, 0));

	printf("crcore_test: carrottus %s\n", header);
}

static void test_allocator_system(void) {
	const CR_ALLOCATOR allocator = cr_allocator_system();
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

	CHECK(strcmp(cr_result_str(CR_RESULT_NULL_ALLOCATOR), "CR_RESULT_NULL_ALLOCATOR") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_INVALID_ALLOCATOR), "CR_RESULT_INVALID_ALLOCATOR") == 0);
}

static void test_result_names(void) {
	CHECK(strcmp(cr_result_str(CR_SUCCESS), "CR_SUCCESS") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_GPU_INVALID_HEAP), "CR_RESULT_GPU_INVALID_HEAP") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED), "CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_FILES_INVALID_LIBRARY), "CR_RESULT_FILES_INVALID_LIBRARY") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_FILES_LIBRARY_NOT_LOADED), "CR_RESULT_FILES_LIBRARY_NOT_LOADED") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_FILES_SYMBOL_NOT_FOUND), "CR_RESULT_FILES_SYMBOL_NOT_FOUND") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED), "CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED") == 0);
	CHECK(strcmp(cr_result_str((CR_RESULT)123456), "CR_RESULT_UNKNOWN") == 0);
}

int main(void) {
	test_version();
	test_result_names();
	test_allocator_system();

	if (failures)
		fprintf(stderr, "crcore_test: %d failure(s)\n", failures);
	else
		printf("crcore_test: all passed\n");
	return failures ? 1 : 0;
}
