/*
 * crnet_interface_draft.h - Carrottus Net: this machine's network
 * interfaces, listed (card S9, decided 2026-10-28 with the user:
 * CRNET_TODO.md, "Missing pieces", "Interfaces"), and the address value
 * type the UDP and discovery drafts beside it share.
 *
 * APPROVED by the user 2026-11-22, version 7 (SHA-256 in CRNET_TODO.md's
 * approval record). Not built,
 * not in any target.
 *   Version 1: the first.
 *   Version 2 (critique cycle 1, each library's own rules): no caller
 *   named in the text; the list's reads marked Thread: (Q6 answered);
 *   enumerate's platform error also to the message callback once card
 *   S10 is built; crcore.h's range comment [CHANGED]; Q3 answered.
 *   Version 3 (critique cycle 2, platform truth): Windows' UP from
 *   AdminStatus and BROADCAST from the broadcast prefix, so both mean
 *   what they mean elsewhere (Q2 answered) [CHANGED]; a prefix length
 *   Windows doesn't know said as such [ADDED]; a link-local address's
 *   scope taken from KAME's embedded form where Apple leaves
 *   sin6_scope_id 0 [CHANGED]; Android 11's rule cited; Q4 and Q5
 *   answered.
 *   Version 4 (critique cycle 5, the callers): Q1 answered.
 *   Version 5 (critique cycle 6, failures): to_string's output on failure
 *   stated; an injected out-of-memory tested.
 *   Version 6 (critique cycle 7, threads): how the Debug check sees a
 *   destroy overlapping a read.
 *   Version 7 (the critique's read of the whole): text later versions
 *   had left behind brought into line, every open question answered
 *   under itself, the specs agreeing (prompts/remote_drafts_critique_record.md,
 *   "Whole").
 * Written against include/crnet.h and include/crcore.h
 * as read on 2026-11-21 (carrottus version 0.4.0), and against
 * src/crnet.c, where crnet_instance_get_local_ipv4_addresses() lives
 * today. Asked for by a caller of card X46: the drafts
 * now, built Mac first, then
 * Windows, Linux and Android. Its siblings: crnet_udp_draft.h and
 * crnet_discovery_draft.h, which include this one for crnet_address.
 *
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. It compiles beside today's crnet.h: an
 * [ADDED] call or type is declared under its own name; an [ADDED] result
 * code or struct type, which belongs in crcore.h's enums, is a macro here
 * of the enum's type, with the value it would take, only so the file
 * compiles; a [REMOVED] call is named in a comment and left in crnet.h.
 * Checked with
 *   clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -I include -x c
 *   clang -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic -I include -x c++
 * with -DCARROTTUS_PLATFORM_APPLE=1 (every part) and bare (every platform
 * macro 0: nothing below CRNET_EXT_TCP is declared).
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * crnet_instance_get_local_ipv4_addresses() is the one call in crnet.h
 * that breaks the module's own rules, as its group says: it asks the
 * system resolver for the machine's own name (gethostname + getaddrinfo),
 * so it blocks with no time limit and no abort; it reports IPv4 only; it
 * reports what the resolver associates with the host name, not what the
 * interfaces hold; and it decides for the caller (loopback left out,
 * duplicates folded). Card S9 replaces it now, while nothing outside
 * carrottus calls it (its callers are src/crnet.c, tests/crnet_test.c and
 * tests/cr_misuse_tests.c), with a listing of the interfaces as the
 * system keeps them: getifaddrs on Apple, Linux and Android, and
 * GetAdaptersAddresses on Windows. Neither blocks on the network nor
 * touches the resolver: both read the kernel's tables.
 *
 * A caller needs it at once (card X46). A device that advertises a
 * service, or shows "connect to 192.168.1.20" beside a QR code, has to
 * know which interfaces it has and which addresses each holds, IPv6
 * link-local ones with their scope included. Which interface to advertise
 * on, and which address to show, stays the caller's: this lists, and
 * leaves nothing out.
 *
 * crnet_address is added here because three drafts need one value for
 * "an IP address and a port": this listing's addresses, UDP's
 * destinations and sources (a datagram's source is read every frame and
 * compared to the peers a caller knows, which text would make a parse and
 * a string compare), and discovery's resolved addresses. TCP keeps its
 * numeric strings (Q3).
 *
 * ---------------------------------------------------------------------------
 * What the platforms have, and what each field is made from
 * ---------------------------------------------------------------------------
 *
 * | Field                        | Apple, Linux, Android (getifaddrs)              | Windows (GetAdaptersAddresses)               |
 * |------------------------------|-------------------------------------------------|----------------------------------------------|
 * | name                         | ifa_name ("en0", "wlan0")                       | AdapterName (the GUID string, "{...}")       |
 * | display_name                 | empty: the system keeps none at this level      | FriendlyName ("Wi-Fi"), UTF-16 to UTF-8      |
 * | index                        | if_nametoindex(ifa_name)                        | IfIndex                                      |
 * | ipv6_index                   | the same as index                               | Ipv6IfIndex                                  |
 * | flags UP                     | IFF_UP                                          | AdminStatus is NET_IF_ADMIN_STATUS_UP (*)    |
 * | flags RUNNING                | IFF_RUNNING                                     | OperStatus is IfOperStatusUp                 |
 * | flags LOOPBACK               | IFF_LOOPBACK                                    | IfType is IF_TYPE_SOFTWARE_LOOPBACK          |
 * | flags POINT_TO_POINT         | IFF_POINTOPOINT                                 | IfType is IF_TYPE_PPP or IF_TYPE_TUNNEL      |
 * | flags MULTICAST              | IFF_MULTICAST                                   | no IP_ADAPTER_NO_MULTICAST in Flags          |
 * | flags BROADCAST              | IFF_BROADCAST                                   | a broadcast prefix in FirstPrefix (**)       |
 * | mtu                          | Apple: AF_LINK's if_data.ifi_mtu; Linux and     | Mtu                                          |
 * |                              | Android: SIOCGIFMTU on a datagram socket        |                                              |
 * | addresses                    | each AF_INET and AF_INET6 entry of the name     | FirstUnicastAddress's list                   |
 * | address prefix_length        | the bits set in ifa_netmask                     | OnLinkPrefixLength                           |
 * | address scope_id             | sin6_scope_id (IPv6 link-local, else 0) (***)   | sin6_scope_id (IPv6 link-local, else 0)      |
 *
 * (*) [CHANGED] MIB_IF_ROW2::AdminStatus from one GetIfTable2() call,
 * matched to each adapter by its Luid: the administrative state, as
 * IFF_UP is; GetIfEntry2 and GetIfTable2 document no privilege needed
 * (learn.microsoft.com, GetIfEntry2, read 2026-11-21).
 * (**) [CHANGED] Windows has no broadcast flag per interface; with
 * GAA_FLAG_INCLUDE_PREFIX, an IPv4 address's FirstPrefix list holds "the
 * subnet broadcast IP address prefix" (IP_ADAPTER_ADDRESSES_LH, read
 * 2026-11-21), and an interface with one has BROADCAST.
 * (***) [ADDED] Apple: a KAME stack may give a link-local address with
 * its interface index in bytes 2 and 3 and sin6_scope_id 0; where it
 * does, the backend takes the scope from those bytes and clears them, so
 * every fe80::/10 address is handed over with bytes 2 and 3 zero and its
 * scope in scope_id [unconfirmed (C2.13): FreeBSD's lists describe it;
 * current macOS and iOS not checked].
 *
 * An interface with no address is listed with none: Linux and Apple give
 * it an AF_PACKET / AF_LINK entry, Windows lists the adapter. Android 11
 * and later refuse an app's netlink link dump (RTM_GETLINK), and bionic's
 * getifaddrs then lists only the interfaces that have an address
 * ([CHANGED] Android's "only network interfaces with an IP address are
 * visible", developer.android.com/identity/user-data-ids, read
 * 2026-11-21; a test owed on the A53): the listing is still right for
 * every interface the caller could use.
 *
 * What is not listed, and why:
 * - Hardware (MAC) addresses. Android 11 and later and iOS hide them from
 *   apps, and no caller of card X46 needs one.
 * - The kind of interface (Wi-Fi, Ethernet, cellular). Windows has IfType;
 *   getifaddrs reports Wi-Fi and Ethernet alike (IFT_ETHER, ARPHRD_ETHER).
 *   A field only one platform could fill truthfully would be a
 *   per-platform difference in the header (Q4).
 * - IPv6 address states (temporary, deprecated, tentative). Windows has
 *   them (PrefixOrigin, SuffixOrigin, DadState); Apple only through an
 *   ioctl per address (SIOCGIFAFLAG_IN6); Linux only through netlink's
 *   IFA_F_* flags, not through getifaddrs (Q5).
 * - Gateways, DNS servers, routes: not this machine's addresses.
 *
 * ---------------------------------------------------------------------------
 * The shape
 * ---------------------------------------------------------------------------
 *
 * crnet_interface_enumerate() takes one snapshot of the system's table
 * into a crnet_interface_list, copied through the instance's allocator,
 * and the list is read with plain calls that never fail for any reason
 * but a bad argument. Why a list object and not the caller's arrays, as
 * crnet_resolver_wait() takes: an interface holds any number of addresses,
 * so arrays sized by the caller would need two counts and a retry when the
 * table changed between asking for the counts and filling them (Vulkan's
 * VK_INCOMPLETE). A snapshot is consistent by construction, as
 * crfiles_listing is, and its entries are CR_STRUCT_BASE_OUT structs that
 * can grow. The order is the system's: getifaddrs' order of first
 * appearance of each name, GetAdaptersAddresses' order. Nothing is
 * sorted, filtered or folded.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * The platforms: getifaddrs(3) on Apple (AF_LINK entries with if_data),
 * Linux (AF_PACKET entries, netlink underneath) and bionic (API 24 and
 * later; its Android 11 netlink restrictions); if_nametoindex;
 * SIOCGIFMTU; GetAdaptersAddresses (IP_ADAPTER_ADDRESSES_LH: AdapterName,
 * FriendlyName, IfIndex, Ipv6IfIndex, OperStatus, IfType, Mtu, Flags,
 * OnLinkPrefixLength; the ERROR_BUFFER_OVERFLOW sizing loop);
 * inet_pton/inet_ntop and RtlIpv6AddressToStringExA for the address
 * helpers.
 *
 * Industry practice: Go's net.Interfaces() and Interface.Addrs() (an
 * interface with Index, MTU, Name, Flags and its addresses with their
 * prefixes: the shape followed here); libuv's uv_interface_addresses()
 * (one flat entry per address with its interface's name repeated,
 * is_internal for loopback, the netmask as an address; no index, and it
 * leaves out interfaces with no address); Rust's if-addrs and nix's
 * getifaddrs (the same flat shape); Qt's QNetworkInterface::allInterfaces()
 * (per interface, with humanReadableName from FriendlyName on Windows and
 * the name elsewhere, the same split as `display_name` here); SDL_net 3's
 * SDLNet_GetLocalAddresses() (addresses only, no interface). Go's and
 * Qt's per-interface shape is kept because a caller chooses an
 * interface first and an address on it second.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Breaks, before 1.0, each listed against abi.txt (./check.sh
 * --update-abi) and in the commit:
 * - [REMOVED] crnet_instance_get_local_ipv4_addresses() and
 *   CRNET_MAX_IPV4_STRING_LEN, which only it used. Callers: carrottus's
 *   own src/crnet.c, tests/crnet_test.c ("records") and
 *   tests/cr_misuse_tests.c (its factory), changed in the same commit.
 *   None outside carrottus.
 * Added: crnet_address, crnet_address_from_string(),
 * crnet_address_to_string(), crnet_interface_list,
 * crnet_interface_enumerate(), crnet_interface_list_get_count(),
 * crnet_interface_list_get_properties(),
 * crnet_interface_list_get_address(), crnet_interface_list_destroy(),
 * crnet_interface_properties, crnet_interface_address,
 * CRNET_INTERFACE_FLAG_*, CRNET_INTERFACE_NAME_CAPACITY,
 * CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN; two struct types
 * (CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES 3007,
 * CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS 3008) and two results
 * (CR_RESULT_NET_INVALID_INTERFACE_LIST 3034,
 * CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED 3035), each with its name in
 * cr_result_str(). carrottus goes to the next minor version (0.5.0 if
 * nothing else takes it first), with UDP and discovery if they are built
 * together.
 *
 * ---------------------------------------------------------------------------
 * Tests (crnet_test, a new part "interfaces"; the misuse harness)
 * ---------------------------------------------------------------------------
 *
 * Each case seen failing first, against a stub that returns
 * CR_RESULT_NOT_IMPLEMENTED_YET.
 * - Every platform: the list has at least one interface with the LOOPBACK
 *   flag, holding 127.0.0.1/8 and ::1/128 where the system has IPv6 on
 *   loopback; every interface's index is non-zero and if_nametoindex /
 *   ConvertInterfaceLuidToIndex agrees; every address's family is IPv4 or
 *   IPv6, its prefix within 0-32 or 0-128 (or UNKNOWN, Windows only), its
 *   port 0, its `reserved` 0; every fe80::/10 address's bytes 2 and 3 zero
 *   [CHANGED]; on Windows UP read back off for an adapter disabled by
 *   hand, BROADCAST on the Wi-Fi adapter [CHANGED];
 *   every IPv6 link-local address (fe80::/10) has a non-zero scope_id equal
 *   to its interface's ipv6_index, and every other address scope_id 0;
 *   each name null-terminated and valid UTF-8. A list read twice gives the
 *   same answers; two lists taken in a row agree on the loopback entry.
 * - Every argument check, in order, with zeroes and (Debug) a destroyed
 *   list: a new factory in tests/misuse/make_misuse_tests.py; CR_STALE
 *   right after each null check; the list quarantined by its instance.
 * - crnet_address_from_string() and _to_string(): round trips of
 *   "127.0.0.1", "::1", "fe80::1" with a scope, "::ffff:1.2.3.4" (45
 *   characters at most, CRNET_MAX_ADDRESS_STRING_LEN), refusals of names,
 *   of a scope on IPv4, and of text with a '%'.
 * - [CHANGED] Two threads reading one list at once (every read), under
 *   -tsan, nothing reported; a destroy overlapping a read reported by the
 *   Debug overlap check.
 * - [CHANGED] An injected out-of-memory at the enumerate's copy: OUT_OF_MEMORY,
 *   no list, nothing leaked.
 * - cr_noalloc_tests: none (a listing allocates by design; it is not a
 *   per-frame call).
 * - Per platform: macOS (plain, Release, ASan, TSan); the iOS simulator
 *   (CRNET_TEST_LAUNCHER; it lists the Mac's interfaces); Windows 11 on the
 *   ThinkPad (Debug, Release, -asan; FriendlyName non-empty, the Wi-Fi
 *   adapter's display_name "Wi-Fi" or its localised name, seen by hand
 *   against `ipconfig /all`); Fedora 44 (GCC and clang, -asan and -tsan;
 *   checked against `ip -o addr`); Android, on the A53 once Carrottus
 *   builds there (card S47): bionic's listing under Android 11's netlink
 *   rules, against `adb shell ip addr`.
 */

