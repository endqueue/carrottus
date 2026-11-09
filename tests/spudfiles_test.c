/*
 * spudfiles_test — SpudFiles, in a temporary directory.
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
 * a second thread. Every block
 * goes through a counting allocator, checked at the end. Needs nothing but
 * a writable temporary directory.
 *
 * Exits 0 when every check passes, 1 when one fails, 77 (skipped) with no
 * temporary directory.
 */

#include <spudfiles.h>

#include "spudtest_allocator.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SPUDLIB_PLATFORM_WIN32
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
		const SPUDRESULT r_ = (call);                                                                                        \
		if (r_ != (expected)) {                                                                                              \
			fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, #call, spudresult_str(r_), #expected); \
			++failures;                                                                                                      \
		}                                                                                                                    \
	} while (0)

static char g_dir[1024];
static spudfile_instance g_files;

static const char *path_in(
    char *buf,
    size_t cap,
    const char *name) {
	snprintf(buf, cap, "%s" SLASH "%s", g_dir, name);
	return buf;
}

static spudfile_open_desc open_desc(
    const char *path,
    SPUDFILE_ACCESS access,
    SPUDFILE_CREATE create) {
	spudfile_open_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_FILES_OPEN_DESC;
	desc.path      = path;
	desc.access    = access;
	desc.create    = create;
	desc.share     = SPUDFILE_SHARE_READ | SPUDFILE_SHARE_WRITE | SPUDFILE_SHARE_DELETE;
	return desc;
}

static spudfile_file open_file(
    const char *path,
    SPUDFILE_ACCESS access,
    SPUDFILE_CREATE create) {
	const spudfile_open_desc desc = open_desc(path, access, create);
	spudfile_file file            = NULL;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUD_SUCCESS);
	return file;
}

static void write_whole(
    const char *path,
    const char *text) {
	spudfile_file file = open_file(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_TRUNCATE);
	if (!file)
		return;
	CHECK_RESULT(spudfile_file_write_at(file, 0, text, strlen(text)), SPUD_SUCCESS);
	spudfile_file_close(file);
}

static int holds(
    const char *path,
    const char *text) {
	spudfile_file file = open_file(path, SPUDFILE_ACCESS_READ, SPUDFILE_CREATE_OPEN_EXISTING);
	if (!file)
		return 0;
	char buf[256]  = {0};
	uint64_t count = 0;
	CHECK_RESULT(spudfile_file_read_at(file, 0, buf, sizeof(buf) - 1, &count), SPUD_SUCCESS);
	spudfile_file_close(file);
	return count == strlen(text) && memcmp(buf, text, (size_t)count) == 0;
}

static SPUDFILE_PATH_INFO info_of(
    const char *path,
    SPUDRESULT *out_result) {
	SPUDFILE_PATH_INFO info;
	memset(&info, 0, sizeof(info));
	info.base.type = SPUD_STRUCT_TYPE_FILES_PATH_INFO;
	*out_result    = spudfile_path_get_info(g_files, path, &info);
	return info;
}

static void test_instance(const SPUD_ALLOCATOR *allocator) {
	spudfile_instance instance = (spudfile_instance)(uintptr_t)1;
	spudfile_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	CHECK_RESULT(spudfile_instance_create(NULL, &instance), SPUDRESULT_NULL_DESC);
	CHECK_RESULT(spudfile_instance_create(&desc, &instance), SPUDRESULT_STRUCT_WRONG_TYPE);
	desc.base.type = SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.base.next = (const SPUD_STRUCT_BASE *)&desc;
	CHECK_RESULT(spudfile_instance_create(&desc, &instance), SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;
	CHECK_RESULT(spudfile_instance_create(&desc, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(spudfile_instance_create(&desc, &instance), SPUDRESULT_NULL_ALLOCATOR);
	CHECK(instance == NULL);
	SPUD_ALLOCATOR missing = *allocator;
	missing.allocate       = NULL;
	desc.allocator         = &missing;
	CHECK_RESULT(spudfile_instance_create(&desc, &instance), SPUDRESULT_INVALID_ALLOCATOR);
	spudfile_instance_destroy(NULL);
}

/* Card X27 (spudlib's src/spudstale.h): in a Debug build a closed file's
 * memory stays its instance's until the instance is destroyed, so the
 * destroyed-handle check reads SPUD_DEAD however many blocks of the same
 * size are allocated after it, and destroying the instance frees every one.
 * Its own instance and counting allocator, to count exactly what the
 * instance holds. Debug only: Release has no check, and frees at once. */
static void test_quarantine(void) {
#if defined(_DEBUG) && _DEBUG
	enum { ROUNDS = 256, KEPT_OPEN = 64 };
	spudtest_allocator tracker;
	SPUD_ALLOCATOR allocator = spudtest_allocator_make(&tracker);
	spudfile_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator = &allocator;
	spudfile_instance instance = NULL;
	CHECK_RESULT(spudfile_instance_create(&desc, &instance), SPUD_SUCCESS);
	if (!instance)
		return;
	const size_t instance_blocks = tracker.live_blocks;

	char path[1100];
	path_in(path, sizeof(path), "quarantine");
	spudfile_open_desc open = open_desc(path, SPUDFILE_ACCESS_READ_WRITE, SPUDFILE_CREATE_TRUNCATE);
	spudfile_file dead      = NULL;
	CHECK_RESULT(spudfile_file_open(instance, &open, &dead), SPUD_SUCCESS);
	if (!dead) {
		spudfile_instance_destroy(instance);
		return;
	}
	spudfile_file_close(dead);
	CHECK(tracker.live_blocks == instance_blocks + 1);

	/* Blocks of the same size, made and freed again and again: an allocator
	 * that reused the closed file's block would hand it to one of these. */
	open.create = SPUDFILE_CREATE_OPEN_EXISTING;
	for (int i = 0; i < ROUNDS; ++i) {
		spudfile_file file = NULL;
		CHECK_RESULT(spudfile_file_open(instance, &open, &file), SPUD_SUCCESS);
		CHECK(file != dead);
		spudfile_file_close(file);
	}
	spudfile_file open_files[KEPT_OPEN];
	for (int i = 0; i < KEPT_OPEN; ++i) {
		open_files[i] = NULL;
		CHECK_RESULT(spudfile_file_open(instance, &open, &open_files[i]), SPUD_SUCCESS);
		CHECK(open_files[i] != dead);
	}
	uint64_t size = 0;
	CHECK_RESULT(spudfile_file_get_size(dead, &size), SPUDRESULT_SPUDFILE_INVALID_FILE);
	spudfile_file_close(dead);
	for (int i = 0; i < KEPT_OPEN; ++i)
		spudfile_file_close(open_files[i]);

	/* Every closed file is still the instance's, then none is. */
	CHECK(tracker.live_blocks == instance_blocks + 1 + ROUNDS + KEPT_OPEN);
	spudfile_instance_destroy(instance);
	CHECK(spudtest_allocator_clean(&tracker));
	CHECK(remove(path) == 0);
#endif
}

static void test_open_checks(void) {
	char path[1100];
	path_in(path, sizeof(path), "checks.txt");
	spudfile_file file      = (spudfile_file)(uintptr_t)1;
	spudfile_open_desc desc = open_desc(path, SPUDFILE_ACCESS_READ, SPUDFILE_CREATE_OPEN_EXISTING);

	CHECK_RESULT(spudfile_file_open(NULL, &desc, &file), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_file_open(g_files, NULL, &file), SPUDRESULT_NULL_DESC);
	desc.base.type = SPUD_STRUCT_TYPE_FILES_PATH_INFO;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_STRUCT_WRONG_TYPE);
	desc.base.type = SPUD_STRUCT_TYPE_FILES_OPEN_DESC;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	desc.path = NULL;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK(file == NULL);
	desc.path = "";
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_NULL_PATH);
	desc.path   = path;
	desc.access = SPUDFILE_ACCESS_NONE;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.access = (SPUDFILE_ACCESS)9;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.access = SPUDFILE_ACCESS_READ;
	desc.create = SPUDFILE_CREATE_NONE;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.create = (SPUDFILE_CREATE)9;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.create = SPUDFILE_CREATE_OPEN_EXISTING;
	desc.share  = 0x80;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.share  = SPUDFILE_SHARE_NONE;
	desc.create = SPUDFILE_CREATE_TRUNCATE; // emptying a file opened only to read
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_DESC_INVALID_PARAMETERS);

	// Nothing there yet.
	desc.create = SPUDFILE_CREATE_OPEN_EXISTING;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_NOT_FOUND);
	char deeper[1200];
	snprintf(deeper, sizeof(deeper), "%s" SLASH "missing" SLASH "x.txt", g_dir);
	desc.path   = deeper;
	desc.access = SPUDFILE_ACCESS_WRITE;
	desc.create = SPUDFILE_CREATE_OPEN_OR_CREATE;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_NOT_FOUND);
	// A directory isn't a file.
	desc.path   = g_dir;
	desc.access = SPUDFILE_ACCESS_READ;
	desc.create = SPUDFILE_CREATE_OPEN_EXISTING;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_NOT_A_FILE);
	CHECK(file == NULL);
	spudfile_file_close(NULL);
}

