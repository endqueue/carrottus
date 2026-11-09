
#if SPUDLIB_PLATFORM_WIN32

/*
 * SpudNet TCP sockets on Winsock2, and SpudNet's startup for Windows.
 *
 * Every socket here is non-blocking, and that is an internal matter:
 * spudnet.h has no non-blocking mode. A call tries the operation, and if
 * Winsock says it would have to wait, waits itself - for as long as the
 * caller's time limit allows, and alongside the abort event - then tries
 * again. That one wait is what makes the time limit mean the same thing as
 * on the other backends and what lets another thread end it.
 *
 * Winsock ties a socket to a single event for all its activity
 * (WSAEventSelect), and a receiver and a sender may be waiting on one
 * socket at once. So whichever thread wakes on the socket's event asks
 * Winsock what happened and passes it on through two plain events, one per
 * direction; each waiter resets its own direction's event before trying
 * its operation, so news that arrives between the try and the wait is
 * still there to wake it.
 *
 * The resolver is here too, at the end, on Winsock's GetAddrInfoExW - the
 * one system resolver of the three that can be told to stop.
 */

#include "../../spudnetshared.h"
#include "spudnet.h"
#include "../../../spudstructbase.h"

/* winsock2.h must come before any transitive <windows.h> pull-in (PCH or
 * otherwise) — <windows.h> alone drags in the legacy winsock.h and the two
 * headers conflict. */
#include <winsock2.h>
#include <ws2tcpip.h>

/* The two calls that make a GetAddrInfoExW lookup cancellable are in the
 * Windows SDK's ws2tcpip.h and in ws2_32 itself, but mingw-w64's copy of
 * the header leaves them out. Declared here for that toolchain only, as the
 * SDK declares them. */
#if defined(__MINGW32__)
WINSOCK_API_LINKAGE INT WSAAPI GetAddrInfoExCancel(LPHANDLE lpHandle);
WINSOCK_API_LINKAGE INT WSAAPI GetAddrInfoExOverlappedResult(LPOVERLAPPED lpOverlapped);
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* One start of Winsock. Winsock counts them and stays up until as many
 * WSACleanup calls have been made, and both calls may come from any thread
 * at any time, which is everything an instance needs. WinHTTP, which
 * carries HTTP and WebSocket here, needs no start of its own. */
static SPUDRESULT spudnet_win_start(spudnet_error *out_error) {
	WSADATA wsa_data;
	// Returns its error rather than leaving it for WSAGetLastError.
	int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (result != 0)
		return spudnet_error_record(out_error, SPUDRESULT_SPUDNET_STARTUP_FAILED, SPUDNET_ERROR_SOURCE_WIN32, result, NULL);
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_instance_create(
    const spudnet_instance_desc *desc,
    spudnet_instance *out_instance,
    spudnet_error *out_error) {
	spudnet_error_clear(out_error);
	const SPUDRESULT desc_result = spudnet_instance_desc_check(desc, out_instance);
	if (desc_result != SPUD_SUCCESS)
		return desc_result;

	struct spudnet_instance_t *instance = SPUD_NEW_ZEROED(desc->allocator, struct spudnet_instance_t);
	if (!instance)
		return SPUDRESULT_OUT_OF_MEMORY;
	instance->head.allocator = *desc->allocator;
	SPUD_BIRTH(instance);
	SPUD_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);
	SPUDRESULT result        = spudnet_win_start(out_error);
	if (SPUDFAIL(result)) {
		SPUD_DELETE(desc->allocator, struct spudnet_instance_t, instance);
		return result;
	}
	*out_instance = instance;
	return SPUD_SUCCESS;
}

void spudnet_instance_destroy(spudnet_instance instance) {
	if (!instance)
		return;
	if (SPUD_STALE(instance, "spudnet_instance_destroy", "instance"))
		return;
	WSACleanup();
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	SPUD_QUARANTINE_RELEASE(&instance->quarantine);
	spud_string_free(&allocator, instance->head.debug_name);
	SPUD_DELETE(&allocator, struct spudnet_instance_t, instance);
}

// --------------------------------------------------------------------------
// Shared by listeners and sockets
// --------------------------------------------------------------------------

/* One Winsock socket and what waiting on it takes. See the top of the
 * file for why there are three events besides abort's. */
struct spudnet_win_endpoint {
	SOCKET   handle;         // INVALID_SOCKET until there is one
	WSAEVENT network_event;  // Winsock sets it for any activity on `handle`
	HANDLE   readable_event; // something to read, accept, or a close
	HANDLE   writable_event; // room to write, a connect finished, or a close
	HANDLE   abort_event;    // set by abort and never reset
	volatile LONG aborted;
};

static void spudnet_win_endpoint_free(struct spudnet_win_endpoint *endpoint) {
	if (endpoint->handle != INVALID_SOCKET)
		closesocket(endpoint->handle);
	if (endpoint->network_event != WSA_INVALID_EVENT)
		WSACloseEvent(endpoint->network_event);
	if (endpoint->readable_event)
		CloseHandle(endpoint->readable_event);
	if (endpoint->writable_event)
		CloseHandle(endpoint->writable_event);
	if (endpoint->abort_event)
		CloseHandle(endpoint->abort_event);
}

static bool spudnet_win_endpoint_init(struct spudnet_win_endpoint *endpoint) {
	endpoint->handle         = INVALID_SOCKET;
	endpoint->aborted        = 0;
	endpoint->network_event  = WSACreateEvent();
	endpoint->readable_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	endpoint->writable_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	endpoint->abort_event    = CreateEventW(NULL, TRUE, FALSE, NULL);
	return endpoint->network_event != WSA_INVALID_EVENT && endpoint->readable_event && endpoint->writable_event && endpoint->abort_event;
}

