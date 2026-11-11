#include "crfiles.h"
#include "files/crfilesshared.h"
#include "crallocator.h"
#include "crstructbase.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- Messages ---- */

void crfiles_report_platform(const crfiles_report *report, CR_RESULT result, CR_PLATFORM_ERROR_SOURCE source, int64_t code, const char *text) {
	cr_message_platform(report->instance->message_callback, report->instance->message_user_data, &report->about, result, source, code, text);
}

/* The report of a call made on the instance; [subject] is the debug name a
 * create's desc gives what it makes. */
static crfiles_report crfiles_instance_report(crfiles_instance instance, const char *call, const char *subject) {
	crfiles_report report;
	report.instance                 = instance;
	report.about.call               = call;
	report.about.object             = instance;
	report.about.object_debug_name  = NULL;
	report.about.subject_debug_name = subject;
	return report;
}

/* The report of a call made on [object], one of [instance]'s. */
static crfiles_report crfiles_object_report(crfiles_instance instance, const char *call, const void *object, const char *name) {
	crfiles_report report;
	report.instance                 = instance;
	report.about.call               = call;
	report.about.object             = object;
	report.about.object_debug_name  = name;
	report.about.subject_debug_name = NULL;
	return report;
}

static crfiles_report crfiles_file_report(crfiles_file file, const char *call) {
	return crfiles_object_report(file->instance, call, file, file->head.debug_name);
}

/* ---- Instance ---- */

CR_RESULT crfiles_instance_create(
    const crfiles_instance_desc *desc,
    crfiles_instance *out_instance) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_FILES_INSTANCE_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_instance)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_instance                      = NULL;
	const CR_RESULT allocator_result  = cr_allocator_check(desc->allocator);
	if (allocator_result != CR_SUCCESS)
		return allocator_result;

	crfiles_instance instance = CR_NEW_ZEROED(desc->allocator, struct crfiles_instance_t);
	if (!instance)
		return CR_RESULT_OUT_OF_MEMORY;
	instance->head.allocator     = *desc->allocator;
	instance->message_callback  = desc->message_callback;
	instance->message_user_data = desc->message_user_data;
	CR_BIRTH(instance);
	CR_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);
	*out_instance       = instance;
	return CR_SUCCESS;
}

void crfiles_instance_destroy(crfiles_instance instance) {
	if (!instance)
		return;
	if (CR_STALE(instance, "crfiles_instance_destroy", "instance"))
		return;
	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	CR_QUARANTINE_RELEASE(&instance->quarantine);
	CR_DELETE(&allocator, struct crfiles_instance_t, instance);
}

/* ---- Opening ---- */

static bool crfiles_path_given(const char *path) { return path && path[0]; }

CR_RESULT crfiles_file_open(
    crfiles_instance instance,
    const crfiles_open_desc *desc,
    crfiles_file *out_file) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_file_open", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_FILES_OPEN_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_file)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_file = NULL;
	if (!crfiles_path_given(desc->path))
		return CR_RESULT_FILES_NULL_PATH;
	if (desc->access != CRFILES_ACCESS_READ && desc->access != CRFILES_ACCESS_WRITE && desc->access != CRFILES_ACCESS_READ_WRITE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->create < CRFILES_CREATE_OPEN_EXISTING || desc->create > CRFILES_CREATE_TRUNCATE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->share & ~(CRFILES_SHARE)(CRFILES_SHARE_READ | CRFILES_SHARE_WRITE | CRFILES_SHARE_DELETE))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->create == CRFILES_CREATE_TRUNCATE && desc->access == CRFILES_ACCESS_READ)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	crfiles_file file = CR_NEW_ZEROED(&instance->head.allocator, struct crfiles_file_t);
	if (!file)
		return CR_RESULT_OUT_OF_MEMORY;
	file->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(file, &instance->quarantine);
	file->instance       = instance;
	file->access         = desc->access;

	const crfiles_report report = crfiles_instance_report(instance, "crfiles_file_open", desc->debug_name);
	const CR_RESULT result      = crfiles_backend_open(&report, desc, &file->native);
	if (result != CR_SUCCESS) {
		const CR_ALLOCATOR allocator = file->head.allocator;
		CR_DELETE(&allocator, struct crfiles_file_t, file);
		return result;
	}
	if (cr_debug_name_set(file, desc->debug_name) != CR_SUCCESS) {
		crfiles_file_close(file);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	*out_file = file;
	return CR_SUCCESS;
}

