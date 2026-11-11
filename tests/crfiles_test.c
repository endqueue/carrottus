/*
 * crfiles_test — Carrottus Files, in a temporary directory.
 *
 * Every call and its argument checks: the instance; opening with each
 * access and creation choice and what each refuses; reading and writing at
 * an offset, a short read at the end, a write past it; sizing; flushing;
 * mapping at an offset no page boundary falls on, outliving its file; what
 * is at a path; replacing, removing and making a directory; a file with a
 * name outside ASCII; two threads reading one file at once; listing a
 * directory (each kind of entry the platform lets the test make, names
 * that aren't UTF-8); removing a directory (empty, full, a link to one)
 * and renaming one (to a free name at each level, onto each kind of thing
 * in the way), and a tree written beside its place, renamed in and removed;
 * a file's permissions copied to another (Windows' read-only attribute,
 * POSIX's nine bits whatever the umask, never the set-ID bits; links
 * followed; directories); and locking a file between two files opened on one path, one waiting on
 * a second thread; the instance's messages for platform failures (each
 * with its number, none for a refused argument, from two threads at once,
 * nothing allocated to send one); and dynamic libraries opened, searched
 * and closed (card S44). Every block
 * goes through a counting allocator, checked at the end. Needs nothing but
 * a writable temporary directory.
 *
 * Exits 0 when every check passes, 1 when one fails, 77 (skipped) with no
 * temporary directory.
 */

#include <crfiles.h>

#include "crtest_allocator.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CARROTTUS_PLATFORM_WIN32
#include <Windows.h>
#include <direct.h>
#include <process.h>
#define rmdir _rmdir
#define SLASH "\\"
#else
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#define SLASH "/"
#endif

#define EXIT_SKIPPED 77

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

#define CHECK_RESULT(call, expected)                                                                                         \
	do {                                                                                                                     \
		const CR_RESULT r_ = (call);                                                                                        \
		if (r_ != (expected)) {                                                                                              \
			fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, #call, cr_result_str(r_), #expected); \
			++failures;                                                                                                      \
		}                                                                                                                    \
	} while (0)

static char g_dir[1024];
static crfiles_instance g_files;

static const char *path_in(
    char *buf,
    size_t cap,
    const char *name) {
	snprintf(buf, cap, "%s" SLASH "%s", g_dir, name);
	return buf;
}

static crfiles_open_desc open_desc(
    const char *path,
    CRFILES_ACCESS access,
    CRFILES_CREATE create) {
	crfiles_open_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_FILES_OPEN_DESC;
	desc.path      = path;
	desc.access    = access;
	desc.create    = create;
	desc.share     = CRFILES_SHARE_READ | CRFILES_SHARE_WRITE | CRFILES_SHARE_DELETE;
	return desc;
}

static crfiles_file open_file(
    const char *path,
    CRFILES_ACCESS access,
    CRFILES_CREATE create) {
	const crfiles_open_desc desc = open_desc(path, access, create);
	crfiles_file file            = NULL;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_SUCCESS);
	return file;
}

static void write_whole(
    const char *path,
    const char *text) {
	crfiles_file file = open_file(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
	if (!file)
		return;
	CHECK_RESULT(crfiles_file_write_at(file, 0, text, strlen(text)), CR_SUCCESS);
	crfiles_file_close(file);
}

static int holds(
    const char *path,
    const char *text) {
	crfiles_file file = open_file(path, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	if (!file)
		return 0;
	char buf[256]  = {0};
	uint64_t count = 0;
	CHECK_RESULT(crfiles_file_read_at(file, 0, buf, sizeof(buf) - 1, &count), CR_SUCCESS);
	crfiles_file_close(file);
	return count == strlen(text) && memcmp(buf, text, (size_t)count) == 0;
}

static CRFILES_PATH_INFO info_of(
    const char *path,
    CR_RESULT *out_result) {
	CRFILES_PATH_INFO info;
	memset(&info, 0, sizeof(info));
	info.base.type = CR_STRUCT_TYPE_FILES_PATH_INFO;
	*out_result    = crfiles_path_get_info(g_files, path, &info);
	return info;
}

static void test_instance(const CR_ALLOCATOR *allocator) {
	crfiles_instance instance = (crfiles_instance)(uintptr_t)1;
	crfiles_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	CHECK_RESULT(crfiles_instance_create(NULL, &instance), CR_RESULT_NULL_DESC);
	CHECK_RESULT(crfiles_instance_create(&desc, &instance), CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.base.next = (const CR_STRUCT_BASE *)&desc;
	CHECK_RESULT(crfiles_instance_create(&desc, &instance), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;
	CHECK_RESULT(crfiles_instance_create(&desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(crfiles_instance_create(&desc, &instance), CR_RESULT_NULL_ALLOCATOR);
	CHECK(instance == NULL);
	CR_ALLOCATOR missing = *allocator;
	missing.allocate       = NULL;
	desc.allocator         = &missing;
	CHECK_RESULT(crfiles_instance_create(&desc, &instance), CR_RESULT_INVALID_ALLOCATOR);
	crfiles_instance_destroy(NULL);
}

/* Card X27 (carrottus's src/crstale.h): in a Debug build a closed file's
 * memory stays its instance's until the instance is destroyed, so the
 * destroyed-handle check reads CR_DEAD however many blocks of the same
 * size are allocated after it, and destroying the instance frees every one.
 * Its own instance and counting allocator, to count exactly what the
 * instance holds. Debug only: Release has no check, and frees at once. */
static void test_quarantine(void) {
#if defined(_DEBUG) && _DEBUG
	enum { ROUNDS = 256, KEPT_OPEN = 64 };
	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
	crfiles_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator = &allocator;
	crfiles_instance instance = NULL;
	CHECK_RESULT(crfiles_instance_create(&desc, &instance), CR_SUCCESS);
	if (!instance)
		return;
	const size_t instance_blocks = tracker.live_blocks;

	char path[1100];
	path_in(path, sizeof(path), "quarantine");
	crfiles_open_desc open = open_desc(path, CRFILES_ACCESS_READ_WRITE, CRFILES_CREATE_TRUNCATE);
	crfiles_file dead      = NULL;
	CHECK_RESULT(crfiles_file_open(instance, &open, &dead), CR_SUCCESS);
	if (!dead) {
		crfiles_instance_destroy(instance);
		return;
	}
	crfiles_file_close(dead);
	CHECK(tracker.live_blocks == instance_blocks + 1);

	/* Blocks of the same size, made and freed again and again: an allocator
	 * that reused the closed file's block would hand it to one of these. */
	open.create = CRFILES_CREATE_OPEN_EXISTING;
	for (int i = 0; i < ROUNDS; ++i) {
		crfiles_file file = NULL;
		CHECK_RESULT(crfiles_file_open(instance, &open, &file), CR_SUCCESS);
		CHECK(file != dead);
		crfiles_file_close(file);
	}
	crfiles_file open_files[KEPT_OPEN];
	for (int i = 0; i < KEPT_OPEN; ++i) {
		open_files[i] = NULL;
		CHECK_RESULT(crfiles_file_open(instance, &open, &open_files[i]), CR_SUCCESS);
		CHECK(open_files[i] != dead);
	}
	uint64_t size = 0;
	CHECK_RESULT(crfiles_file_get_size(dead, &size), CR_RESULT_FILES_INVALID_FILE);
	crfiles_file_close(dead);
	for (int i = 0; i < KEPT_OPEN; ++i)
		crfiles_file_close(open_files[i]);

	/* Every closed file is still the instance's, then none is. */
	CHECK(tracker.live_blocks == instance_blocks + 1 + ROUNDS + KEPT_OPEN);
	crfiles_instance_destroy(instance);
	CHECK(crtest_allocator_clean(&tracker));
	CHECK(remove(path) == 0);
#endif
}

static void test_open_checks(void) {
	char path[1100];
	path_in(path, sizeof(path), "checks.txt");
	crfiles_file file      = (crfiles_file)(uintptr_t)1;
	crfiles_open_desc desc = open_desc(path, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);

	CHECK_RESULT(crfiles_file_open(NULL, &desc, &file), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_file_open(g_files, NULL, &file), CR_RESULT_NULL_DESC);
	desc.base.type = CR_STRUCT_TYPE_FILES_PATH_INFO;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_FILES_OPEN_DESC;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	desc.path = NULL;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_NULL_PATH);
	CHECK(file == NULL);
	desc.path = "";
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_NULL_PATH);
	desc.path   = path;
	desc.access = CRFILES_ACCESS_NONE;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.access = (CRFILES_ACCESS)9;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.access = CRFILES_ACCESS_READ;
	desc.create = CRFILES_CREATE_NONE;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.create = (CRFILES_CREATE)9;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.create = CRFILES_CREATE_OPEN_EXISTING;
	desc.share  = 0x80;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.share  = CRFILES_SHARE_NONE;
	desc.create = CRFILES_CREATE_TRUNCATE; // emptying a file opened only to read
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_DESC_INVALID_PARAMETERS);

	// Nothing there yet.
	desc.create = CRFILES_CREATE_OPEN_EXISTING;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_NOT_FOUND);
	char deeper[1200];
	snprintf(deeper, sizeof(deeper), "%s" SLASH "missing" SLASH "x.txt", g_dir);
	desc.path   = deeper;
	desc.access = CRFILES_ACCESS_WRITE;
	desc.create = CRFILES_CREATE_OPEN_OR_CREATE;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_NOT_FOUND);
	// A directory isn't a file.
	desc.path   = g_dir;
	desc.access = CRFILES_ACCESS_READ;
	desc.create = CRFILES_CREATE_OPEN_EXISTING;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_NOT_A_FILE);
	CHECK(file == NULL);
	crfiles_file_close(NULL);
}

