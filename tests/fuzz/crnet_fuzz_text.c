/*
 * crnet_fuzz_text - Carrottus Net's small text helpers (src/crnet.c,
 * src/net/crnethttp.c), each against a plain reading of its own:
 *
 * - crnet_utf8_valid, which decides whether a WebSocket text message may
 *   go out, against a decoder that works out each code point and refuses
 *   overlong forms, surrogates and values past U+10FFFF by value;
 * - crnet_error_record, which cuts a stack's message (a server's words
 *   among them) to fit a record: the cut is a prefix, always terminated,
 *   never longer than the record holds, and valid UTF-8 whenever the
 *   message was;
 * - crnet_ascii_equal_no_case and crnet_url_has_scheme;
 * - crnet_headers_accept_encoding, over request headers made from the
 *   input.
 *
 * The input is: byte 0, where the input is split in two for the
 * comparisons; then the bytes.
 */

#include "crfuzz.h"

#include "net/crnetshared.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* UTF-8 as RFC 3629 has it, decided by each sequence's value. */
static bool ref_utf8_valid(const uint8_t *bytes, size_t size) {
	size_t i = 0;
	while (i < size) {
		uint8_t  lead = bytes[i];
		size_t   follow;
		uint32_t value;
		if (lead < 0x80) {
			i++;
			continue;
		} else if ((lead & 0xE0) == 0xC0) {
			follow = 1;
			value  = lead & 0x1F;
		} else if ((lead & 0xF0) == 0xE0) {
			follow = 2;
			value  = lead & 0x0F;
		} else if ((lead & 0xF8) == 0xF0) {
			follow = 3;
			value  = lead & 0x07;
		} else {
			return false; // a continuation byte, or 0xF8 and above
		}
		if (follow > size - i - 1)
			return false;
		for (size_t k = 1; k <= follow; ++k) {
			if ((bytes[i + k] & 0xC0) != 0x80)
				return false;
			value = (value << 6) | (bytes[i + k] & 0x3F);
		}
		static const uint32_t smallest[4] = {0, 0x80, 0x800, 0x10000};
		if (value < smallest[follow])
			return false; // overlong
		if (value >= 0xD800 && value <= 0xDFFF)
			return false; // a surrogate
		if (value > 0x10FFFF)
			return false;
		i += follow + 1;
	}
	return true;
}

static bool ref_equal_no_case(const char *a, const char *b, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		unsigned char x = (unsigned char)a[i];
		unsigned char y = (unsigned char)b[i];
		if (x >= 'A' && x <= 'Z')
			x = (unsigned char)(x + 32);
		if (y >= 'A' && y <= 'Z')
			y = (unsigned char)(y + 32);
		if (x != y)
			return false;
	}
	return true;
}

static void check_error_record(const uint8_t *data, size_t size) {
	char *text = crfuzz_terminated(data, size);

	crnet_error error;
	memset(&error, 0xAB, sizeof(error));
	CR_RESULT result = crnet_error_record(&error, CR_RESULT_NET_RECV_FAILED, CR_PLATFORM_ERROR_SOURCE_CURL, -7, text);
	CRFUZZ_CHECK(result == CR_RESULT_NET_RECV_FAILED);
	CRFUZZ_CHECK(error.result == CR_RESULT_NET_RECV_FAILED);
	CRFUZZ_CHECK(error.source == CR_PLATFORM_ERROR_SOURCE_CURL);
	CRFUZZ_CHECK(error.code == -7);

	const size_t whole  = strlen(text);
	const size_t kept   = strnlen(error.text, sizeof(error.text));
	CRFUZZ_CHECK(kept < sizeof(error.text)); // terminated
	CRFUZZ_CHECK(kept <= whole);
	CRFUZZ_CHECK(memcmp(error.text, text, kept) == 0);
	if (whole < sizeof(error.text))
		CRFUZZ_CHECK(kept == whole);
	if (ref_utf8_valid((const uint8_t *)text, whole)) {
		CRFUZZ_CHECK(ref_utf8_valid((const uint8_t *)error.text, kept));
		// At most one character, four bytes, is given up to the cut.
		if (whole >= sizeof(error.text))
			CRFUZZ_CHECK(kept + 4 >= sizeof(error.text) - 1);
	}
	// The rest of the record is zero, as an empty one is.
	for (size_t i = kept; i < sizeof(error.text); ++i)
		CRFUZZ_CHECK(error.text[i] == '\0');

	// No record to fill is allowed, and only returns the result.
	CRFUZZ_CHECK(crnet_error_record(NULL, CR_RESULT_NET_SEND_FAILED, CR_PLATFORM_ERROR_SOURCE_CURL, 0, text) == CR_RESULT_NET_SEND_FAILED);
	free(text);
}

/* Request headers made from the input: lines split at '\n', each a name, the
 * first ':', and a value. A line with no ':' is an entry with a NULL value,
 * and an empty line one with a NULL name, which the call passes over. */
static void check_accept_encoding(const uint8_t *data, size_t size) {
	char *text = crfuzz_terminated(data, size);
	// No line holds a '\0' of its own: a C string ends there.
	size_t length = strlen(text);

	crnet_header headers[64];
	uint32_t       count     = 0;
	bool           any_other = false;
	bool           identity  = false;
	char          *at        = text;
	char          *end       = text + length;
	while (at <= end && count < 64) {
		char *newline = (char *)memchr(at, '\n', (size_t)(end - at));
		if (newline)
			*newline = '\0';
		char *colon = strchr(at, ':');
		if (colon)
			*colon = '\0';
		headers[count].name  = at[0] || colon ? at : NULL;
		headers[count].value = colon ? colon + 1 : NULL;

		if (headers[count].name && headers[count].value && strlen(headers[count].name) == 15 &&
		    ref_equal_no_case(headers[count].name, "accept-encoding", 15)) {
			const char *value = headers[count].value;
			size_t      n     = strlen(value);
			while (n > 0 && (*value == ' ' || *value == '\t')) {
				value++;
				n--;
			}
			while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t'))
				n--;
			if (n == 8 && ref_equal_no_case(value, "identity", 8))
				identity = true;
			else
				any_other = true;
		}
		count++;
		if (!newline)
			break;
		at = newline + 1;
	}

	enum crnet_accept_encoding found = crnet_headers_accept_encoding(headers, count);
	if (any_other)
		CRFUZZ_CHECK(found == CRNET_ACCEPT_ENCODING_INVALID);
	else if (identity)
		CRFUZZ_CHECK(found == CRNET_ACCEPT_ENCODING_IDENTITY);
	else
		CRFUZZ_CHECK(found == CRNET_ACCEPT_ENCODING_STACK);
	free(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	crfuzz_reader reader = {data, size};
	const uint8_t   split  = crfuzz_byte(&reader);

	// The whole of it, in a block of its own size.
	uint8_t *bytes = (uint8_t *)malloc(reader.size ? reader.size : 1);
	if (!bytes)
		abort();
	if (reader.size)
		memcpy(bytes, reader.data, reader.size);
	CRFUZZ_CHECK(crnet_utf8_valid(bytes, reader.size) == ref_utf8_valid(bytes, reader.size));
	free(bytes);

	check_error_record(reader.data, reader.size);
	check_accept_encoding(reader.data, reader.size);

	// The two halves compared without regard to case, over the shorter.
	size_t first  = reader.size ? split % (reader.size + 1) : 0;
	size_t second = reader.size - first;
	size_t common = first < second ? first : second;
	char  *a      = crfuzz_terminated(reader.data, common);
	char  *b      = crfuzz_terminated(reader.data + first, common);
	CRFUZZ_CHECK(crnet_ascii_equal_no_case(a, b, common) == ref_equal_no_case(a, b, common));
	free(a);
	free(b);

	// The first part as a scheme and the rest as a URL, as C strings.
	char  *scheme = crfuzz_terminated(reader.data, first);
	char  *url    = crfuzz_terminated(reader.data + first, second);
	size_t s      = strlen(scheme);
	size_t u      = strlen(url);
	bool   ref    = u >= s + 3 && ref_equal_no_case(url, scheme, s) && memcmp(url + s, "://", 3) == 0;
	CRFUZZ_CHECK(crnet_url_has_scheme(url, scheme) == ref);
	free(scheme);
	free(url);
	return 0;
}
