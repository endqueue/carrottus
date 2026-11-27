
/*
 * Carrottus Net: addresses as values, and the interface listing's front end
 * (crnet.h, "Addresses" and "Interfaces").
 *
 * Every argument check of those calls is here, once, and so is what a
 * listing is: one block holding the list, its interfaces and their
 * addresses, which a backend sizes and fills through
 * crnet_interface_list_alloc (crnetshared.h, "Interfaces"). The backend
 * reads the system's table (crnet_interface_backend_enumerate): getifaddrs
 * in backends/posix/crnetifaddrs.c, GetAdaptersAddresses on Windows.
 *
 * The conversions between a crnet_address and a sockaddr are here too,
 * since inet_pton, inet_ntop and the sockaddr structs are the same on
 * Winsock and BSD sockets, and UDP and discovery need them as well.
 */

#include "crnetshared.h"
#include "crnet.h"
#include "../crstructbase.h"

#if CRNET_EXT_TCP

#if CARROTTUS_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <stdio.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Addresses
// --------------------------------------------------------------------------

bool crnet_address_valid(const crnet_address *address) {
	if (!address || address->reserved != 0)
		return false;
	switch (address->family) {
	case CRNET_ADDRESS_FAMILY_IPV4:
		if (address->scope_id != 0)
			return false;
		for (int i = 4; i < 16; ++i) {
			if (address->bytes[i] != 0)
				return false;
		}
		return true;
	case CRNET_ADDRESS_FAMILY_IPV6:
		return true;
	default:
		return false;
	}
}

bool crnet_address_is_link_local(const uint8_t bytes[16]) { return bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80; }

/* fe80::/64, link-local unicast (RFC 4291, 2.5.6): fe80 and 54 zero bits. */
static bool crnet_address_is_link_local_unicast(const uint8_t bytes[16]) {
	if (bytes[0] != 0xFE || bytes[1] != 0x80)
		return false;
	for (int i = 2; i < 8; ++i) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}

/* The scope_id a Winsock SCOPE_ID's Level would change the meaning of. */
#define CRNET_TCP_SCOPE_LIMIT 0x10000000u

CR_RESULT crnet_tcp_address_check(const crnet_address *address, enum crnet_tcp_address_use use) {
	if (!crnet_address_valid(address))
		return CR_RESULT_NET_INVALID_ADDRESS;
	const uint8_t *bytes = address->bytes;
	const bool     six   = address->family == CRNET_ADDRESS_FAMILY_IPV6;
	bool           zero  = true;
	for (int i = 0; i < 16 && zero; ++i)
		zero = bytes[i] == 0;
	const bool link_local_outside = six && crnet_address_is_link_local(bytes) && !crnet_address_is_link_local_unicast(bytes);
	const bool multicast          = six ? bytes[0] == 0xFF : (bytes[0] & 0xF0) == 0xE0;
	bool       mapped             = six && bytes[10] == 0xFF && bytes[11] == 0xFF;
	for (int i = 0; i < 10 && mapped; ++i)
		mapped = bytes[i] == 0;
	// The address's conditions, each the same result, joined; then the
	// scope's. Two faults in one address have the address's answer.
	if (link_local_outside || multicast || mapped || (use == CRNET_TCP_ADDRESS_REMOTE && (zero || address->port == 0)))
		return CR_RESULT_NET_INVALID_ADDRESS;
	if (six && (address->scope_id >= CRNET_TCP_SCOPE_LIMIT || crnet_address_is_link_local_unicast(bytes) != (address->scope_id != 0)))
		return CR_RESULT_NET_INVALID_SCOPE;
	return CR_SUCCESS;
}