static void test_create_modes(void) {
	char path[1100];
	path_in(path, sizeof(path), "modes.txt");

	// NEW makes it, and refuses a second time.
	crfiles_file file = open_file(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_NEW);
	if (file) {
		CHECK_RESULT(crfiles_file_write_at(file, 0, "first", 5), CR_SUCCESS);
		crfiles_file_close(file);
	}
	crfiles_open_desc desc = open_desc(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_NEW);
	file               = NULL;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_ALREADY_EXISTS);
	CHECK(holds(path, "first"));

	// OPEN_OR_CREATE keeps what's there.
	file = open_file(path, CRFILES_ACCESS_READ_WRITE, CRFILES_CREATE_OPEN_OR_CREATE);
	if (file) {
		CHECK_RESULT(crfiles_file_write_at(file, 0, "F", 1), CR_SUCCESS);
		crfiles_file_close(file);
	}
	CHECK(holds(path, "First"));
	// TRUNCATE empties it.
	file = open_file(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
	if (file) {
		uint64_t size = 99;
		CHECK_RESULT(crfiles_file_get_size(file, &size), CR_SUCCESS);
		CHECK(size == 0);
		crfiles_file_close(file);
	}
	// OPEN_OR_CREATE makes one too.
	char other[1100];
	path_in(other, sizeof(other), "created.txt");
	file = open_file(other, CRFILES_ACCESS_WRITE, CRFILES_CREATE_OPEN_OR_CREATE);
	crfiles_file_close(file);
	CR_RESULT r;
	CRFILES_PATH_INFO info = info_of(other, &r);
	CHECK(r == CR_SUCCESS && info.kind == CRFILES_PATH_KIND_FILE && info.size == 0);
	CHECK_RESULT(crfiles_path_remove(g_files, other), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
}

static void test_read_write(void) {
	char path[1100];
	path_in(path, sizeof(path), "rw.bin");
	crfiles_open_desc desc = open_desc(path, CRFILES_ACCESS_READ_WRITE, CRFILES_CREATE_TRUNCATE);
	desc.debug_name    = "rw file";
	crfiles_file file      = NULL;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_SUCCESS);
	if (!file)
		return;
	CHECK(cr_debug_name_get(file) && strcmp(cr_debug_name_get(file), "rw file") == 0);

	uint8_t bytes[1000];
	for (int i = 0; i < 1000; ++i)
		bytes[i] = (uint8_t)(i * 7);
	CHECK_RESULT(crfiles_file_write_at(file, 0, bytes, sizeof(bytes)), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_write_at(file, 0, NULL, 0), CR_SUCCESS);
	uint64_t value = 0;
	CHECK_RESULT(crfiles_file_get_size(file, &value), CR_SUCCESS);
	CHECK(value == 1000);

	uint8_t back[200];
	uint64_t count = 0;
	CHECK_RESULT(crfiles_file_read_at(file, 400, back, sizeof(back), &count), CR_SUCCESS);
	CHECK(count == 200 && memcmp(back, bytes + 400, 200) == 0);
	// The file ends first: what there was, then nothing at or past the end.
	CHECK_RESULT(crfiles_file_read_at(file, 900, back, sizeof(back), &count), CR_SUCCESS);
	CHECK(count == 100 && memcmp(back, bytes + 900, 100) == 0);
	CHECK_RESULT(crfiles_file_read_at(file, 1000, back, sizeof(back), &count), CR_SUCCESS);
	CHECK(count == 0);
	CHECK_RESULT(crfiles_file_read_at(file, 5000, back, sizeof(back), &count), CR_SUCCESS);
	CHECK(count == 0);

	// Over the middle changes only those bytes.
	const uint8_t patch[4] = {1, 2, 3, 4};
	CHECK_RESULT(crfiles_file_write_at(file, 10, patch, sizeof(patch)), CR_SUCCESS);
	uint8_t around[8];
	CHECK_RESULT(crfiles_file_read_at(file, 8, around, sizeof(around), &count), CR_SUCCESS);
	CHECK(around[0] == bytes[8] && around[1] == bytes[9] && memcmp(around + 2, patch, 4) == 0 && around[6] == bytes[14]);

	// Past the end grows the file; the gap reads zero.
	CHECK_RESULT(crfiles_file_write_at(file, 1100, patch, sizeof(patch)), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_get_size(file, &value), CR_SUCCESS);
	CHECK(value == 1104);
	uint8_t gap[128];
	memset(gap, 0xAA, sizeof(gap));
	CHECK_RESULT(crfiles_file_read_at(file, 1000, gap, 100, &count), CR_SUCCESS);
	int zeros = count == 100;
	for (int i = 0; i < 100; ++i)
		zeros = zeros && gap[i] == 0;
	CHECK(zeros);

	// Sizing: shorter, then longer with zeros.
	CHECK_RESULT(crfiles_file_set_size(file, 500), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_get_size(file, &value), CR_SUCCESS);
	CHECK(value == 500);
	CHECK_RESULT(crfiles_file_set_size(file, 600), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_read_at(file, 499, gap, 101, &count), CR_SUCCESS);
	CHECK(count == 101 && gap[0] == bytes[499] && gap[1] == 0 && gap[100] == 0);

	CHECK_RESULT(crfiles_file_flush(file, CRFILES_FLUSH_LEVEL_DEVICE), CR_SUCCESS);
	const CR_RESULT media = crfiles_file_flush(file, CRFILES_FLUSH_LEVEL_MEDIA);
	CHECK(media == CR_SUCCESS || media == CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED);
	CHECK_RESULT(crfiles_file_flush(file, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_DESC_INVALID_PARAMETERS);

	// Argument checks.
	CHECK_RESULT(crfiles_file_read_at(NULL, 0, back, 1, &count), CR_RESULT_FILES_INVALID_FILE);
	CHECK_RESULT(crfiles_file_read_at(file, 0, NULL, 1, &count), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(crfiles_file_read_at(file, 0, back, 1, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(crfiles_file_write_at(NULL, 0, back, 1), CR_RESULT_FILES_INVALID_FILE);
	CHECK_RESULT(crfiles_file_write_at(file, 0, NULL, 1), CR_RESULT_NULL_DESC);
	CHECK_RESULT(crfiles_file_get_size(NULL, &value), CR_RESULT_FILES_INVALID_FILE);
	CHECK_RESULT(crfiles_file_get_size(file, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(crfiles_file_set_size(NULL, 0), CR_RESULT_FILES_INVALID_FILE);
	CHECK_RESULT(crfiles_file_flush(NULL, CRFILES_FLUSH_LEVEL_DEVICE), CR_RESULT_FILES_INVALID_FILE);
	crfiles_file_close(file);

	// What each access refuses.
	file = open_file(path, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	if (file) {
		CHECK_RESULT(crfiles_file_write_at(file, 0, patch, 1), CR_RESULT_FILES_NOT_WRITABLE);
		CHECK_RESULT(crfiles_file_set_size(file, 0), CR_RESULT_FILES_NOT_WRITABLE);
		CHECK_RESULT(crfiles_file_flush(file, CRFILES_FLUSH_LEVEL_DEVICE), CR_RESULT_FILES_NOT_WRITABLE);
		crfiles_file_close(file);
	}
	file = open_file(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_OPEN_EXISTING);
	if (file) {
		CHECK_RESULT(crfiles_file_read_at(file, 0, back, 1, &count), CR_RESULT_FILES_NOT_READABLE);
		crfiles_mapping mapping = NULL;
		CHECK_RESULT(crfiles_file_map(file, 0, 1, &mapping), CR_RESULT_FILES_NOT_READABLE);
		crfiles_file_close(file);
	}
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
}

static void test_map(void) {
	char path[1100];
	path_in(path, sizeof(path), "map.bin");
	const uint64_t size = 100000;
	uint8_t *bytes      = (uint8_t *)malloc((size_t)size);
	CHECK(bytes != NULL);
	if (!bytes)
		return;
	for (uint64_t i = 0; i < size; ++i)
		bytes[i] = (uint8_t)(i * 13 + 5);
	crfiles_file file = open_file(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
	if (file) {
		CHECK_RESULT(crfiles_file_write_at(file, 0, bytes, size), CR_SUCCESS);
		crfiles_file_close(file);
	}
	file = open_file(path, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	if (!file) {
		free(bytes);
		return;
	}
	crfiles_mapping mapping = (crfiles_mapping)(uintptr_t)1;
	CHECK_RESULT(crfiles_file_map(file, 0, 10, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(crfiles_file_map(NULL, 0, 10, &mapping), CR_RESULT_FILES_INVALID_FILE);
	CHECK(mapping == NULL);
	CHECK_RESULT(crfiles_file_map(file, 0, 0, &mapping), CR_RESULT_ZERO_SIZE);
	CHECK_RESULT(crfiles_file_map(file, size, 1, &mapping), CR_RESULT_INDEX_OUT_OF_RANGE);
	CHECK_RESULT(crfiles_file_map(file, 10, size, &mapping), CR_RESULT_INDEX_OUT_OF_RANGE);

	crfiles_mapping whole = NULL;
	crfiles_mapping part  = NULL;
	CHECK_RESULT(crfiles_file_map(file, 0, size, &whole), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_map(file, 12345, 54321, &part), CR_SUCCESS);
	crfiles_file_close(file); // a mapping keeps what it needs
	if (whole) {
		CHECK(crfiles_mapping_get_size(whole) == size);
		CHECK(memcmp(crfiles_mapping_get_data(whole), bytes, (size_t)size) == 0);
		crfiles_mapping_release(whole);
	}
	if (part) {
		CHECK(crfiles_mapping_get_size(part) == 54321);
		CHECK(memcmp(crfiles_mapping_get_data(part), bytes + 12345, 54321) == 0);
		crfiles_mapping_release(part);
	}
	CHECK(crfiles_mapping_get_data(NULL) == NULL);
	CHECK(crfiles_mapping_get_size(NULL) == 0);
	crfiles_mapping_release(NULL);
	free(bytes);
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
}

static void test_paths(void) {
	char file_path[1100];
	path_in(file_path, sizeof(file_path), "info.txt");
	write_whole(file_path, "twelve bytes");

	CR_RESULT r;
	CRFILES_PATH_INFO info = info_of(file_path, &r);
	CHECK(r == CR_SUCCESS);
	CHECK(info.kind == CRFILES_PATH_KIND_FILE);
	CHECK(info.size == 12);
	// Modified within a day of now.
	CHECK(info.modified_ns > (int64_t)1700000000 * 1000000000);
	info = info_of(g_dir, &r);
	CHECK(r == CR_SUCCESS && info.kind == CRFILES_PATH_KIND_DIRECTORY && info.size == 0);
	char missing[1100];
	path_in(missing, sizeof(missing), "missing.txt");
	info = info_of(missing, &r);
	CHECK(r == CR_RESULT_FILES_NOT_FOUND);

	CRFILES_PATH_INFO wrong;
	memset(&wrong, 0, sizeof(wrong));
	CHECK_RESULT(crfiles_path_get_info(g_files, file_path, &wrong), CR_RESULT_STRUCT_WRONG_TYPE);
	CHECK_RESULT(crfiles_path_get_info(NULL, file_path, &info), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_path_get_info(g_files, NULL, &info), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_get_info(g_files, "", &info), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_get_info(g_files, file_path, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);

	// Replace: onto nothing, onto a file, at each level.
	char source[1100];
	char target[1100];
	path_in(source, sizeof(source), "new.txt");
	path_in(target, sizeof(target), "doc.txt");
	const CRFILES_FLUSH_LEVEL levels[] = {CRFILES_FLUSH_LEVEL_NONE, CRFILES_FLUSH_LEVEL_DEVICE, CRFILES_FLUSH_LEVEL_MEDIA};
	const char *texts[]            = {"one", "two", "three"};
	for (int i = 0; i < 3; ++i) {
		write_whole(source, texts[i]);
		r = crfiles_path_replace(g_files, source, target, levels[i]);
		CHECK(r == CR_SUCCESS || (levels[i] == CRFILES_FLUSH_LEVEL_MEDIA && r == CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED));
		info_of(source, &r);
		CHECK(r == CR_RESULT_FILES_NOT_FOUND);
		CHECK(holds(target, texts[i]));
	}
	CHECK_RESULT(crfiles_path_replace(g_files, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NOT_FOUND);
	CHECK(holds(target, "three"));
	CHECK_RESULT(crfiles_path_replace(NULL, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_path_replace(g_files, NULL, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_replace(g_files, source, "", CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_replace(g_files, source, target, (CRFILES_FLUSH_LEVEL)9), CR_RESULT_DESC_INVALID_PARAMETERS);

	// Remove.
	CHECK_RESULT(crfiles_path_remove(g_files, target), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, target), CR_RESULT_FILES_NOT_FOUND);
	CHECK_RESULT(crfiles_path_remove(g_files, g_dir), CR_RESULT_FILES_NOT_A_FILE);
	CHECK_RESULT(crfiles_path_remove(NULL, target), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_path_remove(g_files, NULL), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_remove(g_files, file_path), CR_SUCCESS);

	// Directories, one level at a time.
	char top[1100];
	char nested[1200];
	path_in(top, sizeof(top), "a");
	snprintf(nested, sizeof(nested), "%s" SLASH "b", top);
	CHECK_RESULT(crfiles_directory_create(g_files, nested), CR_RESULT_FILES_NOT_FOUND);
	CHECK_RESULT(crfiles_directory_create(g_files, top), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_create(g_files, nested), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_create(g_files, nested), CR_RESULT_FILES_ALREADY_EXISTS);
	info = info_of(nested, &r);
	CHECK(r == CR_SUCCESS && info.kind == CRFILES_PATH_KIND_DIRECTORY);
	char in_the_way[1300];
	snprintf(in_the_way, sizeof(in_the_way), "%s" SLASH "file", top);
	write_whole(in_the_way, "x");
	CHECK_RESULT(crfiles_directory_create(g_files, in_the_way), CR_RESULT_FILES_ALREADY_EXISTS);
	CHECK_RESULT(crfiles_directory_create(NULL, top), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_directory_create(g_files, ""), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_remove(g_files, in_the_way), CR_SUCCESS);
	CHECK(rmdir(nested) == 0);
	CHECK(rmdir(top) == 0);
}

/* A name outside ASCII: UTF-8 here, UTF-16 on Windows. */
static void test_unicode_name(void) {
	char path[1100];
	path_in(path, sizeof(path), "pl\xC3\xA4n \xE5\x9B\xB3.txt"); // "plän 図.txt"
	write_whole(path, "unicode");
	CHECK(holds(path, "unicode"));
	CR_RESULT r;
	CRFILES_PATH_INFO info = info_of(path, &r);
	CHECK(r == CR_SUCCESS && info.size == 7);
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
}

/* Two threads reading one file at once, each at its own offsets. */
struct reader {
	crfiles_file file;
	uint64_t start;
	int ok;
};

#if CARROTTUS_PLATFORM_WIN32
static unsigned __stdcall read_half(void *argument)
#else
static void *read_half(void *argument)
#endif
{
	struct reader *reader = (struct reader *)argument;
	reader->ok            = 1;
	for (int round = 0; round < 200; ++round) {
		for (uint64_t at = reader->start; at < reader->start + 4096; at += 256) {
			uint8_t buf[256];
			uint64_t count = 0;
			if (crfiles_file_read_at(reader->file, at, buf, sizeof(buf), &count) != CR_SUCCESS || count != 256) {
				reader->ok = 0;
				continue;
			}
			for (int i = 0; i < 256; ++i)
				reader->ok = reader->ok && buf[i] == (uint8_t)((at + (uint64_t)i) * 3);
		}
	}
	return 0;
}

static void test_two_readers(void) {
	char path[1100];
	path_in(path, sizeof(path), "shared.bin");
	uint8_t bytes[8192];
	for (int i = 0; i < 8192; ++i)
		bytes[i] = (uint8_t)(i * 3);
	crfiles_file file = open_file(path, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
	if (file) {
		CHECK_RESULT(crfiles_file_write_at(file, 0, bytes, sizeof(bytes)), CR_SUCCESS);
		crfiles_file_close(file);
	}
	file = open_file(path, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	if (!file)
		return;
	struct reader a = {file, 0, 0};
	struct reader b = {file, 4096, 0};
#if CARROTTUS_PLATFORM_WIN32
	HANDLE ta = (HANDLE)_beginthreadex(NULL, 0, read_half, &a, 0, NULL);
	HANDLE tb = (HANDLE)_beginthreadex(NULL, 0, read_half, &b, 0, NULL);
	WaitForSingleObject(ta, INFINITE);
	WaitForSingleObject(tb, INFINITE);
	CloseHandle(ta);
	CloseHandle(tb);
#else
	pthread_t ta;
	pthread_t tb;
	pthread_create(&ta, NULL, read_half, &a);
	pthread_create(&tb, NULL, read_half, &b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
#endif
	CHECK(a.ok && b.ok);
	crfiles_file_close(file);
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
}

/* ---- Listing ---- */

static crfiles_listing_desc listing_desc(const char *path) {
	crfiles_listing_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_FILES_LISTING_DESC;
	desc.path      = path;
	return desc;
}

static CRFILES_LISTING_ENTRY empty_entry(void) {
	CRFILES_LISTING_ENTRY entry;
	memset(&entry, 0, sizeof(entry));
	entry.base.type = CR_STRUCT_TYPE_FILES_LISTING_ENTRY;
	return entry;
}

/* What a listing of a directory gave: up to 16 entries. */
struct seen {
	char names[16][64];
	CRFILES_ENTRY_KIND kinds[16];
	bool utf8[16];
	int count;
	int extra; /* past 16, or a name too long to keep */
	CR_RESULT end;
};

static struct seen list_dir(const char *dir) {
	struct seen seen;
	memset(&seen, 0, sizeof(seen));
	const crfiles_listing_desc desc = listing_desc(dir);
	crfiles_listing listing         = NULL;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_SUCCESS);
	if (!listing)
		return seen;
	for (;;) {
		CRFILES_LISTING_ENTRY entry = empty_entry();
		const CR_RESULT result      = crfiles_listing_next(listing, &entry);
		if (result != CR_SUCCESS) {
			seen.end = result;
			break;
		}
		CHECK(entry.name_size == strlen(entry.name));
		if (seen.count < 16 && entry.name_size < 64) {
			memcpy(seen.names[seen.count], entry.name, entry.name_size + 1);
			seen.kinds[seen.count] = entry.kind;
			seen.utf8[seen.count]  = entry.name_is_utf8;
			++seen.count;
		} else {
			++seen.extra;
		}
	}
	// The end is said again, and the entry left as it was.
	CRFILES_LISTING_ENTRY after = empty_entry();
	after.name_size              = 77;
	CHECK_RESULT(crfiles_listing_next(listing, &after), CR_RESULT_FILES_END_OF_LISTING);
	CHECK(after.name_size == 77);
	crfiles_listing_close(listing);
	return seen;
}

/* How many times [name] was given, its kind and whether it was UTF-8. */
static int seen_count(
    const struct seen *seen,
    const char *name,
    CRFILES_ENTRY_KIND *out_kind,
    bool *out_utf8) {
	int count = 0;
	for (int i = 0; i < seen->count; ++i) {
		if (strcmp(seen->names[i], name) == 0) {
			++count;
			if (out_kind)
				*out_kind = seen->kinds[i];
			if (out_utf8)
				*out_utf8 = seen->utf8[i];
		}
	}
	return count;
}

static void test_listing_checks(void) {
	char dir[1100];
	path_in(dir, sizeof(dir), "listing_checks");
	CHECK_RESULT(crfiles_directory_create(g_files, dir), CR_SUCCESS);
	crfiles_listing_desc desc = listing_desc(dir);
	crfiles_listing listing   = (crfiles_listing)(uintptr_t)1;

	// Every argument in order, nothing made.
	CHECK_RESULT(crfiles_listing_open(NULL, &desc, &listing), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_listing_open(g_files, NULL, &listing), CR_RESULT_NULL_DESC);
	desc.base.type = CR_STRUCT_TYPE_FILES_OPEN_DESC;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_FILES_LISTING_DESC;
	desc.base.next = &desc.base;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	desc.path = NULL;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_FILES_NULL_PATH);
	CHECK(listing == NULL);
	desc.path = "";
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_FILES_NULL_PATH);

	// Missing, and a file where a directory is wanted.
	char missing[1100];
	path_in(missing, sizeof(missing), "listing_checks" SLASH "none");
	desc.path = missing;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_FILES_NOT_FOUND);
	char missing_parent[1100];
	path_in(missing_parent, sizeof(missing_parent), "listing_checks" SLASH "none" SLASH "deeper");
	desc.path = missing_parent;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_FILES_NOT_FOUND);
	char file[1100];
	path_in(file, sizeof(file), "listing_checks" SLASH "a_file");
	write_whole(file, "x");
	desc.path = file;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_RESULT_FILES_NOT_A_DIRECTORY);
	CHECK(listing == NULL);

	// An empty directory ends at once.
	char empty[1100];
	path_in(empty, sizeof(empty), "listing_checks" SLASH "empty");
	CHECK_RESULT(crfiles_directory_create(g_files, empty), CR_SUCCESS);
	const struct seen seen = list_dir(empty);
	CHECK(seen.count == 0 && seen.extra == 0);
	CHECK(seen.end == CR_RESULT_FILES_END_OF_LISTING);

	// next's own checks, and a debug name.
	desc.path       = dir;
	desc.debug_name = "checks";
	listing         = NULL;
	CHECK_RESULT(crfiles_listing_open(g_files, &desc, &listing), CR_SUCCESS);
	CRFILES_LISTING_ENTRY entry = empty_entry();
	CHECK_RESULT(crfiles_listing_next(NULL, &entry), CR_RESULT_FILES_INVALID_LISTING);
	CHECK_RESULT(crfiles_listing_next(listing, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	entry.base.type = CR_STRUCT_TYPE_FILES_PATH_INFO;
	CHECK_RESULT(crfiles_listing_next(listing, &entry), CR_RESULT_STRUCT_WRONG_TYPE);
	entry.base.type = CR_STRUCT_TYPE_FILES_LISTING_ENTRY;
	entry.base.next = &entry.base;
	CHECK_RESULT(crfiles_listing_next(listing, &entry), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	crfiles_listing_close(listing);
	crfiles_listing_close(NULL);

	CHECK_RESULT(crfiles_path_remove(g_files, file), CR_SUCCESS);
	CHECK(rmdir(empty) == 0);
	CHECK(rmdir(dir) == 0);
}

/* Makes a symbolic link at [link] to [target] (a directory if
 * [directory]); false where the platform won't let the test (Windows
 * without developer mode or the privilege). */
static bool make_symbolic_link(
    const char *target,
    const char *link,
    bool directory) {
#if CARROTTUS_PLATFORM_WIN32
	wchar_t wide_target[1100];
	wchar_t wide_link[1100];
	if (!MultiByteToWideChar(CP_UTF8, 0, target, -1, wide_target, 1100) || !MultiByteToWideChar(CP_UTF8, 0, link, -1, wide_link, 1100))
		return false;
	const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
	return CreateSymbolicLinkW(wide_link, wide_target, flags) != 0;
#else
	(void)directory;
	return symlink(target, link) == 0;
#endif
}

static void remove_link(
    const char *link,
    bool directory) {
#if CARROTTUS_PLATFORM_WIN32
	wchar_t wide[1100];
	MultiByteToWideChar(CP_UTF8, 0, link, -1, wide, 1100);
	CHECK(directory ? RemoveDirectoryW(wide) != 0 : DeleteFileW(wide) != 0);
#else
	(void)directory;
	CHECK(unlink(link) == 0);
#endif
}

static void test_listing_kinds(void) {
	char dir[1100];
	path_in(dir, sizeof(dir), "listing");
	CHECK_RESULT(crfiles_directory_create(g_files, dir), CR_SUCCESS);
	char a[1100];
	char sub[1100];
	char hidden[1100];
	char unicode[1100];
	path_in(a, sizeof(a), "listing" SLASH "a.txt");
	path_in(sub, sizeof(sub), "listing" SLASH "sub");
	path_in(hidden, sizeof(hidden), "listing" SLASH ".hidden");
	path_in(unicode, sizeof(unicode), "listing" SLASH "pl\xC3\xA4n \xE5\x9B\xB3.txt");
	write_whole(a, "a");
	write_whole(hidden, "h");
	write_whole(unicode, "u");
	CHECK_RESULT(crfiles_directory_create(g_files, sub), CR_SUCCESS);
#if CARROTTUS_PLATFORM_WIN32
	// Hidden by its attribute too, as Explorer hides a file.
	wchar_t wide_hidden[1100];
	MultiByteToWideChar(CP_UTF8, 0, hidden, -1, wide_hidden, 1100);
	CHECK(SetFileAttributesW(wide_hidden, FILE_ATTRIBUTE_HIDDEN) != 0);
#endif

	char file_link[1100];
	char dir_link[1100];
	path_in(file_link, sizeof(file_link), "listing" SLASH "file_link");
	path_in(dir_link, sizeof(dir_link), "listing" SLASH "dir_link");
	const bool file_linked = make_symbolic_link(a, file_link, false);
	const bool dir_linked  = make_symbolic_link(sub, dir_link, true);
	if (!file_linked || !dir_linked)
		printf("crfiles_test: symbolic links not made here (Windows needs developer mode): not listed\n");

#if CARROTTUS_PLATFORM_WIN32
	// A junction, through mklink, which needs no privilege.
	char junction[1100];
	path_in(junction, sizeof(junction), "listing" SLASH "junction");
	char command[2400];
	snprintf(command, sizeof(command), "cmd /c mklink /J \"%s\" \"%s\" >NUL", junction, sub);
	const bool junctioned = system(command) == 0;
	if (!junctioned)
		printf("crfiles_test: no junction made: not listed\n");
#else
	// A socket: OTHER.
	char socket_path[1100];
	path_in(socket_path, sizeof(socket_path), "listing" SLASH "sock");
	bool socketed = false;
	const int fd  = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd >= 0) {
		struct sockaddr_un address;
		memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		if (strlen(socket_path) < sizeof(address.sun_path)) {
			memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
			socketed = bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
		}
		close(fd);
	}
	if (!socketed)
		printf("crfiles_test: no socket made (path too long?): not listed\n");
#endif

	const struct seen seen = list_dir(dir);
	CHECK(seen.end == CR_RESULT_FILES_END_OF_LISTING);
	CHECK(seen.extra == 0);
	CHECK(seen_count(&seen, ".", NULL, NULL) == 0);
	CHECK(seen_count(&seen, "..", NULL, NULL) == 0);
	CRFILES_ENTRY_KIND kind = (CRFILES_ENTRY_KIND)0;
	bool utf8                = false;
	CHECK(seen_count(&seen, "a.txt", &kind, &utf8) == 1 && kind == CRFILES_ENTRY_KIND_FILE && utf8);
	CHECK(seen_count(&seen, "sub", &kind, NULL) == 1 && kind == CRFILES_ENTRY_KIND_DIRECTORY);
	CHECK(seen_count(&seen, ".hidden", &kind, NULL) == 1 && kind == CRFILES_ENTRY_KIND_FILE);
	CHECK(seen_count(&seen, "pl\xC3\xA4n \xE5\x9B\xB3.txt", &kind, &utf8) == 1 && kind == CRFILES_ENTRY_KIND_FILE && utf8);
	int expected = 4;
	if (file_linked) {
		CHECK(seen_count(&seen, "file_link", &kind, NULL) == 1 && kind == CRFILES_ENTRY_KIND_SYMBOLIC_LINK);
		++expected;
	}
	if (dir_linked) {
		CHECK(seen_count(&seen, "dir_link", &kind, NULL) == 1 && kind == CRFILES_ENTRY_KIND_SYMBOLIC_LINK);
		++expected;
	}
#if CARROTTUS_PLATFORM_WIN32
	if (junctioned) {
		CHECK(seen_count(&seen, "junction", &kind, NULL) == 1 && kind == CRFILES_ENTRY_KIND_SYMBOLIC_LINK);
		++expected;
	}
#else
	if (socketed) {
		CHECK(seen_count(&seen, "sock", &kind, NULL) == 1 && kind == CRFILES_ENTRY_KIND_OTHER);
		++expected;
	}
#endif
	CHECK(seen.count == expected);

	// A listed name round-trips: opened by the path made from it.
	char reopened[1100];
	snprintf(reopened, sizeof(reopened), "%s" SLASH "%s", dir, "pl\xC3\xA4n \xE5\x9B\xB3.txt");
	CHECK(holds(reopened, "u"));

	if (file_linked)
		remove_link(file_link, false);
	if (dir_linked)
		remove_link(dir_link, true);
#if CARROTTUS_PLATFORM_WIN32
	if (junctioned)
		remove_link(junction, true);
	CHECK(SetFileAttributesW(wide_hidden, FILE_ATTRIBUTE_NORMAL) != 0);
#else
	if (socketed)
		CHECK(unlink(socket_path) == 0);
#endif
	CHECK_RESULT(crfiles_path_remove(g_files, a), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, hidden), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, unicode), CR_SUCCESS);
	CHECK(rmdir(sub) == 0);
	CHECK(rmdir(dir) == 0);
}

/* Names that aren't UTF-8: listed with name_is_utf8 false, and opened by
 * the name given. */
static void test_listing_non_utf8(void) {
	char dir[1100];
	path_in(dir, sizeof(dir), "non_utf8");
	CHECK_RESULT(crfiles_directory_create(g_files, dir), CR_SUCCESS);
#if CARROTTUS_PLATFORM_WIN32
	// An unpaired surrogate, made through the W call; its WTF-8 is ED A0 80.
	wchar_t wide[1100];
	MultiByteToWideChar(CP_UTF8, 0, dir, -1, wide, 1100);
	wcscat_s(wide, 1100, L"\\lone\xD800.txt");
	HANDLE handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(handle != INVALID_HANDLE_VALUE);
	if (handle != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		CHECK(WriteFile(handle, "w", 1, &written, NULL) && written == 1);
		CloseHandle(handle);
	}
	const char *const name = "lone\xED\xA0\x80.txt";
#elif CARROTTUS_PLATFORM_APPLE
	// APFS and HFS+ refuse a name that isn't UTF-8 (EILSEQ): the file
	// system's rule, passed on. Nothing is made, so nothing is listed.
	const char *const name = "bad\xFF.txt";
	(void)name;
	{
		char made[1100];
		path_in(made, sizeof(made), "non_utf8" SLASH "bad\xFF.txt");
		const crfiles_open_desc desc = open_desc(made, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
		crfiles_file file            = NULL;
		CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_API_SPECIFIC_FAILURE);
		CHECK(file == NULL);
		const struct seen none = list_dir(dir);
		CHECK(none.count == 0);
		CHECK(rmdir(dir) == 0);
		return;
	}
#else
	// An invalid byte: Linux's file systems take a name's bytes as they are.
	const char *const name = "bad\xFF.txt";
	char made[1100];
	path_in(made, sizeof(made), "non_utf8" SLASH "bad\xFF.txt");
	write_whole(made, "w");
#endif
	const struct seen seen   = list_dir(dir);
	CRFILES_ENTRY_KIND kind = (CRFILES_ENTRY_KIND)0;
	bool utf8                = true;
	CHECK(seen.count == 1);
	CHECK(seen_count(&seen, name, &kind, &utf8) == 1 && kind == CRFILES_ENTRY_KIND_FILE && !utf8);
	char path[1100];
	snprintf(path, sizeof(path), "%s" SLASH "%s", dir, name);
	CHECK(holds(path, "w"));
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
	CHECK(rmdir(dir) == 0);

#if CARROTTUS_PLATFORM_WIN32
	// The converter's refusals: an encoded surrogate pair (WTF-8 forbids
	// it: the pair is the code point's own four bytes), an overlong '/',
	// a surrogate cut short, and a lead byte with nothing after it. None
	// names a file.
	const char *const refused[] = {"pair\xED\xA0\x80\xED\xB0\x80.txt", "over\xC0\xAF.txt", "cut\xED\xA0.txt", "end\xE5"};
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
		char bad[1100];
		path_in(bad, sizeof(bad), refused[i]);
		const crfiles_open_desc desc = open_desc(bad, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
		crfiles_file file            = NULL;
		CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_RESULT_FILES_NOT_FOUND);
	}
#endif
}

/* ---- Locks ---- */

static crfiles_file open_shared(const char *path) {
	crfiles_open_desc desc = open_desc(path, CRFILES_ACCESS_READ_WRITE, CRFILES_CREATE_OPEN_OR_CREATE);
	crfiles_file file      = NULL;
	CHECK_RESULT(crfiles_file_open(g_files, &desc, &file), CR_SUCCESS);
	return file;
}

struct waiter {
	crfiles_file file;
	atomic_int released; /* set by the holder just before it unlocks */
	atomic_int took;     /* 1 once the lock is had after that; 2 if before */
	CR_RESULT result;
};

#if CARROTTUS_PLATFORM_WIN32
static unsigned __stdcall wait_for_lock(void *argument)
#else
static void *wait_for_lock(void *argument)
#endif
{
	struct waiter *waiter = (struct waiter *)argument;
	waiter->result        = crfiles_file_lock(waiter->file, CRFILES_LOCK_EXCLUSIVE, true);
	atomic_store(&waiter->took, atomic_load(&waiter->released) ? 1 : 2);
	return 0;
}

static void sleep_ms(unsigned ms) {
#if CARROTTUS_PLATFORM_WIN32
	Sleep(ms);
#else
	struct timespec wait = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&wait, NULL);
#endif
}

/* Locks in [dir], a directory on the file system to test them on. */
static void test_locks_in(const char *dir) {
	char path[1100];
	snprintf(path, sizeof(path), "%s" SLASH "lock.bin", dir);
	crfiles_file one = open_shared(path);
	crfiles_file two = open_shared(path);
	if (!one || !two) {
		crfiles_file_close(one);
		crfiles_file_close(two);
		return;
	}
	CHECK_RESULT(crfiles_file_write_at(one, 0, "0123", 4), CR_SUCCESS);

	// Checks, in order.
	CHECK_RESULT(crfiles_file_lock(NULL, CRFILES_LOCK_EXCLUSIVE, false), CR_RESULT_FILES_INVALID_FILE);
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_NONE, false), CR_RESULT_DESC_INVALID_PARAMETERS);
	CHECK_RESULT(crfiles_file_lock(one, (CRFILES_LOCK)3, false), CR_RESULT_DESC_INVALID_PARAMETERS);
	CHECK_RESULT(crfiles_file_unlock(NULL), CR_RESULT_FILES_INVALID_FILE);
	CHECK_RESULT(crfiles_file_unlock(one), CR_RESULT_FILES_NOT_LOCKED);

	// Exclusive refuses exclusive and shared; it is advisory: the other
	// file still reads and writes.
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_EXCLUSIVE, false), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_EXCLUSIVE, false), CR_RESULT_FILES_ALREADY_LOCKED);
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_SHARED, false), CR_RESULT_FILES_ALREADY_LOCKED);
	CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_EXCLUSIVE, false), CR_RESULT_FILES_LOCKED);
	CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_SHARED, false), CR_RESULT_FILES_LOCKED);
	char read[4]   = {0};
	uint64_t count = 0;
	CHECK_RESULT(crfiles_file_read_at(two, 0, read, 4, &count), CR_SUCCESS);
	CHECK(count == 4 && memcmp(read, "0123", 4) == 0);
	CHECK_RESULT(crfiles_file_write_at(two, 1, "x", 1), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_set_size(two, 8), CR_SUCCESS);

	// Unlocking lets the other take it.
	CHECK_RESULT(crfiles_file_unlock(one), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_unlock(one), CR_RESULT_FILES_NOT_LOCKED);
	CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_EXCLUSIVE, false), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_SHARED, false), CR_RESULT_FILES_LOCKED);
	CHECK_RESULT(crfiles_file_unlock(two), CR_SUCCESS);

	// Shared allows shared, and refuses exclusive.
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_SHARED, false), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_SHARED, false), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_unlock(two), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_EXCLUSIVE, false), CR_RESULT_FILES_LOCKED);
	CHECK_RESULT(crfiles_file_unlock(one), CR_SUCCESS);

	// Closing lets go.
	CHECK_RESULT(crfiles_file_lock(one, CRFILES_LOCK_EXCLUSIVE, false), CR_SUCCESS);
	crfiles_file_close(one);
	CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_EXCLUSIVE, false), CR_SUCCESS);
	CHECK_RESULT(crfiles_file_unlock(two), CR_SUCCESS);

	// A waiting lock is had once the holder lets go, and not before.
	one = open_shared(path);
	if (one) {
		CHECK_RESULT(crfiles_file_lock(two, CRFILES_LOCK_EXCLUSIVE, false), CR_SUCCESS);
		struct waiter waiter;
		waiter.file = one;
		atomic_init(&waiter.released, 0);
		atomic_init(&waiter.took, 0);
		waiter.result = CR_RESULT_API_SPECIFIC_FAILURE;
#if CARROTTUS_PLATFORM_WIN32
		HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, wait_for_lock, &waiter, 0, NULL);
