/*
 * crnet_fuzz_ifaddr - what the interface listing and UDP make of a
 * system's sockaddr (src/net/crnetinterfaces.c):
 *
 * - crnet_address_from_sockaddr, over a sockaddr_in or sockaddr_in6 built
 *   from the input at any length, its scope taken from sin6_scope_id, a
 *   KAME stack's bytes 2 and 3, or the interface's index, against a plain
 *   reading of those rules; the result a valid crnet_address;
 * - crnet_netmask_prefix_length over a netmask from the input, contiguous
 *   or not, against a count of its bits by another means.
 *
 * The input is: byte 0, the family (even IPv4, odd IPv6) and the length
 * (its upper bits); bytes 1 to 4, the interface index; the rest, the
 * sockaddr's bytes, then the netmask's.
 */

#include "crfuzz.h"

#include "net/crnetshared.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ref_bits(const uint8_t *b, size_t n) {
	uint32_t total = 0;
	for (size_t i = 0; i < n; ++i)
		total += (uint32_t)__builtin_popcount(b[i]);
	return total;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	crfuzz_reader reader = {data, size};
	const uint8_t shape  = crfuzz_byte(&reader);
	uint32_t      index  = 0;
	for (int i = 0; i < 4; ++i)
		index = (index << 8) | crfuzz_byte(&reader);
	const bool six = (shape & 1) != 0;

	struct sockaddr_storage storage;
	memset(&storage, 0, sizeof(storage));
	const size_t full = six ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	for (size_t i = 0; i < full; ++i)
		((uint8_t *)&storage)[i] = crfuzz_byte(&reader);
	((struct sockaddr *)&storage)->sa_family = six ? AF_INET6 : AF_INET;
	// Any length up to the struct's: a short one is refused.
	const size_t length = (size_t)(shape >> 1) % (full + 1);

	// In a block of its own length, so a read past it is ASan's to see.
	uint8_t *block = (uint8_t *)malloc(length ? length : 1);
	if (!block)
		abort();
	memcpy(block, &storage, length);
	crnet_address address;
	memset(&address, 0xEE, sizeof(address));
	bool         embedded = false;
	const bool   read     = crnet_address_from_sockaddr(block, length, index, &address, &embedded);
	free(block);
	CRFUZZ_CHECK(read == (length >= full));
	if (read) {
		CRFUZZ_CHECK(crnet_address_valid(&address));
		if (!six) {
			struct sockaddr_in in;
			memcpy(&in, &storage, sizeof(in));
			CRFUZZ_CHECK(address.family == CRNET_ADDRESS_FAMILY_IPV4 && address.port == ntohs(in.sin_port) && memcmp(address.bytes, &in.sin_addr, 4) == 0);
			CRFUZZ_CHECK(!embedded);
		} else {
			struct sockaddr_in6 in6;
			memcpy(&in6, &storage, sizeof(in6));
			uint8_t bytes[16];
			memcpy(bytes, &in6.sin6_addr, 16);
			const bool link_local = bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80;
			CRFUZZ_CHECK(address.family == CRNET_ADDRESS_FAMILY_IPV6 && address.port == ntohs(in6.sin6_port));
			if (link_local) {
				const uint32_t in_bytes = ((uint32_t)bytes[2] << 8) | bytes[3];
				const uint32_t want     = in6.sin6_scope_id ? in6.sin6_scope_id : in_bytes ? in_bytes : index;
				CRFUZZ_CHECK(address.scope_id == want);
				CRFUZZ_CHECK(embedded == (in6.sin6_scope_id == 0 && in_bytes != 0));
				CRFUZZ_CHECK(address.bytes[2] == 0 && address.bytes[3] == 0);
				bytes[2] = bytes[3] = 0;
			} else {
				CRFUZZ_CHECK(address.scope_id == 0 && !embedded);
			}
			CRFUZZ_CHECK(memcmp(address.bytes, bytes, 16) == 0);
		}
	}

	// The netmask: what is left of the input, as a sockaddr of the family.
	uint8_t mask[sizeof(struct sockaddr_in6)];
	memset(mask, 0, sizeof(mask));
	for (size_t i = 0; i < sizeof(mask); ++i)
		mask[i] = crfuzz_byte(&reader);
	const size_t mask_length = (size_t)(shape >> 1) % (sizeof(mask) + 1);
	const size_t offset      = six ? offsetof(struct sockaddr_in6, sin6_addr) : offsetof(struct sockaddr_in, sin_addr);
	const size_t bytes       = six ? 16 : 4;
	const uint32_t prefix    = crnet_netmask_prefix_length(mask, mask_length, six ? AF_INET6 : AF_INET);
	if (mask_length < offset + bytes)
		CRFUZZ_CHECK(prefix == CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN);
	else
		CRFUZZ_CHECK(prefix == ref_bits(mask + offset, bytes) && prefix <= bytes * 8);
	CRFUZZ_CHECK(crnet_netmask_prefix_length(NULL, 0, AF_INET) == CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN);
	return 0;
}
