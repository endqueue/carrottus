
/*
 * Carrottus Net wait set - one file for every platform.
 *
 * poll() is the same call on Linux and Apple, and WSAPoll is Winsock's
 * spelling of it, so unlike the sockets themselves this needs no backend of
 * its own: the differences are a handful of names, set out below.
 *
 * A wait is one poll() over the sockets of the set's members and one socket
 * of the set's own, the wake socket, which is how anything else ends the
 * wait: crnet_wait_set_abort, crnet_wait_set_wake, and a WebSocket whose stack reports through
 * callbacks rather than through a socket the system can poll (NSURLSession,
 * WinHTTP). Such a member has no entry in the poll; it writes to the wake
 * socket when its state changes, and the wait asks it directly whether it
 * is ready (see the end of crnetshared.h).
 *
 * The wake socket is a pipe where there are pipes. Winsock can't poll one,
 * so there it is a UDP socket bound to the loopback address that sends
 * datagrams to itself. Another process on the machine could send to that
 * port too; all it would cause is a wake with nothing to report, which the
 * wait already treats as a reason to look again.
 *
 * poll() is asked afresh on every wait and costs time in proportion to the
 * members. That is the price of one implementation; epoll and kqueue (and
 * nothing as direct on Windows) would lift it behind the same header if a
 * set ever holds enough to matter.
 */

#include "crnetshared.h"
#include "crnet.h"

// A wait set waits on TCP objects and WebSockets and on nothing else, so a
// build with neither (watchOS) has no wait set, and each kind of member is
// only here where its part of Carrottus Net is.
#if CRNET_EXT_TCP || CRNET_EXT_WEBSOCKET

#if CARROTTUS_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#endif

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// The names that differ
// --------------------------------------------------------------------------

#if CARROTTUS_PLATFORM_WIN32

typedef WSAPOLLFD crnet_poll_entry;
typedef SOCKET    crnet_poll_socket;
// WSAPoll refuses the out-of-band bits the POSIX names include.
#define CRNET_POLL_INCOMING POLLRDNORM
#define CRNET_POLL_ROOM     POLLWRNORM
#define CRNET_POLL_ENDED    (POLLERR | POLLHUP | POLLNVAL)

#else

typedef struct pollfd crnet_poll_entry;
typedef int           crnet_poll_socket;
#define CRNET_POLL_INCOMING POLLIN
#define CRNET_POLL_ROOM     POLLOUT
#define CRNET_POLL_ENDED    (POLLERR | POLLHUP | POLLNVAL)

#endif

// --------------------------------------------------------------------------
// The set
// --------------------------------------------------------------------------

enum crnet_wait_kind {
	CRNET_WAIT_KIND_TCP_SOCKET,
	CRNET_WAIT_KIND_TCP_LISTENER,
	CRNET_WAIT_KIND_WEBSOCKET,
};

struct crnet_wait_member {
	enum crnet_wait_kind kind;
	void                  *object; // the handle it was added as
	void                  *user;
	uint32_t               wait_for;
	// A socket for poll(), or none: a WebSocket that notifies and is asked.
	bool     has_native;
	intptr_t native;
	// Named by the next wait whatever poll() says, then cleared.
	bool report_once;
};

typedef EQ_ARRAY(struct crnet_wait_member) crnet_wait_member_array;
typedef EQ_ARRAY(crnet_poll_entry) crnet_poll_entry_array;

struct crnet_wait_set_t {
	cr_object_head head;
	// The one it was made with, and every member's.
	crnet_instance instance;
	// eqstd's view of head.allocator (cr_eq_allocator), which both arrays
	// below point at.
	EQ_ALLOCATOR             allocator;
	crnet_wait_member_array members;
	// What poll() is handed: the wake socket, then a member's socket each.
	// One more than the members' capacity, kept between waits so a wait
	// allocates only when the set has grown.
	crnet_poll_entry_array entries;

#if CARROTTUS_PLATFORM_WIN32
	SOCKET             wake; // INVALID_SOCKET until made
	struct sockaddr_in wake_address;
	volatile LONG      aborted;
	volatile LONG      woken; // crnet_wait_set_wake, until a wait takes it
#else
	int         wake_read; // -1 until made
	int         wake_write;
	atomic_bool aborted;
	atomic_bool woken; // crnet_wait_set_wake, until a wait takes it
#endif

	// The last wait that failed.
	crnet_error error;
};

