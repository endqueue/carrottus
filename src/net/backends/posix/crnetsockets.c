
#if CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE

#include "../../crnetshared.h"
#include "crnet.h"
#include "../../../crstructbase.h"

// Everything in this file is the part of Carrottus Net that CRNET_EXT_TCP says
// a platform has: where it is 0 (watchOS) the system refuses these calls
// to an ordinary application, and none of this is built.
#if CRNET_EXT_TCP

/*
 * Carrottus Net TCP sockets on BSD sockets - Linux, and every Apple platform
 * that allows them.
 *
 * Every descriptor here is non-blocking, and that is an internal matter:
 * crnet.h has no non-blocking mode. A call tries the operation, and if
 * the system says it would have to wait, waits itself in poll() - on the
 * connection, and on a pipe that abort writes to - for as long as the
 * caller's time limit allows, then tries again. That one wait is what
 * makes the time limit mean the same thing as on the other backends and
 * what lets another thread end it.
 *
 * crnet_instance_create/_destroy are not here: BSD sockets have nothing
 * to start, and each platform's HTTP backend defines them for what it
 * needs. The objects here keep the instance they were made with all the
 * same, for a wait set to compare.
 *
 * The resolver is here too, at the end: getaddrinfo is the same call on
 * both systems, and waiting for it is the same poll() as everything above.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if __cplusplus
extern "C" {
#endif

// A send on a connection the peer has closed raises SIGPIPE unless told
// not to, and the two systems are told differently: per call on Linux, per
// socket on Apple.
#ifdef MSG_NOSIGNAL
#define CRNET_POSIX_SEND_FLAGS MSG_NOSIGNAL
#else
#define CRNET_POSIX_SEND_FLAGS 0
#endif

// --------------------------------------------------------------------------
// Shared by listeners and sockets
// --------------------------------------------------------------------------

/* What abort acts on: a flag every call checks, and a pipe whose read end
 * sits in every poll() so a wait in progress ends too. */
struct crnet_posix_abort {
	atomic_bool aborted;
	int         wake_read;
	int         wake_write;
};

static bool crnet_posix_set_descriptor_flags(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
	int descriptor_flags = fcntl(fd, F_GETFD, 0);
	return descriptor_flags >= 0 && fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) >= 0;
}

static bool crnet_posix_abort_init(struct crnet_posix_abort *abort) {
	atomic_init(&abort->aborted, false);
	abort->wake_read  = -1;
	abort->wake_write = -1;
	int ends[2];
	if (pipe(ends) != 0)
		return false;
	abort->wake_read  = ends[0];
	abort->wake_write = ends[1];
	return crnet_posix_set_descriptor_flags(ends[0]) && crnet_posix_set_descriptor_flags(ends[1]);
}

static void crnet_posix_abort_free(struct crnet_posix_abort *abort) {
	if (abort->wake_read >= 0)
		close(abort->wake_read);
	if (abort->wake_write >= 0)
		close(abort->wake_write);
}

static void crnet_posix_abort_set(struct crnet_posix_abort *abort) {
	atomic_store(&abort->aborted, true);
	char byte = 1;
	if (write(abort->wake_write, &byte, 1) < 0) {
		// Full means it is already readable, which is all this is for.
	}
}

static uint64_t crnet_posix_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Milliseconds left of `timeout_ms` since `start`, as poll() takes them:
 * -1 for no limit. */
static int crnet_posix_remaining(uint64_t start, uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return -1;
	uint64_t elapsed = crnet_posix_now_ms() - start;
	if (elapsed >= timeout_ms)
		return 0;
	uint64_t left = timeout_ms - elapsed;
	return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}

/* Waits for `fd` to become readable or writable (`events`), for the time
 * to run out, or for an abort. CR_SUCCESS means it is worth trying the
 * operation again - which includes an error or hang-up on the descriptor,
 * since the operation is what reports those. */
