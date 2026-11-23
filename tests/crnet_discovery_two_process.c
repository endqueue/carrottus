/*
 * crnet_discovery_two_process - discovery and UDP between two processes on
 * one machine, through the system's own service (POSIX only: Apple, and
 * Linux once Avahi is built).
 *
 * The driver spawns itself as the advertiser (posix_spawn, the role as an
 * argument). The advertiser opens a UDP socket on every IPv4 address and
 * one on every IPv6 address of one port, advertises _crnettest._udp on
 * that port with a TXT record, and writes "REGISTERED <port> <name>" to a
 * pipe; then echoes every datagram back to where it came from. The driver
 * browses for that name, resolves it, and sends a datagram to each address
 * found, the echo being the proof of delivery: loopback's first, then the
 * active interface's IPv4 address and its fe80:: address with the scope
 * (en0, or the interface `route -n get default` names; with none, the
 * off-loopback half is not run, which is said and not a failure).
 * Addresses on awdl0, llw0, utun*, anpi* and bridge* are seen and recorded,
 * not sent to. Then the advertiser goes, once by a clean exit (its advert
 * destroyed, so the system says goodbye) and once by SIGKILL (the system's
 * own cleanup of a client that went), and each time the driver's browser
 * sees every FOUND of it LOST within the bound.
 *
 *   crnet_discovery_two_process              the driver (a ctest)
 *   crnet_discovery_two_process advertiser   what the driver spawns
 */

#include <crnet.h>

#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TYPE     "_crnettest._udp"
#define BOUND_MS 5000u

static int g_passed, g_failed;

static void check(bool ok, const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	printf("  %s  ", ok ? "PASS" : "FAIL");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	fflush(stdout);
	if (ok)
		g_passed++;
	else
		g_failed++;
}

static void note(const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	printf("  NOTE  ");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	fflush(stdout);
}

static uint64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static crnet_instance make_instance(CR_ALLOCATOR *allocator) {
	*allocator = cr_allocator_system();
	crnet_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	desc.allocator          = allocator;
	crnet_instance instance = NULL;
	if (crnet_instance_create(&desc, &instance, NULL) != CR_SUCCESS)
		return NULL;
	return instance;
}

static crnet_udp_socket open_udp(crnet_instance instance, const char *any, uint16_t port) {
	crnet_udp_socket_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC;
	crnet_address_from_string(any, port, 0, &desc.local);
	desc.sharing             = CRNET_UDP_PORT_SHARING_EXCLUSIVE;
	desc.receive_buffer_size = CRNET_SOCKET_BUFFER_SIZE_SYSTEM;
	desc.send_buffer_size    = CRNET_SOCKET_BUFFER_SIZE_SYSTEM;
	crnet_udp_socket socket  = NULL;
	if (crnet_udp_socket_create(instance, &desc, &socket, NULL) != CR_SUCCESS)
		return NULL;
	return socket;
}

// --------------------------------------------------------------------------
// The advertiser
// --------------------------------------------------------------------------

/* An advertiser's failure, said on stderr (the driver's ctest keeps it). */
static int advertiser_failed(int code, const char *what, CR_RESULT result) {
	fprintf(stderr, "advertiser %ld: %s: %s\n", (long)getpid(), what, cr_result_str(result));
	return code;
}