/* Gives the endpoint its socket and has Winsock report `events` on it,
 * which also makes the socket non-blocking. Takes the socket over: it is
 * the endpoint's to close from here, whether this succeeds or not. */
static bool spudnet_win_endpoint_attach(struct spudnet_win_endpoint *endpoint, SOCKET handle, long events) {
	endpoint->handle = handle;
	return handle != INVALID_SOCKET && WSAEventSelect(handle, endpoint->network_event, events) == 0;
}

static void spudnet_win_endpoint_abort(struct spudnet_win_endpoint *endpoint) {
	InterlockedExchange(&endpoint->aborted, 1);
	SetEvent(endpoint->abort_event);
}

static bool spudnet_win_endpoint_aborted(struct spudnet_win_endpoint *endpoint) {
	return InterlockedCompareExchange(&endpoint->aborted, 0, 0) != 0;
}

/* Milliseconds left of `timeout_ms` since `start`, for a Win32 wait. */
static DWORD spudnet_win_socket_remaining(ULONGLONG start, uint32_t timeout_ms) {
	if (timeout_ms == SPUDNET_WAIT_FOREVER)
		return INFINITE;
	ULONGLONG elapsed = GetTickCount64() - start;
	if (elapsed >= timeout_ms)
		return 0;
	ULONGLONG left = timeout_ms - elapsed;
	return left >= INFINITE ? INFINITE - 1 : (DWORD)left;
}

/* Waits for news in one direction, for the time to run out, or for an
 * abort. The caller reset `own_event` (the endpoint's readable or writable
 * event) before the attempt that came back "would block". SPUD_SUCCESS
 * means it is worth trying the operation again. */
static SPUDRESULT spudnet_win_endpoint_wait(struct spudnet_win_endpoint *endpoint, HANDLE own_event, ULONGLONG start, uint32_t timeout_ms) {
	for (;;) {
		// Abort is listed first so it wins when several are set.
		HANDLE waits[3] = {endpoint->abort_event, own_event, endpoint->network_event};
		DWORD  waited   = WaitForMultipleObjects(3, waits, FALSE, spudnet_win_socket_remaining(start, timeout_ms));
		if (waited == WAIT_OBJECT_0)
			return SPUDRESULT_SPUDNET_ABORTED;
		if (waited == WAIT_OBJECT_0 + 1)
			return SPUD_SUCCESS;
		if (waited != WAIT_OBJECT_0 + 2)
			return SPUDRESULT_SPUDNET_TIMED_OUT;

		// Something happened on the socket. Asking what also resets the
		// network event; the answer is passed on to both directions, since
		// the other one's waiter may be the one it was for.
		WSANETWORKEVENTS happened;
		memset(&happened, 0, sizeof(happened));
		if (WSAEnumNetworkEvents(endpoint->handle, endpoint->network_event, &happened) != 0)
			return SPUD_SUCCESS; // the operation will say what is wrong
		if (happened.lNetworkEvents & (FD_READ | FD_ACCEPT | FD_CLOSE))
			SetEvent(endpoint->readable_event);
		if (happened.lNetworkEvents & (FD_WRITE | FD_CONNECT | FD_CLOSE))
			SetEvent(endpoint->writable_event);
	}
}

/* A failed Winsock call as a record: `error` is what WSAGetLastError said
 * after it. Returns `result`. */
static SPUDRESULT spudnet_win_socket_failed(spudnet_error *record, SPUDRESULT result, int error) {
	return spudnet_error_record(record, result, SPUDNET_ERROR_SOURCE_WIN32, error, NULL);
}

/* The same for a call on a connection, where one error is a result of its
 * own: the system stopped waiting for the other end - a connect nobody
 * answered, a peer that went silent - with time still left on the caller's
 * limit. `otherwise` is the result for every other error. */
static SPUDRESULT spudnet_win_connection_failed(spudnet_error *record, SPUDRESULT otherwise, int error) {
	return spudnet_win_socket_failed(record, error == WSAETIMEDOUT ? SPUDRESULT_SPUDNET_SYSTEM_TIMED_OUT : otherwise, error);
}

/* A numeric address and a port as the sockaddr bind() and connect() take.
 * AI_NUMERICHOST keeps getaddrinfo to parsing: it never asks a name server,
 * so it never waits. False for anything that isn't a numeric address. */
static bool spudnet_win_parse_address(const char *address, uint16_t port, struct sockaddr_storage *out_storage, int *out_length) {
	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = AI_NUMERICHOST;

	struct addrinfo *parsed = NULL;
	if (getaddrinfo(address, port_text, &hints, &parsed) != 0 || !parsed)
		return false;
	bool usable = (parsed->ai_family == AF_INET || parsed->ai_family == AF_INET6) && parsed->ai_addrlen <= sizeof(*out_storage);
	if (usable) {
		memset(out_storage, 0, sizeof(*out_storage));
		memcpy(out_storage, parsed->ai_addr, parsed->ai_addrlen);
		*out_length = (int)parsed->ai_addrlen;
	}
	freeaddrinfo(parsed);
	return usable;
}

/* A new TCP socket. Its own function because the public calls below name
 * their parameter `socket`, as spudnet.h does, which hides socket(). */
static SOCKET spudnet_win_open_tcp(int family) {
	return socket(family, SOCK_STREAM, IPPROTO_TCP);
}

// --------------------------------------------------------------------------
// The two objects
// --------------------------------------------------------------------------