static CR_RESULT crnet_posix_wait(int fd, short events, struct crnet_posix_abort *abort, uint64_t start, uint32_t timeout_ms) {
	for (;;) {
		if (atomic_load(&abort->aborted))
			return CR_RESULT_NET_ABORTED;

		struct pollfd waited[2];
		waited[0].fd      = fd;
		waited[0].events  = events;
		waited[0].revents = 0;
		waited[1].fd      = abort->wake_read;
		waited[1].events  = POLLIN;
		waited[1].revents = 0;
		int ready         = poll(waited, 2, crnet_posix_remaining(start, timeout_ms));
		if (ready == 0)
			return CR_RESULT_NET_TIMED_OUT;
		if (ready < 0 && errno == EINTR)
			continue;
		if (atomic_load(&abort->aborted))
			return CR_RESULT_NET_ABORTED;
		return CR_SUCCESS;
	}
}

/* A failed system call as a record: `error` is the errno it left. Returns
 * `result`. */
static CR_RESULT crnet_posix_failed(crnet_error *record, CR_RESULT result, int error) {
	return crnet_error_record(record, result, CR_PLATFORM_ERROR_SOURCE_ERRNO, error, NULL);
}

/* The same for a call on a connection, where one errno is a result of its
 * own: the system stopped waiting for the other end - a connect nobody
 * answered, a peer that went silent - with time still left on the caller's
 * limit. `otherwise` is the result for every other errno. */
static CR_RESULT crnet_posix_connection_failed(crnet_error *record, CR_RESULT otherwise, int error) {
	return crnet_posix_failed(record, error == ETIMEDOUT ? CR_RESULT_NET_SYSTEM_TIMED_OUT : otherwise, error);
}

/* A numeric address and a port as the sockaddr bind() and connect() take.
 * AI_NUMERICHOST keeps getaddrinfo to parsing: it never asks a name server,
 * so it never waits. False for anything that isn't a numeric address. */
static bool crnet_posix_parse_address(const char *address, uint16_t port, struct sockaddr_storage *out_storage, socklen_t *out_length) {
	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = AI_NUMERICHOST | AI_NUMERICSERV;

	struct addrinfo *parsed = NULL;
	if (getaddrinfo(address, port_text, &hints, &parsed) != 0 || !parsed)
		return false;
	bool usable = (parsed->ai_family == AF_INET || parsed->ai_family == AF_INET6) && parsed->ai_addrlen <= sizeof(*out_storage);
	if (usable) {
		memset(out_storage, 0, sizeof(*out_storage));
		memcpy(out_storage, parsed->ai_addr, parsed->ai_addrlen);
		*out_length = (socklen_t)parsed->ai_addrlen;
	}
	freeaddrinfo(parsed);
	return usable;
}

/* A descriptor set up the way every one here is. -1 on failure. */
static int crnet_posix_prepare(int fd) {
	if (fd < 0)
		return -1;
	if (!crnet_posix_set_descriptor_flags(fd)) {
		close(fd);
		return -1;
	}
#ifdef SO_NOSIGPIPE
	int no_sigpipe = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
	return fd;
}

/* A new TCP descriptor. Its own function because the public calls below
 * name their parameter `socket`, as crnet.h does, which hides socket(). */
static int crnet_posix_open_tcp(int family) {
	return crnet_posix_prepare(socket(family, SOCK_STREAM, IPPROTO_TCP));
}

// --------------------------------------------------------------------------
// The two objects
// --------------------------------------------------------------------------

struct crnet_tcp_listener_t {
	cr_object_head head; // its allocator, also every socket's it accepts
	crnet_instance           instance; // also every socket's it accepts
	int                        fd;
	struct crnet_posix_abort abort;
	// The last accept that failed.
	crnet_error error;
};

struct crnet_tcp_socket_t {
	cr_object_head head;
	crnet_instance instance;
	// -1 until connect makes it; read by other calls only once `open` says
	// the connection is up, and closed only by destroy.
	int                        fd;
	struct crnet_posix_abort abort;
	atomic_bool                connect_called;
	atomic_bool                open;
	// By CRNET_ERROR_SIDE; each is written only by its side's caller.
	crnet_error errors[CRNET_ERROR_SIDE_COUNT];
};

/* A socket with its abort set up and no descriptor, made through
 * `allocator` (its instance's, or for an accepted socket its listener's),
 * which it keeps a copy of. NULL on failure. */
static struct crnet_tcp_socket_t *crnet_posix_socket_alloc(crnet_instance instance, const CR_ALLOCATOR *allocator) {
	struct crnet_tcp_socket_t *socket = CR_NEW_ZEROED(allocator, struct crnet_tcp_socket_t);
	if (!socket)
		return NULL;
	socket->head.allocator = *allocator;
	CR_BIRTH_IN(socket, &instance->quarantine);
	socket->instance       = instance;
	socket->fd             = -1;
	atomic_init(&socket->connect_called, false);
	atomic_init(&socket->open, false);
	if (!crnet_posix_abort_init(&socket->abort)) {
		crnet_posix_abort_free(&socket->abort);
		CR_DELETE(allocator, struct crnet_tcp_socket_t, socket);
		return NULL;
	}
	return socket;
}

// For a wait set to poll (see the end of crnetshared.h).

bool crnet_tcp_socket_native(crnet_tcp_socket socket, intptr_t *out_native) {
	if (!socket || !atomic_load(&socket->open))
		return false;
	*out_native = socket->fd;
	return true;
}

bool crnet_tcp_listener_native(crnet_tcp_listener listener, intptr_t *out_native) {
	if (!listener || listener->fd < 0)
		return false;
	*out_native = listener->fd;
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
	if (listener->fd >= 0)
		close(listener->fd);
	crnet_posix_abort_free(&listener->abort);
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
	socklen_t               length = 0;
	if (!crnet_posix_parse_address(desc->address, desc->port, &storage, &length))
		return CR_RESULT_NET_INVALID_ADDRESS;

	struct crnet_tcp_listener_t *listener = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_tcp_listener_t);
	if (!listener)
		return CR_RESULT_OUT_OF_MEMORY;
	listener->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(listener, &instance->quarantine);
	listener->instance       = instance;
	listener->fd             = -1;
	if (!crnet_posix_abort_init(&listener->abort)) {
		int error = errno;
		crnet_tcp_listener_destroy(listener);
		return crnet_posix_failed(out_error, CR_RESULT_GENERAL_FAILURE, error);
	}

	CR_RESULT result = CR_RESULT_NET_LISTEN_FAILED;
	listener->fd      = crnet_posix_open_tcp(storage.ss_family);
	if (listener->fd < 0)
		goto failed;

	// Whether an IPv6 listener also takes IPv4 is a system setting that
	// differs between platforms; fixed to "no" so it is the same on all.
	if (storage.ss_family == AF_INET6) {
		int v6_only = 1;
		if (setsockopt(listener->fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only)) != 0)
			goto failed;
	}
	// Left to itself the system refuses the port for a minute or two after
	// its last listener went, while it holds that listener's old
	// connections. Windows has no such wait, so there is none anywhere.
	{
		int reuse = 1;
		if (setsockopt(listener->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
			goto failed;
	}

	if (bind(listener->fd, (struct sockaddr *)&storage, length) != 0) {
		if (errno == EADDRINUSE)
			result = CR_RESULT_NET_ADDRESS_IN_USE;
		goto failed;
	}
	if (listen(listener->fd, desc->backlog > (uint32_t)INT_MAX ? INT_MAX : (int)desc->backlog) != 0) {
		if (errno == EADDRINUSE)
			result = CR_RESULT_NET_ADDRESS_IN_USE;
		goto failed;
	}

	*out_listener = listener;
	return CR_SUCCESS;

failed:;
	// Every jump here comes straight from the call that failed.
	int error = errno;
	crnet_tcp_listener_destroy(listener);
	return crnet_posix_failed(out_error, result, error);
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
	socklen_t               length = sizeof(storage);
	if (getsockname(listener->fd, (struct sockaddr *)&storage, &length) != 0)
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

	uint64_t start = crnet_posix_now_ms();
	for (;;) {
		if (atomic_load(&listener->abort.aborted))
			return CR_RESULT_NET_ABORTED;

		int fd = accept(listener->fd, NULL, NULL);
		if (fd >= 0) {
			struct crnet_tcp_socket_t *socket = crnet_posix_socket_alloc(listener->instance, &listener->head.allocator);
			if (!socket) {
				close(fd);
				return CR_RESULT_OUT_OF_MEMORY;
			}
			socket->fd = crnet_posix_prepare(fd);
			if (socket->fd < 0) {
				int error = errno;
				crnet_tcp_socket_destroy(socket);
				return crnet_posix_failed(&listener->error, CR_RESULT_NET_ACCEPT_FAILED, error);
			}
			atomic_store(&socket->connect_called, true);
			atomic_store(&socket->open, true);
			*out_socket = socket;
			return CR_SUCCESS;
		}

		// A connection that came and went before it was accepted is not
		// the listener failing; the next one is tried.
		if (errno == EINTR || errno == ECONNABORTED)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return crnet_posix_failed(&listener->error, CR_RESULT_NET_ACCEPT_FAILED, errno);

		CR_RESULT waited = crnet_posix_wait(listener->fd, POLLIN, &listener->abort, start, timeout_ms);
		if (CR_FAILED(waited))
			return waited;
	}
}

void crnet_tcp_listener_abort(crnet_tcp_listener listener) {
	if (!listener)
		return;
	if (CR_STALE(listener, "crnet_tcp_listener_abort", "listener"))
		return;
	crnet_posix_abort_set(&listener->abort);
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
	*out_socket = crnet_posix_socket_alloc(instance, &instance->head.allocator);
	return *out_socket ? CR_SUCCESS : CR_RESULT_OUT_OF_MEMORY;
}

void crnet_tcp_socket_destroy(crnet_tcp_socket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_tcp_socket_destroy", "socket"))
		return;
	if (socket->fd >= 0)
		close(socket->fd);
	crnet_posix_abort_free(&socket->abort);
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
	crnet_posix_abort_set(&socket->abort);
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
static CR_RESULT crnet_posix_socket_usable(struct crnet_tcp_socket_t *socket, const char *function) {
	(void)function; // read by the stale check only, which a Release build has none of
	if (!socket)
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (CR_STALE(socket, function, "socket"))
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (atomic_load(&socket->abort.aborted))
		return CR_RESULT_NET_ABORTED;
	if (!atomic_load(&socket->open))
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

	uint64_t                start = crnet_posix_now_ms();
	struct sockaddr_storage storage;
	socklen_t               length = 0;
	if (!crnet_posix_parse_address(desc->address, desc->port, &storage, &length))
		return CR_RESULT_NET_INVALID_ADDRESS;

	if (atomic_load(&socket->abort.aborted))
		return CR_RESULT_NET_ABORTED;
	if (atomic_load(&socket->connect_called))
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (desc->timeout_ms == CRNET_NO_WAIT)
		return CR_RESULT_NET_TIMED_OUT;
	// From here the socket is spent unless this succeeds.
	atomic_store(&socket->connect_called, true);

	crnet_error *record = &socket->errors[CRNET_ERROR_SIDE_RECEIVING];
	socket->fd            = crnet_posix_open_tcp(storage.ss_family);
	if (socket->fd < 0)
		return crnet_posix_failed(record, CR_RESULT_NET_CONNECT_FAILED, errno);

	// On a non-blocking descriptor connect() only starts the attempt; the
	// descriptor turns writable when it has ended, and SO_ERROR says how.
	if (connect(socket->fd, (struct sockaddr *)&storage, length) != 0) {
		if (errno != EINPROGRESS && errno != EINTR)
			return crnet_posix_connection_failed(record, CR_RESULT_NET_CONNECT_FAILED, errno);

		CR_RESULT waited = crnet_posix_wait(socket->fd, POLLOUT, &socket->abort, start, desc->timeout_ms);
		if (CR_FAILED(waited))
			return waited;

		int       error        = 0;
		socklen_t error_length = sizeof(error);
		if (getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0)
			return crnet_posix_failed(record, CR_RESULT_NET_CONNECT_FAILED, errno);
		if (error != 0)
			return crnet_posix_connection_failed(record, CR_RESULT_NET_CONNECT_FAILED, error);
	}

	atomic_store(&socket->open, true);
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
	CR_RESULT usable = crnet_posix_socket_usable(socket, "crnet_tcp_socket_send");
	if (CR_FAILED(usable))
		return usable;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;

	uint64_t start = crnet_posix_now_ms();
	size_t   room  = size > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)size;
	for (;;) {
		ssize_t sent = send(socket->fd, data, room, CRNET_POSIX_SEND_FLAGS);
		if (sent > 0) {
			*out_sent = (uint64_t)sent;
			return CR_SUCCESS;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return crnet_posix_connection_failed(&socket->errors[CRNET_ERROR_SIDE_SENDING], CR_RESULT_NET_SEND_FAILED, errno);

		CR_RESULT waited = crnet_posix_wait(socket->fd, POLLOUT, &socket->abort, start, timeout_ms);
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
	CR_RESULT usable = crnet_posix_socket_usable(socket, "crnet_tcp_socket_recv");
	if (CR_FAILED(usable))
		return usable;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;

	uint64_t start = crnet_posix_now_ms();
	size_t   room  = size > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)size;
	for (;;) {
		ssize_t received = recv(socket->fd, data, room, 0);
		if (received >= 0) {
			*out_received = (uint64_t)received; // 0: the peer has finished sending
			return CR_SUCCESS;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return crnet_posix_connection_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], CR_RESULT_NET_RECV_FAILED, errno);

		CR_RESULT waited = crnet_posix_wait(socket->fd, POLLIN, &socket->abort, start, timeout_ms);
		if (CR_FAILED(waited))
			return waited;
	}
}

CR_RESULT crnet_tcp_socket_finish_sending(crnet_tcp_socket socket) {
	CR_RESULT usable = crnet_posix_socket_usable(socket, "crnet_tcp_socket_finish_sending");
	if (CR_FAILED(usable))
		return usable;
	if (shutdown(socket->fd, SHUT_WR) != 0)
		return crnet_posix_failed(&socket->errors[CRNET_ERROR_SIDE_SENDING], CR_RESULT_NET_SEND_FAILED, errno);
	return CR_SUCCESS;
}

CR_RESULT crnet_tcp_socket_set_no_delay(
    crnet_tcp_socket socket,
    bool no_delay) {
	CR_RESULT usable = crnet_posix_socket_usable(socket, "crnet_tcp_socket_set_no_delay");
	if (CR_FAILED(usable))
		return usable;
	int value = no_delay ? 1 : 0;
	if (setsockopt(socket->fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0)
		return crnet_posix_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], CR_RESULT_GENERAL_FAILURE, errno);
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
	CR_RESULT usable = crnet_posix_socket_usable(socket, "crnet_tcp_socket_get_peer_address");
	if (CR_FAILED(usable))
		return usable;

	struct sockaddr_storage storage;
	socklen_t               length = sizeof(storage);
	if (getpeername(socket->fd, (struct sockaddr *)&storage, &length) != 0)
		return crnet_posix_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], CR_RESULT_GENERAL_FAILURE, errno);

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

/* getaddrinfo can be neither given a time limit nor interrupted, so the
 * lookup and the waiting for it are two calls on two threads of the
 * caller's. crnet_resolver_run does the lookup, for as long as the system
 * takes, and leaves the answer here; crnet_resolver_wait waits for that
 * the way every wait in this file is done - poll() on a descriptor, beside
 * the abort pipe - the descriptor being a pipe run writes to when it is
 * through.
 *
 * Either thread may be the last to need the resolver: a waiter that gave
 * up destroys it while the lookup is still out, and a lookup that finishes
 * first leaves it for the waiter. So it is held twice from the start, once
 * for run and once for destroy, and freed when both have let go.
 *
 * The thread in run may also outlast the instance the resolver was made
 * with. Nothing has to be done about that here: an instance holds a start
 * of the platform's networking, and getaddrinfo on these platforms needs
 * none (the instance's is libcurl's on Linux, and nothing at all on
 * Apple's systems). The resolver keeps nothing of the instance but a copy
 * of its allocator, in its head, which is what it is freed through - by
 * whichever of run and destroy lets go last, so possibly on the thread in
 * run, after the instance is gone. */
struct crnet_resolver_t {
	cr_object_head head;
	atomic_int       references;

	// Set by create and only read after.
	char                  *host;
	CRNET_ADDRESS_FAMILY family;

	struct crnet_posix_abort abort;
	atomic_bool                run_called;
	// Readable, for good, once run has finished.
	int done_read;
	int done_write;

	// What run leaves for wait and get_error.
	pthread_mutex_t lock;
	bool            finished;
	CR_RESULT      result;
	char (*addresses)[CRNET_MAX_ADDRESS_STRING_LEN];
	uint32_t      address_count;
	uint32_t      address_capacity; // the entries `addresses` was allocated with
	crnet_error error;
};

static void crnet_posix_resolver_release(struct crnet_resolver_t *resolver) {
	if (atomic_fetch_sub(&resolver->references, 1) != 1)
		return;
	crnet_posix_abort_free(&resolver->abort);
	if (resolver->done_read >= 0)
		close(resolver->done_read);
	if (resolver->done_write >= 0)
		close(resolver->done_write);
	pthread_mutex_destroy(&resolver->lock);
	// The resolver's own copy: this may run after its instance is gone.
	const CR_ALLOCATOR allocator = resolver->head.allocator;
	cr_free_array(&allocator, resolver->addresses, resolver->address_capacity, CRNET_MAX_ADDRESS_STRING_LEN, 1);
	cr_string_free(&allocator, resolver->host);
	cr_string_free(&allocator, resolver->head.debug_name);
	CR_DELETE(&allocator, struct crnet_resolver_t, resolver);
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

	struct crnet_resolver_t *resolver = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_resolver_t);
	if (!resolver)
		return CR_RESULT_OUT_OF_MEMORY;
	resolver->head.allocator = instance->head.allocator;
	CR_BIRTH(resolver);
	// One for this function's failure paths to let go of; the second is
	// added only once there is a resolver to hand out.
	atomic_init(&resolver->references, 1);
	atomic_init(&resolver->run_called, false);
	pthread_mutex_init(&resolver->lock, NULL);
	// Descriptor 0 is a real one; these say "none yet" for the release
	// below, whichever step fails.
	resolver->abort.wake_read  = -1;
	resolver->abort.wake_write = -1;
	resolver->done_read        = -1;
	resolver->done_write       = -1;
	resolver->family           = desc->family;

	resolver->host       = cr_string_copy(&resolver->head.allocator, desc->host);
	int  ends[2]         = {-1, -1};
	bool made            = resolver->host && crnet_posix_abort_init(&resolver->abort) && pipe(ends) == 0;
	resolver->done_read  = ends[0];
	resolver->done_write = ends[1];
	if (!made || !crnet_posix_set_descriptor_flags(ends[0]) || !crnet_posix_set_descriptor_flags(ends[1])) {
		bool out_of_memory = !resolver->host;
		crnet_posix_resolver_release(resolver);
		return out_of_memory ? CR_RESULT_OUT_OF_MEMORY : CR_RESULT_GENERAL_FAILURE;
	}

	atomic_store(&resolver->references, 2);
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
	crnet_posix_resolver_release(resolver);
}

