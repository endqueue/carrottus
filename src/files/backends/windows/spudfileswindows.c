#if SPUDLIB_PLATFORM_WIN32

#include "../../spudfilesshared.h"

#include <Windows.h>
#include <stdint.h>
#include <string.h>

/* The UTF-16 units [wtf8] stands for, written to [out] when it isn't null,
 * counted either way; false for bytes that aren't WTF-8: UTF-8 (RFC 3629)
 * that may also hold an unpaired surrogate as its three bytes, but never a
 * lead surrogate's three bytes followed by a trail's (that pair is the code
 * point's own four bytes). MultiByteToWideChar refuses every surrogate, so
 * SpudFiles converts paths itself. */
static bool spudfile_windows_from_wtf8(const char *wtf8, wchar_t *out, size_t *out_count) {
	const uint8_t *at = (const uint8_t *)wtf8;
	size_t count      = 0;
	bool after_lead   = false; // the last unit written was a lone lead surrogate
	while (*at) {
		const uint8_t lead = *at;
		uint32_t code      = 0;
		uint32_t extra     = 0;
		uint32_t least     = 0;
		if (lead < 0x80) {
			code = lead;
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			code  = lead & 0x1Fu;
			extra = 1;
			least = 0x80;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			code  = lead & 0x0Fu;
			extra = 2;
			least = 0x800;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			code  = lead & 0x07u;
			extra = 3;
			least = 0x10000;
		} else {
			return false;
		}
		for (uint32_t i = 1; i <= extra; ++i) {
			if ((at[i] & 0xC0u) != 0x80u) // a null ends it here too
				return false;
			code = (code << 6) | (at[i] & 0x3Fu);
		}
		if (code < least || code > 0x10FFFFu)
			return false;
		const bool trail = code >= 0xDC00u && code <= 0xDFFFu;
		if (trail && after_lead)
			return false;
		after_lead = code >= 0xD800u && code <= 0xDBFFu;
		if (code >= 0x10000u) {
			if (out) {
				out[count]     = (wchar_t)(0xD800u + ((code - 0x10000u) >> 10));
				out[count + 1] = (wchar_t)(0xDC00u + ((code - 0x10000u) & 0x3FFu));
			}
			count += 2;
		} else {
			if (out)
				out[count] = (wchar_t)code;
			count += 1;
		}
		at += extra + 1;
	}
	if (out)
		out[count] = 0;
	*out_count = count + 1;
	return true;
}

/* [wtf8] as UTF-16, through [instance]'s allocator; freed with
 * spudfile_windows_free_wide and the count it gives. */
static SPUDRESULT spudfile_windows_wide(spudfile_instance instance, const char *wtf8, wchar_t **out_wide, int *out_count) {
	*out_wide    = NULL;
	*out_count   = 0;
	size_t count = 0;
	if (!spudfile_windows_from_wtf8(wtf8, NULL, &count))
		return SPUDRESULT_SPUDFILE_NOT_FOUND; // not WTF-8: no file can have that name
	if (count > (size_t)INT32_MAX)
		return SPUDRESULT_SPUDFILE_NOT_FOUND; // longer than any path Windows takes
	wchar_t *wide = SPUD_NEW_ARRAY(&instance->head.allocator, wchar_t, count);
	if (!wide)
		return SPUDRESULT_OUT_OF_MEMORY;
	spudfile_windows_from_wtf8(wtf8, wide, &count);
	*out_wide  = wide;
	*out_count = (int)count;
	return SPUD_SUCCESS;
}

static void spudfile_windows_free_wide(spudfile_instance instance, wchar_t *wide, int count) {
	if (wide)
		SPUD_DELETE_ARRAY(&instance->head.allocator, wchar_t, wide, (size_t)count);
}

/* Sends GetLastError's [error] for [report]'s call, and returns [result]
 * (spudcore.h, "Messages", M2). Win32's numbers carry no text of their
 * own: FormatMessage reads them. */
static SPUDRESULT spudfile_windows_fail(const spudfile_report *report, DWORD error, SPUDRESULT result) {
	spudfile_report_platform(report, result, SPUD_PLATFORM_ERROR_SOURCE_WIN32, (int64_t)error, NULL);
	return result;
}

