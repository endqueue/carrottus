/*
 * crnet_fuzz_http_head - a response's header block, as the stacks hand
 * it to Carrottus Net's front end (src/net/crnethttp.c).
 *
 * libcurl and WinHTTP give Carrottus Net the raw block a line at a time, which
 * crnet_head_add_header_line takes apart; NSURLSession gives name and
 * value pairs, which go to crnet_head_add_header. Either way the bytes
 * are the server's. The input is:
 *
 *   byte 0   bit 0: lines (0) or pairs (1); bit 1: each line handed over
 *            with its '\n' (libcurl's way) or without it (WinHTTP's);
 *            bit 2, on Windows: the block as WinHTTP's backend takes it
 *            (crnet_win_add_raw_headers), bits 0 and 1 unread
 *   byte 1   0xFF: no allocation refused; otherwise the allocations after
 *            which every one is refused
 *   rest     the block, lines split at '\n'; in pair mode each line is a
 *            name, the first ':', and a value, taken as they are; in
 *            WinHTTP's mode UTF-16LE code units (an odd last byte left
 *            out), made UTF-8 and split by the backend, whose complete
 *            lines the plain reading takes
 *
 * Checked: nothing is read outside a line, nothing leaks or is freed
 * wrongly (on the out-of-memory paths too), every entry is terminated at
 * its length, and, with no allocation refused, the head holds exactly what
 * a plain reading of the block gives - each name once, without regard to
 * case, a repeat's value joined on with ", " - and crnet_head_get_header
 * finds each of them under its name in any case.
 */

#include "crfuzz.h"

#include "net/crnetshared.h"
#if defined(_WIN32)
#include "net/backends/windows/crnetwinhttphead.h"

#include <windows.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --------------------------------------------------------------------------
// The plain reading, to compare with
// --------------------------------------------------------------------------

typedef struct ref_bytes {
	char  *data;
	size_t length;
} ref_bytes;

typedef struct ref_entry {
	ref_bytes name;
	ref_bytes value;
} ref_entry;

typedef struct ref_head {
	ref_entry *entries;
	size_t     count;
	size_t     capacity;
} ref_head;

static char ref_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool ref_same_name(const char *a, size_t a_length, const char *b, size_t b_length) {
	if (a_length != b_length)
		return false;
	for (size_t i = 0; i < a_length; ++i) {
		if (ref_lower(a[i]) != ref_lower(b[i]))
			return false;
	}
	return true;
}

static ref_bytes ref_copy(const char *data, size_t length) {
	ref_bytes bytes;
	bytes.data = (char *)malloc(length + 1);
	if (!bytes.data)
		abort();
	if (length)
		memcpy(bytes.data, data, length);
	bytes.data[length] = '\0';
	bytes.length       = length;
	return bytes;
}

static void ref_clear(ref_head *head) {
	for (size_t i = 0; i < head->count; ++i) {
		free(head->entries[i].name.data);
		free(head->entries[i].value.data);
	}
	head->count = 0;
}

static void ref_add(ref_head *head, const char *name, size_t name_length, const char *value, size_t value_length) {
	for (size_t i = 0; i < head->count; ++i) {
		ref_entry *entry = &head->entries[i];
		if (!ref_same_name(entry->name.data, entry->name.length, name, name_length))
			continue;
		size_t joined_length = entry->value.length + 2 + value_length;
		char  *joined        = (char *)malloc(joined_length + 1);
		if (!joined)
			abort();
		memcpy(joined, entry->value.data, entry->value.length);
		joined[entry->value.length]     = ',';
		joined[entry->value.length + 1] = ' ';
		if (value_length)
			memcpy(joined + entry->value.length + 2, value, value_length);
		joined[joined_length] = '\0';
		free(entry->value.data);
		entry->value.data   = joined;
		entry->value.length = joined_length;
		return;
	}
	if (head->count == head->capacity) {
		head->capacity = head->capacity ? head->capacity * 2 : 16;
		ref_entry *moved = (ref_entry *)realloc(head->entries, head->capacity * sizeof(ref_entry));
		if (!moved)
			abort();
		head->entries = moved;
	}
	head->entries[head->count].name  = ref_copy(name, name_length);
	head->entries[head->count].value = ref_copy(value, value_length);
	head->count++;
}

