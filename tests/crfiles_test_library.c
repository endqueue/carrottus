/*
 * crfiles_test_library.c - the small dynamic libraries crfiles_test
 * opens (card S44). One source, built as several libraries by
 * CMakeLists.txt, each with its own defines:
 *
 * - CRFILES_TEST_LIBRARY_VALUE: what crfiles_test_value() returns (1 for
 *   crfiles_test_library_a, 2 for _b).
 * - CRFILES_TEST_LIBRARY_NEEDS_DEPENDENCY: imports a function of
 *   crfiles_test_dependency, which is built into a directory no search
 *   reaches, so the loader can't find it.
 * - CRFILES_TEST_LIBRARY_FAILS_INIT: its initializer fails (Windows only:
 *   DllMain returns FALSE; a POSIX constructor can't fail).
 * - CRFILES_TEST_DEPENDENCY: the dependency itself.
 *
 * Each library counts its initializer's runs (crfiles_test_initialized)
 * and a counter a caller bumps (crfiles_test_bump): two handles over one
 * image share them, two images don't.
 */

#if defined(_WIN32)
#define CRFILES_TEST_EXPORT __declspec(dllexport)
#define CRFILES_TEST_IMPORT __declspec(dllimport)
#else
#define CRFILES_TEST_EXPORT __attribute__((visibility("default")))
#define CRFILES_TEST_IMPORT
#endif

#if defined(CRFILES_TEST_DEPENDENCY)

CRFILES_TEST_EXPORT int crfiles_test_dependency_value(void) { return 7; }

#else

#if defined(CRFILES_TEST_LIBRARY_NEEDS_DEPENDENCY)
CRFILES_TEST_IMPORT int crfiles_test_dependency_value(void);
#endif

static int initialized = 0;
static int bumped      = 0;

#if defined(_WIN32)
#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		++initialized;
#if defined(CRFILES_TEST_LIBRARY_FAILS_INIT)
		return FALSE;
#endif
	}
	return TRUE;
}
#else
__attribute__((constructor)) static void crfiles_test_initialize(void) { ++initialized; }
#endif

CRFILES_TEST_EXPORT int crfiles_test_value(void) {
#if defined(CRFILES_TEST_LIBRARY_NEEDS_DEPENDENCY)
	return crfiles_test_dependency_value();
#else
	return CRFILES_TEST_LIBRARY_VALUE;
#endif
}

CRFILES_TEST_EXPORT int crfiles_test_initialized(void) { return initialized; }

CRFILES_TEST_EXPORT int crfiles_test_bump(void) { return ++bumped; }

#endif
