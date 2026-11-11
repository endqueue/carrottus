#ifndef CRALLOCATOR_H
#define CRALLOCATOR_H

// How Carrottus takes memory from the caller's allocator (CR_ALLOCATOR,
// crcore.h), written once so that every module and backend allocates,
// frees and checks it the same way.
//
// Every handle's first member is a cr_object_head: the handle's debug name,
// then a copy of the allocator its instance was made with, then in a Debug
// build the tag that catches a destroyed handle (crstale.h). A handle frees
// through its own copy, its name included, so no destroy call takes an
// allocator, and a handle that outlasts its instance (a Carrottus Net resolver whose
// lookup is still out) still has it. A pointer to a handle converts to a pointer to its head
// (C11 6.7.2.1p15), which is how cr_debug_name_set reaches both from a
// void *.
//
// CR_ALLOCATOR's contract says Carrottus never asks for 0 bytes. These
// functions keep it by asking for 1 byte where the size is 0, at allocation
// and at free alike, so a block of a count that may be 0 is allocated and
// freed with the same count and needs no case of its own.

#include "crcore.h"
#include "crstale.h"
#include <assert.h>
#include <eqstd.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(__cplusplus)
#define CR_ALIGNOF(T) alignof(T)
#else
#define CR_ALIGNOF(T) _Alignof(T)
#endif

// The alignment of max_align_t: what a block that may hold any scalar type is
// allocated and freed with. MSVC's C compiler has no max_align_t (its
// <stddef.h> declares one for C++ only, as double), so there it is double's,
// the same value MSVC's C++ gives.
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
#define CR_MAX_ALIGN _Alignof(double)
#else
#define CR_MAX_ALIGN CR_ALIGNOF(max_align_t)
#endif

typedef struct cr_object_head {
	// NULL, or a copy made through [allocator].
	const char *debug_name;
	// The allocator the handle was made with and frees through: a copy of
	// its instance's.
	CR_ALLOCATOR allocator;
#if _DEBUG
	// CR_LIVE while the handle lives, CR_DEAD from its destroy call on
	// (crstale.h). Debug builds only: a Release head is the two members
	// above.
	uint64_t debug_tag;
	// The quarantine the handle's block goes to when it is destroyed: its
	// owner's, or NULL where it has none and is freed at once (crstale.h,
	// card X27). Set with the tag, by CR_BIRTH or CR_BIRTH_IN.
	struct cr_quarantine *debug_quarantine;
#endif
} cr_object_head;

// The check a create call makes of a desc's allocator, right after the
// desc's struct check.
static inline CR_RESULT cr_allocator_check(const CR_ALLOCATOR *allocator) {
	if (!allocator)
		return CR_RESULT_NULL_ALLOCATOR;
	if (!allocator->allocate || !allocator->free)
		return CR_RESULT_INVALID_ALLOCATOR;
	return CR_SUCCESS;
}

// A block of [size] bytes (1 if [size] is 0), contents undefined, or NULL.
static inline void *cr_allocate(const CR_ALLOCATOR *allocator, size_t size, size_t alignment) {
	assert(alignment && !(alignment & (alignment - 1)));
	return allocator->allocate(allocator->user_data, size ? size : 1, alignment);
}

// cr_allocate, zeroed: what calloc was.
static inline void *cr_allocate_zeroed(const CR_ALLOCATOR *allocator, size_t size, size_t alignment) {
	void *memory = cr_allocate(allocator, size, alignment);
	if (memory)
		memset(memory, 0, size);
	return memory;
}

// Frees a block cr_allocate gave, with the same [size] and [alignment].
// NULL is nothing to free.
static inline void cr_free(const CR_ALLOCATOR *allocator, const void *memory, size_t size, size_t alignment) {
	if (memory)
		allocator->free(allocator->user_data, (void *)memory, size ? size : 1, alignment);
}

// ---- The quarantine (crstale.h, card X27; Debug builds only) ----
//
// A list of the dead handles' blocks an owner holds until it is destroyed
// itself. A block is linked through its own memory: its head's debug name
// and allocator, which the destroy has already freed and copied out, hold
// the link, the size and the alignment, and the tag after them stays
// CR_DEAD. So retiring a block allocates nothing and can't fail. Blocks
// are pushed with a compare-and-swap, since an owner's objects may be
// destroyed on different threads at once; the list is emptied only by the
// owner's own destroy, when nothing else may be in it.
//
// Release has none of it: CR_RETIRE is CR_DELETE, the other macros are
// nothing, and an owner's quarantine member exists only under _DEBUG.
#if _DEBUG
#if defined(_MSC_VER)
#include <intrin.h>
#endif

typedef struct cr_quarantined {
	struct cr_quarantined *next;
	size_t size;
	size_t alignment;
} cr_quarantined;