static bool spudfile_windows_is_directory(const wchar_t *path) {
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* The result a GetLastError() value stands for. Whatever has no result of
 * its own is the platform's failure. */
static SPUDRESULT spudfile_windows_result(DWORD error) {
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_NETPATH:
		return SPUDRESULT_SPUDFILE_NOT_FOUND;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return SPUDRESULT_SPUDFILE_ALREADY_EXISTS;
	case ERROR_ACCESS_DENIED:
	case ERROR_WRITE_PROTECT:
		return SPUDRESULT_SPUDFILE_ACCESS_DENIED;
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_USER_MAPPED_FILE:
		return SPUDRESULT_SPUDFILE_IN_USE;
	case ERROR_NOT_SAME_DEVICE:
		return SPUDRESULT_SPUDFILE_DIFFERENT_VOLUME;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return SPUDRESULT_OUT_OF_MEMORY;
	default:
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
}

/* [error]'s result, sent. */
static SPUDRESULT spudfile_windows_failed(const spudfile_report *report, DWORD error) {
	return spudfile_windows_fail(report, error, spudfile_windows_result(error));
}

SPUDRESULT spudfile_backend_open(const spudfile_report *report, const spudfile_open_desc *desc, intptr_t *out_native) {
	DWORD access = 0;
	if (desc->access == SPUDFILE_ACCESS_READ || desc->access == SPUDFILE_ACCESS_READ_WRITE)
		access |= GENERIC_READ;
	if (desc->access == SPUDFILE_ACCESS_WRITE || desc->access == SPUDFILE_ACCESS_READ_WRITE)
		access |= GENERIC_WRITE;
	DWORD disposition = 0;
	switch (desc->create) {
	case SPUDFILE_CREATE_OPEN_EXISTING:
		disposition = OPEN_EXISTING;
		break;
	case SPUDFILE_CREATE_NEW:
		disposition = CREATE_NEW;
		break;
	case SPUDFILE_CREATE_OPEN_OR_CREATE:
		disposition = OPEN_ALWAYS;
		break;
	case SPUDFILE_CREATE_TRUNCATE:
		disposition = CREATE_ALWAYS;
		break;
	default:
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	}
	DWORD share = 0;
	if (desc->share & SPUDFILE_SHARE_READ)
		share |= FILE_SHARE_READ;
	if (desc->share & SPUDFILE_SHARE_WRITE)
		share |= FILE_SHARE_WRITE;
	if (desc->share & SPUDFILE_SHARE_DELETE)
		share |= FILE_SHARE_DELETE;

	spudfile_instance instance = report->instance;
	wchar_t *path              = NULL;
	int count                  = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, desc->path, &path, &count);
	if (result != SPUD_SUCCESS)
		return result;
	HANDLE handle = CreateFileW(path, access, share, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		const DWORD error = GetLastError();
		// A directory is refused as access denied.
		result = error == ERROR_ACCESS_DENIED && spudfile_windows_is_directory(path) ? SPUDRESULT_SPUDFILE_NOT_A_FILE : spudfile_windows_result(error);
		spudfile_windows_free_wide(instance, path, count);
		return spudfile_windows_fail(report, error, result);
	}
	spudfile_windows_free_wide(instance, path, count);
	// A device (CON, a pipe) opens too; a file is what is wanted.
	if (GetFileType(handle) != FILE_TYPE_DISK) {
		if (!CloseHandle(handle))
			spudfile_windows_fail(report, GetLastError(), SPUDRESULT_SPUDFILE_NOT_A_FILE);
		return SPUDRESULT_SPUDFILE_NOT_A_FILE;
	}
	*out_native = (intptr_t)handle;
	return SPUD_SUCCESS;
}

void spudfile_backend_close(const spudfile_report *report, intptr_t native) {
	if (!CloseHandle((HANDLE)native))
		spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
}

/* ReadFile and WriteFile take a DWORD count. */
static const uint64_t SPUDFILE_WINDOWS_CHUNK = 1u << 30;