struct spudnet_tcp_listener_t {
	spud_object_head            head; // its allocator, also every socket's it accepts
	spudnet_instance            instance; // also every socket's it accepts
	struct spudnet_win_endpoint endpoint;
	// The last accept that failed.
	spudnet_error error;
};

struct spudnet_tcp_socket_t {
	spud_object_head head;
	spudnet_instance instance;
	// The endpoint's handle is made by connect, read by other calls only
	// once `open` says the connection is up, and closed only by destroy.
	struct spudnet_win_endpoint endpoint;
	volatile LONG               connect_called;
	volatile LONG               open;
	// By SPUDNET_ERROR_SIDE; each is written only by its side's caller.
	spudnet_error errors[SPUDNET_ERROR_SIDE_COUNT];
};

/* A socket with its events set up and no connection, made through
 * `allocator` (its instance's, or for an accepted socket its listener's),
 * which it keeps a copy of. NULL on failure. */
static struct spudnet_tcp_socket_t *spudnet_win_socket_alloc(spudnet_instance instance, const SPUD_ALLOCATOR *allocator) {
	struct spudnet_tcp_socket_t *created = SPUD_NEW_ZEROED(allocator, struct spudnet_tcp_socket_t);
	if (!created)
		return NULL;
	created->head.allocator = *allocator;
	SPUD_BIRTH_IN(created, &instance->quarantine);
	created->instance       = instance;
	if (!spudnet_win_endpoint_init(&created->endpoint)) {
		spudnet_win_endpoint_free(&created->endpoint);
		SPUD_DELETE(allocator, struct spudnet_tcp_socket_t, created);
		return NULL;
	}
	return created;
}

// For a wait set to poll (see the end of spudnetshared.h). WSAPoll asks
// about a socket without disturbing the WSAEventSelect tie the calls here
// wait through.

bool spudnet_tcp_socket_native(spudnet_tcp_socket socket, intptr_t *out_native) {
	if (!socket || InterlockedCompareExchange(&socket->open, 0, 0) == 0)
		return false;
	*out_native = (intptr_t)socket->endpoint.handle;
	return true;
}

bool spudnet_tcp_listener_native(spudnet_tcp_listener listener, intptr_t *out_native) {
	if (!listener || listener->endpoint.handle == INVALID_SOCKET)
		return false;
	*out_native = (intptr_t)listener->endpoint.handle;
	return true;
}

spudnet_instance spudnet_tcp_socket_instance(spudnet_tcp_socket socket) { return socket ? socket->instance : NULL; }

spudnet_instance spudnet_tcp_listener_instance(spudnet_tcp_listener listener) { return listener ? listener->instance : NULL; }

// --------------------------------------------------------------------------
// Listener
// --------------------------------------------------------------------------

void spudnet_tcp_listener_destroy(spudnet_tcp_listener listener) {
	if (!listener)
		return;
	if (SPUD_STALE(listener, "spudnet_tcp_listener_destroy", "listener"))
		return;
	spudnet_win_endpoint_free(&listener->endpoint);
	SPUD_KILL(listener);
	const SPUD_ALLOCATOR allocator = listener->head.allocator;
	spud_string_free(&allocator, listener->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(listener), &allocator, struct spudnet_tcp_listener_t, listener);
}

SPUDRESULT spudnet_tcp_listener_create(
    spudnet_instance instance,
    const spudnet_tcp_listener_desc *desc,
    spudnet_tcp_listener *out_listener,
    spudnet_error *out_error) {
	spudnet_error_clear(out_error);
	if (!out_listener)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_listener = NULL;
	if (!instance)
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudnet_tcp_listener_create", "instance"))
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_NET_TCP_LISTENER_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!desc->address || desc->backlog == 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	struct sockaddr_storage storage;
	int                     length = 0;
	if (!spudnet_win_parse_address(desc->address, desc->port, &storage, &length))
		return SPUDRESULT_SPUDNET_INVALID_ADDRESS;

	struct spudnet_tcp_listener_t *listener = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudnet_tcp_listener_t);
	if (!listener)
		return SPUDRESULT_OUT_OF_MEMORY;
	listener->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(listener, &instance->quarantine);
	listener->instance       = instance;
	if (!spudnet_win_endpoint_init(&listener->endpoint)) {
		int error = (int)GetLastError();
		spudnet_tcp_listener_destroy(listener);
		return spudnet_win_socket_failed(out_error, SPUDRESULT_GENERAL_FAILURE, error);
	}

	SPUDRESULT result = SPUDRESULT_SPUDNET_LISTEN_FAILED;
	if (!spudnet_win_endpoint_attach(&listener->endpoint, spudnet_win_open_tcp(storage.ss_family), FD_ACCEPT))
		goto failed;
	SOCKET handle = listener->endpoint.handle;

	// Whether an IPv6 listener also takes IPv4 is a system setting that
	// differs between platforms; fixed to "no" so it is the same on all.
	if (storage.ss_family == AF_INET6) {
		DWORD v6_only = 1;
		if (setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6_only, sizeof(v6_only)) != 0)
			goto failed;
	}
	// Nothing is set to let the port be listened on again as soon as its
	// last listener has gone, which spudnet.h promises: Winsock allows that
	// unasked. Its SO_REUSEADDR is a different thing from the BSD option of
	// that name - it lets a second socket bind a port that is in active
	// use - and is not wanted.

	if (bind(handle, (struct sockaddr *)&storage, length) != 0) {
		if (WSAGetLastError() == WSAEADDRINUSE)
			result = SPUDRESULT_SPUDNET_ADDRESS_IN_USE;
		goto failed;
	}
	if (listen(handle, desc->backlog > (uint32_t)INT_MAX ? INT_MAX : (int)desc->backlog) != 0) {
		if (WSAGetLastError() == WSAEADDRINUSE)
			result = SPUDRESULT_SPUDNET_ADDRESS_IN_USE;
		goto failed;
	}

	*out_listener = listener;
	return SPUD_SUCCESS;

