#ifndef CRMEMORY_H
#define CRMEMORY_H

/**
 * @file crmemory.h
 * @brief Carrottus Memory: the platform's virtual memory, a page at a time.
 *
 * Address space is reserved, then pages of it are committed (given memory
 * the caller can read and write), decommitted (their memory given back, the
 * address space kept) and the whole reservation released. That is all: how
 * much to reserve, when to commit, and how the pages are divided up are the
 * caller's.
 *
 * Backends: VirtualAlloc/VirtualFree on Windows; mmap/mprotect/munmap on
 * Linux and every Apple system.
 *
 * Rules that hold on every backend:
 *
 * - Every size, and every address passed in, is a multiple of the page size
 *   (crmem_get_page_size()). Nothing is rounded: a size or address that isn't
 *   one is refused with CR_RESULT_MEM_NOT_PAGE_ALIGNED.
 * - A committed page reads zero until it is written, the first time and
 *   every time it is committed again after a decommit. Committing a page
 *   that is already committed leaves its contents as they are.
 * - Reading or writing a page that isn't committed is undefined (it
 *   faults on every backend today).
 * - The pages committed and decommitted are within one reservation, and a
 *   release gives back the whole of one: the address and size
 *   crmem_reserve() returned. Anything else is undefined (Windows can't
 *   release part of a reservation).
 *
 * @par Threads
 * No call takes an object or holds state, so under the library rule
 * (@ref cr_threads "Threads", crcore.h) any call may overlap any other.
 * Two calls changing the same pages at once are the caller's to prevent.
 */

#include <crcore.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/** @brief @p n kibibytes, in bytes, as a uint64_t. */
#define CRMEM_KiB(n) ((uint64_t)(n) << 10)
/** @brief @p n mebibytes, in bytes, as a uint64_t. */
#define CRMEM_MiB(n) ((uint64_t)(n) << 20)
/** @brief @p n gibibytes, in bytes, as a uint64_t. */
#define CRMEM_GiB(n) ((uint64_t)(n) << 30)

/**
 * @brief The size of a page, in bytes: what every size and address the
 * other calls take is a multiple of. A power of two; the same for the life
 * of the process. 4 KiB on most x86-64 systems, 16 KiB on Apple Silicon.
 * @return The page size in bytes.
 */
uint32_t crmem_get_page_size(void);

/**
 * @brief The reservation granularity, in bytes: where the system starts a
 * reservation, and so the size below which a reservation strands address
 * space no other reservation can use. A power of two and a multiple of the
 * page size; the same for the life of the process. 64 KiB on Windows
 * (`dwAllocationGranularity`); the page size on Linux and every Apple
 * system.
 *
 * crmem_reserve() still takes any multiple of the page size: a caller
 * that reserves many small ranges reads this to size them, and nothing is
 * rounded for it.
 *
 * @return The reservation granularity in bytes.
 */
uint32_t crmem_get_reserve_granularity(void);

/**
 * @brief Reserves address space, none of it committed.
 *
 * @param[in]  size        Bytes to reserve: a multiple of the page size.
 * @param[out] out_address Receives the start of the reservation, a
 *                         multiple of the reservation granularity
 *                         (crmem_get_reserve_granularity()); NULL on
 *                         failure.
 *
 * @retval CR_SUCCESS The space is reserved.
 * @retval CR_RESULT_ZERO_SIZE `size` is 0.
 * @retval CR_RESULT_MEM_NOT_PAGE_ALIGNED `size` is not a multiple of the
 *         page size.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The system has no address space or
 *         memory left for it.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform refused for another
 *         reason.
 */
CR_RESULT crmem_reserve(
    uint64_t size,
    void **out_address);

/**
 * @brief Commits pages of a reservation, so that they can be read and
 * written.
 *
 * @param[in] address The first page: a multiple of the page size.
 * @param[in] size    Bytes to commit: a multiple of the page size.
 *
 * @retval CR_SUCCESS The pages are committed.
 * @retval CR_RESULT_MEM_NULL_ADDRESS `address` is NULL.
 * @retval CR_RESULT_MEM_NOT_PAGE_ALIGNED `address` or `size` is not a
 *         multiple of the page size.
 * @retval CR_RESULT_ZERO_SIZE `size` is 0.
 * @retval CR_RESULT_OUT_OF_MEMORY The system has no memory left for them.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform refused for another
 *         reason.
 */
CR_RESULT crmem_commit(
    void *address,
    uint64_t size);

/**
 * @brief Decommits pages of a reservation: their memory goes back to the
 * system, and the address space stays reserved. Pages that aren't committed
 * may be in the range.
 *
 * @param[in] address The first page: a multiple of the page size.
 * @param[in] size    Bytes to decommit: a multiple of the page size.
 *
 * @retval CR_SUCCESS The pages are decommitted.
 * @retval CR_RESULT_MEM_NULL_ADDRESS `address` is NULL.
 * @retval CR_RESULT_MEM_NOT_PAGE_ALIGNED `address` or `size` is not a
 *         multiple of the page size.
 * @retval CR_RESULT_ZERO_SIZE `size` is 0.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform refused.
 */
CR_RESULT crmem_decommit(
    void *address,
    uint64_t size);

/**
 * @brief Releases a whole reservation, committed pages and all.
 *
 * @param[in] address The start crmem_reserve() returned.
 * @param[in] size    The size it was reserved with.
 *
 * @retval CR_SUCCESS The reservation is gone.
 * @retval CR_RESULT_MEM_NULL_ADDRESS `address` is NULL.
 * @retval CR_RESULT_MEM_NOT_PAGE_ALIGNED `address` or `size` is not a
 *         multiple of the page size.
 * @retval CR_RESULT_ZERO_SIZE `size` is 0.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform refused.
 */
CR_RESULT crmem_release(
    void *address,
    uint64_t size);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CRMEMORY_H
