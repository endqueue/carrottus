#ifndef CRMEMORYSHARED_H
#define CRMEMORYSHARED_H

/*
 * What a Carrottus Memory backend provides. src/crmemory.c is the front end:
 * every argument check is there, once, and a backend is called only with
 * arguments that passed them (a non-null, page-aligned address and a
 * non-zero, page-multiple size).
 */

#include "crmemory.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t crmem_backend_page_size(void);
uint32_t crmem_backend_reserve_granularity(void);
CR_RESULT crmem_backend_reserve(
    uint64_t size,
    void **out_address);
CR_RESULT crmem_backend_commit(
    void *address,
    uint64_t size);
CR_RESULT crmem_backend_decommit(
    void *address,
    uint64_t size);
CR_RESULT crmem_backend_release(
    void *address,
    uint64_t size);

#ifdef __cplusplus
}
#endif

#endif // CRMEMORYSHARED_H