// --------------------------------------------------------------------------
// The wake socket, the clock and poll() itself, per platform
// --------------------------------------------------------------------------

#if CARROTTUS_PLATFORM_WIN32

/* The error a failed socket call left, as the record takes it. */
static CR_RESULT crnet_wait_failed(crnet_error *record, CR_RESULT result) {
	return crnet_error_record(record, result, CR_PLATFORM_ERROR_SOURCE_WIN32, WSAGetLastError(), NULL);
}

static void crnet_wait_wake_reset(struct crnet_wait_set_t *set) {
	set->wake    = INVALID_SOCKET;
	set->aborted = 0;
	set->woken   = 0;
}

static bool crnet_wait_wake_create(struct crnet_wait_set_t *set) {
	set->wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (set->wake == INVALID_SOCKET)
		return false;

	memset(&set->wake_address, 0, sizeof(set->wake_address));
	set->wake_address.sin_family      = AF_INET;
	set->wake_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	set->wake_address.sin_port        = 0; // the system picks
	int    length       = sizeof(set->wake_address);
	u_long non_blocking = 1;
	return bind(set->wake, (struct sockaddr *)&set->wake_address, sizeof(set->wake_address)) == 0 &&
	       getsockname(set->wake, (struct sockaddr *)&set->wake_address, &length) == 0 && ioctlsocket(set->wake, FIONBIO, &non_blocking) == 0;
}

static void crnet_wait_wake_destroy(struct crnet_wait_set_t *set) {
	if (set->wake != INVALID_SOCKET)
		closesocket(set->wake);
}

static void crnet_wait_wake_signal(struct crnet_wait_set_t *set) {
	// A full buffer means datagrams are already waiting, which is all this
	// is for.
	char byte = 1;
	sendto(set->wake, &byte, 1, 0, (const struct sockaddr *)&set->wake_address, sizeof(set->wake_address));
}

static void crnet_wait_wake_drain(struct crnet_wait_set_t *set) {
	char bytes[64];
	while (recv(set->wake, bytes, sizeof(bytes), 0) != SOCKET_ERROR) {
	}
}

static crnet_poll_socket crnet_wait_wake_socket(const struct crnet_wait_set_t *set) { return set->wake; }

static void crnet_wait_set_aborted_flag(struct crnet_wait_set_t *set) { InterlockedExchange(&set->aborted, 1); }

static bool crnet_wait_is_aborted(struct crnet_wait_set_t *set) { return InterlockedCompareExchange(&set->aborted, 0, 0) != 0; }

static void crnet_wait_set_woken_flag(struct crnet_wait_set_t *set) { InterlockedExchange(&set->woken, 1); }

/* Whether a wake was asked for since the last time this was. */
static bool crnet_wait_take_woken(struct crnet_wait_set_t *set) { return InterlockedExchange(&set->woken, 0) != 0; }

static uint64_t crnet_wait_now_ms(void) { return (uint64_t)GetTickCount64(); }

/* poll() for `timeout` milliseconds, -1 for no limit. Below 0 on failure;
 * `*out_interrupted` then says it is only worth trying again. */
static int crnet_wait_poll(crnet_poll_entry *entries, uint32_t count, int timeout, bool *out_interrupted) {
	*out_interrupted = false;
	return WSAPoll(entries, (ULONG)count, timeout);
}

#else

static CR_RESULT crnet_wait_failed(crnet_error *record, CR_RESULT result) {
	return crnet_error_record(record, result, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);
}

static void crnet_wait_wake_reset(struct crnet_wait_set_t *set) {
	set->wake_read  = -1;
	set->wake_write = -1;
	atomic_init(&set->aborted, false);
	atomic_init(&set->woken, false);
}

static bool crnet_wait_descriptor_flags(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
	int descriptor_flags = fcntl(fd, F_GETFD, 0);
	return descriptor_flags >= 0 && fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) >= 0;
}

static bool crnet_wait_wake_create(struct crnet_wait_set_t *set) {
	int ends[2];
	if (pipe(ends) != 0)
		return false;
	set->wake_read  = ends[0];
	set->wake_write = ends[1];
	return crnet_wait_descriptor_flags(ends[0]) && crnet_wait_descriptor_flags(ends[1]);
}

static void crnet_wait_wake_destroy(struct crnet_wait_set_t *set) {
	if (set->wake_read >= 0)
		close(set->wake_read);
	if (set->wake_write >= 0)
		close(set->wake_write);
}