CR_RESULT crnet_address_from_string(
    const char *text,
    uint16_t port,
    uint32_t scope_id,
    crnet_address *out_address) {
	if (!text)
		return CR_RESULT_NET_INVALID_ADDRESS;
	// inet_pton takes no scope on any platform, but a '%' is refused here
	// by rule rather than by what each system's parser happens to do.
	if (strchr(text, '%'))
		return CR_RESULT_NET_INVALID_ADDRESS;

	crnet_address address;
	memset(&address, 0, sizeof(address));
	address.port = port;
	if (inet_pton(AF_INET, text, address.bytes) == 1) {
		if (scope_id != 0)
			return CR_RESULT_NET_INVALID_ADDRESS;
		address.family = CRNET_ADDRESS_FAMILY_IPV4;
	} else if (inet_pton(AF_INET6, text, address.bytes) == 1) {
		address.family   = CRNET_ADDRESS_FAMILY_IPV6;
		address.scope_id = scope_id;
	} else {
		return CR_RESULT_NET_INVALID_ADDRESS;
	}
	if (!out_address)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_address = address;
	return CR_SUCCESS;
}

CR_RESULT crnet_address_to_string(
    const crnet_address *address,
    char out_text[CRNET_MAX_ADDRESS_STRING_LEN]) {
	if (!crnet_address_valid(address))
		return CR_RESULT_NET_INVALID_ADDRESS;
	if (!out_text)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	char text[CRNET_MAX_ADDRESS_STRING_LEN];
	const int family = address->family == CRNET_ADDRESS_FAMILY_IPV4 ? AF_INET : AF_INET6;
	// The bytes are a copy: Winsock's inet_ntop takes a non-const pointer.
	uint8_t bytes[16];
	memcpy(bytes, address->bytes, sizeof(bytes));
	if (!inet_ntop(family, bytes, text, sizeof(text)))
		return CR_RESULT_NET_INVALID_ADDRESS; // can't happen for a valid address and room for 46
	memcpy(out_text, text, strlen(text) + 1);
	return CR_SUCCESS;
}

uint32_t crnet_sockaddr_from_address(const crnet_address *address, void *out_storage, size_t capacity) {
	if (address->family == CRNET_ADDRESS_FAMILY_IPV4) {
		struct sockaddr_in in;
		if (capacity < sizeof(in))
			return 0;
		memset(&in, 0, sizeof(in));
		in.sin_family = AF_INET;
		in.sin_port   = htons(address->port);
		memcpy(&in.sin_addr, address->bytes, 4);
		memcpy(out_storage, &in, sizeof(in));
		return (uint32_t)sizeof(in);
	}
	struct sockaddr_in6 in6;
	if (capacity < sizeof(in6))
		return 0;
	memset(&in6, 0, sizeof(in6));
	in6.sin6_family   = AF_INET6;
	in6.sin6_port     = htons(address->port);
	in6.sin6_scope_id = address->scope_id;
	memcpy(&in6.sin6_addr, address->bytes, 16);
	memcpy(out_storage, &in6, sizeof(in6));
	return (uint32_t)sizeof(in6);
}

bool crnet_address_from_sockaddr(const void *sockaddr, size_t length, uint32_t interface_index, crnet_address *out_address, bool *out_embedded_scope) {
	if (out_embedded_scope)
		*out_embedded_scope = false;
	if (!sockaddr || length < sizeof(struct sockaddr))
		return false;
	// Read through copies: a sockaddr from the system or a fuzzer need not
	// be aligned for the struct it is.
	struct sockaddr base;
	memcpy(&base, sockaddr, sizeof(base));
	crnet_address address;
	memset(&address, 0, sizeof(address));
	if (base.sa_family == AF_INET) {
		struct sockaddr_in in;
		if (length < sizeof(in))
			return false;
		memcpy(&in, sockaddr, sizeof(in));
		address.family = CRNET_ADDRESS_FAMILY_IPV4;
		address.port   = ntohs(in.sin_port);
		memcpy(address.bytes, &in.sin_addr, 4);
	} else if (base.sa_family == AF_INET6) {
		struct sockaddr_in6 in6;
		if (length < sizeof(in6))
			return false;
		memcpy(&in6, sockaddr, sizeof(in6));
		address.family = CRNET_ADDRESS_FAMILY_IPV6;
		address.port   = ntohs(in6.sin6_port);
		memcpy(address.bytes, &in6.sin6_addr, 16);
		if (crnet_address_is_link_local(address.bytes)) {
			// A KAME stack (Apple's, the BSDs') may hand a link-local
			// address over with its interface index in bytes 2 and 3 and
			// sin6_scope_id 0. The scope is then taken from there; either
			// way bytes 2 and 3 go back to 0, which is what they are in a
			// link-local address on the wire (RFC 4291, 2.5.6).
			const uint32_t embedded = ((uint32_t)address.bytes[2] << 8) | address.bytes[3];
#if CARROTTUS_PLATFORM_WIN32
			// Winsock's sin6_scope_id is a SCOPE_ID: a 28-bit Zone and a
			// 4-bit Level above it (ws2def.h). The Zone alone is the
			// interface, as on every other platform; it is taken before the
			// fallbacks for a scope of 0.
			uint32_t scope = in6.sin6_scope_id & 0x0FFFFFFFu;
#else
			uint32_t scope = in6.sin6_scope_id;
#endif
			if (scope == 0 && embedded != 0) {
				scope = embedded;
				if (out_embedded_scope)
					*out_embedded_scope = true;
			}
			// A link-local address the system gave with no scope at all is
			// still on the interface it was listed under.
			if (scope == 0)
				scope = interface_index;
			address.bytes[2] = 0;
			address.bytes[3] = 0;
			address.scope_id = scope;
		}
		// Every other address is global in scope here: scope_id 0.
	} else {
		return false;
	}
	*out_address = address;
	return true;
}