#ifndef CRNET_INTERFACE_DRAFT_H
#define CRNET_INTERFACE_DRAFT_H

#include "crnet.h"

#if __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* crcore.h, CR_RESULT and CR_STRUCT_TYPE [ADDED]                           */
/* ======================================================================== */

/* In crcore.h these are enum values beside CR_RESULT_NET_INVALID_INSTANCE
 * and CR_STRUCT_TYPE_NET_INSTANCE_DESC; macros here only so the draft
 * compiles. */

/** [ADDED] An interface list argument is NULL. */
#define CR_RESULT_NET_INVALID_INTERFACE_LIST ((CR_RESULT)3034)
/** [ADDED] The system wouldn't list its interfaces; the call's `out_error`
 * has its reason. */
#define CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED ((CR_RESULT)3035)

/** [ADDED] crnet_interface_properties (CR_STRUCT_BASE_OUT). */
#define CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES ((CR_STRUCT_TYPE)3007)
/** [ADDED] crnet_interface_address (CR_STRUCT_BASE_OUT). */
#define CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS ((CR_STRUCT_TYPE)3008)

/* [CHANGED] crcore.h's comment above CR_RESULT, "3001-3033 Carrottus Net",
 * becomes "3001-3048" once this draft and its two siblings are built (each
 * takes the next run: 3034-3035 here, 3036-3039 UDP, 3040-3046 discovery,
 * then UDP's 3047 and discovery's 3048 of cycle 6). */