failed:;
	// Every jump here comes straight from the call that failed.
	int error = WSAGetLastError();
	spudnet_tcp_listener_destroy(listener);
	return spudnet_win_socket_failed(out_error, result, error);
}

SPUDRESULT spudnet_tcp_listener_get_port(
    spudnet_tcp_listener listener,
    uint16_t *out_port) {
	if (!out_port)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_port = 0;
	if (!listener)
		return SPUDRESULT_SPUDNET_INVALID_TCP_LISTENER;
	if (SPUD_STALE(listener, "spudnet_tcp_listener_get_port", "listener"))
		return SPUDRESULT_SPUDNET_INVALID_TCP_LISTENER;

	struct sockaddr_storage storage;
	int                     length = sizeof(storage);
	if (getsockname(listener->endpoint.handle, (struct sockaddr *)&storage, &length) != 0)
		return SPUDRESULT_SPUDNET_INVALID_TCP_LISTENER;
	if (storage.ss_family == AF_INET6)
		*out_port = ntohs(((struct sockaddr_in6 *)&storage)->sin6_port);
	else
		*out_port = ntohs(((struct sockaddr_in *)&storage)->sin_port);
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_tcp_listener_accept(
    spudnet_tcp_listener listener,
    uint32_t timeout_ms,
    spudnet_tcp_socket *out_socket) {
	if (!out_socket)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!listener)
		return SPUDRESULT_SPUDNET_INVALID_TCP_LISTENER;
	if (SPUD_STALE(listener, "spudnet_tcp_listener_accept", "listener"))
		return SPUDRESULT_SPUDNET_INVALID_TCP_LISTENER;

	struct spudnet_win_endpoint *endpoint = &listener->endpoint;
	ULONGLONG                    start    = GetTickCount64();
	for (;;) {
		if (spudnet_win_endpoint_aborted(endpoint))
			return SPUDRESULT_SPUDNET_ABORTED;

		ResetEvent(endpoint->readable_event);
		SOCKET handle = accept(endpoint->handle, NULL, NULL);
		if (handle != INVALID_SOCKET) {
			struct spudnet_tcp_socket_t *accepted = spudnet_win_socket_alloc(listener->instance, &listener->head.allocator);
			if (!accepted) {
				closesocket(handle);
				return SPUDRESULT_OUT_OF_MEMORY;
			}
			// An accepted socket starts out tied to the listener's event;
			// this ties it to its own.
			if (!spudnet_win_endpoint_attach(&accepted->endpoint, handle, FD_READ | FD_WRITE | FD_CLOSE)) {
				int error = WSAGetLastError();
				spudnet_tcp_socket_destroy(accepted);
				return spudnet_win_socket_failed(&listener->error, SPUDRESULT_SPUDNET_ACCEPT_FAILED, error);
			}
			InterlockedExchange(&accepted->connect_called, 1);
			InterlockedExchange(&accepted->open, 1);
			*out_socket = accepted;
			return SPUD_SUCCESS;
		}

		// A connection that came and went before it was accepted is not
		// the listener failing; the next one is tried.
		int error = WSAGetLastError();
		if (error == WSAECONNRESET)
			continue;
		if (error != WSAEWOULDBLOCK)
			return spudnet_win_socket_failed(&listener->error, SPUDRESULT_SPUDNET_ACCEPT_FAILED, error);

		SPUDRESULT waited = spudnet_win_endpoint_wait(endpoint, endpoint->readable_event, start, timeout_ms);
		if (SPUDFAIL(waited))
			return waited;
	}
}

void spudnet_tcp_listener_abort(spudnet_tcp_listener listener) {
	if (!listener)
		return;
	if (SPUD_STALE(listener, "spudnet_tcp_listener_abort", "listener"))
		return;
	spudnet_win_endpoint_abort(&listener->endpoint);
}

void spudnet_tcp_listener_get_error(
    spudnet_tcp_listener listener,
    spudnet_error *out_error) {
	if (!listener) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	if (SPUD_STALE(listener, "spudnet_tcp_listener_get_error", "listener")) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	spudnet_error_get(&listener->error, out_error);
}

// --------------------------------------------------------------------------
// Socket
// --------------------------------------------------------------------------

SPUDRESULT spudnet_tcp_socket_create(
    spudnet_instance instance,
    spudnet_tcp_socket *out_socket) {
	if (!out_socket)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudnet_tcp_socket_create", "instance"))
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	*out_socket = spudnet_win_socket_alloc(instance, &instance->head.allocator);
	return *out_socket ? SPUD_SUCCESS : SPUDRESULT_OUT_OF_MEMORY;
}

void spudnet_tcp_socket_destroy(spudnet_tcp_socket socket) {
	if (!socket)
		return;
	if (SPUD_STALE(socket, "spudnet_tcp_socket_destroy", "socket"))
		return;
	spudnet_win_endpoint_free(&socket->endpoint);
	SPUD_KILL(socket);
	const SPUD_ALLOCATOR allocator = socket->head.allocator;
	spud_string_free(&allocator, socket->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(socket), &allocator, struct spudnet_tcp_socket_t, socket);
}

