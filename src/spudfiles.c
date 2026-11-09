#include "spudfiles.h"
#include "files/spudfilesshared.h"
#include "spudallocator.h"
#include "spudstructbase.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- Messages ---- */

void spudfile_report_platform(const spudfile_report *report, SPUDRESULT result, SPUD_PLATFORM_ERROR_SOURCE source, int64_t code, const char *text) {
	spud_message_platform(report->instance->message_callback, report->instance->message_user_data, &report->about, result, source, code, text);
}

/* The report of a call made on the instance; [subject] is the debug name a
 * create's desc gives what it makes. */
static spudfile_report spudfile_instance_report(spudfile_instance instance, const char *call, const char *subject) {
	spudfile_report report;
	report.instance                 = instance;
	report.about.call               = call;
	report.about.object             = instance;
	report.about.object_debug_name  = NULL;
	report.about.subject_debug_name = subject;
	return report;
}

/* The report of a call made on [object], one of [instance]'s. */
static spudfile_report spudfile_object_report(spudfile_instance instance, const char *call, const void *object, const char *name) {
	spudfile_report report;
	report.instance                 = instance;
	report.about.call               = call;
	report.about.object             = object;
	report.about.object_debug_name  = name;
	report.about.subject_debug_name = NULL;
	return report;
}

static spudfile_report spudfile_file_report(spudfile_file file, const char *call) {
	return spudfile_object_report(file->instance, call, file, file->head.debug_name);
}

/* ---- Instance ---- */

SPUDRESULT spudfile_instance_create(
    const spudfile_instance_desc *desc,
    spudfile_instance *out_instance) {
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_result = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (!out_instance)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_instance                      = NULL;
	const SPUDRESULT allocator_result  = spud_allocator_check(desc->allocator);
	if (allocator_result != SPUD_SUCCESS)
		return allocator_result;

	spudfile_instance instance = SPUD_NEW_ZEROED(desc->allocator, struct spudfile_instance_t);
	if (!instance)
		return SPUDRESULT_OUT_OF_MEMORY;
	instance->head.allocator     = *desc->allocator;
	instance->message_callback  = desc->message_callback;
	instance->message_user_data = desc->message_user_data;
	SPUD_BIRTH(instance);
	SPUD_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);
	*out_instance       = instance;
	return SPUD_SUCCESS;
}

void spudfile_instance_destroy(spudfile_instance instance) {
	if (!instance)
		return;
	if (SPUD_STALE(instance, "spudfile_instance_destroy", "instance"))
		return;
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	SPUD_QUARANTINE_RELEASE(&instance->quarantine);
	SPUD_DELETE(&allocator, struct spudfile_instance_t, instance);
}

/* ---- Opening ---- */

static bool spudfile_path_given(const char *path) { return path && path[0]; }

SPUDRESULT spudfile_file_open(
    spudfile_instance instance,
    const spudfile_open_desc *desc,
    spudfile_file *out_file) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_file_open", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_result = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_FILES_OPEN_DESC);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (!out_file)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_file = NULL;
	if (!spudfile_path_given(desc->path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (desc->access != SPUDFILE_ACCESS_READ && desc->access != SPUDFILE_ACCESS_WRITE && desc->access != SPUDFILE_ACCESS_READ_WRITE)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->create < SPUDFILE_CREATE_OPEN_EXISTING || desc->create > SPUDFILE_CREATE_TRUNCATE)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->share & ~(SPUDFILE_SHARE)(SPUDFILE_SHARE_READ | SPUDFILE_SHARE_WRITE | SPUDFILE_SHARE_DELETE))
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->create == SPUDFILE_CREATE_TRUNCATE && desc->access == SPUDFILE_ACCESS_READ)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	spudfile_file file = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudfile_file_t);
	if (!file)
		return SPUDRESULT_OUT_OF_MEMORY;
	file->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(file, &instance->quarantine);
	file->instance       = instance;
	file->access         = desc->access;

	const spudfile_report report = spudfile_instance_report(instance, "spudfile_file_open", desc->debug_name);
	const SPUDRESULT result      = spudfile_backend_open(&report, desc, &file->native);
	if (result != SPUD_SUCCESS) {
		const SPUD_ALLOCATOR allocator = file->head.allocator;
		SPUD_DELETE(&allocator, struct spudfile_file_t, file);
		return result;
	}
	if (spud_debug_name_set(file, desc->debug_name) != SPUD_SUCCESS) {
		spudfile_file_close(file);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	*out_file = file;
	return SPUD_SUCCESS;
}

