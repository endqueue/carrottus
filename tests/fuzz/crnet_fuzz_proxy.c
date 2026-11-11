/*
 * crnet_fuzz_proxy - a proxy desc taken apart (crnet_proxy_parse,
 * src/crnet.c). The URL is the caller's, not the network's, but it is
 * text Carrottus Net reads byte by byte into a host and a port, so it is fuzzed
 * with the rest.
 *
 * The input is: byte 0, the mode (any value, the invalid ones included);
 * byte 1, the allocations after which every one is refused (0xFF: none);
 * byte 2, bit 0: the URL is NULL; then the URL, up to its first '\0'.
 *
 * Checked: nothing is read past the URL's terminator, nothing leaks or is
 * freed wrongly (on the out-of-memory paths too), and the answer is the one
 * a plain reading of crnet.h's rules gives: "http://host:port" with the
 * scheme in any case, the host a name or address with no '@', '/' or ' ',
 * or an IPv6 address in brackets, and a port of 1 to 65535 in decimal
 * digits, with nothing after it.
 */

#include "crfuzz.h"

#include "net/crnetshared.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct ref_proxy {
	CR_RESULT  result;
	const char *host;
	size_t      host_length;
	bool        host_is_ipv6;
	uint32_t    port;
} ref_proxy;

static ref_proxy ref_parse(CRNET_PROXY mode, const char *url) {
	ref_proxy ref;
	memset(&ref, 0, sizeof(ref));
	ref.result = CR_RESULT_DESC_INVALID_PARAMETERS;
	if (mode == CRNET_PROXY_SYSTEM || mode == CRNET_PROXY_NONE) {
		if (!url)
			ref.result = CR_SUCCESS;
		return ref;
	}
	if (mode != CRNET_PROXY_EXPLICIT || !url)
		return ref;

	const char *prefix = "http://";
	for (size_t i = 0; i < 7; ++i) {
		char c = url[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		if (c != prefix[i])
			return ref; // also where the URL ends early: '\0' matches nothing
	}
	const char *host = url + 7;
	const char *rest;
	if (host[0] == '[') {
		const char *closing = host + 1;
		while (*closing && *closing != ']')
			closing++;
		if (!*closing)
			return ref;
		ref.host         = host + 1;
		ref.host_length  = (size_t)(closing - ref.host);
		ref.host_is_ipv6 = true;
		rest             = closing + 1;
	} else {
		const char *colon = host;
		while (*colon && *colon != ':')
			colon++;
		ref.host        = host;
		ref.host_length = (size_t)(colon - host);
		rest            = colon;
	}
	if (ref.host_length == 0 || *rest != ':')
		return ref;
	for (size_t i = 0; i < ref.host_length; ++i) {
		if (ref.host[i] == '@' || ref.host[i] == '/' || ref.host[i] == ' ')
			return ref;
	}
	const char *digits = rest + 1;
	if (!*digits)
		return ref;
	uint32_t port = 0;
	for (; *digits; ++digits) {
		if (*digits < '0' || *digits > '9')
			return ref;
		port = port * 10 + (uint32_t)(*digits - '0');
		if (port > 65535)
			return ref;
	}
	if (port == 0)
		return ref;
	ref.port   = port;
	ref.result = CR_SUCCESS;
	return ref;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	crfuzz_reader     reader       = {data, size};
	const CRNET_PROXY mode         = (CRNET_PROXY)(crfuzz_byte(&reader) % 5);
	const uint8_t       refuse       = crfuzz_byte(&reader);
	const bool          null_url     = (crfuzz_byte(&reader) & 1) != 0;
	const size_t        refuse_after = refuse == 0xFF ? SIZE_MAX : refuse;

	char *url = crfuzz_terminated(reader.data, reader.size);

	crtest_allocator   tracker;
	const CR_ALLOCATOR allocator = crfuzz_allocator(&tracker, refuse_after);
	crnet_proxy_desc   desc;
	desc.mode = mode;
	desc.url  = null_url ? NULL : url;

	struct crnet_proxy proxy;
	memset(&proxy, 0xAB, sizeof(proxy));
	CR_RESULT result = crnet_proxy_parse(&allocator, &desc, &proxy);
	ref_proxy  ref    = ref_parse(mode, desc.url);

	CRFUZZ_CHECK(proxy.mode == mode);
	if (result == CR_RESULT_OUT_OF_MEMORY) {
		// Only where an allocation was refused, and only for a URL that
		// would otherwise have been taken; nothing is left to free.
		CRFUZZ_CHECK(refuse_after != SIZE_MAX);
		CRFUZZ_CHECK(ref.result == CR_SUCCESS && mode == CRNET_PROXY_EXPLICIT);
		CRFUZZ_CHECK(proxy.url == NULL && proxy.host == NULL);
	} else {
		CRFUZZ_CHECK(result == ref.result);
	}
	if (result == CR_SUCCESS && mode == CRNET_PROXY_EXPLICIT) {
		CRFUZZ_CHECK(proxy.url && strcmp(proxy.url, url) == 0);
		CRFUZZ_CHECK(proxy.host && strlen(proxy.host) == ref.host_length);
		CRFUZZ_CHECK(memcmp(proxy.host, ref.host, ref.host_length) == 0);
		CRFUZZ_CHECK(proxy.host_is_ipv6 == ref.host_is_ipv6);
		CRFUZZ_CHECK(proxy.port == ref.port);
	} else if (result != CR_RESULT_OUT_OF_MEMORY) {
		// Refused, or a mode with no URL: nothing was copied.
		CRFUZZ_CHECK(proxy.url == NULL && proxy.host == NULL && proxy.port == 0);
	}

	// What the backends do with it whatever came back.
	crnet_proxy_free(&allocator, &proxy);
	CRFUZZ_CHECK(crtest_allocator_clean(&tracker));
	free(url);
	return 0;
}
