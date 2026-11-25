#if CARROTTUS_PLATFORM_WIN32

#include "../../crnetshared.h"
#include "crnet.h"

#if CRNET_EXT_TCP

/*
 * The interface listing on Windows (crnet.h, "Interfaces"; the front end is
 * src/net/crnetinterfaces.c).
 *
 * GetAdaptersAddresses gives one entry per adapter with its unicast
 * addresses and, with GAA_FLAG_INCLUDE_PREFIX, its prefixes; GetIfTable2
 * gives every interface's administrative state, matched to an adapter by
 * its Luid. Both are read once, the listing's block is sized by a first
 * walk and made from the instance's allocator, a second walk fills it, and
 * the system's memory and the buffer the adapters were read into are given
 * back before the call returns.
 *
 * An adapter whose IfIndex is 0 (IPv4 switched off on it) is left out:
 * crnet.h says an index is never 0, as the getifaddrs backend leaves out an
 * interface whose index comes back 0.
 */

// winsock2.h before anything that pulls in windows.h, and iphlpapi.h after both.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on

#include <string.h>

#if __cplusplus
extern "C" {
#endif

static const ULONG crnet_win_adapter_flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;

/* Microsoft's suggested first size (GetAdaptersAddresses, "Remarks"), which
 * the sizing loop grows to whatever the call asks for. */
#define CRNET_WIN_ADAPTERS_FIRST_SIZE 15000u
#define CRNET_WIN_ADAPTERS_ALIGNMENT 8u

/* An adapter's FriendlyName as UTF-8 in `out` (`capacity` bytes with its
 * terminator), cut before the first character that doesn't fit. An
 * unpaired surrogate becomes U+FFFD, as WideCharToMultiByte makes it. */
static void crnet_win_utf8_from_wide(
    const WCHAR *text,
    char *out,
    size_t capacity) {
	size_t used = 0;
	if (text) {
		for (size_t i = 0; text[i] != 0; ++i) {
			uint32_t point = text[i];
			if (point >= 0xD800 && point <= 0xDBFF && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
				point = 0x10000u + ((point - 0xD800u) << 10) + ((uint32_t)text[i + 1] - 0xDC00u);
				++i;
			} else if (point >= 0xD800 && point <= 0xDFFF) {
				point = 0xFFFD;
			}
			uint8_t bytes[4];
			size_t length;
			if (point < 0x80) {
				bytes[0] = (uint8_t)point;
				length   = 1;
			} else if (point < 0x800) {
				bytes[0] = (uint8_t)(0xC0 | (point >> 6));
				bytes[1] = (uint8_t)(0x80 | (point & 0x3F));
				length   = 2;
			} else if (point < 0x10000) {
				bytes[0] = (uint8_t)(0xE0 | (point >> 12));
				bytes[1] = (uint8_t)(0x80 | ((point >> 6) & 0x3F));
				bytes[2] = (uint8_t)(0x80 | (point & 0x3F));
				length   = 3;
			} else {
				bytes[0] = (uint8_t)(0xF0 | (point >> 18));
				bytes[1] = (uint8_t)(0x80 | ((point >> 12) & 0x3F));
				bytes[2] = (uint8_t)(0x80 | ((point >> 6) & 0x3F));
				bytes[3] = (uint8_t)(0x80 | (point & 0x3F));
				length   = 4;
			}
			if (used + length > capacity - 1)
				break;
			memcpy(out + used, bytes, length);
			used += length;
		}
	}
	out[used] = '\0';
}

/* An IPv4 prefix's address as a number, false for any other family. */
static bool crnet_win_ipv4_of(
    const SOCKET_ADDRESS *address,
    uint32_t *out_value) {
	if (!address->lpSockaddr || address->lpSockaddr->sa_family != AF_INET || address->iSockaddrLength < (INT)sizeof(struct sockaddr_in))
		return false;
	struct sockaddr_in in;
	memcpy(&in, address->lpSockaddr, sizeof(in));
	*out_value = ntohl(in.sin_addr.s_addr);
	return true;
}

/* True when the adapter's prefixes hold a subnet broadcast address: an IPv4
 * /32 that is the last address of another IPv4 prefix of the adapter's
 * (IP_ADAPTER_ADDRESSES_LH, FirstPrefix: "the subnet broadcast IP address
 * prefix"). */
static bool crnet_win_has_broadcast(const IP_ADAPTER_ADDRESSES *adapter) {
	for (const IP_ADAPTER_PREFIX *host = adapter->FirstPrefix; host; host = host->Next) {
		uint32_t value;
		if (host->PrefixLength != 32 || !crnet_win_ipv4_of(&host->Address, &value))
			continue;
		for (const IP_ADAPTER_PREFIX *subnet = adapter->FirstPrefix; subnet; subnet = subnet->Next) {
			uint32_t base;
			if (subnet->PrefixLength == 0 || subnet->PrefixLength >= 31 || !crnet_win_ipv4_of(&subnet->Address, &base))
				continue;
			const uint32_t host_bits = 0xFFFFFFFFu >> subnet->PrefixLength;
			if ((base & ~host_bits) == (value & ~host_bits) && (value & host_bits) == host_bits)
				return true;
		}
	}
	return false;
}

/* The administrative state GetIfTable2 holds for the adapter's Luid. */
static bool crnet_win_admin_up(
    const MIB_IF_TABLE2 *table,
    const NET_LUID *luid) {
	for (ULONG i = 0; i < table->NumEntries; ++i) {
		if (table->Table[i].InterfaceLuid.Value == luid->Value)
			return table->Table[i].AdminStatus == NET_IF_ADMIN_STATUS_UP;
	}
	return false;
}

static uint32_t crnet_win_interface_flags(
    const IP_ADAPTER_ADDRESSES *adapter,
    const MIB_IF_TABLE2 *table) {
	uint32_t flags = 0;
	if (crnet_win_admin_up(table, &adapter->Luid))
		flags |= CRNET_INTERFACE_FLAG_UP;
	if (adapter->OperStatus == IfOperStatusUp)
		flags |= CRNET_INTERFACE_FLAG_RUNNING;
	if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
		flags |= CRNET_INTERFACE_FLAG_LOOPBACK;
	if (adapter->IfType == IF_TYPE_PPP || adapter->IfType == IF_TYPE_TUNNEL)
		flags |= CRNET_INTERFACE_FLAG_POINT_TO_POINT;
	if (!(adapter->Flags & IP_ADAPTER_NO_MULTICAST))
		flags |= CRNET_INTERFACE_FLAG_MULTICAST;
	if (crnet_win_has_broadcast(adapter))
		flags |= CRNET_INTERFACE_FLAG_BROADCAST;
	return flags;
}

static bool crnet_win_is_address(const IP_ADAPTER_UNICAST_ADDRESS *unicast) {
	const struct sockaddr *address = unicast->Address.lpSockaddr;
	return address && (address->sa_family == AF_INET || address->sa_family == AF_INET6);
}

/* Reads the adapters into a buffer from the instance's allocator, grown as
 * the call asks (it may ask again if an adapter came meanwhile). Returns
 * the Win32 result; on NO_ERROR `*out_buffer` and `*out_size` are the
 * caller's to free; ERROR_NOT_ENOUGH_MEMORY when the allocator refused. */
static ULONG crnet_win_read_adapters(
    const CR_ALLOCATOR *allocator,
    IP_ADAPTER_ADDRESSES **out_buffer,
    ULONG *out_size) {
	ULONG size = CRNET_WIN_ADAPTERS_FIRST_SIZE;
	for (int attempt = 0; attempt < 4; ++attempt) {
		IP_ADAPTER_ADDRESSES *buffer = (IP_ADAPTER_ADDRESSES *)cr_allocate(allocator, size, CRNET_WIN_ADAPTERS_ALIGNMENT);
		if (!buffer)
			return ERROR_NOT_ENOUGH_MEMORY;
		ULONG asked        = size;
		const ULONG result = GetAdaptersAddresses(AF_UNSPEC, crnet_win_adapter_flags, NULL, buffer, &asked);
		if (result == NO_ERROR) {
			*out_buffer = buffer;
			*out_size   = size;
			return NO_ERROR;
		}
		cr_free(allocator, buffer, size, CRNET_WIN_ADAPTERS_ALIGNMENT);
		if (result != ERROR_BUFFER_OVERFLOW)
			return result;
		size = asked;
	}
	return ERROR_BUFFER_OVERFLOW;
}

CR_RESULT crnet_interface_backend_enumerate(
    crnet_instance instance,
    struct crnet_interface_list_t **out_list,
    crnet_error *out_error) {
	const CR_ALLOCATOR *allocator  = &instance->head.allocator;
	IP_ADAPTER_ADDRESSES *adapters = NULL;
	ULONG size                     = 0;
	const ULONG read               = crnet_win_read_adapters(allocator, &adapters, &size);
	if (read == ERROR_NOT_ENOUGH_MEMORY)
		return CR_RESULT_OUT_OF_MEMORY;
	// No adapter at all is an empty list, as crnet.h promises.
	if (read != NO_ERROR && read != ERROR_NO_DATA)
		return crnet_error_record(out_error, CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED, CR_PLATFORM_ERROR_SOURCE_WIN32, (int64_t)read, NULL);

	MIB_IF_TABLE2 *table = NULL;
	if (read == NO_ERROR) {
		const DWORD tabled = GetIfTable2(&table);
		if (tabled != NO_ERROR) {
			cr_free(allocator, adapters, size, CRNET_WIN_ADAPTERS_ALIGNMENT);
			return crnet_error_record(out_error, CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED, CR_PLATFORM_ERROR_SOURCE_WIN32, (int64_t)tabled, NULL);
		}
	}

	// The first walk: how many interfaces and addresses, so the block is
	// made once and exactly.
	uint32_t interface_count = 0;
	uint32_t address_count   = 0;
	if (read == NO_ERROR) {
		for (const IP_ADAPTER_ADDRESSES *adapter = adapters; adapter; adapter = adapter->Next) {
			if (adapter->IfIndex == 0)
				continue;
			interface_count++;
			for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
				if (crnet_win_is_address(unicast))
					address_count++;
			}
		}
	}

	struct crnet_interface_list_t *list = NULL;
	const CR_RESULT made                = crnet_interface_list_alloc(instance, interface_count, address_count, &list);
	if (CR_FAILED(made)) {
		if (table)
			FreeMibTable(table);
		if (adapters)
			cr_free(allocator, adapters, size, CRNET_WIN_ADAPTERS_ALIGNMENT);
		return made;
	}

	// The second walk fills it, in the system's order.
	for (const IP_ADAPTER_ADDRESSES *adapter = read == NO_ERROR ? adapters : NULL; adapter; adapter = adapter->Next) {
		if (adapter->IfIndex == 0 || list->interface_count == interface_count)
			continue;
		struct crnet_interface_entry *out = &list->interfaces[list->interface_count];
		if (adapter->AdapterName) {
			const size_t length = strnlen(adapter->AdapterName, sizeof(out->name) - 1);
			memcpy(out->name, adapter->AdapterName, length); // the block is zeroed: terminated
		}
		crnet_win_utf8_from_wide(adapter->FriendlyName, out->display_name, sizeof(out->display_name));
		out->index         = adapter->IfIndex;
		out->ipv6_index    = adapter->Ipv6IfIndex;
		out->flags         = crnet_win_interface_flags(adapter, table);
		out->mtu           = adapter->Mtu == 0xFFFFFFFFu ? 0 : adapter->Mtu;
		out->first_address = list->address_count;
		for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
			if (!crnet_win_is_address(unicast) || list->address_count == address_count)
				continue;
			struct crnet_interface_address_entry *address = &list->addresses[list->address_count];
			bool embedded                                 = false;
			if (!crnet_address_from_sockaddr(
			        unicast->Address.lpSockaddr, (size_t)unicast->Address.iSockaddrLength, adapter->Ipv6IfIndex, &address->address, &embedded))
				continue;
			// 255 is "an illegal value" (IP_ADAPTER_UNICAST_ADDRESS_LH):
			// the system holds no prefix for the address.
			address->prefix_length = unicast->OnLinkPrefixLength == 255 ? CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN : unicast->OnLinkPrefixLength;
#if _DEBUG
			if (embedded)
				list->debug_embedded_scopes++;
#endif
			out->address_count++;
			list->address_count++;
		}
		list->interface_count++;
	}

	if (table)
		FreeMibTable(table);
	if (adapters)
		cr_free(allocator, adapters, size, CRNET_WIN_ADAPTERS_ALIGNMENT);
	*out_list = list;
	return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_TCP

#endif // CARROTTUS_PLATFORM_WIN32