/* ======================================================================== */
/* crnet.h, text that changes [CHANGED]                                     */
/* ======================================================================== */

/* [CHANGED] CRNET_EXT_TCP's comment: "the local address query" becomes
 * "the interface listing (crnet_interface_enumerate()) and crnet_address".
 *
 * [CHANGED] "Error detail": the calls that take an `out_error` because they
 * leave no object to ask become crnet_instance_create(),
 * crnet_interface_enumerate() and crnet_tcp_listener_create() (and
 * crnet_udp_socket_create(), crnet_udp_draft.h).
 *
 * [REMOVED] The group "Local address discovery" (crnet_local), with
 * crnet_instance_get_local_ipv4_addresses() and CRNET_MAX_IPV4_STRING_LEN.
 * Its replacement is the group below, "Interfaces", under crnet_tcp as it
 * was. Why removed rather than kept beside: it blocks in the resolver
 * with no limit, answers a different question (what the host name
 * resolves to) from the one its callers ask (what this machine's
 * interfaces hold), and every use of it is a filter over the listing
 * (IPv4, not loopback), which is the caller's to write.
 */

#if CRNET_EXT_TCP

/* ======================================================================== */
/* crnet.h, Addresses [ADDED]                                               */
/* ======================================================================== */

/**
 * @defgroup crnet_addresses Addresses
 * @ingroup crnet_tcp
 * @brief [ADDED] An IP address and a port as a value: what the interface
 * listing, UDP sockets and service discovery hand over and take.
 *
 * Why a value and not text: a datagram's source is read on every receive
 * and compared with the peers the caller knows, and an IPv6 link-local
 * address is only usable with its interface (its scope), which text
 * carries as a suffix ("fe80::1%en0") the platforms parse differently.
 * TCP keeps its numeric strings for now (Q3).
 *
 * Two crnet_address values Carrottus Net made for one address and port
 * are equal byte for byte: unused bytes and `reserved` are always 0. So a
 * caller may compare them with memcmp and hash their bytes.
 * @{
 */

