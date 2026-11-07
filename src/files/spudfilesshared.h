#ifndef SPUDFILESSHARED_H
#define SPUDFILESSHARED_H

/*
 * What a SpudFiles backend provides. src/spudfiles.c is the front end: the
 * instance, every argument check, and every object's memory are there,
 * once. A backend is called only with arguments that passed the checks, and
 * only translates: a file is its native handle (a POSIX descriptor or a
 * Windows HANDLE, as an intptr_t), a mapping its base and size.
 */

#include "spudfiles.h"
#include "../spudallocator.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spudfile_instance_t {
	/* No debug name: the instance desc has none. */
	spud_object_head head;
};

struct spudfile_file_t {
	spud_object_head head;
	spudfile_instance instance;
	SPUDFILE_ACCESS access;
	/* The lock taken through this file (SPUDFILE_LOCK), or NONE: touched
	 * only by lock, unlock and close, which the caller doesn't overlap. */
	uint8_t lock;
	intptr_t native;
};

struct spudfile_listing_t {
	spud_object_head head;
	spudfile_instance instance;
	/* The backend's own state, made by spudfile_backend_listing_open
	 * through the instance's allocator and freed by _close. */
	void *native;
	/* Whether the end was reached: every later next says so, without
	 * asking the platform again. */
	bool ended;
};

struct spudfile_mapping_t {
	spud_object_head head;
	void *base;         /* what the platform mapped: aligned, before data */
	uint64_t base_size; /* what was mapped from base */
	const uint8_t *data;
	uint64_t size;
};

/* Opens [desc->path]; the checks have passed. [instance] is for memory the
 * translation needs (Windows' UTF-16 paths). */
SPUDRESULT spudfile_backend_open(spudfile_instance instance, const spudfile_open_desc *desc, intptr_t *out_native);
void spudfile_backend_close(intptr_t native);
SPUDRESULT spudfile_backend_read_at(intptr_t native, uint64_t offset, void *buffer, uint64_t size, uint64_t *out_read);
SPUDRESULT spudfile_backend_write_at(intptr_t native, uint64_t offset, const void *data, uint64_t size);
SPUDRESULT spudfile_backend_get_size(intptr_t native, uint64_t *out_size);
SPUDRESULT spudfile_backend_set_size(intptr_t native, uint64_t size);
SPUDRESULT spudfile_backend_flush(intptr_t native, SPUDFILE_FLUSH_LEVEL level);
/* Maps [size] bytes from [offset], which lie inside the file: from the
 * aligned position at or before [offset], [*out_lead] bytes before it. */
SPUDRESULT spudfile_backend_map(intptr_t native, uint64_t offset, uint64_t size, void **out_base, uint64_t *out_base_size, uint64_t *out_lead);
void spudfile_backend_unmap(void *base, uint64_t base_size);
SPUDRESULT spudfile_backend_path_get_info(spudfile_instance instance, const char *path, SPUDFILE_PATH_INFO *out_info);
SPUDRESULT spudfile_backend_path_replace(spudfile_instance instance, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level);
SPUDRESULT spudfile_backend_path_remove(spudfile_instance instance, const char *path);
SPUDRESULT spudfile_backend_directory_create(spudfile_instance instance, const char *path);
/* Opens a listing of [path]; its state through [instance]'s allocator. */
SPUDRESULT spudfile_backend_listing_open(spudfile_instance instance, const char *path, void **out_native);
/* The next entry, "." and ".." skipped; SPUDRESULT_SPUDFILE_END_OF_LISTING
 * at the end. [out_entry]'s base is checked. */
SPUDRESULT spudfile_backend_listing_next(spudfile_instance instance, void *native, SPUDFILE_LISTING_ENTRY *out_entry);
void spudfile_backend_listing_close(spudfile_instance instance, void *native);
/* [lock] is SHARED or EXCLUSIVE; the file holds none taken here. */
SPUDRESULT spudfile_backend_lock(intptr_t native, SPUDFILE_LOCK lock, bool wait);
SPUDRESULT spudfile_backend_unlock(intptr_t native);
/* Whether [bytes] are valid UTF-8 (no surrogate, no overlong form, nothing
 * past U+10FFFF): what a listing entry's name_is_utf8 says. */
bool spudfile_utf8_valid(const char *bytes, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif // SPUDFILESSHARED_H