void spudnet_tcp_socket_abort(spudnet_tcp_socket socket) {
	if (!socket)
		return;
	if (SPUD_STALE(socket, "spudnet_tcp_socket_abort", "socket"))
		return;
	spudnet_win_endpoint_abort(&socket->endpoint);
}

void spudnet_tcp_socket_get_error(
    spudnet_tcp_socket socket,
    SPUDNET_ERROR_SIDE side,
    spudnet_error *out_error) {
	if (!socket) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	if (SPUD_STALE(socket, "spudnet_tcp_socket_get_error", "socket")) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	bool known = side == SPUDNET_ERROR_SIDE_RECEIVING || side == SPUDNET_ERROR_SIDE_SENDING;
	spudnet_error_get(known ? &socket->errors[side] : NULL, out_error);
}

/* What a call on a socket that can't carry it returns, or SPUD_SUCCESS for
 * one that can. [function] is the public call's name, for the line a
 * destroyed socket prints (spudstale.h). */
static SPUDRESULT spudnet_win_socket_usable(struct spudnet_tcp_socket_t *socket, const char *function) {
	(void)function; // read by the stale check only, which a Release build has none of
	if (!socket)
		return SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET;
	if (SPUD_STALE(socket, function, "socket"))
		return SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET;
	if (spudnet_win_endpoint_aborted(&socket->endpoint))
		return SPUDRESULT_SPUDNET_ABORTED;
	if (InterlockedCompareExchange(&socket->open, 0, 0) == 0)
		return SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET;
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_tcp_socket_connect(
    spudnet_tcp_socket socket,
    const spudnet_tcp_socket_connect_desc *desc) {
	if (!socket)
		return SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET;
	if (SPUD_STALE(socket, "spudnet_tcp_socket_connect", "socket"))
		return SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!desc->address)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	ULONGLONG               start = GetTickCount64();
	struct sockaddr_storage storage;
	int                     length = 0;
	if (!spudnet_win_parse_address(desc->address, desc->port, &storage, &length))
		return SPUDRESULT_SPUDNET_INVALID_ADDRESS;

	struct spudnet_win_endpoint *endpoint = &socket->endpoint;
	if (spudnet_win_endpoint_aborted(endpoint))
		return SPUDRESULT_SPUDNET_ABORTED;
	if (InterlockedCompareExchange(&socket->connect_called, 0, 0) != 0)
		return SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET;
	if (desc->timeout_ms == SPUDNET_NO_WAIT)
		return SPUDRESULT_SPUDNET_TIMED_OUT;
	// From here the socket is spent unless this succeeds.
	InterlockedExchange(&socket->connect_called, 1);

	spudnet_error *record = &socket->errors[SPUDNET_ERROR_SIDE_RECEIVING];
	if (!spudnet_win_endpoint_attach(endpoint, spudnet_win_open_tcp(storage.ss_family), FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE))
		return spudnet_win_socket_failed(record, SPUDRESULT_SPUDNET_CONNECT_FAILED, WSAGetLastError());

	// On a non-blocking socket connect() only starts the attempt; Winsock
	// reports it finished, and SO_ERROR says how it went.
	ResetEvent(endpoint->writable_event);
	if (connect(endpoint->handle, (struct sockaddr *)&storage, length) != 0) {
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return spudnet_win_connection_failed(record, SPUDRESULT_SPUDNET_CONNECT_FAILED, WSAGetLastError());

		SPUDRESULT waited = spudnet_win_endpoint_wait(endpoint, endpoint->writable_event, start, desc->timeout_ms);
		if (SPUDFAIL(waited))
			return waited;

		int error        = 0;
		int error_length = sizeof(error);
		if (getsockopt(endpoint->handle, SOL_SOCKET, SO_ERROR, (char *)&error, &error_length) != 0)
			return spudnet_win_socket_failed(record, SPUDRESULT_SPUDNET_CONNECT_FAILED, WSAGetLastError());
		if (error != 0)
			return spudnet_win_connection_failed(record, SPUDRESULT_SPUDNET_CONNECT_FAILED, error);
	}

	InterlockedExchange(&socket->open, 1);
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_tcp_socket_send(
    spudnet_tcp_socket socket,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_sent) {
	if (!out_sent)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_sent         = 0;
	SPUDRESULT usable = spudnet_win_socket_usable(socket, "spudnet_tcp_socket_send");
	if (SPUDFAIL(usable))
		return usable;
	if (!data || size == 0)
		return SPUDRESULT_ZERO_SIZE;

	struct spudnet_win_endpoint *endpoint = &socket->endpoint;
	ULONGLONG                    start    = GetTickCount64();
	int                          room     = size > (uint64_t)INT_MAX ? INT_MAX : (int)size;
	for (;;) {
		ResetEvent(endpoint->writable_event);
		int sent = send(endpoint->handle, (const char *)data, room, 0);
		if (sent > 0) {
			*out_sent = (uint64_t)sent;
			return SPUD_SUCCESS;
		}
		if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
			return spudnet_win_connection_failed(&socket->errors[SPUDNET_ERROR_SIDE_SENDING], SPUDRESULT_SPUDNET_SEND_FAILED, WSAGetLastError());

		SPUDRESULT waited = spudnet_win_endpoint_wait(endpoint, endpoint->writable_event, start, timeout_ms);
		if (SPUDFAIL(waited))
			return waited;
	}
}

SPUDRESULT spudnet_tcp_socket_recv(
    spudnet_tcp_socket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received) {
	if (!out_received)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_received     = 0;
	SPUDRESULT usable = spudnet_win_socket_usable(socket, "spudnet_tcp_socket_recv");
	if (SPUDFAIL(usable))
		return usable;
	if (!data || size == 0)
		return SPUDRESULT_ZERO_SIZE;

	struct spudnet_win_endpoint *endpoint = &socket->endpoint;
	ULONGLONG                    start    = GetTickCount64();
	int                          room     = size > (uint64_t)INT_MAX ? INT_MAX : (int)size;
	for (;;) {
		ResetEvent(endpoint->readable_event);
		int received = recv(endpoint->handle, (char *)data, room, 0);
		if (received >= 0) {
			*out_received = (uint64_t)received; // 0: the peer has finished sending
			return SPUD_SUCCESS;
		}
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return spudnet_win_connection_failed(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], SPUDRESULT_SPUDNET_RECV_FAILED, WSAGetLastError());

		SPUDRESULT waited = spudnet_win_endpoint_wait(endpoint, endpoint->readable_event, start, timeout_ms);
		if (SPUDFAIL(waited))
			return waited;
	}
}

