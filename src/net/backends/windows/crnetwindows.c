
#if CARROTTUS_PLATFORM_WIN32

/*
 * Carrottus Net TCP sockets on Winsock2, and Carrottus Net's startup for Windows.
 *
 * Every socket here is non-blocking, and that is an internal matter:
 * crnet.h has no non-blocking mode. A call tries the operation, and if
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

#include "../../crnetshared.h"
#include "crnet.h"
#include "../../../crstructbase.h"

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
static CR_RESULT crnet_win_start(crnet_error *out_error) {
	WSADATA wsa_data;
	// Returns its error rather than leaving it for WSAGetLastError.
	int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (result != 0)
		return crnet_error_record(out_error, CR_RESULT_NET_STARTUP_FAILED, CR_PLATFORM_ERROR_SOURCE_WIN32, result, NULL);
	return CR_SUCCESS;
}

CR_RESULT crnet_instance_create(
    const crnet_instance_desc *desc,
    crnet_instance *out_instance,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	const CR_RESULT desc_result = crnet_instance_desc_check(desc, out_instance);
	if (desc_result != CR_SUCCESS)
		return desc_result;

	struct crnet_instance_t *instance = CR_NEW_ZEROED(desc->allocator, struct crnet_instance_t);
	if (!instance)
		return CR_RESULT_OUT_OF_MEMORY;
	instance->head.allocator = *desc->allocator;
	CR_BIRTH(instance);
	CR_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);
	CR_RESULT result        = crnet_win_start(out_error);
	if (CR_FAILED(result)) {
		CR_DELETE(desc->allocator, struct crnet_instance_t, instance);
		return result;
	}
	*out_instance = instance;
	return CR_SUCCESS;
}

void crnet_instance_destroy(crnet_instance instance) {
	if (!instance)
		return;
	if (CR_STALE(instance, "crnet_instance_destroy", "instance"))
		return;
	WSACleanup();
	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	CR_QUARANTINE_RELEASE(&instance->quarantine);
	cr_string_free(&allocator, instance->head.debug_name);
	CR_DELETE(&allocator, struct crnet_instance_t, instance);
}

// --------------------------------------------------------------------------
// Shared by listeners and sockets
// --------------------------------------------------------------------------

/* One Winsock socket and what waiting on it takes. See the top of the
 * file for why there are three events besides abort's. */
struct crnet_win_endpoint {
	SOCKET   handle;         // INVALID_SOCKET until there is one
	WSAEVENT network_event;  // Winsock sets it for any activity on `handle`
	HANDLE   readable_event; // something to read, accept, or a close
	HANDLE   writable_event; // room to write, a connect finished, or a close
	HANDLE   abort_event;    // set by abort and never reset
	volatile LONG aborted;
};

