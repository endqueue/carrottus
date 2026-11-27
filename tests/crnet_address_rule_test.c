/*
 * crnet_address_rule_test - TCP's address rule from inside
 * (src/net/crnetinterfaces.c), where crnet_test can't reach (card N26,
 * crnet_tcp_address_draft.h, "Tests"):
 *
 * - crnet_tcp_address_check() over the rule's table, at a listener (LOCAL)
 *   and at connect (REMOTE), every row asserted;
 * - crnet_tcp_peer_from_sockaddr() over made sockaddrs: what the peer
 *   getter refuses, and what it fills;
 * - crnet_address_from_sockaddr()'s Zone mask: Windows keeps the low 28
 *   bits of sin6_scope_id (SCOPE_ID's Zone) before its fallbacks; every
 *   other platform reads the value as given.
 *
 * No network and no system service: a ctest on every platform.
 */

#include "net/crnetshared.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_passed, g_failed;

static void check(bool ok, const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	printf("  %s  ", ok ? "PASS" : "FAIL");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	if (ok)
		g_passed++;
	else
		g_failed++;
}

static crnet_address make(const char *text, uint32_t scope_id, uint16_t port) {
	crnet_address address;
	memset(&address, 0, sizeof(address));
	if (crnet_address_from_string(text, port, 0, &address) != CR_SUCCESS)
		printf("  (from_string refused \"%s\")\n", text);
	// The scope is set by hand: from_string refuses one on IPv4, and the
	// table has rows the check itself has to refuse.
	address.scope_id = scope_id;
	return address;
}

struct row {
	const char *text;
	uint32_t    scope_id;
	uint16_t    port;
	CR_RESULT   local;
	CR_RESULT   remote;
};

#define OK CR_SUCCESS
#define ADDRESS CR_RESULT_NET_INVALID_ADDRESS
#define SCOPE CR_RESULT_NET_INVALID_SCOPE

static const struct row g_rows[] = {
    // Link-local: a scope exactly when, below 2^28.
    {"fe80::1", 1, 80, OK, OK},
    {"fe80::1", 0, 80, SCOPE, SCOPE},
    {"fe80::1", 0x0FFFFFFF, 80, OK, OK},
    {"fe80::1", 0x10000000, 80, SCOPE, SCOPE},
    {"fe80::1", 0xFFFFFFFF, 80, SCOPE, SCOPE},
    // fe80::/10 outside fe80::/64.
    {"febf::1", 1, 80, ADDRESS, ADDRESS},
    {"fe80:abcd::1", 1, 80, ADDRESS, ADDRESS},
    {"fe80:0:0:1::1", 1, 80, ADDRESS, ADDRESS},
    // Site-local, deprecated: scoped by no system here.
    {"fec0::1", 1, 80, SCOPE, SCOPE},
    {"fec0::1", 0, 80, OK, OK},
    // All zero: every interface at a listener, refused at connect.
    {"::", 1, 80, SCOPE, ADDRESS},
    {"::", 0, 0, OK, ADDRESS},
    {"::", 0, 80, OK, ADDRESS},
    {"0.0.0.0", 0, 0, OK, ADDRESS},
    {"0.0.0.0", 0, 80, OK, ADDRESS},
    // Loopback and global: no scope.
    {"::1", 5, 80, SCOPE, SCOPE},
    {"::1", 0, 80, OK, OK},
    {"2001:db8::1", 0x10000000, 80, SCOPE, SCOPE},
    {"2001:db8::1", 3, 80, SCOPE, SCOPE},
    {"2001:db8::1", 0, 80, OK, OK},
    {"127.0.0.1", 0, 80, OK, OK},
    // Port 0: the system's choice at a listener, no destination at connect.
    {"127.0.0.1", 0, 0, OK, ADDRESS},
    // Two faults, one answer: the address's.
    {"fe80::1", 0, 0, SCOPE, ADDRESS},
    {"::1", 5, 0, SCOPE, ADDRESS},
    // IPv4-mapped, with or without a scope.
    {"::ffff:127.0.0.1", 0, 80, ADDRESS, ADDRESS},
    {"::ffff:127.0.0.1", 1, 80, ADDRESS, ADDRESS},
    // Multicast, IPv6 at every scope and IPv4.
    {"ff02::1", 0, 80, ADDRESS, ADDRESS},
    {"ff02::1", 1, 80, ADDRESS, ADDRESS},
    {"ff0e::1", 0, 80, ADDRESS, ADDRESS},
    {"ff01::1", 1, 80, ADDRESS, ADDRESS},
    {"224.0.0.251", 0, 80, ADDRESS, ADDRESS},
    {"239.255.255.255", 0, 80, ADDRESS, ADDRESS},
    // The system's to answer: passed on.
    {"255.255.255.255", 0, 80, OK, OK},
    {"0.1.2.3", 0, 80, OK, OK},
    {"::127.0.0.1", 0, 80, OK, OK},
    {"64:ff9b::7f00:1", 0, 80, OK, OK},
};

static void test_check(void) {
	printf("crnet_tcp_address_check(), the rule's table\n");
	for (size_t i = 0; i < sizeof(g_rows) / sizeof(g_rows[0]); ++i) {
		const struct row   *row     = &g_rows[i];
		const crnet_address address = make(row->text, row->scope_id, row->port);
		const CR_RESULT     local   = crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_LOCAL);
		const CR_RESULT     remote  = crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_REMOTE);
		check(local == row->local, "LOCAL  %s scope %u port %u: %s (want %s)", row->text, (unsigned)row->scope_id, (unsigned)row->port, cr_result_str(local),
		      cr_result_str(row->local));
		check(remote == row->remote, "REMOTE %s scope %u port %u: %s (want %s)", row->text, (unsigned)row->scope_id, (unsigned)row->port, cr_result_str(remote),
		      cr_result_str(row->remote));
	}

	// What text can't state: an IPv4 address with a scope, a zeroed
	// struct, a family that is neither, `reserved` set, IPv4 bytes past
	// the fourth.
	crnet_address address = make("127.0.0.1", 5, 80);
	check(crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_LOCAL) == ADDRESS && crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_REMOTE) == ADDRESS,
	      "127.0.0.1 with scope 5: INVALID_ADDRESS at both, not INVALID_SCOPE");
	memset(&address, 0, sizeof(address));
	check(crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_LOCAL) == ADDRESS && crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_REMOTE) == ADDRESS,
	      "a zeroed struct (family ANY): INVALID_ADDRESS at both");
	address        = make("::1", 0, 80);
	address.family = (CRNET_ADDRESS_FAMILY)7;
	check(crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_REMOTE) == ADDRESS, "family 7: INVALID_ADDRESS");
	address          = make("::1", 0, 80);
	address.reserved = 1;
	check(crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_REMOTE) == ADDRESS, "`reserved` 1: INVALID_ADDRESS");
	address          = make("127.0.0.1", 0, 80);
	address.bytes[4] = 1;
	check(crnet_tcp_address_check(&address, CRNET_TCP_ADDRESS_REMOTE) == ADDRESS, "IPv4 with a fifth byte: INVALID_ADDRESS");
	check(crnet_tcp_address_check(NULL, CRNET_TCP_ADDRESS_REMOTE) == ADDRESS, "NULL: INVALID_ADDRESS");
}

static struct sockaddr_in6 sockaddr6(const char *text, uint32_t scope_id, uint16_t port) {
	struct sockaddr_in6 in6;
	memset(&in6, 0, sizeof(in6));
	in6.sin6_family   = AF_INET6;
	in6.sin6_port     = htons(port);
	in6.sin6_scope_id = scope_id;
	inet_pton(AF_INET6, text, &in6.sin6_addr);
	return in6;
}

static bool all_zero(const crnet_address *address) {
	const uint8_t *bytes = (const uint8_t *)address;
	for (size_t i = 0; i < sizeof(*address); ++i) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}