CR_RESULT crnet_tcp_peer_from_sockaddr(const void *sockaddr, size_t length, int family, crnet_address *out_address) {
	memset(out_address, 0, sizeof(*out_address));
	crnet_address peer;
	// getpeername names no interface: 0, so a link-local peer the system gave
	// no scope comes out with none and is refused below.
	if (!crnet_address_from_sockaddr(sockaddr, length, 0, &peer, NULL))
		return CR_RESULT_GENERAL_FAILURE;
	const int peer_family = peer.family == CRNET_ADDRESS_FAMILY_IPV4 ? AF_INET : AF_INET6;
	if (peer_family != family)
		return CR_RESULT_GENERAL_FAILURE;
	if (peer.family == CRNET_ADDRESS_FAMILY_IPV6 && crnet_address_is_link_local(peer.bytes) &&
	    (peer.scope_id == 0 || !crnet_address_is_link_local_unicast(peer.bytes)))
		return CR_RESULT_GENERAL_FAILURE;
	*out_address = peer;
	return CR_SUCCESS;
}

uint32_t crnet_netmask_prefix_length(const void *netmask, size_t length, int family) {
	if (!netmask)
		return CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN;
	const uint8_t *bytes = NULL;
	size_t         count = 0;
	// Where the mask's bytes start inside its sockaddr. A netmask's own
	// sa_family may be 0 on some systems (Apple leaves it so), so the
	// address's family says which struct it is.
	if (family == AF_INET) {
		if (length < offsetof(struct sockaddr_in, sin_addr) + 4)
			return CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN;
		bytes = (const uint8_t *)netmask + offsetof(struct sockaddr_in, sin_addr);
		count = 4;
	} else if (family == AF_INET6) {
		if (length < offsetof(struct sockaddr_in6, sin6_addr) + 16)
			return CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN;
		bytes = (const uint8_t *)netmask + offsetof(struct sockaddr_in6, sin6_addr);
		count = 16;
	} else {
		return CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN;
	}
	// The bits set (crnet_interface_draft.h's table), which for a mask whose
	// ones aren't contiguous is not the same as its leading ones: the
	// system's mask is passed on as a count, not judged.
	uint32_t bits = 0;
	for (size_t i = 0; i < count; ++i) {
		uint8_t byte = bytes[i];
		while (byte) {
			bits += byte & 1u;
			byte >>= 1;
		}
	}
	return bits;
}

// --------------------------------------------------------------------------
// The list
// --------------------------------------------------------------------------

/* The Debug reader count (crnet.h, "Interfaces", Thread:): plain atomics on
 * an int32_t, since this header-free file is C on every compiler. */