static void crnet_win_endpoint_free(struct crnet_win_endpoint *endpoint) {
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

static bool crnet_win_endpoint_init(struct crnet_win_endpoint *endpoint) {
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
static bool crnet_win_endpoint_attach(struct crnet_win_endpoint *endpoint, SOCKET handle, long events) {
	endpoint->handle = handle;
	return handle != INVALID_SOCKET && WSAEventSelect(handle, endpoint->network_event, events) == 0;
}

static void crnet_win_endpoint_abort(struct crnet_win_endpoint *endpoint) {
	InterlockedExchange(&endpoint->aborted, 1);
	SetEvent(endpoint->abort_event);
}

static bool crnet_win_endpoint_aborted(struct crnet_win_endpoint *endpoint) {
	return InterlockedCompareExchange(&endpoint->aborted, 0, 0) != 0;
}

/* Milliseconds left of `timeout_ms` since `start`, for a Win32 wait. */
static DWORD crnet_win_socket_remaining(ULONGLONG start, uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return INFINITE;
	ULONGLONG elapsed = GetTickCount64() - start;
	if (elapsed >= timeout_ms)
		return 0;
	ULONGLONG left = timeout_ms - elapsed;
	return left >= INFINITE ? INFINITE - 1 : (DWORD)left;
}

/* Waits for news in one direction, for the time to run out, or for an
 * abort. The caller reset `own_event` (the endpoint's readable or writable
 * event) before the attempt that came back "would block". CR_SUCCESS
 * means it is worth trying the operation again. */
static CR_RESULT crnet_win_endpoint_wait(struct crnet_win_endpoint *endpoint, HANDLE own_event, ULONGLONG start, uint32_t timeout_ms) {
	for (;;) {
		// Abort is listed first so it wins when several are set.
		HANDLE waits[3] = {endpoint->abort_event, own_event, endpoint->network_event};
		DWORD  waited   = WaitForMultipleObjects(3, waits, FALSE, crnet_win_socket_remaining(start, timeout_ms));
		if (waited == WAIT_OBJECT_0)
			return CR_RESULT_NET_ABORTED;
		if (waited == WAIT_OBJECT_0 + 1)
			return CR_SUCCESS;
		if (waited != WAIT_OBJECT_0 + 2)
			return CR_RESULT_NET_TIMED_OUT;

		// Something happened on the socket. Asking what also resets the
		// network event; the answer is passed on to both directions, since
		// the other one's waiter may be the one it was for.
		WSANETWORKEVENTS happened;
		memset(&happened, 0, sizeof(happened));
		if (WSAEnumNetworkEvents(endpoint->handle, endpoint->network_event, &happened) != 0)
			return CR_SUCCESS; // the operation will say what is wrong
		if (happened.lNetworkEvents & (FD_READ | FD_ACCEPT | FD_CLOSE))
			SetEvent(endpoint->readable_event);
		if (happened.lNetworkEvents & (FD_WRITE | FD_CONNECT | FD_CLOSE))
			SetEvent(endpoint->writable_event);
	}
}

/* A failed Winsock call as a record: `error` is what WSAGetLastError said
 * after it. Returns `result`. */
static CR_RESULT crnet_win_socket_failed(crnet_error *record, CR_RESULT result, int error) {
	return crnet_error_record(record, result, CR_PLATFORM_ERROR_SOURCE_WIN32, error, NULL);
}

/* The same for a call on a connection, where one error is a result of its
 * own: the system stopped waiting for the other end - a connect nobody
 * answered, a peer that went silent - with time still left on the caller's
 * limit. `otherwise` is the result for every other error. */
static CR_RESULT crnet_win_connection_failed(crnet_error *record, CR_RESULT otherwise, int error) {
	return crnet_win_socket_failed(record, error == WSAETIMEDOUT ? CR_RESULT_NET_SYSTEM_TIMED_OUT : otherwise, error);
}

/* A numeric address and a port as the sockaddr bind() and connect() take.
 * AI_NUMERICHOST keeps getaddrinfo to parsing: it never asks a name server,
 * so it never waits. False for anything that isn't a numeric address. */
static bool crnet_win_parse_address(const char *address, uint16_t port, struct sockaddr_storage *out_storage, int *out_length) {
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
 * their parameter `socket`, as crnet.h does, which hides socket(). */
static SOCKET crnet_win_open_tcp(int family) {
	return socket(family, SOCK_STREAM, IPPROTO_TCP);
}

// --------------------------------------------------------------------------
// The two objects
// --------------------------------------------------------------------------

struct crnet_tcp_listener_t {
	cr_object_head            head; // its allocator, also every socket's it accepts
	crnet_instance            instance; // also every socket's it accepts
	struct crnet_win_endpoint endpoint;
	// The last accept that failed.
	crnet_error error;
};

struct crnet_tcp_socket_t {
	cr_object_head head;
	crnet_instance instance;
	// The endpoint's handle is made by connect, read by other calls only
	// once `open` says the connection is up, and closed only by destroy.
	struct crnet_win_endpoint endpoint;
	volatile LONG               connect_called;
	volatile LONG               open;
	// By CRNET_ERROR_SIDE; each is written only by its side's caller.
	crnet_error errors[CRNET_ERROR_SIDE_COUNT];
};

/* A socket with its events set up and no connection, made through
 * `allocator` (its instance's, or for an accepted socket its listener's),
 * which it keeps a copy of. NULL on failure. */
static struct crnet_tcp_socket_t *crnet_win_socket_alloc(crnet_instance instance, const CR_ALLOCATOR *allocator) {
	struct crnet_tcp_socket_t *created = CR_NEW_ZEROED(allocator, struct crnet_tcp_socket_t);
	if (!created)
		return NULL;
	created->head.allocator = *allocator;
	CR_BIRTH_IN(created, &instance->quarantine);
	created->instance       = instance;
	if (!crnet_win_endpoint_init(&created->endpoint)) {
		crnet_win_endpoint_free(&created->endpoint);
		CR_DELETE(allocator, struct crnet_tcp_socket_t, created);
		return NULL;
	}
	return created;
}

// For a wait set to poll (see the end of crnetshared.h). WSAPoll asks
// about a socket without disturbing the WSAEventSelect tie the calls here
// wait through.

bool crnet_tcp_socket_native(crnet_tcp_socket socket, intptr_t *out_native) {
	if (!socket || InterlockedCompareExchange(&socket->open, 0, 0) == 0)
		return false;
	*out_native = (intptr_t)socket->endpoint.handle;
	return true;
}

bool crnet_tcp_listener_native(crnet_tcp_listener listener, intptr_t *out_native) {
	if (!listener || listener->endpoint.handle == INVALID_SOCKET)
		return false;
	*out_native = (intptr_t)listener->endpoint.handle;
	return true;
}

crnet_instance crnet_tcp_socket_instance(crnet_tcp_socket socket) { return socket ? socket->instance : NULL; }

crnet_instance crnet_tcp_listener_instance(crnet_tcp_listener listener) { return listener ? listener->instance : NULL; }

// --------------------------------------------------------------------------
// Listener
// --------------------------------------------------------------------------

void crnet_tcp_listener_destroy(crnet_tcp_listener listener) {
	if (!listener)
		return;
	if (CR_STALE(listener, "crnet_tcp_listener_destroy", "listener"))
		return;
	crnet_win_endpoint_free(&listener->endpoint);
	CR_KILL(listener);
	const CR_ALLOCATOR allocator = listener->head.allocator;
	cr_string_free(&allocator, listener->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(listener), &allocator, struct crnet_tcp_listener_t, listener);
}

CR_RESULT crnet_tcp_listener_create(
    crnet_instance instance,
    const crnet_tcp_listener_desc *desc,
    crnet_tcp_listener *out_listener,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!out_listener)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_listener = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_tcp_listener_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->address || desc->backlog == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	struct sockaddr_storage storage;
	int                     length = 0;
	if (!crnet_win_parse_address(desc->address, desc->port, &storage, &length))
		return CR_RESULT_NET_INVALID_ADDRESS;

	struct crnet_tcp_listener_t *listener = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_tcp_listener_t);
	if (!listener)
		return CR_RESULT_OUT_OF_MEMORY;
	listener->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(listener, &instance->quarantine);
	listener->instance       = instance;
	if (!crnet_win_endpoint_init(&listener->endpoint)) {
		int error = (int)GetLastError();
		crnet_tcp_listener_destroy(listener);
		return crnet_win_socket_failed(out_error, CR_RESULT_GENERAL_FAILURE, error);
	}

	CR_RESULT result = CR_RESULT_NET_LISTEN_FAILED;
	if (!crnet_win_endpoint_attach(&listener->endpoint, crnet_win_open_tcp(storage.ss_family), FD_ACCEPT))
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
	// last listener has gone, which crnet.h promises: Winsock allows that
	// unasked. Its SO_REUSEADDR is a different thing from the BSD option of
	// that name - it lets a second socket bind a port that is in active
	// use - and is not wanted.

	if (bind(handle, (struct sockaddr *)&storage, length) != 0) {
		if (WSAGetLastError() == WSAEADDRINUSE)
			result = CR_RESULT_NET_ADDRESS_IN_USE;
		goto failed;
	}
	if (listen(handle, desc->backlog > (uint32_t)INT_MAX ? INT_MAX : (int)desc->backlog) != 0) {
		if (WSAGetLastError() == WSAEADDRINUSE)
			result = CR_RESULT_NET_ADDRESS_IN_USE;
		goto failed;
	}

	*out_listener = listener;
	return CR_SUCCESS;

failed:;
	// Every jump here comes straight from the call that failed.
	int error = WSAGetLastError();
	crnet_tcp_listener_destroy(listener);
	return crnet_win_socket_failed(out_error, result, error);
}

CR_RESULT crnet_tcp_listener_get_port(
    crnet_tcp_listener listener,
    uint16_t *out_port) {
	if (!out_port)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_port = 0;
	if (!listener)
		return CR_RESULT_NET_INVALID_TCP_LISTENER;
	if (CR_STALE(listener, "crnet_tcp_listener_get_port", "listener"))
		return CR_RESULT_NET_INVALID_TCP_LISTENER;

	struct sockaddr_storage storage;
	int                     length = sizeof(storage);
	if (getsockname(listener->endpoint.handle, (struct sockaddr *)&storage, &length) != 0)
		return CR_RESULT_NET_INVALID_TCP_LISTENER;
	if (storage.ss_family == AF_INET6)
		*out_port = ntohs(((struct sockaddr_in6 *)&storage)->sin6_port);
	else
		*out_port = ntohs(((struct sockaddr_in *)&storage)->sin_port);
	return CR_SUCCESS;
}

CR_RESULT crnet_tcp_listener_accept(
    crnet_tcp_listener listener,
    uint32_t timeout_ms,
    crnet_tcp_socket *out_socket) {
	if (!out_socket)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!listener)
		return CR_RESULT_NET_INVALID_TCP_LISTENER;
	if (CR_STALE(listener, "crnet_tcp_listener_accept", "listener"))
		return CR_RESULT_NET_INVALID_TCP_LISTENER;

	struct crnet_win_endpoint *endpoint = &listener->endpoint;
	ULONGLONG                    start    = GetTickCount64();
	for (;;) {
		if (crnet_win_endpoint_aborted(endpoint))
			return CR_RESULT_NET_ABORTED;

		ResetEvent(endpoint->readable_event);
		SOCKET handle = accept(endpoint->handle, NULL, NULL);
		if (handle != INVALID_SOCKET) {
			struct crnet_tcp_socket_t *accepted = crnet_win_socket_alloc(listener->instance, &listener->head.allocator);
			if (!accepted) {
				closesocket(handle);
				return CR_RESULT_OUT_OF_MEMORY;
			}
			// An accepted socket starts out tied to the listener's event;
			// this ties it to its own.
			if (!crnet_win_endpoint_attach(&accepted->endpoint, handle, FD_READ | FD_WRITE | FD_CLOSE)) {
				int error = WSAGetLastError();
				crnet_tcp_socket_destroy(accepted);
				return crnet_win_socket_failed(&listener->error, CR_RESULT_NET_ACCEPT_FAILED, error);
			}
			InterlockedExchange(&accepted->connect_called, 1);
			InterlockedExchange(&accepted->open, 1);
			*out_socket = accepted;
			return CR_SUCCESS;
		}

		// A connection that came and went before it was accepted is not
		// the listener failing; the next one is tried.
		int error = WSAGetLastError();
		if (error == WSAECONNRESET)
			continue;
		if (error != WSAEWOULDBLOCK)
			return crnet_win_socket_failed(&listener->error, CR_RESULT_NET_ACCEPT_FAILED, error);

		CR_RESULT waited = crnet_win_endpoint_wait(endpoint, endpoint->readable_event, start, timeout_ms);
		if (CR_FAILED(waited))
			return waited;
	}
}