SPUDRESULT spudnet_tcp_socket_finish_sending(spudnet_tcp_socket socket) {
	SPUDRESULT usable = spudnet_win_socket_usable(socket, "spudnet_tcp_socket_finish_sending");
	if (SPUDFAIL(usable))
		return usable;
	if (shutdown(socket->endpoint.handle, SD_SEND) != 0)
		return spudnet_win_socket_failed(&socket->errors[SPUDNET_ERROR_SIDE_SENDING], SPUDRESULT_SPUDNET_SEND_FAILED, WSAGetLastError());
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_tcp_socket_set_no_delay(
    spudnet_tcp_socket socket,
    bool no_delay) {
	SPUDRESULT usable = spudnet_win_socket_usable(socket, "spudnet_tcp_socket_set_no_delay");
	if (SPUDFAIL(usable))
		return usable;
	BOOL value = no_delay ? TRUE : FALSE;
	if (setsockopt(socket->endpoint.handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&value, sizeof(value)) != 0)
		return spudnet_win_socket_failed(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], SPUDRESULT_GENERAL_FAILURE, WSAGetLastError());
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_tcp_socket_get_peer_address(
    spudnet_tcp_socket socket,
    char out_address[SPUDNET_MAX_ADDRESS_STRING_LEN],
    uint16_t *out_port) {
	if (!out_address || !out_port)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	out_address[0]    = '\0';
	*out_port         = 0;
	SPUDRESULT usable = spudnet_win_socket_usable(socket, "spudnet_tcp_socket_get_peer_address");
	if (SPUDFAIL(usable))
		return usable;

	struct sockaddr_storage storage;
	int                     length = sizeof(storage);
	if (getpeername(socket->endpoint.handle, (struct sockaddr *)&storage, &length) != 0)
		return spudnet_win_socket_failed(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], SPUDRESULT_GENERAL_FAILURE, WSAGetLastError());

	const void *bytes = NULL;
	if (storage.ss_family == AF_INET6) {
		bytes     = &((struct sockaddr_in6 *)&storage)->sin6_addr;
		*out_port = ntohs(((struct sockaddr_in6 *)&storage)->sin6_port);
	} else {
		bytes     = &((struct sockaddr_in *)&storage)->sin_addr;
		*out_port = ntohs(((struct sockaddr_in *)&storage)->sin_port);
	}
	if (!inet_ntop(storage.ss_family, bytes, out_address, SPUDNET_MAX_ADDRESS_STRING_LEN)) {
		out_address[0] = '\0';
		*out_port      = 0;
		return SPUDRESULT_GENERAL_FAILURE;
	}
	return SPUD_SUCCESS;
}

// --------------------------------------------------------------------------
// Resolver
// --------------------------------------------------------------------------

/* The lookup and the waiting for it are two calls on two threads of the
 * caller's, as spudnet.h sets out: spudnet_resolver_run does the lookup and
 * leaves the answer here, spudnet_resolver_wait waits for the event run
 * sets when it is through.
 *
 * Unlike getaddrinfo on the other platforms, GetAddrInfoExW can be started
 * without waiting for it and cancelled, so here an abort also ends the
 * lookup and brings the thread in run back.
 *
 * Either thread may be the last to need the resolver, so it is held twice
 * from the start, once for run and once for destroy, and freed when both
 * have let go.
 *
 * The thread in run may also outlast the instance the resolver was made
 * with: the caller destroys the resolver, then the instance, while the
 * lookup is still out. So the resolver has a start of Winsock of its own
 * (`started`), made by create and given back when the resolver is freed,
 * and a copy of the instance's allocator in its head, which it is freed
 * through - by whichever of run and destroy lets go last, so possibly on
 * the thread in run, after the instance is gone. */
struct spudnet_resolver_t {
	spud_object_head head;
	volatile LONG    references;
	bool          started; // WSAStartup succeeded and WSACleanup is owed

	// Set by create and only read after.
	wchar_t               *host;
	size_t                 host_count; // the wchar_ts `host` was allocated with
	SPUDNET_ADDRESS_FAMILY family;

	HANDLE        abort_event; // set by abort and never reset
	volatile LONG aborted;
	volatile LONG run_called;
	HANDLE        done_event;  // set, for good, once run has finished

	// What run leaves for wait and get_error.
	CRITICAL_SECTION lock;
	bool             finished;
	SPUDRESULT       result;
	char (*addresses)[SPUDNET_MAX_ADDRESS_STRING_LEN];
	uint32_t      address_count;
	uint32_t      address_capacity; // the entries `addresses` was allocated with
	spudnet_error error;
};