void crfiles_file_close(crfiles_file file) {
	if (!file)
		return;
	if (CR_STALE(file, "crfiles_file_close", "file"))
		return;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_close");
	// Windows releases a closed handle's locks only when the system gets to
	// it; let go first, so the next locker never waits on a closed file.
	if (file->lock != CRFILES_LOCK_NONE)
		crfiles_backend_unlock(&report, file->native);
	crfiles_backend_close(&report, file->native);
	CR_KILL(file);
	const CR_ALLOCATOR allocator = file->head.allocator;
	cr_string_free(&allocator, file->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(file), &allocator, struct crfiles_file_t, file);
}

/* ---- I/O at an offset ---- */

static bool crfiles_readable(crfiles_file file) { return file->access == CRFILES_ACCESS_READ || file->access == CRFILES_ACCESS_READ_WRITE; }
static bool crfiles_writable(crfiles_file file) { return file->access == CRFILES_ACCESS_WRITE || file->access == CRFILES_ACCESS_READ_WRITE; }

CR_RESULT crfiles_file_read_at(
    crfiles_file file,
    uint64_t offset,
    void *buffer,
    uint64_t size,
    uint64_t *out_read) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_read_at", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (!buffer || !out_read)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_read = 0;
	if (!crfiles_readable(file))
		return CR_RESULT_FILES_NOT_READABLE;
	if (size == 0)
		return CR_SUCCESS;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_read_at");
	return crfiles_backend_read_at(&report, file->native, offset, buffer, size, out_read);
}

CR_RESULT crfiles_file_write_at(
    crfiles_file file,
    uint64_t offset,
    const void *data,
    uint64_t size) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_write_at", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (!data && size != 0)
		return CR_RESULT_NULL_DESC;
	if (!crfiles_writable(file))
		return CR_RESULT_FILES_NOT_WRITABLE;
	if (size == 0)
		return CR_SUCCESS;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_write_at");
	return crfiles_backend_write_at(&report, file->native, offset, data, size);
}

CR_RESULT crfiles_file_get_size(
    crfiles_file file,
    uint64_t *out_size) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_get_size", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (!out_size)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_get_size");
	return crfiles_backend_get_size(&report, file->native, out_size);
}

CR_RESULT crfiles_file_set_size(
    crfiles_file file,
    uint64_t size) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_set_size", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (!crfiles_writable(file))
		return CR_RESULT_FILES_NOT_WRITABLE;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_set_size");
	return crfiles_backend_set_size(&report, file->native, size);
}

CR_RESULT crfiles_file_flush(
    crfiles_file file,
    CRFILES_FLUSH_LEVEL level) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_flush", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (level != CRFILES_FLUSH_LEVEL_DEVICE && level != CRFILES_FLUSH_LEVEL_MEDIA)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!crfiles_writable(file))
		return CR_RESULT_FILES_NOT_WRITABLE;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_flush");
	return crfiles_backend_flush(&report, file->native, level);
}

/* ---- Mapping ---- */

CR_RESULT crfiles_file_map(
    crfiles_file file,
    uint64_t offset,
    uint64_t size,
    crfiles_mapping *out_mapping) {
	if (!out_mapping)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_mapping = NULL;
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_map", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!crfiles_readable(file))
		return CR_RESULT_FILES_NOT_READABLE;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_map");
	uint64_t file_size           = 0;
	const CR_RESULT sized       = crfiles_backend_get_size(&report, file->native, &file_size);
	if (sized != CR_SUCCESS)
		return sized;
	if (offset > file_size || size > file_size - offset)
		return CR_RESULT_INDEX_OUT_OF_RANGE;

	crfiles_mapping mapping = CR_NEW_ZEROED(&file->head.allocator, struct crfiles_mapping_t);
	if (!mapping)
		return CR_RESULT_OUT_OF_MEMORY;
	mapping->head.allocator = file->head.allocator;
	CR_BIRTH_IN(mapping, &file->instance->quarantine);
	mapping->instance       = file->instance;
	uint64_t lead           = 0;
	const CR_RESULT result = crfiles_backend_map(&report, file->native, offset, size, &mapping->base, &mapping->base_size, &lead);
	if (result != CR_SUCCESS) {
		const CR_ALLOCATOR allocator = mapping->head.allocator;
		CR_DELETE(&allocator, struct crfiles_mapping_t, mapping);
		return result;
	}
	mapping->data = (const uint8_t *)mapping->base + lead;
	mapping->size = size;
	*out_mapping  = mapping;
	return CR_SUCCESS;
}

