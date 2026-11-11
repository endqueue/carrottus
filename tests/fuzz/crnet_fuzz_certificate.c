/*
 * crnet_fuzz_certificate - finding the public key in a server's
 * certificate (crnet_certificate_public_key_info, src/crnet.c).
 *
 * WinHTTP and NSURLSession hand Carrottus Net the server's certificate as DER,
 * and Carrottus Net walks it to the SubjectPublicKeyInfo that a pin is the
 * SHA-256 of, before the pin has been checked: the bytes are whatever the
 * server sent. The whole input is the certificate.
 *
 * Checked: nothing is read outside the input, and a key that is found is a
 * whole DER SEQUENCE inside the input, of the length its own header gives,
 * standing where a plain second walk of the certificate puts it (the
 * seventh element of tbsCertificate, or the sixth without a version).
 */

#include "crfuzz.h"

#include "net/crnetshared.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One DER element at `at`, read the plain way: definite lengths only, the
 * long form in at most eight bytes. False when it doesn't fit in `end`. */
static bool ref_element(const uint8_t *at, const uint8_t *end, uint8_t *out_tag, const uint8_t **out_content, uint64_t *out_length) {
	if (end - at < 2)
		return false;
	uint8_t tag = at[0];
	if ((tag & 0x1F) == 0x1F)
		return false;
	uint64_t length = at[1];
	at += 2;
	if (length & 0x80) {
		uint64_t count = length & 0x7F;
		if (count == 0 || count > 8 || (uint64_t)(end - at) < count)
			return false;
		length = 0;
		for (uint64_t i = 0; i < count; ++i)
			length = (length << 8) | *at++;
	}
	if (length > (uint64_t)(end - at))
		return false;
	*out_tag     = tag;
	*out_content = at;
	*out_length  = length;
	return true;
}

/* Where the key is, by a second walk; false when there is none. */
static bool ref_public_key_info(const uint8_t *data, size_t size, const uint8_t **out_info, size_t *out_size) {
	uint8_t        tag;
	const uint8_t *content;
	uint64_t       length;
	if (!ref_element(data, data + size, &tag, &content, &length) || tag != 0x30)
		return false;
	if (!ref_element(content, content + length, &tag, &content, &length) || tag != 0x30)
		return false;
	const uint8_t *at  = content;
	const uint8_t *end = content + length;
	int key_index = 0;
	for (int index = 0;; ++index) {
		const uint8_t *element_content;
		if (!ref_element(at, end, &tag, &element_content, &length))
			return false;
		const uint8_t *next = element_content + length;
		if (index == 0)
			key_index = tag == 0xA0 ? 6 : 5;
		if (index == key_index) {
			if (tag != 0x30)
				return false;
			*out_info = at;
			*out_size = (size_t)(next - at);
			return true;
		}
		at = next;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	// In a block of its own size, as libFuzzer gives it, so a read past the
	// end is seen. A zero-length input is passed as such, with a pointer
	// that may not be read.
	uint8_t *certificate = (uint8_t *)malloc(size ? size : 1);
	if (!certificate)
		abort();
	if (size)
		memcpy(certificate, data, size);

	const uint8_t *info      = NULL;
	size_t         info_size = 0;
	bool           found     = crnet_certificate_public_key_info(certificate, size, &info, &info_size);

	const uint8_t *ref_info      = NULL;
	size_t         ref_info_size = 0;
	bool           ref_found     = ref_public_key_info(certificate, size, &ref_info, &ref_info_size);

	CRFUZZ_CHECK(found == ref_found);
	if (found) {
		CRFUZZ_CHECK(info == ref_info);
		CRFUZZ_CHECK(info_size == ref_info_size);
		CRFUZZ_CHECK(info >= certificate && info_size >= 2);
		CRFUZZ_CHECK((size_t)(info - certificate) <= size && info_size <= size - (size_t)(info - certificate));
		CRFUZZ_CHECK(info[0] == 0x30);
		// The element found is all of what was handed back, no more.
		uint8_t        tag;
		const uint8_t *content;
		uint64_t       length;
		CRFUZZ_CHECK(ref_element(info, info + info_size, &tag, &content, &length));
		CRFUZZ_CHECK(content + length == info + info_size);
	}
	free(certificate);
	return 0;
}