void spudfile_file_close(spudfile_file file) {
	if (!file)
		return;
	if (SPUD_STALE(file, "spudfile_file_close", "file"))
		return;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_close");
	// Windows releases a closed handle's locks only when the system gets to
	// it; let go first, so the next locker never waits on a closed file.
	if (file->lock != SPUDFILE_LOCK_NONE)
		spudfile_backend_unlock(&report, file->native);
	spudfile_backend_close(&report, file->native);
	SPUD_KILL(file);
	const SPUD_ALLOCATOR allocator = file->head.allocator;
	spud_string_free(&allocator, file->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(file), &allocator, struct spudfile_file_t, file);
}

/* ---- I/O at an offset ---- */

static bool spudfile_readable(spudfile_file file) { return file->access == SPUDFILE_ACCESS_READ || file->access == SPUDFILE_ACCESS_READ_WRITE; }
static bool spudfile_writable(spudfile_file file) { return file->access == SPUDFILE_ACCESS_WRITE || file->access == SPUDFILE_ACCESS_READ_WRITE; }

SPUDRESULT spudfile_file_read_at(
    spudfile_file file,
    uint64_t offset,
    void *buffer,
    uint64_t size,
    uint64_t *out_read) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_read_at", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (!buffer || !out_read)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_read = 0;
	if (!spudfile_readable(file))
		return SPUDRESULT_SPUDFILE_NOT_READABLE;
	if (size == 0)
		return SPUD_SUCCESS;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_read_at");
	return spudfile_backend_read_at(&report, file->native, offset, buffer, size, out_read);
}

SPUDRESULT spudfile_file_write_at(
    spudfile_file file,
    uint64_t offset,
    const void *data,
    uint64_t size) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_write_at", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (!data && size != 0)
		return SPUDRESULT_NULL_DESC;
	if (!spudfile_writable(file))
		return SPUDRESULT_SPUDFILE_NOT_WRITABLE;
	if (size == 0)
		return SPUD_SUCCESS;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_write_at");
	return spudfile_backend_write_at(&report, file->native, offset, data, size);
}

SPUDRESULT spudfile_file_get_size(
    spudfile_file file,
    uint64_t *out_size) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_get_size", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (!out_size)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_get_size");
	return spudfile_backend_get_size(&report, file->native, out_size);
}

SPUDRESULT spudfile_file_set_size(
    spudfile_file file,
    uint64_t size) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_set_size", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (!spudfile_writable(file))
		return SPUDRESULT_SPUDFILE_NOT_WRITABLE;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_set_size");
	return spudfile_backend_set_size(&report, file->native, size);
}

SPUDRESULT spudfile_file_flush(
    spudfile_file file,
    SPUDFILE_FLUSH_LEVEL level) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_flush", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (level != SPUDFILE_FLUSH_LEVEL_DEVICE && level != SPUDFILE_FLUSH_LEVEL_MEDIA)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (!spudfile_writable(file))
		return SPUDRESULT_SPUDFILE_NOT_WRITABLE;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_flush");
	return spudfile_backend_flush(&report, file->native, level);
}

/* ---- Mapping ---- */