static void spudnet_win_resolver_release(struct spudnet_resolver_t *resolver) {
	if (InterlockedDecrement(&resolver->references) != 0)
		return;
	if (resolver->abort_event)
		CloseHandle(resolver->abort_event);
	if (resolver->done_event)
		CloseHandle(resolver->done_event);
	DeleteCriticalSection(&resolver->lock);
	// The resolver's own copy: this may run after its instance is gone.
	const SPUD_ALLOCATOR allocator = resolver->head.allocator;
	spud_free_array(&allocator, resolver->addresses, resolver->address_capacity, SPUDNET_MAX_ADDRESS_STRING_LEN, 1);
	SPUD_DELETE_ARRAY(&allocator, wchar_t, resolver->host, resolver->host_count);
	bool started = resolver->started;
	spud_string_free(&allocator, resolver->head.debug_name);
	SPUD_DELETE(&allocator, struct spudnet_resolver_t, resolver);
	// Last, with nothing of the resolver's left that could need Winsock.
	if (started)
		WSACleanup();
}

SPUDRESULT spudnet_resolver_create(
    spudnet_instance instance,
    const spudnet_resolver_desc *desc,
    spudnet_resolver *out_resolver) {
	if (!out_resolver)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_resolver = NULL;
	if (!instance)
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudnet_resolver_create", "instance"))
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_NET_RESOLVER_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!desc->host || desc->host[0] == '\0')
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	// GetAddrInfoExW takes the name as UTF-16. 0 here means the bytes
	// weren't UTF-8.
	int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc->host, -1, NULL, 0);
	if (wide_count <= 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	struct spudnet_resolver_t *resolver = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudnet_resolver_t);
	if (!resolver)
		return SPUDRESULT_OUT_OF_MEMORY;
	resolver->head.allocator = instance->head.allocator;
	SPUD_BIRTH(resolver);
	// One for this function's failure paths to let go of; the second is
	// added only once there is a resolver to hand out.
	resolver->references = 1;
	InitializeCriticalSection(&resolver->lock);
	resolver->family      = desc->family;
	resolver->abort_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	resolver->done_event  = CreateEventW(NULL, TRUE, FALSE, NULL);
	resolver->host        = SPUD_NEW_ARRAY(&resolver->head.allocator, wchar_t, (size_t)wide_count);
	if (resolver->host)
		resolver->host_count = (size_t)wide_count;
	if (!resolver->abort_event || !resolver->done_event || !resolver->host ||
	    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc->host, -1, resolver->host, wide_count) <= 0) {
		spudnet_win_resolver_release(resolver);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	// The resolver's own start of Winsock; see the struct. spudnet.h gives
	// this call no error record to fill, so only the result says it failed.
	SPUDRESULT started = spudnet_win_start(NULL);
	if (SPUDFAIL(started)) {
		spudnet_win_resolver_release(resolver);
		return started;
	}
	resolver->started = true;

	InterlockedExchange(&resolver->references, 2);
	*out_resolver = resolver;
	return SPUD_SUCCESS;
}

void spudnet_resolver_destroy(spudnet_resolver resolver) {
	if (!resolver)
		return;
	if (SPUD_STALE(resolver, "spudnet_resolver_destroy", "resolver"))
		return;
	// Dead from here, though the run may still hold the memory.
	SPUD_KILL(resolver);
	spudnet_win_resolver_release(resolver);
}

void spudnet_resolver_abort(spudnet_resolver resolver) {
	if (!resolver)
		return;
	if (SPUD_STALE(resolver, "spudnet_resolver_abort", "resolver"))
		return;
	InterlockedExchange(&resolver->aborted, 1);
	SetEvent(resolver->abort_event);
}

void spudnet_resolver_get_error(
    spudnet_resolver resolver,
    spudnet_error *out_error) {
	if (!resolver) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	if (SPUD_STALE(resolver, "spudnet_resolver_get_error", "resolver")) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	// Written by the thread in run, so read under the lock it writes under.
	EnterCriticalSection(&resolver->lock);
	spudnet_error_get(&resolver->error, out_error);
	LeaveCriticalSection(&resolver->lock);
}