#else
		pthread_t thread;
		pthread_create(&thread, NULL, wait_for_lock, &waiter);
#endif
		sleep_ms(200);
		CHECK(atomic_load(&waiter.took) == 0);
		atomic_store(&waiter.released, 1);
		CHECK_RESULT(crfiles_file_unlock(two), CR_SUCCESS);
#if CARROTTUS_PLATFORM_WIN32
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
#else
		pthread_join(thread, NULL);
#endif
		CHECK_RESULT(waiter.result, CR_SUCCESS);
		CHECK(atomic_load(&waiter.took) == 1);
		CHECK_RESULT(crfiles_file_unlock(one), CR_SUCCESS);
		crfiles_file_close(one);
	}
	crfiles_file_close(two);
	CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
}

/* ---- A file's permissions copied (card S42) ---- */

#if CARROTTUS_PLATFORM_WIN32
static bool make_junction(
    const char *target,
    const char *link);

static DWORD attributes_of(const char *path) {
	wchar_t wide[1100];
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1100))
		return INVALID_FILE_ATTRIBUTES;
	return GetFileAttributesW(wide);
}

static void set_attributes(
    const char *path,
    DWORD attributes) {
	wchar_t wide[1100];
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1100);
	CHECK(SetFileAttributesW(wide, attributes) != 0);
}
#else
static mode_t mode_of(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (st.st_mode & 07777) : (mode_t)0xFFFF;
}
#endif

