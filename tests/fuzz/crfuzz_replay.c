/*
 * crfuzz_replay - runs one fuzz target over files, without libFuzzer.
 *
 * Linked with one target's LLVMFuzzerTestOneInput, it takes directories
 * (and files) on its command line and feeds the target every regular file
 * in each, in the order the system lists them. A directory that doesn't
 * exist is passed over, so a target with no regression yet needs none. A
 * failed check in the target aborts, which ctest reports.
 *
 * This is how each crashing input the fuzzer found stays a test on every
 * platform and compiler, libFuzzer or not (CMakeLists.txt registers one
 * per target, over tests/fuzz/corpus/<target> and
 * tests/fuzz/regressions/<target>).
 */

#include "crfuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static size_t replayed = 0;

/* Feeds the target one file; false when it can't be read. */
static int replay_file(const char *path) {
	FILE *file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "crfuzz_replay: can't open %s\n", path);
		return 0;
	}
	size_t   capacity = 4096;
	size_t   size     = 0;
	uint8_t *data     = (uint8_t *)malloc(capacity);
	if (!data)
		abort();
	for (;;) {
		if (size == capacity) {
			capacity *= 2;
			uint8_t *moved = (uint8_t *)realloc(data, capacity);
			if (!moved)
				abort();
			data = moved;
		}
		size_t got = fread(data + size, 1, capacity - size, file);
		if (got == 0)
			break;
		size += got;
	}
	int failed = ferror(file);
	fclose(file);
	if (failed) {
		free(data);
		fprintf(stderr, "crfuzz_replay: can't read %s\n", path);
		return 0;
	}
	// In a block of exactly its size, as libFuzzer hands an input over.
	uint8_t *exact = (uint8_t *)malloc(size ? size : 1);
	if (!exact)
		abort();
	if (size)
		memcpy(exact, data, size);
	free(data);
	LLVMFuzzerTestOneInput(exact, size);
	free(exact);
	replayed++;
	return 1;
}

static int replay_path(const char *path) {
#if defined(_WIN32)
	DWORD attributes = GetFileAttributesA(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 1; // not there: nothing to replay
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return replay_file(path);
	char pattern[MAX_PATH];
	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	WIN32_FIND_DATAA found;
	HANDLE           search = FindFirstFileA(pattern, &found);
	if (search == INVALID_HANDLE_VALUE)
		return 1;
	int ok = 1;
	do {
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		char file[MAX_PATH];
		snprintf(file, sizeof(file), "%s\\%s", path, found.cFileName);
		ok = replay_file(file) && ok;
	} while (FindNextFileA(search, &found));
	FindClose(search);
	return ok;
#else
	struct stat info;
	if (stat(path, &info) != 0)
		return 1; // not there: nothing to replay
	if (!S_ISDIR(info.st_mode))
		return replay_file(path);
	DIR *dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "crfuzz_replay: can't list %s\n", path);
		return 0;
	}
	int            ok = 1;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue; // ., .. and hidden files such as .DS_Store
		char file[4096];
		snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
		struct stat file_info;
		if (stat(file, &file_info) != 0 || !S_ISREG(file_info.st_mode))
			continue;
		ok = replay_file(file) && ok;
	}
	closedir(dir);
	return ok;
#endif
}

int main(int argc, char **argv) {
	int ok = 1;
	for (int i = 1; i < argc; ++i)
		ok = replay_path(argv[i]) && ok;
	printf("crfuzz_replay: %zu inputs\n", replayed);
	if (replayed == 0) {
		fprintf(stderr, "crfuzz_replay: no input found\n");
		return 1;
	}
	return ok ? 0 : 1;
}