const void *crfiles_mapping_get_data(crfiles_mapping mapping) { return mapping ? mapping->data : NULL; }

uint64_t crfiles_mapping_get_size(crfiles_mapping mapping) {
	if (!mapping)
		return 0;
	if (CR_STALE(mapping, "crfiles_mapping_get_size", "mapping"))
		return 0;
	return mapping->size;
}

void crfiles_mapping_release(crfiles_mapping mapping) {
	if (!mapping)
		return;
	if (CR_STALE(mapping, "crfiles_mapping_release", "mapping"))
		return;
	const crfiles_report report = crfiles_object_report(mapping->instance, "crfiles_mapping_release", mapping, mapping->head.debug_name);
	crfiles_backend_unmap(&report, mapping->base, mapping->base_size);
	CR_KILL(mapping);
	const CR_ALLOCATOR allocator = mapping->head.allocator;
	cr_string_free(&allocator, mapping->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(mapping), &allocator, struct crfiles_mapping_t, mapping);
}

/* ---- Paths ---- */

CR_RESULT crfiles_path_get_info(
    crfiles_instance instance,
    const char *path,
    CRFILES_PATH_INFO *out_info) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_path_get_info", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(path))
		return CR_RESULT_FILES_NULL_PATH;
	if (!out_info)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_result = cr_struct_out_check(&out_info->base, CR_STRUCT_TYPE_FILES_PATH_INFO);
	if (base_result != CR_SUCCESS)
		return base_result;
	out_info->kind               = (CRFILES_PATH_KIND)0;
	out_info->size               = 0;
	out_info->modified_ns        = 0;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_path_get_info", NULL);
	return crfiles_backend_path_get_info(&report, path, out_info);
}

CR_RESULT crfiles_path_replace(
    crfiles_instance instance,
    const char *source,
    const char *target,
    CRFILES_FLUSH_LEVEL level) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_path_replace", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(source))
		return CR_RESULT_FILES_NULL_PATH;
	if (!crfiles_path_given(target))
		return CR_RESULT_FILES_NULL_PATH;
	if (level != CRFILES_FLUSH_LEVEL_NONE && level != CRFILES_FLUSH_LEVEL_DEVICE && level != CRFILES_FLUSH_LEVEL_MEDIA)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_path_replace", NULL);
	return crfiles_backend_path_replace(&report, source, target, level);
}

CR_RESULT crfiles_path_remove(
    crfiles_instance instance,
    const char *path) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_path_remove", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(path))
		return CR_RESULT_FILES_NULL_PATH;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_path_remove", NULL);
	return crfiles_backend_path_remove(&report, path);
}

CR_RESULT crfiles_path_copy_mode(
    crfiles_instance instance,
    const char *source,
    const char *target) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_path_copy_mode", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(source))
		return CR_RESULT_FILES_NULL_PATH;
	if (!crfiles_path_given(target))
		return CR_RESULT_FILES_NULL_PATH;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_path_copy_mode", NULL);
	return crfiles_backend_path_copy_mode(&report, source, target);
}

CR_RESULT crfiles_directory_create(
    crfiles_instance instance,
    const char *path) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_directory_create", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(path))
		return CR_RESULT_FILES_NULL_PATH;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_directory_create", NULL);
	return crfiles_backend_directory_create(&report, path);
}

CR_RESULT crfiles_directory_remove(
    crfiles_instance instance,
    const char *path) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_directory_remove", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(path))
		return CR_RESULT_FILES_NULL_PATH;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_directory_remove", NULL);
	return crfiles_backend_directory_remove(&report, path);
}

CR_RESULT crfiles_directory_rename(
    crfiles_instance instance,
    const char *source,
    const char *target,
    CRFILES_FLUSH_LEVEL level) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_directory_rename", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!crfiles_path_given(source))
		return CR_RESULT_FILES_NULL_PATH;
	if (!crfiles_path_given(target))
		return CR_RESULT_FILES_NULL_PATH;
	if (level != CRFILES_FLUSH_LEVEL_NONE && level != CRFILES_FLUSH_LEVEL_DEVICE && level != CRFILES_FLUSH_LEVEL_MEDIA)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	const crfiles_report report = crfiles_instance_report(instance, "crfiles_directory_rename", NULL);
	return crfiles_backend_directory_rename(&report, source, target, level);
}