static void test_copy_mode(void) {
	char source[1100];
	char target[1100];
	char dir[1100];
	char missing[1100];
	path_in(source, sizeof(source), "mode_source.txt");
	path_in(target, sizeof(target), "mode_target.txt");
	path_in(dir, sizeof(dir), "mode_dir");
	path_in(missing, sizeof(missing), "mode_missing.txt");
	write_whole(source, "s");
	write_whole(target, "t");
	CHECK_RESULT(crfiles_directory_create(g_files, dir), CR_SUCCESS);

	// Argument checks, in order.
	CHECK_RESULT(crfiles_path_copy_mode(NULL, NULL, NULL), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, NULL, NULL), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, "", target), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, NULL), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, ""), CR_RESULT_FILES_NULL_PATH);

	// Nothing at either path: not found, the target unchanged.
	CHECK_RESULT(crfiles_path_copy_mode(g_files, missing, target), CR_RESULT_FILES_NOT_FOUND);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, missing), CR_RESULT_FILES_NOT_FOUND);
	CHECK(holds(target, "t"));

#if CARROTTUS_PLATFORM_WIN32
	// The read-only attribute, both ways; the target's hidden attribute kept.
	set_attributes(target, FILE_ATTRIBUTE_HIDDEN);
	set_attributes(source, FILE_ATTRIBUTE_READONLY);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, target), CR_SUCCESS);
	CHECK((attributes_of(target) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));
	set_attributes(source, FILE_ATTRIBUTE_NORMAL);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, target), CR_SUCCESS);
	CHECK((attributes_of(target) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) == FILE_ATTRIBUTE_HIDDEN);
	// Copied again onto a target that has it already: unchanged.
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, target), CR_SUCCESS);
	CHECK((attributes_of(target) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) == FILE_ATTRIBUTE_HIDDEN);
	set_attributes(target, FILE_ATTRIBUTE_NORMAL);

	// A directory either way.
	set_attributes(source, FILE_ATTRIBUTE_READONLY);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, dir), CR_SUCCESS);
	CHECK((attributes_of(dir) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY));
	CHECK_RESULT(crfiles_path_copy_mode(g_files, dir, target), CR_SUCCESS);
	CHECK((attributes_of(target) & FILE_ATTRIBUTE_READONLY) != 0);
	set_attributes(source, FILE_ATTRIBUTE_NORMAL);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, dir), CR_SUCCESS);
	CHECK((attributes_of(dir) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == FILE_ATTRIBUTE_DIRECTORY);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, target), CR_SUCCESS);

	// A junction as the target: what it leads to changed, the link not.
	char junction[1100];
	path_in(junction, sizeof(junction), "mode_junction");
	if (make_junction(dir, junction)) {
		set_attributes(source, FILE_ATTRIBUTE_READONLY);
		CHECK_RESULT(crfiles_path_copy_mode(g_files, source, junction), CR_SUCCESS);
		CHECK((attributes_of(dir) & FILE_ATTRIBUTE_READONLY) != 0);
		CHECK((attributes_of(junction) & FILE_ATTRIBUTE_READONLY) == 0);
		set_attributes(source, FILE_ATTRIBUTE_NORMAL);
		CHECK_RESULT(crfiles_path_copy_mode(g_files, source, junction), CR_SUCCESS);
		CHECK((attributes_of(dir) & FILE_ATTRIBUTE_READONLY) == 0);
		CHECK_RESULT(crfiles_directory_remove(g_files, junction), CR_SUCCESS);
	} else {
		printf("crfiles_test: no junction made: copy_mode through one not run\n");
	}
#else
	// The nine bits, whatever the umask; never the set-ID or sticky bits.
	const mode_t old_umask = umask(077);
	const mode_t modes[]   = {0755, 0444, 0600, 0640};
	for (int i = 0; i < 4; ++i) {
		CHECK(chmod(source, modes[i]) == 0);
		CHECK(chmod(target, 0644) == 0);
		CHECK_RESULT(crfiles_path_copy_mode(g_files, source, target), CR_SUCCESS);
		CHECK(mode_of(target) == modes[i]);
	}
	if (chmod(source, 06755) == 0 && (mode_of(source) & 06000)) {
		CHECK_RESULT(crfiles_path_copy_mode(g_files, source, target), CR_SUCCESS);
		CHECK(mode_of(target) == 0755);
	} else {
		printf("crfiles_test: no set-ID bits on the source here: their copy not run\n");
	}
	umask(old_umask);
	CHECK(chmod(source, 0644) == 0);
	CHECK(chmod(target, 0644) == 0);

	// A directory either way.
	CHECK(chmod(source, 0700) == 0);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source, dir), CR_SUCCESS);
	CHECK(mode_of(dir) == 0700);
	CHECK(chmod(dir, 0751) == 0);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, dir, target), CR_SUCCESS);
	CHECK(mode_of(target) == 0751);
	CHECK(chmod(dir, 0755) == 0);
	CHECK(chmod(source, 0644) == 0);
	CHECK(chmod(target, 0644) == 0);

	// Symbolic links followed both ways, the links themselves unchanged.
	char source_link[1100];
	char target_link[1100];
	path_in(source_link, sizeof(source_link), "mode_source_link");
	path_in(target_link, sizeof(target_link), "mode_target_link");
	CHECK(symlink(source, source_link) == 0);
	CHECK(symlink(target, target_link) == 0);
	struct stat link_before;
	CHECK(lstat(target_link, &link_before) == 0);
	CHECK(chmod(source, 0750) == 0);
	CHECK_RESULT(crfiles_path_copy_mode(g_files, source_link, target_link), CR_SUCCESS);
	CHECK(mode_of(target) == 0750);
	struct stat link_after;
	CHECK(lstat(target_link, &link_after) == 0);
	CHECK(link_after.st_mode == link_before.st_mode);
	CHECK(unlink(source_link) == 0);
	CHECK(unlink(target_link) == 0);
	CHECK(chmod(source, 0644) == 0);

	// Another's entry: refused. The root directory, onto itself, which
	// would change nothing even where the call were allowed.
	if (geteuid() != 0) {
		CHECK_RESULT(crfiles_path_copy_mode(g_files, "/", "/"), CR_RESULT_FILES_ACCESS_DENIED);
	} else {
		printf("crfiles_test: run as root: copy_mode's refusal not run\n");
	}
#endif

	CHECK_RESULT(crfiles_path_remove(g_files, source), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, target), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, dir), CR_SUCCESS);
}

/* ---- Removing and renaming a directory ---- */