SPUDRESULT spudfile_backend_read_at(const spudfile_report *report, intptr_t native, uint64_t offset, void *buffer, uint64_t size, uint64_t *out_read) {
	uint8_t *dst   = (uint8_t *)buffer;
	uint64_t total = 0;
	while (total < size) {
		const uint64_t remaining = size - total;
		const DWORD chunk        = (DWORD)(remaining > SPUDFILE_WINDOWS_CHUNK ? SPUDFILE_WINDOWS_CHUNK : remaining);
		const uint64_t at        = offset + total;
		OVERLAPPED position;
		ZeroMemory(&position, sizeof(position));
		position.Offset     = (DWORD)(at & 0xFFFFFFFFu);
		position.OffsetHigh = (DWORD)(at >> 32);
		DWORD n             = 0;
		if (!ReadFile((HANDLE)native, dst + total, chunk, &n, &position)) {
			const DWORD error = GetLastError();
			if (error == ERROR_HANDLE_EOF)
				break; // at or past the end: an answer, not a failure
			*out_read = total;
			return spudfile_windows_fail(report, error, SPUDRESULT_API_SPECIFIC_FAILURE);
		}
		if (n == 0)
			break;
		total += n;
	}
	*out_read = total;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_write_at(const spudfile_report *report, intptr_t native, uint64_t offset, const void *data, uint64_t size) {
	const uint8_t *src = (const uint8_t *)data;
	uint64_t total     = 0;
	while (total < size) {
		const uint64_t remaining = size - total;
		const DWORD chunk        = (DWORD)(remaining > SPUDFILE_WINDOWS_CHUNK ? SPUDFILE_WINDOWS_CHUNK : remaining);
		const uint64_t at        = offset + total;
		OVERLAPPED position;
		ZeroMemory(&position, sizeof(position));
		position.Offset     = (DWORD)(at & 0xFFFFFFFFu);
		position.OffsetHigh = (DWORD)(at >> 32);
		DWORD n             = 0;
		if (!WriteFile((HANDLE)native, src + total, chunk, &n, &position))
			return spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
		if (n == 0)
			return SPUDRESULT_API_SPECIFIC_FAILURE; // written nothing, and said nothing

		total += n;
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_get_size(const spudfile_report *report, intptr_t native, uint64_t *out_size) {
	LARGE_INTEGER size;
	if (!GetFileSizeEx((HANDLE)native, &size))
		return spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
	*out_size = (uint64_t)size.QuadPart;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_set_size(const spudfile_report *report, intptr_t native, uint64_t size) {
	FILE_END_OF_FILE_INFO end;
	end.EndOfFile.QuadPart = (LONGLONG)size;
	if (!SetFileInformationByHandle((HANDLE)native, FileEndOfFileInfo, &end, sizeof(end)))
		return spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_flush(const spudfile_report *report, intptr_t native, SPUDFILE_FLUSH_LEVEL level) {
	// FlushFileBuffers reaches the media on Windows: DEVICE and MEDIA alike.
	(void)level;
	if (!FlushFileBuffers((HANDLE)native))
		return spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_map(const spudfile_report *report, intptr_t native, uint64_t offset, uint64_t size, void **out_base, uint64_t *out_base_size, uint64_t *out_lead) {
	// View offsets must be multiples of the allocation granularity (64 KiB),
	// not just the page size: map from the granule holding [offset].
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	const uint64_t granularity = info.dwAllocationGranularity;
	const uint64_t aligned     = offset - offset % granularity;
	const uint64_t lead        = offset - aligned;
	if (size > (uint64_t)SIZE_MAX - lead)
		return SPUDRESULT_INDEX_OUT_OF_RANGE; // more than the address space

	HANDLE section = CreateFileMappingW((HANDLE)native, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!section)
		return spudfile_windows_failed(report, GetLastError());
	void *base = MapViewOfFile(section, FILE_MAP_READ, (DWORD)(aligned >> 32), (DWORD)(aligned & 0xFFFFFFFFu), (SIZE_T)(lead + size));
	// The view keeps the section, and through it the file, alive on its own.
	const DWORD error = base ? 0 : GetLastError();
	CloseHandle(section);
	if (!base)
		return spudfile_windows_failed(report, error);
	*out_base      = base;
	*out_base_size = lead + size;
	*out_lead      = lead;
	return SPUD_SUCCESS;
}

void spudfile_backend_unmap(const spudfile_report *report, void *base, uint64_t base_size) {
	(void)base_size;
	if (!UnmapViewOfFile(base))
		spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
}

/* Nanoseconds since 1970 from a FILETIME's 100 ns since 1601. */
static int64_t spudfile_windows_unix_ns(FILETIME time) {
	const uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
	return ((int64_t)ticks - 116444736000000000LL) * 100;
}

SPUDRESULT spudfile_backend_path_get_info(const spudfile_report *report, const char *path, SPUDFILE_PATH_INFO *out_info) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, path, &wide, &count);
	if (result != SPUD_SUCCESS)
		return result;
	// Opened for nothing but its attributes, with backup semantics so that a
	// directory opens too; a symbolic link is followed, as stat follows it.
	HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
	                            FILE_FLAG_BACKUP_SEMANTICS, NULL);
	const DWORD error = handle == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	spudfile_windows_free_wide(instance, wide, count);
	if (handle == INVALID_HANDLE_VALUE)
		return spudfile_windows_failed(report, error);
	BY_HANDLE_FILE_INFORMATION information;
	if (!GetFileInformationByHandle(handle, &information)) {
		const DWORD failed = GetLastError();
		CloseHandle(handle);
		return spudfile_windows_fail(report, failed, SPUDRESULT_API_SPECIFIC_FAILURE);
	}
	const DWORD type = GetFileType(handle);
	CloseHandle(handle);
	if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		out_info->kind = SPUDFILE_PATH_KIND_DIRECTORY;
		out_info->size = 0;
	} else if (type == FILE_TYPE_DISK) {
		out_info->kind = SPUDFILE_PATH_KIND_FILE;
		out_info->size = ((uint64_t)information.nFileSizeHigh << 32) | information.nFileSizeLow;
	} else {
		out_info->kind = SPUDFILE_PATH_KIND_OTHER;
		out_info->size = 0;
	}
	out_info->modified_ns = spudfile_windows_unix_ns(information.ftLastWriteTime);
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_path_replace(const spudfile_report *report, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level) {
	spudfile_instance instance = report->instance;
	wchar_t *wide_source       = NULL;
	wchar_t *wide_target       = NULL;
	int source_count           = 0;
	int target_count           = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, source, &wide_source, &source_count);
	if (result == SPUD_SUCCESS)
		result = spudfile_windows_wide(instance, target, &wide_target, &target_count);
	if (result == SPUD_SUCCESS) {
		DWORD flags = MOVEFILE_REPLACE_EXISTING;
		if (level != SPUDFILE_FLUSH_LEVEL_NONE)
			flags |= MOVEFILE_WRITE_THROUGH;
		if (!MoveFileExW(wide_source, wide_target, flags)) {
			const DWORD error = GetLastError();
			// A target open without FILE_SHARE_DELETE, mapped or read-only is
			// reported as access denied; a directory target the same way.
			if (error == ERROR_ACCESS_DENIED)
				result = spudfile_windows_is_directory(wide_target) ? SPUDRESULT_SPUDFILE_NOT_A_FILE : SPUDRESULT_SPUDFILE_IN_USE;
			else
				result = spudfile_windows_result(error);
			spudfile_windows_fail(report, error, result);
		}
	}
	spudfile_windows_free_wide(instance, wide_source, source_count);
	spudfile_windows_free_wide(instance, wide_target, target_count);
	return result;
}

SPUDRESULT spudfile_backend_path_remove(const spudfile_report *report, const char *path) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, path, &wide, &count);
	if (result != SPUD_SUCCESS)
		return result;
	if (!DeleteFileW(wide)) {
		const DWORD error = GetLastError();
		// An open or mapped file, a read-only one, or a directory: access
		// denied for all of them, told apart by what is there.
		if (error == ERROR_ACCESS_DENIED)
			result = spudfile_windows_is_directory(wide) ? SPUDRESULT_SPUDFILE_NOT_A_FILE : SPUDRESULT_SPUDFILE_IN_USE;
		else
			result = spudfile_windows_result(error);
		spudfile_windows_fail(report, error, result);
	}
	spudfile_windows_free_wide(instance, wide, count);
	return result;
}