/* ---- Listing a directory ---- */

CR_RESULT crfiles_listing_open(
    crfiles_instance instance,
    const crfiles_listing_desc *desc,
    crfiles_listing *out_listing) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_listing_open", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_FILES_LISTING_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_listing)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_listing = NULL;
	if (!crfiles_path_given(desc->path))
		return CR_RESULT_FILES_NULL_PATH;

	crfiles_listing listing = CR_NEW_ZEROED(&instance->head.allocator, struct crfiles_listing_t);
	if (!listing)
		return CR_RESULT_OUT_OF_MEMORY;
	listing->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(listing, &instance->quarantine);
	listing->instance       = instance;

	const crfiles_report report = crfiles_instance_report(instance, "crfiles_listing_open", desc->debug_name);
	const CR_RESULT result      = crfiles_backend_listing_open(&report, desc->path, &listing->native);
	if (result != CR_SUCCESS) {
		const CR_ALLOCATOR allocator = listing->head.allocator;
		CR_DELETE(&allocator, struct crfiles_listing_t, listing);
		return result;
	}
	if (cr_debug_name_set(listing, desc->debug_name) != CR_SUCCESS) {
		crfiles_listing_close(listing);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	*out_listing = listing;
	return CR_SUCCESS;
}

CR_RESULT crfiles_listing_next(
    crfiles_listing listing,
    CRFILES_LISTING_ENTRY *out_entry) {
	if (!listing)
		return CR_RESULT_FILES_INVALID_LISTING;
	if (CR_STALE(listing, "crfiles_listing_next", "listing"))
		return CR_RESULT_FILES_INVALID_LISTING;
	if (!out_entry)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_result = cr_struct_out_check(&out_entry->base, CR_STRUCT_TYPE_FILES_LISTING_ENTRY);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (listing->ended)
		return CR_RESULT_FILES_END_OF_LISTING;
	const crfiles_report report = crfiles_object_report(listing->instance, "crfiles_listing_next", listing, listing->head.debug_name);
	const CR_RESULT result      = crfiles_backend_listing_next(&report, listing->native, out_entry);
	if (result == CR_RESULT_FILES_END_OF_LISTING)
		listing->ended = true;
	return result;
}

void crfiles_listing_close(crfiles_listing listing) {
	if (!listing)
		return;
	if (CR_STALE(listing, "crfiles_listing_close", "listing"))
		return;
	const crfiles_report report = crfiles_object_report(listing->instance, "crfiles_listing_close", listing, listing->head.debug_name);
	crfiles_backend_listing_close(&report, listing->native);
	CR_KILL(listing);
	const CR_ALLOCATOR allocator = listing->head.allocator;
	cr_string_free(&allocator, listing->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(listing), &allocator, struct crfiles_listing_t, listing);
}

/* RFC 3629's UTF-8: no surrogate, no overlong form, nothing past U+10FFFF. */
bool crfiles_utf8_valid(const char *bytes, uint32_t size) {
	const uint8_t *at  = (const uint8_t *)bytes;
	const uint8_t *end = at + size;
	while (at < end) {
		const uint8_t lead = *at;
		if (lead < 0x80) {
			++at;
			continue;
		}
		uint32_t count = 0;
		uint32_t code  = 0;
		uint32_t least = 0;
		if (lead >= 0xC2 && lead <= 0xDF) {
			count = 1;
			code  = lead & 0x1Fu;
			least = 0x80;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			count = 2;
			code  = lead & 0x0Fu;
			least = 0x800;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			count = 3;
			code  = lead & 0x07u;
			least = 0x10000;
		} else {
			return false;
		}
		if ((uint64_t)(end - at) <= count)
			return false;
		for (uint32_t i = 1; i <= count; ++i) {
			if ((at[i] & 0xC0u) != 0x80u)
				return false;
			code = (code << 6) | (at[i] & 0x3Fu);
		}
		if (code < least || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu))
			return false;
		at += count + 1;
	}
	return true;
}

/* ---- Locking a file ---- */

CR_RESULT crfiles_file_lock(
    crfiles_file file,
    CRFILES_LOCK lock,
    bool wait) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_lock", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (lock != CRFILES_LOCK_SHARED && lock != CRFILES_LOCK_EXCLUSIVE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (file->lock != CRFILES_LOCK_NONE)
		return CR_RESULT_FILES_ALREADY_LOCKED;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_lock");
	const CR_RESULT result      = crfiles_backend_lock(&report, file->native, lock, wait);
	if (result == CR_SUCCESS)
		file->lock = (uint8_t)lock;
	return result;
}