#if _DEBUG
#if defined(_MSC_VER)
#include <intrin.h>
static void crnet_debug_count_add(volatile int32_t *count, long delta) { _InterlockedExchangeAdd((volatile long *)count, delta); }
static long crnet_debug_count_load(volatile int32_t *count) { return _InterlockedOr((volatile long *)count, 0); }
#else
static void crnet_debug_count_add(volatile int32_t *count, long delta) { __atomic_fetch_add(count, (int32_t)delta, __ATOMIC_SEQ_CST); }
static long crnet_debug_count_load(volatile int32_t *count) { return __atomic_load_n(count, __ATOMIC_SEQ_CST); }
#endif
#define CRNET_READ_BEGIN(list) crnet_debug_count_add(&(list)->debug_readers, 1)
#define CRNET_READ_END(list) crnet_debug_count_add(&(list)->debug_readers, -1)
#else
#define CRNET_READ_BEGIN(list) ((void)0)
#define CRNET_READ_END(list) ((void)0)
#endif

CR_RESULT crnet_interface_list_alloc(crnet_instance instance, uint32_t interface_count, uint32_t address_count, struct crnet_interface_list_t **out_list) {
	*out_list = NULL;
	// One block: the list, then its interfaces, then its addresses, each
	// start aligned for what is there, so a destroy frees one block and a
	// Debug destroy keeps all of it readable in its instance's quarantine.
	const size_t alignment         = CR_ALIGNOF(struct crnet_interface_list_t) > CR_ALIGNOF(struct crnet_interface_entry)
	                                     ? CR_ALIGNOF(struct crnet_interface_list_t)
	                                     : CR_ALIGNOF(struct crnet_interface_entry);
	const size_t interfaces_offset = (sizeof(struct crnet_interface_list_t) + CR_ALIGNOF(struct crnet_interface_entry) - 1) &
	                                 ~(size_t)(CR_ALIGNOF(struct crnet_interface_entry) - 1);
	const size_t interfaces_size   = (size_t)interface_count * sizeof(struct crnet_interface_entry);
	const size_t addresses_offset  = (interfaces_offset + interfaces_size + CR_ALIGNOF(struct crnet_interface_address_entry) - 1) &
	                                ~(size_t)(CR_ALIGNOF(struct crnet_interface_address_entry) - 1);
	const size_t size              = addresses_offset + (size_t)address_count * sizeof(struct crnet_interface_address_entry);

	uint8_t *block = (uint8_t *)cr_allocate_zeroed(&instance->head.allocator, size, alignment);
	if (!block)
		return CR_RESULT_OUT_OF_MEMORY;
	struct crnet_interface_list_t *list = (struct crnet_interface_list_t *)block;
	list->head.allocator                 = instance->head.allocator;
	CR_BIRTH_IN(list, &instance->quarantine);
	list->block_size      = size;
	list->block_alignment = alignment;
	list->interfaces      = (struct crnet_interface_entry *)(block + interfaces_offset);
	list->addresses       = (struct crnet_interface_address_entry *)(block + addresses_offset);
	*out_list             = list;
	return CR_SUCCESS;
}

void crnet_interface_list_free(struct crnet_interface_list_t *list) {
	if (!list)
		return;
	const CR_ALLOCATOR allocator = list->head.allocator;
	const size_t       size      = list->block_size;
	const size_t       alignment = list->block_alignment;
	cr_string_free(&allocator, list->head.debug_name);
	// Kept in its instance's quarantine in Debug (crstale.h); freed at once
	// in Release, which has none.
#if _DEBUG
	cr_retire(CR_QUARANTINE_OF(list), &allocator, list, size, alignment);
#else
	cr_free(&allocator, list, size, alignment);
#endif
}

CR_RESULT crnet_interface_enumerate(
    crnet_instance instance,
    crnet_interface_list *out_list,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_interface_enumerate", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!out_list)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_list = NULL;
	struct crnet_interface_list_t *list = NULL;
	const CR_RESULT result               = crnet_interface_backend_enumerate(instance, &list, out_error);
	if (CR_FAILED(result))
		return result;
	*out_list = list;
	return CR_SUCCESS;
}

CR_RESULT crnet_interface_list_get_count(
    crnet_interface_list list,
    uint32_t *out_count) {
	if (!list)
		return CR_RESULT_NET_INVALID_INTERFACE_LIST;
	CRNET_READ_BEGIN(list);
	CR_RESULT result = CR_SUCCESS;
	if (CR_STALE(list, "crnet_interface_list_get_count", "list"))
		result = CR_RESULT_NET_INVALID_INTERFACE_LIST;
	else if (!out_count)
		result = CR_RESULT_NULL_OUTPUT_PARAMETER;
	else
		*out_count = list->interface_count;
	CRNET_READ_END(list);
	return result;
}