/* What is at [path], a link followed, opened for [access] to its attributes
 * alone (no sharing check), with backup semantics so a directory opens too;
 * INVALID_HANDLE_VALUE with *out_result set otherwise. */
static HANDLE spudfile_windows_open_attributes(const spudfile_report *report, const char *path, DWORD access, SPUDRESULT *out_result) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	*out_result                = spudfile_windows_wide(instance, path, &wide, &count);
	if (*out_result != SPUD_SUCCESS)
		return INVALID_HANDLE_VALUE;
	HANDLE handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		*out_result = spudfile_windows_failed(report, GetLastError());
	spudfile_windows_free_wide(instance, wide, count);
	return handle;
}

SPUDRESULT spudfile_backend_path_copy_mode(const spudfile_report *report, const char *source, const char *target) {
	SPUDRESULT result = SPUD_SUCCESS;
	HANDLE from       = spudfile_windows_open_attributes(report, source, FILE_READ_ATTRIBUTES, &result);
	if (from == INVALID_HANDLE_VALUE)
		return result;
	FILE_BASIC_INFO source_info;
	const BOOL read         = GetFileInformationByHandleEx(from, FileBasicInfo, &source_info, sizeof(source_info));
	const DWORD read_error  = read ? 0 : GetLastError();
	CloseHandle(from);
	if (!read)
		return spudfile_windows_fail(report, read_error, SPUDRESULT_API_SPECIFIC_FAILURE);
	HANDLE to = spudfile_windows_open_attributes(report, target, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, &result);
	if (to == INVALID_HANDLE_VALUE)
		return result;
	FILE_BASIC_INFO info;
	if (!GetFileInformationByHandleEx(to, FileBasicInfo, &info, sizeof(info))) {
		const DWORD error = GetLastError();
		CloseHandle(to);
		return spudfile_windows_fail(report, error, SPUDRESULT_API_SPECIFIC_FAILURE);
	}
	// Only the read-only attribute is the source's; the target keeps every
	// other, and its times (zero leaves a time as it is).
	const DWORD wanted = (info.FileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY) | (source_info.FileAttributes & FILE_ATTRIBUTE_READONLY);
	if (wanted != info.FileAttributes) {
		FILE_BASIC_INFO change;
		ZeroMemory(&change, sizeof(change));
		// Zero would mean "unchanged": a file with no attribute left is
		// FILE_ATTRIBUTE_NORMAL.
		change.FileAttributes = wanted ? wanted : FILE_ATTRIBUTE_NORMAL;
		if (!SetFileInformationByHandle(to, FileBasicInfo, &change, sizeof(change))) {
			const DWORD error = GetLastError();
			// A read-only volume is the platform's refusal, not a permission.
			result = error == ERROR_WRITE_PROTECT ? SPUDRESULT_API_SPECIFIC_FAILURE : spudfile_windows_result(error);
			spudfile_windows_fail(report, error, result);
		}
	}
	CloseHandle(to);
	return result;
}

SPUDRESULT spudfile_backend_directory_create(const spudfile_report *report, const char *path) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, path, &wide, &count);
	if (result != SPUD_SUCCESS)
		return result;
	if (!CreateDirectoryW(wide, NULL))
		result = spudfile_windows_failed(report, GetLastError());
	spudfile_windows_free_wide(instance, wide, count);
	return result;
}

/* What is at [path] itself, a link not followed: a symbolic link to a
 * directory, or a junction, carries the directory attribute and is removed
 * or renamed as one, itself. */
static SPUDRESULT spudfile_windows_directory_at(const spudfile_report *report, const wchar_t *path) {
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return spudfile_windows_failed(report, GetLastError());
	return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? SPUD_SUCCESS : SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
}

SPUDRESULT spudfile_backend_directory_remove(const spudfile_report *report, const char *path) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, path, &wide, &count);
	if (result != SPUD_SUCCESS)
		return result;
	result = spudfile_windows_directory_at(report, wide);
	if (result == SPUD_SUCCESS && !RemoveDirectoryW(wide)) {
		const DWORD error = GetLastError();
		if (error == ERROR_DIR_NOT_EMPTY)
			result = SPUDRESULT_SPUDFILE_NOT_EMPTY;
		else if (error == ERROR_DIRECTORY)
			result = SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
		else
			result = spudfile_windows_result(error);
		spudfile_windows_fail(report, error, result);
	}
	spudfile_windows_free_wide(instance, wide, count);
	return result;
}

SPUDRESULT spudfile_backend_directory_rename(const spudfile_report *report, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level) {
	spudfile_instance instance = report->instance;
	wchar_t *wide_source       = NULL;
	wchar_t *wide_target       = NULL;
	int source_count           = 0;
	int target_count           = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, source, &wide_source, &source_count);
	if (result == SPUD_SUCCESS)
		result = spudfile_windows_wide(instance, target, &wide_target, &target_count);
	if (result == SPUD_SUCCESS)
		result = spudfile_windows_directory_at(report, wide_source);
	if (result == SPUD_SUCCESS) {
		// No MOVEFILE_REPLACE_EXISTING: anything at the target refuses the
		// move, as a directory can't replace anything on Windows; no
		// MOVEFILE_COPY_ALLOWED: a directory is never copied across volumes.
		const DWORD flags = level != SPUDFILE_FLUSH_LEVEL_NONE ? MOVEFILE_WRITE_THROUGH : 0;
		if (!MoveFileExW(wide_source, wide_target, flags)) {
			const DWORD error = GetLastError();
			// Access denied is also what a directory in the way, or a handle
			// open inside the source, gives: told apart by what is there.
			if (error == ERROR_ACCESS_DENIED)
				result = GetFileAttributesW(wide_target) != INVALID_FILE_ATTRIBUTES ? SPUDRESULT_SPUDFILE_ALREADY_EXISTS : SPUDRESULT_SPUDFILE_IN_USE;
			else
				result = spudfile_windows_result(error);
			spudfile_windows_fail(report, error, result);
		}
	}
	spudfile_windows_free_wide(instance, wide_source, source_count);
	spudfile_windows_free_wide(instance, wide_target, target_count);
	return result;
}

/* ---- Listing a directory ---- */

/* A listing's state: the find handle, and the entry FindFirstFileExW gave
 * at open, not yet handed out. */
typedef struct spudfile_windows_listing {
	HANDLE find; /* INVALID_HANDLE_VALUE for an empty drive root */
	WIN32_FIND_DATAW data;
	bool pending;
} spudfile_windows_listing;

SPUDRESULT spudfile_backend_listing_open(const spudfile_report *report, const char *path, void **out_native) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	SPUDRESULT result          = spudfile_windows_wide(instance, path, &wide, &count);
	if (result != SPUD_SUCCESS)
		return result;
	// The pattern: the path, a separator unless it ends in one, and "*".
	const size_t length   = (size_t)count - 1;
	const bool separated  = length > 0 && (wide[length - 1] == L'\\' || wide[length - 1] == L'/');
	const size_t capacity = length + (separated ? 0 : 1) + 2;
	wchar_t *pattern      = SPUD_NEW_ARRAY(&instance->head.allocator, wchar_t, capacity);
	if (!pattern) {
		spudfile_windows_free_wide(instance, wide, count);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(pattern, wide, length * sizeof(wchar_t));
	size_t at = length;
	if (!separated)
		pattern[at++] = L'\\';
	pattern[at++] = L'*';
	pattern[at]   = 0;

	spudfile_windows_listing *listing = SPUD_NEW_ZEROED(&instance->head.allocator, spudfile_windows_listing);
	if (!listing) {
		SPUD_DELETE_ARRAY(&instance->head.allocator, wchar_t, pattern, capacity);
		spudfile_windows_free_wide(instance, wide, count);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	listing->find     = FindFirstFileExW(pattern, FindExInfoBasic, &listing->data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	const DWORD error = listing->find == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	SPUD_DELETE_ARRAY(&instance->head.allocator, wchar_t, pattern, capacity);
	if (listing->find != INVALID_HANDLE_VALUE) {
		spudfile_windows_free_wide(instance, wide, count);
		listing->pending = true;
		*out_native      = listing;
		return SPUD_SUCCESS;
	}
	// FindFirstFileExW answers a missing directory and a file alike: the
	// path's attributes tell them apart. A directory that gives no entry at
	// all is an empty drive root.
	const DWORD attributes = GetFileAttributesW(wide);
	spudfile_windows_free_wide(instance, wide, count);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		result = error == ERROR_ACCESS_DENIED ? SPUDRESULT_SPUDFILE_ACCESS_DENIED : SPUDRESULT_SPUDFILE_NOT_FOUND;
	else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
		result = SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
	else if (error == ERROR_FILE_NOT_FOUND)
		result = SPUD_SUCCESS;
	else
		result = spudfile_windows_result(error);
	if (result != SPUD_SUCCESS) {
		spudfile_windows_fail(report, error, result);
		SPUD_DELETE(&instance->head.allocator, spudfile_windows_listing, listing);
		return result;
	}
	*out_native = listing;
	return SPUD_SUCCESS;
}

/* [wide] (null-terminated) as WTF-8 into [out] of [capacity] bytes, its
 * null included; false when it doesn't fit. [*out_utf8] says whether every
 * surrogate was paired. */
static bool spudfile_windows_to_wtf8(const wchar_t *wide, char *out, uint32_t capacity, uint32_t *out_size, bool *out_utf8) {
	uint32_t size = 0;
	bool utf8     = true;
	for (const wchar_t *at = wide; *at; ++at) {
		uint32_t code = (uint32_t)*at;
		if (code >= 0xD800u && code <= 0xDBFFu && at[1] >= 0xDC00u && at[1] <= 0xDFFFu) {
			code = 0x10000u + ((code - 0xD800u) << 10) + ((uint32_t)at[1] - 0xDC00u);
			++at;
		} else if (code >= 0xD800u && code <= 0xDFFFu) {
			utf8 = false; // unpaired: its three bytes, as WTF-8 has it
		}
		uint8_t bytes[4];
		uint32_t n = 0;
		if (code < 0x80u) {
			bytes[n++] = (uint8_t)code;
		} else if (code < 0x800u) {
			bytes[n++] = (uint8_t)(0xC0u | (code >> 6));
			bytes[n++] = (uint8_t)(0x80u | (code & 0x3Fu));
		} else if (code < 0x10000u) {
			bytes[n++] = (uint8_t)(0xE0u | (code >> 12));
			bytes[n++] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
			bytes[n++] = (uint8_t)(0x80u | (code & 0x3Fu));
		} else {
			bytes[n++] = (uint8_t)(0xF0u | (code >> 18));
			bytes[n++] = (uint8_t)(0x80u | ((code >> 12) & 0x3Fu));
			bytes[n++] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
			bytes[n++] = (uint8_t)(0x80u | (code & 0x3Fu));
		}
		if (size + n >= capacity)
			return false;
		memcpy(out + size, bytes, n);
		size += n;
	}
	out[size] = 0;
	*out_size = size;
	*out_utf8 = utf8;
	return true;
}

/* What a found entry is: a reparse point by its tag, else by its
 * attributes. */
static SPUDFILE_ENTRY_KIND spudfile_windows_kind(const WIN32_FIND_DATAW *data) {
	if (data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		switch (data->dwReserved0) {
		case IO_REPARSE_TAG_SYMLINK:
		case IO_REPARSE_TAG_MOUNT_POINT:
			return SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK;
		case IO_REPARSE_TAG_AF_UNIX:
			return SPUDFILE_ENTRY_KIND_OTHER;
		default:
			break; // a placeholder or the like: what it stands for
		}
	}
	if (data->dwFileAttributes & FILE_ATTRIBUTE_DEVICE)
		return SPUDFILE_ENTRY_KIND_OTHER;
	if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return SPUDFILE_ENTRY_KIND_DIRECTORY;
	return SPUDFILE_ENTRY_KIND_FILE;
}

SPUDRESULT spudfile_backend_listing_next(const spudfile_report *report, void *native, SPUDFILE_LISTING_ENTRY *out_entry) {
	spudfile_windows_listing *listing = (spudfile_windows_listing *)native;
	if (listing->find == INVALID_HANDLE_VALUE)
		return SPUDRESULT_SPUDFILE_END_OF_LISTING;
	for (;;) {
		if (listing->pending) {
			listing->pending = false;
		} else if (!FindNextFileW(listing->find, &listing->data)) {
			const DWORD error = GetLastError();
			if (error == ERROR_NO_MORE_FILES)
				return SPUDRESULT_SPUDFILE_END_OF_LISTING; // an answer, not a failure
			return spudfile_windows_fail(report, error, SPUDRESULT_API_SPECIFIC_FAILURE);
		}
		const wchar_t *name = listing->data.cFileName;
		if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0)))
			continue;
		uint32_t size = 0;
		bool utf8     = true;
		// MAX_PATH units of WTF-8 always fit SPUDFILE_NAME_CAPACITY's bytes.
		if (!spudfile_windows_to_wtf8(name, out_entry->name, SPUDFILE_NAME_CAPACITY, &size, &utf8))
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		out_entry->kind         = spudfile_windows_kind(&listing->data);
		out_entry->name_is_utf8 = utf8;
		out_entry->name_size    = size;
		return SPUD_SUCCESS;
	}
}