void crnet_tcp_listener_abort(crnet_tcp_listener listener) {
	if (!listener)
		return;
	if (CR_STALE(listener, "crnet_tcp_listener_abort", "listener"))
		return;
	crnet_win_endpoint_abort(&listener->endpoint);
}

void crnet_tcp_listener_get_error(
    crnet_tcp_listener listener,
    crnet_error *out_error) {
	if (!listener) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(listener, "crnet_tcp_listener_get_error", "listener")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	crnet_error_get(&listener->error, out_error);
}

// --------------------------------------------------------------------------
// Socket
// --------------------------------------------------------------------------

CR_RESULT crnet_tcp_socket_create(
    crnet_instance instance,
    crnet_tcp_socket *out_socket) {
	if (!out_socket)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_tcp_socket_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	*out_socket = crnet_win_socket_alloc(instance, &instance->head.allocator);
	return *out_socket ? CR_SUCCESS : CR_RESULT_OUT_OF_MEMORY;
}

void crnet_tcp_socket_destroy(crnet_tcp_socket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_tcp_socket_destroy", "socket"))
		return;
	crnet_win_endpoint_free(&socket->endpoint);
	CR_KILL(socket);
	const CR_ALLOCATOR allocator = socket->head.allocator;
	cr_string_free(&allocator, socket->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(socket), &allocator, struct crnet_tcp_socket_t, socket);
}