CR_RESULT crnet_interface_list_get_properties(
    crnet_interface_list list,
    uint32_t position,
    crnet_interface_properties *out_properties) {
	if (!list)
		return CR_RESULT_NET_INVALID_INTERFACE_LIST;
	CRNET_READ_BEGIN(list);
	CR_RESULT result = CR_SUCCESS;
	if (CR_STALE(list, "crnet_interface_list_get_properties", "list"))
		result = CR_RESULT_NET_INVALID_INTERFACE_LIST;
	else if (position >= list->interface_count)
		result = CR_RESULT_INDEX_OUT_OF_RANGE;
	else if (!out_properties)
		result = CR_RESULT_NULL_OUTPUT_PARAMETER;
	else
		result = cr_struct_out_check(&out_properties->base, CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES);
	if (result == CR_SUCCESS) {
		const struct crnet_interface_entry *entry = &list->interfaces[position];
		memcpy(out_properties->name, entry->name, sizeof(out_properties->name));
		memcpy(out_properties->display_name, entry->display_name, sizeof(out_properties->display_name));
		out_properties->index         = entry->index;
		out_properties->ipv6_index    = entry->ipv6_index;
		out_properties->flags         = entry->flags;
		out_properties->mtu           = entry->mtu;
		out_properties->address_count = entry->address_count;
	}
	CRNET_READ_END(list);
	return result;
}

CR_RESULT crnet_interface_list_get_address(
    crnet_interface_list list,
    uint32_t position,
    uint32_t address_position,
    crnet_interface_address *out_address) {
	if (!list)
		return CR_RESULT_NET_INVALID_INTERFACE_LIST;
	CRNET_READ_BEGIN(list);
	CR_RESULT result = CR_SUCCESS;
	if (CR_STALE(list, "crnet_interface_list_get_address", "list"))
		result = CR_RESULT_NET_INVALID_INTERFACE_LIST;
	else if (position >= list->interface_count || address_position >= list->interfaces[position].address_count)
		result = CR_RESULT_INDEX_OUT_OF_RANGE;
	else if (!out_address)
		result = CR_RESULT_NULL_OUTPUT_PARAMETER;
	else
		result = cr_struct_out_check(&out_address->base, CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS);
	if (result == CR_SUCCESS) {
		const struct crnet_interface_address_entry *entry = &list->addresses[list->interfaces[position].first_address + address_position];
		out_address->address                              = entry->address;
		out_address->prefix_length                        = entry->prefix_length;
	}
	CRNET_READ_END(list);
	return result;
}

void crnet_interface_list_destroy(crnet_interface_list list) {
	if (!list)
		return;
	if (CR_STALE(list, "crnet_interface_list_destroy", "list"))
		return;
	// Dead first, then the count read: a read that begins after this sees
	// the tag and refuses, and one already inside is counted.
	CR_KILL(list);
#if _DEBUG
	const long readers = crnet_debug_count_load(&list->debug_readers);
	if (readers != 0) {
		crnet_debug_count_add(&list->debug_overlaps, 1);
		fprintf(stderr,
		        "crnet: crnet_interface_list_destroy: called while %ld read%s of the same list %s inside on another thread (crnet.h, \"Interfaces\": "
		        "a read may overlap a read, never the destroy)\n",
		        readers, readers == 1 ? "" : "s", readers == 1 ? "is" : "are");
	}
#endif
	crnet_interface_list_free(list);
}

#if _DEBUG
uint32_t crnet_interface_list_debug_embedded_scopes(crnet_interface_list list) { return list ? list->debug_embedded_scopes : 0; }
uint32_t crnet_interface_list_debug_overlaps(crnet_interface_list list) { return list ? (uint32_t)crnet_debug_count_load(&list->debug_overlaps) : 0; }
#endif

#if __cplusplus
}
#endif

#endif // CRNET_EXT_TCP