void spudfile_backend_listing_close(const spudfile_report *report, void *native) {
	spudfile_windows_listing *listing = (spudfile_windows_listing *)native;
	if (listing->find != INVALID_HANDLE_VALUE && !FindClose(listing->find))
		spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
	SPUD_DELETE(&report->instance->head.allocator, spudfile_windows_listing, listing);
}

/* ---- Locking a file ---- */

/* The one byte every lock takes: offset 2^64 - 2, past any data a file can
 * hold (NTFS' largest file is 2^64 - 64 KiB), so a lock refuses other
 * locks and no read or write. */
static OVERLAPPED spudfile_windows_lock_byte(void) {
	OVERLAPPED position;
	ZeroMemory(&position, sizeof(position));
	position.Offset     = 0xFFFFFFFEu;
	position.OffsetHigh = 0xFFFFFFFFu;
	return position;
}

SPUDRESULT spudfile_backend_lock(const spudfile_report *report, intptr_t native, SPUDFILE_LOCK lock, bool wait) {
	DWORD flags = 0;
	if (lock == SPUDFILE_LOCK_EXCLUSIVE)
		flags |= LOCKFILE_EXCLUSIVE_LOCK;
	if (!wait)
		flags |= LOCKFILE_FAIL_IMMEDIATELY;
	OVERLAPPED position = spudfile_windows_lock_byte();
	if (LockFileEx((HANDLE)native, flags, 0, 1, 0, &position))
		return SPUD_SUCCESS;
	const DWORD error = GetLastError();
	if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING)
		return SPUDRESULT_SPUDFILE_LOCKED; // held elsewhere, not waiting: an answer
	return spudfile_windows_fail(report, error, SPUDRESULT_API_SPECIFIC_FAILURE);
}