/**
 * @brief [ADDED] One IPv4 or IPv6 address and a port.
 */
/* Frozen: a value type, passed in and filled in alike. It never changes; a new need gets a new type. */
typedef struct crnet_address {
	/** CRNET_ADDRESS_FAMILY_IPV4 or CRNET_ADDRESS_FAMILY_IPV6. ANY is never
	 * an address's family and is refused wherever one is taken. */
	CRNET_ADDRESS_FAMILY family;
	/** IPv6: the interface index a link-local (fe80::/10) or other scoped
	 * address is used through, as sin6_scope_id has it; 0 for a global
	 * address. IPv4: always 0. */
	uint32_t scope_id;
	/** The address in network byte order, as on the wire: IPv4 in the first
	 * 4 bytes and the other 12 zero, IPv6 in all 16. */
	uint8_t bytes[16];
	/** The port in the machine's own byte order; 0 where no port belongs
	 * (an interface's address) or where 0 asks the system for one (a UDP
	 * socket's local address). */
	uint16_t port;
	/** Always 0, so that equal addresses are equal byte for byte. Refused
	 * when not. */
	uint16_t reserved;
} crnet_address;

/**
 * @brief [ADDED] Makes an address from numeric text. Pure: no system
 * state, no instance, never blocks; inet_pton on every platform.
 *
 * Only a numeric address is taken, never a name ("localhost" is refused:
 * a name is the resolver's). No '%' suffix is taken either: the scope is
 * its own argument, an interface index as crnet_interface_properties has
 * it, so it means the same on every platform.
 *
 * @param[in]  text        "192.168.1.20", "::1", "fe80::1c2b:..."; UTF-8
 *                         and null-terminated. NULL is refused.
 * @param[in]  port        The port, in the machine's byte order.
 * @param[in]  scope_id    0, or for IPv6 the interface index; non-zero with
 *                         an IPv4 address is refused.
 * @param[out] out_address Receives the address. Left as it was on a
 *                         failure.
 *
 * @retval CR_SUCCESS `out_address` was written.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `text` is NULL, not a numeric IPv4
 *         or IPv6 address, holds a '%', or is IPv4 with a non-zero
 *         `scope_id`.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 */
