#ifndef CRFILESSHARED_H
#define CRFILESSHARED_H

/*
 * What a Carrottus Files backend provides. src/crfiles.c is the front end: the
 * instance, every argument check, and every object's memory are there,
 * once. A backend is called only with arguments that passed the checks, and
 * only translates: a file is its native handle (a POSIX descriptor or a
 * Windows HANDLE, as an intptr_t), a mapping its base and size, a library
 * the loader's handle.
 *
 * Every backend call takes the report of the public call it serves: where
 * a platform call fails, the backend sends what the platform said through
 * crfiles_report_platform, inside that call (crcore.h, "Messages", M2),
 * whatever result it returns for it. A check of Carrottus Files' own, or an
 * answer the platform gives as an outcome (the end of a file or a listing,
 * a lock held elsewhere when not waiting), sends nothing.
 */

#include "crfiles.h"
#include "../crallocator.h"
#include "../crmessage.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct crfiles_instance_t {
	/* No debug name: the instance desc has none. */
	cr_object_head head;
	/* Where its and its objects' messages go (crcore.h, "Messages"). */
	CR_MESSAGE_CALLBACK message_callback;
	void *message_user_data;
#if _DEBUG
	/* Its closed files, listings and libraries and released mappings,
	 * freed with it (crstale.h, card X27). */
	cr_quarantine quarantine;
#endif
};

struct crfiles_file_t {
	cr_object_head head;
	crfiles_instance instance;
	CRFILES_ACCESS access;
	/* The lock taken through this file (CRFILES_LOCK), or NONE: touched
	 * only by lock, unlock and close, which the caller doesn't overlap. */
	uint8_t lock;
	intptr_t native;
};

struct crfiles_listing_t {
	cr_object_head head;
	crfiles_instance instance;
	/* The backend's own state, made by crfiles_backend_listing_open
	 * through the instance's allocator and freed by _close. */
	void *native;
	/* Whether the end was reached: every later next says so, without
	 * asking the platform again. */
	bool ended;
};

struct crfiles_mapping_t {
	cr_object_head head;
	crfiles_instance instance; /* where the release's message goes */
	void *base;                 /* what the platform mapped: aligned, before data */
	uint64_t base_size;         /* what was mapped from base */
	const uint8_t *data;
	uint64_t size;
};

struct crfiles_library_t {
	cr_object_head head;
	crfiles_instance instance;
	/* The loader's handle: an HMODULE, or what dlopen gave. */
	void *native;
};

/* The public call a backend call serves: the instance (its allocator and
 * its callback) and who a message is about. Made by the front end on the
 * stack, per call. */
typedef struct crfiles_report {
	crfiles_instance instance;
	cr_message_about about;
} crfiles_report;

/* Sends a platform failure for [report]'s call (M2). [text] is the
 * platform's own wording, or NULL. */
void crfiles_report_platform(const crfiles_report *report, CR_RESULT result, CR_PLATFORM_ERROR_SOURCE source, int64_t code, const char *text);

/* Opens [desc->path]; the checks have passed. [report]'s instance is for
 * memory the translation needs (Windows' UTF-16 paths). */
CR_RESULT crfiles_backend_open(const crfiles_report *report, const crfiles_open_desc *desc, intptr_t *out_native);
void crfiles_backend_close(const crfiles_report *report, intptr_t native);
CR_RESULT crfiles_backend_read_at(const crfiles_report *report, intptr_t native, uint64_t offset, void *buffer, uint64_t size, uint64_t *out_read);
CR_RESULT crfiles_backend_write_at(const crfiles_report *report, intptr_t native, uint64_t offset, const void *data, uint64_t size);
CR_RESULT crfiles_backend_get_size(const crfiles_report *report, intptr_t native, uint64_t *out_size);
CR_RESULT crfiles_backend_set_size(const crfiles_report *report, intptr_t native, uint64_t size);
CR_RESULT crfiles_backend_flush(const crfiles_report *report, intptr_t native, CRFILES_FLUSH_LEVEL level);
/* Maps [size] bytes from [offset], which lie inside the file: from the
 * aligned position at or before [offset], [*out_lead] bytes before it. */
CR_RESULT crfiles_backend_map(const crfiles_report *report, intptr_t native, uint64_t offset, uint64_t size, void **out_base, uint64_t *out_base_size,
                                uint64_t *out_lead);
void crfiles_backend_unmap(const crfiles_report *report, void *base, uint64_t base_size);
CR_RESULT crfiles_backend_path_get_info(const crfiles_report *report, const char *path, CRFILES_PATH_INFO *out_info);
CR_RESULT crfiles_backend_path_replace(const crfiles_report *report, const char *source, const char *target, CRFILES_FLUSH_LEVEL level);
CR_RESULT crfiles_backend_path_remove(const crfiles_report *report, const char *path);
/* Gives [target] [source]'s permissions: POSIX's nine bits, Windows'
 * read-only attribute; links followed. */
CR_RESULT crfiles_backend_path_copy_mode(const crfiles_report *report, const char *source, const char *target);
CR_RESULT crfiles_backend_directory_create(const crfiles_report *report, const char *path);
/* Removes the empty directory, or the link to one, at [path]. */
CR_RESULT crfiles_backend_directory_remove(const crfiles_report *report, const char *path);
/* Renames the directory [source] to [target], where nothing is; [level] is
 * checked. */
CR_RESULT crfiles_backend_directory_rename(const crfiles_report *report, const char *source, const char *target, CRFILES_FLUSH_LEVEL level);
/* Opens a listing of [path]; its state through [report]'s instance's
 * allocator. */
CR_RESULT crfiles_backend_listing_open(const crfiles_report *report, const char *path, void **out_native);
/* The next entry, "." and ".." skipped; CR_RESULT_FILES_END_OF_LISTING
 * at the end. [out_entry]'s base is checked. */
CR_RESULT crfiles_backend_listing_next(const crfiles_report *report, void *native, CRFILES_LISTING_ENTRY *out_entry);
void crfiles_backend_listing_close(const crfiles_report *report, void *native);
/* [lock] is SHARED or EXCLUSIVE; the file holds none taken here. */
CR_RESULT crfiles_backend_lock(const crfiles_report *report, intptr_t native, CRFILES_LOCK lock, bool wait);
CR_RESULT crfiles_backend_unlock(const crfiles_report *report, intptr_t native);
/* Whether [bytes] are valid UTF-8 (no surrogate, no overlong form, nothing
 * past U+10FFFF): what a listing entry's name_is_utf8 says. */
bool crfiles_utf8_valid(const char *bytes, uint32_t size);

/* Whether the platform's loader has this choice; each value is one of its
 * enum's, NONE excluded. */
bool crfiles_backend_library_binding_supported(CRFILES_LIBRARY_BINDING binding);
bool crfiles_backend_library_scope_supported(CRFILES_LIBRARY_SCOPE scope);
bool crfiles_backend_library_dependencies_supported(CRFILES_LIBRARY_DEPENDENCIES dependencies);
/* Whether [path] is fully qualified, as the loader needs it for
 * CRFILES_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST; asked only where
 * that choice is supported. */
bool crfiles_backend_path_fully_qualified(const char *path);
/* Opens the library at [desc->path]; every check has passed. */
CR_RESULT crfiles_backend_library_open(const crfiles_report *report, const crfiles_library_desc *desc, void **out_native);
/* Finds [name] among the library's own exports. */
CR_RESULT crfiles_backend_library_find(const crfiles_report *report, void *native, const char *name, crfiles_library_function *out_function);
void crfiles_backend_library_close(const crfiles_report *report, void *native);

#ifdef __cplusplus
}
#endif

#endif // CRFILESSHARED_H