void crnet_tcp_socket_abort(crnet_tcp_socket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_tcp_socket_abort", "socket"))
		return;
	crnet_win_endpoint_abort(&socket->endpoint);
}

void crnet_tcp_socket_get_error(
    crnet_tcp_socket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error) {
	if (!socket) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(socket, "crnet_tcp_socket_get_error", "socket")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	bool known = side == CRNET_ERROR_SIDE_RECEIVING || side == CRNET_ERROR_SIDE_SENDING;
	crnet_error_get(known ? &socket->errors[side] : NULL, out_error);
}

/* What a call on a socket that can't carry it returns, or CR_SUCCESS for
 * one that can. [function] is the public call's name, for the line a
 * destroyed socket prints (crstale.h). */
static CR_RESULT crnet_win_socket_usable(struct crnet_tcp_socket_t *socket, const char *function) {
	(void)function; // read by the stale check only, which a Release build has none of
	if (!socket)
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (CR_STALE(socket, function, "socket"))
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (crnet_win_endpoint_aborted(&socket->endpoint))
		return CR_RESULT_NET_ABORTED;
	if (InterlockedCompareExchange(&socket->open, 0, 0) == 0)
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	return CR_SUCCESS;
}

CR_RESULT crnet_tcp_socket_connect(
    crnet_tcp_socket socket,
    const crnet_tcp_socket_connect_desc *desc) {
	if (!socket)
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (CR_STALE(socket, "crnet_tcp_socket_connect", "socket"))
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->address)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	ULONGLONG               start = GetTickCount64();
	struct sockaddr_storage storage;
	int                     length = 0;
	if (!crnet_win_parse_address(desc->address, desc->port, &storage, &length))
		return CR_RESULT_NET_INVALID_ADDRESS;

	struct crnet_win_endpoint *endpoint = &socket->endpoint;
	if (crnet_win_endpoint_aborted(endpoint))
		return CR_RESULT_NET_ABORTED;
	if (InterlockedCompareExchange(&socket->connect_called, 0, 0) != 0)
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (desc->timeout_ms == CRNET_NO_WAIT)
		return CR_RESULT_NET_TIMED_OUT;
	// From here the socket is spent unless this succeeds.
	InterlockedExchange(&socket->connect_called, 1);

	crnet_error *record = &socket->errors[CRNET_ERROR_SIDE_RECEIVING];
	if (!crnet_win_endpoint_attach(endpoint, crnet_win_open_tcp(storage.ss_family), FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE))
		return crnet_win_socket_failed(record, CR_RESULT_NET_CONNECT_FAILED, WSAGetLastError());

	// On a non-blocking socket connect() only starts the attempt; Winsock
	// reports it finished, and SO_ERROR says how it went.
	ResetEvent(endpoint->writable_event);
	if (connect(endpoint->handle, (struct sockaddr *)&storage, length) != 0) {
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return crnet_win_connection_failed(record, CR_RESULT_NET_CONNECT_FAILED, WSAGetLastError());

		CR_RESULT waited = crnet_win_endpoint_wait(endpoint, endpoint->writable_event, start, desc->timeout_ms);
		if (CR_FAILED(waited))
			return waited;

		int error        = 0;
		int error_length = sizeof(error);
		if (getsockopt(endpoint->handle, SOL_SOCKET, SO_ERROR, (char *)&error, &error_length) != 0)
			return crnet_win_socket_failed(record, CR_RESULT_NET_CONNECT_FAILED, WSAGetLastError());
		if (error != 0)
			return crnet_win_connection_failed(record, CR_RESULT_NET_CONNECT_FAILED, error);
	}

	InterlockedExchange(&socket->open, 1);
	return CR_SUCCESS;
}