CR_RESULT crnet_address_from_string(
    const char *text,
    uint16_t port,
    uint32_t scope_id,
    crnet_address *out_address);

/**
 * @brief [ADDED] Writes an address's numeric text, without its port or
 * scope: inet_ntop on every platform, so IPv6 in RFC 5952's form (the
 * longest run of zeroes as "::", lower case), and an IPv4-mapped IPv6
 * address as "::ffff:1.2.3.4". Pure, never blocks.
 *
 * @param[in]  address  The address. NULL is refused.
 * @param[out] out_text Receives the text, null-terminated; room for
 *                      CRNET_MAX_ADDRESS_STRING_LEN bytes. [CHANGED]
 *                      Untouched on failure.
 *
 * @retval CR_SUCCESS `out_text` was written.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `address` is NULL, or its family
 *         is neither IPv4 nor IPv6, or its `reserved`, or an IPv4
 *         address's last 12 bytes or scope, aren't 0.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_text` is NULL.
 */
CR_RESULT crnet_address_to_string(
    const crnet_address *address,
    char out_text[CRNET_MAX_ADDRESS_STRING_LEN]);

/** @} */

/* ======================================================================== */
/* crnet.h, Interfaces [ADDED]                                              */
/* ======================================================================== */

/**
 * @defgroup crnet_interfaces Interfaces
 * @ingroup crnet_tcp
 * @brief [ADDED] This machine's network interfaces and the addresses each
 * holds, as the system's own table has them at one moment: getifaddrs on
 * Apple, Linux and Android, GetAdaptersAddresses on Windows. Known
 * problems: [CHANGED] on Android 11 and later an interface with no address
 * is missing; on Windows a prefix length may be unknown
 * (CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN).
 *
 * Nothing is left out, sorted or folded: loopback is listed, an interface
 * that is down is listed, and an address the system holds twice is listed
 * twice. Which interface to use, advertise on or show is the caller's.
 * The listing never blocks and never asks the resolver: both systems read
 * the kernel's tables.
 *
 * A listing is a snapshot. It doesn't change when the system's table
 * does, and nothing here says when that happens (no change notification:
 * Q1); a caller that wants a fresh view takes a new listing.
 *
 * Thread: a list is one caller at a time, as every object is, but for
 * its three reads, which change nothing and may overlap each other (each
 * marked Thread:), so a snapshot can be handed to jobs. Its Debug overlap
 * check opens on the destroy only ([CHANGED, cycle 7] each read adds one to
 * an atomic reader count and takes it off, which the destroy checks, and
 * writes nothing else): a destroy overlapping a read is
 * reported. (Q6, answered C1.3.)
 * @{
 */

