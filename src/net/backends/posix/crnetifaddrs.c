
#if CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE

#include "../../crnetshared.h"
#include "crnet.h"

#if CRNET_EXT_TCP

/*
 * The interface listing on getifaddrs - Linux and every Apple platform that
 * has sockets (crnet.h, "Interfaces"; the front end is crnetinterfaces.c).
 *
 * getifaddrs hands over one entry per address the kernel holds, and one
 * more per interface for its link (AF_LINK on Apple, AF_PACKET on Linux),
 * each carrying the interface's name. The listing groups them by name in
 * the order each name first appears, and copies everything into one block
 * from the instance's allocator: a first walk counts what is there, the
 * block is made to that size, and a second walk fills it. The system's list
 * is freed before the call returns.
 *
 * Each interface's index is its link entry's (sdl_index, sll_ifindex),
 * which costs no system call, or if_nametoindex where it has none. An
 * interface whose index comes back 0 went away while it was being listed
 * and is left out, as it would be had the listing come a moment later.
 */

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#if CARROTTUS_PLATFORM_APPLE
#include <net/if_dl.h>
#endif
#if CARROTTUS_PLATFORM_LINUX
#include <linux/if_packet.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#if __cplusplus
extern "C" {
#endif

/* True for the first entry of its name: the one an interface is counted
 * and listed at. */
static bool crnet_ifaddrs_first_of_name(const struct ifaddrs *all, const struct ifaddrs *entry) {
	for (const struct ifaddrs *earlier = all; earlier != entry; earlier = earlier->ifa_next) {
		if (strcmp(earlier->ifa_name, entry->ifa_name) == 0)
			return false;
	}
	return true;
}

static bool crnet_ifaddrs_is_address(const struct ifaddrs *entry) {
	return entry->ifa_addr && (entry->ifa_addr->sa_family == AF_INET || entry->ifa_addr->sa_family == AF_INET6);
}

/* What the system gave a sockaddr's length as: sa_len where it has one. */
static size_t crnet_ifaddrs_sockaddr_length(const struct sockaddr *address) {
#if CARROTTUS_PLATFORM_APPLE
	return address->sa_len;
#else
	return address->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
#endif
}

/* The interface's index, from its link entry or by name; 0 when it has
 * gone. */
static uint32_t crnet_ifaddrs_index(const struct ifaddrs *all, const char *name) {
	for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
		if (!entry->ifa_addr || strcmp(entry->ifa_name, name) != 0)
			continue;
#if CARROTTUS_PLATFORM_APPLE
		if (entry->ifa_addr->sa_family == AF_LINK) {
			struct sockaddr_dl link;
			memcpy(&link, entry->ifa_addr, sizeof(link) < entry->ifa_addr->sa_len ? sizeof(link) : entry->ifa_addr->sa_len);
			if (link.sdl_index != 0)
				return link.sdl_index;
		}
#endif
#if CARROTTUS_PLATFORM_LINUX
		if (entry->ifa_addr->sa_family == AF_PACKET) {
			struct sockaddr_ll link;
			memcpy(&link, entry->ifa_addr, sizeof(link));
			if (link.sll_ifindex > 0)
				return (uint32_t)link.sll_ifindex;
		}
#endif
	}
	return if_nametoindex(name);
}

/* The flags every entry of the name carries, as Carrottus Net's bits. */
static uint32_t crnet_ifaddrs_flags(unsigned int system) {
	uint32_t flags = 0;
	if (system & IFF_UP)
		flags |= CRNET_INTERFACE_FLAG_UP;
	if (system & IFF_RUNNING)
		flags |= CRNET_INTERFACE_FLAG_RUNNING;
	if (system & IFF_LOOPBACK)
		flags |= CRNET_INTERFACE_FLAG_LOOPBACK;
	if (system & IFF_POINTOPOINT)
		flags |= CRNET_INTERFACE_FLAG_POINT_TO_POINT;
	if (system & IFF_MULTICAST)
		flags |= CRNET_INTERFACE_FLAG_MULTICAST;
	if (system & IFF_BROADCAST)
		flags |= CRNET_INTERFACE_FLAG_BROADCAST;
	return flags;
}

/* The link's MTU as the system reports it; 0 where it reports none.
 * `probe` is a datagram socket for Linux's ioctl, or -1. */