SPUDRESULT spudfile_file_map(
    spudfile_file file,
    uint64_t offset,
    uint64_t size,
    spudfile_mapping *out_mapping) {
	if (!out_mapping)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_mapping = NULL;
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_map", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (size == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (!spudfile_readable(file))
		return SPUDRESULT_SPUDFILE_NOT_READABLE;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_map");
	uint64_t file_size           = 0;
	const SPUDRESULT sized       = spudfile_backend_get_size(&report, file->native, &file_size);
	if (sized != SPUD_SUCCESS)
		return sized;
	if (offset > file_size || size > file_size - offset)
		return SPUDRESULT_INDEX_OUT_OF_RANGE;

	spudfile_mapping mapping = SPUD_NEW_ZEROED(&file->head.allocator, struct spudfile_mapping_t);
	if (!mapping)
		return SPUDRESULT_OUT_OF_MEMORY;
	mapping->head.allocator = file->head.allocator;
	SPUD_BIRTH_IN(mapping, &file->instance->quarantine);
	mapping->instance       = file->instance;
	uint64_t lead           = 0;
	const SPUDRESULT result = spudfile_backend_map(&report, file->native, offset, size, &mapping->base, &mapping->base_size, &lead);
	if (result != SPUD_SUCCESS) {
		const SPUD_ALLOCATOR allocator = mapping->head.allocator;
		SPUD_DELETE(&allocator, struct spudfile_mapping_t, mapping);
		return result;
	}
	mapping->data = (const uint8_t *)mapping->base + lead;
	mapping->size = size;
	*out_mapping  = mapping;
	return SPUD_SUCCESS;
}

const void *spudfile_mapping_get_data(spudfile_mapping mapping) { return mapping ? mapping->data : NULL; }

uint64_t spudfile_mapping_get_size(spudfile_mapping mapping) {
	if (!mapping)
		return 0;
	if (SPUD_STALE(mapping, "spudfile_mapping_get_size", "mapping"))
		return 0;
	return mapping->size;
}

void spudfile_mapping_release(spudfile_mapping mapping) {
	if (!mapping)
		return;
	if (SPUD_STALE(mapping, "spudfile_mapping_release", "mapping"))
		return;
	const spudfile_report report = spudfile_object_report(mapping->instance, "spudfile_mapping_release", mapping, mapping->head.debug_name);
	spudfile_backend_unmap(&report, mapping->base, mapping->base_size);
	SPUD_KILL(mapping);
	const SPUD_ALLOCATOR allocator = mapping->head.allocator;
	spud_string_free(&allocator, mapping->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(mapping), &allocator, struct spudfile_mapping_t, mapping);
}

/* ---- Paths ---- */

SPUDRESULT spudfile_path_get_info(
    spudfile_instance instance,
    const char *path,
    SPUDFILE_PATH_INFO *out_info) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_path_get_info", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (!out_info)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_result = spud_struct_out_check(&out_info->base, SPUD_STRUCT_TYPE_FILES_PATH_INFO);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	out_info->kind               = (SPUDFILE_PATH_KIND)0;
	out_info->size               = 0;
	out_info->modified_ns        = 0;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_path_get_info", NULL);
	return spudfile_backend_path_get_info(&report, path, out_info);
}

SPUDRESULT spudfile_path_replace(
    spudfile_instance instance,
    const char *source,
    const char *target,
    SPUDFILE_FLUSH_LEVEL level) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_path_replace", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(source))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (!spudfile_path_given(target))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (level != SPUDFILE_FLUSH_LEVEL_NONE && level != SPUDFILE_FLUSH_LEVEL_DEVICE && level != SPUDFILE_FLUSH_LEVEL_MEDIA)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_path_replace", NULL);
	return spudfile_backend_path_replace(&report, source, target, level);
}

SPUDRESULT spudfile_path_remove(
    spudfile_instance instance,
    const char *path) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_path_remove", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_path_remove", NULL);
	return spudfile_backend_path_remove(&report, path);
}

SPUDRESULT spudfile_path_copy_mode(
    spudfile_instance instance,
    const char *source,
    const char *target) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_path_copy_mode", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(source))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (!spudfile_path_given(target))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_path_copy_mode", NULL);
	return spudfile_backend_path_copy_mode(&report, source, target);
}

SPUDRESULT spudfile_directory_create(
    spudfile_instance instance,
    const char *path) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_directory_create", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_directory_create", NULL);
	return spudfile_backend_directory_create(&report, path);
}

SPUDRESULT spudfile_directory_remove(
    spudfile_instance instance,
    const char *path) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_directory_remove", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_directory_remove", NULL);
	return spudfile_backend_directory_remove(&report, path);
}

