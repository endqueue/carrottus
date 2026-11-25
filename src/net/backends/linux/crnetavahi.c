#if CARROTTUS_PLATFORM_LINUX

#include "../../crnetshared.h"
#include "crnet.h"

#if CRNET_EXT_DISCOVERY

/*
 * Service discovery on Avahi, over D-Bus to avahi-daemon (crnet.h, "Service
 * discovery"; the front end is src/net/crnetdiscovery.c).
 *
 * Each object opens libavahi-common.so.3 and libavahi-client.so.3 itself at
 * its create (dlopen), finds the calls it needs into a table of its own
 * (dlsym), and closes both at its destroy: nothing is kept between objects
 * or in the instance. Neither library is linked: the build takes Avahi's
 * headers only. Both are opened RTLD_NODELETE, so the loader keeps them,
 * and libdbus below them, mapped once the first object has opened them:
 * libdbus keeps memory in its own globals for the process's life and has
 * no unload of its own, so each unload lost it for good (about 3.7 KB a
 * time, 34 allocations, Fedora 44, libdbus 1.16.4, 2026-11-25), as
 * LeakSanitizer showed and showed nothing once the libraries stayed. A library or a call missing is
 * CR_RESULT_NET_DISCOVERY_UNAVAILABLE, the loader's words recorded under
 * CR_PLATFORM_ERROR_SOURCE_DLERROR.
 *
 * Each object is one AvahiClient (one private D-Bus connection to the
 * daemon) made without AVAHI_CLIENT_NO_FAIL, so a daemon that isn't there
 * is AVAHI_ERR_NO_DAEMON at the create rather than a wait. Avahi's main
 * loop is the object's own AvahiPoll: its watches (descriptors) and
 * timeouts are kept in the object, and run only inside the caller's calls -
 * `pump`, inside `_next`, polls them beside a pipe an abort writes to and
 * runs what is due on the caller's thread; a wait set is handed the same
 * descriptors and the earliest timeout (crnet_wait_poll_add,
 * crnet_wait_poll_timer), so the set names the object and its `_next` runs
 * them. Nothing here runs on a thread of the system's, and nothing is
 * called back to the caller. Avahi's callbacks hand what they were given to
 * the front end's crnet_discovery_report_* calls.
 *
 * - An advert is an AvahiEntryGroup with one service, added
 *   (avahi_entry_group_add_service_strlst, the front end's wire-form TXT
 *   record through avahi_string_list_parse, which keeps its order on the
 *   wire) and committed. A name taken is
 *   CR_RESULT_NET_SERVICE_NAME_CONFLICT whichever way Avahi says it:
 *   AVAHI_ERR_COLLISION from the add or the commit (avahi-daemon's answer
 *   at once for a name this machine already has), or the group's
 *   AVAHI_ENTRY_GROUP_COLLISION state later. Either way the create succeeds
 *   and the advert's next `_next` returns it, as the contract says. Avahi
 *   never renames.
 * - A browser is an AvahiServiceBrowser of the type in "local", every IP
 *   protocol (AVAHI_PROTO_UNSPEC): AVAHI_BROWSER_NEW is FOUND and
 *   AVAHI_BROWSER_REMOVE LOST, once per interface and protocol;
 *   ALL_FOR_NOW and CACHE_EXHAUSTED are no event; FAILURE ends it.
 * - A resolver is an AvahiServiceResolver: its first AVAHI_RESOLVER_FOUND
 *   is SERVICE and the address it carries, a later one only an address of
 *   a family not yet given (at most one per family, crnet.h); its
 *   AVAHI_RESOLVER_FAILURE with AVAHI_ERR_TIMEOUT (avahi-daemon's own time
 *   limit) is CR_RESULT_NET_SERVICE_NOT_ANSWERING.
 *
 * Every create, and every destroy, makes blocking D-Bus round trips to the
 * daemon (avahi_client_new, the objects' _new and _free calls): crnet.h's
 * stated known problem. The client's own failure (the daemon gone or
 * restarted) ends the object with CR_RESULT_NET_DISCOVERY_FAILED.
 *
 * Avahi's codes are recorded under CR_PLATFORM_ERROR_SOURCE_OTHER, the text
 * "avahi: <call>: <avahi_strerror>": crcore.h has no source of their own.
 * Avahi's own memory and libdbus' are the system's: avahi_set_allocator is
 * process-wide and never called. The watches, timeouts and poll array the
 * AvahiPoll hands Avahi come from the object's allocator, made when Avahi
 * asks for them (at a create or a destroy, and once per D-Bus round trip
 * libdbus times) and never while the object only reads.
 */

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/strlst.h>
#include <avahi-common/watch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if __cplusplus
extern "C" {
#endif

/* The libraries' names. A test build of this file points the client's
 * elsewhere, to see a create without it; never a variable (no writable
 * static). */
#ifndef CRNET_AVAHI_CLIENT_LIBRARY
#define CRNET_AVAHI_CLIENT_LIBRARY "libavahi-client.so.3"
#endif
#ifndef CRNET_AVAHI_COMMON_LIBRARY
#define CRNET_AVAHI_COMMON_LIBRARY "libavahi-common.so.3"
#endif

/* The domain every object asks for: link-local multicast DNS only. */
#define CRNET_AVAHI_DOMAIN "local"

/* How many rounds of due timeouts one pump runs at most after its poll:
 * each of Avahi's D-Bus dispatches is one timeout that hands over one
 * message and asks to run again while more are queued, so a pump empties
 * the queue rather than handing over one message per `_next`. The bound
 * only keeps a daemon that floods the connection from holding a call. */
#define CRNET_AVAHI_DISPATCH_ROUNDS 256

typedef AvahiClient *(*crnet_avahi_client_new_fn)(
    const AvahiPoll *,
    AvahiClientFlags,
    AvahiClientCallback,
    void *,
    int *);
typedef void (*crnet_avahi_client_free_fn)(AvahiClient *);
typedef int (*crnet_avahi_client_errno_fn)(AvahiClient *);
typedef AvahiEntryGroup *(*crnet_avahi_group_new_fn)(
    AvahiClient *,
    AvahiEntryGroupCallback,
    void *);
typedef int (*crnet_avahi_group_call_fn)(AvahiEntryGroup *);
typedef int (*crnet_avahi_group_add_fn)(
    AvahiEntryGroup *,
    AvahiIfIndex,
    AvahiProtocol,
    AvahiPublishFlags,
    const char *,
    const char *,
    const char *,
    const char *,
    uint16_t,
    AvahiStringList *);
typedef AvahiServiceBrowser *(*crnet_avahi_browser_new_fn)(
    AvahiClient *,
    AvahiIfIndex,
    AvahiProtocol,
    const char *,
    const char *,
    AvahiLookupFlags,
    AvahiServiceBrowserCallback,
    void *);
typedef int (*crnet_avahi_browser_free_fn)(AvahiServiceBrowser *);
typedef AvahiServiceResolver *(*crnet_avahi_resolver_new_fn)(
    AvahiClient *,
    AvahiIfIndex,
    AvahiProtocol,
    const char *,
    const char *,
    const char *,
    AvahiProtocol,
    AvahiLookupFlags,
    AvahiServiceResolverCallback,
    void *);
typedef int (*crnet_avahi_resolver_free_fn)(AvahiServiceResolver *);
typedef const char *(*crnet_avahi_strerror_fn)(int);
typedef int (*crnet_avahi_list_parse_fn)(
    const void *,
    size_t,
    AvahiStringList **);
typedef void (*crnet_avahi_list_free_fn)(AvahiStringList *);

/* The calls an object takes from its own open of the libraries. */
struct crnet_avahi_calls {
	// libavahi-client
	crnet_avahi_client_new_fn client_new;
	crnet_avahi_client_free_fn client_free;
	crnet_avahi_client_errno_fn client_errno;
	crnet_avahi_group_new_fn group_new;
	crnet_avahi_group_call_fn group_free;
	crnet_avahi_group_call_fn group_commit;
	crnet_avahi_group_add_fn group_add_service;
	crnet_avahi_browser_new_fn browser_new;
	crnet_avahi_browser_free_fn browser_free;
	crnet_avahi_resolver_new_fn resolver_new;
	crnet_avahi_resolver_free_fn resolver_free;
	// libavahi-common
	crnet_avahi_strerror_fn strerror;
	crnet_avahi_list_parse_fn list_parse;
	crnet_avahi_list_free_fn list_free;
};

struct crnet_avahi;

/* A descriptor Avahi asked to have polled (AvahiPoll's watch_new). Kept in
 * a list on the object; one freed while the list is being walked is only
 * marked, and taken out once the walk is over. */
struct AvahiWatch {
	struct crnet_avahi *state;
	struct AvahiWatch *next;
	AvahiWatchCallback callback;
	void *userdata;
	int fd;
	AvahiWatchEvent events;
	AvahiWatchEvent happened;
	// Its place in this round's poll array, or -1.
	int slot;
	bool dead;
};

/* A time Avahi asked to be called at (AvahiPoll's timeout_new): absolute,
 * on gettimeofday's clock, as Avahi gives it. */
struct AvahiTimeout {
	struct crnet_avahi *state;
	struct AvahiTimeout *next;
	AvahiTimeoutCallback callback;
	void *userdata;
	struct timeval at;
	bool enabled;
	bool dead;
};

struct crnet_avahi {
	struct crnet_discovery_object *object;
	void *client_library;
	void *common_library;
	struct crnet_avahi_calls calls;
	// The main loop Avahi runs on: `api.userdata` is this state.
	AvahiPoll api;
	struct AvahiWatch *watches;
	struct AvahiTimeout *timeouts;
	uint32_t live_watches;
	// The poll array, the abort pipe's end first: grown as watches are
	// made, never while the object only reads.
	struct pollfd *polled;
	uint32_t polled_capacity;
	// Inside a walk of the watches or timeouts: a free only marks.
	uint32_t walking;
	// Set at the destroy: Avahi's callbacks report nothing more.
	bool stopping;
	// The object's allocator refused something Avahi asked for (a watch, a
	// timeout, the poll array). Avahi and libdbus report such a refusal as
	// whatever their own failure becomes (a D-Bus error, a failed connect),
	// so a create that fails after one is CR_RESULT_OUT_OF_MEMORY.
	bool refused_memory;
	AvahiClient *client;
	AvahiEntryGroup *group;
	AvahiServiceBrowser *browser;
	AvahiServiceResolver *resolver;
	// The resolver's SERVICE handed over, and which families' addresses.
	bool answered;
	bool gave_ipv4;
	bool gave_ipv6;
	// What an abort writes to; -1 until made.
	int wake[2];
};

static struct crnet_avahi *crnet_avahi_of(struct crnet_discovery_object *object) { return (struct crnet_avahi *)object->backend; }

// --------------------------------------------------------------------------
// Failures
// --------------------------------------------------------------------------

/* What an Avahi code is: refused, no daemon, a name taken, a resolve the
 * instance never answered, or anything else. */
static CR_RESULT crnet_avahi_result(
    int code,
    bool at_create) {
	switch (code) {
	case AVAHI_ERR_NO_DAEMON:
		return at_create ? CR_RESULT_NET_DISCOVERY_UNAVAILABLE : CR_RESULT_NET_DISCOVERY_FAILED;
	case AVAHI_ERR_ACCESS_DENIED:
	case AVAHI_ERR_NOT_PERMITTED:
		return CR_RESULT_NET_DISCOVERY_DENIED;
	case AVAHI_ERR_COLLISION:
		return CR_RESULT_NET_SERVICE_NAME_CONFLICT;
	case AVAHI_ERR_TIMEOUT:
		return CR_RESULT_NET_SERVICE_NOT_ANSWERING;
	case AVAHI_ERR_NO_MEMORY:
		return CR_RESULT_OUT_OF_MEMORY;
	default:
		return CR_RESULT_NET_DISCOVERY_FAILED;
	}
}

static void crnet_avahi_text(
    const struct crnet_avahi *state,
    char *text,
    size_t capacity,
    const char *call,
    int code) {
	const char *words = state->calls.strerror ? state->calls.strerror(code) : NULL;
	snprintf(text, capacity, "avahi: %s: %s", call, words ? words : "unknown error");
}

/* A failure Avahi reported in a callback, as the object's end. */
static void crnet_avahi_end(
    struct crnet_avahi *state,
    CR_RESULT result,
    const char *call,
    int code) {
	char text[CRNET_ERROR_TEXT_SIZE];
	crnet_avahi_text(state, text, sizeof(text), call, code);
	crnet_discovery_report_end(state->object, result, CR_PLATFORM_ERROR_SOURCE_OTHER, code, text);
}

/* A create's failure, recorded in `out_error` and returned: out of memory
 * when the object's allocator refused Avahi something on the way, whatever
 * Avahi made of it. */
static CR_RESULT crnet_avahi_start_failed(
    struct crnet_avahi *state,
    crnet_error *out_error,
    const char *call,
    int code) {
	if (state->refused_memory)
		return CR_RESULT_OUT_OF_MEMORY;
	char text[CRNET_ERROR_TEXT_SIZE];
	crnet_avahi_text(state, text, sizeof(text), call, code);
	return crnet_error_record(out_error, crnet_avahi_result(code, true), CR_PLATFORM_ERROR_SOURCE_OTHER, code, text);
}

/* The client's last error, for a call that returned NULL or a failure
 * without one of its own. */
static int crnet_avahi_errno(struct crnet_avahi *state) { return state->client ? state->calls.client_errno(state->client) : AVAHI_ERR_FAILURE; }

// --------------------------------------------------------------------------
// The libraries
// --------------------------------------------------------------------------

/* The loader's words for a failed dlopen or dlsym, as a create's failure. */
static CR_RESULT crnet_avahi_loader_failed(crnet_error *out_error) {
	const char *words = dlerror();
	return crnet_error_record(out_error, CR_RESULT_NET_DISCOVERY_UNAVAILABLE, CR_PLATFORM_ERROR_SOURCE_DLERROR, 0, words);
}

/* One call found in `library` into `*out`, or false with dlerror set. A
 * function pointer read from dlsym's object pointer by its bytes: POSIX
 * makes them the same size. */
static bool crnet_avahi_find(
    void *library,
    const char *name,
    void *out,
    size_t size) {
	(void)dlerror(); // an earlier call's message is not this one's
	void *found = dlsym(library, name);
	if (!found)
		return false;
	memcpy(out, &found, size);
	return true;
}

/* Each object's own open of the two libraries and its table of calls. */
static CR_RESULT crnet_avahi_open(
    struct crnet_avahi *state,
    crnet_error *out_error) {
	(void)dlerror();
	state->common_library = dlopen(CRNET_AVAHI_COMMON_LIBRARY, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
	if (!state->common_library)
		return crnet_avahi_loader_failed(out_error);
	state->client_library = dlopen(CRNET_AVAHI_CLIENT_LIBRARY, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
	if (!state->client_library)
		return crnet_avahi_loader_failed(out_error);
	struct crnet_avahi_calls *calls = &state->calls;
#define CRNET_AVAHI_FIND(library, field, name)                                                                                                                 \
	if (!crnet_avahi_find(state->library, name, &calls->field, sizeof(calls->field)))                                                                          \
	return crnet_avahi_loader_failed(out_error)
	CRNET_AVAHI_FIND(common_library, strerror, "avahi_strerror");
	CRNET_AVAHI_FIND(common_library, list_parse, "avahi_string_list_parse");
	CRNET_AVAHI_FIND(common_library, list_free, "avahi_string_list_free");
	CRNET_AVAHI_FIND(client_library, client_new, "avahi_client_new");
	CRNET_AVAHI_FIND(client_library, client_free, "avahi_client_free");
	CRNET_AVAHI_FIND(client_library, client_errno, "avahi_client_errno");
	CRNET_AVAHI_FIND(client_library, group_new, "avahi_entry_group_new");
	CRNET_AVAHI_FIND(client_library, group_free, "avahi_entry_group_free");
	CRNET_AVAHI_FIND(client_library, group_commit, "avahi_entry_group_commit");
	CRNET_AVAHI_FIND(client_library, group_add_service, "avahi_entry_group_add_service_strlst");
	CRNET_AVAHI_FIND(client_library, browser_new, "avahi_service_browser_new");
	CRNET_AVAHI_FIND(client_library, browser_free, "avahi_service_browser_free");
	CRNET_AVAHI_FIND(client_library, resolver_new, "avahi_service_resolver_new");
	CRNET_AVAHI_FIND(client_library, resolver_free, "avahi_service_resolver_free");
#undef CRNET_AVAHI_FIND
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// The main loop Avahi runs on (AvahiPoll)
// --------------------------------------------------------------------------

static const CR_ALLOCATOR *crnet_avahi_allocator(struct crnet_avahi *state) { return &state->object->head.allocator; }

/* Room in the poll array for every live watch and the abort pipe. */
static bool crnet_avahi_polled_reserve(
    struct crnet_avahi *state,
    uint32_t watches) {
	const uint32_t needed = watches + 1;
	if (needed <= state->polled_capacity)
		return true;
	uint32_t capacity = state->polled_capacity ? state->polled_capacity * 2 : 4;
	while (capacity < needed)
		capacity *= 2;
	struct pollfd *grown = CR_NEW_ARRAY(crnet_avahi_allocator(state), struct pollfd, capacity);
	if (!grown)
		return false;
	if (state->polled)
		CR_DELETE_ARRAY(crnet_avahi_allocator(state), struct pollfd, state->polled, state->polled_capacity);
	state->polled          = grown;
	state->polled_capacity = capacity;
	return true;
}

static AvahiWatch *crnet_avahi_watch_new(
    const AvahiPoll *api,
    int fd,
    AvahiWatchEvent events,
    AvahiWatchCallback callback,
    void *userdata) {
	struct crnet_avahi *state = (struct crnet_avahi *)api->userdata;
	struct AvahiWatch *watch =
	    crnet_avahi_polled_reserve(state, state->live_watches + 1) ? CR_NEW_ZEROED(crnet_avahi_allocator(state), struct AvahiWatch) : NULL;
	if (!watch) {
		state->refused_memory = true;
		return NULL;
	}
	watch->state    = state;
	watch->callback = callback;
	watch->userdata = userdata;
	watch->fd       = fd;
	watch->events   = events;
	watch->slot     = -1;
	// At the list's end, so a walk under way reaches it only after the
	// watches it was walking (its slot is -1 for this round's).
	struct AvahiWatch **end = &state->watches;
	while (*end)
		end = &(*end)->next;
	*end = watch;
	state->live_watches++;
	return watch;
}

static void crnet_avahi_watch_update(
    AvahiWatch *watch,
    AvahiWatchEvent events) {
	watch->events = events;
}

static AvahiWatchEvent crnet_avahi_watch_get_events(AvahiWatch *watch) { return watch->happened; }

/* Takes the marked watches and timeouts out of their lists, outside a walk. */
static void crnet_avahi_sweep(struct crnet_avahi *state) {
	if (state->walking)
		return;
	for (struct AvahiWatch **at = &state->watches; *at;) {
		struct AvahiWatch *watch = *at;
		if (!watch->dead) {
			at = &watch->next;
			continue;
		}
		*at = watch->next;
		CR_DELETE(crnet_avahi_allocator(state), struct AvahiWatch, watch);
	}
	for (struct AvahiTimeout **at = &state->timeouts; *at;) {
		struct AvahiTimeout *timeout = *at;
		if (!timeout->dead) {
			at = &timeout->next;
			continue;
		}
		*at = timeout->next;
		CR_DELETE(crnet_avahi_allocator(state), struct AvahiTimeout, timeout);
	}
}

/* Avahi's own cleanup hands a NULL back when a _new it made failed (its
 * D-Bus glue after a timeout_new refused for memory). */
static void crnet_avahi_watch_free(AvahiWatch *watch) {
	if (!watch || watch->dead)
		return;
	struct crnet_avahi *state = watch->state;
	watch->dead               = true;
	state->live_watches--;
	crnet_avahi_sweep(state);
}

static AvahiTimeout *crnet_avahi_timeout_new(
    const AvahiPoll *api,
    const struct timeval *at,
    AvahiTimeoutCallback callback,
    void *userdata) {
	struct crnet_avahi *state    = (struct crnet_avahi *)api->userdata;
	struct AvahiTimeout *timeout = CR_NEW_ZEROED(crnet_avahi_allocator(state), struct AvahiTimeout);
	if (!timeout) {
		state->refused_memory = true;
		return NULL;
	}
	timeout->state    = state;
	timeout->callback = callback;
	timeout->userdata = userdata;
	timeout->enabled  = at != NULL;
	if (at)
		timeout->at = *at;
	struct AvahiTimeout **end = &state->timeouts;
	while (*end)
		end = &(*end)->next;
	*end = timeout;
	return timeout;
}

static void crnet_avahi_timeout_update(
    AvahiTimeout *timeout,
    const struct timeval *at) {
	timeout->enabled = at != NULL;
	if (at)
		timeout->at = *at;
}

static void crnet_avahi_timeout_free(AvahiTimeout *timeout) {
	if (!timeout || timeout->dead)
		return;
	timeout->dead    = true;
	timeout->enabled = false;
	crnet_avahi_sweep(timeout->state);
}

/* Milliseconds from now until `at` on gettimeofday's clock (Avahi's), 0
 * once it has passed, rounded up so a wait never ends before it. */
static uint32_t crnet_avahi_until_ms(const struct timeval *at) {
	struct timeval now;
	gettimeofday(&now, NULL);
	const int64_t us = ((int64_t)at->tv_sec - (int64_t)now.tv_sec) * 1000000 + ((int64_t)at->tv_usec - (int64_t)now.tv_usec);
	if (us <= 0)
		return 0;
	const int64_t ms = (us + 999) / 1000;
	return ms >= (int64_t)CRNET_WAIT_FOREVER ? CRNET_WAIT_FOREVER - 1 : (uint32_t)ms;
}

/* The earliest enabled timeout's wait, or CRNET_WAIT_FOREVER for none. */
static uint32_t crnet_avahi_next_timeout_ms(const struct crnet_avahi *state) {
	uint32_t earliest = CRNET_WAIT_FOREVER;
	for (const struct AvahiTimeout *timeout = state->timeouts; timeout; timeout = timeout->next) {
		if (timeout->dead || !timeout->enabled)
			continue;
		const uint32_t until = crnet_avahi_until_ms(&timeout->at);
		earliest             = until < earliest ? until : earliest;
	}
	return earliest;
}

/* Runs every timeout that is due, each disabled before its callback (as
 * AvahiPoll says), and again while a callback set another due. True when
 * one ran. */
static bool crnet_avahi_run_timeouts(struct crnet_avahi *state) {
	bool ran = false;
	for (int round = 0; round < CRNET_AVAHI_DISPATCH_ROUNDS; ++round) {
		bool this_round = false;
		state->walking++;
		for (struct AvahiTimeout *timeout = state->timeouts; timeout; timeout = timeout->next) {
			if (timeout->dead || !timeout->enabled || crnet_avahi_until_ms(&timeout->at) != 0)
				continue;
			timeout->enabled = false;
			timeout->callback(timeout, timeout->userdata);
			this_round = true;
		}
		state->walking--;
		crnet_avahi_sweep(state);
		if (!this_round)
			break;
		ran = true;
	}
	return ran;
}

static void crnet_avahi_poll_init(struct crnet_avahi *state) {
	state->api.userdata         = state;
	state->api.watch_new        = crnet_avahi_watch_new;
	state->api.watch_update     = crnet_avahi_watch_update;
	state->api.watch_get_events = crnet_avahi_watch_get_events;
	state->api.watch_free       = crnet_avahi_watch_free;
	state->api.timeout_new      = crnet_avahi_timeout_new;
	state->api.timeout_update   = crnet_avahi_timeout_update;
	state->api.timeout_free     = crnet_avahi_timeout_free;
}

// --------------------------------------------------------------------------
// Avahi's callbacks, each run on the caller's thread: inside a create, or
// inside `pump`
// --------------------------------------------------------------------------

static void crnet_avahi_client_changed(
    AvahiClient *client,
    AvahiClientState state_now,
    void *userdata) {
	struct crnet_avahi *state = (struct crnet_avahi *)userdata;
	if (state->stopping || state_now != AVAHI_CLIENT_FAILURE)
		return;
	// The daemon has gone or restarted: the connection is over, and so is
	// every operation on it.
	crnet_avahi_end(state, CR_RESULT_NET_DISCOVERY_FAILED, "AvahiClient", state->calls.client_errno(client));
}

static void crnet_avahi_group_changed(
    AvahiEntryGroup *group,
    AvahiEntryGroupState state_now,
    void *userdata) {
	(void)group;
	struct crnet_avahi *state = (struct crnet_avahi *)userdata;
	if (state->stopping)
		return;
	switch (state_now) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		crnet_discovery_report_registered(state->object);
		break;
	case AVAHI_ENTRY_GROUP_COLLISION:
		crnet_avahi_end(state, CR_RESULT_NET_SERVICE_NAME_CONFLICT, "AVAHI_ENTRY_GROUP_COLLISION", AVAHI_ERR_COLLISION);
		break;
	case AVAHI_ENTRY_GROUP_FAILURE: {
		const int code = crnet_avahi_errno(state);
		crnet_avahi_end(state, crnet_avahi_result(code, false), "AVAHI_ENTRY_GROUP_FAILURE", code);
		break;
	}
	default:
		break; // uncommitted, registering: nothing yet
	}
}

static void crnet_avahi_browsed(
    AvahiServiceBrowser *browser,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AvahiLookupResultFlags flags,
    void *userdata) {
	(void)browser;
	(void)protocol;
	(void)domain;
	(void)flags;
	struct crnet_avahi *state = (struct crnet_avahi *)userdata;
	if (state->stopping)
		return;
	switch (event) {
	case AVAHI_BROWSER_NEW:
	case AVAHI_BROWSER_REMOVE:
		crnet_discovery_report_found(state->object, event == AVAHI_BROWSER_NEW, interface > 0 ? (uint32_t)interface : 0, name, type);
		break;
	case AVAHI_BROWSER_FAILURE: {
		const int code = crnet_avahi_errno(state);
		crnet_avahi_end(state, CR_RESULT_NET_DISCOVERY_FAILED, "AVAHI_BROWSER_FAILURE", code);
		break;
	}
	default:
		break; // ALL_FOR_NOW, CACHE_EXHAUSTED: no event
	}
}

/* The record a resolve handed over, in DNS wire form (a length byte and
 * that many bytes, each entry), into `out` up to `capacity`; returns the
 * record's whole size. Written here, not by avahi_string_list_serialize: a
 * resolver's list comes in the record's order (avahi-client adds each
 * entry at the list's end), while that call writes a list from its tail,
 * the order of lists Avahi builds by prepending (avahi_string_list_parse's,
 * an advert's), so it would hand the entries back reversed - seen against
 * the record on the wire, Fedora 44, Avahi 0.9-rc2, 2026-11-25. It also
 * cuts an entry short to fit where this keeps the first `capacity` bytes.
 * A record with no entry is the one empty string the wire carries for it
 * (RFC 6763, 6.1), as Avahi's own call writes it. */
static size_t crnet_avahi_txt_wire(
    const AvahiStringList *list,
    uint8_t *out,
    size_t capacity) {
	size_t used = 0;
	for (const AvahiStringList *entry = list; entry; entry = entry->next) {
		const size_t length = entry->size > 255 ? 255 : entry->size;
		if (used < capacity)
			out[used] = (uint8_t)length;
		if (used + 1 < capacity) {
			const size_t room = capacity - (used + 1);
			memcpy(out + used + 1, entry->text, length < room ? length : room);
		}
		used += 1 + length;
	}
	if (used == 0) {
		if (capacity > 0)
			out[0] = 0;
		used = 1;
	}
	return used;
}

/* An address a resolve gave, as an event, if of a family asked for and
 * not yet given. */
static void crnet_avahi_report_address(
    struct crnet_avahi *state,
    const AvahiAddress *given,
    AvahiIfIndex interface,
    uint16_t port) {
	struct crnet_discovery_object *object = state->object;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
	const void *sockaddr = NULL;
	size_t length        = 0;
	if (given->proto == AVAHI_PROTO_INET && !state->gave_ipv4) {
		memset(&in, 0, sizeof(in));
		in.sin_family      = AF_INET;
		in.sin_addr.s_addr = given->data.ipv4.address; // network order, as Avahi keeps it
		sockaddr           = &in;
		length             = sizeof(in);
	} else if (given->proto == AVAHI_PROTO_INET6 && !state->gave_ipv6) {
		memset(&in6, 0, sizeof(in6));
		in6.sin6_family = AF_INET6;
		memcpy(&in6.sin6_addr, given->data.ipv6.address, sizeof(in6.sin6_addr));
		sockaddr = &in6;
		length   = sizeof(in6);
	}
	if (!sockaddr)
		return;
	crnet_address address;
	if (!crnet_address_from_sockaddr(sockaddr, length, interface > 0 ? (uint32_t)interface : 0, &address, NULL))
		return;
	if ((object->family == CRNET_ADDRESS_FAMILY_IPV4 && address.family != CRNET_ADDRESS_FAMILY_IPV4) ||
	    (object->family == CRNET_ADDRESS_FAMILY_IPV6 && address.family != CRNET_ADDRESS_FAMILY_IPV6))
		return;
	address.port = port;
	if (address.family == CRNET_ADDRESS_FAMILY_IPV4)
		state->gave_ipv4 = true;
	else
		state->gave_ipv6 = true;
	crnet_discovery_report_address(object, true, &address);
}

static void crnet_avahi_resolved(
    AvahiServiceResolver *resolver,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiResolverEvent event,
    const char *name,
    const char *type,
    const char *domain,
    const char *host,
    const AvahiAddress *address,
    uint16_t port,
    AvahiStringList *txt,
    AvahiLookupResultFlags flags,
    void *userdata) {
	(void)resolver;
	(void)protocol;
	(void)domain;
	(void)flags;
	struct crnet_avahi *state             = (struct crnet_avahi *)userdata;
	struct crnet_discovery_object *object = state->object;
	if (state->stopping)
		return;
	if (event == AVAHI_RESOLVER_FAILURE) {
		const int code = crnet_avahi_errno(state);
		crnet_avahi_end(state, crnet_avahi_result(code, false), "AVAHI_RESOLVER_FAILURE", code);
		return;
	}
	if (event != AVAHI_RESOLVER_FOUND)
		return;
	// Only the instance asked for (Avahi hands its name unescaped).
	if (!name || strcmp(name, object->name) != 0 || (type && !crnet_service_type_matches(type, object->type)))
		return;
	if (!state->answered) {
		uint8_t record[CRNET_SERVICE_TXT_CAPACITY];
		const size_t size = crnet_avahi_txt_wire(txt, record, sizeof(record));
		if (!crnet_discovery_report_service(object, NULL, host, port, record, size))
			return;
		state->answered = true;
	}
	if (address)
		crnet_avahi_report_address(state, address, interface, port);
}

// --------------------------------------------------------------------------
// The verbs
// --------------------------------------------------------------------------

/* The backend's state, its abort pipe, the libraries and the client,
 * before the operation is begun. Whatever was made is in `object->backend`
 * for stop, on a failure too. */
static CR_RESULT crnet_avahi_make(
    struct crnet_discovery_object *object,
    crnet_error *out_error) {
	struct crnet_avahi *state = CR_NEW_ZEROED(&object->head.allocator, struct crnet_avahi);
	if (!state)
		return CR_RESULT_OUT_OF_MEMORY;
	state->object   = object;
	state->wake[0]  = -1;
	state->wake[1]  = -1;
	object->backend = state;
	crnet_avahi_poll_init(state);
	if (!crnet_avahi_polled_reserve(state, 0))
		return CR_RESULT_OUT_OF_MEMORY;
	int ends[2];
	if (pipe2(ends, O_NONBLOCK | O_CLOEXEC) != 0)
		return crnet_error_record(out_error, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);
	state->wake[0]         = ends[0];
	state->wake[1]         = ends[1];
	const CR_RESULT opened = crnet_avahi_open(state, out_error);
	if (CR_FAILED(opened))
		return opened;
	int code      = 0;
	state->client = state->calls.client_new(&state->api, (AvahiClientFlags)0, crnet_avahi_client_changed, state, &code);
	if (!state->client)
		return crnet_avahi_start_failed(state, out_error, "avahi_client_new", code);
	return CR_SUCCESS;
}

/* The interface Avahi is asked for: one, or every one. */
static AvahiIfIndex crnet_avahi_interface(const struct crnet_discovery_object *object) {
	return object->interface_index != 0 ? (AvahiIfIndex)object->interface_index : AVAHI_IF_UNSPEC;
}

CR_RESULT crnet_discovery_backend_advert_start(
    struct crnet_discovery_object *object,
    const uint8_t *txt,
    size_t txt_size,
    crnet_error *out_error) {
	const CR_RESULT made = crnet_avahi_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_avahi *state = crnet_avahi_of(object);
	state->group              = state->calls.group_new(state->client, crnet_avahi_group_changed, state);
	if (!state->group)
		return crnet_avahi_start_failed(state, out_error, "avahi_entry_group_new", crnet_avahi_errno(state));
	// The front end's record, checked there, as Avahi's list.
	AvahiStringList *list = NULL;
	if (txt_size && state->calls.list_parse(txt, txt_size, &list) != 0)
		return crnet_avahi_start_failed(state, out_error, "avahi_string_list_parse", AVAHI_ERR_INVALID_RECORD);
	int code = state->calls.group_add_service(
	    state->group, crnet_avahi_interface(object), AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0, object->name, object->type, CRNET_AVAHI_DOMAIN, NULL,
	    object->port, list);
	if (list)
		state->calls.list_free(list);
	const char *call = "avahi_entry_group_add_service_strlst";
	if (code == 0) {
		code = state->calls.group_commit(state->group);
		call = "avahi_entry_group_commit";
	}
	// A name taken is the advert's end, handed over by its next `_next`;
	// anything else fails the create.
	if (code == AVAHI_ERR_COLLISION) {
		crnet_avahi_end(state, CR_RESULT_NET_SERVICE_NAME_CONFLICT, call, code);
		return CR_SUCCESS;
	}
	if (code < 0)
		return crnet_avahi_start_failed(state, out_error, call, code);
	return CR_SUCCESS;
}

CR_RESULT crnet_discovery_backend_browser_start(
    struct crnet_discovery_object *object,
    crnet_error *out_error) {
	const CR_RESULT made = crnet_avahi_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_avahi *state = crnet_avahi_of(object);
	state->browser            = state->calls.browser_new(
	    state->client, crnet_avahi_interface(object), AVAHI_PROTO_UNSPEC, object->type, CRNET_AVAHI_DOMAIN, (AvahiLookupFlags)0, crnet_avahi_browsed, state);
	if (!state->browser)
		return crnet_avahi_start_failed(state, out_error, "avahi_service_browser_new", crnet_avahi_errno(state));
	return CR_SUCCESS;
}

/* The address family a resolve asks Avahi for. */
static AvahiProtocol crnet_avahi_protocol(CRNET_ADDRESS_FAMILY family) {
	switch (family) {
	case CRNET_ADDRESS_FAMILY_IPV4:
		return AVAHI_PROTO_INET;
	case CRNET_ADDRESS_FAMILY_IPV6:
		return AVAHI_PROTO_INET6;
	default:
		return AVAHI_PROTO_UNSPEC;
	}
}

CR_RESULT crnet_discovery_backend_resolver_start(
    struct crnet_discovery_object *object,
    crnet_error *out_error) {
	const CR_RESULT made = crnet_avahi_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_avahi *state = crnet_avahi_of(object);
	state->resolver           = state->calls.resolver_new(
	    state->client, crnet_avahi_interface(object), AVAHI_PROTO_UNSPEC, object->name, object->type, CRNET_AVAHI_DOMAIN, crnet_avahi_protocol(object->family),
	    (AvahiLookupFlags)0, crnet_avahi_resolved, state);
	if (!state->resolver)
		return crnet_avahi_start_failed(state, out_error, "avahi_service_resolver_new", crnet_avahi_errno(state));
	return CR_SUCCESS;
}

void crnet_discovery_backend_stop(struct crnet_discovery_object *object) {
	struct crnet_avahi *state = crnet_avahi_of(object);
	if (!state)
		return;
	state->stopping = true;
	// The operation first, then its client, which frees whatever of its
	// own is left (its watches and timeouts among them).
	if (state->resolver)
		state->calls.resolver_free(state->resolver);
	if (state->browser)
		state->calls.browser_free(state->browser);
	if (state->group)
		state->calls.group_free(state->group);
	if (state->client)
		state->calls.client_free(state->client);
	// Whatever Avahi didn't free (none, once the client is gone).
	for (struct AvahiWatch *watch = state->watches; watch;) {
		struct AvahiWatch *next = watch->next;
		CR_DELETE(&object->head.allocator, struct AvahiWatch, watch);
		watch = next;
	}
	for (struct AvahiTimeout *timeout = state->timeouts; timeout;) {
		struct AvahiTimeout *next = timeout->next;
		CR_DELETE(&object->head.allocator, struct AvahiTimeout, timeout);
		timeout = next;
	}
	if (state->polled)
		CR_DELETE_ARRAY(&object->head.allocator, struct pollfd, state->polled, state->polled_capacity);
	// The client's library before the common one it was loaded over.
	if (state->client_library)
		dlclose(state->client_library);
	if (state->common_library)
		dlclose(state->common_library);
	if (state->wake[0] >= 0)
		close(state->wake[0]);
	if (state->wake[1] >= 0)
		close(state->wake[1]);
	object->backend = NULL;
	CR_DELETE(&object->head.allocator, struct crnet_avahi, state);
}

void crnet_discovery_backend_wake(struct crnet_discovery_object *object) {
	struct crnet_avahi *state = crnet_avahi_of(object);
	char byte                 = 1;
	if (write(state->wake[1], &byte, 1) < 0) {
		// Full means it is already readable, which is all this is for.
	}
}

static uint64_t crnet_avahi_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Runs the watches the poll found ready. True when one ran. */
static bool crnet_avahi_run_watches(struct crnet_avahi *state) {
	bool ran = false;
	state->walking++;
	for (struct AvahiWatch *watch = state->watches; watch; watch = watch->next) {
		if (watch->dead || watch->slot < 0)
			continue;
		const short got = state->polled[watch->slot].revents;
		watch->slot     = -1;
		if (got == 0)
			continue;
		// AvahiWatchEvent's bits are poll()'s (avahi-common/watch.h).
		watch->happened = (AvahiWatchEvent)got;
		watch->callback(watch, watch->fd, watch->happened, watch->userdata);
		watch->happened = (AvahiWatchEvent)0;
		ran             = true;
	}
	state->walking--;
	crnet_avahi_sweep(state);
	return ran;
}

CR_RESULT crnet_discovery_backend_pump(
    struct crnet_discovery_object *object,
    uint32_t timeout_ms) {
	struct crnet_avahi *state = crnet_avahi_of(object);
	const uint64_t start      = crnet_avahi_now_ms();
	for (;;) {
		if (crnet_atomic_load32(&object->aborted))
			return CR_RESULT_NET_ABORTED;
		// What Avahi has already read and asked to have run (a D-Bus
		// dispatch is a timeout due now) goes first.
		if (crnet_avahi_run_timeouts(state))
			return CR_SUCCESS;
		uint32_t remaining = CRNET_WAIT_FOREVER;
		if (timeout_ms != CRNET_WAIT_FOREVER) {
			const uint64_t elapsed = crnet_avahi_now_ms() - start;
			remaining              = elapsed >= timeout_ms ? 0 : (uint32_t)(timeout_ms - elapsed);
		}
		const uint32_t avahi_wait = crnet_avahi_next_timeout_ms(state);
		const uint32_t wait       = avahi_wait < remaining ? avahi_wait : remaining;

		// The abort pipe, then every live watch, in the array grown for them.
		nfds_t count                 = 0;
		state->polled[count].fd      = state->wake[0];
		state->polled[count].events  = POLLIN;
		state->polled[count].revents = 0;
		count++;
		for (struct AvahiWatch *watch = state->watches; watch; watch = watch->next) {
			watch->slot = -1;
			if (watch->dead || count == state->polled_capacity)
				continue;
			watch->slot                  = (int)count;
			state->polled[count].fd      = watch->fd;
			state->polled[count].events  = (short)watch->events;
			state->polled[count].revents = 0;
			count++;
		}
		const int ready = poll(state->polled, count, wait == CRNET_WAIT_FOREVER ? -1 : (wait > (uint32_t)INT32_MAX ? INT32_MAX : (int)wait));
		if (ready < 0 && errno == EINTR)
			continue;
		if (crnet_atomic_load32(&object->aborted))
			return CR_RESULT_NET_ABORTED;
		if (ready < 0) {
			crnet_discovery_report_end(object, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);
			return CR_SUCCESS;
		}
		bool ran = ready > 0 && crnet_avahi_run_watches(state);
		ran      = crnet_avahi_run_timeouts(state) || ran;
		if (ran)
			return CR_SUCCESS;
		if (remaining == 0 || (ready == 0 && wait == remaining))
			return CR_RESULT_NET_TIMED_OUT;
		// An Avahi timeout's time came a little early by gettimeofday's
		// clock, or the poll ended for nothing: round again.
	}
}

void crnet_discovery_backend_wait_prepare(
    struct crnet_discovery_object *object,
    struct crnet_wait_poll *poll) {
	struct crnet_avahi *state = crnet_avahi_of(object);
	if (!state)
		return;
	// Each descriptor Avahi polls, for what it asks: news on it names the
	// object, whose `_next` runs Avahi on it (libcurl's are handed over the
	// same way, crnetcurl.c).
	for (const struct AvahiWatch *watch = state->watches; watch; watch = watch->next) {
		if (watch->dead || watch->events == 0)
			continue;
		crnet_wait_poll_add(
		    poll, (intptr_t)watch->fd, (watch->events & (AVAHI_WATCH_IN | AVAHI_WATCH_ERR | AVAHI_WATCH_HUP)) != 0, (watch->events & AVAHI_WATCH_OUT) != 0,
		    true);
	}
	// And its earliest timeout, which only a call runs: the object is named
	// when it comes.
	const uint32_t next = crnet_avahi_next_timeout_ms(state);
	if (next != CRNET_WAIT_FOREVER)
		crnet_wait_poll_timer(poll, next);
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_DISCOVERY

#endif // CARROTTUS_PLATFORM_LINUX