/** @brief [ADDED] Opaque handle to one snapshot of the interface table. */
typedef struct crnet_interface_list_t *crnet_interface_list;

/** @brief [ADDED] Room for an interface's name or display name, with its
 * terminator. A Windows GUID string is 38 bytes; a FriendlyName is
 * whatever the person typed, cut to fit at a UTF-8 character's boundary. */
#define CRNET_INTERFACE_NAME_CAPACITY 256

/** @brief [ADDED] [CHANGED] A prefix_length the system doesn't know. Its
 * own value, not 0, since 0 is a real prefix (the default route's). */
#define CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN 0xFFFFFFFFu

/** @brief [ADDED] Bits of crnet_interface_properties::flags. Each is the
 * system's own; how each is read is in the table at the top of the
 * draft. */
enum {
	CRNET_INTERFACE_FLAG_UP             = 0x01, /**< Up: IFF_UP; Windows' AdminStatus up [CHANGED]. */
	CRNET_INTERFACE_FLAG_RUNNING        = 0x02, /**< Carrying traffic: IFF_RUNNING; Windows' OperStatus up. */
	CRNET_INTERFACE_FLAG_LOOPBACK       = 0x04, /**< This machine only. */
	CRNET_INTERFACE_FLAG_POINT_TO_POINT = 0x08, /**< A link with one peer (PPP, a tunnel, some VPNs). */
	CRNET_INTERFACE_FLAG_MULTICAST      = 0x10, /**< Takes multicast (which DNS-SD uses). */
	CRNET_INTERFACE_FLAG_BROADCAST      = 0x20, /**< Takes IPv4 broadcast: IFF_BROADCAST; on Windows a broadcast prefix [CHANGED]. */
};

/** @brief [ADDED] One interface of a listing, filled by
 * crnet_interface_list_get_properties(). */
