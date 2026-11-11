/*
 * crtest_allocator.h — a CR_ALLOCATOR for tests that checks how Carrottus
 * uses it.
 *
 * Each block carries a header in front of it holding the size and alignment
 * it was asked for, so that a free with another size or alignment, a free of
 * a block it never gave, and one freed twice are each counted. At the end a
 * test checks that nothing is left and nothing went wrong:
 *
 *     crtest_allocator tracker;
 *     CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
 *     ... make and destroy things with &allocator ...
 *     CHECK(crtest_allocator_clean(&tracker));
 *
 * Its blocks come from cr_allocator_system(). It can be told to refuse
 * every allocation after a number of them, to test a call that runs out of
 * memory. Not safe from two threads at once: a test calls one instance from
 * one thread.
 */

#ifndef CRTEST_ALLOCATOR_H
#define CRTEST_ALLOCATOR_H

#include <crcore.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CRTEST_ALLOCATOR_MAGIC 0x5350554Du /* "SPUM" */
#define CRTEST_ALLOCATOR_FREED 0x46524545u /* "FREE" */

typedef struct crtest_allocator {
	CR_ALLOCATOR system;
	/* Blocks and bytes given and not yet freed. */
	size_t live_blocks;
	size_t live_bytes;
	/* Every allocation asked for, and every one given. */
	size_t asked;
	size_t given;
	/* A free whose size or alignment isn't the block's, or of a block this
	 * allocator didn't give or had freed; an allocation of 0 bytes, or of an
	 * alignment that isn't a power of two; a block not aligned as asked. */
	size_t wrong_frees;
	size_t wrong_allocations;
	/* Refuse every allocation once `given` reaches it. SIZE_MAX refuses
	 * none. */
	size_t refuse_after;
} crtest_allocator;

typedef struct crtest_allocator_header {
	uint32_t magic;
	size_t size;
	size_t alignment;
	size_t offset;
} crtest_allocator_header;

static size_t crtest_allocator_offset(size_t alignment) {
	size_t offset = alignment;
	while (offset < sizeof(crtest_allocator_header))
		offset += alignment;
	return offset;
}

static void *crtest_allocator_allocate(void *user_data, size_t size, size_t alignment) {
	crtest_allocator *tracker = (crtest_allocator *)user_data;
	++tracker->asked;
	if (size == 0 || alignment == 0 || (alignment & (alignment - 1))) {
		++tracker->wrong_allocations;
		return NULL;
	}
	if (tracker->given >= tracker->refuse_after)
		return NULL;

	const size_t block_alignment = alignment < _Alignof(crtest_allocator_header) ? _Alignof(crtest_allocator_header) : alignment;
	const size_t offset          = crtest_allocator_offset(block_alignment);
	if (size > SIZE_MAX - offset)
		return NULL;
	uint8_t *block = (uint8_t *)tracker->system.allocate(tracker->system.user_data, offset + size, block_alignment);
	if (!block)
		return NULL;

	uint8_t *memory = block + offset;
	crtest_allocator_header header;
	header.magic     = CRTEST_ALLOCATOR_MAGIC;
	header.size      = size;
	header.alignment = alignment;
	header.offset    = offset;
	memcpy(memory - sizeof(header), &header, sizeof(header));
	/* What a caller reads before writing is undefined: make it visible. */
	memset(memory, 0xCD, size);

	if ((uintptr_t)memory & (alignment - 1))
		++tracker->wrong_allocations;
	++tracker->given;
	++tracker->live_blocks;
	tracker->live_bytes += size;
	return memory;
}

static void crtest_allocator_free(void *user_data, void *memory, size_t size, size_t alignment) {
	crtest_allocator *tracker = (crtest_allocator *)user_data;
	if (!memory) {
		++tracker->wrong_frees;
		return;
	}

	crtest_allocator_header header;
	memcpy(&header, (uint8_t *)memory - sizeof(header), sizeof(header));
	if (header.magic != CRTEST_ALLOCATOR_MAGIC) {
		/* Never given, or freed already: left alone. */
		++tracker->wrong_frees;
		return;
	}
	if (header.size != size || header.alignment != alignment) {
		++tracker->wrong_frees;
		fprintf(stderr, "crtest_allocator: block of %zu bytes at %zu freed as %zu at %zu\n", header.size, header.alignment, size,
		        alignment);
	}

	header.magic = CRTEST_ALLOCATOR_FREED;
	memcpy((uint8_t *)memory - sizeof(header), &header, sizeof(header));
	--tracker->live_blocks;
	tracker->live_bytes -= header.size;
	const size_t block_alignment = alignment < _Alignof(crtest_allocator_header) ? _Alignof(crtest_allocator_header) : alignment;
	tracker->system.free(tracker->system.user_data, (uint8_t *)memory - header.offset, header.offset + header.size, block_alignment);
}

static CR_ALLOCATOR crtest_allocator_make(crtest_allocator *tracker) {
	memset(tracker, 0, sizeof(*tracker));
	tracker->system       = cr_allocator_system();
	tracker->refuse_after = SIZE_MAX;

	CR_ALLOCATOR allocator;
	allocator.user_data = tracker;
	allocator.allocate  = crtest_allocator_allocate;
	allocator.free      = crtest_allocator_free;
	return allocator;
}

/* Whether every block was freed, each with its own size and alignment, and
 * nothing was asked for wrongly. Prints what isn't so. */
static bool crtest_allocator_clean(const crtest_allocator *tracker) {
	const bool clean = tracker->live_blocks == 0 && tracker->live_bytes == 0 && tracker->wrong_frees == 0 && tracker->wrong_allocations == 0;
	if (!clean)
		fprintf(stderr, "crtest_allocator: %zu blocks (%zu bytes) left, %zu wrong frees, %zu wrong allocations\n", tracker->live_blocks,
		        tracker->live_bytes, tracker->wrong_frees, tracker->wrong_allocations);
	return clean;
}

#endif // CRTEST_ALLOCATOR_H