static int advertise(int pipe_fd) {
	CR_ALLOCATOR   allocator;
	crnet_instance instance = make_instance(&allocator);
	if (!instance)
		return advertiser_failed(2, "no instance", CR_SUCCESS);
	crnet_udp_socket four = open_udp(instance, "0.0.0.0", 0);
	crnet_address    local;
	if (!four || crnet_udp_socket_get_local_address(four, &local) != CR_SUCCESS)
		return advertiser_failed(2, "no IPv4 socket", CR_SUCCESS);
	// The IPv6 socket on the same port: another process may hold that port
	// on IPv6 already, so a few tries, each on a new IPv4 port.
	crnet_udp_socket six = open_udp(instance, "::", local.port);
	for (int tries = 0; !six && tries < 8; ++tries) {
		fprintf(stderr, "advertiser %ld: port %u is held on IPv6; another\n", (long)getpid(), (unsigned)local.port);
		crnet_udp_socket_destroy(four);
		four = open_udp(instance, "0.0.0.0", 0);
		if (!four || crnet_udp_socket_get_local_address(four, &local) != CR_SUCCESS)
			return advertiser_failed(2, "no IPv4 socket", CR_SUCCESS);
		six = open_udp(instance, "::", local.port);
	}
	if (!six)
		return advertiser_failed(2, "no IPv6 socket on the IPv4 socket's port", CR_SUCCESS);
	char name[CRNET_SERVICE_NAME_CAPACITY];
	snprintf(name, sizeof(name), "crnettwo-%ld", (long)getpid());
	static const crnet_service_txt_entry entries[1] = {{"role", "advertiser"}};
	crnet_service_advert_desc            desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type             = CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC;
	desc.name                  = name;
	desc.type                  = TYPE;
	desc.port                  = local.port;
	desc.scope                 = CRNET_DISCOVERY_SCOPE_ALL_INTERFACES;
	desc.txt_entries           = entries;
	desc.txt_entry_count       = 1;
	crnet_service_advert advert  = NULL;
	const CR_RESULT      created = crnet_service_advert_create(instance, &desc, &advert, NULL);
	if (created != CR_SUCCESS)
		return advertiser_failed(2, "the advert's create", created);
	crnet_service_advert_event event;
	memset(&event, 0, sizeof(event));
	event.base.type      = CR_STRUCT_TYPE_NET_SERVICE_ADVERT_EVENT;
	const uint64_t start = now_ms();
	bool           registered = false;
	CR_RESULT      last       = CR_RESULT_NET_TIMED_OUT;
	while (!registered && now_ms() - start < BOUND_MS) {
		last = crnet_service_advert_next(advert, 100, &event);
		if (last == CR_SUCCESS)
			registered = event.kind == CRNET_SERVICE_ADVERT_EVENT_REGISTERED;
		else if (last != CR_RESULT_NET_TIMED_OUT)
			break;
	}
	if (!registered)
		return advertiser_failed(3, "no REGISTERED within the bound", last);
	char line[128];
	const int length = snprintf(line, sizeof(line), "REGISTERED %u %s\n", (unsigned)local.port, name);
	if (write(pipe_fd, line, (size_t)length) != length)
		return 2;

	// Echo until a "QUIT" comes; a SIGKILL may end it first.
	crnet_wait_set set = NULL;
	crnet_wait_set_create(instance, &set, NULL);
	static int four_tag, six_tag;
	crnet_wait_set_add_udp_socket(set, four, CRNET_WAIT_FOR_INCOMING, &four_tag);
	crnet_wait_set_add_udp_socket(set, six, CRNET_WAIT_FOR_INCOMING, &six_tag);
	bool quit = false;
	while (!quit) {
		crnet_wait_event events[2];
		uint32_t         count = 0;
		if (crnet_wait_set_wait(set, 60000, events, 2, &count) != CR_SUCCESS)
			break; // no word in a minute: the driver has gone
		for (uint32_t i = 0; i < count; ++i) {
			crnet_udp_socket socket = events[i].user == &four_tag ? four : six;
			uint8_t          buffer[512];
			uint32_t         size = 0;
			crnet_address    from;
			while (crnet_udp_socket_recv_from(socket, buffer, sizeof(buffer), CRNET_NO_WAIT, &size, &from) == CR_SUCCESS) {
				if (size == 4 && memcmp(buffer, "QUIT", 4) == 0)
					quit = true;
				crnet_udp_socket_send_to(socket, &from, buffer, size, 1000);
			}
		}
	}
	// A clean exit: the advert destroyed, so the system sends its goodbye.
	crnet_wait_set_destroy(set);
	crnet_service_advert_destroy(advert);
	crnet_udp_socket_destroy(six);
	crnet_udp_socket_destroy(four);
	crnet_instance_destroy(instance);
	return 0;
}

// --------------------------------------------------------------------------
// The driver
// --------------------------------------------------------------------------

struct spawned {
	pid_t    pid;
	uint16_t port;
	char     name[CRNET_SERVICE_NAME_CAPACITY];
};

static bool spawn_advertiser(const char *self, struct spawned *out) {
	int ends[2];
	if (pipe(ends) != 0)
		return false;
	char fd_text[16];
	snprintf(fd_text, sizeof(fd_text), "%d", ends[1]);
	char *const argv[] = {(char *)self, (char *)"advertiser", fd_text, NULL};
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addclose(&actions, ends[0]);
	const int spawned = posix_spawn(&out->pid, self, &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(ends[1]);
	if (spawned != 0) {
		close(ends[0]);
		return false;
	}
	// Its REGISTERED line, within the bound.
	char           line[128];
	size_t         have  = 0;
	const uint64_t start = now_ms();
	while (have < sizeof(line) - 1 && now_ms() - start < BOUND_MS * 2) {
		struct timeval wait = {0, 100000};
		fd_set         readable;
		FD_ZERO(&readable);
		FD_SET(ends[0], &readable);
		if (select(ends[0] + 1, &readable, NULL, NULL, &wait) <= 0)
			continue;
		const ssize_t got = read(ends[0], line + have, 1);
		if (got <= 0)
			break;
		have += (size_t)got;
		if (line[have - 1] == '\n')
			break;
	}
	close(ends[0]);
	line[have] = '\0';
	unsigned port = 0;
	if (sscanf(line, "REGISTERED %u %63s", &port, out->name) != 2)
		return false;
	out->port = (uint16_t)port;
	return true;
}

/* Reads the browser's events for `name` for up to `ms`, counting each FOUND
 * (by interface) and LOST, until `found_at_least` FOUND and `lost_at_least`
 * LOST are in. */
static uint32_t g_found_on[16];
static uint32_t g_found_on_count;

static void browse(crnet_service_browser browser, const char *name, uint32_t ms, uint32_t found_at_least, uint32_t lost_at_least, uint32_t *found,
                   uint32_t *lost, uint32_t *first_interface) {
	const uint64_t start = now_ms();
	while (now_ms() - start < ms && !(*found >= found_at_least && *lost >= lost_at_least)) {
		crnet_service_browse_event event;
		memset(&event, 0, sizeof(event));
		event.base.type = CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT;
		if (crnet_service_browser_next(browser, 100, &event) != CR_SUCCESS || strcmp(event.name, name) != 0)
			continue;
		if (event.kind == CRNET_SERVICE_BROWSE_EVENT_FOUND) {
			if (*found == 0 && first_interface)
				*first_interface = event.interface_index;
			if (g_found_on_count < 16)
				g_found_on[g_found_on_count++] = event.interface_index;
			(*found)++;
		} else if (event.kind == CRNET_SERVICE_BROWSE_EVENT_LOST) {
			(*lost)++;
		}
	}
}

/* The interface `route -n get default` names, or "" for none. */
static void default_interface(char out[IF_NAMESIZE + 1]) {
	out[0]     = '\0';
	FILE *pipe = popen("/sbin/route -n get default 2>/dev/null", "r");
	if (!pipe)
		return;
	char line[256];
	while (fgets(line, sizeof(line), pipe)) {
		char name[64];
		if (sscanf(line, " interface: %63s", name) == 1) {
			snprintf(out, IF_NAMESIZE + 1, "%s", name);
			break;
		}
	}
	pclose(pipe);
}

/* The interface an address is on: its scope, or for IPv4 the listing's
 * interface that holds it. */
static void interface_of(crnet_instance instance, const crnet_address *address, char out[CRNET_INTERFACE_NAME_CAPACITY]) {
	out[0] = '\0';
	if (address->scope_id) {
		char name[IF_NAMESIZE + 1];
		if (if_indextoname(address->scope_id, name))
			snprintf(out, CRNET_INTERFACE_NAME_CAPACITY, "%s", name);
		return;
	}
	crnet_interface_list list = NULL;
	if (crnet_interface_enumerate(instance, &list, NULL) != CR_SUCCESS)
		return;
	uint32_t count = 0;
	crnet_interface_list_get_count(list, &count);
	for (uint32_t i = 0; i < count && !out[0]; ++i) {
		crnet_interface_properties properties;
		memset(&properties, 0, sizeof(properties));
		properties.base.type = CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES;
		crnet_interface_list_get_properties(list, i, &properties);
		for (uint32_t a = 0; a < properties.address_count; ++a) {
			crnet_interface_address held;
			memset(&held, 0, sizeof(held));
			held.base.type = CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS;
			crnet_interface_list_get_address(list, i, a, &held);
			if (held.address.family == address->family && memcmp(held.address.bytes, address->bytes, 16) == 0)
				snprintf(out, CRNET_INTERFACE_NAME_CAPACITY, "%s", properties.name);
		}
	}
	crnet_interface_list_destroy(list);
}

static bool skipped_interface(const char *name) {
	static const char *const prefixes[] = {"awdl", "llw", "utun", "anpi", "bridge"};
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
		if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
			return true;
	}
	return false;
}

/* Sends a datagram to `to` and waits for its echo: true when it came back. */
static bool ping(crnet_udp_socket socket, const crnet_address *to, uint32_t n) {
	char message[32];
	const int length = snprintf(message, sizeof(message), "PING %u", n);
	if (crnet_udp_socket_send_to(socket, to, message, (uint32_t)length, 1000) != CR_SUCCESS)
		return false;
	const uint64_t start = now_ms();
	while (now_ms() - start < 2000) {
		uint8_t       buffer[64];
		uint32_t      size = 0;
		crnet_address from;
		if (crnet_udp_socket_recv_from(socket, buffer, sizeof(buffer), 200, &size, &from) != CR_SUCCESS)
			continue;
		if (size == (uint32_t)length && memcmp(buffer, message, size) == 0)
			return true;
	}
	return false;
}

static int drive(const char *self) {
	CR_ALLOCATOR   allocator;
	crnet_instance instance = make_instance(&allocator);
	if (!instance)
		return 2;
	crnet_service_browser_desc bdesc;
	memset(&bdesc, 0, sizeof(bdesc));
	bdesc.base.type             = CR_STRUCT_TYPE_NET_SERVICE_BROWSER_DESC;
	bdesc.type                  = TYPE;
	bdesc.scope                 = CRNET_DISCOVERY_SCOPE_ALL_INTERFACES;
	bdesc.event_capacity        = 256;
	crnet_udp_socket four       = open_udp(instance, "0.0.0.0", 0);
	crnet_udp_socket six        = open_udp(instance, "::", 0);
	check(four && six, "the driver's UDP sockets, IPv4 and IPv6");
	char active[IF_NAMESIZE + 1];
	default_interface(active);
	if (!active[0] && if_nametoindex("en0"))
		snprintf(active, sizeof(active), "en0");
	note("the active interface: %s", active[0] ? active : "none (the off-loopback half is not run)");

	for (int round = 0; round < 2; ++round) {
		const bool killed = round == 1;
		printf("\n== %s\n", killed ? "The advertiser killed (SIGKILL)" : "The advertiser, and its clean exit");
		crnet_service_browser browser = NULL;
		if (crnet_service_browser_create(instance, &bdesc, &browser, NULL) != CR_SUCCESS) {
			check(false, "a browser");
			continue;
		}
		struct spawned advertiser;
		memset(&advertiser, 0, sizeof(advertiser));
		if (!spawn_advertiser(self, &advertiser)) {
			int status = 0;
			if (advertiser.pid > 0 && waitpid(advertiser.pid, &status, 0) == advertiser.pid)
				note("the advertiser %s with %d", WIFEXITED(status) ? "exited" : "was ended", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
			check(false, "the advertiser spawned and REGISTERED within its bound");
			crnet_service_browser_destroy(browser);
			continue;
		}
		check(true, "the advertiser %s REGISTERED on port %u", advertiser.name, advertiser.port);
		uint32_t found = 0, lost = 0, first = 0;
		g_found_on_count = 0;
		browse(browser, advertiser.name, BOUND_MS, 1, 0, &found, &lost, &first);
		check(found >= 1, "FOUND it within %u ms", BOUND_MS);
		browse(browser, advertiser.name, 500, 99, 0, &found, &lost, NULL);
		note("FOUND %u time%s", found, found == 1 ? "" : "s");

		if (!killed) {
			// Resolve on each interface it was found on (a resolve on any
			// interface looks addresses up on the first that answers only),
			// collecting addresses for a moment on each.
			crnet_address addresses[32];
			uint32_t      address_count = 0;
			bool          service       = false;
			for (uint32_t f = 0; f < g_found_on_count; ++f) {
				crnet_service_resolver_desc rdesc;
				memset(&rdesc, 0, sizeof(rdesc));
				rdesc.base.type                 = CR_STRUCT_TYPE_NET_SERVICE_RESOLVER_DESC;
				rdesc.name                      = advertiser.name;
				rdesc.type                      = TYPE;
				rdesc.interface_index           = g_found_on[f];
				rdesc.family                    = CRNET_ADDRESS_FAMILY_ANY;
				rdesc.event_capacity            = 64;
				crnet_service_resolver resolver = NULL;
				if (crnet_service_resolver_create(instance, &rdesc, &resolver, NULL) != CR_SUCCESS)
					continue;
				const uint64_t start = now_ms();
				while (now_ms() - start < 1500) {
					crnet_service_resolve_event event;
					memset(&event, 0, sizeof(event));
					event.base.type = CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT;
					if (crnet_service_resolver_next(resolver, 100, &event) != CR_SUCCESS)
						continue;
					if (event.kind == CRNET_SERVICE_RESOLVE_EVENT_SERVICE) {
						service = service || (event.port == advertiser.port && event.txt_size == 16 && memcmp(event.txt, "\x0frole=advertiser", 16) == 0);
					} else if (event.kind == CRNET_SERVICE_RESOLVE_EVENT_ADDRESS_ADDED && address_count < 32) {
						bool known = false;
						for (uint32_t i = 0; i < address_count && !known; ++i)
							known = memcmp(&addresses[i], &event.address, sizeof(event.address)) == 0;
						if (!known)
							addresses[address_count++] = event.address;
					}
				}
				crnet_service_resolver_destroy(resolver);
			}
			check(service, "resolved: its port and TXT record");
			check(address_count > 0, "and %u address%s", address_count, address_count == 1 ? "" : "es");

			// Datagrams to each, loopback's first, then the active
			// interface's IPv4 address and fe80:: address.
			bool loopback_four = false, loopback_six = false, active_four = false, active_six = false;
			for (int pass = 0; pass < 2; ++pass) {
				for (uint32_t i = 0; i < address_count; ++i) {
					char where[CRNET_INTERFACE_NAME_CAPACITY];
					interface_of(instance, &addresses[i], where);
					char text[CRNET_MAX_ADDRESS_STRING_LEN] = "?";
					crnet_address_to_string(&addresses[i], text);
					const bool loopback = strcmp(where, "lo0") == 0 || strcmp(where, "lo") == 0;
					if (pass == 0 && !loopback)
						continue;
					if (pass == 1 && loopback)
						continue;
					const bool is_four = addresses[i].family == CRNET_ADDRESS_FAMILY_IPV4;
					if (skipped_interface(where)) {
						note("%s%%%u on %s: seen, skipped (not sent to)", text, addresses[i].scope_id, where);
						continue;
					}
					if (!loopback && (!active[0] || strcmp(where, active) != 0)) {
						note("%s%%%u on %s: seen, not the active interface", text, addresses[i].scope_id, where[0] ? where : "?");
						continue;
					}
					const bool echoed = ping(is_four ? four : six, &addresses[i], i);
					check(echoed, "%s %s%%%u on %s: a datagram echoed back", is_four ? "IPv4" : "IPv6", text, addresses[i].scope_id, where);
					bool *reached = loopback ? (is_four ? &loopback_four : &loopback_six) : (is_four ? &active_four : &active_six);
					*reached      = *reached || echoed;
				}
			}
			check(loopback_four || loopback_six, "loopback reached");
			if (active[0])
				note("off loopback on %s: IPv4 %s, fe80:: %s", active, active_four ? "reached" : "not reached or not found",
				     active_six ? "reached" : "not reached or not found");

			// The clean exit.
			crnet_address quit_to;
			crnet_address_from_string("127.0.0.1", advertiser.port, 0, &quit_to);
			crnet_udp_socket_send_to(four, &quit_to, "QUIT", 4, 1000);
		} else {
			kill(advertiser.pid, SIGKILL);
		}

		const uint64_t gone_at = now_ms();
		int            status  = 0;
		const pid_t    reaped  = waitpid(advertiser.pid, &status, 0);
		check(reaped == advertiser.pid && (killed ? (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) : (WIFEXITED(status) && WEXITSTATUS(status) == 0)),
		      killed ? "the advertiser killed" : "the advertiser exited cleanly");
		browse(browser, advertiser.name, BOUND_MS, found, found, &found, &lost, NULL);
		check(lost == found, "and seen gone: LOST %u of %u within %u ms (%llu ms)", lost, found, BOUND_MS, (unsigned long long)(now_ms() - gone_at));
		crnet_service_browser_destroy(browser);
	}
	crnet_udp_socket_destroy(four);
	crnet_udp_socket_destroy(six);
	crnet_instance_destroy(instance);
	printf("\n%d passed, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);
	if (argc >= 3 && strcmp(argv[1], "advertiser") == 0)
		return advertise(atoi(argv[2]));
	return drive(argv[0]);
}