CR_RESULT crnet_tcp_socket_send(
    crnet_tcp_socket socket,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_sent) {
	if (!out_sent)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_sent         = 0;
	CR_RESULT usable = crnet_win_socket_usable(socket, "crnet_tcp_socket_send");
	if (CR_FAILED(usable))
		return usable;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;

	struct crnet_win_endpoint *endpoint = &socket->endpoint;
	ULONGLONG                    start    = GetTickCount64();
	int                          room     = size > (uint64_t)INT_MAX ? INT_MAX : (int)size;
	for (;;) {
		ResetEvent(endpoint->writable_event);
		int sent = send(endpoint->handle, (const char *)data, room, 0);
		if (sent > 0) {
			*out_sent = (uint64_t)sent;
			return CR_SUCCESS;
		}
		if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
			return crnet_win_connection_failed(&socket->errors[CRNET_ERROR_SIDE_SENDING], CR_RESULT_NET_SEND_FAILED, WSAGetLastError());

		CR_RESULT waited = crnet_win_endpoint_wait(endpoint, endpoint->writable_event, start, timeout_ms);
		if (CR_FAILED(waited))
			return waited;
	}
}

CR_RESULT crnet_tcp_socket_recv(
    crnet_tcp_socket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received) {
	if (!out_received)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_received     = 0;
	CR_RESULT usable = crnet_win_socket_usable(socket, "crnet_tcp_socket_recv");
	if (CR_FAILED(usable))
		return usable;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;

	struct crnet_win_endpoint *endpoint = &socket->endpoint;
	ULONGLONG                    start    = GetTickCount64();
	int                          room     = size > (uint64_t)INT_MAX ? INT_MAX : (int)size;
	for (;;) {
		ResetEvent(endpoint->readable_event);
		int received = recv(endpoint->handle, (char *)data, room, 0);
		if (received >= 0) {
			*out_received = (uint64_t)received; // 0: the peer has finished sending
			return CR_SUCCESS;
		}
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return crnet_win_connection_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], CR_RESULT_NET_RECV_FAILED, WSAGetLastError());

		CR_RESULT waited = crnet_win_endpoint_wait(endpoint, endpoint->readable_event, start, timeout_ms);
		if (CR_FAILED(waited))
			return waited;
	}
}