typedef struct crnet_interface_properties {
	/** `type` is CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES; `next` is NULL
	 * (nothing is chained onto this struct yet). The caller sets the type;
	 * Carrottus Net fills in the rest. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** The system's name for it, UTF-8, null-terminated: ifa_name
	 * ("en0"), or on Windows AdapterName, the adapter's GUID string. Stable
	 * while the interface exists; what a caller keeps to find it again. */
	char name[CRNET_INTERFACE_NAME_CAPACITY];
	/** The name a person knows it by, UTF-8, null-terminated, cut to fit:
	 * Windows' FriendlyName ("Wi-Fi", "Ethernet 2"). Empty on Apple, Linux
	 * and Android, which keep none at this level; a caller shows `name`
	 * there. */
	char display_name[CRNET_INTERFACE_NAME_CAPACITY];
	/** The interface's index (if_nametoindex; Windows' IfIndex). Never 0.
	 * What a crnet_address's scope_id and the discovery descs take. */
	uint32_t index;
	/** The index IPv6 uses for it: Windows' Ipv6IfIndex, which may differ
	 * from IfIndex; elsewhere the same as `index`. 0 on Windows for an
	 * interface without IPv6. */
	uint32_t ipv6_index;
	/** CRNET_INTERFACE_FLAG_* bits. */
	uint32_t flags;
	/** The largest IP packet the link takes, in bytes, as the system
	 * reports it; 0 where it reported none. */
	uint32_t mtu;
	/** How many addresses crnet_interface_list_get_address() has for it;
	 * 0 for an interface with none. */
	uint32_t address_count;
} crnet_interface_properties;

/** @brief [ADDED] One address an interface holds, filled by
 * crnet_interface_list_get_address(). */
typedef struct crnet_interface_address {
	/** `type` is CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS; `next` is NULL
	 * (nothing is chained onto this struct yet). The caller sets the type;
	 * Carrottus Net fills in the rest. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** The address, port 0. An IPv6 link-local address has its
	 * interface's ipv6_index as its scope_id, ready to be sent to. */
	crnet_address address;
	/** How many leading bits are the network's: 0-32 for IPv4, 0-128 for
	 * IPv6 (24 for a /24), or [CHANGED] CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN
	 * where the system holds none (Windows' OnLinkPrefixLength 255, "an
	 * illegal value": IP_ADAPTER_UNICAST_ADDRESS_LH, read 2026-11-21). */
	uint32_t prefix_length;
} crnet_interface_address;

/**
 * @brief [ADDED] Takes a snapshot of this machine's interfaces and their
 * addresses: getifaddrs, or GetAdaptersAddresses(AF_UNSPEC,
 * GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
 * GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX [CHANGED]) and
 * GetIfTable2() for the administrative state, copied into the list through the instance's
 * allocator and the system's own memory given back before it returns.
 * Never blocks on the network.
 *
 * @param[in]  instance  The instance the list is made with; destroyed after
 *                       it.
 * @param[out] out_list  Receives the list, the caller's to destroy with
 *                       crnet_interface_list_destroy(). NULL on failure.
 * @param[out] out_error Receives the system's reason when it refuses. May
 *                       be NULL. See @ref crnet_error_detail "Error detail".
 *                       Once card S10 is built, the same platform error
 *                       also goes to the instance's message callback, inside
 *                       this call, as every platform failure does there.
 *
 * @retval CR_SUCCESS The list was made. A machine with no interface at all
 *         gives an empty list, not a failure.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_list` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The list could not be allocated.
 * @retval CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED getifaddrs or
 *         GetAdaptersAddresses failed; `out_error` has its reason (errno,
 *         or the Win32 error).
 */
CR_RESULT crnet_interface_enumerate(
    crnet_instance instance,
    crnet_interface_list *out_list,
    crnet_error *out_error);

/**
 * @brief [ADDED] Reads how many interfaces the list holds.
 *
 * Thread: [CHANGED] a read; may overlap any other read of the same list
 * (the three reads change nothing), never its destroy.
 *
 * @param[in]  list      The list to read.
 * @param[out] out_count Receives the count; 0 for an empty list.
 *
 * @retval CR_SUCCESS `out_count` was written.
 * @retval CR_RESULT_NET_INVALID_INTERFACE_LIST `list` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_count` is NULL.
 */
CR_RESULT crnet_interface_list_get_count(
    crnet_interface_list list,
    uint32_t *out_count);

/**
 * @brief [ADDED] Reads one interface of the list.
 *
 * Thread: [CHANGED] a read, as crnet_interface_list_get_count().
 *
 * @param[in]  list           The list to read.
 * @param[in]  position       Which interface, from 0 to the count less 1,
 *                            in the system's order. Not an interface index.
 * @param[out] out_properties Receives the interface; its `base.type` set by
 *                            the caller.
 *
 * @retval CR_SUCCESS `out_properties` was written.
 * @retval CR_RESULT_NET_INVALID_INTERFACE_LIST `list` is NULL.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `position` is past the last.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_properties` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_properties->base.type` is not
 *         CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_properties->base.next` is
 *         not NULL.
 */
