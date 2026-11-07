#if SPUDLIB_PLATFORM_LINUX || SPUDLIB_PLATFORM_APPLE

#if SPUDLIB_PLATFORM_LINUX && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // pread, pwrite, the st_mtim member, renameat2
#endif

#include "../../spudfilesshared.h"

#include <dirent.h>
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
static SPUDRESULT spudfile_posix_result(int error) {
	switch (error) {
	case ENOENT:
	case ENOTDIR:
		return SPUDRESULT_SPUDFILE_NOT_FOUND;
	case EEXIST:
		return SPUDRESULT_SPUDFILE_ALREADY_EXISTS;
	case EACCES:
	case EPERM:
	case EROFS:
		return SPUDRESULT_SPUDFILE_ACCESS_DENIED;
	case EISDIR:
		return SPUDRESULT_SPUDFILE_NOT_A_FILE;
	case EXDEV:
		return SPUDRESULT_SPUDFILE_DIFFERENT_VOLUME;
	case ENOMEM:
		return SPUDRESULT_OUT_OF_MEMORY;
	default:
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
}

SPUDRESULT spudfile_backend_open(spudfile_instance instance, const spudfile_open_desc *desc, intptr_t *out_native) {
	(void)instance;
	int flags = desc->access == SPUDFILE_ACCESS_READ ? O_RDONLY : desc->access == SPUDFILE_ACCESS_WRITE ? O_WRONLY : O_RDWR;
	switch (desc->create) {
	case SPUDFILE_CREATE_OPEN_EXISTING:
		break;
	case SPUDFILE_CREATE_NEW:
		flags |= O_CREAT | O_EXCL;
		break;
	case SPUDFILE_CREATE_OPEN_OR_CREATE:
		flags |= O_CREAT;
		break;
	case SPUDFILE_CREATE_TRUNCATE:
		flags |= O_CREAT | O_TRUNC;
		break;
	default:
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	}
	// POSIX has no share modes: desc->share is the caller's statement of
	// what others may do, and nothing here can enforce it.
	flags |= O_CLOEXEC;
	int fd;
	do {
		fd = open(desc->path, flags, 0666);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0)
		return spudfile_posix_result(errno);
	// open() takes a directory for reading; a file is what is wanted.
	struct stat st;
	if (fstat(fd, &st) != 0) {
		const int error = errno;
		close(fd);
		return spudfile_posix_result(error);
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return SPUDRESULT_SPUDFILE_NOT_A_FILE;
	}
	*out_native = (intptr_t)fd;
	return SPUD_SUCCESS;
}

void spudfile_backend_close(intptr_t native) { close((int)native); }

SPUDRESULT spudfile_backend_read_at(intptr_t native, uint64_t offset, void *buffer, uint64_t size, uint64_t *out_read) {
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
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}
		if (n == 0)
			break; // the end of the file
		total += (uint64_t)n;
	}
	*out_read = total;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_write_at(intptr_t native, uint64_t offset, const void *data, uint64_t size) {
	const uint8_t *src = (const uint8_t *)data;
	uint64_t total     = 0;
	while (total < size) {
		const uint64_t remaining = size - total;
		const size_t chunk       = remaining > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)remaining;
		const ssize_t n          = pwrite((int)native, src + total, chunk, (off_t)(offset + total));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}
		total += (uint64_t)n;
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_get_size(intptr_t native, uint64_t *out_size) {
	struct stat st;
	if (fstat((int)native, &st) != 0)
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	*out_size = (uint64_t)st.st_size;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_set_size(intptr_t native, uint64_t size) {
	int r;
	do {
		r = ftruncate((int)native, (off_t)size);
	} while (r != 0 && errno == EINTR);
	return r == 0 ? SPUD_SUCCESS : SPUDRESULT_API_SPECIFIC_FAILURE;
}

/* Flushes [fd] to [level] (DEVICE or MEDIA). fsync only hands data to the
 * drive, which may keep it in a volatile cache; F_FULLFSYNC asks the drive
 * to write it to the media. Linux's fsync already flushes the drive cache. */
static SPUDRESULT spudfile_posix_sync(int fd, SPUDFILE_FLUSH_LEVEL level) {
#if defined(F_FULLFSYNC)
	if (level == SPUDFILE_FLUSH_LEVEL_MEDIA) {
		if (fcntl(fd, F_FULLFSYNC) == 0)
			return SPUD_SUCCESS;
		return (errno == ENOTSUP || errno == EINVAL || errno == ENOTTY) ? SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED
		                                                               : SPUDRESULT_API_SPECIFIC_FAILURE;
	}
#else
	(void)level;
#endif
	int r;
	do {
		r = fsync(fd);
	} while (r != 0 && errno == EINTR);
	return r == 0 ? SPUD_SUCCESS : SPUDRESULT_API_SPECIFIC_FAILURE;
}

SPUDRESULT spudfile_backend_flush(intptr_t native, SPUDFILE_FLUSH_LEVEL level) { return spudfile_posix_sync((int)native, level); }

SPUDRESULT spudfile_backend_map(intptr_t native, uint64_t offset, uint64_t size, void **out_base, uint64_t *out_base_size, uint64_t *out_lead) {
	// mmap offsets must be page-aligned: map from the page holding [offset].
	const uint64_t page    = (uint64_t)sysconf(_SC_PAGESIZE);
	const uint64_t aligned = offset - offset % page;
	const uint64_t lead    = offset - aligned;
	if (size > (uint64_t)SIZE_MAX - lead)
		return SPUDRESULT_INDEX_OUT_OF_RANGE; // more than the address space
	void *base = mmap(NULL, (size_t)(lead + size), PROT_READ, MAP_SHARED, (int)native, (off_t)aligned);
	if (base == MAP_FAILED)
		return errno == ENOMEM ? SPUDRESULT_OUT_OF_MEMORY : SPUDRESULT_API_SPECIFIC_FAILURE;
	*out_base      = base;
	*out_base_size = lead + size;
	*out_lead      = lead;
	return SPUD_SUCCESS;
}

void spudfile_backend_unmap(void *base, uint64_t base_size) { munmap(base, (size_t)base_size); }

SPUDRESULT spudfile_backend_path_get_info(spudfile_instance instance, const char *path, SPUDFILE_PATH_INFO *out_info) {
	(void)instance;
	struct stat st;
	if (stat(path, &st) != 0)
		return spudfile_posix_result(errno);
	if (S_ISREG(st.st_mode)) {
		out_info->kind = SPUDFILE_PATH_KIND_FILE;
		out_info->size = (uint64_t)st.st_size;
	} else {
		out_info->kind = S_ISDIR(st.st_mode) ? SPUDFILE_PATH_KIND_DIRECTORY : SPUDFILE_PATH_KIND_OTHER;
		out_info->size = 0;
	}
#if SPUDLIB_PLATFORM_APPLE
	out_info->modified_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
	out_info->modified_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
	return SPUD_SUCCESS;
}

/* Makes a rename in [path]'s directory durable: the directory entry lives in
 * the directory, not the file, so it's the directory that's synced. */
static SPUDRESULT spudfile_posix_flush_parent(const char *path, SPUDFILE_FLUSH_LEVEL level) {
	char dir[PATH_MAX];
	const size_t len = strlen(path);
	if (len >= sizeof(dir))
		return SPUDRESULT_API_SPECIFIC_FAILURE;
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
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	const SPUDRESULT result = spudfile_posix_sync(fd, level);
	close(fd);
	return result;
}

SPUDRESULT spudfile_backend_path_replace(spudfile_instance instance, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level) {
	(void)instance;
	if (rename(source, target) != 0) {
		const int error = errno;
		// A directory in the way of the rename is still not a file to
		// replace; anything else follows the common table.
		return error == EISDIR ? SPUDRESULT_SPUDFILE_NOT_A_FILE : spudfile_posix_result(error);
	}
	if (level != SPUDFILE_FLUSH_LEVEL_NONE)
		return spudfile_posix_flush_parent(target, level);
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_path_remove(spudfile_instance instance, const char *path) {
	(void)instance;
	if (unlink(path) == 0)
		return SPUD_SUCCESS;
	const int error = errno;
	// unlink of a directory is EISDIR on Linux and EPERM on Apple; EPERM is
	// also a real refusal, so it is told apart by what is there.
	if (error == EISDIR || error == EPERM) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			return SPUDRESULT_SPUDFILE_NOT_A_FILE;
	}
	return spudfile_posix_result(error);
}

SPUDRESULT spudfile_backend_path_copy_mode(spudfile_instance instance, const char *source, const char *target) {
	(void)instance;
	struct stat st;
	if (stat(source, &st) != 0)
		return spudfile_posix_result(errno);
	// The nine permission bits only: what set-user-ID, set-group-ID and
	// sticky grant depends on an owner a copy doesn't carry.
	if (chmod(target, st.st_mode & 0777) == 0)
		return SPUD_SUCCESS;
	const int error = errno;
	// A read-only file system is the platform's refusal, not a permission.
	return error == EROFS ? SPUDRESULT_API_SPECIFIC_FAILURE : spudfile_posix_result(error);
}

SPUDRESULT spudfile_backend_directory_create(spudfile_instance instance, const char *path) {
	(void)instance;
	if (mkdir(path, 0777) == 0)
		return SPUD_SUCCESS;
	return spudfile_posix_result(errno);
}

SPUDRESULT spudfile_backend_directory_remove(spudfile_instance instance, const char *path) {
	(void)instance;
	struct stat entry;
	if (lstat(path, &entry) != 0)
		return spudfile_posix_result(errno);
	if (S_ISLNK(entry.st_mode)) {
		// rmdir of a link is ENOTDIR: a link to a directory is unlink's,
		// and goes itself; a link to anything else isn't a directory.
		struct stat target;
		if (stat(path, &target) != 0)
			return spudfile_posix_result(errno);
		if (!S_ISDIR(target.st_mode))
			return SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
		return unlink(path) == 0 ? SPUD_SUCCESS : spudfile_posix_result(errno);
	}
	if (!S_ISDIR(entry.st_mode))
		return SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
	if (rmdir(path) == 0)
		return SPUD_SUCCESS;
	const int error = errno;
	// A directory with entries is ENOTEMPTY, or EEXIST where POSIX allows it.
	if (error == ENOTEMPTY || error == EEXIST)
		return SPUDRESULT_SPUDFILE_NOT_EMPTY;
	return spudfile_posix_result(error);
}

SPUDRESULT spudfile_backend_directory_rename(spudfile_instance instance, const char *source, const char *target, SPUDFILE_FLUSH_LEVEL level) {
	(void)instance;
	// What source leads to must be a directory; a link to one is renamed
	// itself, as rename does.
	struct stat st;
	if (stat(source, &st) != 0)
		return spudfile_posix_result(errno);
	if (!S_ISDIR(st.st_mode))
		return SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
	// rename replaces an empty directory; nothing may be there, so the
	// platform's own rename that refuses any target is used (EEXIST).
	int r;
#if SPUDLIB_PLATFORM_APPLE
	r = renamex_np(source, target, RENAME_EXCL);
#else
	r = renameat2(AT_FDCWD, source, AT_FDCWD, target, RENAME_NOREPLACE);
#endif
	if (r != 0 && (errno == EINVAL || errno == ENOTSUP || errno == ENOSYS)) {
		// A file system without it (some network ones): the same rule,
		// checked first, which a target made in between can slip past.
		if (lstat(target, &st) == 0)
			return SPUDRESULT_SPUDFILE_ALREADY_EXISTS;
		if (errno != ENOENT)
			return spudfile_posix_result(errno);
		r = rename(source, target);
	}
	if (r != 0) {
		const int error = errno;
		if (error == ENOTEMPTY || error == EEXIST)
			return SPUDRESULT_SPUDFILE_ALREADY_EXISTS;
		return spudfile_posix_result(error);
	}
	if (level != SPUDFILE_FLUSH_LEVEL_NONE)
		return spudfile_posix_flush_parent(target, level);
	return SPUD_SUCCESS;
}

/* ---- Listing a directory ---- */

/* A listing's state: the platform's stream over the directory. */
typedef struct spudfile_posix_listing {
	DIR *dir;
} spudfile_posix_listing;

SPUDRESULT spudfile_backend_listing_open(spudfile_instance instance, const char *path, void **out_native) {
	spudfile_posix_listing *listing = SPUD_NEW_ZEROED(&instance->head.allocator, spudfile_posix_listing);
	if (!listing)
		return SPUDRESULT_OUT_OF_MEMORY;
	listing->dir = opendir(path);
	if (!listing->dir) {
		const int error = errno;
		SPUD_DELETE(&instance->head.allocator, spudfile_posix_listing, listing);
		// ENOTDIR is both "a file is there" and "a file is on the way to
		// it": what is at the path tells them apart.
		if (error == ENOTDIR) {
			struct stat status;
			if (stat(path, &status) == 0 && !S_ISDIR(status.st_mode))
				return SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY;
			return SPUDRESULT_SPUDFILE_NOT_FOUND;
		}
		return spudfile_posix_result(error);
	}
	*out_native = listing;
	return SPUD_SUCCESS;
}

SPUDRESULT spudfile_backend_listing_next(spudfile_instance instance, void *native, SPUDFILE_LISTING_ENTRY *out_entry) {
	(void)instance;
	spudfile_posix_listing *listing = (spudfile_posix_listing *)native;
	for (;;) {
		errno                       = 0;
		const struct dirent *found = readdir(listing->dir);
		if (!found)
			return errno == 0 ? SPUDRESULT_SPUDFILE_END_OF_LISTING : SPUDRESULT_API_SPECIFIC_FAILURE;
		const char *name = found->d_name;
		if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
			continue;
		SPUDFILE_ENTRY_KIND kind = SPUDFILE_ENTRY_KIND_OTHER;
		switch (found->d_type) {
		case DT_REG:
			kind = SPUDFILE_ENTRY_KIND_FILE;
			break;
		case DT_DIR:
			kind = SPUDFILE_ENTRY_KIND_DIRECTORY;
			break;
		case DT_LNK:
			kind = SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK;
			break;
		case DT_UNKNOWN: {
			// The file system gives no kind: ask it, without following a
			// link. An entry removed before it could be asked is skipped.
			struct stat status;
			if (fstatat(dirfd(listing->dir), name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
				if (errno == ENOENT)
					continue;
				return SPUDRESULT_API_SPECIFIC_FAILURE;
			}
			if (S_ISREG(status.st_mode))
				kind = SPUDFILE_ENTRY_KIND_FILE;
			else if (S_ISDIR(status.st_mode))
				kind = SPUDFILE_ENTRY_KIND_DIRECTORY;
			else if (S_ISLNK(status.st_mode))
				kind = SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK;
			break;
		}
		default:
			break; // a device, socket or pipe
		}
		const size_t size = strlen(name);
		if (size >= SPUDFILE_NAME_CAPACITY)
			return SPUDRESULT_API_SPECIFIC_FAILURE; // NAME_MAX is 255 everywhere SpudFiles runs
		memcpy(out_entry->name, name, size + 1);
		out_entry->kind         = kind;
		out_entry->name_size    = (uint32_t)size;
		out_entry->name_is_utf8 = spudfile_utf8_valid(name, (uint32_t)size);
		return SPUD_SUCCESS;
	}
}

void spudfile_backend_listing_close(spudfile_instance instance, void *native) {
	spudfile_posix_listing *listing = (spudfile_posix_listing *)native;
	closedir(listing->dir);
	SPUD_DELETE(&instance->head.allocator, spudfile_posix_listing, listing);
}

/* ---- Locking a file ---- */

SPUDRESULT spudfile_backend_lock(intptr_t native, SPUDFILE_LOCK lock, bool wait) {
	int operation = lock == SPUDFILE_LOCK_EXCLUSIVE ? LOCK_EX : LOCK_SH;
	if (!wait)
		operation |= LOCK_NB;
	for (;;) {
		if (flock((int)native, operation) == 0)
			return SPUD_SUCCESS;
		if (errno == EINTR)
			continue; // a signal while waiting: still waiting
		if (errno == EWOULDBLOCK)
			return SPUDRESULT_SPUDFILE_LOCKED;
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
}

SPUDRESULT spudfile_backend_unlock(intptr_t native) { return flock((int)native, LOCK_UN) == 0 ? SPUD_SUCCESS : SPUDRESULT_API_SPECIFIC_FAILURE; }

#endif // SPUDLIB_PLATFORM_LINUX || SPUDLIB_PLATFORM_APPLE