static void test_create_modes(void) {
	char path[1100];
	path_in(path, sizeof(path), "modes.txt");

	// NEW makes it, and refuses a second time.
	spudfile_file file = open_file(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_NEW);
	if (file) {
		CHECK_RESULT(spudfile_file_write_at(file, 0, "first", 5), SPUD_SUCCESS);
		spudfile_file_close(file);
	}
	spudfile_open_desc desc = open_desc(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_NEW);
	file               = NULL;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_ALREADY_EXISTS);
	CHECK(holds(path, "first"));

	// OPEN_OR_CREATE keeps what's there.
	file = open_file(path, SPUDFILE_ACCESS_READ_WRITE, SPUDFILE_CREATE_OPEN_OR_CREATE);
	if (file) {
		CHECK_RESULT(spudfile_file_write_at(file, 0, "F", 1), SPUD_SUCCESS);
		spudfile_file_close(file);
	}
	CHECK(holds(path, "First"));
	// TRUNCATE empties it.
	file = open_file(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_TRUNCATE);
	if (file) {
		uint64_t size = 99;
		CHECK_RESULT(spudfile_file_get_size(file, &size), SPUD_SUCCESS);
		CHECK(size == 0);
		spudfile_file_close(file);
	}
	// OPEN_OR_CREATE makes one too.
	char other[1100];
	path_in(other, sizeof(other), "created.txt");
	file = open_file(other, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_OPEN_OR_CREATE);
	spudfile_file_close(file);
	SPUDRESULT r;
	SPUDFILE_PATH_INFO info = info_of(other, &r);
	CHECK(r == SPUD_SUCCESS && info.kind == SPUDFILE_PATH_KIND_FILE && info.size == 0);
	CHECK_RESULT(spudfile_path_remove(g_files, other), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
}

static void test_read_write(void) {
	char path[1100];
	path_in(path, sizeof(path), "rw.bin");
	spudfile_open_desc desc = open_desc(path, SPUDFILE_ACCESS_READ_WRITE, SPUDFILE_CREATE_TRUNCATE);
	desc.debug_name    = "rw file";
	spudfile_file file      = NULL;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUD_SUCCESS);
	if (!file)
		return;
	CHECK(spud_debug_name_get(file) && strcmp(spud_debug_name_get(file), "rw file") == 0);

	uint8_t bytes[1000];
	for (int i = 0; i < 1000; ++i)
		bytes[i] = (uint8_t)(i * 7);
	CHECK_RESULT(spudfile_file_write_at(file, 0, bytes, sizeof(bytes)), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_write_at(file, 0, NULL, 0), SPUD_SUCCESS);
	uint64_t value = 0;
	CHECK_RESULT(spudfile_file_get_size(file, &value), SPUD_SUCCESS);
	CHECK(value == 1000);

	uint8_t back[200];
	uint64_t count = 0;
	CHECK_RESULT(spudfile_file_read_at(file, 400, back, sizeof(back), &count), SPUD_SUCCESS);
	CHECK(count == 200 && memcmp(back, bytes + 400, 200) == 0);
	// The file ends first: what there was, then nothing at or past the end.
	CHECK_RESULT(spudfile_file_read_at(file, 900, back, sizeof(back), &count), SPUD_SUCCESS);
	CHECK(count == 100 && memcmp(back, bytes + 900, 100) == 0);
	CHECK_RESULT(spudfile_file_read_at(file, 1000, back, sizeof(back), &count), SPUD_SUCCESS);
	CHECK(count == 0);
	CHECK_RESULT(spudfile_file_read_at(file, 5000, back, sizeof(back), &count), SPUD_SUCCESS);
	CHECK(count == 0);

	// Over the middle changes only those bytes.
	const uint8_t patch[4] = {1, 2, 3, 4};
	CHECK_RESULT(spudfile_file_write_at(file, 10, patch, sizeof(patch)), SPUD_SUCCESS);
	uint8_t around[8];
	CHECK_RESULT(spudfile_file_read_at(file, 8, around, sizeof(around), &count), SPUD_SUCCESS);
	CHECK(around[0] == bytes[8] && around[1] == bytes[9] && memcmp(around + 2, patch, 4) == 0 && around[6] == bytes[14]);

	// Past the end grows the file; the gap reads zero.
	CHECK_RESULT(spudfile_file_write_at(file, 1100, patch, sizeof(patch)), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_get_size(file, &value), SPUD_SUCCESS);
	CHECK(value == 1104);
	uint8_t gap[128];
	memset(gap, 0xAA, sizeof(gap));
	CHECK_RESULT(spudfile_file_read_at(file, 1000, gap, 100, &count), SPUD_SUCCESS);
	int zeros = count == 100;
	for (int i = 0; i < 100; ++i)
		zeros = zeros && gap[i] == 0;
	CHECK(zeros);

	// Sizing: shorter, then longer with zeros.
	CHECK_RESULT(spudfile_file_set_size(file, 500), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_get_size(file, &value), SPUD_SUCCESS);
	CHECK(value == 500);
	CHECK_RESULT(spudfile_file_set_size(file, 600), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_read_at(file, 499, gap, 101, &count), SPUD_SUCCESS);
	CHECK(count == 101 && gap[0] == bytes[499] && gap[1] == 0 && gap[100] == 0);

	CHECK_RESULT(spudfile_file_flush(file, SPUDFILE_FLUSH_LEVEL_DEVICE), SPUD_SUCCESS);
	const SPUDRESULT media = spudfile_file_flush(file, SPUDFILE_FLUSH_LEVEL_MEDIA);
	CHECK(media == SPUD_SUCCESS || media == SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED);
	CHECK_RESULT(spudfile_file_flush(file, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_DESC_INVALID_PARAMETERS);

	// Argument checks.
	CHECK_RESULT(spudfile_file_read_at(NULL, 0, back, 1, &count), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK_RESULT(spudfile_file_read_at(file, 0, NULL, 1, &count), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(spudfile_file_read_at(file, 0, back, 1, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(spudfile_file_write_at(NULL, 0, back, 1), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK_RESULT(spudfile_file_write_at(file, 0, NULL, 1), SPUDRESULT_NULL_DESC);
	CHECK_RESULT(spudfile_file_get_size(NULL, &value), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK_RESULT(spudfile_file_get_size(file, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(spudfile_file_set_size(NULL, 0), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK_RESULT(spudfile_file_flush(NULL, SPUDFILE_FLUSH_LEVEL_DEVICE), SPUDRESULT_SPUDFILE_INVALID_FILE);
	spudfile_file_close(file);

	// What each access refuses.
	file = open_file(path, SPUDFILE_ACCESS_READ, SPUDFILE_CREATE_OPEN_EXISTING);
	if (file) {
		CHECK_RESULT(spudfile_file_write_at(file, 0, patch, 1), SPUDRESULT_SPUDFILE_NOT_WRITABLE);
		CHECK_RESULT(spudfile_file_set_size(file, 0), SPUDRESULT_SPUDFILE_NOT_WRITABLE);
		CHECK_RESULT(spudfile_file_flush(file, SPUDFILE_FLUSH_LEVEL_DEVICE), SPUDRESULT_SPUDFILE_NOT_WRITABLE);
		spudfile_file_close(file);
	}
	file = open_file(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_OPEN_EXISTING);
	if (file) {
		CHECK_RESULT(spudfile_file_read_at(file, 0, back, 1, &count), SPUDRESULT_SPUDFILE_NOT_READABLE);
		spudfile_mapping mapping = NULL;
		CHECK_RESULT(spudfile_file_map(file, 0, 1, &mapping), SPUDRESULT_SPUDFILE_NOT_READABLE);
		spudfile_file_close(file);
	}
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
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
	spudfile_file file = open_file(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_TRUNCATE);
	if (file) {
		CHECK_RESULT(spudfile_file_write_at(file, 0, bytes, size), SPUD_SUCCESS);
		spudfile_file_close(file);
	}
	file = open_file(path, SPUDFILE_ACCESS_READ, SPUDFILE_CREATE_OPEN_EXISTING);
	if (!file) {
		free(bytes);
		return;
	}
	spudfile_mapping mapping = (spudfile_mapping)(uintptr_t)1;
	CHECK_RESULT(spudfile_file_map(file, 0, 10, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(spudfile_file_map(NULL, 0, 10, &mapping), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK(mapping == NULL);
	CHECK_RESULT(spudfile_file_map(file, 0, 0, &mapping), SPUDRESULT_ZERO_SIZE);
	CHECK_RESULT(spudfile_file_map(file, size, 1, &mapping), SPUDRESULT_INDEX_OUT_OF_RANGE);
	CHECK_RESULT(spudfile_file_map(file, 10, size, &mapping), SPUDRESULT_INDEX_OUT_OF_RANGE);

	spudfile_mapping whole = NULL;
	spudfile_mapping part  = NULL;
	CHECK_RESULT(spudfile_file_map(file, 0, size, &whole), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_map(file, 12345, 54321, &part), SPUD_SUCCESS);
	spudfile_file_close(file); // a mapping keeps what it needs
	if (whole) {
		CHECK(spudfile_mapping_get_size(whole) == size);
		CHECK(memcmp(spudfile_mapping_get_data(whole), bytes, (size_t)size) == 0);
		spudfile_mapping_release(whole);
	}
	if (part) {
		CHECK(spudfile_mapping_get_size(part) == 54321);
		CHECK(memcmp(spudfile_mapping_get_data(part), bytes + 12345, 54321) == 0);
		spudfile_mapping_release(part);
	}
	CHECK(spudfile_mapping_get_data(NULL) == NULL);
	CHECK(spudfile_mapping_get_size(NULL) == 0);
	spudfile_mapping_release(NULL);
	free(bytes);
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
}

static void test_paths(void) {
	char file_path[1100];
	path_in(file_path, sizeof(file_path), "info.txt");
	write_whole(file_path, "twelve bytes");

	SPUDRESULT r;
	SPUDFILE_PATH_INFO info = info_of(file_path, &r);
	CHECK(r == SPUD_SUCCESS);
	CHECK(info.kind == SPUDFILE_PATH_KIND_FILE);
	CHECK(info.size == 12);
	// Modified within a day of now.
	CHECK(info.modified_ns > (int64_t)1700000000 * 1000000000);
	info = info_of(g_dir, &r);
	CHECK(r == SPUD_SUCCESS && info.kind == SPUDFILE_PATH_KIND_DIRECTORY && info.size == 0);
	char missing[1100];
	path_in(missing, sizeof(missing), "missing.txt");
	info = info_of(missing, &r);
	CHECK(r == SPUDRESULT_SPUDFILE_NOT_FOUND);

	SPUDFILE_PATH_INFO wrong;
	memset(&wrong, 0, sizeof(wrong));
	CHECK_RESULT(spudfile_path_get_info(g_files, file_path, &wrong), SPUDRESULT_STRUCT_WRONG_TYPE);
	CHECK_RESULT(spudfile_path_get_info(NULL, file_path, &info), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_path_get_info(g_files, NULL, &info), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_get_info(g_files, "", &info), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_get_info(g_files, file_path, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);

	// Replace: onto nothing, onto a file, at each level.
	char source[1100];
	char target[1100];
	path_in(source, sizeof(source), "new.txt");
	path_in(target, sizeof(target), "doc.txt");
	const SPUDFILE_FLUSH_LEVEL levels[] = {SPUDFILE_FLUSH_LEVEL_NONE, SPUDFILE_FLUSH_LEVEL_DEVICE, SPUDFILE_FLUSH_LEVEL_MEDIA};
	const char *texts[]            = {"one", "two", "three"};
	for (int i = 0; i < 3; ++i) {
		write_whole(source, texts[i]);
		r = spudfile_path_replace(g_files, source, target, levels[i]);
		CHECK(r == SPUD_SUCCESS || (levels[i] == SPUDFILE_FLUSH_LEVEL_MEDIA && r == SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED));
		info_of(source, &r);
		CHECK(r == SPUDRESULT_SPUDFILE_NOT_FOUND);
		CHECK(holds(target, texts[i]));
	}
	CHECK_RESULT(spudfile_path_replace(g_files, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK(holds(target, "three"));
	CHECK_RESULT(spudfile_path_replace(NULL, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_path_replace(g_files, NULL, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_replace(g_files, source, "", SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_replace(g_files, source, target, (SPUDFILE_FLUSH_LEVEL)9), SPUDRESULT_DESC_INVALID_PARAMETERS);

	// Remove.
	CHECK_RESULT(spudfile_path_remove(g_files, target), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, target), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK_RESULT(spudfile_path_remove(g_files, g_dir), SPUDRESULT_SPUDFILE_NOT_A_FILE);
	CHECK_RESULT(spudfile_path_remove(NULL, target), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_path_remove(g_files, NULL), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_remove(g_files, file_path), SPUD_SUCCESS);

	// Directories, one level at a time.
	char top[1100];
	char nested[1200];
	path_in(top, sizeof(top), "a");
	snprintf(nested, sizeof(nested), "%s" SLASH "b", top);
	CHECK_RESULT(spudfile_directory_create(g_files, nested), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK_RESULT(spudfile_directory_create(g_files, top), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_create(g_files, nested), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_create(g_files, nested), SPUDRESULT_SPUDFILE_ALREADY_EXISTS);
	info = info_of(nested, &r);
	CHECK(r == SPUD_SUCCESS && info.kind == SPUDFILE_PATH_KIND_DIRECTORY);
	char in_the_way[1300];
	snprintf(in_the_way, sizeof(in_the_way), "%s" SLASH "file", top);
	write_whole(in_the_way, "x");
	CHECK_RESULT(spudfile_directory_create(g_files, in_the_way), SPUDRESULT_SPUDFILE_ALREADY_EXISTS);
	CHECK_RESULT(spudfile_directory_create(NULL, top), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_directory_create(g_files, ""), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_remove(g_files, in_the_way), SPUD_SUCCESS);
	CHECK(rmdir(nested) == 0);
	CHECK(rmdir(top) == 0);
}

/* A name outside ASCII: UTF-8 here, UTF-16 on Windows. */
static void test_unicode_name(void) {
	char path[1100];
	path_in(path, sizeof(path), "pl\xC3\xA4n \xE5\x9B\xB3.txt"); // "plän 図.txt"
	write_whole(path, "unicode");
	CHECK(holds(path, "unicode"));
	SPUDRESULT r;
	SPUDFILE_PATH_INFO info = info_of(path, &r);
	CHECK(r == SPUD_SUCCESS && info.size == 7);
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
}

/* Two threads reading one file at once, each at its own offsets. */
struct reader {
	spudfile_file file;
	uint64_t start;
	int ok;
};

#if SPUDLIB_PLATFORM_WIN32
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
			if (spudfile_file_read_at(reader->file, at, buf, sizeof(buf), &count) != SPUD_SUCCESS || count != 256) {
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
	spudfile_file file = open_file(path, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_TRUNCATE);
	if (file) {
		CHECK_RESULT(spudfile_file_write_at(file, 0, bytes, sizeof(bytes)), SPUD_SUCCESS);
		spudfile_file_close(file);
	}
	file = open_file(path, SPUDFILE_ACCESS_READ, SPUDFILE_CREATE_OPEN_EXISTING);
	if (!file)
		return;
	struct reader a = {file, 0, 0};
	struct reader b = {file, 4096, 0};
#if SPUDLIB_PLATFORM_WIN32
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
	spudfile_file_close(file);
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
}

/* ---- Listing ---- */

static spudfile_listing_desc listing_desc(const char *path) {
	spudfile_listing_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_FILES_LISTING_DESC;
	desc.path      = path;
	return desc;
}

static SPUDFILE_LISTING_ENTRY empty_entry(void) {
	SPUDFILE_LISTING_ENTRY entry;
	memset(&entry, 0, sizeof(entry));
	entry.base.type = SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY;
	return entry;
}

/* What a listing of a directory gave: up to 16 entries. */
struct seen {
	char names[16][64];
	SPUDFILE_ENTRY_KIND kinds[16];
	bool utf8[16];
	int count;
	int extra; /* past 16, or a name too long to keep */
	SPUDRESULT end;
};

static struct seen list_dir(const char *dir) {
	struct seen seen;
	memset(&seen, 0, sizeof(seen));
	const spudfile_listing_desc desc = listing_desc(dir);
	spudfile_listing listing         = NULL;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUD_SUCCESS);
	if (!listing)
		return seen;
	for (;;) {
		SPUDFILE_LISTING_ENTRY entry = empty_entry();
		const SPUDRESULT result      = spudfile_listing_next(listing, &entry);
		if (result != SPUD_SUCCESS) {
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
	SPUDFILE_LISTING_ENTRY after = empty_entry();
	after.name_size              = 77;
	CHECK_RESULT(spudfile_listing_next(listing, &after), SPUDRESULT_SPUDFILE_END_OF_LISTING);
	CHECK(after.name_size == 77);
	spudfile_listing_close(listing);
	return seen;
}

/* How many times [name] was given, its kind and whether it was UTF-8. */
static int seen_count(
    const struct seen *seen,
    const char *name,
    SPUDFILE_ENTRY_KIND *out_kind,
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
	CHECK_RESULT(spudfile_directory_create(g_files, dir), SPUD_SUCCESS);
	spudfile_listing_desc desc = listing_desc(dir);
	spudfile_listing listing   = (spudfile_listing)(uintptr_t)1;

	// Every argument in order, nothing made.
	CHECK_RESULT(spudfile_listing_open(NULL, &desc, &listing), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_listing_open(g_files, NULL, &listing), SPUDRESULT_NULL_DESC);
	desc.base.type = SPUD_STRUCT_TYPE_FILES_OPEN_DESC;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_STRUCT_WRONG_TYPE);
	desc.base.type = SPUD_STRUCT_TYPE_FILES_LISTING_DESC;
	desc.base.next = &desc.base;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	desc.path = NULL;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK(listing == NULL);
	desc.path = "";
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_SPUDFILE_NULL_PATH);

	// Missing, and a file where a directory is wanted.
	char missing[1100];
	path_in(missing, sizeof(missing), "listing_checks" SLASH "none");
	desc.path = missing;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_SPUDFILE_NOT_FOUND);
	char missing_parent[1100];
	path_in(missing_parent, sizeof(missing_parent), "listing_checks" SLASH "none" SLASH "deeper");
	desc.path = missing_parent;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_SPUDFILE_NOT_FOUND);
	char file[1100];
	path_in(file, sizeof(file), "listing_checks" SLASH "a_file");
	write_whole(file, "x");
	desc.path = file;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY);
	CHECK(listing == NULL);

	// An empty directory ends at once.
	char empty[1100];
	path_in(empty, sizeof(empty), "listing_checks" SLASH "empty");
	CHECK_RESULT(spudfile_directory_create(g_files, empty), SPUD_SUCCESS);
	const struct seen seen = list_dir(empty);
	CHECK(seen.count == 0 && seen.extra == 0);
	CHECK(seen.end == SPUDRESULT_SPUDFILE_END_OF_LISTING);

	// next's own checks, and a debug name.
	desc.path       = dir;
	desc.debug_name = "checks";
	listing         = NULL;
	CHECK_RESULT(spudfile_listing_open(g_files, &desc, &listing), SPUD_SUCCESS);
	SPUDFILE_LISTING_ENTRY entry = empty_entry();
	CHECK_RESULT(spudfile_listing_next(NULL, &entry), SPUDRESULT_SPUDFILE_INVALID_LISTING);
	CHECK_RESULT(spudfile_listing_next(listing, NULL), SPUDRESULT_NULL_OUTPUT_PARAMETER);
	entry.base.type = SPUD_STRUCT_TYPE_FILES_PATH_INFO;
	CHECK_RESULT(spudfile_listing_next(listing, &entry), SPUDRESULT_STRUCT_WRONG_TYPE);
	entry.base.type = SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY;
	entry.base.next = &entry.base;
	CHECK_RESULT(spudfile_listing_next(listing, &entry), SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN);
	spudfile_listing_close(listing);
	spudfile_listing_close(NULL);

	CHECK_RESULT(spudfile_path_remove(g_files, file), SPUD_SUCCESS);
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
#if SPUDLIB_PLATFORM_WIN32
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
#if SPUDLIB_PLATFORM_WIN32
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
	CHECK_RESULT(spudfile_directory_create(g_files, dir), SPUD_SUCCESS);
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
	CHECK_RESULT(spudfile_directory_create(g_files, sub), SPUD_SUCCESS);
#if SPUDLIB_PLATFORM_WIN32
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
		printf("spudfiles_test: symbolic links not made here (Windows needs developer mode): not listed\n");

#if SPUDLIB_PLATFORM_WIN32
	// A junction, through mklink, which needs no privilege.
	char junction[1100];
	path_in(junction, sizeof(junction), "listing" SLASH "junction");
	char command[2400];
	snprintf(command, sizeof(command), "cmd /c mklink /J \"%s\" \"%s\" >NUL", junction, sub);
	const bool junctioned = system(command) == 0;
	if (!junctioned)
		printf("spudfiles_test: no junction made: not listed\n");
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
		printf("spudfiles_test: no socket made (path too long?): not listed\n");
#endif

	const struct seen seen = list_dir(dir);
	CHECK(seen.end == SPUDRESULT_SPUDFILE_END_OF_LISTING);
	CHECK(seen.extra == 0);
	CHECK(seen_count(&seen, ".", NULL, NULL) == 0);
	CHECK(seen_count(&seen, "..", NULL, NULL) == 0);
	SPUDFILE_ENTRY_KIND kind = (SPUDFILE_ENTRY_KIND)0;
	bool utf8                = false;
	CHECK(seen_count(&seen, "a.txt", &kind, &utf8) == 1 && kind == SPUDFILE_ENTRY_KIND_FILE && utf8);
	CHECK(seen_count(&seen, "sub", &kind, NULL) == 1 && kind == SPUDFILE_ENTRY_KIND_DIRECTORY);
	CHECK(seen_count(&seen, ".hidden", &kind, NULL) == 1 && kind == SPUDFILE_ENTRY_KIND_FILE);
	CHECK(seen_count(&seen, "pl\xC3\xA4n \xE5\x9B\xB3.txt", &kind, &utf8) == 1 && kind == SPUDFILE_ENTRY_KIND_FILE && utf8);
	int expected = 4;
	if (file_linked) {
		CHECK(seen_count(&seen, "file_link", &kind, NULL) == 1 && kind == SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK);
		++expected;
	}
	if (dir_linked) {
		CHECK(seen_count(&seen, "dir_link", &kind, NULL) == 1 && kind == SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK);
		++expected;
	}
#if SPUDLIB_PLATFORM_WIN32
	if (junctioned) {
		CHECK(seen_count(&seen, "junction", &kind, NULL) == 1 && kind == SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK);
		++expected;
	}
#else
	if (socketed) {
		CHECK(seen_count(&seen, "sock", &kind, NULL) == 1 && kind == SPUDFILE_ENTRY_KIND_OTHER);
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
#if SPUDLIB_PLATFORM_WIN32
	if (junctioned)
		remove_link(junction, true);
	CHECK(SetFileAttributesW(wide_hidden, FILE_ATTRIBUTE_NORMAL) != 0);
#else
	if (socketed)
		CHECK(unlink(socket_path) == 0);
#endif
	CHECK_RESULT(spudfile_path_remove(g_files, a), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, hidden), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, unicode), SPUD_SUCCESS);
	CHECK(rmdir(sub) == 0);
	CHECK(rmdir(dir) == 0);
}

/* Names that aren't UTF-8: listed with name_is_utf8 false, and opened by
 * the name given. */
static void test_listing_non_utf8(void) {
	char dir[1100];
	path_in(dir, sizeof(dir), "non_utf8");
	CHECK_RESULT(spudfile_directory_create(g_files, dir), SPUD_SUCCESS);
#if SPUDLIB_PLATFORM_WIN32
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
#else
	// An invalid byte: POSIX takes a name's bytes as they are.
	const char *const name = "bad\xFF.txt";
	char made[1100];
	path_in(made, sizeof(made), "non_utf8" SLASH "bad\xFF.txt");
	write_whole(made, "w");
#endif
	const struct seen seen   = list_dir(dir);
	SPUDFILE_ENTRY_KIND kind = (SPUDFILE_ENTRY_KIND)0;
	bool utf8                = true;
	CHECK(seen.count == 1);
	CHECK(seen_count(&seen, name, &kind, &utf8) == 1 && kind == SPUDFILE_ENTRY_KIND_FILE && !utf8);
	char path[1100];
	snprintf(path, sizeof(path), "%s" SLASH "%s", dir, name);
	CHECK(holds(path, "w"));
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
	CHECK(rmdir(dir) == 0);

#if SPUDLIB_PLATFORM_WIN32
	// The converter's refusals: an encoded surrogate pair (WTF-8 forbids
	// it: the pair is the code point's own four bytes), an overlong '/',
	// a surrogate cut short, and a lead byte with nothing after it. None
	// names a file.
	const char *const refused[] = {"pair\xED\xA0\x80\xED\xB0\x80.txt", "over\xC0\xAF.txt", "cut\xED\xA0.txt", "end\xE5"};
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
		char bad[1100];
		path_in(bad, sizeof(bad), refused[i]);
		const spudfile_open_desc desc = open_desc(bad, SPUDFILE_ACCESS_WRITE, SPUDFILE_CREATE_TRUNCATE);
		spudfile_file file            = NULL;
		CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUDRESULT_SPUDFILE_NOT_FOUND);
	}
#endif
}

/* ---- Locks ---- */

static spudfile_file open_shared(const char *path) {
	spudfile_open_desc desc = open_desc(path, SPUDFILE_ACCESS_READ_WRITE, SPUDFILE_CREATE_OPEN_OR_CREATE);
	spudfile_file file      = NULL;
	CHECK_RESULT(spudfile_file_open(g_files, &desc, &file), SPUD_SUCCESS);
	return file;
}

struct waiter {
	spudfile_file file;
	atomic_int released; /* set by the holder just before it unlocks */
	atomic_int took;     /* 1 once the lock is had after that; 2 if before */
	SPUDRESULT result;
};

#if SPUDLIB_PLATFORM_WIN32
static unsigned __stdcall wait_for_lock(void *argument)
#else
static void *wait_for_lock(void *argument)
#endif
{
	struct waiter *waiter = (struct waiter *)argument;
	waiter->result        = spudfile_file_lock(waiter->file, SPUDFILE_LOCK_EXCLUSIVE, true);
	atomic_store(&waiter->took, atomic_load(&waiter->released) ? 1 : 2);
	return 0;
}

static void sleep_ms(unsigned ms) {
#if SPUDLIB_PLATFORM_WIN32
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
	spudfile_file one = open_shared(path);
	spudfile_file two = open_shared(path);
	if (!one || !two) {
		spudfile_file_close(one);
		spudfile_file_close(two);
		return;
	}
	CHECK_RESULT(spudfile_file_write_at(one, 0, "0123", 4), SPUD_SUCCESS);

	// Checks, in order.
	CHECK_RESULT(spudfile_file_lock(NULL, SPUDFILE_LOCK_EXCLUSIVE, false), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_NONE, false), SPUDRESULT_DESC_INVALID_PARAMETERS);
	CHECK_RESULT(spudfile_file_lock(one, (SPUDFILE_LOCK)3, false), SPUDRESULT_DESC_INVALID_PARAMETERS);
	CHECK_RESULT(spudfile_file_unlock(NULL), SPUDRESULT_SPUDFILE_INVALID_FILE);
	CHECK_RESULT(spudfile_file_unlock(one), SPUDRESULT_SPUDFILE_NOT_LOCKED);

	// Exclusive refuses exclusive and shared; it is advisory: the other
	// file still reads and writes.
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_EXCLUSIVE, false), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_EXCLUSIVE, false), SPUDRESULT_SPUDFILE_ALREADY_LOCKED);
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_SHARED, false), SPUDRESULT_SPUDFILE_ALREADY_LOCKED);
	CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_EXCLUSIVE, false), SPUDRESULT_SPUDFILE_LOCKED);
	CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_SHARED, false), SPUDRESULT_SPUDFILE_LOCKED);
	char read[4]   = {0};
	uint64_t count = 0;
	CHECK_RESULT(spudfile_file_read_at(two, 0, read, 4, &count), SPUD_SUCCESS);
	CHECK(count == 4 && memcmp(read, "0123", 4) == 0);
	CHECK_RESULT(spudfile_file_write_at(two, 1, "x", 1), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_set_size(two, 8), SPUD_SUCCESS);

	// Unlocking lets the other take it.
	CHECK_RESULT(spudfile_file_unlock(one), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_unlock(one), SPUDRESULT_SPUDFILE_NOT_LOCKED);
	CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_EXCLUSIVE, false), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_SHARED, false), SPUDRESULT_SPUDFILE_LOCKED);
	CHECK_RESULT(spudfile_file_unlock(two), SPUD_SUCCESS);

	// Shared allows shared, and refuses exclusive.
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_SHARED, false), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_SHARED, false), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_unlock(two), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_EXCLUSIVE, false), SPUDRESULT_SPUDFILE_LOCKED);
	CHECK_RESULT(spudfile_file_unlock(one), SPUD_SUCCESS);

	// Closing lets go.
	CHECK_RESULT(spudfile_file_lock(one, SPUDFILE_LOCK_EXCLUSIVE, false), SPUD_SUCCESS);
	spudfile_file_close(one);
	CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_EXCLUSIVE, false), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_file_unlock(two), SPUD_SUCCESS);

	// A waiting lock is had once the holder lets go, and not before.
	one = open_shared(path);
	if (one) {
		CHECK_RESULT(spudfile_file_lock(two, SPUDFILE_LOCK_EXCLUSIVE, false), SPUD_SUCCESS);
		struct waiter waiter;
		waiter.file = one;
		atomic_init(&waiter.released, 0);
		atomic_init(&waiter.took, 0);
		waiter.result = SPUDRESULT_API_SPECIFIC_FAILURE;
#if SPUDLIB_PLATFORM_WIN32
		HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, wait_for_lock, &waiter, 0, NULL);
#else
		pthread_t thread;
		pthread_create(&thread, NULL, wait_for_lock, &waiter);
#endif
		sleep_ms(200);
		CHECK(atomic_load(&waiter.took) == 0);
		atomic_store(&waiter.released, 1);
		CHECK_RESULT(spudfile_file_unlock(two), SPUD_SUCCESS);
#if SPUDLIB_PLATFORM_WIN32
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
#else
		pthread_join(thread, NULL);
#endif
		CHECK_RESULT(waiter.result, SPUD_SUCCESS);
		CHECK(atomic_load(&waiter.took) == 1);
		CHECK_RESULT(spudfile_file_unlock(one), SPUD_SUCCESS);
		spudfile_file_close(one);
	}
	spudfile_file_close(two);
	CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
}

/* ---- A file's permissions copied (card S42) ---- */

#if SPUDLIB_PLATFORM_WIN32
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
	CHECK_RESULT(spudfile_directory_create(g_files, dir), SPUD_SUCCESS);

	// Argument checks, in order.
	CHECK_RESULT(spudfile_path_copy_mode(NULL, NULL, NULL), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, NULL, NULL), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, "", target), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, NULL), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, ""), SPUDRESULT_SPUDFILE_NULL_PATH);

	// Nothing at either path: not found, the target unchanged.
	CHECK_RESULT(spudfile_path_copy_mode(g_files, missing, target), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, missing), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK(holds(target, "t"));

#if SPUDLIB_PLATFORM_WIN32
	// The read-only attribute, both ways; the target's hidden attribute kept.
	set_attributes(target, FILE_ATTRIBUTE_HIDDEN);
	set_attributes(source, FILE_ATTRIBUTE_READONLY);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, target), SPUD_SUCCESS);
	CHECK((attributes_of(target) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));
	set_attributes(source, FILE_ATTRIBUTE_NORMAL);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, target), SPUD_SUCCESS);
	CHECK((attributes_of(target) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) == FILE_ATTRIBUTE_HIDDEN);
	// Copied again onto a target that has it already: unchanged.
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, target), SPUD_SUCCESS);
	CHECK((attributes_of(target) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) == FILE_ATTRIBUTE_HIDDEN);
	set_attributes(target, FILE_ATTRIBUTE_NORMAL);

	// A directory either way.
	set_attributes(source, FILE_ATTRIBUTE_READONLY);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, dir), SPUD_SUCCESS);
	CHECK((attributes_of(dir) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY));
	CHECK_RESULT(spudfile_path_copy_mode(g_files, dir, target), SPUD_SUCCESS);
	CHECK((attributes_of(target) & FILE_ATTRIBUTE_READONLY) != 0);
	set_attributes(source, FILE_ATTRIBUTE_NORMAL);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, dir), SPUD_SUCCESS);
	CHECK((attributes_of(dir) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == FILE_ATTRIBUTE_DIRECTORY);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, target), SPUD_SUCCESS);

	// A junction as the target: what it leads to changed, the link not.
	char junction[1100];
	path_in(junction, sizeof(junction), "mode_junction");
	if (make_junction(dir, junction)) {
		set_attributes(source, FILE_ATTRIBUTE_READONLY);
		CHECK_RESULT(spudfile_path_copy_mode(g_files, source, junction), SPUD_SUCCESS);
		CHECK((attributes_of(dir) & FILE_ATTRIBUTE_READONLY) != 0);
		CHECK((attributes_of(junction) & FILE_ATTRIBUTE_READONLY) == 0);
		set_attributes(source, FILE_ATTRIBUTE_NORMAL);
		CHECK_RESULT(spudfile_path_copy_mode(g_files, source, junction), SPUD_SUCCESS);
		CHECK((attributes_of(dir) & FILE_ATTRIBUTE_READONLY) == 0);
		CHECK_RESULT(spudfile_directory_remove(g_files, junction), SPUD_SUCCESS);
	} else {
		printf("spudfiles_test: no junction made: copy_mode through one not run\n");
	}
#else
	// The nine bits, whatever the umask; never the set-ID or sticky bits.
	const mode_t old_umask = umask(077);
	const mode_t modes[]   = {0755, 0444, 0600, 0640};
	for (int i = 0; i < 4; ++i) {
		CHECK(chmod(source, modes[i]) == 0);
		CHECK(chmod(target, 0644) == 0);
		CHECK_RESULT(spudfile_path_copy_mode(g_files, source, target), SPUD_SUCCESS);
		CHECK(mode_of(target) == modes[i]);
	}
	if (chmod(source, 06755) == 0 && (mode_of(source) & 06000)) {
		CHECK_RESULT(spudfile_path_copy_mode(g_files, source, target), SPUD_SUCCESS);
		CHECK(mode_of(target) == 0755);
	} else {
		printf("spudfiles_test: no set-ID bits on the source here: their copy not run\n");
	}
	umask(old_umask);
	CHECK(chmod(source, 0644) == 0);
	CHECK(chmod(target, 0644) == 0);

	// A directory either way.
	CHECK(chmod(source, 0700) == 0);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source, dir), SPUD_SUCCESS);
	CHECK(mode_of(dir) == 0700);
	CHECK(chmod(dir, 0751) == 0);
	CHECK_RESULT(spudfile_path_copy_mode(g_files, dir, target), SPUD_SUCCESS);
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
	CHECK_RESULT(spudfile_path_copy_mode(g_files, source_link, target_link), SPUD_SUCCESS);
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
		CHECK_RESULT(spudfile_path_copy_mode(g_files, "/", "/"), SPUDRESULT_SPUDFILE_ACCESS_DENIED);
	} else {
		printf("spudfiles_test: run as root: copy_mode's refusal not run\n");
	}