SPUDRESULT spudfile_backend_unlock(const spudfile_report *report, intptr_t native) {
	OVERLAPPED position = spudfile_windows_lock_byte();
	if (!UnlockFileEx((HANDLE)native, 0, 1, 0, &position))
		return spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
	return SPUD_SUCCESS;
}

/* ---- Dynamic libraries ---- */

/* Windows binds a module's imports at load and keeps each module's exports
 * its own: NOW and LOCAL are its only behaviour. */
bool spudfile_backend_library_binding_supported(SPUDFILE_LIBRARY_BINDING binding) { return binding == SPUDFILE_LIBRARY_BINDING_NOW; }
bool spudfile_backend_library_scope_supported(SPUDFILE_LIBRARY_SCOPE scope) { return scope == SPUDFILE_LIBRARY_SCOPE_LOCAL; }
bool spudfile_backend_library_dependencies_supported(SPUDFILE_LIBRARY_DEPENDENCIES dependencies) {
	(void)dependencies;
	return true;
}

static bool spudfile_windows_separator(char c) { return c == '\\' || c == '/'; }

/* A drive letter, a colon and a separator, or two separators (a UNC or
 * device path): what LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR takes. A drive and a
 * name with no separator, or one separator first, is relative to a drive's
 * current directory or to the current drive. */
bool spudfile_backend_path_fully_qualified(const char *path) {
	const char letter = path[0];
	if (((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')) && path[1] == ':' && spudfile_windows_separator(path[2]))
		return true;
	return spudfile_windows_separator(path[0]) && spudfile_windows_separator(path[1]);
}

SPUDRESULT spudfile_backend_library_open(const spudfile_report *report, const spudfile_library_desc *desc, void **out_native) {
	spudfile_instance instance = report->instance;
	wchar_t *wide              = NULL;
	int count                  = 0;
	const SPUDRESULT converted = spudfile_windows_wide(instance, desc->path, &wide, &count);
	if (converted == SPUDRESULT_SPUDFILE_NOT_FOUND)
		return SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED; // not WTF-8: no file has that name
	if (converted != SPUD_SUCCESS)
		return converted;
	// No search flag is the standard search order; the library's own
	// directory first takes the two flags that make it so.
	const DWORD flags =
	    desc->dependencies == SPUDFILE_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS : 0;
	HMODULE module    = LoadLibraryExW(wide, NULL, flags);
	const DWORD error = module ? 0 : GetLastError();
	spudfile_windows_free_wide(instance, wide, count);
	if (!module)
		return spudfile_windows_fail(report, error, SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED);
	*out_native = (void *)module;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_library_find(const spudfile_report *report, void *native, const char *name, spudfile_library_function *out_function) {
	const FARPROC found = GetProcAddress((HMODULE)native, name);
	if (!found)
		return spudfile_windows_fail(report, GetLastError(), SPUDRESULT_SPUDFILE_SYMBOL_NOT_FOUND);
	// One function pointer type to another, which C allows.
	*out_function = (spudfile_library_function)found;
	return SPUD_SUCCESS;
}

void spudfile_backend_library_close(const spudfile_report *report, void *native) {
	if (!FreeLibrary((HMODULE)native))
		spudfile_windows_fail(report, GetLastError(), SPUDRESULT_API_SPECIFIC_FAILURE);
}

#endif // SPUDLIB_PLATFORM_WIN32
