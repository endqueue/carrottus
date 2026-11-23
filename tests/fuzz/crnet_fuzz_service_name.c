/*
 * crnet_fuzz_service_name - the names and types discovery takes and is
 * handed (src/net/crnetdiscovery.c), each against a plain reading:
 *
 * - crnet_service_name_valid: 1 to 63 bytes, UTF-8, no ASCII control
 *   character (what every name handed over in a FOUND must be);
 * - crnet_service_type_valid and crnet_service_type_matches;
 * - crnet_dns_label_unescape, which reads the first label of a full name as
 *   dns_sd's replies carry it ("\." "\\" "\ddd"), against a decoder of its
 *   own; and crnet_discovery_full_name_matches against a name escaped by a
 *   plain escaper and read back;
 * - crnet_discovery_report_found with the input as a name: dropped exactly
 *   when the name check refuses it.
 *
 * The input is: byte 0, which of its bytes start the type; the rest, the
 * name and then the type.
 */

#include "crfuzz.h"

#include "net/crnetshared.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

static bool ref_name_valid(const uint8_t *b, size_t n) {
	if (n == 0 || n > 63)
		return false;
	for (size_t i = 0; i < n; ++i) {
		if (b[i] < 0x20 || b[i] == 0x7F)
			return false;
	}
	return ref_utf8(b, n);
}

static bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool ref_type_valid(const char *t, size_t n) {
	// "_" name "." "_udp"|"_tcp", the name 1-15 of [A-Za-z0-9-], ends alnum.
	if (n < 7 || t[0] != '_')
		return false;
	const char *dot = NULL;
	for (size_t i = 1; i < n; ++i) {
		if (t[i] == '.') {
			dot = t + i;
			break;
		}
	}
	if (!dot)
		return false;
	const size_t name = (size_t)(dot - t) - 1;
	if (name < 1 || name > 15 || !alnum(t[1]) || !alnum(dot[-1]))
		return false;
	for (size_t i = 1; i <= name; ++i) {
		if (!alnum(t[i]) && t[i] != '-')
			return false;
	}
	const size_t rest = n - (size_t)(dot - t);
	if (rest != 5)
		return false;
	char proto[6];
	for (int i = 0; i < 5; ++i)
		proto[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + 32 : dot[i]);
	proto[5] = '\0';
	return strcmp(proto, "._udp") == 0 || strcmp(proto, "._tcp") == 0;
}

/* A label's escaped form: '.' and '\' escaped, bytes below 0x21 and 0x7F
 * as \ddd, the rest as they are. */
static size_t ref_escape(const uint8_t *b, size_t n, char *out) {
	size_t o = 0;
	for (size_t i = 0; i < n; ++i) {
		if (b[i] == '.' || b[i] == '\\') {
			out[o++] = '\\';
			out[o++] = (char)b[i];
		} else if (b[i] < 0x21 || b[i] == 0x7F) {
			o += (size_t)snprintf(out + o, 5, "\\%03u", (unsigned)b[i]);
		} else
			out[o++] = (char)b[i];
	}
	out[o] = '\0';
	return o;
}

/* A plain reading of one escaped label: SIZE_MAX where crnet's refuses. */
static size_t ref_unescape(const char *t, size_t n, uint8_t *out, size_t cap, size_t *used) {
	size_t i = 0, o = 0;
	while (i < n && t[i] != '.') {
		unsigned v;
		if (t[i] != '\\')
			v = (uint8_t)t[i++];
		else if (i + 1 >= n)
			return SIZE_MAX;
		else if (t[i + 1] >= '0' && t[i + 1] <= '9') {
			if (i + 3 >= n + 0 && n - i < 4)
				return SIZE_MAX;
			if (!(t[i + 2] >= '0' && t[i + 2] <= '9') || !(t[i + 3] >= '0' && t[i + 3] <= '9'))
				return SIZE_MAX;
			v = (unsigned)(t[i + 1] - '0') * 100 + (unsigned)(t[i + 2] - '0') * 10 + (unsigned)(t[i + 3] - '0');
			if (v > 255)
				return SIZE_MAX;
			i += 4;
		} else {
			v = (uint8_t)t[i + 1];
			i += 2;
		}
		if (o == cap)
			return SIZE_MAX;
		out[o++] = (uint8_t)v;
	}
	*used = i;
	return o;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	crfuzz_reader reader = {data, size};
	const uint8_t split  = crfuzz_byte(&reader);
	const size_t  first  = reader.size ? split % (reader.size + 1) : 0;

	// The name: its bytes as they are (a 0 among them stops it as C text).
	char        *name   = crfuzz_terminated(reader.data, first);
	const size_t length = strlen(name);
	CRFUZZ_CHECK(crnet_service_name_valid(name, length) == ref_name_valid((const uint8_t *)name, length));

	// The type.
	char        *type        = crfuzz_terminated(reader.data + first, reader.size - first);
	const size_t type_length = strlen(type);
	CRFUZZ_CHECK(crnet_service_type_valid(type, type_length) == ref_type_valid(type, type_length));
	if (crnet_service_type_valid(type, type_length)) {
		char dotted[40];
		snprintf(dotted, sizeof(dotted), "%s.", type);
		CRFUZZ_CHECK(crnet_service_type_matches(dotted, type));
		CRFUZZ_CHECK(crnet_service_type_matches(type, type));
	}

	// The whole input as an escaped label, both readings agreeing.
	char        *text = crfuzz_terminated(reader.data, reader.size);
	const size_t n    = strlen(text);
	char         label[CRNET_SERVICE_NAME_CAPACITY];
	uint8_t      ref_label[CRNET_SERVICE_NAME_CAPACITY];
	size_t       used = 0, ref_used = 0;
	const size_t got  = crnet_dns_label_unescape(text, n, label, sizeof(label) - 1, &used);
	const size_t want = ref_unescape(text, n, ref_label, sizeof(ref_label) - 1, &ref_used);
	CRFUZZ_CHECK(got == want);
	if (got != SIZE_MAX) {
		CRFUZZ_CHECK(used == ref_used && memcmp(label, ref_label, got) == 0);
		CRFUZZ_CHECK(used == n || text[used] == '.');
	}

	// A valid name escaped and read back as a full name.
	if (crnet_service_name_valid(name, length)) {
		char full[64 * 4 + 40];
		size_t at = ref_escape((const uint8_t *)name, length, full);
		snprintf(full + at, sizeof(full) - at, "._crnettest._udp.local.");
		CRFUZZ_CHECK(crnet_discovery_full_name_matches(full, name, "_crnettest._udp"));
		CRFUZZ_CHECK(!crnet_discovery_full_name_matches(full, name, "_other._udp"));
	}

	// The reply path: dropped exactly when the name is refused.
	crtest_allocator tracker;
	CR_ALLOCATOR     allocator = crfuzz_allocator(&tracker, SIZE_MAX);
	struct crnet_discovery_object object;
	memset(&object, 0, sizeof(object));
	object.kind = CRNET_DISCOVERY_BROWSER;
	strcpy(object.type, "_crnettest._udp");
	CRFUZZ_CHECK(crnet_event_ring_init(&object.ring, &allocator, 1, sizeof(crnet_service_browse_event), offsetof(crnet_service_browse_event, kind)));
	const bool taken = crnet_discovery_report_found(&object, true, 1, name, "_crnettest._udp.");
	CRFUZZ_CHECK(taken == ref_name_valid((const uint8_t *)name, length));
	CRFUZZ_CHECK(crnet_event_ring_ready(&object.ring) == taken);
	crnet_event_ring_free(&object.ring, &allocator);
	CRFUZZ_CHECK(crtest_allocator_clean(&tracker));

	free(name);
	free(type);
	free(text);
	return 0;
}