SPUDRESULT spudfile_directory_rename(
    spudfile_instance instance,
    const char *source,
    const char *target,
    SPUDFILE_FLUSH_LEVEL level) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_directory_rename", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!spudfile_path_given(source))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (!spudfile_path_given(target))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (level != SPUDFILE_FLUSH_LEVEL_NONE && level != SPUDFILE_FLUSH_LEVEL_DEVICE && level != SPUDFILE_FLUSH_LEVEL_MEDIA)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	const spudfile_report report = spudfile_instance_report(instance, "spudfile_directory_rename", NULL);
	return spudfile_backend_directory_rename(&report, source, target, level);
}

/* ---- Listing a directory ---- */

SPUDRESULT spudfile_listing_open(
    spudfile_instance instance,
    const spudfile_listing_desc *desc,
    spudfile_listing *out_listing) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_listing_open", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_result = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_FILES_LISTING_DESC);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (!out_listing)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_listing = NULL;
	if (!spudfile_path_given(desc->path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;

	spudfile_listing listing = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudfile_listing_t);
	if (!listing)
		return SPUDRESULT_OUT_OF_MEMORY;
	listing->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(listing, &instance->quarantine);
	listing->instance       = instance;

	const spudfile_report report = spudfile_instance_report(instance, "spudfile_listing_open", desc->debug_name);
	const SPUDRESULT result      = spudfile_backend_listing_open(&report, desc->path, &listing->native);
	if (result != SPUD_SUCCESS) {
		const SPUD_ALLOCATOR allocator = listing->head.allocator;
		SPUD_DELETE(&allocator, struct spudfile_listing_t, listing);
		return result;
	}
	if (spud_debug_name_set(listing, desc->debug_name) != SPUD_SUCCESS) {
		spudfile_listing_close(listing);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	*out_listing = listing;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_listing_next(
    spudfile_listing listing,
    SPUDFILE_LISTING_ENTRY *out_entry) {
	if (!listing)
		return SPUDRESULT_SPUDFILE_INVALID_LISTING;
	if (SPUD_STALE(listing, "spudfile_listing_next", "listing"))
		return SPUDRESULT_SPUDFILE_INVALID_LISTING;
	if (!out_entry)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_result = spud_struct_out_check(&out_entry->base, SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (listing->ended)
		return SPUDRESULT_SPUDFILE_END_OF_LISTING;
	const spudfile_report report = spudfile_object_report(listing->instance, "spudfile_listing_next", listing, listing->head.debug_name);
	const SPUDRESULT result      = spudfile_backend_listing_next(&report, listing->native, out_entry);
	if (result == SPUDRESULT_SPUDFILE_END_OF_LISTING)
		listing->ended = true;
	return result;
}

void spudfile_listing_close(spudfile_listing listing) {
	if (!listing)
		return;
	if (SPUD_STALE(listing, "spudfile_listing_close", "listing"))
		return;
	const spudfile_report report = spudfile_object_report(listing->instance, "spudfile_listing_close", listing, listing->head.debug_name);
	spudfile_backend_listing_close(&report, listing->native);
	SPUD_KILL(listing);
	const SPUD_ALLOCATOR allocator = listing->head.allocator;
	spud_string_free(&allocator, listing->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(listing), &allocator, struct spudfile_listing_t, listing);
}

/* RFC 3629's UTF-8: no surrogate, no overlong form, nothing past U+10FFFF. */
bool spudfile_utf8_valid(const char *bytes, uint32_t size) {
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

SPUDRESULT spudfile_file_lock(
    spudfile_file file,
    SPUDFILE_LOCK lock,
    bool wait) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_lock", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (lock != SPUDFILE_LOCK_SHARED && lock != SPUDFILE_LOCK_EXCLUSIVE)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (file->lock != SPUDFILE_LOCK_NONE)
		return SPUDRESULT_SPUDFILE_ALREADY_LOCKED;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_lock");
	const SPUDRESULT result      = spudfile_backend_lock(&report, file->native, lock, wait);
	if (result == SPUD_SUCCESS)
		file->lock = (uint8_t)lock;
	return result;
}

SPUDRESULT spudfile_file_unlock(spudfile_file file) {
	if (!file)
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (SPUD_STALE(file, "spudfile_file_unlock", "file"))
		return SPUDRESULT_SPUDFILE_INVALID_FILE;
	if (file->lock == SPUDFILE_LOCK_NONE)
		return SPUDRESULT_SPUDFILE_NOT_LOCKED;
	const spudfile_report report = spudfile_file_report(file, "spudfile_file_unlock");
	const SPUDRESULT result      = spudfile_backend_unlock(&report, file->native);
	if (result == SPUD_SUCCESS)
		file->lock = SPUDFILE_LOCK_NONE;
	return result;
}

/* ---- Dynamic libraries ---- */

SPUDRESULT spudfile_library_open(
    spudfile_instance instance,
    const spudfile_library_desc *desc,
    spudfile_library *out_library) {
	if (!instance)
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudfile_library_open", "instance"))
		return SPUDRESULT_SPUDFILE_INVALID_INSTANCE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_result = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_FILES_LIBRARY_DESC);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (!out_library)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_library = NULL;
	if (!spudfile_path_given(desc->path))
		return SPUDRESULT_SPUDFILE_NULL_PATH;
	if (desc->binding != SPUDFILE_LIBRARY_BINDING_NOW && desc->binding != SPUDFILE_LIBRARY_BINDING_LAZY)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->scope != SPUDFILE_LIBRARY_SCOPE_LOCAL && desc->scope != SPUDFILE_LIBRARY_SCOPE_GLOBAL)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->dependencies != SPUDFILE_LIBRARY_DEPENDENCIES_PLATFORM_ORDER && desc->dependencies != SPUDFILE_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (!spudfile_backend_library_binding_supported(desc->binding))
		return SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED;
	if (!spudfile_backend_library_scope_supported(desc->scope))
		return SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED;
	if (!spudfile_backend_library_dependencies_supported(desc->dependencies))
		return SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED;
	if (desc->dependencies == SPUDFILE_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST && !spudfile_backend_path_fully_qualified(desc->path))
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	spudfile_library library = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudfile_library_t);
	if (!library)
		return SPUDRESULT_OUT_OF_MEMORY;
	library->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(library, &instance->quarantine);
	library->instance       = instance;

	const spudfile_report report = spudfile_instance_report(instance, "spudfile_library_open", desc->debug_name);
	const SPUDRESULT result      = spudfile_backend_library_open(&report, desc, &library->native);
	if (result != SPUD_SUCCESS) {
		const SPUD_ALLOCATOR allocator = library->head.allocator;
		SPUD_DELETE(&allocator, struct spudfile_library_t, library);
		return result;
	}
	// The last step: a failed copy closes the library, leaving it unloaded.
	if (spud_debug_name_set(library, desc->debug_name) != SPUD_SUCCESS) {
		spudfile_library_close(library);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	*out_library = library;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_library_find(
    spudfile_library library,
    const char *name,
    spudfile_library_function *out_function) {
	if (!library)
		return SPUDRESULT_SPUDFILE_INVALID_LIBRARY;
	if (SPUD_STALE(library, "spudfile_library_find", "library"))
		return SPUDRESULT_SPUDFILE_INVALID_LIBRARY;
	// name null: stops at !name, name[0] is never read.
	if (!name || !name[0])
		return SPUDRESULT_NULL_DESC;
	if (!out_function)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_function                = NULL;
	const spudfile_report report = spudfile_object_report(library->instance, "spudfile_library_find", library, library->head.debug_name);
	return spudfile_backend_library_find(&report, library->native, name, out_function);
}

void spudfile_library_close(spudfile_library library) {
	if (!library)
		return;
	if (SPUD_STALE(library, "spudfile_library_close", "library"))
		return;
	const spudfile_report report = spudfile_object_report(library->instance, "spudfile_library_close", library, library->head.debug_name);
	spudfile_backend_library_close(&report, library->native);
	SPUD_KILL(library);
	const SPUD_ALLOCATOR allocator = library->head.allocator;
	spud_string_free(&allocator, library->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(library), &allocator, struct spudfile_library_t, library);
}