void crnet_resolver_abort(crnet_resolver resolver) {
	if (!resolver)
		return;
	if (CR_STALE(resolver, "crnet_resolver_abort", "resolver"))
		return;
	crnet_posix_abort_set(&resolver->abort);
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
	pthread_mutex_lock(&resolver->lock);
	crnet_error_get(&resolver->error, out_error);
	pthread_mutex_unlock(&resolver->lock);
}

CR_RESULT crnet_resolver_run(crnet_resolver resolver) {
	if (!resolver)
		return CR_RESULT_NET_INVALID_RESOLVER;
	// No stale check: the run may start after the resolver is destroyed
	// (crnet.h), which kills its tag at the destroy call while the run's
	// reference keeps the memory.
	// A second run has no reference of its own to let go of at the end.
	if (atomic_exchange(&resolver->run_called, true))
		return CR_RESULT_NET_INVALID_RESOLVER;

	CR_RESULT    result = CR_RESULT_NET_ABORTED;
	crnet_error error;
	crnet_error_clear(&error);
	char (*addresses)[CRNET_MAX_ADDRESS_STRING_LEN] = NULL;
	uint32_t address_count                            = 0;
	uint32_t address_capacity                         = 0;

	// An abort that came first spares the lookup; one that comes during it
	// can't stop it.
	if (!atomic_load(&resolver->abort.aborted)) {
		struct addrinfo hints;
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

		struct addrinfo *found  = NULL;
		int              failed = getaddrinfo(resolver->host, NULL, &hints, &found);
		if (failed == EAI_SYSTEM) {
			result = crnet_posix_failed(&error, CR_RESULT_NET_RESOLVE_FAILED, errno);
		} else if (failed != 0) {
			result = crnet_error_record(&error, CR_RESULT_NET_RESOLVE_FAILED, CR_PLATFORM_ERROR_SOURCE_RESOLVER, failed, NULL);
		} else {
			uint32_t entries = 0;
			for (struct addrinfo *entry = found; entry != NULL; entry = entry->ai_next)
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

			for (struct addrinfo *entry = found; entry != NULL && addresses; entry = entry->ai_next) {
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
			freeaddrinfo(found);
	}

	pthread_mutex_lock(&resolver->lock);
	resolver->finished      = true;
	resolver->result        = result;
	resolver->addresses        = addresses;
	resolver->address_count    = address_count;
	resolver->address_capacity = address_capacity;
	resolver->error         = error;
	pthread_mutex_unlock(&resolver->lock);
	char byte = 1;
	if (write(resolver->done_write, &byte, 1) < 0) {
		// Nothing else writes to it, so there is always room for the one byte.
	}

	// The answer stays for a waiter that comes later; what this call
	// reports is what a wait would report now.
	if (atomic_load(&resolver->abort.aborted))
		result = CR_RESULT_NET_ABORTED;
	// The resolver may be gone after this: nothing of it is touched again.
	crnet_posix_resolver_release(resolver);
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

	// The done pipe is never emptied, so once run has finished every wait
	// returns at once, this one and any after it.
	uint64_t   start  = crnet_posix_now_ms();
	CR_RESULT waited = crnet_posix_wait(resolver->done_read, POLLIN, &resolver->abort, start, timeout_ms);
	if (CR_FAILED(waited))
		return waited;

	pthread_mutex_lock(&resolver->lock);
	// poll() also wakes for a descriptor that has gone wrong.
	CR_RESULT result = resolver->finished ? resolver->result : CR_RESULT_GENERAL_FAILURE;
	if (!CR_FAILED(result)) {
		uint32_t count = resolver->address_count < out_capacity ? resolver->address_count : out_capacity;
		for (uint32_t i = 0; i < count; ++i)
			memcpy(out_addresses[i], resolver->addresses[i], CRNET_MAX_ADDRESS_STRING_LEN);
		*out_count = count;
	}
	pthread_mutex_unlock(&resolver->lock);
	return result;
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_TCP

#endif // CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_APPLE
