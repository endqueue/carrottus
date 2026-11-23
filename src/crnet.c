
#include "crnet.h"
#include "net/crnetshared.h"
#include "crstructbase.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Instance (see net/crnetshared.h)
// --------------------------------------------------------------------------

CR_RESULT crnet_instance_desc_check(
    const crnet_instance_desc *desc,
    crnet_instance *out_instance) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_INSTANCE_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_instance)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_instance = NULL;
	return cr_allocator_check(desc->allocator);
}

// --------------------------------------------------------------------------
// Error records (see net/crnetshared.h)
// --------------------------------------------------------------------------

void crnet_error_clear(crnet_error *error) {
	if (error)
		memset(error, 0, sizeof(*error));
}

CR_RESULT crnet_error_record(
    crnet_error *error,
    CR_RESULT result,
    CR_PLATFORM_ERROR_SOURCE source,
    int64_t code,
    const char *text) {
	if (!error)
		return result;
	memset(error, 0, sizeof(*error));
	error->result = result;
	error->source = source;
	error->code   = code;
	if (text) {
		size_t length = strlen(text);
		if (length > sizeof(error->text) - 1) {
			length = sizeof(error->text) - 1;
			// Not through the middle of a UTF-8 character: back off its
			// continuation bytes, then the lead byte they belonged to.
			while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80)
				length--;
		}
		memcpy(error->text, text, length);
	}
	return result;
}

void crnet_error_get(const crnet_error *record, crnet_error *out_error) {
	if (!out_error)
		return;
	if (record)
		*out_error = *record;
	else
		memset(out_error, 0, sizeof(*out_error));
}


/* Looking a host name up is not here: it is the resolver, which each TCP
 * backend defines for the system resolver it has (crnet_resolver_* in
 * net/backends/posix/crnetsockets.c and net/backends/windows/crnetwindows.c). */

// --------------------------------------------------------------------------
// Text (see net/crnetshared.h)
// --------------------------------------------------------------------------

static char crnet_ascii_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool crnet_ascii_equal_no_case(const char *a, const char *b, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		if (crnet_ascii_lower(a[i]) != crnet_ascii_lower(b[i]))
			return false;
	}
	return true;
}

bool crnet_url_has_scheme(const char *url, const char *scheme) {
	size_t length = strlen(scheme);
	if (strlen(url) < length + 3)
		return false;
	return crnet_ascii_equal_no_case(url, scheme, length) && memcmp(url + length, "://", 3) == 0;
}

bool crnet_utf8_valid(const uint8_t *bytes, size_t size) {
	size_t i = 0;
	while (i < size) {
		uint8_t lead = bytes[i];
		if (lead < 0x80) {
			i++;
			continue;
		}
		// How many bytes follow the lead, and the range the first of them
		// must be in: narrower than 0x80-0xBF where a wider one would let an
		// overlong form, a surrogate or a value past U+10FFFF through.
		size_t  follow;
		uint8_t low  = 0x80;
		uint8_t high = 0xBF;
		if (lead >= 0xC2 && lead <= 0xDF)
			follow = 1;
		else if (lead >= 0xE0 && lead <= 0xEF) {
			follow = 2;
			if (lead == 0xE0)
				low = 0xA0;
			else if (lead == 0xED)
				high = 0x9F;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			follow = 3;
			if (lead == 0xF0)
				low = 0x90;
			else if (lead == 0xF4)
				high = 0x8F;
		} else
			return false;
		if (size - i - 1 < follow)
			return false;
		if (bytes[i + 1] < low || bytes[i + 1] > high)
			return false;
		for (size_t k = 2; k <= follow; ++k) {
			if (bytes[i + k] < 0x80 || bytes[i + k] > 0xBF)
				return false;
		}
		i += follow + 1;
	}
	return true;
}

// --------------------------------------------------------------------------
// TLS desc - what is the same on every backend (see net/crnetshared.h)
// --------------------------------------------------------------------------

CR_RESULT crnet_tls_desc_check(const crnet_tls_desc *tls) {
	bool takes_roots = false;
	switch (tls->trust) {
	case CRNET_TLS_TRUST_SYSTEM:
		break;
	case CRNET_TLS_TRUST_ROOTS:
	case CRNET_TLS_TRUST_SYSTEM_AND_ROOTS:
		takes_roots = true;
		break;
	case CRNET_TLS_TRUST_ANY:
		if (!tls->skip_host_name_check)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		break;
	default:
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	}

	if (takes_roots) {
		if (!tls->roots || tls->root_count == 0)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		for (uint32_t i = 0; i < tls->root_count; ++i) {
			if (!tls->roots[i].data || tls->roots[i].size == 0)
				return CR_RESULT_DESC_INVALID_PARAMETERS;
		}
	} else if (tls->roots || tls->root_count != 0) {
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	}

	if (tls->pin_count > 0 ? !tls->pins : tls->pins != NULL)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	return CR_SUCCESS;
}

/* Reads the DER element that starts at `*at`: its tag, and where its
 * content lies. `*at` moves past the whole element. False when it doesn't
 * fit in `size`. */
static bool crnet_der_element(
    const uint8_t *data,
    size_t size,
    size_t *at,
    uint8_t *out_tag,
    size_t *out_content,
    size_t *out_length) {
	size_t position = *at;
	if (position > size || size - position < 2)
		return false;
	uint8_t tag = data[position++];
	if ((tag & 0x1F) == 0x1F)
		return false; // a tag number too big for one byte; a certificate has none
	size_t length = data[position++];
	if (length & 0x80) {
		// Long form: the low bits say how many bytes hold the length.
		size_t count = length & 0x7F;
		if (count == 0 || count > sizeof(size_t) || count > size - position)
			return false;
		length = 0;
		for (size_t i = 0; i < count; ++i)
			length = (length << 8) | data[position++];
	}
	if (length > size - position)
		return false;
	*out_tag     = tag;
	*out_content = position;
	*out_length  = length;
	*at          = position + length;
	return true;
}

/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
 * and inside tbsCertificate, in order: [0] version (left out of a v1
 * certificate), serialNumber, signature, issuer, validity, subject, and
 * then subjectPublicKeyInfo - the seventh element, or the sixth. */
bool crnet_certificate_public_key_info(
    const void *certificate,
    size_t size,
    const uint8_t **out_info,
    size_t *out_size) {
	const uint8_t *data = (const uint8_t *)certificate;
	uint8_t        tag;
	size_t         content, length;

	size_t at = 0;
	if (!crnet_der_element(data, size, &at, &tag, &content, &length) || tag != 0x30)
		return false;
	data += content;
	size  = length;

	at = 0;
	if (!crnet_der_element(data, size, &at, &tag, &content, &length) || tag != 0x30)
		return false;
	data += content;
	size  = length;

	at = 0;
	if (!crnet_der_element(data, size, &at, &tag, &content, &length))
		return false;
	// The first element is the version or, without one, already the serial number.
	uint32_t to_skip = tag == 0xA0 ? 5 : 4;
	for (uint32_t i = 0; i < to_skip; ++i) {
		if (!crnet_der_element(data, size, &at, &tag, &content, &length))
			return false;
	}

	size_t start = at;
	if (!crnet_der_element(data, size, &at, &tag, &content, &length) || tag != 0x30)
		return false;
	*out_info = data + start;
	*out_size = at - start;
	return true;
}

// --------------------------------------------------------------------------
// Proxy desc - what is the same on every backend (see net/crnetshared.h)
// --------------------------------------------------------------------------

void crnet_proxy_free(const CR_ALLOCATOR *allocator, struct crnet_proxy *proxy) {
	// Both were copied with their terminator and hold no other, so
	// cr_string_free finds the size each was allocated with.
	cr_string_free(allocator, proxy->url);
	cr_string_free(allocator, proxy->host);
	memset(proxy, 0, sizeof(*proxy));
}

/* A copy of `length` bytes of `text` with a terminator, made through
 * `allocator`. `text` holds no '\0' in those bytes. */
static char *crnet_copy_text(const CR_ALLOCATOR *allocator, const char *text, size_t length) {
	char *copy = (char *)cr_allocate(allocator, length + 1, 1);
	if (!copy)
		return NULL;
	memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}

CR_RESULT crnet_proxy_parse(const CR_ALLOCATOR *allocator, const crnet_proxy_desc *desc, struct crnet_proxy *out_proxy) {
	memset(out_proxy, 0, sizeof(*out_proxy));
	out_proxy->mode = desc->mode;
	switch (desc->mode) {
	case CRNET_PROXY_SYSTEM:
	case CRNET_PROXY_NONE:
		// A URL that would be ignored is refused instead.
		return desc->url ? CR_RESULT_DESC_INVALID_PARAMETERS : CR_SUCCESS;
	case CRNET_PROXY_EXPLICIT:
		break;
	default:
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	}

	// "http://host:port" and nothing else. The host is a name, an IPv4
	// address, or an IPv6 address in brackets.
	static const char scheme[] = "http://";
	if (!desc->url || strlen(desc->url) < sizeof(scheme) || !crnet_ascii_equal_no_case(desc->url, scheme, sizeof(scheme) - 1))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	const char *host        = desc->url + sizeof(scheme) - 1;
	size_t      host_length = 0;
	const char *after       = NULL; // the ':' before the port
	if (*host == '[') {
		const char *closing = strchr(host, ']');
		if (!closing)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		host++;
		host_length             = (size_t)(closing - host);
		after                   = closing + 1;
		out_proxy->host_is_ipv6 = true;
	} else {
		after       = strchr(host, ':');
		host_length = after ? (size_t)(after - host) : 0;
	}
	if (host_length == 0 || !after || *after != ':')
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	// No user name before the host, no path after it.
	for (size_t i = 0; i < host_length; ++i) {
		if (host[i] == '@' || host[i] == '/' || host[i] == ' ')
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}

	uint32_t    port   = 0;
	const char *digits = after + 1;
	if (*digits == '\0')
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	for (; *digits != '\0'; ++digits) {
		if (*digits < '0' || *digits > '9')
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		port = port * 10 + (uint32_t)(*digits - '0');
		if (port > 65535)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	if (port == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	out_proxy->port = (uint16_t)port;
	out_proxy->url  = cr_string_copy(allocator, desc->url);
	out_proxy->host = crnet_copy_text(allocator, host, host_length);
	if (!out_proxy->url || !out_proxy->host) {
		crnet_proxy_free(allocator, out_proxy);
		out_proxy->mode = desc->mode;
		return CR_RESULT_OUT_OF_MEMORY;
	}
	return CR_SUCCESS;
}

#if __cplusplus
}
#endif
