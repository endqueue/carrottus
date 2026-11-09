/*
 * spudfiles_test_library.c - the small dynamic libraries spudfiles_test
 * opens (card S44). One source, built as several libraries by
 * CMakeLists.txt, each with its own defines:
 *
 * - SPUDFILES_TEST_LIBRARY_VALUE: what spudfiles_test_value() returns (1 for
 *   spudfiles_test_library_a, 2 for _b).
 * - SPUDFILES_TEST_LIBRARY_NEEDS_DEPENDENCY: imports a function of
 *   spudfiles_test_dependency, which is built into a directory no search
 *   reaches, so the loader can't find it.
 * - SPUDFILES_TEST_LIBRARY_FAILS_INIT: its initializer fails (Windows only:
 *   DllMain returns FALSE; a POSIX constructor can't fail).
 * - SPUDFILES_TEST_DEPENDENCY: the dependency itself.
 *
 * Each library counts its initializer's runs (spudfiles_test_initialized)
 * and a counter a caller bumps (spudfiles_test_bump): two handles over one
 * image share them, two images don't.
 */

#if defined(_WIN32)
#define SPUDFILES_TEST_EXPORT __declspec(dllexport)
#define SPUDFILES_TEST_IMPORT __declspec(dllimport)
#else
#define SPUDFILES_TEST_EXPORT __attribute__((visibility("default")))
#define SPUDFILES_TEST_IMPORT
#endif

#if defined(SPUDFILES_TEST_DEPENDENCY)

SPUDFILES_TEST_EXPORT int spudfiles_test_dependency_value(void) { return 7; }

#else

#if defined(SPUDFILES_TEST_LIBRARY_NEEDS_DEPENDENCY)
SPUDFILES_TEST_IMPORT int spudfiles_test_dependency_value(void);
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
#if defined(SPUDFILES_TEST_LIBRARY_FAILS_INIT)
		return FALSE;
#endif
	}
	return TRUE;
}
#else
__attribute__((constructor)) static void spudfiles_test_initialize(void) { ++initialized; }
#endif

SPUDFILES_TEST_EXPORT int spudfiles_test_value(void) {
#if defined(SPUDFILES_TEST_LIBRARY_NEEDS_DEPENDENCY)
	return spudfiles_test_dependency_value();
#else
	return SPUDFILES_TEST_LIBRARY_VALUE;
#endif
}

SPUDFILES_TEST_EXPORT int spudfiles_test_initialized(void) { return initialized; }

SPUDFILES_TEST_EXPORT int spudfiles_test_bump(void) { return ++bumped; }

#endif