static bool ref_blank(char c) {
	return c == ' ' || c == '\t';
}

/* A raw line as RFC 9112 lays a header out, read the plain way: line
 * ending off; a status line starts the block again; "name: value" with the
 * value's blanks trimmed; anything else is not a header. */
static void ref_add_line(ref_head *head, const char *line, size_t length) {
	while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
		length--;
	if (length >= 5 && ref_same_name(line, 5, "HTTP/", 5)) {
		ref_clear(head);
		return;
	}
	const char *colon = (const char *)memchr(line, ':', length);
	if (!colon || colon == line)
		return;
	size_t      name_length = (size_t)(colon - line);
	const char *value       = colon + 1;
	const char *end         = line + length;
	while (value < end && ref_blank(*value))
		value++;
	while (end > value && ref_blank(end[-1]))
		end--;
	ref_add(head, line, name_length, value, (size_t)(end - value));
}

// --------------------------------------------------------------------------
// The target
// --------------------------------------------------------------------------

/* Every entry is a name and a value, each terminated at its length. */
static void check_entries(const struct crnet_http_head *head) {
	CRFUZZ_CHECK(head->entries.count <= head->entries.capacity);
	CRFUZZ_CHECK(head->entries.capacity == 0 || head->entries.data);
	CRFUZZ_CHECK(head->slot_count == 2 * head->entries.capacity);
	for (size_t i = 0; i < head->entries.count; ++i) {
		const struct crnet_http_header_entry *entry = &head->entries.data[i];
		CRFUZZ_CHECK(entry->name && entry->value);
		CRFUZZ_CHECK(entry->name[entry->name_length] == '\0');
		CRFUZZ_CHECK(entry->value[entry->value_length] == '\0');
	}
}

/* `name` in the case each letter is not, which a lookup must not mind. */
static char *flip_case(const char *name, size_t length) {
	char *flipped = (char *)malloc(length + 1);
	if (!flipped)
		abort();
	for (size_t i = 0; i < length; ++i) {
		char c = name[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - ('a' - 'A'));
		else if (c >= 'A' && c <= 'Z')
			c = (char)(c + ('a' - 'A'));
		flipped[i] = c;
	}
	flipped[length] = '\0';
	return flipped;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	crfuzz_reader reader       = {data, size};
	const uint8_t   mode         = crfuzz_byte(&reader);
	const uint8_t   refuse       = crfuzz_byte(&reader);
	const bool      pairs        = (mode & 1) != 0;
	const bool      keep_newline = (mode & 2) != 0;
	const size_t    refuse_after = refuse == 0xFF ? SIZE_MAX : refuse;

	crtest_allocator       tracker;
	const CR_ALLOCATOR     allocator = crfuzz_allocator(&tracker, refuse_after);
	struct crnet_http_head head;
	memset(&head, 0, sizeof(head));
	ref_head ref;
	memset(&ref, 0, sizeof(ref));

	bool           stored = true;
	const uint8_t *at     = reader.data;
	const uint8_t *end    = reader.data + reader.size;
#if defined(_WIN32)
	if (mode & 4) {
		// The code units in a block of their own size, as WinHTTP's is.
		const size_t wide_count = reader.size / 2;
		wchar_t     *raw        = (wchar_t *)malloc(wide_count ? wide_count * sizeof(wchar_t) : 1);
		if (!raw)
			abort();
		for (size_t i = 0; i < wide_count; ++i)
			raw[i] = (wchar_t)(reader.data[2 * i] | (reader.data[2 * i + 1] << 8));
		const CR_RESULT added = crnet_win_add_raw_headers(&allocator, raw, wide_count, &head);
		stored                 = added == CR_SUCCESS;
		CRFUZZ_CHECK(stored || added == CR_RESULT_OUT_OF_MEMORY);
		check_entries(&head);
		// The plain reading: the same conversion, the complete lines.
		if (stored && wide_count > 0) {
			const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, raw, (int)wide_count, NULL, 0, NULL, NULL);
			char     *utf8      = (char *)malloc(utf8_size > 0 ? (size_t)utf8_size : 1);
			if (!utf8)
				abort();
			WideCharToMultiByte(CP_UTF8, 0, raw, (int)wide_count, utf8, utf8_size, NULL, NULL);
			size_t line_start = 0;
			for (size_t i = 0; i < (size_t)utf8_size; ++i) {
				if (utf8[i] != '\n')
					continue;
				ref_add_line(&ref, utf8 + line_start, i - line_start);
				line_start = i + 1;
			}
			free(utf8);
		}
		free(raw);
		at = end;
	}