CR_RESULT crfiles_file_unlock(crfiles_file file) {
	if (!file)
		return CR_RESULT_FILES_INVALID_FILE;
	if (CR_STALE(file, "crfiles_file_unlock", "file"))
		return CR_RESULT_FILES_INVALID_FILE;
	if (file->lock == CRFILES_LOCK_NONE)
		return CR_RESULT_FILES_NOT_LOCKED;
	const crfiles_report report = crfiles_file_report(file, "crfiles_file_unlock");
	const CR_RESULT result      = crfiles_backend_unlock(&report, file->native);
	if (result == CR_SUCCESS)
		file->lock = CRFILES_LOCK_NONE;
	return result;
}

/* ---- Dynamic libraries ---- */

CR_RESULT crfiles_library_open(
    crfiles_instance instance,
    const crfiles_library_desc *desc,
    crfiles_library *out_library) {
	if (!instance)
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (CR_STALE(instance, "crfiles_library_open", "instance"))
		return CR_RESULT_FILES_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_FILES_LIBRARY_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_library)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_library = NULL;
	if (!crfiles_path_given(desc->path))
		return CR_RESULT_FILES_NULL_PATH;
	if (desc->binding != CRFILES_LIBRARY_BINDING_NOW && desc->binding != CRFILES_LIBRARY_BINDING_LAZY)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->scope != CRFILES_LIBRARY_SCOPE_LOCAL && desc->scope != CRFILES_LIBRARY_SCOPE_GLOBAL)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->dependencies != CRFILES_LIBRARY_DEPENDENCIES_PLATFORM_ORDER && desc->dependencies != CRFILES_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!crfiles_backend_library_binding_supported(desc->binding))
		return CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED;
	if (!crfiles_backend_library_scope_supported(desc->scope))
		return CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED;
	if (!crfiles_backend_library_dependencies_supported(desc->dependencies))
		return CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED;
	if (desc->dependencies == CRFILES_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST && !crfiles_backend_path_fully_qualified(desc->path))
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	crfiles_library library = CR_NEW_ZEROED(&instance->head.allocator, struct crfiles_library_t);
	if (!library)
		return CR_RESULT_OUT_OF_MEMORY;
	library->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(library, &instance->quarantine);
	library->instance       = instance;

	const crfiles_report report = crfiles_instance_report(instance, "crfiles_library_open", desc->debug_name);
	const CR_RESULT result      = crfiles_backend_library_open(&report, desc, &library->native);
	if (result != CR_SUCCESS) {
		const CR_ALLOCATOR allocator = library->head.allocator;
		CR_DELETE(&allocator, struct crfiles_library_t, library);
		return result;
	}
	// The last step: a failed copy closes the library, leaving it unloaded.
	if (cr_debug_name_set(library, desc->debug_name) != CR_SUCCESS) {
		crfiles_library_close(library);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	*out_library = library;
	return CR_SUCCESS;
}

CR_RESULT crfiles_library_find(
    crfiles_library library,
    const char *name,
    crfiles_library_function *out_function) {
	if (!library)
		return CR_RESULT_FILES_INVALID_LIBRARY;
	if (CR_STALE(library, "crfiles_library_find", "library"))
		return CR_RESULT_FILES_INVALID_LIBRARY;
	// name null: stops at !name, name[0] is never read.
	if (!name || !name[0])
		return CR_RESULT_NULL_DESC;
	if (!out_function)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_function                = NULL;
	const crfiles_report report = crfiles_object_report(library->instance, "crfiles_library_find", library, library->head.debug_name);
	return crfiles_backend_library_find(&report, library->native, name, out_function);
}

void crfiles_library_close(crfiles_library library) {
	if (!library)
		return;
	if (CR_STALE(library, "crfiles_library_close", "library"))
		return;
	const crfiles_report report = crfiles_object_report(library->instance, "crfiles_library_close", library, library->head.debug_name);
	crfiles_backend_library_close(&report, library->native);
	CR_KILL(library);
	const CR_ALLOCATOR allocator = library->head.allocator;
	cr_string_free(&allocator, library->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(library), &allocator, struct crfiles_library_t, library);
}