CR_RESULT crnet_tcp_socket_finish_sending(crnet_tcp_socket socket) {
	CR_RESULT usable = crnet_win_socket_usable(socket, "crnet_tcp_socket_finish_sending");
	if (CR_FAILED(usable))
		return usable;
	if (shutdown(socket->endpoint.handle, SD_SEND) != 0)
		return crnet_win_socket_failed(&socket->errors[CRNET_ERROR_SIDE_SENDING], CR_RESULT_NET_SEND_FAILED, WSAGetLastError());
	return CR_SUCCESS;
}

CR_RESULT crnet_tcp_socket_set_no_delay(
    crnet_tcp_socket socket,
    bool no_delay) {
	CR_RESULT usable = crnet_win_socket_usable(socket, "crnet_tcp_socket_set_no_delay");
	if (CR_FAILED(usable))
		return usable;
	BOOL value = no_delay ? TRUE : FALSE;
	if (setsockopt(socket->endpoint.handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&value, sizeof(value)) != 0)
		return crnet_win_socket_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], CR_RESULT_GENERAL_FAILURE, WSAGetLastError());
	return CR_SUCCESS;
}

CR_RESULT crnet_tcp_socket_get_peer_address(
    crnet_tcp_socket socket,
    char out_address[CRNET_MAX_ADDRESS_STRING_LEN],
    uint16_t *out_port) {
	if (!out_address || !out_port)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	out_address[0]    = '\0';
	*out_port         = 0;
	CR_RESULT usable = crnet_win_socket_usable(socket, "crnet_tcp_socket_get_peer_address");
	if (CR_FAILED(usable))
		return usable;

	struct sockaddr_storage storage;
	int                     length = sizeof(storage);
	if (getpeername(socket->endpoint.handle, (struct sockaddr *)&storage, &length) != 0)
		return crnet_win_socket_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], CR_RESULT_GENERAL_FAILURE, WSAGetLastError());

	const void *bytes = NULL;
	if (storage.ss_family == AF_INET6) {
		bytes     = &((struct sockaddr_in6 *)&storage)->sin6_addr;
		*out_port = ntohs(((struct sockaddr_in6 *)&storage)->sin6_port);
	} else {
		bytes     = &((struct sockaddr_in *)&storage)->sin_addr;
		*out_port = ntohs(((struct sockaddr_in *)&storage)->sin_port);
	}
	if (!inet_ntop(storage.ss_family, bytes, out_address, CRNET_MAX_ADDRESS_STRING_LEN)) {
		out_address[0] = '\0';
		*out_port      = 0;
		return CR_RESULT_GENERAL_FAILURE;
	}
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// Resolver
// --------------------------------------------------------------------------