#if CARROTTUS_PLATFORM_WIN32
/* A junction at [link] to [target], through mklink, which needs no
 * privilege; false if none was made. */
static bool make_junction(
    const char *target,
    const char *link) {
	char command[2400];
	snprintf(command, sizeof(command), "cmd /c mklink /J \"%s\" \"%s\" >NUL", link, target);
	return system(command) == 0;
}

/* [path] opened for reading, sharing read and write but not delete, so a
 * remove or rename of it, or of what holds it, is refused. (A handle that
 * asks only for attributes takes no part in sharing: Windows lets a delete
 * past it.) */
static HANDLE hold_without_delete(const char *path) {
	wchar_t wide[1100];
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1100))
		return INVALID_HANDLE_VALUE;
	return CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}
#endif

static CR_RESULT kind_at(
    const char *path,
    CRFILES_PATH_KIND *out_kind) {
	CR_RESULT r                  = CR_SUCCESS;
	const CRFILES_PATH_INFO info = info_of(path, &r);
	*out_kind                     = info.kind;
	return r;
}

static void test_directory_remove(void) {
	CRFILES_PATH_KIND kind = (CRFILES_PATH_KIND)0;

	// Argument checks, in order.
	char dir[1100];
	path_in(dir, sizeof(dir), "rm");
	CHECK_RESULT(crfiles_directory_remove(NULL, dir), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_directory_remove(NULL, NULL), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_directory_remove(g_files, NULL), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_directory_remove(g_files, ""), CR_RESULT_FILES_NULL_PATH);

	// An empty directory goes; then nothing is there.
	CHECK_RESULT(crfiles_directory_create(g_files, dir), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, dir), CR_SUCCESS);
	CHECK_RESULT(kind_at(dir, &kind), CR_RESULT_FILES_NOT_FOUND);
	CHECK_RESULT(crfiles_directory_remove(g_files, dir), CR_RESULT_FILES_NOT_FOUND);

	// One that holds a file is refused, and stays with its file.
	char full[1100];
	char inside[1200];
	path_in(full, sizeof(full), "rm_full");
	snprintf(inside, sizeof(inside), "%s" SLASH "keep.txt", full);
	CHECK_RESULT(crfiles_directory_create(g_files, full), CR_SUCCESS);
	write_whole(inside, "kept");
	CHECK_RESULT(crfiles_directory_remove(g_files, full), CR_RESULT_FILES_NOT_EMPTY);
	CHECK(kind_at(full, &kind) == CR_SUCCESS && kind == CRFILES_PATH_KIND_DIRECTORY);
	CHECK(holds(inside, "kept"));

	// One that holds only a directory is refused too.
	char only_dir[1100];
	char nested[1200];
	path_in(only_dir, sizeof(only_dir), "rm_nested");
	snprintf(nested, sizeof(nested), "%s" SLASH "inner", only_dir);
	CHECK_RESULT(crfiles_directory_create(g_files, only_dir), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_create(g_files, nested), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, only_dir), CR_RESULT_FILES_NOT_EMPTY);
	CHECK_RESULT(crfiles_directory_remove(g_files, nested), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, only_dir), CR_SUCCESS);

	// A file isn't a directory, and stays.
	char file[1100];
	path_in(file, sizeof(file), "rm_file.txt");
	write_whole(file, "f");
	CHECK_RESULT(crfiles_directory_remove(g_files, file), CR_RESULT_FILES_NOT_A_DIRECTORY);
	CHECK(holds(file, "f"));

	// Nothing on the way.
	char under_missing[1200];
	snprintf(under_missing, sizeof(under_missing), "%s" SLASH "missing" SLASH "x", g_dir);
	CHECK_RESULT(crfiles_directory_remove(g_files, under_missing), CR_RESULT_FILES_NOT_FOUND);

	// A link to a directory is removed itself; what it led to stays, with
	// its file. A link to a file isn't a directory.
	char dir_link[1100];
	char file_link[1100];
	path_in(dir_link, sizeof(dir_link), "rm_dir_link");
	path_in(file_link, sizeof(file_link), "rm_file_link");
	if (make_symbolic_link(full, dir_link, true)) {
		CHECK_RESULT(crfiles_directory_remove(g_files, dir_link), CR_SUCCESS);
		CHECK_RESULT(kind_at(dir_link, &kind), CR_RESULT_FILES_NOT_FOUND);
		CHECK(holds(inside, "kept"));
	} else {
		printf("crfiles_test: no symbolic link to a directory made: its remove not run\n");
	}
	if (make_symbolic_link(file, file_link, false)) {
		CHECK_RESULT(crfiles_directory_remove(g_files, file_link), CR_RESULT_FILES_NOT_A_DIRECTORY);
		remove_link(file_link, false);
	} else {
		printf("crfiles_test: no symbolic link to a file made: its remove not run\n");
	}
#if CARROTTUS_PLATFORM_WIN32
	char junction[1100];
	path_in(junction, sizeof(junction), "rm_junction");
	if (make_junction(full, junction)) {
		CHECK_RESULT(crfiles_directory_remove(g_files, junction), CR_SUCCESS);
		CHECK_RESULT(kind_at(junction, &kind), CR_RESULT_FILES_NOT_FOUND);
		CHECK(holds(inside, "kept"));
	} else {
		printf("crfiles_test: no junction made: its remove not run\n");
	}

	// Held open without sharing delete: in use, and still there.
	CHECK_RESULT(crfiles_directory_create(g_files, dir), CR_SUCCESS);
	HANDLE held = hold_without_delete(dir);
	CHECK(held != INVALID_HANDLE_VALUE);
	if (held != INVALID_HANDLE_VALUE) {
		CHECK_RESULT(crfiles_directory_remove(g_files, dir), CR_RESULT_FILES_IN_USE);
		CloseHandle(held);
	}
	CHECK_RESULT(crfiles_directory_remove(g_files, dir), CR_SUCCESS);
#endif

	CHECK_RESULT(crfiles_path_remove(g_files, file), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, inside), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, full), CR_SUCCESS);
}