static void crnet_wait_wake_signal(struct crnet_wait_set_t *set) {
	char byte = 1;
	if (write(set->wake_write, &byte, 1) < 0) {
		// Full means it is already readable, which is all this is for.
	}
}

static void crnet_wait_wake_drain(struct crnet_wait_set_t *set) {
	char bytes[64];
	while (read(set->wake_read, bytes, sizeof(bytes)) > 0) {
	}
}

static crnet_poll_socket crnet_wait_wake_socket(const struct crnet_wait_set_t *set) { return set->wake_read; }

static void crnet_wait_set_aborted_flag(struct crnet_wait_set_t *set) { atomic_store(&set->aborted, true); }

static bool crnet_wait_is_aborted(struct crnet_wait_set_t *set) { return atomic_load(&set->aborted); }

static void crnet_wait_set_woken_flag(struct crnet_wait_set_t *set) { atomic_store(&set->woken, true); }

/* Whether a wake was asked for since the last time this was. */
static bool crnet_wait_take_woken(struct crnet_wait_set_t *set) { return atomic_exchange(&set->woken, false); }

static uint64_t crnet_wait_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int crnet_wait_poll(crnet_poll_entry *entries, uint32_t count, int timeout, bool *out_interrupted) {
	int ready        = poll(entries, (nfds_t)count, timeout);
	*out_interrupted = ready < 0 && errno == EINTR;
	return ready;
}

#endif

/* Milliseconds left of `timeout_ms` since `start`, as poll() takes them:
 * -1 for no limit. */
static int crnet_wait_remaining(uint64_t start, uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return -1;
	uint64_t elapsed = crnet_wait_now_ms() - start;
	if (elapsed >= timeout_ms)
		return 0;
	uint64_t left = timeout_ms - elapsed;
	return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}

// --------------------------------------------------------------------------
// Create, destroy, abort
// --------------------------------------------------------------------------

CR_RESULT crnet_wait_set_create(
    crnet_instance instance,
    crnet_wait_set *out_set,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!out_set)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_set = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_wait_set_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;

	struct crnet_wait_set_t *set = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_wait_set_t);
	if (!set)
		return CR_RESULT_OUT_OF_MEMORY;
	set->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(set, &instance->quarantine);
	set->allocator      = cr_eq_allocator(&set->head.allocator);
	eq_array_init(&set->members, &set->allocator);
	eq_array_init(&set->entries, &set->allocator);
	set->instance = instance;
	crnet_wait_wake_reset(set);
	if (!crnet_wait_wake_create(set)) {
		// Recorded before the cleanup that could replace the system's error.
		CR_RESULT result = crnet_wait_failed(out_error, CR_RESULT_GENERAL_FAILURE);
		crnet_wait_wake_destroy(set);
		CR_DELETE(&instance->head.allocator, struct crnet_wait_set_t, set);
		return result;
	}

	*out_set = set;
	return CR_SUCCESS;
}

void crnet_wait_set_destroy(crnet_wait_set set) {
	if (!set)
		return;
	if (CR_STALE(set, "crnet_wait_set_destroy", "set"))
		return;
	// A WebSocket that notifies holds this set; each lets go before the
	// wake socket it would write to is closed.
#if CRNET_EXT_WEBSOCKET
	for (size_t i = 0; i < set->members.count; ++i) {
		if (set->members.data[i].kind == CRNET_WAIT_KIND_WEBSOCKET)
			crnet_websocket_wait_detach((crnet_websocket)set->members.data[i].object);
	}
#endif
	crnet_wait_wake_destroy(set);
	eq_array_free(&set->members);
	eq_array_free(&set->entries);
	CR_KILL(set);
	const CR_ALLOCATOR allocator = set->head.allocator;
	cr_string_free(&allocator, set->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(set), &allocator, struct crnet_wait_set_t, set);
}

void crnet_wait_set_abort(crnet_wait_set set) {
	if (!set)
		return;
	if (CR_STALE(set, "crnet_wait_set_abort", "set"))
		return;
	crnet_wait_set_aborted_flag(set);
	crnet_wait_wake_signal(set);
}

void crnet_wait_set_wake(crnet_wait_set set) {
	if (!set)
		return;
	if (CR_STALE(set, "crnet_wait_set_wake", "set"))
		return;
	// The flag first: the wait that the signal ends has to find it.
	crnet_wait_set_woken_flag(set);
	crnet_wait_wake_signal(set);
}