/* The lookup and the waiting for it are two calls on two threads of the
 * caller's, as crnet.h sets out: crnet_resolver_run does the lookup and
 * leaves the answer here, crnet_resolver_wait waits for the event run
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
struct crnet_resolver_t {
	cr_object_head head;
	volatile LONG    references;
	bool          started; // WSAStartup succeeded and WSACleanup is owed

	// Set by create and only read after.
	wchar_t               *host;
	size_t                 host_count; // the wchar_ts `host` was allocated with
	CRNET_ADDRESS_FAMILY family;

	HANDLE        abort_event; // set by abort and never reset
	volatile LONG aborted;
	volatile LONG run_called;
	HANDLE        done_event;  // set, for good, once run has finished

	// What run leaves for wait and get_error.
	CRITICAL_SECTION lock;
	bool             finished;
	CR_RESULT       result;
	char (*addresses)[CRNET_MAX_ADDRESS_STRING_LEN];
	uint32_t      address_count;
	uint32_t      address_capacity; // the entries `addresses` was allocated with
	crnet_error error;
};

static void crnet_win_resolver_release(struct crnet_resolver_t *resolver) {
	if (InterlockedDecrement(&resolver->references) != 0)
		return;
	if (resolver->abort_event)
		CloseHandle(resolver->abort_event);
	if (resolver->done_event)
		CloseHandle(resolver->done_event);
	DeleteCriticalSection(&resolver->lock);
	// The resolver's own copy: this may run after its instance is gone.
	const CR_ALLOCATOR allocator = resolver->head.allocator;
	cr_free_array(&allocator, resolver->addresses, resolver->address_capacity, CRNET_MAX_ADDRESS_STRING_LEN, 1);
	CR_DELETE_ARRAY(&allocator, wchar_t, resolver->host, resolver->host_count);
	bool started = resolver->started;
	cr_string_free(&allocator, resolver->head.debug_name);
	CR_DELETE(&allocator, struct crnet_resolver_t, resolver);
	// Last, with nothing of the resolver's left that could need Winsock.
	if (started)
		WSACleanup();
}

CR_RESULT crnet_resolver_create(
    crnet_instance instance,
    const crnet_resolver_desc *desc,
    crnet_resolver *out_resolver) {
	if (!out_resolver)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_resolver = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_resolver_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_RESOLVER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->host || desc->host[0] == '\0')
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	// GetAddrInfoExW takes the name as UTF-16. 0 here means the bytes
	// weren't UTF-8.
	int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc->host, -1, NULL, 0);
	if (wide_count <= 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	struct crnet_resolver_t *resolver = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_resolver_t);
	if (!resolver)
		return CR_RESULT_OUT_OF_MEMORY;
	resolver->head.allocator = instance->head.allocator;
	CR_BIRTH(resolver);
	// One for this function's failure paths to let go of; the second is
	// added only once there is a resolver to hand out.
	resolver->references = 1;
	InitializeCriticalSection(&resolver->lock);
	resolver->family      = desc->family;
	resolver->abort_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	resolver->done_event  = CreateEventW(NULL, TRUE, FALSE, NULL);
	resolver->host        = CR_NEW_ARRAY(&resolver->head.allocator, wchar_t, (size_t)wide_count);
	if (resolver->host)
		resolver->host_count = (size_t)wide_count;
	if (!resolver->abort_event || !resolver->done_event || !resolver->host ||
	    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc->host, -1, resolver->host, wide_count) <= 0) {
		crnet_win_resolver_release(resolver);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	// The resolver's own start of Winsock; see the struct. crnet.h gives
	// this call no error record to fill, so only the result says it failed.
	CR_RESULT started = crnet_win_start(NULL);
	if (CR_FAILED(started)) {
		crnet_win_resolver_release(resolver);
		return started;
	}
	resolver->started = true;

	InterlockedExchange(&resolver->references, 2);
	*out_resolver = resolver;
	return CR_SUCCESS;
}

void crnet_resolver_destroy(crnet_resolver resolver) {
	if (!resolver)
		return;
	if (CR_STALE(resolver, "crnet_resolver_destroy", "resolver"))
		return;
	// Dead from here, though the run may still hold the memory.
	CR_KILL(resolver);
	crnet_win_resolver_release(resolver);
}

void crnet_resolver_abort(crnet_resolver resolver) {
	if (!resolver)
		return;
	if (CR_STALE(resolver, "crnet_resolver_abort", "resolver"))
		return;
	InterlockedExchange(&resolver->aborted, 1);
	SetEvent(resolver->abort_event);
}

void crnet_resolver_get_error(
    crnet_resolver resolver,
    crnet_error *out_error) {
	if (!resolver) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(resolver, "crnet_resolver_get_error", "resolver")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	// Written by the thread in run, so read under the lock it writes under.
	EnterCriticalSection(&resolver->lock);
	crnet_error_get(&resolver->error, out_error);
	LeaveCriticalSection(&resolver->lock);
}

CR_RESULT crnet_resolver_run(crnet_resolver resolver) {
	if (!resolver)
		return CR_RESULT_NET_INVALID_RESOLVER;
	// No stale check: the run may start after the resolver is destroyed
	// (crnet.h), which kills its tag at the destroy call while the run's
	// reference keeps the memory.
	// A second run has no reference of its own to let go of at the end.
	if (InterlockedExchange(&resolver->run_called, 1) != 0)
		return CR_RESULT_NET_INVALID_RESOLVER;

	CR_RESULT    result = CR_RESULT_NET_ABORTED;
	crnet_error error;
	crnet_error_clear(&error);
	char (*addresses)[CRNET_MAX_ADDRESS_STRING_LEN] = NULL;
	uint32_t address_count                            = 0;
	uint32_t address_capacity                         = 0;

	// GetAddrInfoExW reports through this event when it couldn't answer on
	// the spot. Without it there is no lookup to make.
	HANDLE answered = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!answered) {
		result = crnet_win_socket_failed(&error, CR_RESULT_GENERAL_FAILURE, (int)GetLastError());
	} else if (InterlockedCompareExchange(&resolver->aborted, 0, 0) == 0) {
		ADDRINFOEXW hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_STREAM; // one entry per address, not one per socket type
		switch (resolver->family) {
		case CRNET_ADDRESS_FAMILY_IPV4:
			hints.ai_family = AF_INET;
			break;
		case CRNET_ADDRESS_FAMILY_IPV6:
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
			result = crnet_win_socket_failed(&error, CR_RESULT_NET_RESOLVE_FAILED, failed);
		} else {
			uint32_t entries = 0;
			for (PADDRINFOEXW entry = found; entry != NULL; entry = entry->ai_next)
				entries++;
			result = CR_RESULT_NET_RESOLVE_FAILED; // until an address turns up
			// On the caller's thread in run, through the resolver's own
			// allocator (the instance may be gone by now).
			if (entries > 0) {
				addresses = (char (*)[CRNET_MAX_ADDRESS_STRING_LEN])cr_allocate_array(
				    &resolver->head.allocator, entries, CRNET_MAX_ADDRESS_STRING_LEN, 1, true);
				if (addresses)
					address_capacity = entries;
			}
			if (entries > 0 && !addresses)
				result = CR_RESULT_OUT_OF_MEMORY;

			for (PADDRINFOEXW entry = found; entry != NULL && addresses; entry = entry->ai_next) {
				const void *bytes = NULL;
				if (entry->ai_family == AF_INET)
					bytes = &((struct sockaddr_in *)entry->ai_addr)->sin_addr;
				else if (entry->ai_family == AF_INET6)
					bytes = &((struct sockaddr_in6 *)entry->ai_addr)->sin6_addr;
				else
					continue;
				char text[CRNET_MAX_ADDRESS_STRING_LEN];
				if (!inet_ntop(entry->ai_family, bytes, text, sizeof(text)))
					continue;

				// The resolver can list one address more than once.
				bool duplicate = false;
				for (uint32_t i = 0; i < address_count && !duplicate; ++i)
					duplicate = strcmp(addresses[i], text) == 0;
				if (duplicate)
					continue;
				memcpy(addresses[address_count++], text, strlen(text) + 1);
				result = CR_SUCCESS;
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
		result = CR_RESULT_NET_ABORTED;
	// The resolver may be gone after this: nothing of it is touched again.
	crnet_win_resolver_release(resolver);
	return result;
}

CR_RESULT crnet_resolver_wait(
    crnet_resolver resolver,
    uint32_t timeout_ms,
    char out_addresses[][CRNET_MAX_ADDRESS_STRING_LEN],
    uint32_t out_capacity,
    uint32_t *out_count) {
	if (!out_addresses || !out_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_count = 0;
	if (!resolver)
		return CR_RESULT_NET_INVALID_RESOLVER;
	if (CR_STALE(resolver, "crnet_resolver_wait", "resolver"))
		return CR_RESULT_NET_INVALID_RESOLVER;

	// Abort is listed first so it wins when both are set. The done event is
	// never reset, so once run has finished every wait returns at once.
	ULONGLONG start    = GetTickCount64();
	HANDLE    waits[2] = {resolver->abort_event, resolver->done_event};
	DWORD     waited   = WaitForMultipleObjects(2, waits, FALSE, crnet_win_socket_remaining(start, timeout_ms));
	if (waited == WAIT_OBJECT_0)
		return CR_RESULT_NET_ABORTED;
	if (waited != WAIT_OBJECT_0 + 1)
		return CR_RESULT_NET_TIMED_OUT;

	EnterCriticalSection(&resolver->lock);
	CR_RESULT result = resolver->result;
	if (!CR_FAILED(result)) {
		uint32_t count = resolver->address_count < out_capacity ? resolver->address_count : out_capacity;
		for (uint32_t i = 0; i < count; ++i)
			memcpy(out_addresses[i], resolver->addresses[i], CRNET_MAX_ADDRESS_STRING_LEN);
		*out_count = count;
	}
	LeaveCriticalSection(&resolver->lock);
	return result;
}

#if __cplusplus
}
#endif

#endif // CARROTTUS_PLATFORM_WIN32