static void test_peer(void) {
	printf("crnet_tcp_peer_from_sockaddr(), the peer getter's reader\n");
	crnet_address out;

	struct sockaddr_in6 in6 = sockaddr6("fe80::1", 0, 4242);
	memset(&out, 0xA5, sizeof(out));
	CR_RESULT result = crnet_tcp_peer_from_sockaddr(&in6, sizeof(in6), AF_INET6, &out);
	check(result == CR_RESULT_GENERAL_FAILURE && all_zero(&out), "fe80::1, sin6_scope_id 0, nothing embedded: GENERAL_FAILURE, the output zeroed (%s)",
	      cr_result_str(result));

	in6                          = sockaddr6("fe80::1", 0, 4242);
	((uint8_t *)&in6.sin6_addr)[3] = 7; // a KAME stack's embedded index
	result                       = crnet_tcp_peer_from_sockaddr(&in6, sizeof(in6), AF_INET6, &out);
	{
		const crnet_address want = make("fe80::1", 7, 4242);
		check(result == CR_SUCCESS && memcmp(&out, &want, sizeof(out)) == 0, "fe80::1 with 7 embedded in bytes 2 and 3: scope 7, the bytes cleared (%s)",
		      cr_result_str(result));
	}

	in6    = sockaddr6("fe80::1", 3, 4242);
	result = crnet_tcp_peer_from_sockaddr(&in6, sizeof(in6), AF_INET6, &out);
	{
		const crnet_address want = make("fe80::1", 3, 4242);
		check(result == CR_SUCCESS && memcmp(&out, &want, sizeof(out)) == 0, "fe80::1, sin6_scope_id 3: the scope as given, the port (%s)", cr_result_str(result));
		check(crnet_tcp_address_check(&out, CRNET_TCP_ADDRESS_REMOTE) == CR_SUCCESS, "and it passes connect's rule");
	}

	in6                            = sockaddr6("fe80::1", 1, 4242);
	((uint8_t *)&in6.sin6_addr)[5] = 1; // outside fe80::/64
	memset(&out, 0xA5, sizeof(out));
	result = crnet_tcp_peer_from_sockaddr(&in6, sizeof(in6), AF_INET6, &out);
	check(result == CR_RESULT_GENERAL_FAILURE && all_zero(&out), "fe80::1 with byte 5 set (outside fe80::/64), scope 1: GENERAL_FAILURE, zeroed (%s)",
	      cr_result_str(result));

	in6    = sockaddr6("fe80::1:0:0:0:1", 1, 4242);
	result = crnet_tcp_peer_from_sockaddr(&in6, sizeof(in6), AF_INET6, &out);
	check(result == CR_RESULT_GENERAL_FAILURE, "fe80::1:0:0:0:1, scope 1: GENERAL_FAILURE (%s)", cr_result_str(result));

	in6    = sockaddr6("::1", 0, 4242);
	result = crnet_tcp_peer_from_sockaddr(&in6, sizeof(in6), AF_INET6, &out);
	{
		const crnet_address want = make("::1", 0, 4242);
		check(result == CR_SUCCESS && memcmp(&out, &want, sizeof(out)) == 0, "::1: scope 0 (%s)", cr_result_str(result));
	}

	struct sockaddr_in in;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_port   = htons(4242);
	inet_pton(AF_INET, "127.0.0.1", &in.sin_addr);
	memset(&out, 0xA5, sizeof(out));
	result = crnet_tcp_peer_from_sockaddr(&in, sizeof(in), AF_INET6, &out);
	check(result == CR_RESULT_GENERAL_FAILURE && all_zero(&out), "an IPv4 sockaddr for an IPv6 socket: GENERAL_FAILURE, zeroed (%s)", cr_result_str(result));
	result = crnet_tcp_peer_from_sockaddr(&in, sizeof(in), AF_INET, &out);
	{
		const crnet_address want = make("127.0.0.1", 0, 4242);
		check(result == CR_SUCCESS && memcmp(&out, &want, sizeof(out)) == 0, "the same for an IPv4 socket: 127.0.0.1 (%s)", cr_result_str(result));
	}

	struct sockaddr_in6 zero_family;
	memset(&zero_family, 0, sizeof(zero_family));
	memset(&out, 0xA5, sizeof(out));
	result = crnet_tcp_peer_from_sockaddr(&zero_family, sizeof(zero_family), AF_INET6, &out);
	check(result == CR_RESULT_GENERAL_FAILURE && all_zero(&out), "a family of 0: GENERAL_FAILURE, zeroed (%s)", cr_result_str(result));

	in6    = sockaddr6("fe80::1", 3, 4242);
	result = crnet_tcp_peer_from_sockaddr(&in6, 8, AF_INET6, &out);
	check(result == CR_RESULT_GENERAL_FAILURE, "a short length: GENERAL_FAILURE (%s)", cr_result_str(result));
}

static void test_zone_mask(void) {
	printf("crnet_address_from_sockaddr(), the Zone mask\n");
	crnet_address       out;
	struct sockaddr_in6 in6 = sockaddr6("fe80::1", 0x2000000F, 0);
	bool                read = crnet_address_from_sockaddr(&in6, sizeof(in6), 9, &out, NULL);
	in6                      = sockaddr6("fe80::1", 0x20000000, 0);
	crnet_address out_level  = {0};
	bool          read_level = crnet_address_from_sockaddr(&in6, sizeof(in6), 9, &out_level, NULL);
#ifdef _WIN32
	check(read && out.scope_id == 15, "Windows: 0x2000000F reads as its Zone, 15 (%u)", (unsigned)out.scope_id);
	check(read_level && out_level.scope_id == 9, "Windows: 0x20000000, a Zone of 0, reads as the interface fallback, 9 (%u)", (unsigned)out_level.scope_id);
#else
	check(read && out.scope_id == 0x2000000F, "0x2000000F reads as given: the mask is Windows' only (0x%x)", (unsigned)out.scope_id);
	check(read_level && out_level.scope_id == 0x20000000, "0x20000000 reads as given (0x%x)", (unsigned)out_level.scope_id);
#endif
}

int main(void) {
	test_check();
	test_peer();
	test_zone_mask();
	printf("\n%d passed, %d failed\n", g_passed, g_failed);
	return g_failed == 0 ? 0 : 1;
}