void crnet_wait_set_notify(struct crnet_wait_set_t *set) {
	if (set)
		crnet_wait_wake_signal(set);
}

void crnet_wait_set_get_error(
    crnet_wait_set set,
    crnet_error *out_error) {
	if (!set) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(set, "crnet_wait_set_get_error", "set")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	crnet_error_get(&set->error, out_error);
}

// --------------------------------------------------------------------------
// Members
// --------------------------------------------------------------------------

static struct crnet_wait_member *crnet_wait_find(struct crnet_wait_set_t *set, enum crnet_wait_kind kind, void *object) {
	for (size_t i = 0; i < set->members.count; ++i) {
		if (set->members.data[i].kind == kind && set->members.data[i].object == object)
			return &set->members.data[i];
	}
	return NULL;
}

/* Room for one more member. NULL when out of memory; the set is untouched
 * then. The member returned is not counted until the caller counts it. */
static struct crnet_wait_member *crnet_wait_grow(struct crnet_wait_set_t *set) {
	if (set->members.count == set->members.capacity) {
		// 16, then twice as many, reserved exactly rather than left to
		// eqstd's growth, so the count fits the uint32_t a wait counts its
		// poll entries in.
		if (set->members.capacity > CR_UINT32_MAX / 2)
			return NULL;
		size_t capacity = set->members.capacity ? set->members.capacity * 2 : 16;
		if (eq_array_reserve(&set->members, capacity) != EQ_SUCCESS)
			return NULL;
	}
	struct crnet_wait_member *member = &set->members.data[set->members.count];
	memset(member, 0, sizeof(*member));
	return member;
}

static void crnet_wait_remove(struct crnet_wait_set_t *set, struct crnet_wait_member *member) {
	// Order isn't promised, so the last one fills the gap. The member is
	// one of the set's, so the index is in range and nothing can fail.
	EQ_RESULT removed = eq_array_remove_swap(&set->members, (size_t)(member - set->members.data));
	assert(removed == EQ_SUCCESS);
	(void)removed;
}

#if CRNET_EXT_TCP