static uint32_t crnet_ifaddrs_mtu(const struct ifaddrs *all, const char *name, int probe) {
#if CARROTTUS_PLATFORM_APPLE
	(void)probe;
	for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
		if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_LINK && entry->ifa_data && strcmp(entry->ifa_name, name) == 0) {
			struct if_data data;
			memcpy(&data, entry->ifa_data, sizeof(data));
			return data.ifi_mtu;
		}
	}
	return 0;
#else
	(void)all;
	if (probe < 0)
		return 0;
	struct ifreq request;
	memset(&request, 0, sizeof(request));
	strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1);
	if (ioctl(probe, SIOCGIFMTU, &request) != 0 || request.ifr_mtu < 0)
		return 0;
	return (uint32_t)request.ifr_mtu;
#endif
}

CR_RESULT crnet_interface_backend_enumerate(crnet_instance instance, struct crnet_interface_list_t **out_list, crnet_error *out_error) {
	struct ifaddrs *all = NULL;
	if (getifaddrs(&all) != 0)
		return crnet_error_record(out_error, CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);

	// The first walk: how many interfaces, and how many addresses of
	// theirs, so the block is made once and exactly.
	uint32_t interface_count = 0;
	uint32_t address_count   = 0;
	for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
		if (crnet_ifaddrs_first_of_name(all, entry))
			interface_count++;
		if (crnet_ifaddrs_is_address(entry))
			address_count++;
	}

	struct crnet_interface_list_t *list   = NULL;
	const CR_RESULT                made   = crnet_interface_list_alloc(instance, interface_count, address_count, &list);
	if (CR_FAILED(made)) {
		freeifaddrs(all);
		return made;
	}

#if CARROTTUS_PLATFORM_LINUX
	// SIOCGIFMTU asks through a socket; any datagram socket will do. Without
	// one every MTU is 0, "reported none".
	const int probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
	const int probe = -1;
#endif

	// The second walk fills it, in the order each name first came.
	for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
		if (!crnet_ifaddrs_first_of_name(all, entry) || list->interface_count == interface_count)
			continue;
		const uint32_t index = crnet_ifaddrs_index(all, entry->ifa_name);
		if (index == 0)
			continue;
		struct crnet_interface_entry *out = &list->interfaces[list->interface_count];
		strncpy(out->name, entry->ifa_name, sizeof(out->name) - 1);
		out->index         = index;
		out->ipv6_index    = index;
		out->flags         = crnet_ifaddrs_flags(entry->ifa_flags);
		out->mtu           = crnet_ifaddrs_mtu(all, entry->ifa_name, probe);
		out->first_address = list->address_count;
		for (const struct ifaddrs *other = entry; other; other = other->ifa_next) {
			if (!crnet_ifaddrs_is_address(other) || strcmp(other->ifa_name, entry->ifa_name) != 0 || list->address_count == address_count)
				continue;
			struct crnet_interface_address_entry *address  = &list->addresses[list->address_count];
			bool                                   embedded = false;
			if (!crnet_address_from_sockaddr(other->ifa_addr, crnet_ifaddrs_sockaddr_length(other->ifa_addr), index, &address->address, &embedded))
				continue;
			const size_t netmask_length = other->ifa_netmask ? crnet_ifaddrs_sockaddr_length(other->ifa_netmask) : 0;
			// Apple gives some netmasks a short sa_len, stopping after the
			// last byte that isn't 0; the bytes past it are 0.
			uint8_t netmask[sizeof(struct sockaddr_in6)];
			memset(netmask, 0, sizeof(netmask));
			if (other->ifa_netmask)
				memcpy(netmask, other->ifa_netmask, netmask_length < sizeof(netmask) ? netmask_length : sizeof(netmask));
			address->prefix_length = crnet_netmask_prefix_length(other->ifa_netmask ? netmask : NULL, sizeof(netmask), other->ifa_addr->sa_family);
#if _DEBUG
			if (embedded)
				list->debug_embedded_scopes++;
#endif
			out->address_count++;
			list->address_count++;
		}
		list->interface_count++;
	}

#if CARROTTUS_PLATFORM_LINUX
	if (probe >= 0)
		close(probe);
#endif
	freeifaddrs(all);
	*out_list = list;
	return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_TCP

#endif // CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE
