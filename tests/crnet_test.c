/*
 * crnet_test — Carrottus Net against a real network stack and a server that
 * isn't Carrottus Net.
 *
 * The first run of Carrottus Net at all: it goes through crnet.h part by part
 * - TCP sockets and listeners, the wait set, the resolver, HTTP transfers,
 * the TLS and proxy descs, WebSockets - and checks each promise the header
 * makes that can be checked from one machine. The other end is
 * tests/crnet_test_server.py, on this machine, which says where it is
 * listening in a file this program reads.
 *
 *   crnet_test <info file>          every part
 *   crnet_test <info file> <part>   one of: tcp wait resolver http tls proxy websocket
 *                                   resumable (or internet, opt-in)
 *
 * tests/run_crnet_test.sh starts the server, runs this and stops the
 * server again.
 *
 * Three kinds of line come out. PASS and FAIL are checks against the
 * header. NOTE is something the header leaves to the stack, printed so
 * that what each stack does is on record - the request headers it adds,
 * the platform's error code behind a failure. Exits 0 when every check
 * passes, 1 when one fails, 2 when it couldn't start.
 */

#include <crnet.h>

#include "crtest_allocator.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE test_thread;
typedef DWORD  test_thread_result;
#define TEST_THREAD_CALL WINAPI
static void sleep_ms(uint32_t ms) { Sleep(ms); }
static test_thread thread_start(test_thread_result(TEST_THREAD_CALL *run)(void *), void *argument) { return CreateThread(NULL, 0, run, argument, 0, NULL); }
static void thread_join(test_thread thread) {
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
}
static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t test_thread;
typedef void     *test_thread_result;
#define TEST_THREAD_CALL
static void sleep_ms(uint32_t ms) {
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}
static test_thread thread_start(test_thread_result (*run)(void *), void *argument) {
	pthread_t thread;
	pthread_create(&thread, NULL, run, argument);
	return thread;
}
static void thread_join(test_thread thread) { pthread_join(thread, NULL); }
static uint64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
#endif

// Long enough that only something broken runs into it.
#define PATIENCE_MS 15000u

// --------------------------------------------------------------------------
// Reporting
// --------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

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
	fflush(stdout);
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

static void part(const char *name) {
	printf("\n== %s\n", name);
	fflush(stdout);
}

static const char *source_name(CR_PLATFORM_ERROR_SOURCE source) {
	switch (source) {
	case CR_PLATFORM_ERROR_SOURCE_NONE:
		return "none";
	case CR_PLATFORM_ERROR_SOURCE_WIN32:
		return "win32";
	case CR_PLATFORM_ERROR_SOURCE_ERRNO:
		return "errno";
	case CR_PLATFORM_ERROR_SOURCE_RESOLVER:
		return "resolver";
	case CR_PLATFORM_ERROR_SOURCE_CURL:
		return "curl";
	case CR_PLATFORM_ERROR_SOURCE_NSURL:
		return "nsurl";
	case CR_PLATFORM_ERROR_SOURCE_OSSTATUS:
		return "osstatus";
	case CR_PLATFORM_ERROR_SOURCE_TLS_PIN:
		return "tls-pin";
	default:
		return "other";
	}
}

/* Checks a call's result, and for a failure that was expected puts the
 * platform's detail on record. */
static bool expect(CR_RESULT got, CR_RESULT want, const char *what) {
	check(got == want, "%s: %s%s%s", what, cr_result_str(got), got == want ? "" : ", expected ", got == want ? "" : cr_result_str(want));
	return got == want;
}

static void note_error(const char *what, const crnet_error *error) {
	note("%s: detail is %s, source %s, code %lld%s%s%s", what, cr_result_str(error->result), source_name(error->source), (long long)error->code,
	     error->text[0] ? ", \"" : "", error->text, error->text[0] ? "\"" : "");
}

// --------------------------------------------------------------------------
// Where the server is
// --------------------------------------------------------------------------

static struct {
	int      http_port, https_port, ws_port, wss_port, proxy_port;
	char     cert_der_path[1024];
	uint8_t *cert_der;
	size_t   cert_der_size;
	uint8_t  pin[CRNET_TLS_PIN_SIZE];
} g_server;

static bool read_info(const char *path) {
	FILE *file = fopen(path, "r");
	if (!file)
		return false;
	char line[2048];
	char pin_hex[128] = "";
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *equals = strchr(line, '=');
		if (!equals)
			continue;
		*equals           = '\0';
		const char *value = equals + 1;
		if (strcmp(line, "http_port") == 0)
			g_server.http_port = atoi(value);
		else if (strcmp(line, "https_port") == 0)
			g_server.https_port = atoi(value);
		else if (strcmp(line, "ws_port") == 0)
			g_server.ws_port = atoi(value);
		else if (strcmp(line, "wss_port") == 0)
			g_server.wss_port = atoi(value);
		else if (strcmp(line, "proxy_port") == 0)
			g_server.proxy_port = atoi(value);
		else if (strcmp(line, "cert_der") == 0)
			snprintf(g_server.cert_der_path, sizeof(g_server.cert_der_path), "%s", value);
		else if (strcmp(line, "pin") == 0)
			snprintf(pin_hex, sizeof(pin_hex), "%s", value);
	}
	fclose(file);

	for (int i = 0; i < CRNET_TLS_PIN_SIZE; ++i) {
		unsigned byte = 0;
		if (sscanf(pin_hex + i * 2, "%2x", &byte) != 1)
			return false;
		g_server.pin[i] = (uint8_t)byte;
	}
	FILE *der = fopen(g_server.cert_der_path, "rb");
	if (!der)
		return false;
	fseek(der, 0, SEEK_END);
	long size = ftell(der);
	fseek(der, 0, SEEK_SET);
	g_server.cert_der      = (uint8_t *)malloc((size_t)size);
	g_server.cert_der_size = fread(g_server.cert_der, 1, (size_t)size, der);
	fclose(der);
	return g_server.http_port && g_server.https_port && g_server.ws_port && g_server.wss_port && g_server.proxy_port && g_server.cert_der_size > 0;
}

/* The byte the server's /big and "send:N" put at `position`. */
static uint8_t pattern_at(uint64_t position) { return (uint8_t)(((position % 256) * 7 + 3) & 0xFF); }

/* The instance every test makes its objects with, made and destroyed by
 * main. "Instance" below makes others beside it. */
static crnet_instance g_instance = NULL;

/* What g_instance and every other instance is made with, but the tracked one
 * in "Instance": most of this test runs Carrottus Net on several threads, and the
 * tracker (crtest_allocator.h) is for one thread at a time. Set by main. */
static CR_ALLOCATOR g_system;

/* A desc that makes an instance through `allocator`. */
static crnet_instance_desc instance_desc(const CR_ALLOCATOR *allocator) {
	crnet_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	desc.allocator = allocator;
	return desc;
}

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* Makes an instance and destroys it again, on a thread of its own, while
 * the main thread goes on using g_instance. */
static test_thread_result TEST_THREAD_CALL make_instance_elsewhere(void *argument) {
	CR_RESULT                 *result = (CR_RESULT *)argument;
	const crnet_instance_desc desc   = instance_desc(&g_system);
	crnet_instance            other  = NULL;
	*result                            = crnet_instance_create(&desc, &other, NULL);
	sleep_ms(100);
	crnet_instance_destroy(other);
	return 0;
}

/* Defined after the HTTP helpers it uses. */
static void test_instance_memory(void);

static void test_instance(void) {
	part("Instance");

	// The desc's checks, in the order crnet.h lists them.
	crnet_error         error;
	crnet_instance      made = (crnet_instance)(uintptr_t)1;
	crnet_instance_desc desc = instance_desc(&g_system);
	expect(crnet_instance_create(NULL, &made, &error), CR_RESULT_NULL_DESC, "create with no desc");
	desc.base.type = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
	expect(crnet_instance_create(&desc, &made, &error), CR_RESULT_STRUCT_WRONG_TYPE, "create with another desc's type");
	desc.base.type                    = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	const CR_STRUCT_BASE chained_on = {CR_STRUCT_TYPE_NET_RESOLVER_DESC, NULL};
	desc.base.next                    = &chained_on;
	expect(crnet_instance_create(&desc, &made, &error), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "create with something chained on");
	desc.base.next = NULL;
	expect(crnet_instance_create(&desc, NULL, &error), CR_RESULT_NULL_OUTPUT_PARAMETER, "create with nowhere to put it");
	desc.allocator = NULL;
	expect(crnet_instance_create(&desc, &made, &error), CR_RESULT_NULL_ALLOCATOR, "create with no allocator");
	check(made == NULL, "and nothing handed back");
	CR_ALLOCATOR lacking = g_system;
	lacking.allocate       = NULL;
	desc.allocator         = &lacking;
	made                   = (crnet_instance)(uintptr_t)1;
	expect(crnet_instance_create(&desc, &made, &error), CR_RESULT_INVALID_ALLOCATOR, "create with an allocator lacking allocate");
	check(made == NULL, "and nothing handed back");
	lacking      = g_system;
	lacking.free = NULL;
	expect(crnet_instance_create(&desc, &made, &error), CR_RESULT_INVALID_ALLOCATOR, "create with an allocator lacking free");
	desc = instance_desc(&g_system);
	crnet_instance_destroy(NULL);

	test_instance_memory();

	// A second one, beside main's, and gone again while main's stays up.
	crnet_instance second = NULL;
	expect(crnet_instance_create(&desc, &second, &error), CR_SUCCESS, "a second instance while the first exists");
	check(second != NULL && second != g_instance, "which is an instance of its own");

	// One made and destroyed on another thread, while this one is in use.
	CR_RESULT  elsewhere = CR_RESULT_GENERAL_FAILURE;
	test_thread thread    = thread_start(make_instance_elsewhere, &elsewhere);
	crnet_http_client      client = NULL;
	crnet_http_client_desc client_desc;
	memset(&client_desc, 0, sizeof(client_desc));
	client_desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	expect(crnet_http_client_create(second, &client_desc, &client), CR_SUCCESS, "an object made with the second while a third is made elsewhere");
	crnet_http_client_destroy(client);
	thread_join(thread);
	expect(elsewhere, CR_SUCCESS, "an instance made on another thread");

	// No instance, no object.
	client = NULL;
	expect(crnet_http_client_create(NULL, &client_desc, &client), CR_RESULT_NET_INVALID_INSTANCE, "an HTTP client with no instance");
	check(client == NULL, "and nothing handed back");

#if CRNET_EXT_TCP
	crnet_tcp_socket socket = NULL;
	expect(crnet_tcp_socket_create(NULL, &socket), CR_RESULT_NET_INVALID_INSTANCE, "a socket with no instance");
	crnet_wait_set set = NULL;
	expect(crnet_wait_set_create(NULL, &set, &error), CR_RESULT_NET_INVALID_INSTANCE, "a wait set with no instance");
	crnet_resolver      resolver = NULL;
	crnet_resolver_desc resolver_desc;
	memset(&resolver_desc, 0, sizeof(resolver_desc));
	resolver_desc.base.type = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
	resolver_desc.host   = "localhost";
	resolver_desc.family = CRNET_ADDRESS_FAMILY_IPV4;
	expect(crnet_resolver_create(NULL, &resolver_desc, &resolver), CR_RESULT_NET_INVALID_INSTANCE, "a resolver with no instance");

	// A wait set takes members of its own instance and no other's.
	crnet_tcp_listener      listener = NULL;
	crnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address = "127.0.0.1";
	listen_desc.port    = 0;
	listen_desc.backlog = 4;
	int tag             = 0;
	if (expect(crnet_tcp_listener_create(second, &listen_desc, &listener, &error), CR_SUCCESS, "a listener made with the second instance") &&
	    expect(crnet_wait_set_create(g_instance, &set, &error), CR_SUCCESS, "and a wait set made with the first")) {
		expect(crnet_wait_set_add_tcp_listener(set, listener, &tag), CR_RESULT_NET_INVALID_INSTANCE, "the set refuses the other instance's listener");
		crnet_wait_set_destroy(set);
		expect(crnet_wait_set_create(second, &set, &error), CR_SUCCESS, "a wait set made with the second");
		expect(crnet_wait_set_add_tcp_listener(set, listener, &tag), CR_SUCCESS, "takes it");
		crnet_wait_set_remove_tcp_listener(set, listener);
	}
	crnet_wait_set_destroy(set);
	crnet_tcp_listener_destroy(listener);

	// A resolver destroyed with its lookup never run, and then its
	// instance: the order crnet.h allows a lookup still out to be left
	// in. (Run afterwards, aborted, as every resolver has to be run.)
	crnet_instance third = NULL;
	if (expect(crnet_instance_create(&desc, &third, &error), CR_SUCCESS, "a third instance, for a resolver to outlast")) {
		resolver = NULL;
		expect(crnet_resolver_create(third, &resolver_desc, &resolver), CR_SUCCESS, "a resolver made with it");
		crnet_resolver_abort(resolver);
		crnet_resolver_destroy(resolver);
		crnet_instance_destroy(third);
		expect(crnet_resolver_run(resolver), CR_RESULT_NET_ABORTED, "run after its instance has gone returns as an aborted run does");
	}
#endif

	crnet_instance_destroy(second);
}

// --------------------------------------------------------------------------
// TCP
// --------------------------------------------------------------------------

struct abort_later {
	uint32_t           delay_ms;
	crnet_tcp_socket socket;
};

static test_thread_result TEST_THREAD_CALL abort_socket_later(void *argument) {
	struct abort_later *later = (struct abort_later *)argument;
	sleep_ms(later->delay_ms);
	crnet_tcp_socket_abort(later->socket);
	return 0;
}

static void test_tcp(void) {
	part("TCP sockets and listeners");

	crnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address = "127.0.0.1";
	listen_desc.port    = 0;
	listen_desc.backlog = 8;

	crnet_tcp_listener listener = NULL;
	crnet_error        error;
	if (!expect(crnet_tcp_listener_create(g_instance, &listen_desc, &listener, &error), CR_SUCCESS, "listener on 127.0.0.1, port of the system's choosing"))
		return;
	uint16_t port = 0;
	expect(crnet_tcp_listener_get_port(listener, &port), CR_SUCCESS, "get_port");
	check(port != 0, "the system picked a port (%u)", (unsigned)port);

	// A second listener on what the first holds.
	listen_desc.port                = port;
	crnet_tcp_listener second     = NULL;
	CR_RESULT           in_use     = crnet_tcp_listener_create(g_instance, &listen_desc, &second, &error);
	expect(in_use, CR_RESULT_NET_ADDRESS_IN_USE, "a second listener on the same address and port");
	note_error("address in use", &error);
	crnet_tcp_listener_destroy(second);

	crnet_tcp_socket no_one = NULL;
	expect(crnet_tcp_listener_accept(listener, CRNET_NO_WAIT, &no_one), CR_RESULT_NET_TIMED_OUT, "accept with CRNET_NO_WAIT and nobody connecting");
	uint64_t before = now_ms();
	expect(crnet_tcp_listener_accept(listener, 200, &no_one), CR_RESULT_NET_TIMED_OUT, "accept with a 200 ms limit and nobody connecting");
	uint64_t waited = now_ms() - before;
	check(waited >= 150 && waited < 1500, "that accept took about its limit (%llu ms)", (unsigned long long)waited);

	// Connect, accept, and bytes both ways.
	crnet_tcp_socket client = NULL;
	expect(crnet_tcp_socket_create(g_instance, &client), CR_SUCCESS, "socket create");
	uint64_t sent = 0;
	expect(crnet_tcp_socket_send(client, "x", 1, PATIENCE_MS, &sent), CR_RESULT_NET_INVALID_TCP_SOCKET, "send on a socket that isn't connected");

	crnet_tcp_socket_connect_desc connect_desc;
	memset(&connect_desc, 0, sizeof(connect_desc));
	connect_desc.base.type = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
	connect_desc.address    = "127.0.0.1";
	connect_desc.port       = port;
	connect_desc.timeout_ms = PATIENCE_MS;
	expect(crnet_tcp_socket_connect(client, &connect_desc), CR_SUCCESS, "connect to the listener");
	expect(crnet_tcp_socket_connect(client, &connect_desc), CR_RESULT_NET_INVALID_TCP_SOCKET, "a second connect on the same socket");

	crnet_tcp_socket server = NULL;
	expect(crnet_tcp_listener_accept(listener, PATIENCE_MS, &server), CR_SUCCESS, "accept");
	if (!client || !server) {
		crnet_tcp_listener_destroy(listener);
		return;
	}

	char     peer[CRNET_MAX_ADDRESS_STRING_LEN];
	uint16_t peer_port = 0;
	expect(crnet_tcp_socket_get_peer_address(client, peer, &peer_port), CR_SUCCESS, "get_peer_address");
	check(strcmp(peer, "127.0.0.1") == 0 && peer_port == port, "the client's peer is the listener (%s:%u)", peer, (unsigned)peer_port);
	expect(crnet_tcp_socket_set_no_delay(client, true), CR_SUCCESS, "set_no_delay");

	uint8_t  received[64];
	uint64_t count = 0;
	expect(crnet_tcp_socket_recv(server, received, sizeof(received), CRNET_NO_WAIT, &count), CR_RESULT_NET_TIMED_OUT, "recv with CRNET_NO_WAIT and nothing sent");
	expect(crnet_tcp_socket_send(client, "hello", 5, PATIENCE_MS, &sent), CR_SUCCESS, "send 5 bytes");
	check(sent == 5, "all 5 were taken");
	expect(crnet_tcp_socket_recv(server, received, sizeof(received), PATIENCE_MS, &count), CR_SUCCESS, "recv them");
	check(count == 5 && memcmp(received, "hello", 5) == 0, "they arrived as sent");
	expect(crnet_tcp_socket_send(server, "back", 4, PATIENCE_MS, &sent), CR_SUCCESS, "send the other way");
	expect(crnet_tcp_socket_recv(client, received, sizeof(received), PATIENCE_MS, &count), CR_SUCCESS, "recv the other way");
	check(count == 4 && memcmp(received, "back", 4) == 0, "they arrived as sent");

	// A megabyte through short writes and short reads.
	{
		size_t   total = 1024 * 1024;
		uint8_t *out   = (uint8_t *)malloc(total);
		uint8_t *in    = (uint8_t *)malloc(total);
		for (size_t i = 0; i < total; ++i)
			out[i] = pattern_at(i);
		size_t     written = 0, read = 0;
		CR_RESULT result  = CR_SUCCESS;
		while (read < total && !CR_FAILED(result)) {
			// As much as goes without waiting, then take what has arrived.
			if (written < total) {
				result = crnet_tcp_socket_send(client, out + written, total - written, CRNET_NO_WAIT, &sent);
				if (result == CR_RESULT_NET_TIMED_OUT)
					result = CR_SUCCESS;
				else if (!CR_FAILED(result))
					written += (size_t)sent;
			}
			if (!CR_FAILED(result)) {
				result = crnet_tcp_socket_recv(server, in + read, total - read, 20, &count);
				if (result == CR_RESULT_NET_TIMED_OUT)
					result = CR_SUCCESS;
				else if (!CR_FAILED(result))
					read += (size_t)count;
			}
		}
		check(!CR_FAILED(result) && read == total && memcmp(in, out, total) == 0, "1 MiB through short sends and short recvs arrives intact (%s)", cr_result_str(result));
		free(out);
		free(in);
	}

	// A half-close: the peer's recv returns 0 bytes with success.
	expect(crnet_tcp_socket_finish_sending(client), CR_SUCCESS, "finish_sending");
	expect(crnet_tcp_socket_recv(server, received, sizeof(received), PATIENCE_MS, &count), CR_SUCCESS, "the peer's recv after it");
	check(count == 0, "returned 0 bytes: the other side has finished sending");
	expect(crnet_tcp_socket_send(server, "still", 5, PATIENCE_MS, &sent), CR_SUCCESS, "the half-closed side can still be sent to");
	expect(crnet_tcp_socket_recv(client, received, sizeof(received), PATIENCE_MS, &count), CR_SUCCESS, "and still receives");

	// An abort from another thread ends a recv that would wait forever.
	{
		struct abort_later later  = {200, client};
		test_thread        thread = thread_start(abort_socket_later, &later);
		before                    = now_ms();
		CR_RESULT result         = crnet_tcp_socket_recv(client, received, sizeof(received), CRNET_WAIT_FOREVER, &count);
		waited                    = now_ms() - before;
		thread_join(thread);
		expect(result, CR_RESULT_NET_ABORTED, "a recv waiting forever, aborted from another thread");
		check(waited < 2000, "it came back promptly (%llu ms)", (unsigned long long)waited);
		expect(crnet_tcp_socket_recv(client, received, sizeof(received), CRNET_NO_WAIT, &count), CR_RESULT_NET_ABORTED, "an abort doesn't wear off");
	}
	crnet_tcp_socket_destroy(client);
	crnet_tcp_socket_destroy(server);
	crnet_tcp_listener_destroy(listener);

	// The port is free at once, with its old connections barely cold.
	listen_desc.port = port;
	expect(crnet_tcp_listener_create(g_instance, &listen_desc, &listener, &error), CR_SUCCESS, "listening again on the port straight after its listener was destroyed");
	crnet_tcp_listener_destroy(listener);

	// Nobody listening: a refusal, with the system's reason behind it.
	{
		crnet_tcp_socket refused = NULL;
		crnet_tcp_socket_create(g_instance, &refused);
		expect(crnet_tcp_socket_connect(refused, &connect_desc), CR_RESULT_NET_CONNECT_FAILED, "connect to a port nobody listens on");
		crnet_tcp_socket_get_error(refused, CRNET_ERROR_SIDE_RECEIVING, &error);
		check(error.result == CR_RESULT_NET_CONNECT_FAILED && error.source != CR_PLATFORM_ERROR_SOURCE_NONE, "get_error has the system's code for it");
		note_error("connection refused", &error);
		crnet_tcp_socket_destroy(refused);
	}
	{
		crnet_tcp_socket bad = NULL;
		crnet_tcp_socket_create(g_instance, &bad);
		connect_desc.address = "localhost";
		expect(crnet_tcp_socket_connect(bad, &connect_desc), CR_RESULT_NET_INVALID_ADDRESS, "connect to a name rather than a numeric address");
		// Refused, begun with CRNET_NO_WAIT: the refusal comes from the
		// connect or from its continue, whichever finds it.
		connect_desc.address    = "127.0.0.1";
		connect_desc.timeout_ms = CRNET_NO_WAIT;
		CR_RESULT begun        = crnet_tcp_socket_connect(bad, &connect_desc);
		check(begun == CR_RESULT_NET_TIMED_OUT || begun == CR_RESULT_NET_CONNECT_FAILED, "connect with CRNET_NO_WAIT begins it: %s", cr_result_str(begun));
		if (begun == CR_RESULT_NET_TIMED_OUT)
			begun = crnet_tcp_socket_connect_continue(bad, PATIENCE_MS);
		expect(begun, CR_RESULT_NET_CONNECT_FAILED, "and the refusal is reported");
		crnet_tcp_socket_destroy(bad);
	}

	// IPv6, where the machine has it.
	listen_desc.address = "::1";
	listen_desc.port    = 0;
	if (crnet_tcp_listener_create(g_instance, &listen_desc, &listener, &error) == CR_SUCCESS) {
		crnet_tcp_listener_get_port(listener, &port);
		crnet_tcp_socket six = NULL;
		crnet_tcp_socket_create(g_instance, &six);
		connect_desc.address    = "::1";
		connect_desc.port       = port;
		connect_desc.timeout_ms = PATIENCE_MS;
		expect(crnet_tcp_socket_connect(six, &connect_desc), CR_SUCCESS, "listen and connect over IPv6 (::1)");
		crnet_tcp_socket_destroy(six);
		crnet_tcp_listener_destroy(listener);
	} else {
		note("no IPv6 loopback here; skipped");
	}
}

// --------------------------------------------------------------------------
// Records: listener abort and error records, local addresses, debug names
// --------------------------------------------------------------------------

struct abort_listener_later {
	uint32_t             delay_ms;
	crnet_tcp_listener listener;
};

static test_thread_result TEST_THREAD_CALL abort_listener_later_run(void *argument) {
	struct abort_listener_later *later = (struct abort_listener_later *)argument;
	sleep_ms(later->delay_ms);
	crnet_tcp_listener_abort(later->listener);
	return 0;
}

static bool error_is_empty(const crnet_error *error) {
	return error->result == CR_SUCCESS && error->source == CR_PLATFORM_ERROR_SOURCE_NONE && error->code == 0 && error->text[0] == '\0';
}

static void test_records(void) {
	part("Listener abort, error records, local addresses, debug names");
	crnet_error error;

#if CRNET_EXT_TCP
	crnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address   = "127.0.0.1";
	listen_desc.port      = 0;
	listen_desc.backlog   = 8;
	crnet_tcp_listener listener = NULL;
	if (expect(crnet_tcp_listener_create(g_instance, &listen_desc, &listener, &error), CR_SUCCESS, "a listener to abort")) {
		memset(&error, 0xA5, sizeof(error));
		crnet_tcp_listener_get_error(listener, &error);
		check(error_is_empty(&error), "a listener that hasn't failed has an empty error record");

		struct abort_listener_later later = {200, listener};
		test_thread thread                = thread_start(abort_listener_later_run, &later);
		const uint64_t started            = now_ms();
		crnet_tcp_socket accepted       = NULL;
		expect(crnet_tcp_listener_accept(listener, CRNET_WAIT_FOREVER, &accepted), CR_RESULT_NET_ABORTED,
		    "an accept waiting forever, aborted from another thread");
		const uint64_t took = now_ms() - started;
		check(took < 5000, "it came back promptly (%llu ms)", (unsigned long long)took);
		check(accepted == NULL, "and gave no socket");
		thread_join(thread);
		expect(crnet_tcp_listener_accept(listener, CRNET_NO_WAIT, &accepted), CR_RESULT_NET_ABORTED,
		    "every accept after the abort is aborted too");
		crnet_tcp_listener_destroy(listener);
	}
	crnet_tcp_listener_abort(NULL);
	memset(&error, 0xA5, sizeof(error));
	crnet_tcp_listener_get_error(NULL, &error);
	check(error_is_empty(&error), "a NULL listener's error record is empty");

	char addresses[8][CRNET_MAX_IPV4_STRING_LEN];
	uint32_t count = 99;
	expect(crnet_instance_get_local_ipv4_addresses(NULL, addresses, 8, &count, &error), CR_RESULT_NET_INVALID_INSTANCE, "local addresses with no instance");
	expect(crnet_instance_get_local_ipv4_addresses(g_instance, NULL, 8, &count, &error), CR_RESULT_NULL_OUTPUT_PARAMETER, "local addresses with nowhere to put them");
	expect(crnet_instance_get_local_ipv4_addresses(g_instance, addresses, 8, NULL, &error), CR_RESULT_NULL_OUTPUT_PARAMETER, "local addresses with nowhere to count them");
	if (expect(crnet_instance_get_local_ipv4_addresses(g_instance, addresses, 8, &count, NULL), CR_SUCCESS, "this machine's IPv4 addresses")) {
		check(count <= 8, "no more than there was room for (%u)", (unsigned)count);
		bool well_formed = true;
		for (uint32_t i = 0; i < count && i < 8; ++i) {
			const size_t length = strnlen(addresses[i], CRNET_MAX_IPV4_STRING_LEN);
			if (length == 0 || length == CRNET_MAX_IPV4_STRING_LEN || !strchr(addresses[i], '.'))
				well_formed = false;
			else
				note("local address %s", addresses[i]);
		}
		check(well_formed, "each a terminated dotted address");
	}
	count = 99;
	expect(crnet_instance_get_local_ipv4_addresses(g_instance, addresses, 0, &count, NULL), CR_SUCCESS, "local addresses with room for none");
	check(count == 0, "write none (%u)", (unsigned)count);
#endif

	crnet_wait_set set = NULL;
	if (expect(crnet_wait_set_create(g_instance, &set, &error), CR_SUCCESS, "a wait set")) {
		memset(&error, 0xA5, sizeof(error));
		crnet_wait_set_get_error(set, &error);
		check(error_is_empty(&error), "a wait set that hasn't failed has an empty error record");
		crnet_wait_set_destroy(set);
	}
	memset(&error, 0xA5, sizeof(error));
	crnet_wait_set_get_error(NULL, &error);
	check(error_is_empty(&error), "a NULL wait set's error record is empty");

	// A name is copied: the caller's string can go at once.
	char name[32];
	strcpy(name, "records instance");
	expect(cr_debug_name_set(g_instance, name), CR_SUCCESS, "a name for the instance");
	const char *got = cr_debug_name_get(g_instance);
	check(got && strcmp(got, "records instance") == 0, "reads back as set");
	check(got != name, "from Carrottus Net's own copy");
	strcpy(name, "changed");
	check(got && strcmp(cr_debug_name_get(g_instance), "records instance") == 0, "unchanged by the caller's string changing");
	expect(cr_debug_name_set(g_instance, NULL), CR_SUCCESS, "the name removed");
	check(cr_debug_name_get(g_instance) == NULL, "reads back as none");
	check(cr_debug_name_get(NULL) == NULL, "a NULL object has no name");
}

// --------------------------------------------------------------------------
// Wait set
// --------------------------------------------------------------------------

static test_thread_result TEST_THREAD_CALL abort_wait_set_later(void *argument) {
	sleep_ms(200);
	crnet_wait_set_abort((crnet_wait_set)argument);
	return 0;
}

static test_thread_result TEST_THREAD_CALL wake_wait_set_later(void *argument) {
	sleep_ms(200);
	crnet_wait_set_wake((crnet_wait_set)argument);
	return 0;
}

static void test_wait_set(void) {
	part("Wait set");

	crnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address = "127.0.0.1";
	listen_desc.backlog = 8;
	crnet_tcp_listener listener = NULL;
	crnet_error        error;
	if (CR_FAILED(crnet_tcp_listener_create(g_instance, &listen_desc, &listener, &error))) {
		check(false, "listener for the wait set");
		return;
	}
	uint16_t port = 0;
	crnet_tcp_listener_get_port(listener, &port);

	crnet_wait_set set = NULL;
	if (!expect(crnet_wait_set_create(g_instance, &set, &error), CR_SUCCESS, "wait set create")) {
		crnet_tcp_listener_destroy(listener);
		return;
	}
	int listener_tag = 0, first_tag = 1, second_tag = 2;
	expect(crnet_wait_set_add_tcp_listener(set, listener, &listener_tag), CR_SUCCESS, "add the listener");
	expect(crnet_wait_set_add_tcp_listener(set, listener, &listener_tag), CR_RESULT_NET_ALREADY_IN_WAIT_SET, "add it a second time");

	crnet_wait_event events[8];
	uint32_t           count = 0;
	expect(crnet_wait_set_wait(set, CRNET_NO_WAIT, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "wait with CRNET_NO_WAIT and nothing happening");
	uint64_t before = now_ms();
	expect(crnet_wait_set_wait(set, 200, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "wait with a 200 ms limit and nothing happening");
	uint64_t waited = now_ms() - before;
	check(waited >= 150 && waited < 1500, "that wait took about its limit (%llu ms)", (unsigned long long)waited);

	// Two connections in. The listener is named for each.
	crnet_tcp_socket_connect_desc connect_desc;
	memset(&connect_desc, 0, sizeof(connect_desc));
	connect_desc.base.type = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
	connect_desc.address    = "127.0.0.1";
	connect_desc.port       = port;
	connect_desc.timeout_ms = PATIENCE_MS;
	crnet_tcp_socket clients[2] = {NULL, NULL};
	crnet_tcp_socket servers[2] = {NULL, NULL};
	for (int i = 0; i < 2; ++i) {
		crnet_tcp_socket_create(g_instance, &clients[i]);
		crnet_tcp_socket_connect(clients[i], &connect_desc);
		CR_RESULT result = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
		check(!CR_FAILED(result) && count == 1 && events[0].user == &listener_tag && events[0].ready == CRNET_WAIT_FOR_INCOMING, "connection %d: the set names the listener", i + 1);
		expect(crnet_tcp_listener_accept(listener, CRNET_NO_WAIT, &servers[i]), CR_SUCCESS, "and accept with CRNET_NO_WAIT has it");
	}
	if (!servers[0] || !servers[1]) {
		crnet_wait_set_destroy(set);
		crnet_tcp_listener_destroy(listener);
		return;
	}
	expect(crnet_wait_set_add_tcp_socket(set, servers[0], 0, &first_tag), CR_RESULT_DESC_INVALID_PARAMETERS, "add a socket asking about nothing");
	expect(crnet_wait_set_add_tcp_socket(set, servers[0], CRNET_WAIT_FOR_INCOMING, &first_tag), CR_SUCCESS, "add the first accepted socket");
	expect(crnet_wait_set_add_tcp_socket(set, servers[1], CRNET_WAIT_FOR_INCOMING, &second_tag), CR_SUCCESS, "add the second");
	expect(crnet_wait_set_wait(set, 100, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "nothing sent: nothing named");

	// Only the socket that was sent to is named, and draining it quiets it.
	uint64_t sent = 0;
	crnet_tcp_socket_send(clients[1], "second", 6, PATIENCE_MS, &sent);
	CR_RESULT result = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
	check(!CR_FAILED(result) && count == 1 && events[0].user == &second_tag, "a send to the second connection: the set names the second and only it");
	uint8_t  buffer[32];
	uint64_t received = 0;
	crnet_tcp_socket_recv(servers[1], buffer, sizeof(buffer), CRNET_NO_WAIT, &received);
	check(received == 6, "its data is there for a recv with CRNET_NO_WAIT");
	expect(crnet_tcp_socket_recv(servers[1], buffer, sizeof(buffer), CRNET_NO_WAIT, &received), CR_RESULT_NET_TIMED_OUT, "drained");
	expect(crnet_wait_set_wait(set, 100, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "and the set is quiet again");

	// Both at once.
	crnet_tcp_socket_send(clients[0], "a", 1, PATIENCE_MS, &sent);
	crnet_tcp_socket_send(clients[1], "b", 1, PATIENCE_MS, &sent);
	sleep_ms(100);
	result = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
	check(!CR_FAILED(result) && count == 2, "a send to each: the set names both in one wait (%u named)", count);
	result = crnet_wait_set_wait(set, PATIENCE_MS, events, 1, &count);
	check(!CR_FAILED(result) && count == 1, "with room for one event it gives one");
	crnet_tcp_socket_recv(servers[0], buffer, sizeof(buffer), CRNET_NO_WAIT, &received);
	crnet_tcp_socket_recv(servers[1], buffer, sizeof(buffer), CRNET_NO_WAIT, &received);

	// A peer that goes away is something to be told about.
	crnet_tcp_socket_destroy(clients[0]);
	clients[0] = NULL;
	result     = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
	check(!CR_FAILED(result) && count == 1 && events[0].user == &first_tag, "a peer closing: the set names its connection");
	expect(crnet_tcp_socket_recv(servers[0], buffer, sizeof(buffer), CRNET_NO_WAIT, &received), CR_SUCCESS, "whose recv then reports the close");
	check(received == 0, "as 0 bytes");
	crnet_wait_set_remove_tcp_socket(set, servers[0]);
	expect(crnet_wait_set_wait(set, 100, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "removed, it is no longer named");

	// Room to send: asked about on a connection with room, it is always there.
	crnet_wait_set_remove_tcp_socket(set, servers[1]);
	expect(crnet_wait_set_add_tcp_socket(set, servers[1], CRNET_WAIT_FOR_ROOM, &second_tag), CR_SUCCESS, "add again, asking about room");
	result = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
	check(!CR_FAILED(result) && count == 1 && events[0].ready == CRNET_WAIT_FOR_ROOM, "an idle connection has room at once");

	// A wake ends one wait and leaves the set as it was.
	crnet_wait_set_remove_tcp_socket(set, servers[1]);
	crnet_wait_set_wake(set);
	result = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
	check(result == CR_SUCCESS && count == 0, "a wake made beforehand ends the next wait, with nothing named");
	expect(crnet_wait_set_wait(set, 100, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "and only that one");
	test_thread waker = thread_start(wake_wait_set_later, set);
	before            = now_ms();
	result            = crnet_wait_set_wait(set, CRNET_WAIT_FOREVER, events, 8, &count);
	waited            = now_ms() - before;
	thread_join(waker);
	check(result == CR_SUCCESS && count == 0, "a wait with no limit, woken from another thread");
	check(waited < 2000, "it came back promptly (%llu ms)", (unsigned long long)waited);
	expect(crnet_wait_set_wait(set, 100, events, 8, &count), CR_RESULT_NET_TIMED_OUT, "and the set waits again afterwards");

	// Abort from another thread.
	test_thread thread = thread_start(abort_wait_set_later, set);
	before             = now_ms();
	result             = crnet_wait_set_wait(set, CRNET_WAIT_FOREVER, events, 8, &count);
	waited             = now_ms() - before;
	thread_join(thread);
	expect(result, CR_RESULT_NET_ABORTED, "a wait with no limit, aborted from another thread");
	check(waited < 2000, "it came back promptly (%llu ms)", (unsigned long long)waited);

	crnet_wait_set_destroy(set);
	for (int i = 0; i < 2; ++i) {
		crnet_tcp_socket_destroy(clients[i]);
		crnet_tcp_socket_destroy(servers[i]);
	}
	crnet_tcp_listener_destroy(listener);
}

// --------------------------------------------------------------------------
// Resolver
// --------------------------------------------------------------------------

static test_thread_result TEST_THREAD_CALL run_resolver(void *argument) {
	crnet_resolver_run((crnet_resolver)argument);
	return 0;
}

static void test_resolver(void) {
	part("Resolver");

	char                  addresses[8][CRNET_MAX_ADDRESS_STRING_LEN];
	uint32_t              count = 0;
	crnet_resolver_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
	crnet_resolver resolver = NULL;

	// Run and wait on one thread: an ordinary blocking lookup.
	desc.host   = "localhost";
	desc.family = CRNET_ADDRESS_FAMILY_IPV4;
	expect(crnet_resolver_create(g_instance, &desc, &resolver), CR_SUCCESS, "create for \"localhost\", IPv4");
	expect(crnet_resolver_wait(resolver, CRNET_NO_WAIT, addresses, 8, &count), CR_RESULT_NET_TIMED_OUT, "wait before it has been run");
	expect(crnet_resolver_run(resolver), CR_SUCCESS, "run");
	expect(crnet_resolver_wait(resolver, CRNET_NO_WAIT, addresses, 8, &count), CR_SUCCESS, "wait after it");
	check(count >= 1 && strcmp(addresses[0], "127.0.0.1") == 0, "localhost is 127.0.0.1 (%u address%s, first %s)", count, count == 1 ? "" : "es", count ? addresses[0] : "-");
	expect(crnet_resolver_wait(resolver, CRNET_NO_WAIT, addresses, 8, &count), CR_SUCCESS, "the answer is there for a second wait");
	crnet_resolver_destroy(resolver);

	// The two-thread use: run on a thread of the caller's.
	desc.family = CRNET_ADDRESS_FAMILY_ANY;
	crnet_resolver_create(g_instance, &desc, &resolver);
	test_thread thread = thread_start(run_resolver, resolver);
	expect(crnet_resolver_wait(resolver, PATIENCE_MS, addresses, 8, &count), CR_SUCCESS, "run on another thread, wait on this one");
	check(count >= 1, "it has %u address%s", count, count == 1 ? "" : "es");
	crnet_resolver_destroy(resolver);
	thread_join(thread);

	// A numeric address comes back as itself.
	desc.host = "192.0.2.7";
	crnet_resolver_create(g_instance, &desc, &resolver);
	crnet_resolver_run(resolver);
	expect(crnet_resolver_wait(resolver, CRNET_NO_WAIT, addresses, 8, &count), CR_SUCCESS, "a numeric address");
	check(count == 1 && strcmp(addresses[0], "192.0.2.7") == 0, "comes back as itself");
	crnet_resolver_destroy(resolver);

	// A name that has no address. ".invalid" is reserved never to.
	desc.host = "crnet-test.invalid";
	crnet_resolver_create(g_instance, &desc, &resolver);
	expect(crnet_resolver_run(resolver), CR_RESULT_NET_RESOLVE_FAILED, "run for a name that doesn't exist");
	expect(crnet_resolver_wait(resolver, CRNET_NO_WAIT, addresses, 8, &count), CR_RESULT_NET_RESOLVE_FAILED, "and its wait");
	crnet_error error;
	crnet_resolver_get_error(resolver, &error);
	check(error.source != CR_PLATFORM_ERROR_SOURCE_NONE, "get_error has the resolver's reason");
	note_error("no such name", &error);
	expect(crnet_resolver_run(resolver), CR_RESULT_NET_INVALID_RESOLVER, "a second run");
	crnet_resolver_destroy(resolver);

	// The resolver that turns out not to be needed: abort, run, destroy.
	desc.host = "localhost";
	crnet_resolver_create(g_instance, &desc, &resolver);
	crnet_resolver_abort(resolver);
	uint64_t before = now_ms();
	expect(crnet_resolver_run(resolver), CR_RESULT_NET_ABORTED, "run after abort");
	check(now_ms() - before < 500, "returned at once");
	expect(crnet_resolver_wait(resolver, PATIENCE_MS, addresses, 8, &count), CR_RESULT_NET_ABORTED, "and so does its wait");
	crnet_resolver_destroy(resolver);

	// Destroyed before its run has even started: the thread that runs it
	// later must find it still there. (This is the case the object exists
	// for; a crash here is the failure.)
	crnet_resolver_create(g_instance, &desc, &resolver);
	crnet_resolver_destroy(resolver);
	expect(crnet_resolver_run(resolver), CR_SUCCESS, "run on a resolver that was destroyed first");

	desc.host = "";
	expect(crnet_resolver_create(g_instance, &desc, &resolver), CR_RESULT_DESC_INVALID_PARAMETERS, "create with an empty name");
}

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------

struct exchange {
	// In.
	const char           *method;
	const char           *path; // appended to the server's base URL
	const crnet_header *headers;
	uint32_t              header_count;
	CRNET_HTTP_BODY     body;
	const uint8_t        *body_data;
	uint64_t              body_size;
	bool                  follow_redirects;
	bool                  secure;
	const char           *host; // NULL for 127.0.0.1
	// Out.
	uint32_t      status;
	uint8_t      *response;
	size_t        response_size;
	const char   *failed_in; // the call that failed, or NULL
	crnet_error error;
	// The transfer, left for the caller when `keep` is set.
	bool                  keep;
	crnet_http_transfer transfer;
};

static void exchange_free(struct exchange *exchange) {
	free(exchange->response);
	exchange->response = NULL;
	crnet_http_transfer_destroy(exchange->transfer);
	exchange->transfer = NULL;
}

/* One whole request and response on `client`. The result of the call that
 * failed, with `failed_in` saying which, or CR_SUCCESS. */
static CR_RESULT exchange_run(crnet_http_client client, struct exchange *exchange) {
	exchange->status        = 0;
	exchange->response      = NULL;
	exchange->response_size = 0;
	exchange->failed_in     = NULL;
	exchange->transfer      = NULL;
	memset(&exchange->error, 0, sizeof(exchange->error));

	char url[512];
	snprintf(url, sizeof(url), "%s://%s:%d%s", exchange->secure ? "https" : "http", exchange->host ? exchange->host : "127.0.0.1",
	         exchange->secure ? g_server.https_port : g_server.http_port, exchange->path);

	crnet_http_transfer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
	desc.method           = exchange->method;
	desc.url              = url;
	desc.headers          = exchange->headers;
	desc.header_count     = exchange->header_count;
	desc.body             = exchange->body;
	desc.body_size        = exchange->body == CRNET_HTTP_BODY_SIZED ? exchange->body_size : 0;
	desc.follow_redirects = exchange->follow_redirects;
	desc.timeout_ms       = PATIENCE_MS;

	crnet_http_transfer transfer = NULL;
	CR_RESULT            result   = crnet_http_transfer_create(client, &transfer);
	exchange->transfer             = transfer;
	const char *step               = "create";
	if (!CR_FAILED(result)) {
		step   = "start";
		result = crnet_http_transfer_start(transfer, &desc);
	}
	if (!CR_FAILED(result) && exchange->body != CRNET_HTTP_BODY_NONE) {
		step            = "send";
		uint64_t offset = 0;
		while (offset < exchange->body_size && !CR_FAILED(result)) {
			uint64_t sent = 0;
			result        = crnet_http_transfer_send(transfer, exchange->body_data + offset, exchange->body_size - offset, PATIENCE_MS, &sent);
			offset       += sent;
		}
	}
	if (!CR_FAILED(result)) {
		step   = "receive_response";
		result = crnet_http_transfer_receive_response(transfer, PATIENCE_MS);
	}
	if (!CR_FAILED(result)) {
		step             = "recv";
		exchange->status = crnet_http_transfer_get_status(transfer);
		size_t capacity  = 65536;
		exchange->response = (uint8_t *)malloc(capacity + 1);
		for (;;) {
			if (exchange->response_size == capacity) {
				capacity           *= 2;
				exchange->response  = (uint8_t *)realloc(exchange->response, capacity + 1);
			}
			uint64_t received = 0;
			result            = crnet_http_transfer_recv(transfer, exchange->response + exchange->response_size, capacity - exchange->response_size, PATIENCE_MS, &received);
			if (CR_FAILED(result) || received == 0)
				break;
			exchange->response_size += (size_t)received;
		}
		exchange->response[exchange->response_size] = '\0';
	}
	if (CR_FAILED(result)) {
		exchange->failed_in = step;
		crnet_http_transfer_get_error(transfer, &exchange->error);
	}
	if (!exchange->keep) {
		crnet_http_transfer_destroy(transfer);
		exchange->transfer = NULL;
	}
	return result;
}

/* Runs an exchange that is meant to work and says so, or says where it
 * didn't. */
static bool exchange_ok(crnet_http_client client, struct exchange *exchange, const char *what) {
	CR_RESULT result = exchange_run(client, exchange);
	if (CR_FAILED(result)) {
		check(false, "%s: %s failed with %s", what, exchange->failed_in, cr_result_str(result));
		note_error(what, &exchange->error);
		return false;
	}
	check(true, "%s: status %u, %zu bytes of body", what, exchange->status, exchange->response_size);
	return true;
}

static crnet_http_client plain_client(void) {
	crnet_http_client_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode = CRNET_PROXY_NONE; // the test's server is on this machine
	crnet_http_client client = NULL;
	crnet_http_client_create(g_instance, &desc, &client);
	return client;
}

/* The number called `name` in a /stat response, or -1. */
static long stat_value(const char *response, const char *name) {
	char needle[64];
	snprintf(needle, sizeof(needle), "%s=", name);
	const char *found = strstr(response, needle);
	return found ? atol(found + strlen(needle)) : -1;
}

#if CRNET_EXT_TCP
/* A number from the server's /stat, asked over a TCP connection of its own
 * and closed after, so that it is always one connection to the server. An
 * HTTP client won't do: WinHTTP shares its connections between every
 * session in the process, so a new client may ask on one already open. */
static long server_stat(const char *name) {
	crnet_tcp_socket socket = NULL;
	if (CR_FAILED(crnet_tcp_socket_create(g_instance, &socket)))
		return -1;
	crnet_tcp_socket_connect_desc connect_desc;
	memset(&connect_desc, 0, sizeof(connect_desc));
	connect_desc.base.type = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
	connect_desc.address    = "127.0.0.1";
	connect_desc.port       = (uint16_t)g_server.http_port;
	connect_desc.timeout_ms = PATIENCE_MS;
	long value              = -1;
	if (!CR_FAILED(crnet_tcp_socket_connect(socket, &connect_desc))) {
		const char request[] = "GET /stat HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
		size_t     sent      = 0;
		bool       ok        = true;
		while (ok && sent < sizeof(request) - 1) {
			uint64_t count = 0;
			ok             = !CR_FAILED(crnet_tcp_socket_send(socket, request + sent, sizeof(request) - 1 - sent, PATIENCE_MS, &count));
			sent += (size_t)count;
		}
		// The server closes the connection after the response.
		char   response[4096];
		size_t size = 0;
		while (ok && size < sizeof(response) - 1) {
			uint64_t count = 0;
			ok             = !CR_FAILED(crnet_tcp_socket_recv(socket, response + size, sizeof(response) - 1 - size, PATIENCE_MS, &count));
			if (count == 0)
				break;
			size += (size_t)count;
		}
		response[size]   = '\0';
		const char *body = ok ? strstr(response, "\r\n\r\n") : NULL;
		if (body)
			value = stat_value(body, name);
	}
	crnet_tcp_socket_destroy(socket);
	return value;
}
#else
/* A number from the server's /stat, asked on a client of its own. With no
 * TCP to ask over, a stack that shares connections between clients makes
 * this one no more than a connection of its own. */
static long server_stat(const char *name) {
	crnet_http_client client = plain_client();
	struct exchange     stat;
	memset(&stat, 0, sizeof(stat));
	stat.method = "GET";
	stat.path   = "/stat";
	long value  = -1;
	if (!CR_FAILED(exchange_run(client, &stat)) && stat.response)
		value = stat_value((const char *)stat.response, name);
	exchange_free(&stat);
	crnet_http_client_destroy(client);
	return value;
}
#endif

/* Everything here is made, used and destroyed on this thread alone,
 * through a tracker, and checked at the end to have been freed with the
 * size and alignment it was allocated with: an instance, one object of
 * every kind made with it, an accepted socket (its listener's allocator), a
 * transfer (its client's) with a response's headers, and a resolver freed
 * after its instance has gone. */
static void test_instance_memory(void) {
	crtest_allocator    tracker;
	CR_ALLOCATOR        allocator = crtest_allocator_make(&tracker);
	crnet_instance_desc desc      = instance_desc(&allocator);
	crnet_error         error;

	// No memory: refused, with nothing made and nothing kept.
	crnet_instance instance = (crnet_instance)(uintptr_t)1;
	tracker.refuse_after      = 0;
	expect(crnet_instance_create(&desc, &instance, &error), CR_RESULT_OUT_OF_MEMORY, "an instance with no memory to be had");
	check(instance == NULL && tracker.live_blocks == 0, "makes nothing and keeps nothing");
	tracker.refuse_after = SIZE_MAX;

	// The caller's allocator struct need not outlive the call: the instance
	// keeps a copy, so this one is overwritten once it is made.
	CR_ALLOCATOR passed = allocator;
	desc.allocator        = &passed;
	if (!expect(crnet_instance_create(&desc, &instance, &error), CR_SUCCESS, "an instance through the caller's allocator"))
		return;
	memset(&passed, 0, sizeof(passed));
	check(tracker.given > 0, "allocates through it");
	expect(cr_debug_name_set(instance, "tracked instance"), CR_SUCCESS, "a name for the instance");

	// A client that keeps a proxy, roots and a pin (the copies each stack
	// keeps of them), made and destroyed without a transfer.
	{
		crnet_tls_certificate root;
		root.data = g_server.cert_der;
		root.size = g_server.cert_der_size;
		crnet_tls_pin pin;
		memcpy(pin.sha256, g_server.pin, sizeof(pin.sha256));
		char proxy_url[64];
		snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%d", g_server.proxy_port);

		crnet_http_client_desc client_desc;
		memset(&client_desc, 0, sizeof(client_desc));
		client_desc.base.type      = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
		client_desc.tls.trust      = CRNET_TLS_TRUST_ROOTS;
		client_desc.tls.roots      = &root;
		client_desc.tls.root_count = 1;
		client_desc.tls.pins       = &pin;
		client_desc.tls.pin_count  = 1;
		client_desc.proxy.mode     = CRNET_PROXY_EXPLICIT;
		client_desc.proxy.url      = proxy_url;
		crnet_http_client client = NULL;
		expect(crnet_http_client_create(instance, &client_desc, &client), CR_SUCCESS, "tracked: an HTTP client with a proxy, a root and a pin");
		crnet_http_client_destroy(client);
	}

	// A whole exchange: the response's headers, a repeated one joined, are
	// the transfer's, through its client's allocator.
	{
		crnet_http_client_desc client_desc;
		memset(&client_desc, 0, sizeof(client_desc));
		client_desc.base.type      = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
		client_desc.proxy.mode     = CRNET_PROXY_NONE;
		crnet_http_client client = NULL;
		if (expect(crnet_http_client_create(instance, &client_desc, &client), CR_SUCCESS, "tracked: a plain HTTP client")) {
			expect(cr_debug_name_set(client, "tracked client"), CR_SUCCESS, "a name for the client");
			struct exchange x;
			memset(&x, 0, sizeof(x));
			x.method = "GET";
			x.path   = "/hello";
			x.keep   = true;
			if (exchange_ok(client, &x, "tracked: GET /hello")) {
				const char *multi = crnet_http_transfer_get_header(x.transfer, "x-multi");
				check(multi && strcmp(multi, "a, b") == 0, "tracked: the joined header (\"%s\")", multi ? multi : "(none)");
				expect(cr_debug_name_set(x.transfer, "tracked transfer"), CR_SUCCESS, "a name for the transfer");
			}
			exchange_free(&x);
			crnet_http_client_destroy(client);
		}
	}

#if CRNET_EXT_WEBSOCKET
	{
		crnet_websocket socket = NULL;
		if (expect(crnet_websocket_create(instance, &socket), CR_SUCCESS, "tracked: a WebSocket")) {
			expect(cr_debug_name_set(socket, "tracked WebSocket"), CR_SUCCESS, "a name for the WebSocket");
			crnet_websocket_destroy(socket);
		}
	}
#endif

#if CRNET_EXT_TCP
	// A listener, a socket connected to it and the one it accepts, and a
	// wait set holding them. Connecting on loopback finishes without the
	// accept, so all of it is on this thread.
	{
		crnet_tcp_listener      listener = NULL;
		crnet_tcp_listener_desc listen_desc;
		memset(&listen_desc, 0, sizeof(listen_desc));
		listen_desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
		listen_desc.address   = "127.0.0.1";
		listen_desc.port      = 0;
		listen_desc.backlog   = 4;
		uint16_t           port     = 0;
		crnet_tcp_socket client   = NULL;
		crnet_tcp_socket accepted = NULL;
		crnet_wait_set   set      = NULL;
		if (expect(crnet_tcp_listener_create(instance, &listen_desc, &listener, &error), CR_SUCCESS, "tracked: a listener") &&
		    expect(crnet_tcp_listener_get_port(listener, &port), CR_SUCCESS, "on a port of its own") &&
		    expect(crnet_tcp_socket_create(instance, &client), CR_SUCCESS, "tracked: a socket")) {
			expect(cr_debug_name_set(listener, "tracked listener"), CR_SUCCESS, "a name for the listener");
			crnet_tcp_socket_connect_desc connect_desc;
			memset(&connect_desc, 0, sizeof(connect_desc));
			connect_desc.base.type  = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
			connect_desc.address    = "127.0.0.1";
			connect_desc.port       = port;
			connect_desc.timeout_ms = PATIENCE_MS;
			if (expect(crnet_tcp_socket_connect(client, &connect_desc), CR_SUCCESS, "tracked: connected to the listener") &&
			    expect(crnet_tcp_listener_accept(listener, PATIENCE_MS, &accepted), CR_SUCCESS, "tracked: and accepted")) {
				expect(cr_debug_name_set(accepted, "tracked accepted socket"), CR_SUCCESS, "a name for the accepted socket");
				int tag = 0;
				if (expect(crnet_wait_set_create(instance, &set, &error), CR_SUCCESS, "tracked: a wait set")) {
					expect(crnet_wait_set_add_tcp_listener(set, listener, &tag), CR_SUCCESS, "holding the listener");
					expect(crnet_wait_set_add_tcp_socket(set, accepted, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "and the accepted socket");
					// Something for the accepted socket to read, so the wait
					// has a member to name.
					uint64_t sent = 0;
					expect(crnet_tcp_socket_send(client, "x", 1, PATIENCE_MS, &sent), CR_SUCCESS, "tracked: a byte sent");
					crnet_wait_event events[4];
					uint32_t           count = 0;
					expect(crnet_wait_set_wait(set, PATIENCE_MS, events, 4, &count), CR_SUCCESS, "tracked: a wait");
					check(count >= 1, "names the accepted socket (%u named)", count);
				}
			}
		}
		crnet_wait_set_destroy(set);
		crnet_tcp_socket_destroy(accepted);
		crnet_tcp_socket_destroy(client);
		crnet_tcp_listener_destroy(listener);
	}

	// A resolver outlasting its instance: destroyed unrun, the instance
	// destroyed, and only then run, which is what frees it - through the
	// copy of the allocator it kept, the instance's being gone.
	crnet_resolver      resolver = NULL;
	crnet_resolver_desc resolver_desc;
	memset(&resolver_desc, 0, sizeof(resolver_desc));
	resolver_desc.base.type = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
	resolver_desc.host      = "localhost";
	resolver_desc.family    = CRNET_ADDRESS_FAMILY_IPV4;
	bool made_resolver      = expect(crnet_resolver_create(instance, &resolver_desc, &resolver), CR_SUCCESS, "tracked: a resolver");
	if (made_resolver) {
		expect(cr_debug_name_set(resolver, "tracked resolver"), CR_SUCCESS, "a name for the resolver");
		crnet_resolver_abort(resolver);
		crnet_resolver_destroy(resolver);
	}
	crnet_instance_destroy(instance);
	if (made_resolver) {
		check(tracker.live_blocks > 0, "the resolver is still held after its instance has gone");
		expect(crnet_resolver_run(resolver), CR_RESULT_NET_ABORTED, "tracked: run after the instance has gone");
	}
#else
	crnet_instance_destroy(instance);
#endif

	check(crtest_allocator_clean(&tracker), "everything tracked was freed with the size and alignment it was allocated with (%zu allocations)", tracker.given);
}

struct abort_transfer_later {
	uint32_t              delay_ms;
	crnet_http_transfer transfer;
};

static test_thread_result TEST_THREAD_CALL abort_transfer_later_run(void *argument) {
	struct abort_transfer_later *later = (struct abort_transfer_later *)argument;
	sleep_ms(later->delay_ms);
	crnet_http_transfer_abort(later->transfer);
	return 0;
}

static void test_http(void) {
	part("HTTP transfers");

	crnet_http_client client = plain_client();
	if (!client) {
		check(false, "http client create");
		return;
	}
	struct exchange x;

	// The plain case, and the response's headers.
	memset(&x, 0, sizeof(x));
	x.method = "GET";
	x.path   = "/hello";
	x.keep   = true;
	if (exchange_ok(client, &x, "GET /hello")) {
		check(x.status == 200 && x.response_size == 5 && memcmp(x.response, "hello", 5) == 0, "200 and the body \"hello\"");
		const char *multi = crnet_http_transfer_get_header(x.transfer, "x-multi");
		check(multi && strcmp(multi, "a, b") == 0, "a header sent on two lines is one value joined with \", \" (\"%s\")", multi ? multi : "(none)");
		check(crnet_http_transfer_get_header(x.transfer, "CONTENT-TYPE") != NULL, "header names compare without case");
		check(crnet_http_transfer_get_header(x.transfer, "X-Absent") == NULL, "a header the response doesn't have is NULL");
		uint32_t    count = crnet_http_transfer_get_header_count(x.transfer);
		uint32_t    multis = 0;
		const char *name, *value;
		for (uint32_t i = 0; i < count; ++i) {
			crnet_http_transfer_get_header_at(x.transfer, i, &name, &value);
			if (name && (strcmp(name, "X-Multi") == 0 || strcmp(name, "x-multi") == 0))
				multis++;
		}
		check(count >= 3 && multis == 1, "the indexed form lists each name once (%u headers, X-Multi %u time%s)", count, multis, multis == 1 ? "" : "s");
		expect(crnet_http_transfer_get_header_at(x.transfer, count, &name, &value), CR_RESULT_INDEX_OUT_OF_RANGE, "an index past the end");
		uint8_t  byte;
		uint64_t more = 99;
		expect(crnet_http_transfer_recv(x.transfer, &byte, 1, PATIENCE_MS, &more), CR_SUCCESS, "recv after the body's end");
		check(more == 0, "says the end again");
		expect(crnet_http_transfer_receive_response(x.transfer, PATIENCE_MS), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "receive_response out of order");
	}
	exchange_free(&x);

	// What each stack adds to a request, for the record.
	memset(&x, 0, sizeof(x));
	x.method = "GET";
	x.path   = "/headers";
	if (exchange_ok(client, &x, "GET /headers (what the request carried)")) {
		char *line = strtok((char *)x.response, "\n");
		while (line) {
			note("request header the server saw: %s", line);
			line = strtok(NULL, "\n");
		}
	}
	exchange_free(&x);

	// Connection reuse: two transfers read to their ends, one connection.
	{
		long before = server_stat("connections");
		for (int i = 0; i < 3; ++i) {
			memset(&x, 0, sizeof(x));
			x.method = "GET";
			x.path   = "/hello";
			exchange_run(client, &x);
			exchange_free(&x);
		}
		long after = server_stat("connections");
		// Each server_stat is a connection of its own; the three transfers
		// should have added none, the client already holding one.
		check(after - before == 1, "three more transfers on the client made no new connection (%ld opened, 1 of them the question itself)", after - before);
	}

	// HEAD, and a response with no body.
	memset(&x, 0, sizeof(x));
	x.method = "HEAD";
	x.path   = "/hello";
	x.keep   = true;
	if (exchange_ok(client, &x, "HEAD /hello")) {
		const char *length = crnet_http_transfer_get_header(x.transfer, "Content-Length");
		check(x.status == 200 && x.response_size == 0, "200 and no body");
		check(length && strcmp(length, "5") == 0, "though its Content-Length says 5");
	}
	exchange_free(&x);
	memset(&x, 0, sizeof(x));
	x.method = "GET";
	x.path   = "/empty";
	if (exchange_ok(client, &x, "GET /empty"))
		check(x.status == 204 && x.response_size == 0, "204 and no body");
	exchange_free(&x);

	// Request bodies.
	size_t   upload_size = 3 * 1024 * 1024 + 123;
	uint8_t *upload      = (uint8_t *)malloc(upload_size);
	for (size_t i = 0; i < upload_size; ++i)
		upload[i] = pattern_at(i);

	memset(&x, 0, sizeof(x));
	x.method    = "POST";
	x.path      = "/echo";
	x.body      = CRNET_HTTP_BODY_SIZED;
	x.body_data = upload;
	x.body_size = upload_size;
	if (exchange_ok(client, &x, "POST /echo, a sized body of 3 MiB"))
		check(x.status == 200 && x.response_size == upload_size && memcmp(x.response, upload, upload_size) == 0, "came back intact");
	exchange_free(&x);

	memset(&x, 0, sizeof(x));
	x.method    = "PUT";
	x.path      = "/echo";
	x.body      = CRNET_HTTP_BODY_UNSIZED;
	x.body_data = upload;
	x.body_size = upload_size;
	if (exchange_ok(client, &x, "PUT /echo, an unsized body of 3 MiB"))
		check(x.status == 200 && x.response_size == upload_size && memcmp(x.response, upload, upload_size) == 0, "came back intact");
	exchange_free(&x);

	// How each kind of body is announced.
	{
		static const struct {
			const char       *method;
			CRNET_HTTP_BODY body;
			const char       *what;
		} kinds[] = {
		    {"GET", CRNET_HTTP_BODY_NONE, "GET with no body"},
		    {"POST", CRNET_HTTP_BODY_NONE, "POST with no body"},
		    {"POST", CRNET_HTTP_BODY_SIZED, "POST with an empty body"},
		    {"DELETE", CRNET_HTTP_BODY_SIZED, "DELETE with an empty body"},
		    {"POST", CRNET_HTTP_BODY_UNSIZED, "POST with an unsized body"},
		};
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
			memset(&x, 0, sizeof(x));
			x.method    = kinds[i].method;
			x.path      = "/headers";
			x.body      = kinds[i].body;
			x.body_data = (const uint8_t *)"";
			x.keep      = true;
			if (!CR_FAILED(exchange_run(client, &x))) {
				bool        has_length  = strstr((const char *)x.response, "Content-Length: 0") != NULL;
				bool        any_length  = strstr((const char *)x.response, "Content-Length:") != NULL;
				bool        is_chunked  = strstr((const char *)x.response, "chunked") != NULL;
				const char *seen_method = crnet_http_transfer_get_header(x.transfer, "X-Method");
				check(seen_method && strcmp(seen_method, kinds[i].method) == 0, "%s: the server saw the method as given", kinds[i].what);
				if (kinds[i].body == CRNET_HTTP_BODY_SIZED)
					check(has_length, "%s: announced as Content-Length: 0", kinds[i].what);
				else if (kinds[i].body == CRNET_HTTP_BODY_UNSIZED)
					check(is_chunked && !any_length, "%s: sent chunked, with no Content-Length", kinds[i].what);
				else
					note("%s: the stack %s", kinds[i].what, any_length ? "added a Content-Length of its own" : "sent no Content-Length");
			} else {
				check(false, "%s: %s failed", kinds[i].what, x.failed_in);
				note_error(kinds[i].what, &x.error);
			}
			exchange_free(&x);
		}
	}

	// Redirects.
	memset(&x, 0, sizeof(x));
	x.method = "GET";
	x.path   = "/redirect";
	x.keep   = true;
	if (exchange_ok(client, &x, "GET /redirect, not following")) {
		const char *location = crnet_http_transfer_get_header(x.transfer, "Location");
		check(x.status == 302 && location && strcmp(location, "/hello") == 0, "the 302 is the response, with its Location");
	}
	exchange_free(&x);
	memset(&x, 0, sizeof(x));
	x.method           = "GET";
	x.path             = "/redirect";
	x.follow_redirects = true;
	x.keep             = true;
	if (exchange_ok(client, &x, "GET /redirect, following")) {
		check(x.status == 200 && x.response_size == 5, "the response is the one it led to");
		check(crnet_http_transfer_get_header(x.transfer, "Location") == NULL && crnet_http_transfer_get_header(x.transfer, "X-Multi") != NULL,
		      "and the headers are that response's only");
	}
	exchange_free(&x);

	// Decoding, and declining it.
	memset(&x, 0, sizeof(x));
	x.method = "GET";
	x.path   = "/gzip";
	x.keep   = true;
	if (exchange_ok(client, &x, "GET /gzip")) {
		bool intact = x.response_size == 20000;
		for (size_t i = 0; intact && i < x.response_size; ++i)
			intact = x.response[i] == pattern_at(i);
		const char *asked = crnet_http_transfer_get_header(x.transfer, "X-Got-Accept-Encoding");
		check(intact, "a compressed body arrives decoded");
		note("the stack's own Accept-Encoding was \"%s\"", asked ? asked : "(none)");
	}
	exchange_free(&x);
	{
		crnet_header identity = {"Accept-Encoding", "identity"};
		memset(&x, 0, sizeof(x));
		x.method       = "GET";
		x.path         = "/gzip";
		x.headers      = &identity;
		x.header_count = 1;
		x.keep         = true;
		if (exchange_ok(client, &x, "GET /gzip with Accept-Encoding: identity")) {
			const char *asked    = crnet_http_transfer_get_header(x.transfer, "X-Got-Accept-Encoding");
			const char *encoding = crnet_http_transfer_get_header(x.transfer, "Content-Encoding");
			check(asked && strcmp(asked, "identity") == 0, "the server was sent the caller's header and no other (\"%s\")", asked ? asked : "(none)");
			check(!encoding && x.response_size == 20000, "and sent the body as it is");
		}
		exchange_free(&x);
	}

	// What start refuses.
	{
		crnet_http_transfer      transfer = NULL;
		crnet_http_transfer_desc desc;
		char                       url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", g_server.http_port);
		crnet_header gzip     = {"Accept-Encoding", "gzip"};
		crnet_header length   = {"content-length", "5"};
		uint64_t       sent     = 0;
		uint8_t        byte     = 0;
		uint64_t       received = 0;

		crnet_http_transfer_create(client, &transfer);
		expect(crnet_http_transfer_send(transfer, "x", 1, PATIENCE_MS, &sent), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "send before start");
		expect(crnet_http_transfer_recv(transfer, &byte, 1, PATIENCE_MS, &received), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "recv before start");
		check(crnet_http_transfer_get_status(transfer) == 0 && crnet_http_transfer_get_header_count(transfer) == 0, "no status and no headers before a response");

		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
		desc.method     = "GET";
		desc.url        = url;
		desc.timeout_ms = PATIENCE_MS;
		desc.headers      = &gzip;
		desc.header_count = 1;
		expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_DESC_INVALID_PARAMETERS, "start with Accept-Encoding: gzip");
		desc.headers = &length;
		expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_DESC_INVALID_PARAMETERS, "start with a Content-Length of the caller's");
		desc.headers      = NULL;
		desc.header_count = 0;
		desc.body_size    = 5;
		expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_DESC_INVALID_PARAMETERS, "start with a body size and no body");
		desc.body_size        = 0;
		desc.body             = CRNET_HTTP_BODY_SIZED;
		desc.follow_redirects = true;
		expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_DESC_INVALID_PARAMETERS, "start following redirects with a body");
		desc.body             = CRNET_HTTP_BODY_NONE;
		desc.follow_redirects = false;
		desc.url              = "ftp://127.0.0.1/";
		expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_NET_INVALID_URL, "start with a URL that isn't http or https");
		desc.url        = url;

		// None of those started it: it still can be, and CRNET_NO_WAIT
		// starts it as any limit does.
		desc.timeout_ms = CRNET_NO_WAIT;
		CR_RESULT begun = crnet_http_transfer_start(transfer, &desc);
		check(begun == CR_SUCCESS || begun == CR_RESULT_NET_TIMED_OUT, "and the same transfer starts, with CRNET_NO_WAIT: %s", cr_result_str(begun));
		desc.timeout_ms = PATIENCE_MS;
		expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "start again is refused");
		expect(crnet_http_transfer_send(transfer, "x", 1, PATIENCE_MS, &sent), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "send on a request with no body");
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_SUCCESS, "receive_response");
		crnet_http_transfer_destroy(transfer);

		// A sized body is held to its size.
		crnet_http_transfer_create(client, &transfer);
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/echo", g_server.http_port);
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
		desc.method     = "POST";
		desc.url        = url;
		desc.body       = CRNET_HTTP_BODY_SIZED;
		desc.body_size  = 4;
		desc.timeout_ms = PATIENCE_MS;
		expect(crnet_http_transfer_start(transfer, &desc), CR_SUCCESS, "start a POST announcing 4 bytes");
		expect(crnet_http_transfer_send(transfer, "ab", 2, PATIENCE_MS, &sent), CR_SUCCESS, "send 2 of them");
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "receive_response with 2 still owed");
		expect(crnet_http_transfer_send(transfer, "cdefgh", 6, PATIENCE_MS, &sent), CR_SUCCESS, "send 6 more");
		check(sent == 2, "only the 2 that were owed are taken (%llu)", (unsigned long long)sent);
		expect(crnet_http_transfer_send(transfer, "x", 1, PATIENCE_MS, &sent), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "send past the announced size");
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_SUCCESS, "receive_response now");
		char back[16];
		received = 0;
		crnet_http_transfer_recv(transfer, back, sizeof(back), PATIENCE_MS, &received);
		check(received == 4 && memcmp(back, "abcd", 4) == 0, "the server got exactly the 4");
		crnet_http_transfer_destroy(transfer);
	}

	// Time limits and abort while waiting for a response.
	{
		crnet_http_transfer      transfer = NULL;
		crnet_http_transfer_desc desc;
		char                       url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow?ms=1500", g_server.http_port);
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
		desc.method     = "GET";
		desc.url        = url;
		desc.timeout_ms = PATIENCE_MS;

		crnet_http_transfer_create(client, &transfer);
		expect(crnet_http_transfer_start(transfer, &desc), CR_SUCCESS, "start a request the server takes 1.5 s over");
		uint64_t before = now_ms();
		expect(crnet_http_transfer_receive_response(transfer, 300), CR_RESULT_NET_TIMED_OUT, "receive_response with a 300 ms limit");
		uint64_t waited = now_ms() - before;
		check(waited >= 250 && waited < 1200, "took about its limit (%llu ms)", (unsigned long long)waited);
		expect(crnet_http_transfer_receive_response(transfer, CRNET_NO_WAIT), CR_RESULT_NET_TIMED_OUT, "again with CRNET_NO_WAIT: still not there");
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_SUCCESS, "again with time enough: the transfer was still waiting for it");
		check(crnet_http_transfer_get_status(transfer) == 200, "and it is the response");
		crnet_http_transfer_destroy(transfer);

		snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow?ms=5000", g_server.http_port);
		crnet_http_transfer_create(client, &transfer);
		crnet_http_transfer_start(transfer, &desc);
		struct abort_transfer_later later  = {300, transfer};
		test_thread                 thread = thread_start(abort_transfer_later_run, &later);
		before                             = now_ms();
		CR_RESULT result                  = crnet_http_transfer_receive_response(transfer, CRNET_WAIT_FOREVER);
		waited                             = now_ms() - before;
		thread_join(thread);
		expect(result, CR_RESULT_NET_ABORTED, "receive_response with no limit, aborted from another thread");
		check(waited < 2500, "it came back promptly (%llu ms)", (unsigned long long)waited);
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_RESULT_NET_ABORTED, "an abort doesn't wear off");
		crnet_http_transfer_destroy(transfer);

		// The client is untouched by one of its transfers being aborted.
		memset(&x, 0, sizeof(x));
		x.method = "GET";
		x.path   = "/hello";
		exchange_ok(client, &x, "the client's next transfer after an abort");
		exchange_free(&x);
	}

	// A server that isn't there.
	{
		crnet_tcp_listener_desc listen_desc;
		memset(&listen_desc, 0, sizeof(listen_desc));
		listen_desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
		listen_desc.address = "127.0.0.1";
		listen_desc.backlog = 1;
		crnet_tcp_listener holder = NULL;
		crnet_error        error;
		uint16_t             dead_port = 0;
		// A port that was free a moment ago and has nobody on it now.
		crnet_tcp_listener_create(g_instance, &listen_desc, &holder, &error);
		crnet_tcp_listener_get_port(holder, &dead_port);
		crnet_tcp_listener_destroy(holder);

		char url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%u/hello", (unsigned)dead_port);
		crnet_http_transfer_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
		desc.method     = "GET";
		desc.url        = url;
		desc.timeout_ms = PATIENCE_MS;
		crnet_http_transfer transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		CR_RESULT  result = crnet_http_transfer_start(transfer, &desc);
		const char *where  = "start";
		if (!CR_FAILED(result)) {
			where  = "receive_response";
			result = crnet_http_transfer_receive_response(transfer, PATIENCE_MS);
		}
		expect(result, CR_RESULT_NET_CONNECT_FAILED, "a request to a port nobody listens on");
		note("this stack reports it from %s", where);
		crnet_http_transfer_get_error(transfer, &error);
		check(error.source != CR_PLATFORM_ERROR_SOURCE_NONE, "get_error has the stack's code for it");
		note_error("connection refused", &error);
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "after a failure only destroy is left");
		crnet_http_transfer_destroy(transfer);
	}

	// A server that answers without waiting for the body.
	memset(&x, 0, sizeof(x));
	x.method    = "POST";
	x.path      = "/early";
	x.body      = CRNET_HTTP_BODY_SIZED;
	x.body_data = upload;
	x.body_size = upload_size;
	{
		CR_RESULT result = exchange_run(client, &x);
		if (!CR_FAILED(result))
			note("a server that answers before the body is sent: this stack reaches its response (status %u)", x.status);
		else {
			note("a server that answers before the body is sent: this stack fails in %s with %s", x.failed_in, cr_result_str(result));
			note_error("early answer", &x.error);
		}
	}
	exchange_free(&x);

	// The same with Expect: 100-continue, the caller asking to hear a
	// refusal before the body goes.
	{
		const crnet_header expect_continue = {"Expect", "100-continue"};
		memset(&x, 0, sizeof(x));
		x.method          = "POST";
		x.path            = "/early";
		x.headers         = &expect_continue;
		x.header_count    = 1;
		x.body            = CRNET_HTTP_BODY_SIZED;
		x.body_data       = upload;
		x.body_size       = upload_size;
		CR_RESULT result = exchange_run(client, &x);
		if (!CR_FAILED(result))
			note("the same with Expect: 100-continue: this stack reaches its response (status %u)", x.status);
		else {
			note("the same with Expect: 100-continue: this stack fails in %s with %s", x.failed_in, cr_result_str(result));
			note_error("early answer, Expect: 100-continue", &x.error);
		}
		exchange_free(&x);
	}
	free(upload);

	// A download bigger than anyone would want in memory, read slowly: is
	// the stack held back, or does it read on ahead?
	{
		const long long total = 256LL * 1024 * 1024;
		char            url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/big?n=%lld", g_server.http_port, total);
		crnet_http_transfer_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
		desc.method     = "GET";
		desc.url        = url;
		desc.timeout_ms = PATIENCE_MS;
		crnet_http_transfer transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		CR_RESULT result = crnet_http_transfer_start(transfer, &desc);
		if (!CR_FAILED(result))
			result = crnet_http_transfer_receive_response(transfer, PATIENCE_MS);
		size_t   chunk    = 256 * 1024;
		uint8_t *buffer   = (uint8_t *)malloc(chunk);
		uint64_t received = 0, taken = 0;
		// A megabyte taken, then nothing asked for for two seconds.
		while (!CR_FAILED(result) && taken < 1024 * 1024) {
			result = crnet_http_transfer_recv(transfer, buffer, chunk, PATIENCE_MS, &received);
			taken += received;
		}
		sleep_ms(2000);
		long written = server_stat("big_written");
		check(!CR_FAILED(result), "a 256 MiB download started (%s)", cr_result_str(result));
		check(written > 0 && written < 64LL * 1024 * 1024,
		      "with 1 MiB taken and the caller idle for 2 s, the server has been let send %.1f MiB of 256: the stack is held back", (double)written / (1024.0 * 1024.0));

		// The rest, checked byte for byte.
		bool     intact   = true;
		uint64_t position = taken; // only the first megabyte went unchecked
		while (!CR_FAILED(result)) {
			result = crnet_http_transfer_recv(transfer, buffer, chunk, PATIENCE_MS, &received);
			if (CR_FAILED(result) || received == 0)
				break;
			for (uint64_t i = 0; intact && i < received; i += 4099)
				intact = buffer[i] == pattern_at(position + i);
			position += received;
		}
		check(!CR_FAILED(result) && position == (uint64_t)total && intact, "the whole 256 MiB then arrives, in order (%llu bytes, %s)", (unsigned long long)position, cr_result_str(result));
		free(buffer);
		crnet_http_transfer_destroy(transfer);

		// And one dropped part way: destroy is how a response nobody wants
		// the rest of is stopped.
		crnet_http_transfer_create(client, &transfer);
		crnet_http_transfer_start(transfer, &desc);
		crnet_http_transfer_receive_response(transfer, PATIENCE_MS);
		uint8_t partial[1024];
		crnet_http_transfer_recv(transfer, partial, sizeof(partial), PATIENCE_MS, &received);
		uint64_t before = now_ms();
		crnet_http_transfer_destroy(transfer);
		check(now_ms() - before < 2000, "destroying a transfer 1 KiB into a 256 MiB response returns promptly (%llu ms)", (unsigned long long)(now_ms() - before));
		memset(&x, 0, sizeof(x));
		x.method = "GET";
		x.path   = "/hello";
		exchange_ok(client, &x, "and the client's next transfer is unaffected");
		exchange_free(&x);
	}

	crnet_http_client_destroy(client);
}

// --------------------------------------------------------------------------
// TLS
// --------------------------------------------------------------------------

/* One GET of /hello over https with `tls`, to `host`. The result of the
 * call that failed, or CR_SUCCESS; `error` is that transfer's detail. */
static CR_RESULT tls_get(const crnet_tls_desc *tls, const char *host, crnet_error *error) {
	crnet_http_client_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.tls        = *tls;
	desc.proxy.mode = CRNET_PROXY_NONE;
	crnet_http_client client = NULL;
	CR_RESULT          result = crnet_http_client_create(g_instance, &desc, &client);
	memset(error, 0, sizeof(*error));
	if (CR_FAILED(result))
		return result;

	struct exchange x;
	memset(&x, 0, sizeof(x));
	x.method = "GET";
	x.path   = "/hello";
	x.secure = true;
	x.host   = host;
	result   = exchange_run(client, &x);
	*error   = x.error;
	if (!CR_FAILED(result) && (x.status != 200 || x.response_size != 5))
		result = CR_RESULT_GENERAL_FAILURE;
	exchange_free(&x);
	crnet_http_client_destroy(client);
	return result;
}

static void test_tls(void) {
	part("TLS desc (a server whose certificate is self-signed, for the name \"localhost\")");

	crnet_tls_certificate root  = {g_server.cert_der, g_server.cert_der_size};
	crnet_tls_pin         right = {{0}}, wrong = {{0}};
	memcpy(right.sha256, g_server.pin, CRNET_TLS_PIN_SIZE);
	memcpy(wrong.sha256, g_server.pin, CRNET_TLS_PIN_SIZE);
	wrong.sha256[0] ^= 0xFF;

	crnet_tls_desc tls;
	crnet_error    error;

	memset(&tls, 0, sizeof(tls));
	expect(tls_get(&tls, "localhost", &error), CR_RESULT_NET_TLS_FAILED, "the system's trust store: it is not in it");
	note_error("untrusted certificate", &error);

	memset(&tls, 0, sizeof(tls));
	tls.trust      = CRNET_TLS_TRUST_ROOTS;
	tls.roots      = &root;
	tls.root_count = 1;
	expect(tls_get(&tls, "localhost", &error), CR_SUCCESS, "TRUST_ROOTS with the certificate as the one root");
	if (error.result != CR_SUCCESS)
		note_error("its own certificate as root", &error);
	expect(tls_get(&tls, "127.0.0.1", &error), CR_RESULT_NET_TLS_FAILED, "the same, reached as 127.0.0.1: the certificate is for another name");
	note_error("host name mismatch", &error);
	tls.skip_host_name_check = true;
	expect(tls_get(&tls, "127.0.0.1", &error), CR_SUCCESS, "the same with skip_host_name_check");
	if (error.result != CR_SUCCESS)
		note_error("skip_host_name_check", &error);

	memset(&tls, 0, sizeof(tls));
	tls.trust      = CRNET_TLS_TRUST_SYSTEM_AND_ROOTS;
	tls.roots      = &root;
	tls.root_count = 1;
	expect(tls_get(&tls, "localhost", &error), CR_SUCCESS, "TRUST_SYSTEM_AND_ROOTS with the certificate among the roots");
	if (error.result != CR_SUCCESS)
		note_error("system and roots", &error);

	memset(&tls, 0, sizeof(tls));
	tls.trust                = CRNET_TLS_TRUST_ANY;
	tls.skip_host_name_check = true;
	expect(tls_get(&tls, "127.0.0.1", &error), CR_SUCCESS, "TRUST_ANY");
	if (error.result != CR_SUCCESS)
		note_error("trust any", &error);
	tls.pins      = &right;
	tls.pin_count = 1;
	expect(tls_get(&tls, "127.0.0.1", &error), CR_SUCCESS, "TRUST_ANY with the right pin");
	if (error.result != CR_SUCCESS)
		note_error("right pin", &error);
	tls.pins = &wrong;
	expect(tls_get(&tls, "127.0.0.1", &error), CR_RESULT_NET_TLS_FAILED, "TRUST_ANY with a wrong pin");
	note_error("pin mismatch", &error);
	crnet_tls_pin both[2] = {wrong, right};
	tls.pins                = both;
	tls.pin_count           = 2;
	expect(tls_get(&tls, "127.0.0.1", &error), CR_SUCCESS, "a wrong pin and the right one: any match will do");

	memset(&tls, 0, sizeof(tls));
	tls.trust      = CRNET_TLS_TRUST_ROOTS;
	tls.roots      = &root;
	tls.root_count = 1;
	tls.pins       = &wrong;
	tls.pin_count  = 1;
	expect(tls_get(&tls, "localhost", &error), CR_RESULT_NET_TLS_FAILED, "a trusted chain with a wrong pin is still turned away");

	// Descs that contradict themselves, refused before anything is sent.
	memset(&tls, 0, sizeof(tls));
	tls.trust = CRNET_TLS_TRUST_ANY;
	expect(tls_get(&tls, "localhost", &error), CR_RESULT_DESC_INVALID_PARAMETERS, "TRUST_ANY without skip_host_name_check");
	memset(&tls, 0, sizeof(tls));
	tls.trust = CRNET_TLS_TRUST_ROOTS;
	expect(tls_get(&tls, "localhost", &error), CR_RESULT_DESC_INVALID_PARAMETERS, "TRUST_ROOTS with no roots");
	memset(&tls, 0, sizeof(tls));
	tls.roots      = &root;
	tls.root_count = 1;
	expect(tls_get(&tls, "localhost", &error), CR_RESULT_DESC_INVALID_PARAMETERS, "TRUST_SYSTEM with roots it would ignore");
	{
		static const uint8_t    junk[]   = "this is not a certificate";
		crnet_tls_certificate not_cert = {junk, sizeof(junk)};
		memset(&tls, 0, sizeof(tls));
		tls.trust      = CRNET_TLS_TRUST_ROOTS;
		tls.roots      = &not_cert;
		tls.root_count = 1;
		CR_RESULT result = tls_get(&tls, "localhost", &error);
		check(result == CR_RESULT_DESC_INVALID_PARAMETERS || result == CR_RESULT_NET_TLS_FAILED, "a root that isn't a certificate is refused (%s)", cr_result_str(result));
	}
}

// --------------------------------------------------------------------------
// Proxy
// --------------------------------------------------------------------------

static void test_proxy(void) {
	part("Proxy desc");

	char proxy_url[64];
	snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%d", g_server.proxy_port);
	crnet_http_client_desc desc;
	crnet_http_client      client = NULL;
	struct exchange          x;

	// The host asked for doesn't exist. Only the test's proxy knows what to
	// do with it (it takes every host to be this machine), so a request for
	// it can arrive only by going through the proxy - and a stack that went
	// round the proxy fails to find the host, rather than quietly
	// succeeding. ".test" is reserved never to resolve.
	static const char *proxied_host = "crnet-proxied.test";

	// Through the proxy, plain.
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode = CRNET_PROXY_EXPLICIT;
	desc.proxy.url  = proxy_url;
	if (expect(crnet_http_client_create(g_instance, &desc, &client), CR_SUCCESS, "a client with an explicit proxy")) {
		long before = server_stat("proxy_requests");
		memset(&x, 0, sizeof(x));
		x.method = "GET";
		x.path   = "/hello";
		x.host   = proxied_host;
		if (exchange_ok(client, &x, "GET http://crnet-proxied.test/hello, a host only the proxy can reach"))
			check(server_stat("proxy_requests") - before >= 1, "the proxy carried the request");
		exchange_free(&x);
		crnet_http_client_destroy(client);
	}

	// Through the proxy, TLS: a CONNECT tunnel, with TLS still to the server.
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode               = CRNET_PROXY_EXPLICIT;
	desc.proxy.url                = proxy_url;
	desc.tls.trust                = CRNET_TLS_TRUST_ANY;
	desc.tls.skip_host_name_check = true;
	crnet_tls_pin pin;
	memcpy(pin.sha256, g_server.pin, CRNET_TLS_PIN_SIZE);
	desc.tls.pins      = &pin;
	desc.tls.pin_count = 1;
	client             = NULL;
	if (expect(crnet_http_client_create(g_instance, &desc, &client), CR_SUCCESS, "a client with an explicit proxy and a pin")) {
		long before = server_stat("proxy_requests");
		memset(&x, 0, sizeof(x));
		x.method = "GET";
		x.path   = "/hello";
		x.secure = true;
		x.host   = proxied_host;
		if (exchange_ok(client, &x, "GET https://crnet-proxied.test/hello, the same over TLS"))
			check(server_stat("proxy_requests") - before >= 1, "the proxy tunnelled it, and the pin held against the server beyond");
		exchange_free(&x);
		crnet_http_client_destroy(client);
	}

	// With no proxy the same host is nowhere.
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode = CRNET_PROXY_NONE;
	client          = NULL;
	if (!CR_FAILED(crnet_http_client_create(g_instance, &desc, &client))) {
		memset(&x, 0, sizeof(x));
		x.method = "GET";
		x.path   = "/hello";
		x.host   = proxied_host;
		CR_RESULT result = exchange_run(client, &x);
		expect(result, CR_RESULT_NET_RESOLVE_FAILED, "the same host with CRNET_PROXY_NONE");
		exchange_free(&x);
		crnet_http_client_destroy(client);
	}

	// A proxy that isn't there.
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode = CRNET_PROXY_EXPLICIT;
	desc.proxy.url  = "http://127.0.0.1:9";
	client          = NULL;
	if (!CR_FAILED(crnet_http_client_create(g_instance, &desc, &client))) {
		memset(&x, 0, sizeof(x));
		x.method = "GET";
		x.path   = "/hello";
		x.host   = proxied_host;
		CR_RESULT result = exchange_run(client, &x);
		check(result == CR_RESULT_NET_CONNECT_FAILED, "a proxy that isn't there: %s, in %s", cr_result_str(result), x.failed_in ? x.failed_in : "nothing");
		note_error("dead proxy", &x.error);
		exchange_free(&x);
		crnet_http_client_destroy(client);
	}

	// Descs that are refused.
	static const struct {
		CRNET_PROXY mode;
		const char   *url;
		const char   *what;
	} bad[] = {
	    {CRNET_PROXY_SYSTEM, "http://127.0.0.1:8080", "SYSTEM with a URL it would ignore"},
	    {CRNET_PROXY_NONE, "http://127.0.0.1:8080", "NONE with a URL it would ignore"},
	    {CRNET_PROXY_EXPLICIT, NULL, "EXPLICIT with no URL"},
	    {CRNET_PROXY_EXPLICIT, "http://127.0.0.1", "EXPLICIT with no port"},
	    {CRNET_PROXY_EXPLICIT, "socks5://127.0.0.1:1080", "EXPLICIT with a proxy that isn't http"},
	    {CRNET_PROXY_EXPLICIT, "http://127.0.0.1:8080/path", "EXPLICIT with a path"},
	    {CRNET_PROXY_EXPLICIT, "http://user:secret@127.0.0.1:8080", "EXPLICIT with a user name and password"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
		desc.proxy.mode = bad[i].mode;
		desc.proxy.url  = bad[i].url;
		client          = NULL;
		expect(crnet_http_client_create(g_instance, &desc, &client), CR_RESULT_DESC_INVALID_PARAMETERS, bad[i].what);
		crnet_http_client_destroy(client);
	}
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode = CRNET_PROXY_EXPLICIT;
	desc.proxy.url  = "http://[::1]:8080";
	client          = NULL;
	expect(crnet_http_client_create(g_instance, &desc, &client), CR_SUCCESS, "EXPLICIT with an IPv6 address in brackets is taken");
	crnet_http_client_destroy(client);
}

// --------------------------------------------------------------------------
// WebSocket
// --------------------------------------------------------------------------

#if CRNET_EXT_WEBSOCKET

static CR_RESULT websocket_open(crnet_websocket *out_socket, const char *path, bool secure, uint64_t max_message_size, bool follow_redirects, uint32_t *out_status) {
	char url[256];
	snprintf(url, sizeof(url), "%s://%s:%d%s", secure ? "wss" : "ws", secure ? "localhost" : "127.0.0.1", secure ? g_server.wss_port : g_server.ws_port, path);
	crnet_websocket_connect_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC;
	desc.url              = url;
	desc.timeout_ms       = PATIENCE_MS;
	desc.max_message_size = max_message_size;
	desc.follow_redirects = follow_redirects;
	desc.proxy.mode       = CRNET_PROXY_NONE;
	crnet_tls_certificate root = {g_server.cert_der, g_server.cert_der_size};
	if (secure) {
		desc.tls.trust      = CRNET_TLS_TRUST_ROOTS;
		desc.tls.roots      = &root;
		desc.tls.root_count = 1;
	}
	*out_socket       = NULL;
	CR_RESULT result = crnet_websocket_create(g_instance, out_socket);
	if (CR_FAILED(result))
		return result;
	return crnet_websocket_connect(*out_socket, &desc, out_status);
}

/* One whole message, however many recvs it takes. */
static CR_RESULT websocket_read_message(crnet_websocket socket, uint8_t *buffer, size_t capacity, size_t *out_size, CRNET_WEBSOCKET_MESSAGE *out_type, uint32_t timeout_ms) {
	*out_size = 0;
	for (;;) {
		uint64_t   received = 0;
		bool       complete = false;
		CR_RESULT result   = crnet_websocket_recv(socket, buffer + *out_size, capacity - *out_size, timeout_ms, &received, out_type, &complete);
		if (CR_FAILED(result))
			return result;
		*out_size += (size_t)received;
		if (complete)
			return CR_SUCCESS;
		if (*out_size == capacity)
			return CR_RESULT_GENERAL_FAILURE;
	}
}

static test_thread_result TEST_THREAD_CALL abort_websocket_later(void *argument) {
	sleep_ms(300);
	crnet_websocket_abort((crnet_websocket)argument);
	return 0;
}

static void test_websocket(void) {
	part("WebSocket");

	crnet_websocket         socket = NULL;
	uint32_t                  status = 0;
	static uint8_t            buffer[512 * 1024];
	size_t                    size = 0;
	CRNET_WEBSOCKET_MESSAGE type;
	crnet_error             error;

	if (CR_FAILED(crnet_websocket_create(g_instance, &socket))) {
		note("this build's stack has no WebSocket; skipped");
		return;
	}
	uint64_t received = 0;
	bool     complete = false;
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "x", 1, PATIENCE_MS), CR_RESULT_NET_INVALID_WEBSOCKET, "send before connect");
	crnet_websocket_destroy(socket);

	if (!expect(websocket_open(&socket, "/echo", false, 1024 * 1024, false, &status), CR_SUCCESS, "connect to ws://.../echo")) {
		crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
		note_error("connect", &error);
		crnet_websocket_destroy(socket);
		return;
	}
	check(status == 101, "the opening request's status is 101");

	expect(crnet_websocket_recv(socket, buffer, sizeof(buffer), CRNET_NO_WAIT, &received, &type, &complete), CR_RESULT_NET_TIMED_OUT, "recv with CRNET_NO_WAIT and nothing sent");
	uint64_t before = now_ms();
	expect(crnet_websocket_recv(socket, buffer, sizeof(buffer), 200, &received, &type, &complete), CR_RESULT_NET_TIMED_OUT, "recv with a 200 ms limit and nothing sent");
	uint64_t waited = now_ms() - before;
	check(waited >= 150 && waited < 1500, "that recv took about its limit (%llu ms)", (unsigned long long)waited);

	// Text and binary, echoed.
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "hello, socket", 13, PATIENCE_MS), CR_SUCCESS, "send a text message");
	expect(websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, PATIENCE_MS), CR_SUCCESS, "recv its echo");
	check(size == 13 && memcmp(buffer, "hello, socket", 13) == 0 && type == CRNET_WEBSOCKET_MESSAGE_TEXT, "the same text, as text");

	uint8_t binary[300];
	for (size_t i = 0; i < sizeof(binary); ++i)
		binary[i] = (uint8_t)i;
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_BINARY, binary, sizeof(binary), PATIENCE_MS), CR_SUCCESS, "send a binary message");
	expect(websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, PATIENCE_MS), CR_SUCCESS, "recv its echo");
	check(size == sizeof(binary) && memcmp(buffer, binary, sizeof(binary)) == 0 && type == CRNET_WEBSOCKET_MESSAGE_BINARY, "the same bytes, as binary");

	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, NULL, 0, PATIENCE_MS), CR_SUCCESS, "send an empty message");
	received = 99;
	complete = false;
	expect(crnet_websocket_recv(socket, buffer, sizeof(buffer), PATIENCE_MS, &received, &type, &complete), CR_SUCCESS, "recv its echo");
	check(received == 0 && complete, "0 bytes, and complete");

	static const uint8_t not_utf8[] = {0xC3, 0x28};
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, not_utf8, sizeof(not_utf8), PATIENCE_MS), CR_RESULT_NET_SEND_FAILED, "send text that isn't UTF-8");
	if (CR_FAILED(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "still alive", 11, PATIENCE_MS)) ||
	    CR_FAILED(websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, PATIENCE_MS))) {
		note("refusing that text ended the connection on this stack; reconnecting");
		crnet_websocket_destroy(socket);
		websocket_open(&socket, "/echo", false, 1024 * 1024, false, &status);
	}

	// A message taken in pieces through a small buffer.
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "send:100000", 11, PATIENCE_MS), CR_SUCCESS, "ask the server for a 100000-byte message");
	{
		uint8_t    piece[1000];
		size_t     total  = 0;
		int        pieces = 0;
		bool       intact = true;
		CR_RESULT result = CR_SUCCESS;
		complete          = false;
		while (!complete) {
			result = crnet_websocket_recv(socket, piece, sizeof(piece), PATIENCE_MS, &received, &type, &complete);
			if (CR_FAILED(result))
				break;
			for (uint64_t i = 0; i < received; ++i)
				intact = intact && piece[i] == pattern_at(total + i);
			total += (size_t)received;
			pieces++;
		}
		check(!CR_FAILED(result) && total == 100000 && intact && pieces >= 100, "it arrives whole and in order through a 1000-byte buffer (%d recvs, %zu bytes, %s)", pieces, total, cr_result_str(result));
	}

	// Several messages queued at once keep their boundaries.
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "burst:5", 7, PATIENCE_MS), CR_SUCCESS, "ask the server for five messages at once");
	{
		bool separate = true;
		for (int i = 0; i < 5; ++i) {
			char want[32];
			snprintf(want, sizeof(want), "burst %d", i);
			CR_RESULT result = websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, PATIENCE_MS);
			separate          = separate && !CR_FAILED(result) && size == strlen(want) && memcmp(buffer, want, size) == 0;
		}
		check(separate, "they arrive as five, in order");
	}

	// The wait set names a WebSocket that has something.
	{
		crnet_wait_set set = NULL;
		crnet_wait_set_create(g_instance, &set, &error);
		int                tag = 7;
		crnet_wait_event events[4];
		uint32_t           count = 0;
		expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "add the socket to a wait set");
		// Being named once for no reason is allowed; drain and ask again.
		if (!CR_FAILED(crnet_wait_set_wait(set, 100, events, 4, &count)))
			crnet_websocket_recv(socket, buffer, sizeof(buffer), CRNET_NO_WAIT, &received, &type, &complete);
		expect(crnet_wait_set_wait(set, 100, events, 4, &count), CR_RESULT_NET_TIMED_OUT, "nothing sent: the set is quiet");
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "wake", 4, PATIENCE_MS);
		CR_RESULT result = crnet_wait_set_wait(set, PATIENCE_MS, events, 4, &count);
		check(!CR_FAILED(result) && count == 1 && events[0].user == &tag, "an echo on its way: the set names the socket");
		expect(websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, CRNET_NO_WAIT), CR_SUCCESS, "and recv with CRNET_NO_WAIT has the message");
		crnet_wait_set_remove_websocket(set, socket);
		crnet_wait_set_destroy(set);
	}

	// The server closes.
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "close-me", 8, PATIENCE_MS), CR_SUCCESS, "ask the server to close with code 4001");
	expect(crnet_websocket_recv(socket, buffer, sizeof(buffer), PATIENCE_MS, &received, &type, &complete), CR_RESULT_NET_WS_CLOSED, "recv then reports the close");
	check(crnet_websocket_get_close_code(socket) == 4001, "get_close_code is the server's (%u)", (unsigned)crnet_websocket_get_close_code(socket));
	expect(crnet_websocket_close(socket, 1000, NULL), CR_RESULT_NET_SEND_FAILED, "close on a connection that has already ended");
	crnet_websocket_destroy(socket);

	// This side closes: the end of the connection there and then.
	if (!CR_FAILED(websocket_open(&socket, "/echo", false, 1024 * 1024, false, &status))) {
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "unread", 6, PATIENCE_MS);
		sleep_ms(300); // its echo has arrived and is not received
		expect(crnet_websocket_close(socket, 1000, "done"), CR_SUCCESS, "close from this side, with an echo unread");
		before = now_ms();
		expect(crnet_websocket_recv(socket, buffer, sizeof(buffer), PATIENCE_MS, &received, &type, &complete), CR_RESULT_NET_WS_CLOSED, "recv after it");
		check(now_ms() - before < 1000, "at once, without waiting for the peer (%llu ms)", (unsigned long long)(now_ms() - before));
		check(crnet_websocket_get_close_code(socket) == 0, "get_close_code is 0 for a close this side made");
		check(CR_FAILED(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "x", 1, PATIENCE_MS)), "send after it fails");
		check(CR_FAILED(crnet_websocket_close(socket, 1000, NULL)), "a second close fails");
		crnet_websocket_destroy(socket);
	}

	// A message over the limit.
	if (!CR_FAILED(websocket_open(&socket, "/echo", false, 1024, false, &status))) {
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "send:5000", 9, PATIENCE_MS);
		CR_RESULT result = CR_SUCCESS;
		complete          = false;
		while (!CR_FAILED(result))
			result = crnet_websocket_recv(socket, buffer, sizeof(buffer), PATIENCE_MS, &received, &type, &complete);
		expect(result, CR_RESULT_NET_WS_MESSAGE_TOO_BIG, "a 5000-byte message against a 1024-byte limit");
		crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
		note_error("message too big", &error);
		crnet_websocket_destroy(socket);
	}

	// Redirects and refusals of the opening request.
	expect(websocket_open(&socket, "/redirect", false, 1024, false, &status), CR_RESULT_NET_WS_UPGRADE_REFUSED, "an opening request answered with a redirect, not following");
	check(status == 302, "its status is the 302 (%u)", status);
	crnet_websocket_destroy(socket);
#if defined(__linux__)
	// libcurl never follows a WebSocket redirect: a known problem in crnet.h.
	CR_RESULT followed = websocket_open(&socket, "/redirect", false, 1024, true, &status);
	if (followed == CR_RESULT_NET_WS_UPGRADE_REFUSED && status == 302) {
		note("the same, following: handed back as the 302 on this stack (known problem)");
	} else if (expect(followed, CR_SUCCESS, "the same, following")) {
#else
	if (expect(websocket_open(&socket, "/redirect", false, 1024, true, &status), CR_SUCCESS, "the same, following")) {
#endif
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "moved", 5, PATIENCE_MS);
		expect(websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, PATIENCE_MS), CR_SUCCESS, "and the socket is connected to where it led");
	} else {
		crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
		note_error("followed redirect", &error);
	}
	crnet_websocket_destroy(socket);
	expect(websocket_open(&socket, "/refuse", false, 1024, false, &status), CR_RESULT_NET_WS_UPGRADE_REFUSED, "an opening request answered with 401");
	check(status == 401, "its status is the 401 (%u)", status);
	crnet_websocket_destroy(socket);

	// wss, with the certificate as the root.
	if (expect(websocket_open(&socket, "/echo", true, 1024 * 1024, false, &status), CR_SUCCESS, "connect to wss://localhost/echo with the certificate as root")) {
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "secure", 6, PATIENCE_MS);
		expect(websocket_read_message(socket, buffer, sizeof(buffer), &size, &type, PATIENCE_MS), CR_SUCCESS, "and an echo over it");
	} else {
		crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
		note_error("wss connect", &error);
	}
	crnet_websocket_destroy(socket);
	{
		// The same server with the system's trust: turned away.
		char url[128];
		snprintf(url, sizeof(url), "wss://localhost:%d/echo", g_server.wss_port);
		crnet_websocket_connect_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC;
		desc.url              = url;
		desc.timeout_ms       = PATIENCE_MS;
		desc.max_message_size = 1024;
		desc.proxy.mode       = CRNET_PROXY_NONE;
		crnet_websocket_create(g_instance, &socket);
		expect(crnet_websocket_connect(socket, &desc, &status), CR_RESULT_NET_TLS_FAILED, "wss with the system's trust store");
		crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
		note_error("wss untrusted", &error);
		crnet_websocket_destroy(socket);
	}

	// Abort from another thread.
	if (!CR_FAILED(websocket_open(&socket, "/echo", false, 1024, false, &status))) {
		test_thread thread = thread_start(abort_websocket_later, socket);
		before             = now_ms();
		CR_RESULT result  = crnet_websocket_recv(socket, buffer, sizeof(buffer), CRNET_WAIT_FOREVER, &received, &type, &complete);
		waited             = now_ms() - before;
		thread_join(thread);
		expect(result, CR_RESULT_NET_ABORTED, "a recv waiting forever, aborted from another thread");
		check(waited < 2500, "it came back promptly (%llu ms)", (unsigned long long)waited);
		crnet_websocket_destroy(socket);
	}
}

#endif // CRNET_EXT_WEBSOCKET

// --------------------------------------------------------------------------
// Resumable starts: steps begun with CRNET_NO_WAIT and carried on later
// --------------------------------------------------------------------------

/* Every loop below advances its object with CRNET_NO_WAIT only and sleeps in
 * crnet_wait_set_wait() between calls, never anywhere else. The server holds
 * each answer back for RESUMABLE_HELD_MS, so a call that waited for one would
 * take that long; each call's time is measured, and the longest must stay
 * under RESUMABLE_BOUND_MS whatever else the machine is doing. */
#define RESUMABLE_HELD_MS  1000u
#define RESUMABLE_BOUND_MS 250u

static uint64_t    g_call_start;
static uint64_t    g_longest_ms;
static const char *g_longest_what = "none";

static void call_begin(void) { g_call_start = now_ms(); }

static CR_RESULT call_end(CR_RESULT result, const char *what) {
	uint64_t took = now_ms() - g_call_start;
	if (took >= g_longest_ms) {
		g_longest_ms   = took;
		g_longest_what = what;
	}
	return result;
}

/* A call with CRNET_NO_WAIT, timed. */
#define TIMED(what, call) (call_begin(), call_end((call), (what)))

static void longest_reset(void) {
	g_longest_ms   = 0;
	g_longest_what = "none";
}

static void longest_check(const char *loop) {
	check(g_longest_ms < RESUMABLE_BOUND_MS, "%s: no call held the thread %u ms (longest %llu ms, %s)", loop, RESUMABLE_BOUND_MS, (unsigned long long)g_longest_ms,
	      g_longest_what);
}

/* The one place the loops wait. `*out_ready` gathers what was named. False
 * when nothing was named within PATIENCE_MS or the wait failed. */
static bool sleep_in(crnet_wait_set set, uint32_t *out_ready) {
	crnet_wait_event events[8];
	uint32_t           count = 0;
	*out_ready               = 0;
	CR_RESULT result        = crnet_wait_set_wait(set, PATIENCE_MS, events, 8, &count);
	for (uint32_t i = 0; i < count; ++i)
		*out_ready |= events[i].ready;
	return result == CR_SUCCESS;
}

/* A port nothing listens on: one the system gave a listener that is gone. */
static uint16_t closed_port(void) {
	crnet_tcp_listener_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	desc.address            = "127.0.0.1";
	desc.backlog            = 1;
	crnet_tcp_listener listener = NULL;
	uint16_t           port     = 0;
	if (!CR_FAILED(crnet_tcp_listener_create(g_instance, &desc, &listener, NULL)))
		crnet_tcp_listener_get_port(listener, &port);
	crnet_tcp_listener_destroy(listener);
	return port;
}

#if CRNET_EXT_TCP

static void resumable_tcp(crnet_wait_set set) {
	crnet_error error;
	int         tag = 1;

	crnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type          = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address            = "127.0.0.1";
	listen_desc.backlog            = 4;
	crnet_tcp_listener listener = NULL;
	if (!expect(crnet_tcp_listener_create(g_instance, &listen_desc, &listener, &error), CR_SUCCESS, "a listener for the TCP connects"))
		return;
	uint16_t port = 0;
	crnet_tcp_listener_get_port(listener, &port);

	crnet_tcp_socket_connect_desc connect_desc;
	memset(&connect_desc, 0, sizeof(connect_desc));
	connect_desc.base.type  = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
	connect_desc.address    = "127.0.0.1";
	connect_desc.port       = port;
	connect_desc.timeout_ms = CRNET_NO_WAIT;

	crnet_tcp_socket socket = NULL;
	crnet_tcp_socket_create(g_instance, &socket);
	expect(crnet_tcp_socket_connect_continue(socket, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_TCP_SOCKET, "TCP connect_continue before any connect");
	expect(crnet_wait_set_add_tcp_socket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_INVALID_TCP_SOCKET, "a socket never connected isn't taken by a wait set");

	longest_reset();
	CR_RESULT result = TIMED("crnet_tcp_socket_connect", crnet_tcp_socket_connect(socket, &connect_desc));
	check(result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT, "TCP connect with CRNET_NO_WAIT begins it: %s", cr_result_str(result));
	if (result == CR_RESULT_NET_TIMED_OUT) {
		uint64_t sent = 0;
		expect(crnet_tcp_socket_connect(socket, &connect_desc), CR_RESULT_NET_INVALID_TCP_SOCKET, "a second connect while it is under way");
		expect(crnet_tcp_socket_send(socket, "x", 1, CRNET_NO_WAIT, &sent), CR_RESULT_NET_INVALID_TCP_SOCKET, "send while it is under way");
		expect(crnet_wait_set_add_tcp_socket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "a connecting TCP socket goes in a wait set");
		bool named = false;
		while (result == CR_RESULT_NET_TIMED_OUT) {
			uint32_t ready = 0;
			if (!sleep_in(set, &ready))
				break;
			named  = true;
			result = TIMED("crnet_tcp_socket_connect_continue", crnet_tcp_socket_connect_continue(socket, CRNET_NO_WAIT));
		}
		check(named, "the set named it once its connect had an outcome");
		crnet_wait_set_remove_tcp_socket(set, socket);
	} else {
		note("the system connected inside the call");
	}
	expect(result, CR_SUCCESS, "carried on with CRNET_NO_WAIT between waits on the set, it connects");
	expect(crnet_tcp_socket_connect_continue(socket, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_TCP_SOCKET, "connect_continue after the outcome");
	expect(crnet_tcp_socket_connect_continue(socket, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_TCP_SOCKET, "and a second time");
	crnet_tcp_socket server = NULL;
	if (expect(crnet_tcp_listener_accept(listener, PATIENCE_MS, &server), CR_SUCCESS, "the listener has the connection")) {
		uint64_t sent = 0, received = 0;
		char     buffer[8];
		crnet_tcp_socket_send(socket, "ping", 4, PATIENCE_MS, &sent);
		crnet_tcp_socket_recv(server, buffer, sizeof(buffer), PATIENCE_MS, &received);
		check(sent == 4 && received == 4 && memcmp(buffer, "ping", 4) == 0, "and bytes cross it");
	}
	longest_check("TCP connect");
	crnet_tcp_socket_destroy(server);
	crnet_tcp_socket_destroy(socket);
	crnet_tcp_listener_destroy(listener);

	// Refused: the outcome comes from whichever call finds it.
	socket                  = NULL;
	connect_desc.port       = closed_port();
	crnet_tcp_socket_create(g_instance, &socket);
	result                  = crnet_tcp_socket_connect(socket, &connect_desc);
	const char *reported_by = "connect";
	if (result == CR_RESULT_NET_TIMED_OUT) {
		reported_by = "connect_continue";
		expect(crnet_wait_set_add_tcp_socket(set, socket, CRNET_WAIT_FOR_ROOM, &tag), CR_SUCCESS, "a socket connecting to nobody goes in a wait set");
		uint32_t ready = 0;
		check(sleep_in(set, &ready), "the set names it once refused");
		crnet_wait_set_remove_tcp_socket(set, socket);
		result = crnet_tcp_socket_connect_continue(socket, PATIENCE_MS);
	}
	check(result == CR_RESULT_NET_CONNECT_FAILED, "a refused connect begun with CRNET_NO_WAIT, reported by %s: %s", reported_by, cr_result_str(result));
	crnet_tcp_socket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
	check(error.result == CR_RESULT_NET_CONNECT_FAILED && error.source != CR_PLATFORM_ERROR_SOURCE_NONE, "with the system's reason on the receiving side");
	expect(crnet_tcp_socket_connect_continue(socket, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_TCP_SOCKET, "connect_continue after a refusal");
	expect(crnet_wait_set_add_tcp_socket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_INVALID_TCP_SOCKET, "a socket whose connect failed isn't taken by a wait set");
	crnet_tcp_socket_destroy(socket);
}

#endif // CRNET_EXT_TCP

/* Drives a started transfer's response and body with CRNET_NO_WAIT only,
 * sleeping in `set` (where the transfer is, for INCOMING) between calls.
 * The body's first `capacity` bytes are kept; `*out_size` is all of it. */
static CR_RESULT http_drive(crnet_http_transfer transfer, crnet_wait_set set, uint8_t *body, size_t capacity, size_t *out_size, uint32_t *out_named) {
	*out_size        = 0;
	*out_named       = 0;
	CR_RESULT result = CR_RESULT_NET_TIMED_OUT;
	while ((result = TIMED("crnet_http_transfer_receive_response", crnet_http_transfer_receive_response(transfer, CRNET_NO_WAIT))) == CR_RESULT_NET_TIMED_OUT) {
		uint32_t ready = 0;
		if (!sleep_in(set, &ready))
			return CR_RESULT_NET_TIMED_OUT;
		*out_named |= ready;
	}
	if (CR_FAILED(result))
		return result;
	static uint8_t piece[64 * 1024];
	for (;;) {
		uint64_t got = 0;
		result       = TIMED("crnet_http_transfer_recv", crnet_http_transfer_recv(transfer, piece, sizeof(piece), CRNET_NO_WAIT, &got));
		if (result == CR_RESULT_NET_TIMED_OUT) {
			uint32_t ready = 0;
			if (!sleep_in(set, &ready))
				return CR_RESULT_NET_TIMED_OUT;
			*out_named |= ready;
			continue;
		}
		if (CR_FAILED(result))
			return result;
		if (got == 0)
			return CR_SUCCESS;
		if (*out_size < capacity)
			memcpy(body + *out_size, piece, (size_t)(capacity - *out_size < got ? capacity - *out_size : got));
		*out_size += (size_t)got;
	}
}

static crnet_http_transfer_desc http_desc(const char *method, const char *url, CRNET_HTTP_BODY body, uint64_t body_size, uint32_t timeout_ms) {
	crnet_http_transfer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
	desc.method    = method;
	desc.url       = url;
	desc.body      = body;
	desc.body_size = body_size;
	desc.timeout_ms = timeout_ms;
	return desc;
}

/* Sends `size` bytes of the pattern as a request body with CRNET_NO_WAIT
 * only, sleeping in `set` (ROOM asked) whenever nothing was taken. */
static CR_RESULT http_send_pattern(crnet_http_transfer transfer, crnet_wait_set set, uint64_t size, bool *out_backed_up, bool *out_room_named) {
	static uint8_t piece[16 * 1024];
	uint64_t       done = 0;
	*out_backed_up      = false;
	*out_room_named     = false;
	while (done < size) {
		size_t count = (size_t)(size - done < sizeof(piece) ? size - done : sizeof(piece));
		for (size_t i = 0; i < count; ++i)
			piece[i] = pattern_at(done + i);
		uint64_t  sent   = 0;
		CR_RESULT result = TIMED("crnet_http_transfer_send", crnet_http_transfer_send(transfer, piece, count, CRNET_NO_WAIT, &sent));
		if (result == CR_RESULT_NET_TIMED_OUT) {
			*out_backed_up = true;
			uint32_t ready = 0;
			if (!sleep_in(set, &ready))
				return CR_RESULT_NET_TIMED_OUT;
			if (ready & CRNET_WAIT_FOR_ROOM)
				*out_room_named = true;
			continue;
		}
		if (CR_FAILED(result))
			return result;
		done += sent;
	}
	return CR_SUCCESS;
}

struct abort_http_later {
	crnet_http_transfer transfer;
	uint32_t            delay_ms;
};

static test_thread_result TEST_THREAD_CALL abort_http_later_run(void *argument) {
	struct abort_http_later *later = (struct abort_http_later *)argument;
	sleep_ms(later->delay_ms);
	crnet_http_transfer_abort(later->transfer);
	return 0;
}

static void resumable_http(crnet_wait_set set, crnet_wait_set other_set) {
	crnet_http_client client = plain_client();
	if (!client)
		return;
	int      tag = 2;
	char     url[256];
	uint8_t  body[256];
	size_t   size  = 0;
	uint32_t named = 0;

	// A GET whose answer the server holds back.
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow?ms=%u", g_server.http_port, RESUMABLE_HELD_MS);
	crnet_http_transfer_desc desc     = http_desc("GET", url, CRNET_HTTP_BODY_NONE, 0, CRNET_NO_WAIT);
	crnet_http_transfer      transfer = NULL;
	crnet_http_transfer_create(client, &transfer);
	expect(crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "a transfer not started isn't taken by a wait set");
	longest_reset();
	CR_RESULT result = TIMED("crnet_http_transfer_start", crnet_http_transfer_start(transfer, &desc));
	check(result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT, "start with CRNET_NO_WAIT begins the request: %s", cr_result_str(result));
	expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "start again is refused, whatever the first returned");
	expect(crnet_wait_set_add_http_transfer(set, transfer, 0, &tag), CR_RESULT_DESC_INVALID_PARAMETERS, "add a transfer asking for nothing");
	expect(crnet_wait_set_add_http_transfer(set, transfer, 4, &tag), CR_RESULT_DESC_INVALID_PARAMETERS, "add a transfer asking for a bit that isn't one");
	expect(crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "a started transfer goes in a wait set");
	expect(crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_ALREADY_IN_WAIT_SET, "and not twice");
	expect(crnet_wait_set_add_http_transfer(other_set, transfer, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_ALREADY_IN_WAIT_SET, "nor in a second set");
	result = http_drive(transfer, set, body, sizeof(body), &size, &named);
	check(!CR_FAILED(result) && crnet_http_transfer_get_status(transfer) == 200 && size == 4 && memcmp(body, "slow", 4) == 0,
	      "a GET driven with CRNET_NO_WAIT only, sleeping in the set: 200 \"slow\" (%s)", cr_result_str(result));
	check((named & CRNET_WAIT_FOR_INCOMING) != 0, "the set named the transfer INCOMING for its response and body");
	longest_check("HTTP GET held back 1 s");
	crnet_wait_set_remove_http_transfer(set, transfer);
	crnet_wait_set_remove_http_transfer(set, transfer); // a second time: nothing to do
	crnet_http_transfer_destroy(transfer);

	// A sized POST to a server that reads it slowly: room comes and goes.
	{
		const uint64_t total = 1024 * 1024;
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow-read?ms=5", g_server.http_port);
		desc     = http_desc("POST", url, CRNET_HTTP_BODY_SIZED, total, CRNET_NO_WAIT);
		transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		longest_reset();
		result = TIMED("crnet_http_transfer_start", crnet_http_transfer_start(transfer, &desc));
		check(result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT, "a sized POST started with CRNET_NO_WAIT: %s", cr_result_str(result));
		expect(crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING | CRNET_WAIT_FOR_ROOM, &tag), CR_SUCCESS, "in a set for INCOMING and ROOM");
		bool backed_up = false, room_named = false;
		result         = http_send_pattern(transfer, set, total, &backed_up, &room_named);
		expect(result, CR_SUCCESS, "its 1 MiB body sent with CRNET_NO_WAIT only");
		if (backed_up)
			check(room_named, "the body backed up, and the set named the transfer ROOM when it had room again");
		else
			note("the body never backed up on this stack");
		crnet_wait_set_remove_http_transfer(set, transfer);
		expect(crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "back in the set for INCOMING alone");
		result = http_drive(transfer, set, body, sizeof(body) - 1, &size, &named);
		body[size < sizeof(body) ? size : sizeof(body) - 1] = 0;
		check(!CR_FAILED(result) && crnet_http_transfer_get_status(transfer) == 200 && strcmp((char *)body, "1048576") == 0,
		      "and the server read all of it (\"%s\", %s)", (char *)body, cr_result_str(result));
		longest_check("HTTP sized POST read slowly");
		crnet_wait_set_remove_http_transfer(set, transfer);
		crnet_http_transfer_destroy(transfer);
	}

	// An unsized upload, echoed.
	{
		const uint64_t total = 300 * 1024;
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/echo", g_server.http_port);
		desc     = http_desc("PUT", url, CRNET_HTTP_BODY_UNSIZED, 0, CRNET_NO_WAIT);
		transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		longest_reset();
		result = TIMED("crnet_http_transfer_start", crnet_http_transfer_start(transfer, &desc));
		check(result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT, "an unsized PUT started with CRNET_NO_WAIT: %s", cr_result_str(result));
		crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_ROOM, &tag);
		bool backed_up = false, room_named = false;
		expect(http_send_pattern(transfer, set, total, &backed_up, &room_named), CR_SUCCESS, "its 300 KiB sent with CRNET_NO_WAIT only");
		crnet_wait_set_remove_http_transfer(set, transfer);
		crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING, &tag);
		static uint8_t echoed[300 * 1024];
		result      = http_drive(transfer, set, echoed, sizeof(echoed), &size, &named);
		bool intact = size == total;
		for (size_t i = 0; intact && i < size; ++i)
			intact = echoed[i] == pattern_at(i);
		check(!CR_FAILED(result) && intact, "and its echo comes back whole (%zu bytes, %s)", size, cr_result_str(result));
		longest_check("HTTP unsized PUT");
		crnet_wait_set_remove_http_transfer(set, transfer);
		crnet_http_transfer_destroy(transfer);
	}

	// A start that can't connect is reported by the next call.
	{
		snprintf(url, sizeof(url), "http://127.0.0.1:%u/hello", (unsigned)closed_port());
		desc     = http_desc("GET", url, CRNET_HTTP_BODY_NONE, 0, CRNET_NO_WAIT);
		transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		result = crnet_http_transfer_start(transfer, &desc);
		check(result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT, "start with CRNET_NO_WAIT to a closed port: %s", cr_result_str(result));
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_RESULT_NET_CONNECT_FAILED, "receive_response reports the refusal");
		crnet_error error;
		crnet_http_transfer_get_error(transfer, &error);
		check(error.result == CR_RESULT_NET_CONNECT_FAILED, "and the transfer's record has it");
		note_error("start to a closed port", &error);
		crnet_http_transfer_destroy(transfer);
	}

	// A longer limit that runs out leaves the transfer usable.
	{
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow?ms=500", g_server.http_port);
		desc     = http_desc("GET", url, CRNET_HTTP_BODY_NONE, 0, 1);
		transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		result = crnet_http_transfer_start(transfer, &desc);
		check(result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT, "start with a 1 ms limit: %s", cr_result_str(result));
		expect(crnet_http_transfer_receive_response(transfer, 1), CR_RESULT_NET_TIMED_OUT, "receive_response with 1 ms: not yet");
		expect(crnet_http_transfer_receive_response(transfer, PATIENCE_MS), CR_SUCCESS, "and then the response, the transfer as usable as before");
		check(crnet_http_transfer_get_status(transfer) == 200, "its status is 200");
		crnet_http_transfer_destroy(transfer);
	}

	// Aborted while in a set: named, then ABORTED.
	{
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow?ms=3000", g_server.http_port);
		desc     = http_desc("GET", url, CRNET_HTTP_BODY_NONE, 0, CRNET_NO_WAIT);
		transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		crnet_http_transfer_start(transfer, &desc);
		crnet_wait_set_add_http_transfer(set, transfer, CRNET_WAIT_FOR_INCOMING, &tag);
		struct abort_http_later later  = {transfer, 300};
		test_thread             thread = thread_start(abort_http_later_run, &later);
		uint64_t                before = now_ms();
		uint32_t                ready  = 0;
		bool                    woke   = sleep_in(set, &ready);
		uint64_t                waited = now_ms() - before;
		thread_join(thread);
		check(woke && (ready & CRNET_WAIT_FOR_INCOMING) && waited < 2500, "a transfer in a set, aborted from another thread: the set names it (%llu ms)", (unsigned long long)waited);
		expect(crnet_http_transfer_receive_response(transfer, CRNET_NO_WAIT), CR_RESULT_NET_ABORTED, "and receive_response says ABORTED");
		crnet_wait_set_remove_http_transfer(set, transfer);
		crnet_http_transfer_destroy(transfer);
	}

	// Aborted while receive_response carries the start on.
	{
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow?ms=3000", g_server.http_port);
		desc     = http_desc("GET", url, CRNET_HTTP_BODY_NONE, 0, CRNET_NO_WAIT);
		transfer = NULL;
		crnet_http_transfer_create(client, &transfer);
		crnet_http_transfer_start(transfer, &desc);
		struct abort_http_later later  = {transfer, 300};
		test_thread             thread = thread_start(abort_http_later_run, &later);
		uint64_t                before = now_ms();
		result                         = crnet_http_transfer_receive_response(transfer, CRNET_WAIT_FOREVER);
		uint64_t waited                = now_ms() - before;
		thread_join(thread);
		check(result == CR_RESULT_NET_ABORTED && waited < 2500, "a start carried on by receive_response, aborted from another thread: %s (%llu ms)", cr_result_str(result),
		      (unsigned long long)waited);
		crnet_http_transfer_destroy(transfer);
	}
	crnet_http_client_destroy(client);
}

#if CRNET_EXT_WEBSOCKET

static crnet_websocket_connect_desc websocket_desc(char *url, size_t capacity, const char *path, bool secure, uint32_t timeout_ms, crnet_tls_certificate *root) {
	snprintf(url, capacity, "%s://%s:%d%s", secure ? "wss" : "ws", secure ? "localhost" : "127.0.0.1", secure ? g_server.wss_port : g_server.ws_port, path);
	crnet_websocket_connect_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC;
	desc.url              = url;
	desc.timeout_ms       = timeout_ms;
	desc.max_message_size = 128ull * 1024 * 1024;
	desc.proxy.mode       = CRNET_PROXY_NONE;
	if (root) {
		desc.tls.trust      = CRNET_TLS_TRUST_ROOTS;
		desc.tls.roots      = root;
		desc.tls.root_count = 1;
	}
	return desc;
}

/* Reads one whole message into `buffer` and checks it against the pattern
 * from `offset` (each byte pattern_at(offset + i)), or against `text`. */
static bool websocket_message_is(crnet_websocket socket, uint8_t *buffer, size_t capacity, uint64_t pattern_size, const char *text) {
	size_t                    size = 0;
	CRNET_WEBSOCKET_MESSAGE type;
	if (CR_FAILED(websocket_read_message(socket, buffer, capacity, &size, &type, PATIENCE_MS)))
		return false;
	if (text)
		return size == strlen(text) && memcmp(buffer, text, size) == 0;
	if (size != pattern_size)
		return false;
	for (size_t i = 0; i < size; ++i) {
		if (buffer[i] != pattern_at(i))
			return false;
	}
	return true;
}

static test_thread_result TEST_THREAD_CALL abort_websocket_later_long(void *argument) {
	sleep_ms(300);
	crnet_websocket_abort((crnet_websocket)argument);
	return 0;
}

struct flush_elsewhere {
	crnet_websocket socket;
	CR_RESULT       result;
};

static test_thread_result TEST_THREAD_CALL flush_elsewhere_run(void *argument) {
	struct flush_elsewhere *flush = (struct flush_elsewhere *)argument;
	flush->result                 = crnet_websocket_flush(flush->socket, PATIENCE_MS);
	return 0;
}

static test_thread_result TEST_THREAD_CALL send_later_run(void *argument) {
	sleep_ms(100);
	crnet_websocket_send((crnet_websocket)argument, CRNET_WEBSOCKET_MESSAGE_TEXT, "from elsewhere", 14, PATIENCE_MS);
	return 0;
}

static void resumable_websocket(crnet_wait_set set) {
	char                      url[256];
	uint32_t                  status = 0;
	int                       tag    = 3;
	crnet_error             error;
	CRNET_WEBSOCKET_MESSAGE type;
	uint64_t                  received = 0;
	bool                      complete = false;

	const size_t big_size = 64u * 1024 * 1024;
	uint8_t     *big      = (uint8_t *)malloc(big_size);
	if (!big) {
		note("no 64 MiB to test with; skipped");
		return;
	}

	// The opening, held back by the server, driven with CRNET_NO_WAIT.
	crnet_websocket socket = NULL;
	crnet_websocket_create(g_instance, &socket);
	expect(crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status), CR_RESULT_NET_INVALID_WEBSOCKET, "WebSocket connect_continue before any connect");
	expect(crnet_websocket_flush(socket, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_WEBSOCKET, "flush before any connect");
	expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_INVALID_WEBSOCKET, "a WebSocket never connected isn't taken by a wait set");
	char path[64];
	snprintf(path, sizeof(path), "/slow-upgrade?ms=%u", RESUMABLE_HELD_MS);
	crnet_websocket_connect_desc desc = websocket_desc(url, sizeof(url), path, false, CRNET_NO_WAIT, NULL);
	longest_reset();
	CR_RESULT result = TIMED("crnet_websocket_connect", crnet_websocket_connect(socket, &desc, &status));
	expect(result, CR_RESULT_NET_TIMED_OUT, "connect with CRNET_NO_WAIT begins the opening and returns (the 101 held back 1 s)");
	check(status == 0, "its status is 0 meanwhile");
	expect(crnet_websocket_connect(socket, &desc, &status), CR_RESULT_NET_INVALID_WEBSOCKET, "a second connect while it is under way");
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "x", 1, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_WEBSOCKET, "send while it is under way");
	expect(crnet_wait_set_add_websocket(set, socket, 0, &tag), CR_RESULT_DESC_INVALID_PARAMETERS, "add a WebSocket asking for nothing");
	expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "a WebSocket whose opening is under way goes in a wait set");
	bool named = false;
	while (result == CR_RESULT_NET_TIMED_OUT) {
		uint32_t ready = 0;
		if (!sleep_in(set, &ready))
			break;
		named  = true;
		result = TIMED("crnet_websocket_connect_continue", crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status));
	}
	expect(result, CR_SUCCESS, "carried on with CRNET_NO_WAIT between waits on the set, it opens");
	check(named && status == 101, "the set named it once the opening had an outcome, and the status is 101 (%u)", status);
	expect(crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status), CR_RESULT_NET_INVALID_WEBSOCKET, "connect_continue after the outcome");
	expect(crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status), CR_RESULT_NET_INVALID_WEBSOCKET, "and a second time");
	longest_check("WebSocket opening held back 1 s");
	crnet_wait_set_remove_websocket(set, socket);

	// A burst sent with CRNET_NO_WAIT only, the thread sleeping for ROOM, to
	// a path that echoes nothing (echoes nobody reads would hold the
	// server's reading up) and checks each message itself.
	{
		crnet_websocket sink = NULL;
		crnet_websocket_connect_desc sink_desc = websocket_desc(url, sizeof(url), "/sink", false, PATIENCE_MS, NULL);
		crnet_websocket_create(g_instance, &sink);
		const int    count = 16;
		const size_t size  = 256 * 1024;
		for (size_t i = 0; i < size; ++i)
			big[i] = pattern_at(i);
		if (expect(crnet_websocket_connect(sink, &sink_desc, &status), CR_SUCCESS, "a second socket, to a server that takes messages and echoes none")) {
			expect(crnet_websocket_flush(sink, CRNET_NO_WAIT), CR_SUCCESS, "flush with nothing in flight");
			crnet_websocket_send(sink, CRNET_WEBSOCKET_MESSAGE_TEXT, "pause:1000", 10, PATIENCE_MS);
			expect(crnet_wait_set_add_websocket(set, sink, CRNET_WAIT_FOR_ROOM, &tag), CR_SUCCESS, "in a set for ROOM");
			longest_reset();
			int  taken     = 0;
			int  refused   = 0;
			bool room_seen = false;
			while (taken < count) {
				result = TIMED("crnet_websocket_send", crnet_websocket_send(sink, CRNET_WEBSOCKET_MESSAGE_BINARY, big, size, CRNET_NO_WAIT));
				if (result == CR_SUCCESS) {
					taken++;
					continue;
				}
				if (result != CR_RESULT_NET_TIMED_OUT)
					break;
				refused++;
				uint32_t ready = 0;
				if (!sleep_in(set, &ready))
					break;
				room_seen = room_seen || (ready & CRNET_WAIT_FOR_ROOM);
			}
			check(taken == count, "%d messages of 256 KiB sent with CRNET_NO_WAIT only (%d sends not taken, the one before still in flight)", taken, refused);
			check(refused == 0 || room_seen, "the set named the socket ROOM when the one in flight had gone");
			while ((result = TIMED("crnet_websocket_flush", crnet_websocket_flush(sink, CRNET_NO_WAIT))) == CR_RESULT_NET_TIMED_OUT) {
				uint32_t ready = 0;
				if (!sleep_in(set, &ready))
					break;
			}
			expect(result, CR_SUCCESS, "flush with CRNET_NO_WAIT, between waits, sees the last one off");
			crnet_wait_set_remove_websocket(set, sink);
			crnet_wait_set_add_websocket(set, sink, CRNET_WAIT_FOR_INCOMING, &tag);
			crnet_websocket_send(sink, CRNET_WEBSOCKET_MESSAGE_TEXT, "report", 6, CRNET_NO_WAIT);
			char report[128] = "";
			size_t got = 0;
			for (;;) {
				result = TIMED("crnet_websocket_recv", crnet_websocket_recv(sink, report + got, sizeof(report) - 1 - got, CRNET_NO_WAIT, &received, &type, &complete));
				if (result == CR_RESULT_NET_TIMED_OUT) {
					uint32_t ready = 0;
					if (!sleep_in(set, &ready))
						break;
					continue;
				}
				if (CR_FAILED(result))
					break;
				got += (size_t)received;
				if (complete || got == sizeof(report) - 1)
					break;
			}
			report[got] = '\0';
			char want[128];
			snprintf(want, sizeof(want), "messages=%d bytes=%zu bad=0", count, (size_t)count * size);
			check(strcmp(report, want) == 0, "the server got all %d, whole and as sent (\"%s\"), its report received with CRNET_NO_WAIT only", count, report);
			longest_check("WebSocket burst");
			crnet_wait_set_remove_websocket(set, sink);
		}
		crnet_websocket_destroy(sink);
	}

	// A 64 MiB message, taken at once and in flight while the server isn't
	// reading: what is refused and what still works meanwhile.
	{
		for (size_t i = 0; i < big_size; ++i)
			big[i] = pattern_at(i);
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "pause:1500", 10, PATIENCE_MS);
		expect(crnet_websocket_flush(socket, PATIENCE_MS), CR_SUCCESS, "the server told to stop reading");
		uint64_t before = now_ms();
		result          = crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_BINARY, big, big_size, CRNET_NO_WAIT);
		uint64_t took   = now_ms() - before;
		expect(result, CR_SUCCESS, "a 64 MiB message sent with CRNET_NO_WAIT is taken");
		note("taking it took %llu ms", (unsigned long long)took);
		memset(big, 0, big_size); // the caller's own again: what arrives is the copy
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "after", 5, 1), CR_RESULT_NET_TIMED_OUT, "the next send with a 1 ms limit isn't taken");
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "after", 5, CRNET_NO_WAIT), CR_RESULT_NET_TIMED_OUT, "nor with CRNET_NO_WAIT");
		expect(crnet_websocket_flush(socket, CRNET_NO_WAIT), CR_RESULT_NET_TIMED_OUT, "flush with CRNET_NO_WAIT: still in flight");
		expect(crnet_websocket_close(socket, 1000, NULL), CR_RESULT_NET_TIMED_OUT, "close with a message in flight sends nothing");
		expect(crnet_websocket_recv(socket, big, 16, CRNET_NO_WAIT, &received, &type, &complete), CR_RESULT_NET_TIMED_OUT, "and recv still works: the socket isn't closed");
		// How long a message stays in flight is the stack's: NSURLSession
		// calls one gone once its connection has taken it, whatever the
		// peer reads. So the set is checked for naming it, and the time is
		// put on record.
		expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_ROOM, &tag), CR_SUCCESS, "in a set for ROOM, with the message in flight");
		uint32_t ready = 0;
		before         = now_ms();
		check(sleep_in(set, &ready) && (ready & CRNET_WAIT_FOR_ROOM), "the set names it ROOM once the message has gone");
		note("the 64 MiB message was in flight about %llu ms more", (unsigned long long)(now_ms() - before));
		crnet_wait_set_remove_websocket(set, socket);
		expect(crnet_websocket_flush(socket, PATIENCE_MS), CR_SUCCESS, "flush says it has gone");
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "after", 5, PATIENCE_MS), CR_SUCCESS, "the same message sent again is taken");
		check(websocket_message_is(socket, big, big_size + 1, big_size, NULL), "the 64 MiB echo is whole and in order: the bytes taken, not the buffer overwritten after");
		check(websocket_message_is(socket, big, big_size + 1, 0, "after"), "and the message after it comes after it");
	}

	// A ping from the server while a large message is part sent.
	{
		for (size_t i = 0; i < 16u * 1024 * 1024; ++i)
			big[i] = pattern_at(i);
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "pause-and-ping:1000", 19, PATIENCE_MS);
		crnet_websocket_flush(socket, PATIENCE_MS);
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_BINARY, big, 16u * 1024 * 1024, CRNET_NO_WAIT), CR_SUCCESS, "a 16 MiB message taken while the server pings");
		expect(crnet_websocket_flush(socket, PATIENCE_MS), CR_SUCCESS, "and seen off");
		check(websocket_message_is(socket, big, big_size + 1, 16u * 1024 * 1024, NULL), "its echo is whole: the ping didn't break the frame");
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "alive", 5, PATIENCE_MS);
		check(websocket_message_is(socket, big, big_size + 1, 0, "alive"), "and the connection goes on");
	}

	// Two threads: a recv while another thread flushes, and a wait on a set
	// while another thread sends on its member.
	{
		for (size_t i = 0; i < 16u * 1024 * 1024; ++i)
			big[i] = pattern_at(i);
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "pause:300", 9, PATIENCE_MS);
		crnet_websocket_flush(socket, PATIENCE_MS);
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_BINARY, big, 16u * 1024 * 1024, CRNET_NO_WAIT);
		struct flush_elsewhere flush  = {socket, CR_RESULT_GENERAL_FAILURE};
		test_thread            thread = thread_start(flush_elsewhere_run, &flush);
		bool                   echoed = websocket_message_is(socket, big + 16u * 1024 * 1024, big_size - 16u * 1024 * 1024, 16u * 1024 * 1024, NULL);
		thread_join(thread);
		check(echoed && flush.result == CR_SUCCESS, "a recv on one thread while another flushes (%s)", cr_result_str(flush.result));

		expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_SUCCESS, "in a set for INCOMING");
		thread         = thread_start(send_later_run, socket);
		uint32_t ready = 0;
		bool     woke  = sleep_in(set, &ready);
		thread_join(thread);
		check(woke && (ready & CRNET_WAIT_FOR_INCOMING), "a wait on the set while another thread sends on its member");
		crnet_wait_set_remove_websocket(set, socket);
		check(websocket_message_is(socket, big, big_size + 1, 0, "from elsewhere"), "and the echo is there");
	}

	// Sent and then flushed with a limit: today's "sent".
	expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "flushed", 7, CRNET_NO_WAIT), CR_SUCCESS, "send with CRNET_NO_WAIT");
	expect(crnet_websocket_flush(socket, PATIENCE_MS), CR_SUCCESS, "then flush with a limit");
	check(websocket_message_is(socket, big, big_size + 1, 0, "flushed"), "and it arrives");

	// Text that isn't UTF-8 is refused before anything is taken.
	{
		static const uint8_t not_utf8[] = {0xC3, 0x28};
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, not_utf8, sizeof(not_utf8), CRNET_NO_WAIT), CR_RESULT_NET_SEND_FAILED, "text that isn't UTF-8 is refused");
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "fine", 4, CRNET_NO_WAIT), CR_SUCCESS, "and the connection is as it was");
		check(websocket_message_is(socket, big, big_size + 1, 0, "fine"), "its echo arrives");
	}
	crnet_websocket_destroy(socket);

	// Destroyed with a message in flight: dropped, nothing left behind.
	socket = NULL;
	desc   = websocket_desc(url, sizeof(url), "/echo", false, PATIENCE_MS, NULL);
	crnet_websocket_create(g_instance, &socket);
	if (expect(crnet_websocket_connect(socket, &desc, &status), CR_SUCCESS, "connect with a limit, as before")) {
		crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "pause:500", 9, PATIENCE_MS);
		crnet_websocket_flush(socket, PATIENCE_MS);
		expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_BINARY, big, 16u * 1024 * 1024, CRNET_NO_WAIT), CR_SUCCESS, "a message taken");
	}
	crnet_websocket_destroy(socket);
	note("destroyed at once after it: the message in flight dropped (no check; the sanitizers watch it)");

	// Refusals, reported by the continuing call.
	{
		socket = NULL;
		desc   = websocket_desc(url, sizeof(url), "/refuse", false, CRNET_NO_WAIT, NULL);
		crnet_websocket_create(g_instance, &socket);
		result = crnet_websocket_connect(socket, &desc, &status);
		if (result == CR_RESULT_NET_TIMED_OUT)
			result = crnet_websocket_connect_continue(socket, PATIENCE_MS, &status);
		check(result == CR_RESULT_NET_WS_UPGRADE_REFUSED && status == 401, "an opening refused with 401, reported by connect_continue: %s (%u)", cr_result_str(result), status);
		expect(crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status), CR_RESULT_NET_INVALID_WEBSOCKET, "connect_continue after a refusal");
		expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_INCOMING, &tag), CR_RESULT_NET_INVALID_WEBSOCKET, "a WebSocket whose connect failed isn't taken by a wait set");
		crnet_websocket_destroy(socket);

		socket = NULL;
		desc   = websocket_desc(url, sizeof(url), "/echo", true, CRNET_NO_WAIT, NULL);
		crnet_websocket_create(g_instance, &socket);
		result = crnet_websocket_connect(socket, &desc, &status);
		if (result == CR_RESULT_NET_TIMED_OUT) {
			expect(crnet_wait_set_add_websocket(set, socket, CRNET_WAIT_FOR_ROOM, &tag), CR_SUCCESS, "a wss opening under way in a set");
			uint32_t ready = 0;
			check(sleep_in(set, &ready), "named once its certificate is turned away");
			crnet_wait_set_remove_websocket(set, socket);
			result = crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status);
		}
		expect(result, CR_RESULT_NET_TLS_FAILED, "a wss certificate turned away, reported by connect_continue");
		crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
		check(error.result == CR_RESULT_NET_TLS_FAILED && error.source != CR_PLATFORM_ERROR_SOURCE_NONE, "with the reason on the receiving side");
		note_error("wss turned away, continued", &error);
		crnet_websocket_destroy(socket);
	}

	// Aborts from another thread during each step under way.
	{
		socket = NULL;
		snprintf(path, sizeof(path), "/slow-upgrade?ms=3000");
		desc = websocket_desc(url, sizeof(url), path, false, CRNET_NO_WAIT, NULL);
		crnet_websocket_create(g_instance, &socket);
		crnet_websocket_connect(socket, &desc, &status);
		test_thread thread = thread_start(abort_websocket_later_long, socket);
		uint64_t    before = now_ms();
		result             = crnet_websocket_connect_continue(socket, CRNET_WAIT_FOREVER, &status);
		uint64_t waited    = now_ms() - before;
		thread_join(thread);
		check(result == CR_RESULT_NET_ABORTED && waited < 2500, "connect_continue waiting forever, aborted from another thread: %s (%llu ms)", cr_result_str(result),
		      (unsigned long long)waited);
		expect(crnet_websocket_connect_continue(socket, CRNET_NO_WAIT, &status), CR_RESULT_NET_ABORTED, "and again: ABORTED");
		crnet_websocket_destroy(socket);

		socket = NULL;
		desc   = websocket_desc(url, sizeof(url), "/echo", false, PATIENCE_MS, NULL);
		crnet_websocket_create(g_instance, &socket);
		if (!CR_FAILED(crnet_websocket_connect(socket, &desc, &status))) {
			crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "pause:3000", 10, PATIENCE_MS);
			crnet_websocket_flush(socket, PATIENCE_MS);
			crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_BINARY, big, big_size, CRNET_NO_WAIT);
			thread = thread_start(abort_websocket_later_long, socket);
			before = now_ms();
			result = crnet_websocket_flush(socket, CRNET_WAIT_FOREVER);
			waited = now_ms() - before;
			thread_join(thread);
			if (result == CR_SUCCESS && waited < 300) {
				// The stack's connection had taken it before the abort came.
				note("the 64 MiB message had gone before the abort (%llu ms): this stack's connection takes it whole", (unsigned long long)waited);
			} else {
				check(result == CR_RESULT_NET_ABORTED && waited < 2500, "flush waiting forever on a 64 MiB message, aborted from another thread: %s (%llu ms)", cr_result_str(result),
				      (unsigned long long)waited);
			}
			expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, "x", 1, CRNET_NO_WAIT), CR_RESULT_NET_ABORTED, "and send after it: ABORTED");
			expect(crnet_websocket_flush(socket, CRNET_NO_WAIT), CR_RESULT_NET_ABORTED, "and flush: ABORTED");
		}
		crnet_websocket_destroy(socket);
	}
	free(big);
}

#endif // CRNET_EXT_WEBSOCKET

static void test_resumable(void) {
	part("Resumable starts: steps begun with CRNET_NO_WAIT, carried on between waits on a wait set");

	expect(crnet_websocket_flush(NULL, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_WEBSOCKET, "flush on NULL");
	expect(crnet_websocket_connect_continue(NULL, CRNET_NO_WAIT, NULL), CR_RESULT_NET_INVALID_WEBSOCKET, "WebSocket connect_continue on NULL");
#if CRNET_EXT_TCP
	expect(crnet_tcp_socket_connect_continue(NULL, CRNET_NO_WAIT), CR_RESULT_NET_INVALID_TCP_SOCKET, "TCP connect_continue on NULL");
#endif
	expect(crnet_wait_set_add_http_transfer(NULL, NULL, CRNET_WAIT_FOR_INCOMING, NULL), CR_RESULT_NET_INVALID_WAIT_SET, "add a transfer to NULL");
	crnet_wait_set_remove_http_transfer(NULL, NULL);

	crnet_wait_set set = NULL, other_set = NULL;
	crnet_error    error;
	if (!expect(crnet_wait_set_create(g_instance, &set, &error), CR_SUCCESS, "a wait set to sleep in") ||
	    !expect(crnet_wait_set_create(g_instance, &other_set, &error), CR_SUCCESS, "and another")) {
		crnet_wait_set_destroy(set);
		return;
	}
	expect(crnet_wait_set_add_http_transfer(set, NULL, CRNET_WAIT_FOR_INCOMING, NULL), CR_RESULT_NET_INVALID_HTTP_TRANSFER, "add a NULL transfer");
#if CRNET_EXT_TCP
	resumable_tcp(set);
#endif
	resumable_http(set, other_set);
#if CRNET_EXT_WEBSOCKET
	resumable_websocket(set);
#endif
	crnet_wait_set_destroy(other_set);
	crnet_wait_set_destroy(set);
}

// --------------------------------------------------------------------------

// Every based desc (CR_STRUCT_BASE, crcore.h) is refused, before
// anything else of it is read, when its type isn't the call's or something
// is chained on: nothing can be chained onto a Carrottus Net desc yet.
// --------------------------------------------------------------------------
// The internet (opt-in: run only as the "internet" part)
// --------------------------------------------------------------------------

/* GET [url] on a client with a zeroed TLS desc (the system's trust) and
 * proxy desc (the system's proxy). The body is read whole into [body]
 * (cut to [capacity]); [out_status] the status. */
static CR_RESULT internet_get(const char *url, uint32_t timeout_ms, char *body, size_t capacity, uint32_t *out_status, crnet_error *out_error) {
	*out_status = 0;
	body[0]     = '\0';
	memset(out_error, 0, sizeof(*out_error));
	crnet_http_client_desc client_desc;
	memset(&client_desc, 0, sizeof(client_desc));
	client_desc.base.type    = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	crnet_http_client client = NULL;
	CR_RESULT result        = crnet_http_client_create(g_instance, &client_desc, &client);
	if (CR_FAILED(result))
		return result;
	crnet_http_transfer transfer = NULL;
	result                         = crnet_http_transfer_create(client, &transfer);
	if (!CR_FAILED(result)) {
		crnet_http_transfer_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type        = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
		desc.method           = "GET";
		desc.url              = url;
		desc.follow_redirects = true;
		desc.timeout_ms       = timeout_ms;
		result                = crnet_http_transfer_start(transfer, &desc);
		if (!CR_FAILED(result))
			result = crnet_http_transfer_receive_response(transfer, timeout_ms);
		if (!CR_FAILED(result)) {
			*out_status = crnet_http_transfer_get_status(transfer);
			size_t size = 0;
			for (;;) {
				uint64_t received = 0;
				if (size + 1 >= capacity) {
					char discard[4096];
					result = crnet_http_transfer_recv(transfer, discard, sizeof(discard), timeout_ms, &received);
				} else {
					result = crnet_http_transfer_recv(transfer, body + size, capacity - 1 - size, timeout_ms, &received);
					size += (size_t)received;
				}
				if (CR_FAILED(result) || received == 0)
					break;
			}
			body[size] = '\0';
		}
		if (CR_FAILED(result))
			crnet_http_transfer_get_error(transfer, out_error);
		crnet_http_transfer_destroy(transfer);
	}
	crnet_http_client_destroy(client);
	return result;
}

static void test_internet(void) {
	part("The internet: real DNS, certificate chains, a WebSocket server, a host that never answers");
	static char body[262144];
	uint32_t status = 0;
	crnet_error error;

#if CRNET_EXT_TCP
	// Real name lookup, through a DNS server.
	{
		char addresses[16][CRNET_MAX_ADDRESS_STRING_LEN];
		uint32_t count = 0;
		crnet_resolver_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type            = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
		desc.host                 = "example.com";
		desc.family               = CRNET_ADDRESS_FAMILY_ANY;
		crnet_resolver resolver = NULL;
		if (expect(crnet_resolver_create(g_instance, &desc, &resolver), CR_SUCCESS, "a resolver for example.com")) {
			test_thread thread = thread_start(run_resolver, resolver);
			if (expect(crnet_resolver_wait(resolver, 15000, addresses, 16, &count), CR_SUCCESS, "example.com, looked up by a DNS server")) {
				check(count >= 1, "it has %u address%s", count, count == 1 ? "" : "es");
				for (uint32_t i = 0; i < count && i < 16; ++i)
					note("example.com is %s", addresses[i]);
			}
			crnet_resolver_destroy(resolver);
			thread_join(thread);
		}
	}
#endif

	// An ordinary https:// site: a certificate from a public authority,
	// through intermediates, checked against the system's trust store.
	CR_RESULT r = internet_get("https://example.com/", 20000, body, sizeof(body), &status, &error);
	if (expect(r, CR_SUCCESS, "https://example.com/ with a zeroed TLS desc")) {
		check(status == 200, "status %u", (unsigned)status);
		check(strstr(body, "Example Domain") != NULL, "and its page");
	} else {
		note_error("example.com", &error);
	}

	// Certificates that must be turned away with the system's trust.
	const char *bad[][2] = {
		{"https://expired.badssl.com/", "an expired certificate"},
		{"https://self-signed.badssl.com/", "a self-signed certificate"},
		{"https://wrong.host.badssl.com/", "a certificate for another host"},
		{"https://untrusted-root.badssl.com/", "a chain to a root nobody trusts"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
		char what[128];
		snprintf(what, sizeof(what), "%s is refused", bad[i][1]);
		r = internet_get(bad[i][0], 20000, body, sizeof(body), &status, &error);
		expect(r, CR_RESULT_NET_TLS_FAILED, what);
		note_error(bad[i][1], &error);
	}

	// A redirect, followed: http:// to https://.
	r = internet_get("http://github.com/", 20000, body, sizeof(body), &status, &error);
	if (expect(r, CR_SUCCESS, "http://github.com/, which redirects to https://")) {
		check(status == 200, "status %u after the redirect", (unsigned)status);
	} else {
		note_error("github.com", &error);
	}

#if CRNET_EXT_WEBSOCKET
	// A real WebSocket server, over TLS with the system's trust. This one
	// sends a greeting first, then echoes.
	{
		crnet_websocket_connect_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type          = CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC;
		desc.url                = "wss://echo.websocket.org/";
		desc.timeout_ms         = 20000;
		desc.max_message_size   = 1 << 20;
		desc.follow_redirects   = false;
		crnet_websocket socket = NULL;
		uint32_t opening        = 0;
		if (expect(crnet_websocket_create(g_instance, &socket), CR_SUCCESS, "a WebSocket")) {
			if (expect(crnet_websocket_connect(socket, &desc, &opening), CR_SUCCESS, "connect to wss://echo.websocket.org/")) {
				const char *text = "crnet_test over the internet";
				expect(crnet_websocket_send(socket, CRNET_WEBSOCKET_MESSAGE_TEXT, text, strlen(text), 20000), CR_SUCCESS, "send a text message");
				bool echoed = false;
				for (int i = 0; i < 4 && !echoed; ++i) {
					uint8_t message[4096];
					size_t size = 0;
					CRNET_WEBSOCKET_MESSAGE type;
					if (CR_FAILED(websocket_read_message(socket, message, sizeof(message) - 1, &size, &type, 20000)))
						break;
					message[size] = '\0';
					echoed        = type == CRNET_WEBSOCKET_MESSAGE_TEXT && strcmp((const char *)message, text) == 0;
					if (!echoed)
						note("the server said: %.80s", (const char *)message);
				}
				check(echoed, "and get it back");
				expect(crnet_websocket_close(socket, 1000, "done"), CR_SUCCESS, "close with 1000");
			} else {
				crnet_websocket_get_error(socket, CRNET_ERROR_SIDE_RECEIVING, &error);
				note_error("echo.websocket.org", &error);
			}
			crnet_websocket_destroy(socket);
		}
	}
#endif

#if CRNET_EXT_TCP
	// A host that swallows packets: a private address nothing answers on.
	// The caller's time limit is what ends the wait.
	{
		crnet_tcp_socket socket = NULL;
		if (expect(crnet_tcp_socket_create(g_instance, &socket), CR_SUCCESS, "a socket for a silent host")) {
			crnet_tcp_socket_connect_desc desc;
			memset(&desc, 0, sizeof(desc));
			desc.base.type       = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
			desc.address         = "10.255.255.1";
			desc.port            = 80;
			desc.timeout_ms      = 2000;
			const uint64_t start = now_ms();
			r                    = crnet_tcp_socket_connect(socket, &desc);
			const uint64_t took  = now_ms() - start;
			if (r == CR_RESULT_NET_CONNECT_FAILED) {
				note("10.255.255.1 is refused or unreachable from here (%llu ms), not silent; the time limit isn't tried", (unsigned long long)took);
			} else {
				expect(r, CR_RESULT_NET_TIMED_OUT, "TCP connect to a silent host, 2 s allowed");
				check(took >= 1800 && took < 4000, "it waited about as long as allowed (%llu ms)", (unsigned long long)took);
			}
			crnet_tcp_socket_destroy(socket);
		}
	}
#endif
	{
		const uint64_t start = now_ms();
		r                    = internet_get("http://10.255.255.1/", 2000, body, sizeof(body), &status, &error);
		const uint64_t took  = now_ms() - start;
		if (r == CR_RESULT_NET_CONNECT_FAILED) {
			note("HTTP to 10.255.255.1 is refused or unreachable from here (%llu ms)", (unsigned long long)took);
		} else {
			expect(r, CR_RESULT_NET_TIMED_OUT, "HTTP to a silent host, 2 s allowed");
			check(took >= 1800 && took < 4000, "it waited about as long as allowed (%llu ms)", (unsigned long long)took);
		}
		note_error("silent host over HTTP", &error);
	}
#if CRNET_EXT_TCP
	// A connect left going: begun with CRNET_NO_WAIT to TEST-NET-1, which
	// never answers (loopback can't hold one on every system), carried on
	// several times, then aborted.
	{
		crnet_tcp_socket socket = NULL;
		crnet_tcp_socket_create(g_instance, &socket);
		crnet_tcp_socket_connect_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type  = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
		desc.address    = "192.0.2.1";
		desc.port       = 80;
		desc.timeout_ms = CRNET_NO_WAIT;
		CR_RESULT r    = crnet_tcp_socket_connect(socket, &desc);
		if (r == CR_RESULT_NET_CONNECT_FAILED) {
			note("192.0.2.1 is refused or unreachable from here, not silent; skipped");
		} else {
			expect(r, CR_RESULT_NET_TIMED_OUT, "a TCP connect to a silent host with CRNET_NO_WAIT is left going");
			bool still = true;
			for (int i = 0; i < 3 && still; ++i)
				still = crnet_tcp_socket_connect_continue(socket, 100) == CR_RESULT_NET_TIMED_OUT;
			check(still, "still TIMED_OUT after three continues of 100 ms each");
			crnet_tcp_socket_abort(socket);
			expect(crnet_tcp_socket_connect_continue(socket, PATIENCE_MS), CR_RESULT_NET_ABORTED, "and aborted: the continue says ABORTED");
		}
		crnet_tcp_socket_destroy(socket);
	}
#endif
}

static void test_struct_bases(void) {
	part("Struct bases");

	const CR_STRUCT_BASE other = {CR_STRUCT_TYPE_NET_RESOLVER_DESC, NULL};
#if CRNET_EXT_TCP
	{
		crnet_resolver_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.host               = "localhost";
		crnet_resolver resolver = NULL;
		expect(crnet_resolver_create(g_instance, &desc, &resolver), CR_RESULT_STRUCT_WRONG_TYPE, "resolver: a zeroed base");
		desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
		expect(crnet_resolver_create(g_instance, &desc, &resolver), CR_RESULT_STRUCT_WRONG_TYPE, "resolver: another desc's type");
		desc.base.type = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
		desc.base.next = &other;
		expect(crnet_resolver_create(g_instance, &desc, &resolver), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "resolver: something chained on");
		check(resolver == NULL, "resolver: none made");
	}
	{
		crnet_tcp_listener_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.address                  = "127.0.0.1";
		crnet_tcp_listener listener = NULL;
		expect(crnet_tcp_listener_create(g_instance, &desc, &listener, NULL), CR_RESULT_STRUCT_WRONG_TYPE, "listener: a zeroed base");
		desc.base.type = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
		desc.base.next = &other;
		expect(crnet_tcp_listener_create(g_instance, &desc, &listener, NULL), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "listener: something chained on");
	}
	{
		crnet_tcp_socket socket = NULL;
		expect(crnet_tcp_socket_create(g_instance, &socket), CR_SUCCESS, "a socket to connect");
		crnet_tcp_socket_connect_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.address = "127.0.0.1";
		expect(crnet_tcp_socket_connect(socket, &desc), CR_RESULT_STRUCT_WRONG_TYPE, "connect: a zeroed base");
		desc.base.type = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
		desc.base.next = &other;
		expect(crnet_tcp_socket_connect(socket, &desc), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "connect: something chained on");
		crnet_tcp_socket_destroy(socket);
	}
#endif
	crnet_http_client_desc client_desc;
	memset(&client_desc, 0, sizeof(client_desc));
	crnet_http_client client = NULL;
	expect(crnet_http_client_create(g_instance, &client_desc, &client), CR_RESULT_STRUCT_WRONG_TYPE, "HTTP client: a zeroed base");
	client_desc.base.type = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	client_desc.base.next = &other;
	expect(crnet_http_client_create(g_instance, &client_desc, &client), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "HTTP client: something chained on");
	client_desc.base.next = NULL;
	expect(crnet_http_client_create(g_instance, &client_desc, &client), CR_SUCCESS, "HTTP client: its own base");
	crnet_http_transfer transfer = NULL;
	expect(crnet_http_transfer_create(client, &transfer), CR_SUCCESS, "a transfer to start");
	crnet_http_transfer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.method = "GET";
	desc.url    = "http://127.0.0.1:9/";
	expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_STRUCT_WRONG_TYPE, "transfer: a zeroed base");
	desc.base.type = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
	desc.base.next = &other;
	expect(crnet_http_transfer_start(transfer, &desc), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "transfer: something chained on");
	crnet_http_transfer_destroy(transfer);
	crnet_http_client_destroy(client);
#if CRNET_EXT_WEBSOCKET
	{
		crnet_websocket socket = NULL;
		expect(crnet_websocket_create(g_instance, &socket), CR_SUCCESS, "a WebSocket to connect");
		crnet_websocket_connect_desc ws_desc;
		memset(&ws_desc, 0, sizeof(ws_desc));
		ws_desc.url              = "ws://127.0.0.1:9/";
		ws_desc.max_message_size = 1024;
		expect(crnet_websocket_connect(socket, &ws_desc, NULL), CR_RESULT_STRUCT_WRONG_TYPE, "WebSocket: a zeroed base");
		ws_desc.base.type = CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC;
		ws_desc.base.next = &other;
		expect(crnet_websocket_connect(socket, &ws_desc, NULL), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "WebSocket: something chained on");
		crnet_websocket_destroy(socket);
	}
#endif
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: crnet_test <info file> [instance|bases|records|tcp|wait|resolver|http|tls|proxy|websocket|resumable|internet]\n");
		return 2;
	}
	if (!read_info(argv[1])) {
		fprintf(stderr, "crnet_test: couldn't read the server's info file '%s'\n", argv[1]);
		return 2;
	}
	const char *only = argc > 2 ? argv[2] : NULL;

	g_system                         = cr_allocator_system();
	const crnet_instance_desc desc = instance_desc(&g_system);
	crnet_error               error;
	if (CR_FAILED(crnet_instance_create(&desc, &g_instance, &error))) {
		fprintf(stderr, "crnet_test: crnet_instance_create failed (result %d, source %d, code %lld) %s\n", (int)error.result, (int)error.source, (long long)error.code, error.text);
		return 2;
	}

#define RUN(name, function) \
	if (!only || strcmp(only, name) == 0) \
		function()
	// Needs the internet, so only when asked for by name.
	if (only && strcmp(only, "internet") == 0)
		test_internet();
	RUN("instance", test_instance);
	RUN("bases", test_struct_bases);
	RUN("records", test_records);
#if CRNET_EXT_TCP
	RUN("tcp", test_tcp);
	RUN("wait", test_wait_set);
	RUN("resolver", test_resolver);
#endif
	RUN("http", test_http);
	RUN("tls", test_tls);
	RUN("proxy", test_proxy);
#if CRNET_EXT_WEBSOCKET
	RUN("websocket", test_websocket);
#endif
	RUN("resumable", test_resumable);

	crnet_instance_destroy(g_instance);
	printf("\n%d passed, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