#endif

	CHECK_RESULT(spudfile_path_remove(g_files, source), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, target), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, dir), SPUD_SUCCESS);
}

/* ---- Removing and renaming a directory ---- */

#if SPUDLIB_PLATFORM_WIN32
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

static SPUDRESULT kind_at(
    const char *path,
    SPUDFILE_PATH_KIND *out_kind) {
	SPUDRESULT r                  = SPUD_SUCCESS;
	const SPUDFILE_PATH_INFO info = info_of(path, &r);
	*out_kind                     = info.kind;
	return r;
}

static void test_directory_remove(void) {
	SPUDFILE_PATH_KIND kind = (SPUDFILE_PATH_KIND)0;

	// Argument checks, in order.
	char dir[1100];
	path_in(dir, sizeof(dir), "rm");
	CHECK_RESULT(spudfile_directory_remove(NULL, dir), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_directory_remove(NULL, NULL), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_directory_remove(g_files, NULL), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_directory_remove(g_files, ""), SPUDRESULT_SPUDFILE_NULL_PATH);

	// An empty directory goes; then nothing is there.
	CHECK_RESULT(spudfile_directory_create(g_files, dir), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, dir), SPUD_SUCCESS);
	CHECK_RESULT(kind_at(dir, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK_RESULT(spudfile_directory_remove(g_files, dir), SPUDRESULT_SPUDFILE_NOT_FOUND);

	// One that holds a file is refused, and stays with its file.
	char full[1100];
	char inside[1200];
	path_in(full, sizeof(full), "rm_full");
	snprintf(inside, sizeof(inside), "%s" SLASH "keep.txt", full);
	CHECK_RESULT(spudfile_directory_create(g_files, full), SPUD_SUCCESS);
	write_whole(inside, "kept");
	CHECK_RESULT(spudfile_directory_remove(g_files, full), SPUDRESULT_SPUDFILE_NOT_EMPTY);
	CHECK(kind_at(full, &kind) == SPUD_SUCCESS && kind == SPUDFILE_PATH_KIND_DIRECTORY);
	CHECK(holds(inside, "kept"));

	// One that holds only a directory is refused too.
	char only_dir[1100];
	char nested[1200];
	path_in(only_dir, sizeof(only_dir), "rm_nested");
	snprintf(nested, sizeof(nested), "%s" SLASH "inner", only_dir);
	CHECK_RESULT(spudfile_directory_create(g_files, only_dir), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_create(g_files, nested), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, only_dir), SPUDRESULT_SPUDFILE_NOT_EMPTY);
	CHECK_RESULT(spudfile_directory_remove(g_files, nested), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, only_dir), SPUD_SUCCESS);

	// A file isn't a directory, and stays.
	char file[1100];
	path_in(file, sizeof(file), "rm_file.txt");
	write_whole(file, "f");
	CHECK_RESULT(spudfile_directory_remove(g_files, file), SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY);
	CHECK(holds(file, "f"));

	// Nothing on the way.
	char under_missing[1200];
	snprintf(under_missing, sizeof(under_missing), "%s" SLASH "missing" SLASH "x", g_dir);
	CHECK_RESULT(spudfile_directory_remove(g_files, under_missing), SPUDRESULT_SPUDFILE_NOT_FOUND);

	// A link to a directory is removed itself; what it led to stays, with
	// its file. A link to a file isn't a directory.
	char dir_link[1100];
	char file_link[1100];
	path_in(dir_link, sizeof(dir_link), "rm_dir_link");
	path_in(file_link, sizeof(file_link), "rm_file_link");
	if (make_symbolic_link(full, dir_link, true)) {
		CHECK_RESULT(spudfile_directory_remove(g_files, dir_link), SPUD_SUCCESS);
		CHECK_RESULT(kind_at(dir_link, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
		CHECK(holds(inside, "kept"));
	} else {
		printf("spudfiles_test: no symbolic link to a directory made: its remove not run\n");
	}
	if (make_symbolic_link(file, file_link, false)) {
		CHECK_RESULT(spudfile_directory_remove(g_files, file_link), SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY);
		remove_link(file_link, false);
	} else {
		printf("spudfiles_test: no symbolic link to a file made: its remove not run\n");
	}
#if SPUDLIB_PLATFORM_WIN32
	char junction[1100];
	path_in(junction, sizeof(junction), "rm_junction");
	if (make_junction(full, junction)) {
		CHECK_RESULT(spudfile_directory_remove(g_files, junction), SPUD_SUCCESS);
		CHECK_RESULT(kind_at(junction, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
		CHECK(holds(inside, "kept"));
	} else {
		printf("spudfiles_test: no junction made: its remove not run\n");
	}

	// Held open without sharing delete: in use, and still there.
	CHECK_RESULT(spudfile_directory_create(g_files, dir), SPUD_SUCCESS);
	HANDLE held = hold_without_delete(dir);
	CHECK(held != INVALID_HANDLE_VALUE);
	if (held != INVALID_HANDLE_VALUE) {
		CHECK_RESULT(spudfile_directory_remove(g_files, dir), SPUDRESULT_SPUDFILE_IN_USE);
		CloseHandle(held);
	}
	CHECK_RESULT(spudfile_directory_remove(g_files, dir), SPUD_SUCCESS);
#endif

	CHECK_RESULT(spudfile_path_remove(g_files, file), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, inside), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, full), SPUD_SUCCESS);
}

static void test_directory_rename(void) {
	SPUDFILE_PATH_KIND kind = (SPUDFILE_PATH_KIND)0;
	char source[1100];
	char target[1100];
	char source_file[1200];
	char target_file[1200];
	path_in(source, sizeof(source), "mv_source");
	path_in(target, sizeof(target), "mv_target");
	snprintf(source_file, sizeof(source_file), "%s" SLASH "f.txt", source);
	snprintf(target_file, sizeof(target_file), "%s" SLASH "f.txt", target);

	// Argument checks, in order.
	CHECK_RESULT(spudfile_directory_rename(NULL, NULL, NULL, (SPUDFILE_FLUSH_LEVEL)9), SPUDRESULT_SPUDFILE_INVALID_INSTANCE);
	CHECK_RESULT(spudfile_directory_rename(g_files, NULL, NULL, (SPUDFILE_FLUSH_LEVEL)9), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_directory_rename(g_files, "", target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_directory_rename(g_files, source, NULL, (SPUDFILE_FLUSH_LEVEL)9), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_directory_rename(g_files, source, "", SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NULL_PATH);
	CHECK_RESULT(spudfile_directory_rename(g_files, source, target, (SPUDFILE_FLUSH_LEVEL)9), SPUDRESULT_DESC_INVALID_PARAMETERS);

	// No source.
	CHECK_RESULT(spudfile_directory_rename(g_files, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NOT_FOUND);

	// A directory with a file, to a free name and back, at each level: the
	// file under the new name, nothing under the old.
	CHECK_RESULT(spudfile_directory_create(g_files, source), SPUD_SUCCESS);
	write_whole(source_file, "moved");
	const SPUDFILE_FLUSH_LEVEL levels[] = {SPUDFILE_FLUSH_LEVEL_NONE, SPUDFILE_FLUSH_LEVEL_DEVICE, SPUDFILE_FLUSH_LEVEL_MEDIA};
	for (int i = 0; i < 3; ++i) {
		SPUDRESULT r = spudfile_directory_rename(g_files, source, target, levels[i]);
		CHECK(r == SPUD_SUCCESS || (levels[i] == SPUDFILE_FLUSH_LEVEL_MEDIA && r == SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED));
		CHECK_RESULT(kind_at(source, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
		CHECK(holds(target_file, "moved"));
		r = spudfile_directory_rename(g_files, target, source, levels[i]);
		CHECK(r == SPUD_SUCCESS || (levels[i] == SPUDFILE_FLUSH_LEVEL_MEDIA && r == SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED));
		CHECK(holds(source_file, "moved"));
	}

	// Something at the target: a file, an empty directory, a full one. Each
	// refused, both left as they were.
	write_whole(target, "in the way");
	CHECK_RESULT(spudfile_directory_rename(g_files, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_ALREADY_EXISTS);
	CHECK(holds(target, "in the way"));
	CHECK(holds(source_file, "moved"));
	CHECK_RESULT(spudfile_path_remove(g_files, target), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_create(g_files, target), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_rename(g_files, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_ALREADY_EXISTS);
	CHECK(kind_at(target, &kind) == SPUD_SUCCESS && kind == SPUDFILE_PATH_KIND_DIRECTORY);
	CHECK(holds(source_file, "moved"));
	write_whole(target_file, "theirs");
	CHECK_RESULT(spudfile_directory_rename(g_files, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_ALREADY_EXISTS);
	CHECK(holds(target_file, "theirs"));
	CHECK(holds(source_file, "moved"));
	CHECK_RESULT(spudfile_path_remove(g_files, target_file), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, target), SPUD_SUCCESS);

	// A file source isn't a directory.
	char file[1100];
	path_in(file, sizeof(file), "mv_file.txt");
	write_whole(file, "f");
	CHECK_RESULT(spudfile_directory_rename(g_files, file, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY);
	CHECK(holds(file, "f"));
	CHECK_RESULT(kind_at(target, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);

	// No parent for the target.
	char orphan[1200];
	snprintf(orphan, sizeof(orphan), "%s" SLASH "missing" SLASH "x", g_dir);
	CHECK_RESULT(spudfile_directory_rename(g_files, source, orphan, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK(holds(source_file, "moved"));

#if SPUDLIB_PLATFORM_WIN32
	// A file inside held open without sharing delete: in use, nothing moved.
	HANDLE held = hold_without_delete(source_file);
	CHECK(held != INVALID_HANDLE_VALUE);
	if (held != INVALID_HANDLE_VALUE) {
		CHECK_RESULT(spudfile_directory_rename(g_files, source, target, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_IN_USE);
		CloseHandle(held);
		CHECK(holds(source_file, "moved"));
		CHECK_RESULT(kind_at(target, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
	}
#endif

	// Across volumes, where a directory on another one is named.
	const char *other = getenv("SPUDFILES_TEST_OTHER_VOLUME_DIR");
	if (other && other[0]) {
		char away[1200];
		snprintf(away, sizeof(away), "%s" SLASH "spudfiles_test_moved", other);
		CHECK_RESULT(spudfile_directory_rename(g_files, source, away, SPUDFILE_FLUSH_LEVEL_NONE), SPUDRESULT_SPUDFILE_DIFFERENT_VOLUME);
		CHECK(holds(source_file, "moved"));
		printf("spudfiles_test: a directory renamed across volumes, to %s: refused\n", other);
	} else {
		printf("spudfiles_test: no SPUDFILES_TEST_OTHER_VOLUME_DIR: a rename across volumes not run\n");
	}

	CHECK_RESULT(spudfile_path_remove(g_files, file), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_path_remove(g_files, source_file), SPUD_SUCCESS);
	CHECK_RESULT(spudfile_directory_remove(g_files, source), SPUD_SUCCESS);
}

/* Removes the tree at [dir] through the calls a caller has: a listing, then
 * each entry (a directory's tree first), then the directory. */
static void remove_tree(const char *dir) {
	const struct seen seen = list_dir(dir);
	CHECK(seen.extra == 0);
	for (int i = 0; i < seen.count; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s" SLASH "%s", dir, seen.names[i]);
		if (seen.kinds[i] == SPUDFILE_ENTRY_KIND_DIRECTORY)
			remove_tree(path);
		else
			CHECK_RESULT(spudfile_path_remove(g_files, path), SPUD_SUCCESS);
	}
	CHECK_RESULT(spudfile_directory_remove(g_files, dir), SPUD_SUCCESS);
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
		CHECK_RESULT(spudfile_directory_create(g_files, path), SPUD_SUCCESS);
	}
	for (int i = 0; i < 4; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s%s", partial, files[i]);
		write_whole(path, files[i]);
	}
	CHECK_RESULT(spudfile_directory_rename(g_files, partial, done, SPUDFILE_FLUSH_LEVEL_DEVICE), SPUD_SUCCESS);
	for (int i = 0; i < 4; ++i) {
		char path[1300];
		snprintf(path, sizeof(path), "%s%s", done, files[i]);
		CHECK(holds(path, files[i]));
	}
	CHECK_RESULT(spudfile_directory_remove(g_files, done), SPUDRESULT_SPUDFILE_NOT_EMPTY);
	remove_tree(done);
	SPUDFILE_PATH_KIND kind = (SPUDFILE_PATH_KIND)0;
	CHECK_RESULT(kind_at(done, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
	CHECK_RESULT(kind_at(partial, &kind), SPUDRESULT_SPUDFILE_NOT_FOUND);
}

/* The temporary directory's file system, then each directory named in
 * SPUDFILES_TEST_LOCK_DIRS (';'-separated: another file system, a share),
 * each reported. */
static void test_locks(void) {
	test_locks_in(g_dir);
	printf("spudfiles_test: locks on the temporary directory's file system\n");
	const char *more = getenv("SPUDFILES_TEST_LOCK_DIRS");
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
		printf("spudfiles_test: locks in %s: %s\n", dir, failures == before ? "passed" : "FAILED");
	}
}

int main(void) {
#if SPUDLIB_PLATFORM_WIN32
	const char *tmp = getenv("TEMP");
	if (!tmp)
		tmp = ".";
	snprintf(g_dir, sizeof(g_dir), "%s\\spudfiles_test.%u", tmp, (unsigned)_getpid());
	if (_mkdir(g_dir) != 0) {
		printf("spudfiles_test: skipped, no temporary directory\n");
		return EXIT_SKIPPED;
	}
#else
	const char *tmp = getenv("TMPDIR");
	if (!tmp || !tmp[0])
		tmp = "/tmp";
	snprintf(g_dir, sizeof(g_dir), "%s%sspudfiles_test.XXXXXX", tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/");
	if (!mkdtemp(g_dir)) {
		printf("spudfiles_test: skipped, no temporary directory\n");
		return EXIT_SKIPPED;
	}
#endif
	printf("spudfiles_test: in %s\n", g_dir);

	spudtest_allocator tracker;
	SPUD_ALLOCATOR allocator = spudtest_allocator_make(&tracker);
	test_instance(&allocator);
	spudfile_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator = &allocator;
	CHECK_RESULT(spudfile_instance_create(&desc, &g_files), SPUD_SUCCESS);
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
		spudfile_instance_destroy(g_files);
	}
	CHECK(spudtest_allocator_clean(&tracker));

	CHECK(rmdir(g_dir) == 0);
	if (failures) {
		fprintf(stderr, "spudfiles_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("spudfiles_test: all passed\n");
	return 0;
}