CR_RESULT crnet_interface_list_get_properties(
    crnet_interface_list list,
    uint32_t position,
    crnet_interface_properties *out_properties);

/**
 * @brief [ADDED] Reads one address of one interface of the list, in the
 * order the system gave them.
 *
 * Thread: [CHANGED] a read, as crnet_interface_list_get_count().
 *
 * @param[in]  list             The list to read.
 * @param[in]  position         Which interface, as for
 *                              crnet_interface_list_get_properties().
 * @param[in]  address_position Which of its addresses, from 0 to its
 *                              `address_count` less 1.
 * @param[out] out_address      Receives the address; its `base.type` set
 *                              by the caller.
 *
 * @retval CR_SUCCESS `out_address` was written.
 * @retval CR_RESULT_NET_INVALID_INTERFACE_LIST `list` is NULL.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `position` or `address_position` is
 *         past the last.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_address->base.type` is not
 *         CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_address->base.next` is not
 *         NULL.
 */
CR_RESULT crnet_interface_list_get_address(
    crnet_interface_list list,
    uint32_t position,
    uint32_t address_position,
    crnet_interface_address *out_address);

/**
 * @brief [ADDED] Frees the list.
 *
 * @param[in] list The list to destroy. NULL is taken and nothing is done
 *                 with it.
 */
void crnet_interface_list_destroy(crnet_interface_list list);

/** @} */

#endif // CRNET_EXT_TCP

/*
 * ---------------------------------------------------------------------------
 * Open questions
 * ---------------------------------------------------------------------------
 *
 * 1. A change notification (an interface came up, an address changed:
 *    Wi-Fi joined, a VPN connected)? The systems have one each
 *    (NotifyIpInterfaceChange's callback on a system thread; a netlink
 *    socket, RTM_NEWADDR, on Linux; a PF_ROUTE socket or
 *    SCDynamicStore/nw_path_monitor on Apple), all pollable or wakeable
 *    in the wait set. Recommendation: not now. A caller takes a new
 *    listing when it needs one (a connect screen opened, a discovery or
 *    UDP call failed); a watcher is its own draft when a caller
 *    shows it needs one.
 *    Answered C5.42: not in this card. Callers need to notice a changed network, so the caller relists on a return to the foreground, after a send fails and every keepalive (card X46's caller's rule); a watcher is a draft of its own when that is measured too slow.
 * 2. Windows' UP. Read the administrative state from GetIfEntry2
 *    (MIB_IF_ROW2::AdminStatus) so UP and RUNNING mean on Windows what
 *    they mean elsewhere? Answered C2.14: yes, from one GetIfTable2()
 *    call per listing (no privilege is documented for either call), and
 *    BROADCAST from the broadcast prefix the same way.
 * 3. TCP's addresses as crnet_address too (connect and listener descs
 *    taking one beside or in place of their strings)? Answered C1.4: not
 *    in this card. TCP keeps text until a caller of both wants one form;
 *    then a card, with S5's renames, moves TCP over in one change (a
 *    break of built calls, which this draft makes none of).
 * 4. The kind of interface (Wi-Fi, Ethernet, cellular, loopback, tunnel)?
 *    Answered C2.16: leave it out. Only Windows can say Wi-Fi from
 *    Ethernet at this level; on Apple it takes SystemConfiguration (not on
 *    iOS) and on Linux sysfs (/sys/class/net/<name>/wireless). A field one
 *    platform fills truthfully is a per-platform difference.
 * 5. IPv6 address states (temporary, deprecated, tentative), which say
 *    which address to advertise? Answered C2.16: leave them out now (a caller
 *    that advertises through DNS-SD has the system pick the addresses,
 *    and one that shows an address for typing shows IPv4); add them with Q1's watcher if a
 *    caller shows an IPv6 address, reading Linux's from netlink then.
 * 6. Reads of one list from several threads at once. Answered C1.3:
 *    allowed, each read marked Thread: now, the Debug overlap check on
 *    the destroy only.
 */

#if __cplusplus
}
#endif

#endif // CRNET_INTERFACE_DRAFT_H
