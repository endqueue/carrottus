#include "crrandom.h"

#if CARROTTUS_PLATFORM_WIN32
/* bcrypt.h needs windows.h's types (NTSTATUS, ULONG) declared before it,
 * and WIN32_LEAN_AND_MEAN leaves it out of windows.h. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif CARROTTUS_PLATFORM_LINUX
#include <errno.h>
#include <sys/random.h>
#elif CARROTTUS_PLATFORM_APPLE
/* SecRandomCopyBytes, not getentropy: getentropy has no public declaration
 * in the iOS SDKs, and this is the one call Apple documents on all of its
 * platforms. */
#include <Security/SecRandom.h>
#endif

#if __cplusplus
extern "C" {
#endif

CR_RESULT crrandom_fill(
    void *out,
    size_t size) {
	/* out null: refused only when there are bytes to write. */
	if (!out && size > 0)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (size == 0)
		return CR_SUCCESS;

	unsigned char *at = (unsigned char *)out;
#if CARROTTUS_PLATFORM_WIN32
	/* BCryptGenRandom takes a ULONG count: past it, in pieces. */
	while (size > 0) {
		const ULONG piece = size > 0x80000000u ? 0x80000000u : (ULONG)size;
		if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, at, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
			return CR_RESULT_API_SPECIFIC_FAILURE;
		at += piece;
		size -= piece;
	}
	return CR_SUCCESS;
#elif CARROTTUS_PLATFORM_LINUX
	/* A read may come back short, or be interrupted by a signal: on until
	 * every byte is there. */
	while (size > 0) {
		const ssize_t read = getrandom(at, size, 0);
		if (read < 0) {
			if (errno == EINTR)
				continue;
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
		at += (size_t)read;
		size -= (size_t)read;
	}
	return CR_SUCCESS;
#elif CARROTTUS_PLATFORM_APPLE
	if (SecRandomCopyBytes(kSecRandomDefault, size, at) != errSecSuccess)
		return CR_RESULT_API_SPECIFIC_FAILURE;
	return CR_SUCCESS;
#else
	(void)at;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
#endif
}

#if __cplusplus
}
#endif
