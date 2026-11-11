#if CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE

#if CARROTTUS_PLATFORM_LINUX && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // pread, pwrite, the st_mtim member, renameat2
#endif

#include "../../crfilesshared.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h> // rename, renameat2, renamex_np
#include <string.h>
#include <sys/file.h> // flock
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The result an errno from open, stat, mkdir, unlink or rename stands for.
 * Whatever has no result of its own is the platform's failure. */
static CR_RESULT crfiles_posix_result(int error) {
	switch (error) {
	case ENOENT:
	case ENOTDIR:
		return CR_RESULT_FILES_NOT_FOUND;
	case EEXIST:
		return CR_RESULT_FILES_ALREADY_EXISTS;
	case EACCES:
	case EPERM:
	case EROFS:
		return CR_RESULT_FILES_ACCESS_DENIED;
	case EISDIR:
		return CR_RESULT_FILES_NOT_A_FILE;
	case EXDEV:
		return CR_RESULT_FILES_DIFFERENT_VOLUME;
	case ENOMEM:
		return CR_RESULT_OUT_OF_MEMORY;
	default:
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
}

/* Sends errno's [error] for [report]'s call, and returns [result]
 * (crcore.h, "Messages", M2). An errno carries no text of its own:
 * strerror reads it. */
static CR_RESULT crfiles_posix_fail(const crfiles_report *report, int error, CR_RESULT result) {
	crfiles_report_platform(report, result, CR_PLATFORM_ERROR_SOURCE_ERRNO, (int64_t)error, NULL);
	return result;
}

/* [error]'s result, sent. */
static CR_RESULT crfiles_posix_failed(const crfiles_report *report, int error) { return crfiles_posix_fail(report, error, crfiles_posix_result(error)); }

CR_RESULT crfiles_backend_open(const crfiles_report *report, const crfiles_open_desc *desc, intptr_t *out_native) {
	int flags = desc->access == CRFILES_ACCESS_READ ? O_RDONLY : desc->access == CRFILES_ACCESS_WRITE ? O_WRONLY : O_RDWR;
	switch (desc->create) {
	case CRFILES_CREATE_OPEN_EXISTING:
		break;
	case CRFILES_CREATE_NEW:
		flags |= O_CREAT | O_EXCL;
		break;
	case CRFILES_CREATE_OPEN_OR_CREATE:
		flags |= O_CREAT;
		break;
	case CRFILES_CREATE_TRUNCATE:
		flags |= O_CREAT | O_TRUNC;
		break;
	default:
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	// POSIX has no share modes: desc->share is the caller's statement of
	// what others may do, and nothing here can enforce it.
	flags |= O_CLOEXEC;
	int fd;
	do {
		fd = open(desc->path, flags, 0666);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0)
		return crfiles_posix_failed(report, errno);
	// open() takes a directory for reading; a file is what is wanted.
	struct stat st;
	if (fstat(fd, &st) != 0) {
		const int error = errno;
		close(fd);
		return crfiles_posix_failed(report, error);
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return CR_RESULT_FILES_NOT_A_FILE;
	}
	*out_native = (intptr_t)fd;
	return CR_SUCCESS;
}

void crfiles_backend_close(const crfiles_report *report, intptr_t native) {
	// Never retried, EINTR included: the descriptor is gone either way.
	if (close((int)native) != 0)
		crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
}

CR_RESULT crfiles_backend_read_at(const crfiles_report *report, intptr_t native, uint64_t offset, void *buffer, uint64_t size, uint64_t *out_read) {
	uint8_t *dst   = (uint8_t *)buffer;
	uint64_t total = 0;
	while (total < size) {
		const uint64_t remaining = size - total;
		const size_t chunk       = remaining > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)remaining;
		const ssize_t n          = pread((int)native, dst + total, chunk, (off_t)(offset + total));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			*out_read = total;
			return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
		}
		if (n == 0)
			break; // the end of the file
		total += (uint64_t)n;
	}
	*out_read = total;
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_write_at(const crfiles_report *report, intptr_t native, uint64_t offset, const void *data, uint64_t size) {
	const uint8_t *src = (const uint8_t *)data;
	uint64_t total     = 0;
	while (total < size) {
		const uint64_t remaining = size - total;
		const size_t chunk       = remaining > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)remaining;
		const ssize_t n          = pwrite((int)native, src + total, chunk, (off_t)(offset + total));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
		}
		total += (uint64_t)n;
	}
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_get_size(const crfiles_report *report, intptr_t native, uint64_t *out_size) {
	struct stat st;
	if (fstat((int)native, &st) != 0)
		return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	*out_size = (uint64_t)st.st_size;
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_set_size(const crfiles_report *report, intptr_t native, uint64_t size) {
	int r;
	do {
		r = ftruncate((int)native, (off_t)size);
	} while (r != 0 && errno == EINTR);
	if (r != 0)
		return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	return CR_SUCCESS;
}

/* Flushes [fd] to [level] (DEVICE or MEDIA). fsync only hands data to the
 * drive, which may keep it in a volatile cache; F_FULLFSYNC asks the drive
 * to write it to the media. Linux's fsync already flushes the drive cache. */
static CR_RESULT crfiles_posix_sync(const crfiles_report *report, int fd, CRFILES_FLUSH_LEVEL level) {
#if defined(F_FULLFSYNC)
	if (level == CRFILES_FLUSH_LEVEL_MEDIA) {
		if (fcntl(fd, F_FULLFSYNC) == 0)
			return CR_SUCCESS;
		const int error = errno;
		return crfiles_posix_fail(report, error,
		                           (error == ENOTSUP || error == EINVAL || error == ENOTTY) ? CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED
		                                                                                   : CR_RESULT_API_SPECIFIC_FAILURE);
	}
#else
	(void)level;
#endif
	int r;
	do {
		r = fsync(fd);
	} while (r != 0 && errno == EINTR);
	if (r != 0)
		return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_flush(const crfiles_report *report, intptr_t native, CRFILES_FLUSH_LEVEL level) {
	return crfiles_posix_sync(report, (int)native, level);
}

CR_RESULT crfiles_backend_map(const crfiles_report *report, intptr_t native, uint64_t offset, uint64_t size, void **out_base, uint64_t *out_base_size, uint64_t *out_lead) {
	// mmap offsets must be page-aligned: map from the page holding [offset].
	const uint64_t page    = (uint64_t)sysconf(_SC_PAGESIZE);
	const uint64_t aligned = offset - offset % page;
	const uint64_t lead    = offset - aligned;
	if (size > (uint64_t)SIZE_MAX - lead)
		return CR_RESULT_INDEX_OUT_OF_RANGE; // more than the address space
	void *base = mmap(NULL, (size_t)(lead + size), PROT_READ, MAP_SHARED, (int)native, (off_t)aligned);
	if (base == MAP_FAILED) {
		const int error = errno;
		return crfiles_posix_fail(report, error, error == ENOMEM ? CR_RESULT_OUT_OF_MEMORY : CR_RESULT_API_SPECIFIC_FAILURE);
	}
	*out_base      = base;
	*out_base_size = lead + size;
	*out_lead      = lead;
	return CR_SUCCESS;
}

void crfiles_backend_unmap(const crfiles_report *report, void *base, uint64_t base_size) {
	if (munmap(base, (size_t)base_size) != 0)
		crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
}

CR_RESULT crfiles_backend_path_get_info(const crfiles_report *report, const char *path, CRFILES_PATH_INFO *out_info) {
	struct stat st;
	if (stat(path, &st) != 0)
		return crfiles_posix_failed(report, errno);
	if (S_ISREG(st.st_mode)) {
		out_info->kind = CRFILES_PATH_KIND_FILE;
		out_info->size = (uint64_t)st.st_size;
	} else {
		out_info->kind = S_ISDIR(st.st_mode) ? CRFILES_PATH_KIND_DIRECTORY : CRFILES_PATH_KIND_OTHER;
		out_info->size = 0;
	}
#if CARROTTUS_PLATFORM_APPLE
	out_info->modified_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
	out_info->modified_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
	return CR_SUCCESS;
}

/* Makes a rename in [path]'s directory durable: the directory entry lives in
 * the directory, not the file, so it's the directory that's synced. */
static CR_RESULT crfiles_posix_flush_parent(const crfiles_report *report, const char *path, CRFILES_FLUSH_LEVEL level) {
	char dir[PATH_MAX];
	const size_t len = strlen(path);
	if (len >= sizeof(dir))
		return CR_RESULT_API_SPECIFIC_FAILURE;
	memcpy(dir, path, len + 1);
	char *slash = strrchr(dir, '/');
	if (!slash) {
		dir[0] = '.';
		dir[1] = '\0';
	} else if (slash == dir) {
		dir[1] = '\0'; // the root
	} else {
		*slash = '\0';
	}
	const int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	const CR_RESULT result = crfiles_posix_sync(report, fd, level);
	close(fd);
	return result;
}

CR_RESULT crfiles_backend_path_replace(const crfiles_report *report, const char *source, const char *target, CRFILES_FLUSH_LEVEL level) {
	if (rename(source, target) != 0) {
		const int error = errno;
		// A directory in the way of the rename is still not a file to
		// replace; anything else follows the common table.
		return crfiles_posix_fail(report, error, error == EISDIR ? CR_RESULT_FILES_NOT_A_FILE : crfiles_posix_result(error));
	}
	if (level != CRFILES_FLUSH_LEVEL_NONE)
		return crfiles_posix_flush_parent(report, target, level);
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_path_remove(const crfiles_report *report, const char *path) {
	if (unlink(path) == 0)
		return CR_SUCCESS;
	const int error = errno;
	// unlink of a directory is EISDIR on Linux and EPERM on Apple; EPERM is
	// also a real refusal, so it is told apart by what is there.
	if (error == EISDIR || error == EPERM) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			return crfiles_posix_fail(report, error, CR_RESULT_FILES_NOT_A_FILE);
	}
	return crfiles_posix_failed(report, error);
}

CR_RESULT crfiles_backend_path_copy_mode(const crfiles_report *report, const char *source, const char *target) {
	struct stat st;
	if (stat(source, &st) != 0)
		return crfiles_posix_failed(report, errno);
	// The nine permission bits only: what set-user-ID, set-group-ID and
	// sticky grant depends on an owner a copy doesn't carry.
	if (chmod(target, st.st_mode & 0777) == 0)
		return CR_SUCCESS;
	const int error = errno;
	// A read-only file system is the platform's refusal, not a permission.
	return crfiles_posix_fail(report, error, error == EROFS ? CR_RESULT_API_SPECIFIC_FAILURE : crfiles_posix_result(error));
}

CR_RESULT crfiles_backend_directory_create(const crfiles_report *report, const char *path) {
	if (mkdir(path, 0777) == 0)
		return CR_SUCCESS;
	return crfiles_posix_failed(report, errno);
}

CR_RESULT crfiles_backend_directory_remove(const crfiles_report *report, const char *path) {
	struct stat entry;
	if (lstat(path, &entry) != 0)
		return crfiles_posix_failed(report, errno);
	if (S_ISLNK(entry.st_mode)) {
		// rmdir of a link is ENOTDIR: a link to a directory is unlink's,
		// and goes itself; a link to anything else isn't a directory.
		struct stat target;
		if (stat(path, &target) != 0)
			return crfiles_posix_failed(report, errno);
		if (!S_ISDIR(target.st_mode))
			return CR_RESULT_FILES_NOT_A_DIRECTORY;
		return unlink(path) == 0 ? CR_SUCCESS : crfiles_posix_failed(report, errno);
	}
	if (!S_ISDIR(entry.st_mode))
		return CR_RESULT_FILES_NOT_A_DIRECTORY;
	if (rmdir(path) == 0)
		return CR_SUCCESS;
	const int error = errno;
	// A directory with entries is ENOTEMPTY, or EEXIST where POSIX allows it.
	if (error == ENOTEMPTY || error == EEXIST)
		return crfiles_posix_fail(report, error, CR_RESULT_FILES_NOT_EMPTY);
	return crfiles_posix_failed(report, error);
}

CR_RESULT crfiles_backend_directory_rename(const crfiles_report *report, const char *source, const char *target, CRFILES_FLUSH_LEVEL level) {
	// What source leads to must be a directory; a link to one is renamed
	// itself, as rename does.
	struct stat st;
	if (stat(source, &st) != 0)
		return crfiles_posix_failed(report, errno);
	if (!S_ISDIR(st.st_mode))
		return CR_RESULT_FILES_NOT_A_DIRECTORY;
	// rename replaces an empty directory; nothing may be there, so the
	// platform's own rename that refuses any target is used (EEXIST).
	int r;
#if CARROTTUS_PLATFORM_APPLE
	r = renamex_np(source, target, RENAME_EXCL);
#else
	r = renameat2(AT_FDCWD, source, AT_FDCWD, target, RENAME_NOREPLACE);
#endif
	if (r != 0 && (errno == EINVAL || errno == ENOTSUP || errno == ENOSYS)) {
		// A file system without it (some network ones): the same rule,
		// checked first, which a target made in between can slip past.
		if (lstat(target, &st) == 0)
			return CR_RESULT_FILES_ALREADY_EXISTS;
		if (errno != ENOENT)
			return crfiles_posix_failed(report, errno);
		r = rename(source, target);
	}
	if (r != 0) {
		const int error = errno;
		if (error == ENOTEMPTY || error == EEXIST)
			return crfiles_posix_fail(report, error, CR_RESULT_FILES_ALREADY_EXISTS);
		return crfiles_posix_failed(report, error);
	}
	if (level != CRFILES_FLUSH_LEVEL_NONE)
		return crfiles_posix_flush_parent(report, target, level);
	return CR_SUCCESS;
}

/* ---- Listing a directory ---- */

/* A listing's state: the platform's stream over the directory. */
typedef struct crfiles_posix_listing {
	DIR *dir;
} crfiles_posix_listing;

CR_RESULT crfiles_backend_listing_open(const crfiles_report *report, const char *path, void **out_native) {
	crfiles_instance instance      = report->instance;
	crfiles_posix_listing *listing = CR_NEW_ZEROED(&instance->head.allocator, crfiles_posix_listing);
	if (!listing)
		return CR_RESULT_OUT_OF_MEMORY;
	listing->dir = opendir(path);
	if (!listing->dir) {
		const int error = errno;
		CR_DELETE(&instance->head.allocator, crfiles_posix_listing, listing);
		// ENOTDIR is both "a file is there" and "a file is on the way to
		// it": what is at the path tells them apart.
		if (error == ENOTDIR) {
			struct stat status;
			if (stat(path, &status) == 0 && !S_ISDIR(status.st_mode))
				return crfiles_posix_fail(report, error, CR_RESULT_FILES_NOT_A_DIRECTORY);
			return crfiles_posix_fail(report, error, CR_RESULT_FILES_NOT_FOUND);
		}
		return crfiles_posix_failed(report, error);
	}
	*out_native = listing;
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_listing_next(const crfiles_report *report, void *native, CRFILES_LISTING_ENTRY *out_entry) {
	crfiles_posix_listing *listing = (crfiles_posix_listing *)native;
	for (;;) {
		errno                       = 0;
		const struct dirent *found = readdir(listing->dir);
		if (!found) {
			const int error = errno;
			if (error == 0)
				return CR_RESULT_FILES_END_OF_LISTING; // an answer, not a failure
			return crfiles_posix_fail(report, error, CR_RESULT_API_SPECIFIC_FAILURE);
		}
		const char *name = found->d_name;
		if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
			continue;
		CRFILES_ENTRY_KIND kind = CRFILES_ENTRY_KIND_OTHER;
		switch (found->d_type) {
		case DT_REG:
			kind = CRFILES_ENTRY_KIND_FILE;
			break;
		case DT_DIR:
			kind = CRFILES_ENTRY_KIND_DIRECTORY;
			break;
		case DT_LNK:
			kind = CRFILES_ENTRY_KIND_SYMBOLIC_LINK;
			break;
		case DT_UNKNOWN: {
			// The file system gives no kind: ask it, without following a
			// link. An entry removed before it could be asked is skipped.
			struct stat status;
			if (fstatat(dirfd(listing->dir), name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
				if (errno == ENOENT)
					continue;
				return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
			}
			if (S_ISREG(status.st_mode))
				kind = CRFILES_ENTRY_KIND_FILE;
			else if (S_ISDIR(status.st_mode))
				kind = CRFILES_ENTRY_KIND_DIRECTORY;
			else if (S_ISLNK(status.st_mode))
				kind = CRFILES_ENTRY_KIND_SYMBOLIC_LINK;
			break;
		}
		default:
			break; // a device, socket or pipe
		}
		const size_t size = strlen(name);
		if (size >= CRFILES_NAME_CAPACITY)
			return CR_RESULT_API_SPECIFIC_FAILURE; // NAME_MAX is 255 everywhere Carrottus Files runs
		memcpy(out_entry->name, name, size + 1);
		out_entry->kind         = kind;
		out_entry->name_size    = (uint32_t)size;
		out_entry->name_is_utf8 = crfiles_utf8_valid(name, (uint32_t)size);
		return CR_SUCCESS;
	}
}

void crfiles_backend_listing_close(const crfiles_report *report, void *native) {
	crfiles_posix_listing *listing = (crfiles_posix_listing *)native;
	if (closedir(listing->dir) != 0)
		crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	CR_DELETE(&report->instance->head.allocator, crfiles_posix_listing, listing);
}

/* ---- Locking a file ---- */

CR_RESULT crfiles_backend_lock(const crfiles_report *report, intptr_t native, CRFILES_LOCK lock, bool wait) {
	int operation = lock == CRFILES_LOCK_EXCLUSIVE ? LOCK_EX : LOCK_SH;
	if (!wait)
		operation |= LOCK_NB;
	for (;;) {
		if (flock((int)native, operation) == 0)
			return CR_SUCCESS;
		if (errno == EINTR)
			continue; // a signal while waiting: still waiting
		if (errno == EWOULDBLOCK)
			return CR_RESULT_FILES_LOCKED; // held elsewhere, not waiting: an answer
		return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	}
}

CR_RESULT crfiles_backend_unlock(const crfiles_report *report, intptr_t native) {
	if (flock((int)native, LOCK_UN) != 0)
		return crfiles_posix_fail(report, errno, CR_RESULT_API_SPECIFIC_FAILURE);
	return CR_SUCCESS;
}

/* ---- Dynamic libraries ---- */

/* dlopen has both bindings and both scopes; where dependencies are found
 * the library chose when it was linked ($ORIGIN, @loader_path). */
bool crfiles_backend_library_binding_supported(CRFILES_LIBRARY_BINDING binding) {
	(void)binding;
	return true;
}
bool crfiles_backend_library_scope_supported(CRFILES_LIBRARY_SCOPE scope) {
	(void)scope;
	return true;
}
bool crfiles_backend_library_dependencies_supported(CRFILES_LIBRARY_DEPENDENCIES dependencies) {
	return dependencies == CRFILES_LIBRARY_DEPENDENCIES_PLATFORM_ORDER;
}
bool crfiles_backend_path_fully_qualified(const char *path) { return path[0] == '/'; }

/* Sends the loader's words for [report]'s call: dlerror's text, which has
 * no number. dlerror is the calling thread's, on glibc and on Apple, and is
 * read right after the call that failed. */
static CR_RESULT crfiles_posix_loader_fail(const crfiles_report *report, CR_RESULT result) {
	crfiles_report_platform(report, result, CR_PLATFORM_ERROR_SOURCE_DLERROR, 0, dlerror());
	return result;
}

CR_RESULT crfiles_backend_library_open(const crfiles_report *report, const crfiles_library_desc *desc, void **out_native) {
	int flags = desc->binding == CRFILES_LIBRARY_BINDING_NOW ? RTLD_NOW : RTLD_LAZY;
	flags |= desc->scope == CRFILES_LIBRARY_SCOPE_LOCAL ? RTLD_LOCAL : RTLD_GLOBAL;
	(void)dlerror(); // an earlier call's message is not this one's
	void *handle = dlopen(desc->path, flags);
	if (!handle)
		return crfiles_posix_loader_fail(report, CR_RESULT_FILES_LIBRARY_NOT_LOADED);
	*out_native = handle;
	return CR_SUCCESS;
}

CR_RESULT crfiles_backend_library_find(const crfiles_report *report, void *native, const char *name, crfiles_library_function *out_function) {
	(void)dlerror();
	void *found = dlsym(native, name);
	// A function's address is never NULL: NULL is a symbol not found (or
	// one whose value is NULL, which is no function).
	if (!found)
		return crfiles_posix_loader_fail(report, CR_RESULT_FILES_SYMBOL_NOT_FOUND);
	// POSIX makes dlsym's object pointer a function's; copied, not cast,
	// as ISO C has no conversion between the two.
	_Static_assert(sizeof(crfiles_library_function) == sizeof(void *), "a function pointer is an object pointer's size");
	crfiles_library_function function;
	memcpy(&function, &found, sizeof(function));
	*out_function = function;
	return CR_SUCCESS;
}

void crfiles_backend_library_close(const crfiles_report *report, void *native) {
	(void)dlerror();
	if (dlclose(native) != 0)
		crfiles_posix_loader_fail(report, CR_RESULT_API_SPECIFIC_FAILURE);
}

#endif // CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE
