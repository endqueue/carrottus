/**
 * @file crrandom.h
 * @brief Carrottus Random: the operating system's cryptographically secure
 * random bytes.
 *
 * One call, which asks the operating system for bytes from its own secure
 * generator and returns them. Nothing is seeded, kept or mixed here: there
 * is no generator of Carrottus's, no state and no handle. What source each
 * platform reads:
 *
 * | Platform | Source |
 * |---|---|
 * | Windows | `BCryptGenRandom` with `BCRYPT_USE_SYSTEM_PREFERRED_RNG` |
 * | Linux | `getrandom(2)`, blocking only until the kernel's pool is first ready |
 * | Apple (macOS, iOS, iPadOS) | `SecRandomCopyBytes` with `kSecRandomDefault` |
 *
 * @par Threads
 * No call takes an object, so under the library rule (crcore.h, "Threads")
 * any call may overlap any other.
 */

#ifndef CRRANDOM_H
#define CRRANDOM_H

#include "crcore.h"

#include <stddef.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Fills memory with random bytes from the operating system's secure
 * generator (top of this header).
 *
 * Thread: any - it takes no object, so it may overlap any call.
 *
 * @param out  Receives size bytes. Required unless size is 0.
 * @param size Number of bytes. 0 asks for none and succeeds.
 * @return CR_SUCCESS; CR_RESULT_NULL_OUTPUT_PARAMETER for a null out with
 *         a size; CR_RESULT_API_SPECIFIC_FAILURE if the operating system
 *         gave fewer bytes than asked, when out holds nothing to rely on.
 */
CR_RESULT crrandom_fill(
    void *out,
    size_t size);

#if __cplusplus
}
#endif

#endif // CRRANDOM_H
