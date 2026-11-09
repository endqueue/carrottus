#ifndef SPUDFILESSHARED_H
#define SPUDFILESSHARED_H

/*
 * What a SpudFiles backend provides. src/spudfiles.c is the front end: the
 * instance, every argument check, and every object's memory are there,
 * once. A backend is called only with arguments that passed the checks, and
 * only translates: a file is its native handle (a POSIX descriptor or a
 * Windows HANDLE, as an intptr_t), a mapping its base and size, a library
 * the loader's handle.
 *
 * Every backend call takes the report of the public call it serves: where
 * a platform call fails, the backend sends what the platform said through
 * spudfile_report_platform, inside that call (spudcore.h, "Messages", M2),
 * whatever result it returns for it. A check of SpudFiles' own, or an
 * answer the platform gives as an outcome (the end of a file or a listing,
 * a lock held elsewhere when not waiting), sends nothing.
 */

#include "spudfiles.h"
#include "../spudallocator.h"
#include "../spudmessage.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spudfile_instance_t {
	/* No debug name: the instance desc has none. */
	spud_object_head head;
	/* Where its and its objects' messages go (spudcore.h, "Messages"). */
	SPUD_MESSAGE_CALLBACK message_callback;
	void *message_user_data;
#if _DEBUG
	/* Its closed files, listings and libraries and released mappings,
	 * freed with it (spudstale.h, card X27). */
	spud_quarantine quarantine;
#endif
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
	spudfile_instance instance; /* where the release's message goes */
	void *base;                 /* what the platform mapped: aligned, before data */
	uint64_t base_size;         /* what was mapped from base */
	const uint8_t *data;
	uint64_t size;
};

struct spudfile_library_t {
	spud_object_head head;
	spudfile_instance instance;
	/* The loader's handle: an HMODULE, or what dlopen gave. */
	void *native;
};

/* The public call a backend call serves: the instance (its allocator and
 * its callback) and who a message is about. Made by the front end on the
 * stack, per call. */
typedef struct spudfile_report {
	spudfile_instance instance;
	spud_message_about about;
} spudfile_report;

/* Sends a platform failure for [report]'s call (M2). [text] is the
 * platform's own wording, or NULL. */
void spudfile_report_platform(const spudfile_report *report, SPUDRESULT result, SPUD_PLATFORM_ERROR_SOURCE source, int64_t code, const char *text);

/* Opens [desc->path]; the checks have passed. [report]'s instance is for
 * memory the translation needs (Windows' UTF-16 paths). */
SPUDRESULT spudfile_backend_open(const spudfile_report *report, const spudfile_open_desc *desc, intptr_t *out_native);
void spudfile_backend_close(const spudfile_report *report, intptr_t native);
SPUDRESULT spudfile_backend_read_at(const spudfile_report *report, intptr_t native, uint64_t offset, void *buffer, uint64_t size, uint64_t *out_read);
SPUDRESULT spudfile_backend_write_at(const spudfile_report *report, intptr_t native, uint64_t offset, const void *data, uint64_t size);
SPUDRESULT spudfile_backend_get_size(const spudfile_report *report, intptr_t native, uint64_t *out_size);
SPUDRESULT spudfile_backend_set_size(const spudfile_report *report, intptr_t native, uint64_t size);
SPUDRESULT spudfile_backend_flush(const spudfile_report *report, intptr_t native, SPUDFILE_FLUSH_LEVEL level);
/* Maps [size] bytes from [offset], which lie inside the file: from the
 * aligned position at or before [offset], [*out_lead] bytes before it. */
SPUDRESULT spudfile_backend_map(const spudfile_report *report, intptr_t native, uint64_t offset, uint64_t size, void **out_base, uint64_t *out_base_size,
                                uint64_t *out_lead);
void spudfile_backend_unmap(const spudfile_report *report, void *base, uint64_t base_size);
SPUDRESULT spudfile_backend_path_get_info(const spudfile_report *report, const char *path, SPUDFILE_PATH_INFO *out_info);
SPUDRESULT spudfile_backend_path_replace(const spudfile_report *report, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level);
SPUDRESULT spudfile_backend_path_remove(const spudfile_report *report, const char *path);
/* Gives [target] [source]'s permissions: POSIX's nine bits, Windows'
 * read-only attribute; links followed. */
SPUDRESULT spudfile_backend_path_copy_mode(const spudfile_report *report, const char *source, const char *target);
SPUDRESULT spudfile_backend_directory_create(const spudfile_report *report, const char *path);
/* Removes the empty directory, or the link to one, at [path]. */
SPUDRESULT spudfile_backend_directory_remove(const spudfile_report *report, const char *path);
/* Renames the directory [source] to [target], where nothing is; [level] is
 * checked. */
SPUDRESULT spudfile_backend_directory_rename(const spudfile_report *report, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level);
/* Opens a listing of [path]; its state through [report]'s instance's
 * allocator. */
SPUDRESULT spudfile_backend_listing_open(const spudfile_report *report, const char *path, void **out_native);
/* The next entry, "." and ".." skipped; SPUDRESULT_SPUDFILE_END_OF_LISTING
 * at the end. [out_entry]'s base is checked. */
SPUDRESULT spudfile_backend_listing_next(const spudfile_report *report, void *native, SPUDFILE_LISTING_ENTRY *out_entry);
void spudfile_backend_listing_close(const spudfile_report *report, void *native);
/* [lock] is SHARED or EXCLUSIVE; the file holds none taken here. */
SPUDRESULT spudfile_backend_lock(const spudfile_report *report, intptr_t native, SPUDFILE_LOCK lock, bool wait);
SPUDRESULT spudfile_backend_unlock(const spudfile_report *report, intptr_t native);
/* Whether [bytes] are valid UTF-8 (no surrogate, no overlong form, nothing
 * past U+10FFFF): what a listing entry's name_is_utf8 says. */
bool spudfile_utf8_valid(const char *bytes, uint32_t size);

/* Whether the platform's loader has this choice; each value is one of its
 * enum's, NONE excluded. */
bool spudfile_backend_library_binding_supported(SPUDFILE_LIBRARY_BINDING binding);
bool spudfile_backend_library_scope_supported(SPUDFILE_LIBRARY_SCOPE scope);
bool spudfile_backend_library_dependencies_supported(SPUDFILE_LIBRARY_DEPENDENCIES dependencies);
/* Whether [path] is fully qualified, as the loader needs it for
 * SPUDFILE_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST; asked only where
 * that choice is supported. */
bool spudfile_backend_path_fully_qualified(const char *path);
/* Opens the library at [desc->path]; every check has passed. */
SPUDRESULT spudfile_backend_library_open(const spudfile_report *report, const spudfile_library_desc *desc, void **out_native);
/* Finds [name] among the library's own exports. */
SPUDRESULT spudfile_backend_library_find(const spudfile_report *report, void *native, const char *name, spudfile_library_function *out_function);
void spudfile_backend_library_close(const spudfile_report *report, void *native);

#ifdef __cplusplus
}
#endif

#endif // SPUDFILESSHARED_H