CR_RESULT crnet_wait_set_add_tcp_socket(
    crnet_wait_set set,
    crnet_tcp_socket socket,
    uint32_t wait_for,
    void *user) {
	if (!set)
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (CR_STALE(set, "crnet_wait_set_add_tcp_socket", "set"))
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (wait_for == 0 || (wait_for & ~(uint32_t)(CRNET_WAIT_FOR_INCOMING | CRNET_WAIT_FOR_ROOM)) != 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!socket)
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (CR_STALE(socket, "crnet_wait_set_add_tcp_socket", "socket"))
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	intptr_t native = 0;
	if (!crnet_tcp_socket_native(socket, &native))
		return CR_RESULT_NET_INVALID_TCP_SOCKET;
	if (crnet_tcp_socket_instance(socket) != set->instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (crnet_wait_find(set, CRNET_WAIT_KIND_TCP_SOCKET, socket))
		return CR_RESULT_NET_ALREADY_IN_WAIT_SET;

	struct crnet_wait_member *member = crnet_wait_grow(set);
	if (!member)
		return CR_RESULT_OUT_OF_MEMORY;
	member->kind       = CRNET_WAIT_KIND_TCP_SOCKET;
	member->object     = socket;
	member->user       = user;
	member->wait_for   = wait_for;
	member->has_native = true;
	member->native     = native;
	set->members.count++;
	return CR_SUCCESS;
}

CR_RESULT crnet_wait_set_add_tcp_listener(
    crnet_wait_set set,
    crnet_tcp_listener listener,
    void *user) {
	if (!set)
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (CR_STALE(set, "crnet_wait_set_add_tcp_listener", "set"))
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (!listener)
		return CR_RESULT_NET_INVALID_TCP_LISTENER;
	if (CR_STALE(listener, "crnet_wait_set_add_tcp_listener", "listener"))
		return CR_RESULT_NET_INVALID_TCP_LISTENER;
	intptr_t native = 0;
	if (!crnet_tcp_listener_native(listener, &native))
		return CR_RESULT_NET_INVALID_TCP_LISTENER;
	if (crnet_tcp_listener_instance(listener) != set->instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (crnet_wait_find(set, CRNET_WAIT_KIND_TCP_LISTENER, listener))
		return CR_RESULT_NET_ALREADY_IN_WAIT_SET;

	struct crnet_wait_member *member = crnet_wait_grow(set);
	if (!member)
		return CR_RESULT_OUT_OF_MEMORY;
	member->kind       = CRNET_WAIT_KIND_TCP_LISTENER;
	member->object     = listener;
	member->user       = user;
	member->wait_for   = CRNET_WAIT_FOR_INCOMING;
	member->has_native = true;
	member->native     = native;
	set->members.count++;
	return CR_SUCCESS;
}

void crnet_wait_set_remove_tcp_socket(crnet_wait_set set, crnet_tcp_socket socket) {
	if (!set || !socket)
		return;
	if (CR_STALE(set, "crnet_wait_set_remove_tcp_socket", "set"))
		return;
	if (CR_STALE(socket, "crnet_wait_set_remove_tcp_socket", "socket"))
		return;
	struct crnet_wait_member *member = crnet_wait_find(set, CRNET_WAIT_KIND_TCP_SOCKET, socket);
	if (member)
		crnet_wait_remove(set, member);
}

void crnet_wait_set_remove_tcp_listener(crnet_wait_set set, crnet_tcp_listener listener) {
	if (!set || !listener)
		return;
	if (CR_STALE(set, "crnet_wait_set_remove_tcp_listener", "set"))
		return;
	if (CR_STALE(listener, "crnet_wait_set_remove_tcp_listener", "listener"))
		return;
	struct crnet_wait_member *member = crnet_wait_find(set, CRNET_WAIT_KIND_TCP_LISTENER, listener);
	if (member)
		crnet_wait_remove(set, member);
}

#endif // CRNET_EXT_TCP

#if CRNET_EXT_WEBSOCKET

CR_RESULT crnet_wait_set_add_websocket(
    crnet_wait_set set,
    crnet_websocket socket,
    void *user) {
	if (!set)
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (CR_STALE(set, "crnet_wait_set_add_websocket", "set"))
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_wait_set_add_websocket", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (crnet_websocket_instance(socket) != set->instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (crnet_wait_find(set, CRNET_WAIT_KIND_WEBSOCKET, socket))
		return CR_RESULT_NET_ALREADY_IN_WAIT_SET;

	// The room is found first, so that attaching is the last thing that
	// can fail and never has to be undone.
	struct crnet_wait_member *member = crnet_wait_grow(set);
	if (!member)
		return CR_RESULT_OUT_OF_MEMORY;
	CR_RESULT result = crnet_websocket_wait_attach(socket, set, &member->has_native, &member->native, &member->report_once);
	if (CR_FAILED(result))
		return result;
	member->kind     = CRNET_WAIT_KIND_WEBSOCKET;
	member->object   = socket;
	member->user     = user;
	member->wait_for = CRNET_WAIT_FOR_INCOMING;
	set->members.count++;
	return CR_SUCCESS;
}

void crnet_wait_set_remove_websocket(crnet_wait_set set, crnet_websocket socket) {
	if (!set || !socket)
		return;
	if (CR_STALE(set, "crnet_wait_set_remove_websocket", "set"))
		return;
	if (CR_STALE(socket, "crnet_wait_set_remove_websocket", "socket"))
		return;
	struct crnet_wait_member *member = crnet_wait_find(set, CRNET_WAIT_KIND_WEBSOCKET, socket);
	if (!member)
		return;
	crnet_websocket_wait_detach(socket);
	crnet_wait_remove(set, member);
}

#endif // CRNET_EXT_WEBSOCKET

// --------------------------------------------------------------------------
// Wait
// --------------------------------------------------------------------------

/* Asks a member that has no socket to poll whether a receive on it would
 * find something. Only a WebSocket is ever such a member. */
static bool crnet_wait_member_ready(const struct crnet_wait_member *member) {
#if CRNET_EXT_WEBSOCKET
	return crnet_websocket_wait_ready((crnet_websocket)member->object);
#else
	(void)member;
	return false;
#endif
}

CR_RESULT crnet_wait_set_wait(
    crnet_wait_set set,
    uint32_t timeout_ms,
    crnet_wait_event *out_events,
    uint32_t capacity,
    uint32_t *out_count) {
	if (!out_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_count = 0;
	if (!set)
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (CR_STALE(set, "crnet_wait_set_wait", "set"))
		return CR_RESULT_NET_INVALID_WAIT_SET;
	if (!out_events)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (capacity == 0)
		return CR_RESULT_ZERO_SIZE;

	uint64_t start = crnet_wait_now_ms();

	// One entry for the wake socket and one for each member at most.
	if (set->entries.count < set->members.count + 1) {
		if (eq_array_resize(&set->entries, set->members.capacity + 1) != EQ_SUCCESS)
			return CR_RESULT_OUT_OF_MEMORY;
	}

	// A wake that came before this wait, or comes during it. Once taken it
	// is this wait's: the poll only looks from then on, and the wait
	// returns after it whether or not a member was ready.
	bool woken = false;

	for (;;) {
		if (crnet_wait_is_aborted(set))
			return CR_RESULT_NET_ABORTED;

		// Emptied before the members that notify are asked, not after: a
		// notification that lands in between is then either seen by the
		// asking or still in the wake socket to end the poll.
		crnet_wait_wake_drain(set);

		crnet_poll_entry *entries = set->entries.data;
		memset(&entries[0], 0, sizeof(entries[0]));
		entries[0].fd     = crnet_wait_wake_socket(set);
		entries[0].events = CRNET_POLL_INCOMING;
		uint32_t entry_count = 1;

		// Members that are ready without poll() saying so mean the poll
		// only looks; it still runs, so the rest are reported with them.
		bool ready_already = false;
		for (size_t i = 0; i < set->members.count; ++i) {
			struct crnet_wait_member *member = &set->members.data[i];
			if (member->has_native) {
				crnet_poll_entry *entry = &entries[entry_count++];
				memset(entry, 0, sizeof(*entry));
				entry->fd = (crnet_poll_socket)member->native;
				if (member->wait_for & CRNET_WAIT_FOR_INCOMING)
					entry->events |= CRNET_POLL_INCOMING;
				if (member->wait_for & CRNET_WAIT_FOR_ROOM)
					entry->events |= CRNET_POLL_ROOM;
				if (member->report_once)
					ready_already = true;
			} else if (crnet_wait_member_ready(member)) {
				ready_already = true;
			}
		}

		// Taken after the drain for the reason given there: a wake that
		// lands later still has its byte in the wake socket to end the poll.
		if (crnet_wait_take_woken(set))
			woken = true;

		int  remaining   = (ready_already || woken) ? 0 : crnet_wait_remaining(start, timeout_ms);
		bool interrupted = false;
		int  ready       = crnet_wait_poll(entries, entry_count, remaining, &interrupted);
		if (ready < 0) {
			if (interrupted)
				continue;
			return crnet_wait_failed(&set->error, CR_RESULT_GENERAL_FAILURE);
		}
		if (crnet_wait_is_aborted(set))
			return CR_RESULT_NET_ABORTED;

		// The members are walked in the order the entries were made.
		uint32_t count = 0;
		uint32_t entry = 1;
		for (size_t i = 0; i < set->members.count && count < capacity; ++i) {
			struct crnet_wait_member *member = &set->members.data[i];
			uint32_t                    bits   = 0;
			if (member->has_native) {
				short happened = entries[entry++].revents;
				// An ended connection is reported as whatever was asked
				// for; the call the caller then makes says what it is.
				if (happened & CRNET_POLL_ENDED)
					bits = member->wait_for;
				if (happened & CRNET_POLL_INCOMING)
					bits |= CRNET_WAIT_FOR_INCOMING;
				if (happened & CRNET_POLL_ROOM)
					bits |= CRNET_WAIT_FOR_ROOM;
				bits &= member->wait_for;
				if (member->report_once) {
					bits                |= CRNET_WAIT_FOR_INCOMING;
					member->report_once  = false;
				}
			} else if (crnet_wait_member_ready(member)) {
				bits = CRNET_WAIT_FOR_INCOMING;
			}
			if (bits != 0) {
				out_events[count].user  = member->user;
				out_events[count].ready = bits;
				count++;
			}
		}
		if (count > 0) {
			*out_count = count;
			return CR_SUCCESS;
		}

		// Nothing to report. A wake that was asked for is a result of its
		// own; it may also have been what ended the poll just now.
		if (woken || crnet_wait_take_woken(set))
			return CR_SUCCESS;

		// Otherwise the time ran out, or it was only the wake socket - an
		// abort seen at the top, a notification whose member was read
		// before this looked, a stray datagram.
		if (crnet_wait_remaining(start, timeout_ms) == 0)
			return CR_RESULT_NET_TIMED_OUT;
	}
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_TCP || CRNET_EXT_WEBSOCKET