SPUDRESULT spudnet_resolver_run(spudnet_resolver resolver) {
	if (!resolver)
		return SPUDRESULT_SPUDNET_INVALID_RESOLVER;
	// No stale check: the run may start after the resolver is destroyed
	// (spudnet.h), which kills its tag at the destroy call while the run's
	// reference keeps the memory.
	// A second run has no reference of its own to let go of at the end.
	if (InterlockedExchange(&resolver->run_called, 1) != 0)
		return SPUDRESULT_SPUDNET_INVALID_RESOLVER;

	SPUDRESULT    result = SPUDRESULT_SPUDNET_ABORTED;
	spudnet_error error;
	spudnet_error_clear(&error);
	char (*addresses)[SPUDNET_MAX_ADDRESS_STRING_LEN] = NULL;
	uint32_t address_count                            = 0;
	uint32_t address_capacity                         = 0;

	// GetAddrInfoExW reports through this event when it couldn't answer on
	// the spot. Without it there is no lookup to make.
	HANDLE answered = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!answered) {
		result = spudnet_win_socket_failed(&error, SPUDRESULT_GENERAL_FAILURE, (int)GetLastError());
	} else if (InterlockedCompareExchange(&resolver->aborted, 0, 0) == 0) {
		ADDRINFOEXW hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_STREAM; // one entry per address, not one per socket type
		switch (resolver->family) {
		case SPUDNET_ADDRESS_FAMILY_IPV4:
			hints.ai_family = AF_INET;
			break;
		case SPUDNET_ADDRESS_FAMILY_IPV6:
			hints.ai_family = AF_INET6;
			break;
		default:
			hints.ai_family = AF_UNSPEC;
			break;
		}

		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent  = answered;
		PADDRINFOEXW found = NULL;
		HANDLE       stop  = NULL;
		// NS_ALL is every name source getaddrinfo itself consults.
		INT failed = GetAddrInfoExW(resolver->host, NULL, NS_ALL, NULL, &hints, &found, NULL, &overlapped, NULL, &stop);
		if (failed == WSA_IO_PENDING) {
			// Abort is listed first so it wins when both are set. A lookup
			// told to stop still reports through the event, and `overlapped`
			// and `found` are its to write until it has.
			HANDLE waits[2] = {resolver->abort_event, answered};
			if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
				GetAddrInfoExCancel(&stop);
				WaitForSingleObject(answered, INFINITE);
			}
			failed = GetAddrInfoExOverlappedResult(&overlapped);
		}

		if (failed != NO_ERROR) {
			result = spudnet_win_socket_failed(&error, SPUDRESULT_SPUDNET_RESOLVE_FAILED, failed);
		} else {
			uint32_t entries = 0;
			for (PADDRINFOEXW entry = found; entry != NULL; entry = entry->ai_next)
				entries++;
			result = SPUDRESULT_SPUDNET_RESOLVE_FAILED; // until an address turns up
			// On the caller's thread in run, through the resolver's own
			// allocator (the instance may be gone by now).
			if (entries > 0) {
				addresses = (char (*)[SPUDNET_MAX_ADDRESS_STRING_LEN])spud_allocate_array(
				    &resolver->head.allocator, entries, SPUDNET_MAX_ADDRESS_STRING_LEN, 1, true);
				if (addresses)
					address_capacity = entries;
			}
			if (entries > 0 && !addresses)
				result = SPUDRESULT_OUT_OF_MEMORY;

			for (PADDRINFOEXW entry = found; entry != NULL && addresses; entry = entry->ai_next) {
				const void *bytes = NULL;
				if (entry->ai_family == AF_INET)
					bytes = &((struct sockaddr_in *)entry->ai_addr)->sin_addr;
				else if (entry->ai_family == AF_INET6)
					bytes = &((struct sockaddr_in6 *)entry->ai_addr)->sin6_addr;
				else
					continue;
				char text[SPUDNET_MAX_ADDRESS_STRING_LEN];
				if (!inet_ntop(entry->ai_family, bytes, text, sizeof(text)))
					continue;

				// The resolver can list one address more than once.
				bool duplicate = false;
				for (uint32_t i = 0; i < address_count && !duplicate; ++i)
					duplicate = strcmp(addresses[i], text) == 0;
				if (duplicate)
					continue;
				memcpy(addresses[address_count++], text, strlen(text) + 1);
				result = SPUD_SUCCESS;
			}
		}
		if (found)
			FreeAddrInfoExW(found);
	}
	if (answered)
		CloseHandle(answered);

	EnterCriticalSection(&resolver->lock);
	resolver->finished      = true;
	resolver->result        = result;
	resolver->addresses        = addresses;
	resolver->address_count    = address_count;
	resolver->address_capacity = address_capacity;
	resolver->error         = error;
	LeaveCriticalSection(&resolver->lock);
	SetEvent(resolver->done_event);

	// The answer stays for a waiter that comes later; what this call
	// reports is what a wait would report now.
	if (InterlockedCompareExchange(&resolver->aborted, 0, 0) != 0)
		result = SPUDRESULT_SPUDNET_ABORTED;
	// The resolver may be gone after this: nothing of it is touched again.
	spudnet_win_resolver_release(resolver);
	return result;
}

SPUDRESULT spudnet_resolver_wait(
    spudnet_resolver resolver,
    uint32_t timeout_ms,
    char out_addresses[][SPUDNET_MAX_ADDRESS_STRING_LEN],
    uint32_t out_capacity,
    uint32_t *out_count) {
	if (!out_addresses || !out_count)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_count = 0;
	if (!resolver)
		return SPUDRESULT_SPUDNET_INVALID_RESOLVER;
	if (SPUD_STALE(resolver, "spudnet_resolver_wait", "resolver"))
		return SPUDRESULT_SPUDNET_INVALID_RESOLVER;

	// Abort is listed first so it wins when both are set. The done event is
	// never reset, so once run has finished every wait returns at once.
	ULONGLONG start    = GetTickCount64();
	HANDLE    waits[2] = {resolver->abort_event, resolver->done_event};
	DWORD     waited   = WaitForMultipleObjects(2, waits, FALSE, spudnet_win_socket_remaining(start, timeout_ms));
	if (waited == WAIT_OBJECT_0)
		return SPUDRESULT_SPUDNET_ABORTED;
	if (waited != WAIT_OBJECT_0 + 1)
		return SPUDRESULT_SPUDNET_TIMED_OUT;

	EnterCriticalSection(&resolver->lock);
	SPUDRESULT result = resolver->result;
	if (!SPUDFAIL(result)) {
		uint32_t count = resolver->address_count < out_capacity ? resolver->address_count : out_capacity;
		for (uint32_t i = 0; i < count; ++i)
			memcpy(out_addresses[i], resolver->addresses[i], SPUDNET_MAX_ADDRESS_STRING_LEN);
		*out_count = count;
	}
	LeaveCriticalSection(&resolver->lock);
	return result;
}

#if __cplusplus
}
#endif

#endif // SPUDLIB_PLATFORM_WIN32