static void test_directory_rename(void) {
	CRFILES_PATH_KIND kind = (CRFILES_PATH_KIND)0;
	char source[1100];
	char target[1100];
	char source_file[1200];
	char target_file[1200];
	path_in(source, sizeof(source), "mv_source");
	path_in(target, sizeof(target), "mv_target");
	snprintf(source_file, sizeof(source_file), "%s" SLASH "f.txt", source);
	snprintf(target_file, sizeof(target_file), "%s" SLASH "f.txt", target);

	// Argument checks, in order.
	CHECK_RESULT(crfiles_directory_rename(NULL, NULL, NULL, (CRFILES_FLUSH_LEVEL)9), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_directory_rename(g_files, NULL, NULL, (CRFILES_FLUSH_LEVEL)9), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_directory_rename(g_files, "", target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_directory_rename(g_files, source, NULL, (CRFILES_FLUSH_LEVEL)9), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_directory_rename(g_files, source, "", CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NULL_PATH);
	CHECK_RESULT(crfiles_directory_rename(g_files, source, target, (CRFILES_FLUSH_LEVEL)9), CR_RESULT_DESC_INVALID_PARAMETERS);

	// No source.
	CHECK_RESULT(crfiles_directory_rename(g_files, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NOT_FOUND);

	// A directory with a file, to a free name and back, at each level: the
	// file under the new name, nothing under the old.
	CHECK_RESULT(crfiles_directory_create(g_files, source), CR_SUCCESS);
	write_whole(source_file, "moved");
	const CRFILES_FLUSH_LEVEL levels[] = {CRFILES_FLUSH_LEVEL_NONE, CRFILES_FLUSH_LEVEL_DEVICE, CRFILES_FLUSH_LEVEL_MEDIA};
	for (int i = 0; i < 3; ++i) {
		CR_RESULT r = crfiles_directory_rename(g_files, source, target, levels[i]);
		CHECK(r == CR_SUCCESS || (levels[i] == CRFILES_FLUSH_LEVEL_MEDIA && r == CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED));
		CHECK_RESULT(kind_at(source, &kind), CR_RESULT_FILES_NOT_FOUND);
		CHECK(holds(target_file, "moved"));
		r = crfiles_directory_rename(g_files, target, source, levels[i]);
		CHECK(r == CR_SUCCESS || (levels[i] == CRFILES_FLUSH_LEVEL_MEDIA && r == CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED));
		CHECK(holds(source_file, "moved"));
	}

	// Something at the target: a file, an empty directory, a full one. Each
	// refused, both left as they were.
	write_whole(target, "in the way");
	CHECK_RESULT(crfiles_directory_rename(g_files, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_ALREADY_EXISTS);
	CHECK(holds(target, "in the way"));
	CHECK(holds(source_file, "moved"));
	CHECK_RESULT(crfiles_path_remove(g_files, target), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_create(g_files, target), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_rename(g_files, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_ALREADY_EXISTS);
	CHECK(kind_at(target, &kind) == CR_SUCCESS && kind == CRFILES_PATH_KIND_DIRECTORY);
	CHECK(holds(source_file, "moved"));
	write_whole(target_file, "theirs");
	CHECK_RESULT(crfiles_directory_rename(g_files, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_ALREADY_EXISTS);
	CHECK(holds(target_file, "theirs"));
	CHECK(holds(source_file, "moved"));
	CHECK_RESULT(crfiles_path_remove(g_files, target_file), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, target), CR_SUCCESS);

	// A file source isn't a directory.
	char file[1100];
	path_in(file, sizeof(file), "mv_file.txt");
	write_whole(file, "f");
	CHECK_RESULT(crfiles_directory_rename(g_files, file, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NOT_A_DIRECTORY);
	CHECK(holds(file, "f"));
	CHECK_RESULT(kind_at(target, &kind), CR_RESULT_FILES_NOT_FOUND);

	// No parent for the target.
	char orphan[1200];
	snprintf(orphan, sizeof(orphan), "%s" SLASH "missing" SLASH "x", g_dir);
	CHECK_RESULT(crfiles_directory_rename(g_files, source, orphan, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_NOT_FOUND);
	CHECK(holds(source_file, "moved"));

#if CARROTTUS_PLATFORM_WIN32
	// A file inside held open without sharing delete: in use, nothing moved.
	HANDLE held = hold_without_delete(source_file);
	CHECK(held != INVALID_HANDLE_VALUE);
	if (held != INVALID_HANDLE_VALUE) {
		CHECK_RESULT(crfiles_directory_rename(g_files, source, target, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_IN_USE);
		CloseHandle(held);
		CHECK(holds(source_file, "moved"));
		CHECK_RESULT(kind_at(target, &kind), CR_RESULT_FILES_NOT_FOUND);
	}
#endif

	// Across volumes, where a directory on another one is named.
	const char *other = getenv("CRFILES_TEST_OTHER_VOLUME_DIR");
	if (other && other[0]) {
		char away[1200];
		snprintf(away, sizeof(away), "%s" SLASH "crfiles_test_moved", other);
		CHECK_RESULT(crfiles_directory_rename(g_files, source, away, CRFILES_FLUSH_LEVEL_NONE), CR_RESULT_FILES_DIFFERENT_VOLUME);
		CHECK(holds(source_file, "moved"));
		printf("crfiles_test: a directory renamed across volumes, to %s: refused\n", other);
	} else {
		printf("crfiles_test: no CRFILES_TEST_OTHER_VOLUME_DIR: a rename across volumes not run\n");
	}

	CHECK_RESULT(crfiles_path_remove(g_files, file), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(g_files, source_file), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(g_files, source), CR_SUCCESS);
}

/* Removes the tree at [dir] through the calls a caller has: a listing, then
 * each entry (a directory's tree first), then the directory. */
static void remove_tree(const char *dir) {
	const struct seen seen = list_dir(dir);
	CHECK(seen.extra == 0);
	for (int i = 0; i < seen.count; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s" SLASH "%s", dir, seen.names[i]);
		if (seen.kinds[i] == CRFILES_ENTRY_KIND_DIRECTORY)
			remove_tree(path);
		else
			CHECK_RESULT(crfiles_path_remove(g_files, path), CR_SUCCESS);
	}
	CHECK_RESULT(crfiles_directory_remove(g_files, dir), CR_SUCCESS);
}

static void test_directory_tree(void) {
	// A tree written whole beside its place, renamed into it, then removed.
	char partial[1100];
	char done[1100];
	path_in(partial, sizeof(partial), ".tree.partial");
	path_in(done, sizeof(done), "tree");
	const char *dirs[]  = {"", SLASH "a", SLASH "a" SLASH "b", SLASH "c"};
	const char *files[] = {SLASH "top.txt", SLASH "a" SLASH "one.txt", SLASH "a" SLASH "b" SLASH "two.txt", SLASH "a" SLASH "b" SLASH "three.txt"};
	for (int i = 0; i < 4; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s%s", partial, dirs[i]);
		CHECK_RESULT(crfiles_directory_create(g_files, path), CR_SUCCESS);
	}
	for (int i = 0; i < 4; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s%s", partial, files[i]);
		write_whole(path, files[i]);
	}
	CHECK_RESULT(crfiles_directory_rename(g_files, partial, done, CRFILES_FLUSH_LEVEL_DEVICE), CR_SUCCESS);
	for (int i = 0; i < 4; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s%s", done, files[i]);
		CHECK(holds(path, files[i]));
	}
	CHECK_RESULT(crfiles_directory_remove(g_files, done), CR_RESULT_FILES_NOT_EMPTY);
	remove_tree(done);
	CRFILES_PATH_KIND kind = (CRFILES_PATH_KIND)0;
	CHECK_RESULT(kind_at(done, &kind), CR_RESULT_FILES_NOT_FOUND);
	CHECK_RESULT(kind_at(partial, &kind), CR_RESULT_FILES_NOT_FOUND);
}

/* The temporary directory's file system, then each directory named in
 * CRFILES_TEST_LOCK_DIRS (';'-separated: another file system, a share),
 * each reported. */
static void test_locks(void) {
	test_locks_in(g_dir);
	printf("crfiles_test: locks on the temporary directory's file system\n");
	const char *more = getenv("CRFILES_TEST_LOCK_DIRS");
	if (!more || !more[0])
		return;
	char dirs[2048];
	snprintf(dirs, sizeof(dirs), "%s", more);
	char *rest = dirs;
	while (rest && rest[0]) {
		char *dir  = rest;
		char *stop = strchr(rest, ';');
		if (stop) {
			*stop = 0;
			rest  = stop + 1;
		} else {
			rest = NULL;
		}
		const int before = failures;
		test_locks_in(dir);
		printf("crfiles_test: locks in %s: %s\n", dir, failures == before ? "passed" : "FAILED");
	}
}

/* ---- Messages (card S10, Carrottus Files' half) ---- */

/* What one message held, copied in the callback (M5: nothing it points to
 * outlives the call). One thread's: the threaded test counts elsewhere. */
typedef struct recorded_message {
	CR_STRUCT_TYPE type;
	const CR_STRUCT_BASE *next;
	CR_MESSAGE_ORIGIN origin;
	CR_MESSAGE_SEVERITY severity;
	CR_PLATFORM_ERROR_SOURCE source;
	CR_RESULT result;
	uint32_t id;
	uint32_t dropped;
	int64_t code;
	const char *call;
	const void *object;
	char object_name[64];
	char subject_name[64];
	bool text_null;
	char text[1024];
} recorded_message;

typedef struct recorder {
	int count;
	recorded_message last;
} recorder;

static void copy_text(
    char *out,
    size_t capacity,
    const char *text) {
	if (!text) {
		out[0] = 0;
		return;
	}
	snprintf(out, capacity, "%s", text);
}

static void record_message(
    const CR_MESSAGE *message,
    void *user_data) {
	recorder *into         = (recorder *)user_data;
	recorded_message *last = &into->last;
	++into->count;
	last->type      = message->base.type;
	last->next      = message->base.next;
	last->origin    = message->origin;
	last->severity  = message->severity;
	last->source    = message->platform_source;
	last->result    = message->result;
	last->id        = message->id;
	last->dropped   = message->dropped;
	last->code      = message->platform_code;
	last->call      = message->call;
	last->object    = message->object;
	last->text_null = message->text == NULL;
	copy_text(last->object_name, sizeof(last->object_name), message->object_debug_name);
	copy_text(last->subject_name, sizeof(last->subject_name), message->subject_debug_name);
	copy_text(last->text, sizeof(last->text), message->text);
}

static crfiles_instance make_messaged_instance(
    const CR_ALLOCATOR *allocator,
    CR_MESSAGE_CALLBACK callback,
    void *user_data) {
	crfiles_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator          = allocator;
	desc.message_callback   = callback;
	desc.message_user_data  = user_data;
	crfiles_instance files = NULL;
	CHECK_RESULT(crfiles_instance_create(&desc, &files), CR_SUCCESS);
	return files;
}

/* One platform failure's message, as M2 has it. */
static void check_platform_message(
    const recorder *seen,
    const char *call,
    const void *object,
    CR_RESULT result,
    CR_PLATFORM_ERROR_SOURCE source) {
	CHECK(seen->count == 1);
	CHECK(seen->last.type == CR_STRUCT_TYPE_MESSAGE);
	CHECK(seen->last.next == NULL);
	CHECK(seen->last.origin == CR_MESSAGE_ORIGIN_PLATFORM);
	CHECK(seen->last.severity == CR_MESSAGE_SEVERITY_ERROR);
	CHECK(seen->last.source == source);
	CHECK(seen->last.result == result);
	CHECK(seen->last.id == 0);
	CHECK(seen->last.dropped == 0);
	CHECK(seen->last.call && strcmp(seen->last.call, call) == 0);
	CHECK(seen->last.object == object);
	CHECK(!seen->last.text_null);
}

#if CARROTTUS_PLATFORM_WIN32
#define PLATFORM_SOURCE CR_PLATFORM_ERROR_SOURCE_WIN32
#define CODE_NOT_FOUND ERROR_FILE_NOT_FOUND
#define CODE_THROUGH_FILE ERROR_PATH_NOT_FOUND
#define CODE_ACCESS_DENIED ERROR_ACCESS_DENIED
#else
#define PLATFORM_SOURCE CR_PLATFORM_ERROR_SOURCE_ERRNO
#define CODE_NOT_FOUND ENOENT
#define CODE_THROUGH_FILE ENOTDIR
#define CODE_ACCESS_DENIED EACCES
#endif

static void test_messages(void) {
	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
	recorder seen;
	memset(&seen, 0, sizeof(seen));
	crfiles_instance files = make_messaged_instance(&allocator, record_message, &seen);
	crfiles_instance quiet = make_messaged_instance(&allocator, NULL, NULL);
	if (!files || !quiet) {
		crfiles_instance_destroy(files);
		crfiles_instance_destroy(quiet);
		return;
	}

	// A missing file: NOT_FOUND, its message the platform's number, the
	// instance the object and the desc's name the subject.
	char missing[1100];
	path_in(missing, sizeof(missing), "no such file.txt");
	crfiles_open_desc open = open_desc(missing, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	open.debug_name         = "the missing one";
	crfiles_file file      = NULL;
	CHECK_RESULT(crfiles_file_open(files, &open, &file), CR_RESULT_FILES_NOT_FOUND);
	check_platform_message(&seen, "crfiles_file_open", files, CR_RESULT_FILES_NOT_FOUND, PLATFORM_SOURCE);
	CHECK(seen.last.code == CODE_NOT_FOUND);
	CHECK(strcmp(seen.last.subject_name, "the missing one") == 0);
	CHECK(seen.last.object_name[0] == 0);

	// What is at a missing path, and at a path through a file: both
	// NOT_FOUND, told apart by the message's number.
	CRFILES_PATH_INFO info;
	memset(&info, 0, sizeof(info));
	info.base.type = CR_STRUCT_TYPE_FILES_PATH_INFO;
	seen.count     = 0;
	CHECK_RESULT(crfiles_path_get_info(files, missing, &info), CR_RESULT_FILES_NOT_FOUND);
	check_platform_message(&seen, "crfiles_path_get_info", files, CR_RESULT_FILES_NOT_FOUND, PLATFORM_SOURCE);
	CHECK(seen.last.code == CODE_NOT_FOUND);
	CHECK(seen.last.subject_name[0] == 0);
	char plain[1100];
	char through[1300];
	path_in(plain, sizeof(plain), "plain.txt");
	write_whole(plain, "plain");
	snprintf(through, sizeof(through), "%s" SLASH "inside", plain);
	seen.count = 0;
	CHECK_RESULT(crfiles_path_get_info(files, through, &info), CR_RESULT_FILES_NOT_FOUND);
	check_platform_message(&seen, "crfiles_path_get_info", files, CR_RESULT_FILES_NOT_FOUND, PLATFORM_SOURCE);
	CHECK(seen.last.code == CODE_THROUGH_FILE);

	// Access denied: a read-only file opened for writing.
#if CARROTTUS_PLATFORM_WIN32
	set_attributes(plain, FILE_ATTRIBUTE_READONLY);
	const bool denies = true;
#else
	CHECK(chmod(plain, 0444) == 0);
	const bool denies = geteuid() != 0; // root writes anything
#endif
	if (denies) {
		crfiles_open_desc write = open_desc(plain, CRFILES_ACCESS_WRITE, CRFILES_CREATE_OPEN_EXISTING);
		seen.count               = 0;
		CHECK_RESULT(crfiles_file_open(files, &write, &file), CR_RESULT_FILES_ACCESS_DENIED);
		check_platform_message(&seen, "crfiles_file_open", files, CR_RESULT_FILES_ACCESS_DENIED, PLATFORM_SOURCE);
		CHECK(seen.last.code == CODE_ACCESS_DENIED);
	}
#if CARROTTUS_PLATFORM_WIN32
	set_attributes(plain, FILE_ATTRIBUTE_NORMAL);
#else
	CHECK(chmod(plain, 0644) == 0);
	// Access denied by a directory: a file inside one made 0000.
	if (denies) {
		char closed[1100];
		char inside[1300];
		path_in(closed, sizeof(closed), "closed");
		CHECK_RESULT(crfiles_directory_create(files, closed), CR_SUCCESS);
		snprintf(inside, sizeof(inside), "%s" SLASH "inside.txt", closed);
		write_whole(inside, "inside");
		CHECK(chmod(closed, 0000) == 0);
		crfiles_open_desc behind = open_desc(inside, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
		seen.count                = 0;
		CHECK_RESULT(crfiles_file_open(files, &behind, &file), CR_RESULT_FILES_ACCESS_DENIED);
		check_platform_message(&seen, "crfiles_file_open", files, CR_RESULT_FILES_ACCESS_DENIED, PLATFORM_SOURCE);
		CHECK(seen.last.code == EACCES);
		seen.count = 0;
		CHECK_RESULT(crfiles_path_get_info(files, inside, &info), CR_RESULT_FILES_ACCESS_DENIED);
		check_platform_message(&seen, "crfiles_path_get_info", files, CR_RESULT_FILES_ACCESS_DENIED, PLATFORM_SOURCE);
		CHECK(seen.last.code == EACCES);
		CHECK(chmod(closed, 0755) == 0);
		CHECK_RESULT(crfiles_path_remove(files, inside), CR_SUCCESS);
		CHECK_RESULT(crfiles_directory_remove(files, closed), CR_SUCCESS);
	}
#endif

	// Nothing for a refused argument, a check of Carrottus Files' own, or an
	// answer the platform gives as an outcome.
	seen.count = 0;
	CHECK_RESULT(crfiles_path_get_info(files, NULL, &info), CR_RESULT_FILES_NULL_PATH);
	crfiles_open_desc bad = open_desc(missing, CRFILES_ACCESS_NONE, CRFILES_CREATE_OPEN_EXISTING);
	CHECK_RESULT(crfiles_file_open(files, &bad, &file), CR_RESULT_DESC_INVALID_PARAMETERS);
	file = NULL;
	crfiles_open_desc reading = open_desc(plain, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	CHECK_RESULT(crfiles_file_open(files, &reading, &file), CR_SUCCESS);
	if (file) {
		CHECK_RESULT(crfiles_file_write_at(file, 0, "x", 1), CR_RESULT_FILES_NOT_WRITABLE);
		char buf[16];
		uint64_t count = 0;
		CHECK_RESULT(crfiles_file_read_at(file, 1000, buf, sizeof(buf), &count), CR_SUCCESS);
		CHECK(count == 0);
		CHECK_RESULT(crfiles_file_lock(file, CRFILES_LOCK_EXCLUSIVE, false), CR_SUCCESS);
		crfiles_file other = NULL;
		CHECK_RESULT(crfiles_file_open(files, &reading, &other), CR_SUCCESS);
		if (other) {
			CHECK_RESULT(crfiles_file_lock(other, CRFILES_LOCK_EXCLUSIVE, false), CR_RESULT_FILES_LOCKED);
			crfiles_file_close(other);
		}
		crfiles_file_close(file);
	}
	crfiles_listing_desc list;
	memset(&list, 0, sizeof(list));
	list.base.type           = CR_STRUCT_TYPE_FILES_LISTING_DESC;
	list.path                = g_dir;
	crfiles_listing listing = NULL;
	CHECK_RESULT(crfiles_listing_open(files, &list, &listing), CR_SUCCESS);
	if (listing) {
		CRFILES_LISTING_ENTRY entry;
		memset(&entry, 0, sizeof(entry));
		entry.base.type = CR_STRUCT_TYPE_FILES_LISTING_ENTRY;
		CR_RESULT r    = CR_SUCCESS;
		while ((r = crfiles_listing_next(listing, &entry)) == CR_SUCCESS)
			continue;
		CHECK(r == CR_RESULT_FILES_END_OF_LISTING);
		crfiles_listing_close(listing);
	}
#if CARROTTUS_PLATFORM_WIN32
	// Not WTF-8 (a lone continuation byte): no file can have that name, a
	// check of Carrottus Files' own, no platform call made.
	char not_wtf8[1100];
	path_in(not_wtf8, sizeof(not_wtf8), "bad\x80name");
	CHECK_RESULT(crfiles_path_get_info(files, not_wtf8, &info), CR_RESULT_FILES_NOT_FOUND);
#endif
	CHECK(seen.count == 0);

	// A file's call names the file, and its debug name.
	crfiles_open_desc named = open_desc(plain, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	named.debug_name         = "plain file";
	CHECK_RESULT(crfiles_file_open(files, &named, &file), CR_SUCCESS);
	CHECK(seen.count == 0);
	if (file)
		crfiles_file_close(file);

	// Sending allocates nothing: the same failing call costs the same
	// allocations with a callback and without.
	size_t before = tracker.asked;
	CHECK_RESULT(crfiles_path_get_info(quiet, missing, &info), CR_RESULT_FILES_NOT_FOUND);
	const size_t without = tracker.asked - before;
	seen.count           = 0;
	before               = tracker.asked;
	CHECK_RESULT(crfiles_path_get_info(files, missing, &info), CR_RESULT_FILES_NOT_FOUND);
	const size_t with = tracker.asked - before;
	CHECK(seen.count == 1);
	CHECK(with == without);

	// No callback: the same failures, the same results.
	CHECK_RESULT(crfiles_file_open(quiet, &open, &file), CR_RESULT_FILES_NOT_FOUND);

	CHECK_RESULT(crfiles_path_remove(files, plain), CR_SUCCESS);
	crfiles_instance_destroy(files);
	crfiles_instance_destroy(quiet);
	CHECK(crtest_allocator_clean(&tracker));
}

/* The callback from two threads at once (M3): each thread's failures sent
 * on its own thread, none lost. */
enum { THREADED_FAILURES = 200 };

typedef struct threaded_count {
	atomic_int sent;
	atomic_int wrong;
} threaded_count;

static void count_message(
    const CR_MESSAGE *message,
    void *user_data) {
	threaded_count *count = (threaded_count *)user_data;
	atomic_fetch_add(&count->sent, 1);
	if (message->result != CR_RESULT_FILES_NOT_FOUND || !message->call || strcmp(message->call, "crfiles_path_get_info") != 0)
		atomic_fetch_add(&count->wrong, 1);
}

struct failer {
	crfiles_instance files;
	const char *path;
	int refused;
};

#if CARROTTUS_PLATFORM_WIN32
static unsigned __stdcall fail_many(void *argument)
#else
static void *fail_many(void *argument)
#endif
{
	struct failer *f = (struct failer *)argument;
	for (int i = 0; i < THREADED_FAILURES; ++i) {
		CRFILES_PATH_INFO info;
		memset(&info, 0, sizeof(info));
		info.base.type = CR_STRUCT_TYPE_FILES_PATH_INFO;
		if (crfiles_path_get_info(f->files, f->path, &info) == CR_RESULT_FILES_NOT_FOUND)
			++f->refused;
	}
	return 0;
}

static void test_messages_two_threads(void) {
	threaded_count count;
	atomic_init(&count.sent, 0);
	atomic_init(&count.wrong, 0);
	CR_ALLOCATOR system   = cr_allocator_system();
	crfiles_instance files = make_messaged_instance(&system, count_message, &count);
	if (!files)
		return;
	char a_path[1100];
	char b_path[1100];
	struct failer a = {files, path_in(a_path, sizeof(a_path), "missing a"), 0};
	struct failer b = {files, path_in(b_path, sizeof(b_path), "missing b"), 0};
#if CARROTTUS_PLATFORM_WIN32
	HANDLE ta = (HANDLE)_beginthreadex(NULL, 0, fail_many, &a, 0, NULL);
	HANDLE tb = (HANDLE)_beginthreadex(NULL, 0, fail_many, &b, 0, NULL);
	WaitForSingleObject(ta, INFINITE);
	WaitForSingleObject(tb, INFINITE);
	CloseHandle(ta);
	CloseHandle(tb);
#else
	pthread_t ta;
	pthread_t tb;
	pthread_create(&ta, NULL, fail_many, &a);
	pthread_create(&tb, NULL, fail_many, &b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
#endif
	CHECK(a.refused == THREADED_FAILURES);
	CHECK(b.refused == THREADED_FAILURES);
	CHECK(atomic_load(&count.sent) == 2 * THREADED_FAILURES);
	CHECK(atomic_load(&count.wrong) == 0);
	crfiles_instance_destroy(files);
}

/* ---- Dynamic libraries (card S44) ---- */

typedef int (*test_int_function)(void);

static crfiles_library_desc library_desc(const char *path) {
	crfiles_library_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type    = CR_STRUCT_TYPE_FILES_LIBRARY_DESC;
	desc.path         = path;
	desc.binding      = CRFILES_LIBRARY_BINDING_NOW;
	desc.scope        = CRFILES_LIBRARY_SCOPE_LOCAL;
	desc.dependencies = CRFILES_LIBRARY_DEPENDENCIES_PLATFORM_ORDER;
	return desc;
}

/* [path] as the platform writes it: CMake's paths use '/', which
 * LoadLibraryExW reads as a relative name. */
static const char *native_path(
    char *buf,
    size_t cap,
    const char *path) {
	snprintf(buf, cap, "%s", path);
#if CARROTTUS_PLATFORM_WIN32
	for (char *at = buf; *at; ++at)
		if (*at == '/')
			*at = '\\';
#endif
	return buf;
}

static crfiles_library open_library(
    crfiles_instance files,
    const char *path) {
	const crfiles_library_desc desc = library_desc(path);
	crfiles_library library         = NULL;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_SUCCESS);
	return library;
}

static int call_int(
    crfiles_library library,
    const char *name) {
	crfiles_library_function function = NULL;
	CHECK_RESULT(crfiles_library_find(library, name, &function), CR_SUCCESS);
	if (!function)
		return -1;
	return ((test_int_function)function)();
}

/* Copies the file at [from] to [to], whole, through Carrottus Files. */
static void copy_file(
    const char *from,
    const char *to) {
	crfiles_file in  = open_file(from, CRFILES_ACCESS_READ, CRFILES_CREATE_OPEN_EXISTING);
	crfiles_file out = open_file(to, CRFILES_ACCESS_WRITE, CRFILES_CREATE_TRUNCATE);
	if (in && out) {
		static uint8_t buffer[1 << 16];
		uint64_t at = 0;
		for (;;) {
			uint64_t count = 0;
			CHECK_RESULT(crfiles_file_read_at(in, at, buffer, sizeof(buffer), &count), CR_SUCCESS);
			if (count == 0)
				break;
			CHECK_RESULT(crfiles_file_write_at(out, at, buffer, count), CR_SUCCESS);
			at += count;
		}
	}
	if (in)
		crfiles_file_close(in);
	if (out)
		crfiles_file_close(out);
}

#if CARROTTUS_PLATFORM_WIN32
#define LIBRARY_SUFFIX ".dll"
#define SYSTEM_LIBRARY "kernel32.dll"
#define SYSTEM_FUNCTION "GetCurrentProcessId"
#elif CARROTTUS_PLATFORM_APPLE
#define LIBRARY_SUFFIX ".dylib"
#define SYSTEM_LIBRARY "libSystem.B.dylib"
#define SYSTEM_FUNCTION "getpid"
#else
#define LIBRARY_SUFFIX ".so"
#define SYSTEM_LIBRARY "libc.so.6"
#define SYSTEM_FUNCTION "getpid"
#endif

#if CARROTTUS_PLATFORM_WIN32
#define LOADER_SOURCE CR_PLATFORM_ERROR_SOURCE_WIN32
#else
#define LOADER_SOURCE CR_PLATFORM_ERROR_SOURCE_DLERROR
#endif

/* A refused open: NOT_LOADED, nothing handed out, one message from the
 * loader. On POSIX its words name the file (dlerror's); on Windows the
 * number is GetLastError's [win32_code] and the text empty (S10's rule:
 * FormatMessage reads it). */
static void check_not_loaded(
    crfiles_instance files,
    recorder *seen,
    const char *path,
    const char *named,
    long win32_code) {
	crfiles_library_desc desc = library_desc(path);
	desc.debug_name            = "refused";
	crfiles_library library   = (crfiles_library)(uintptr_t)1;
	seen->count                = 0;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_FILES_LIBRARY_NOT_LOADED);
	CHECK(library == NULL);
	check_platform_message(seen, "crfiles_library_open", files, CR_RESULT_FILES_LIBRARY_NOT_LOADED, LOADER_SOURCE);
	CHECK(strcmp(seen->last.subject_name, "refused") == 0);
#if CARROTTUS_PLATFORM_WIN32
	(void)named;
	CHECK(seen->last.code == win32_code);
	CHECK(seen->last.text[0] == 0);
#else
	(void)win32_code;
	CHECK(seen->last.code == 0);
	CHECK(strstr(seen->last.text, named) != NULL);
#endif
}

static void test_library_checks(crfiles_instance files) {
	char a[1100];
	native_path(a, sizeof(a), CRFILES_TEST_LIBRARY_A);
	crfiles_library_desc desc = library_desc(a);
	crfiles_library library   = (crfiles_library)(uintptr_t)1;
	CHECK_RESULT(crfiles_library_open(NULL, &desc, &library), CR_RESULT_FILES_INVALID_INSTANCE);
	CHECK_RESULT(crfiles_library_open(files, NULL, &library), CR_RESULT_NULL_DESC);
	desc.base.type = CR_STRUCT_TYPE_FILES_OPEN_DESC;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_FILES_LIBRARY_DESC;
	desc.base.next = (const CR_STRUCT_BASE *)&desc;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;
	CHECK_RESULT(crfiles_library_open(files, &desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	desc.path = NULL;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_FILES_NULL_PATH);
	CHECK(library == NULL);
	desc.path = "";
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_FILES_NULL_PATH);
	desc.path = a;
	// Each enum's zero and a value past its last.
	const uint32_t wrong[2] = {0, 3};
	for (int i = 0; i < 2; ++i) {
		desc         = library_desc(a);
		desc.binding = (CRFILES_LIBRARY_BINDING)wrong[i];
		CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
		desc       = library_desc(a);
		desc.scope = (CRFILES_LIBRARY_SCOPE)wrong[i];
		CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
		desc              = library_desc(a);
		desc.dependencies = (CRFILES_LIBRARY_DEPENDENCIES)wrong[i];
		CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
	}
	// What the platform's loader has no choice of.
#if CARROTTUS_PLATFORM_WIN32
	desc         = library_desc(a);
	desc.binding = CRFILES_LIBRARY_BINDING_LAZY;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED);
	desc       = library_desc(a);
	desc.scope = CRFILES_LIBRARY_SCOPE_GLOBAL;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED);
	// The library's directory first needs a fully qualified path.
	desc              = library_desc("crfiles_test_library_a.dll");
	desc.dependencies = CRFILES_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.path = "sub\\crfiles_test_library_a.dll";
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.path = "C:crfiles_test_library_a.dll";
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.path = "\\crfiles_test_library_a.dll";
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_DESC_INVALID_PARAMETERS);
	// Fully qualified, it opens, its dependencies searched from its own
	// directory.
	desc.path = a;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_SUCCESS);
	if (library) {
		CHECK(call_int(library, "crfiles_test_value") == 1);
		crfiles_library_close(library);
	}
#else
	desc              = library_desc(a);
	desc.dependencies = CRFILES_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED);
	// dlopen's other two choices taken: lazy binding, global scope, and both.
	const CRFILES_LIBRARY_BINDING bindings[3] = {CRFILES_LIBRARY_BINDING_LAZY, CRFILES_LIBRARY_BINDING_NOW, CRFILES_LIBRARY_BINDING_LAZY};
	const CRFILES_LIBRARY_SCOPE scopes[3]     = {CRFILES_LIBRARY_SCOPE_LOCAL, CRFILES_LIBRARY_SCOPE_GLOBAL, CRFILES_LIBRARY_SCOPE_GLOBAL};
	for (int i = 0; i < 3; ++i) {
		desc         = library_desc(a);
		desc.binding = bindings[i];
		desc.scope   = scopes[i];
		library      = NULL;
		CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_SUCCESS);
		if (library) {
			CHECK(call_int(library, "crfiles_test_value") == 1);
			crfiles_library_close(library);
		}
	}
	library = NULL;
#endif
	CHECK(library == NULL || library != (crfiles_library)(uintptr_t)1);

	// find's arguments.
	library = open_library(files, a);
	if (!library)
		return;
	crfiles_library_function function = (crfiles_library_function)(uintptr_t)1;
	CHECK_RESULT(crfiles_library_find(NULL, "crfiles_test_value", &function), CR_RESULT_FILES_INVALID_LIBRARY);
	CHECK_RESULT(crfiles_library_find(library, NULL, &function), CR_RESULT_NULL_DESC);
	CHECK_RESULT(crfiles_library_find(library, "", &function), CR_RESULT_NULL_DESC);
	CHECK_RESULT(crfiles_library_find(library, "crfiles_test_value", NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	crfiles_library_close(library);
	crfiles_library_close(NULL);
}

static void test_libraries(void) {
	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
	recorder seen;
	memset(&seen, 0, sizeof(seen));
	crfiles_instance files = make_messaged_instance(&allocator, record_message, &seen);
	if (!files)
		return;
	test_library_checks(files);
	CHECK(seen.count == 0);

	char a[1100];
	char b[1100];
	native_path(a, sizeof(a), CRFILES_TEST_LIBRARY_A);
	native_path(b, sizeof(b), CRFILES_TEST_LIBRARY_B);

	// Opened, found, called; the initializer ran once; closed.
	crfiles_library_desc desc = library_desc(a);
	desc.debug_name            = "library a";
	crfiles_library library   = NULL;
	CHECK_RESULT(crfiles_library_open(files, &desc, &library), CR_SUCCESS);
	if (library) {
		CHECK(strcmp(cr_debug_name_get(library), "library a") == 0);
		CHECK(call_int(library, "crfiles_test_value") == 1);
		CHECK(call_int(library, "crfiles_test_initialized") == 1);
		// A name it doesn't export: refused, with the loader's message.
		crfiles_library_function function = (crfiles_library_function)(uintptr_t)1;
		seen.count                         = 0;
		CHECK_RESULT(crfiles_library_find(library, "crfiles_test_nothing", &function), CR_RESULT_FILES_SYMBOL_NOT_FOUND);
		CHECK(function == NULL);
		check_platform_message(&seen, "crfiles_library_find", library, CR_RESULT_FILES_SYMBOL_NOT_FOUND, LOADER_SOURCE);
		CHECK(strcmp(seen.last.object_name, "library a") == 0);
#if CARROTTUS_PLATFORM_WIN32
		CHECK(seen.last.code == ERROR_PROC_NOT_FOUND);
#else
		CHECK(strstr(seen.last.text, "crfiles_test_nothing") != NULL);
#endif
		crfiles_library_close(library);
	}

	// One path opened twice: two handles over one image, each closed on its
	// own.
	crfiles_library first  = open_library(files, a);
	crfiles_library second = open_library(files, a);
	if (first && second) {
		CHECK(first != second);
		CHECK(call_int(first, "crfiles_test_bump") == 1);
		CHECK(call_int(second, "crfiles_test_bump") == 2);
		CHECK(call_int(second, "crfiles_test_initialized") == 1);
		crfiles_library_close(first);
		first = NULL;
		CHECK(call_int(second, "crfiles_test_bump") == 3);
	}
	crfiles_library_close(first);
	crfiles_library_close(second);

	// Two copies under two paths: two images; a copy of b beside a.
	char copy_one[1100];
	char copy_two[1100];
	char copy_b[1100];
	path_in(copy_one, sizeof(copy_one), "copy one" LIBRARY_SUFFIX);
	path_in(copy_two, sizeof(copy_two), "copy two" LIBRARY_SUFFIX);
	path_in(copy_b, sizeof(copy_b), "copy b" LIBRARY_SUFFIX);
	copy_file(a, copy_one);
	copy_file(a, copy_two);
	copy_file(b, copy_b);
	first                   = open_library(files, copy_one);
	second                  = open_library(files, copy_two);
	crfiles_library beside = open_library(files, copy_b);
	if (first && second && beside) {
		CHECK(call_int(first, "crfiles_test_bump") == 1);
		CHECK(call_int(second, "crfiles_test_bump") == 1);
		CHECK(call_int(first, "crfiles_test_bump") == 2);
		CHECK(call_int(first, "crfiles_test_value") == 1);
		CHECK(call_int(beside, "crfiles_test_value") == 2);
		CHECK(call_int(second, "crfiles_test_initialized") == 1);
	}
	crfiles_library_close(first);
	crfiles_library_close(second);
	crfiles_library_close(beside);

#if CARROTTUS_PLATFORM_WIN32
	// The original replaced while its copy is loaded: the copy holds only
	// its own path.
	char original[1100];
	char live[1100];
	path_in(original, sizeof(original), "original" LIBRARY_SUFFIX);
	path_in(live, sizeof(live), "live" LIBRARY_SUFFIX);
	copy_file(a, original);
	copy_file(original, live);
	library = open_library(files, live);
	if (library) {
		char staged[1100];
		path_in(staged, sizeof(staged), "staged" LIBRARY_SUFFIX);
		copy_file(b, staged);
		CHECK_RESULT(crfiles_path_replace(files, staged, original, CRFILES_FLUSH_LEVEL_NONE), CR_SUCCESS);
		CHECK(call_int(library, "crfiles_test_value") == 1);
		crfiles_library rebuilt = open_library(files, original);
		if (rebuilt) {
			CHECK(call_int(rebuilt, "crfiles_test_value") == 2);
			crfiles_library_close(rebuilt);
		}
		// The loaded copy's own file is held while it is loaded.
		CHECK_RESULT(crfiles_path_remove(files, live), CR_RESULT_FILES_IN_USE);
		crfiles_library_close(library);
	}
	CHECK_RESULT(crfiles_path_remove(files, live), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(files, original), CR_SUCCESS);
#endif

	// Refusals, each NOT_LOADED with one message.
	char missing[1100];
	path_in(missing, sizeof(missing), "missing" LIBRARY_SUFFIX);
	check_not_loaded(files, &seen, missing, "missing", 126 /* ERROR_MOD_NOT_FOUND */);
	char text[1100];
	path_in(text, sizeof(text), "text" LIBRARY_SUFFIX);
	write_whole(text, "not a library, only text with the library's extension\n");
	check_not_loaded(files, &seen, text, "text", 193 /* ERROR_BAD_EXE_FORMAT */);
	CHECK_RESULT(crfiles_path_remove(files, text), CR_SUCCESS);
	char dependent[1100];
	native_path(dependent, sizeof(dependent), CRFILES_TEST_LIBRARY_DEPENDENT);
	check_not_loaded(files, &seen, dependent, "crfiles_test_dependency", 126 /* ERROR_MOD_NOT_FOUND */);
#if CARROTTUS_PLATFORM_WIN32
	char failing[1100];
	native_path(failing, sizeof(failing), CRFILES_TEST_LIBRARY_FAILING);
	check_not_loaded(files, &seen, failing, "failing", 1114 /* ERROR_DLL_INIT_FAILED */);
#endif

	// A name with no directory, searched for by the platform.
	library = open_library(files, SYSTEM_LIBRARY);
	if (library) {
		crfiles_library_function function = NULL;
		CHECK_RESULT(crfiles_library_find(library, SYSTEM_FUNCTION, &function), CR_SUCCESS);
		CHECK(function != NULL);
		crfiles_library_close(library);
	}

#if defined(CRFILES_TEST_FRAMEWORK)
	// iOS: a framework beside the test, as an app's bundle holds one (Q5a),
	// opened by the path of its binary inside it.
	library = open_library(files, CRFILES_TEST_FRAMEWORK);
	if (library) {
		CHECK(strstr(CRFILES_TEST_FRAMEWORK, ".framework/") != NULL);
		CHECK(call_int(library, "crfiles_test_value") == 5);
		CHECK(call_int(library, "crfiles_test_initialized") == 1);
		crfiles_library_close(library);
	}
#endif

	// A non-ASCII directory and file name.
	char dir[1100];
	char unicode[1300];
	path_in(dir, sizeof(dir), "d\xC3\xAFr \xE5\x9B\xB3"); // "dïr 図"
	CHECK_RESULT(crfiles_directory_create(files, dir), CR_SUCCESS);
	snprintf(unicode, sizeof(unicode), "%s" SLASH "l\xC3\xAF" "b" LIBRARY_SUFFIX, dir);
	copy_file(b, unicode);
	library = open_library(files, unicode);
	if (library) {
		CHECK(call_int(library, "crfiles_test_value") == 2);
		crfiles_library_close(library);
	}
	CHECK_RESULT(crfiles_path_remove(files, unicode), CR_SUCCESS);
	CHECK_RESULT(crfiles_directory_remove(files, dir), CR_SUCCESS);

#if CARROTTUS_PLATFORM_WIN32
	// Long paths. Carrottus Files takes 259 characters, MAX_PATH's most, and the
	// file calls do; Windows' loader stops at 250 (ERROR_FILENAME_EXCED_RANGE
	// from 251, with either search), and nothing is added to the path to get
	// past it, so 250 loads and 259 is the loader's refusal.
	const size_t lengths[2] = {250, 259};
	for (int i = 0; i < 2; ++i) {
		char longest[300];
		const size_t base = strlen(g_dir) + 1;
		if (base + strlen(LIBRARY_SUFFIX) + 1 >= lengths[i])
			break;
		const size_t name = lengths[i] - base - strlen(LIBRARY_SUFFIX);
		snprintf(longest, sizeof(longest), "%s" SLASH, g_dir);
		memset(longest + base, 'l', name);
		snprintf(longest + base + name, sizeof(longest) - base - name, "%s", LIBRARY_SUFFIX);
		CHECK(strlen(longest) == lengths[i]);
		copy_file(a, longest);
		if (lengths[i] == 250) {
			library = open_library(files, longest);
			if (library) {
				CHECK(call_int(library, "crfiles_test_value") == 1);
				crfiles_library_close(library);
			}
		} else {
			check_not_loaded(files, &seen, longest, "", 206 /* ERROR_FILENAME_EXCED_RANGE */);
		}
		CHECK_RESULT(crfiles_path_remove(files, longest), CR_SUCCESS);
	}
#endif

	// Out of memory at each allocation: refused, the library not left
	// loaded (its counter starts again at the next open that succeeds).
	desc            = library_desc(copy_one);
	desc.debug_name = "counted";
	for (size_t point = 0;; ++point) {
		tracker.refuse_after    = tracker.given + point;
		library                 = (crfiles_library)(uintptr_t)1;
		const CR_RESULT result = crfiles_library_open(files, &desc, &library);
		tracker.refuse_after    = SIZE_MAX;
		if (result == CR_SUCCESS) {
			CHECK(point > 0);
			CHECK(call_int(library, "crfiles_test_bump") == 1);
			crfiles_library_close(library);
			break;
		}
		CHECK(result == CR_RESULT_OUT_OF_MEMORY);
		CHECK(library == NULL);
		if (point > 16) {
			CHECK(!"open never succeeded");
			break;
		}
	}

	CHECK_RESULT(crfiles_path_remove(files, copy_one), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(files, copy_two), CR_SUCCESS);
	CHECK_RESULT(crfiles_path_remove(files, copy_b), CR_SUCCESS);
	crfiles_instance_destroy(files);
	CHECK(crtest_allocator_clean(&tracker));
}

/* Two threads opening, finding, calling and closing at once. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) && CARROTTUS_PLATFORM_APPLE
#define CRFILES_TEST_PIN_LIBRARIES 1
#endif
#endif
#ifndef CRFILES_TEST_PIN_LIBRARIES
#define CRFILES_TEST_PIN_LIBRARIES 0
#endif
enum { LIBRARY_ROUNDS = 50 };

struct library_user {
	crfiles_instance files;
	const char *path;
	int value;
	int ok;
};

#if CARROTTUS_PLATFORM_WIN32
static unsigned __stdcall use_library(void *argument)
#else
static void *use_library(void *argument)
#endif
{
	struct library_user *user = (struct library_user *)argument;
	for (int i = 0; i < LIBRARY_ROUNDS; ++i) {
		const crfiles_library_desc desc = library_desc(user->path);
		crfiles_library library         = NULL;
		if (crfiles_library_open(user->files, &desc, &library) != CR_SUCCESS)
			continue;
		crfiles_library_function function = NULL;
		if (crfiles_library_find(library, "crfiles_test_value", &function) == CR_SUCCESS && ((test_int_function)function)() == user->value)
			++user->ok;
		crfiles_library_close(library);
	}
	return 0;
}

static void test_libraries_two_threads(void) {
	char a[1100];
	char b[1100];
	native_path(a, sizeof(a), CRFILES_TEST_LIBRARY_A);
	native_path(b, sizeof(b), CRFILES_TEST_LIBRARY_B);
	// The system's allocator: the counting one takes one thread at a time.
	CR_ALLOCATOR system   = cr_allocator_system();
	crfiles_instance files = make_messaged_instance(&system, NULL, NULL);
	if (!files)
		return;
	struct library_user one = {files, a, 1, 0};
	struct library_user two = {files, b, 2, 0};
#if CRFILES_TEST_PIN_LIBRARIES
	// ThreadSanitizer's own dlopen on Apple lists the loaded images, and
	// crashes when another thread's dlclose unmaps one meanwhile (seen
	// 2026-11-09 with Apple clang 21 in a program of two threads calling
	// dlopen and dlclose alone, no Carrottus in it; never without TSan). One
	// more open of each here keeps every image mapped, so no close unmaps
	// one: the threads still open, find and close at once through Carrottus.
	crfiles_library pin_a = open_library(files, a);
	crfiles_library pin_b = open_library(files, b);
#endif
#if CARROTTUS_PLATFORM_WIN32
	HANDLE ta = (HANDLE)_beginthreadex(NULL, 0, use_library, &one, 0, NULL);
	HANDLE tb = (HANDLE)_beginthreadex(NULL, 0, use_library, &two, 0, NULL);
	WaitForSingleObject(ta, INFINITE);
	WaitForSingleObject(tb, INFINITE);
	CloseHandle(ta);
	CloseHandle(tb);
#else
	pthread_t ta;
	pthread_t tb;
	pthread_create(&ta, NULL, use_library, &one);
	pthread_create(&tb, NULL, use_library, &two);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
#endif
	CHECK(one.ok == LIBRARY_ROUNDS);
	CHECK(two.ok == LIBRARY_ROUNDS);
#if CRFILES_TEST_PIN_LIBRARIES
	crfiles_library_close(pin_a);
	crfiles_library_close(pin_b);
#endif
	crfiles_instance_destroy(files);
}

int main(void) {
#if CARROTTUS_PLATFORM_WIN32
	// A library whose dependency is missing would otherwise put up the
	// system's modal "not found" box and wait for someone to close it: the
	// error mode is the caller's (crfiles.h, "Dynamic libraries"), and a
	// test has no one to click.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	const char *tmp = getenv("TEMP");
	if (!tmp)
		tmp = ".";
	snprintf(g_dir, sizeof(g_dir), "%s\\crfiles_test.%u", tmp, (unsigned)_getpid());
	if (_mkdir(g_dir) != 0) {
		printf("crfiles_test: skipped, no temporary directory\n");
		return EXIT_SKIPPED;
	}
#else
	const char *tmp = getenv("TMPDIR");
	if (!tmp || !tmp[0])
		tmp = "/tmp";
	snprintf(g_dir, sizeof(g_dir), "%s%scrfiles_test.XXXXXX", tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/");
	if (!mkdtemp(g_dir)) {
		printf("crfiles_test: skipped, no temporary directory\n");
		return EXIT_SKIPPED;
	}
#endif
	printf("crfiles_test: in %s\n", g_dir);

	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
	test_instance(&allocator);
	crfiles_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator = &allocator;
	CHECK_RESULT(crfiles_instance_create(&desc, &g_files), CR_SUCCESS);
	if (g_files) {
		test_open_checks();
		test_quarantine();
		test_create_modes();
		test_read_write();
		test_map();
		test_paths();
		test_unicode_name();
		test_two_readers();
		test_listing_checks();
		test_listing_kinds();
		test_listing_non_utf8();
		test_directory_remove();
		test_directory_rename();
		test_directory_tree();
		test_copy_mode();
		test_locks();
		test_messages();
		test_messages_two_threads();
		test_libraries();
		test_libraries_two_threads();
		crfiles_instance_destroy(g_files);
	}
	CHECK(crtest_allocator_clean(&tracker));

	CHECK(rmdir(g_dir) == 0);
	if (failures) {
		fprintf(stderr, "crfiles_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("crfiles_test: all passed\n");
	return 0;
}
