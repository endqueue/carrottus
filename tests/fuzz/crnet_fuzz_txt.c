/*
 * crnet_fuzz_txt - TXT records (src/net/crnetdiscovery.c), both ways:
 *
 * - Entries to wire bytes: the input split into up to eight "key" and
 *   "value" strings, put through crnet_service_txt_check, against a plain
 *   reading of the same rules (printable ASCII keys without '=', UTF-8
 *   values, 255 bytes an entry, CRNET_SERVICE_TXT_CAPACITY a record, keys
 *   distinct without regard to case); an accepted set written by
 *   crnet_service_txt_write, and the wire bytes read back entry by entry -
 *   on Apple by dns_sd's own TXTRecordGetCount and TXTRecordGetItemAtIndex,
 *   the "second, plainer reading" (README), elsewhere by a plain walk.
 * - Wire bytes to an event: the input as a found TXT record handed to
 *   crnet_discovery_report_service, the resolve event's copy, size and
 *   truncation read back against the input.
 *
 * The input is: byte 0, how many entries (its low three bits, plus one);
 * then the entries' text, each string ended by a 0 byte or the input's end.
 */

#include "crfuzz.h"

#include "net/crnetshared.h"

#if defined(__APPLE__)
#include <dns_sd.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool ref_utf8(const uint8_t *b, size_t n) {
	size_t i = 0;
	while (i < n) {
		uint32_t v;
		size_t   f;
		if (b[i] < 0x80) {
			i++;
			continue;
		} else if ((b[i] & 0xE0) == 0xC0) {
			f = 1, v = b[i] & 0x1F;
		} else if ((b[i] & 0xF0) == 0xE0) {
			f = 2, v = b[i] & 0x0F;
		} else if ((b[i] & 0xF8) == 0xF0) {
			f = 3, v = b[i] & 0x07;
		} else
			return false;
		if (f > n - i - 1)
			return false;
		for (size_t k = 1; k <= f; ++k) {
			if ((b[i + k] & 0xC0) != 0x80)
				return false;
			v = (v << 6) | (b[i + k] & 0x3F);
		}
		static const uint32_t least[4] = {0, 0x80, 0x800, 0x10000};
		if (v < least[f] || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
			return false;
		i += f + 1;
	}
	return true;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool ref_txt_valid(const crnet_service_txt_entry *e, uint32_t count) {
	size_t total = 0;
	for (uint32_t i = 0; i < count; ++i) {
		size_t k = strlen(e[i].key), v = strlen(e[i].value);
		if (k == 0 || k + 1 + v > 255)
			return false;
		for (size_t j = 0; j < k; ++j) {
			unsigned char c = (unsigned char)e[i].key[j];
			if (c < 0x20 || c > 0x7E || c == '=')
				return false;
		}
		if (!ref_utf8((const uint8_t *)e[i].value, v))
			return false;
		for (uint32_t j = 0; j < i; ++j) {
			if (strlen(e[j].key) != k)
				continue;
			bool same = true;
			for (size_t m = 0; m < k && same; ++m)
				same = lower(e[j].key[m]) == lower(e[i].key[m]);
			if (same)
				return false;
		}
		total += 1 + k + 1 + v;
	}
	return total <= CRNET_SERVICE_TXT_CAPACITY;
}

/* The next string of the input: up to a 0 byte or its end. */
static char *next_string(crfuzz_reader *r) {
	size_t n = 0;
	while (n < r->size && r->data[n] != 0)
		n++;
	char *s = crfuzz_terminated(r->data, n);
	r->data += n < r->size ? n + 1 : n;
	r->size -= n < r->size ? n + 1 : n;
	return s;
}

static void check_wire_to_event(const uint8_t *data, size_t size) {
	crtest_allocator tracker;
	CR_ALLOCATOR     allocator = crfuzz_allocator(&tracker, SIZE_MAX);
	struct crnet_discovery_object object;
	memset(&object, 0, sizeof(object));
	object.kind = CRNET_DISCOVERY_RESOLVER;
	strcpy(object.name, "x");
	strcpy(object.type, "_x._udp");
	CRFUZZ_CHECK(crnet_event_ring_init(&object.ring, &allocator, 2, sizeof(crnet_service_resolve_event), offsetof(crnet_service_resolve_event, kind)));
	CRFUZZ_CHECK(crnet_discovery_report_service(&object, NULL, "host.local.", 7, data, size));
	crnet_service_resolve_event event;
	CR_RESULT                   ended = CR_SUCCESS;
	CRFUZZ_CHECK(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_EVENT);
	const size_t kept = size > CRNET_SERVICE_TXT_CAPACITY ? CRNET_SERVICE_TXT_CAPACITY : size;
	CRFUZZ_CHECK(event.kind == CRNET_SERVICE_RESOLVE_EVENT_SERVICE && event.port == 7 && event.txt_size == size);
	CRFUZZ_CHECK(event.txt_truncated == (size > CRNET_SERVICE_TXT_CAPACITY));
	CRFUZZ_CHECK(kept == 0 || memcmp(event.txt, data, kept) == 0);
	for (size_t i = kept; i < CRNET_SERVICE_TXT_CAPACITY; ++i)
		CRFUZZ_CHECK(event.txt[i] == 0);
	crnet_event_ring_free(&object.ring, &allocator);
	CRFUZZ_CHECK(crtest_allocator_clean(&tracker));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	check_wire_to_event(data, size);

	crfuzz_reader           reader = {data, size};
	const uint32_t          count  = (uint32_t)(crfuzz_byte(&reader) & 7) + 1;
	crnet_service_txt_entry entries[8];
	char                   *strings[16];
	for (uint32_t i = 0; i < count; ++i) {
		strings[2 * i]     = next_string(&reader);
		strings[2 * i + 1] = next_string(&reader);
		entries[i].key     = strings[2 * i];
		entries[i].value   = strings[2 * i + 1];
	}
	size_t          wire_size = 0;
	const CR_RESULT checked   = crnet_service_txt_check(entries, count, &wire_size);
	CRFUZZ_CHECK((checked == CR_SUCCESS) == ref_txt_valid(entries, count));
	CRFUZZ_CHECK(checked == CR_SUCCESS || checked == CR_RESULT_DESC_INVALID_PARAMETERS);
	if (checked == CR_SUCCESS) {
		uint8_t wire[CRNET_SERVICE_TXT_CAPACITY];
		CRFUZZ_CHECK(crnet_service_txt_write(entries, count, wire) == wire_size);
#if defined(__APPLE__)
		CRFUZZ_CHECK(TXTRecordGetCount((uint16_t)wire_size, wire) == count);
		for (uint32_t i = 0; i < count; ++i) {
			char        key[256];
			uint8_t     value_size = 0;
			const void *value      = NULL;
			CRFUZZ_CHECK(TXTRecordGetItemAtIndex((uint16_t)wire_size, wire, (uint16_t)i, sizeof(key), key, &value_size, &value) == kDNSServiceErr_NoError);
			CRFUZZ_CHECK(strcmp(key, entries[i].key) == 0);
			CRFUZZ_CHECK(value_size == strlen(entries[i].value) && (value_size == 0 || memcmp(value, entries[i].value, value_size) == 0));
		}
#else
		size_t at = 0;
		for (uint32_t i = 0; i < count; ++i) {
			const size_t k = strlen(entries[i].key), v = strlen(entries[i].value);
			CRFUZZ_CHECK(at < wire_size && wire[at] == k + 1 + v);
			CRFUZZ_CHECK(memcmp(wire + at + 1, entries[i].key, k) == 0 && wire[at + 1 + k] == '=' && memcmp(wire + at + 2 + k, entries[i].value, v) == 0);
			at += 1 + k + 1 + v;
		}
		CRFUZZ_CHECK(at == wire_size);
#endif
	}
	for (uint32_t i = 0; i < 2 * count; ++i)
		free(strings[i]);
	return 0;
}