#if defined(__cplusplus)
static_assert(sizeof(cr_quarantined) <= offsetof(cr_object_head, debug_tag), "a retired block's link overwrites its tag");
#else
_Static_assert(sizeof(cr_quarantined) <= offsetof(cr_object_head, debug_tag), "a retired block's link overwrites its tag");
#endif

typedef struct cr_quarantine {
	// The last block retired, linked to the one before; NULL when empty.
	cr_quarantined *volatile first;
	// The owner's allocator, which every block it holds was allocated
	// through (each handle's head holds a copy of its owner's).
	CR_ALLOCATOR allocator;
} cr_quarantine;

static inline void cr_quarantine_init(cr_quarantine *quarantine, const CR_ALLOCATOR *allocator) {
	quarantine->first     = NULL;
	quarantine->allocator = *allocator;
}

// Puts [block] first in [quarantine]'s list, whatever another thread pushes
// at the same time.
static inline void cr_quarantine_push(cr_quarantine *quarantine, cr_quarantined *block) {
	// The first read is atomic too: a plain one beside another thread's
	// compare-and-swap is a data race in C11 (MSVC's volatile reads are
	// atomic, clang's and GCC's aren't; ThreadSanitizer found it).
#if defined(_MSC_VER)
	for (;;) {
		cr_quarantined *first = quarantine->first;
		block->next             = first;
		if (_InterlockedCompareExchangePointer((void *volatile *)&quarantine->first, block, first) == first)
			return;
	}
#else
	cr_quarantined *first = __atomic_load_n(&quarantine->first, __ATOMIC_RELAXED);
	for (;;) {
		block->next = first;
		// On failure [first] is reloaded with what another thread pushed.
		if (__atomic_compare_exchange_n(&quarantine->first, &first, block, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
			return;
	}
#endif
}

// The quarantine [obj]'s block goes to at its destroy: its owner's, or NULL
// where it has none, or while it isn't dead (a create's failure path frees
// a block no caller held). Read before a C++ destructor runs.
static inline cr_quarantine *cr_quarantine_of(const cr_object_head *head) {
	return head->debug_tag == CR_DEAD ? head->debug_quarantine : NULL;
}

// What a destroy does with a handle's block in place of cr_free: keeps it
// in [quarantine], or frees it at once through [allocator] where that is
// NULL. [memory] starts with a cr_object_head; [size] and [alignment] are
// what it was allocated with, which it is freed with later.
static inline void cr_retire(cr_quarantine *quarantine, const CR_ALLOCATOR *allocator, void *memory, size_t size, size_t alignment) {
	if (!memory)
		return;
	if (!quarantine) {
		cr_free(allocator, memory, size, alignment);
		return;
	}
	assert(allocator->user_data == quarantine->allocator.user_data && allocator->free == quarantine->allocator.free);
	cr_quarantined *block = (cr_quarantined *)memory;
	block->size             = size;
	block->alignment        = alignment;
	cr_quarantine_push(quarantine, block);
}

// Frees every block [quarantine] holds, each as its destroy would have:
// the owner's last step before freeing itself. Nothing may retire into it
// at the same time.
static inline void cr_quarantine_release(cr_quarantine *quarantine) {
	cr_quarantined *block = quarantine->first;
	quarantine->first       = NULL;
	while (block) {
		cr_quarantined *next = block->next;
		cr_free(&quarantine->allocator, block, block->size, block->alignment);
		block = next;
	}
}

#define CR_QUARANTINE_INIT(quarantine, allocator) cr_quarantine_init((quarantine), (allocator))
#define CR_QUARANTINE_RELEASE(quarantine) cr_quarantine_release(quarantine)
#define CR_QUARANTINE_OF(obj) cr_quarantine_of(CR_HEAD_OF(obj))
// For a C++ destroy, which runs the destructor between reading the head and
// retiring the block: [name] holds CR_QUARANTINE_OF(obj) from here on.
#define CR_QUARANTINE_KEEP(name, obj) cr_quarantine *const name = CR_QUARANTINE_OF(obj)
#define CR_RETIRE(quarantine, a, T, p) cr_retire((quarantine), (a), (p), sizeof(T), CR_ALIGNOF(T))
// An array of handles, its first element's head at the block's start.
#define CR_RETIRE_ARRAY(quarantine, a, T, p, n) cr_retire((quarantine), (a), (p), (n) * sizeof(T), CR_ALIGNOF(T))
#else
#define CR_QUARANTINE_INIT(quarantine, allocator) ((void)0)
#define CR_QUARANTINE_RELEASE(quarantine) ((void)0)
#define CR_QUARANTINE_KEEP(name, obj) ((void)0)
#define CR_RETIRE(quarantine, a, T, p) CR_DELETE(a, T, p)
#define CR_RETIRE_ARRAY(quarantine, a, T, p, n) CR_DELETE_ARRAY(a, T, p, n)
#endif

// [count] elements of [size] bytes, or NULL, also when the product would
// overflow.
static inline void *cr_allocate_array(const CR_ALLOCATOR *allocator, size_t count, size_t size, size_t alignment, bool zeroed) {
	if (size && count > SIZE_MAX / size)
		return NULL;
	return zeroed ? cr_allocate_zeroed(allocator, count * size, alignment) : cr_allocate(allocator, count * size, alignment);
}

static inline void cr_free_array(const CR_ALLOCATOR *allocator, const void *memory, size_t count, size_t size, size_t alignment) {
	cr_free(allocator, memory, count * size, alignment);
}

// What realloc was, for an array whose count the caller keeps: a block of
// [new_count] elements holding the first min(old, new) of [memory]'s, the old
// block freed. On failure, NULL, and [memory] is untouched and still the
// caller's. [memory] may be NULL with an [old_count] of 0. Elements past the
// old count are undefined.
static inline void *cr_reallocate_array(
    const CR_ALLOCATOR *allocator,
    void *memory,
    size_t old_count,
    size_t new_count,
    size_t size,
    size_t alignment) {
	void *moved = cr_allocate_array(allocator, new_count, size, alignment, false);
	if (!moved)
		return NULL;
	if (memory) {
		memcpy(moved, memory, (old_count < new_count ? old_count : new_count) * size);
		cr_free_array(allocator, memory, old_count, size, alignment);
	}
	return moved;
}

// A copy of [string] made through [allocator], or NULL if it can't be made.
// [string] is not NULL.
static inline char *cr_string_copy(const CR_ALLOCATOR *allocator, const char *string) {
	size_t size = strlen(string) + 1;
	char *copy  = (char *)cr_allocate(allocator, size, 1);
	if (copy)
		memcpy(copy, string, size);
	return copy;
}

// Frees a copy cr_string_copy made. NULL is nothing to free.
static inline void cr_string_free(const CR_ALLOCATOR *allocator, const char *string) {
	if (string)
		cr_free(allocator, string, strlen(string) + 1, 1);
}

// [allocator] as eqstd takes one, for an EQ_ARRAY (or an EQ_STRING) whose
// memory is the caller's. CR_ALLOCATOR's three fields are EQ_ALLOCATOR's
// first three, with the same function types, and eqstd grows a block by
// allocating, copying and freeing when `resize` is NULL, as
// cr_reallocate_array does. An array keeps a pointer to its EQ_ALLOCATOR,
// so the result is stored beside the array, in the object that owns both
// (never a local that the array outlives). eqstd keeps CR_ALLOCATOR's
// contract: it never asks for 0 bytes, and frees each block with the size and
// alignment it was allocated with (EQ_ARRAY_ALIGNMENT of the element's size
// for an array, a multiple of the element's alignment).
static inline EQ_ALLOCATOR cr_eq_allocator(const CR_ALLOCATOR *allocator) {
	EQ_ALLOCATOR eq = {allocator->user_data, allocator->allocate, allocator->free, NULL};
	return eq;
}

// One element of type T, or an array of [n], made and freed through [a].
#define CR_NEW(a, T) ((T *)cr_allocate((a), sizeof(T), CR_ALIGNOF(T)))
#define CR_NEW_ZEROED(a, T) ((T *)cr_allocate_zeroed((a), sizeof(T), CR_ALIGNOF(T)))
#define CR_DELETE(a, T, p) cr_free((a), (p), sizeof(T), CR_ALIGNOF(T))
#define CR_NEW_ARRAY(a, T, n) ((T *)cr_allocate_array((a), (n), sizeof(T), CR_ALIGNOF(T), false))
#define CR_NEW_ARRAY_ZEROED(a, T, n) ((T *)cr_allocate_array((a), (n), sizeof(T), CR_ALIGNOF(T), true))
#define CR_DELETE_ARRAY(a, T, p, n) cr_free_array((a), (p), (n), sizeof(T), CR_ALIGNOF(T))
#define CR_RESIZE_ARRAY(a, T, p, old_n, new_n) \
	((T *)cr_reallocate_array((a), (p), (old_n), (new_n), sizeof(T), CR_ALIGNOF(T)))

// Gives [head] a debug name: a copy of [name] made through its allocator,
// freeing the one it had. NULL removes the name. [name] may be the head's
// own current name. CR_RESULT_OUT_OF_MEMORY leaves the old name in place.
static inline CR_RESULT cr_object_name_set(cr_object_head *head, const char *name) {
	char *copy = NULL;
	if (name) {
		copy = cr_string_copy(&head->allocator, name);
		if (!copy)
			return CR_RESULT_OUT_OF_MEMORY;
	}
	cr_string_free(&head->allocator, head->debug_name);
	head->debug_name = copy;
	return CR_SUCCESS;
}

#endif // CRALLOCATOR_H