#endif
	while (at < end && stored) {
		const uint8_t *newline = (const uint8_t *)memchr(at, '\n', (size_t)(end - at));
		size_t         length  = newline ? (size_t)(newline - at) : (size_t)(end - at);
		size_t         handed  = length + (newline && keep_newline ? 1 : 0);
		// Each line in a block of its own size, as a stack's buffer ends.
		char *line = crfuzz_terminated(at, handed);
		if (pairs) {
			const char *colon       = (const char *)memchr(line, ':', length);
			size_t      name_length = colon ? (size_t)(colon - line) : length;
			const char *value       = colon ? colon + 1 : line + length;
			size_t      value_len   = colon ? length - name_length - 1 : 0;
			stored                  = crnet_head_add_header(&allocator, &head, line, name_length, value, value_len);
			if (stored)
				ref_add(&ref, line, name_length, value, value_len);
		} else {
			stored = crnet_head_add_header_line(&allocator, &head, line, handed);
			if (stored)
				ref_add_line(&ref, line, handed);
		}
		free(line);
		check_entries(&head);
		at += newline ? length + 1 : length;
	}
	// Only running out of memory may refuse a line.
	CRFUZZ_CHECK(stored || refuse_after != SIZE_MAX);

	if (stored) {
		CRFUZZ_CHECK(head.entries.count == ref.count);
		for (size_t i = 0; i < ref.count; ++i) {
			const struct crnet_http_header_entry *entry = &head.entries.data[i];
			CRFUZZ_CHECK(entry->name_length == ref.entries[i].name.length);
			CRFUZZ_CHECK(memcmp(entry->name, ref.entries[i].name.data, entry->name_length) == 0);
			CRFUZZ_CHECK(entry->value_length == ref.entries[i].value.length);
			CRFUZZ_CHECK(memcmp(entry->value, ref.entries[i].value.data, entry->value_length) == 0);

			// Looked up by a name with no '\0' in it, in either case, the
			// first entry of that name is the one found: there is only one.
			if (strlen(ref.entries[i].name.data) != ref.entries[i].name.length)
				continue;
			CRFUZZ_CHECK(crnet_head_get_header(&head, ref.entries[i].name.data) == entry->value);
			char *flipped = flip_case(ref.entries[i].name.data, ref.entries[i].name.length);
			CRFUZZ_CHECK(crnet_head_get_header(&head, flipped) == entry->value);
			free(flipped);
		}
	}

	crnet_head_clear(&allocator, &head);
	CRFUZZ_CHECK(head.entries.count == 0 && head.entries.capacity == 0 && head.entries.data == NULL);
	CRFUZZ_CHECK(crtest_allocator_clean(&tracker));
	ref_clear(&ref);
	free(ref.entries);
	return 0;
}
