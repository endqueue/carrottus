
#ifndef CRNETSHARED_H
#define CRNETSHARED_H

/*
 * What Carrottus Net's source files share with one another. Not a public header.
 *
 * Four things live here:
 *
 * - What a crnet_instance is, which is the same everywhere: nothing but
 *   the fact that it was made, and the caller's allocator. Each platform
 *   makes and destroys it where it starts and stops its networking.
 * - Small helpers that are the same on every platform: a crnet_error
 *   record's handling, the checks on a crnet_tls_desc and a
 *   crnet_proxy_desc that don't depend on the stack, the walk through a
 *   certificate that finds its public key. Defined in src/crnet.c.
 * - The HTTP transfer. Its order of use, its argument checks, its
 *   accounting of a sized body and the storage of a response's status and
 *   headers are the same whichever stack carries it, so they are written
 *   once, in src/net/crnethttp.c, which defines every
 *   crnet_http_transfer_* in crnet.h. What differs - getting the bytes
 *   to and from the network - is the stack's, behind the
 *   crnet_http_backend_* functions each HTTP backend defines.
 * - What a wait set needs from the objects in it.
 */

#include "crnet.h"
#include "../crallocator.h"

#include <stddef.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* An instance holds one start of the platform's networking and, of Carrottus Net's
 * own, only its head: its debug name and the caller's allocator, which every
 * object made with it copies into its own head. What it started is the
 * platform's, and the platform keeps the count. Made and destroyed by the file that makes the platform's
 * calls: crnetwindows.c (Winsock), crnetcurl.c (libcurl) and
 * crneturlsession.m (nothing to start).
 *
 * The objects a wait set can hold keep the instance they were made with -
 * an accepted socket keeps its listener's - which is how a set tells a
 * member of another instance from one of its own. The rest (an HTTP
 * client, a resolver) take theirs only to refuse NULL and to copy its
 * allocator. */
struct crnet_instance_t {
	cr_object_head head;
#if _DEBUG
	/* The blocks of the objects made with it and destroyed (a resolver's
	 * aside, which may outlast it), freed with it (crstale.h, card X27).
	 * Every backend's crnet_instance_create and _destroy keep it. */
	cr_quarantine quarantine;
#endif
};

/* The checks every platform's crnet_instance_create makes of its desc, in
 * this order, before anything else: CR_RESULT_NULL_DESC, the struct base's,
 * CR_RESULT_NULL_OUTPUT_PARAMETER (and *out_instance set to NULL from there
 * on), then the allocator's (CR_RESULT_NULL_ALLOCATOR,
 * CR_RESULT_INVALID_ALLOCATOR). Defined in src/crnet.c. */
CR_RESULT crnet_instance_desc_check(
    const crnet_instance_desc *desc,
    crnet_instance *out_instance);

// --------------------------------------------------------------------------
// Text
// --------------------------------------------------------------------------

/* True when the first `length` bytes of the two are the same, letters
 * compared without regard to case. ASCII only, which is what HTTP's names
 * are. */
bool crnet_ascii_equal_no_case(const char *a, const char *b, size_t length);

/* True when `url` starts with `scheme` and "://", without regard to case. */
bool crnet_url_has_scheme(const char *url, const char *scheme);

/* True when the `size` bytes are UTF-8 as RFC 3629 has it: no overlong
 * forms, no surrogates, nothing past U+10FFFF. For a stack that sends text
 * without checking it. */
bool crnet_utf8_valid(const uint8_t *bytes, size_t size);

// --------------------------------------------------------------------------
// Error records
// --------------------------------------------------------------------------

/* An object's two records, by CRNET_ERROR_SIDE. */
#define CRNET_ERROR_SIDE_COUNT 2

/* Empties a record. Takes NULL. */
void crnet_error_clear(crnet_error *error);

/* Fills a record and returns `result`, so a failure is recorded where it is
 * returned. `text` may be NULL and is cut to fit. Takes a NULL `error`,
 * for the calls whose `out_error` the caller may leave out. */
CR_RESULT crnet_error_record(
    crnet_error *error,
    CR_RESULT result,
    CR_PLATFORM_ERROR_SOURCE source,
    int64_t code,
    const char *text);

/* What every _get_error does once it has found the record: `record` may be
 * NULL (no object, or no such side) and `out_error` then comes back empty. */
void crnet_error_get(const crnet_error *record, crnet_error *out_error);

// --------------------------------------------------------------------------
// TLS and proxy descs
// --------------------------------------------------------------------------

/* CR_SUCCESS for a TLS desc that makes sense, CR_RESULT_DESC_INVALID_PARAMETERS
 * for one that doesn't (the rules are with crnet_tls_desc in crnet.h).
 * Whether each root really is a certificate is the stack's to find out. */
CR_RESULT crnet_tls_desc_check(const crnet_tls_desc *tls);

/* Finds the SubjectPublicKeyInfo inside a DER certificate - the bytes a
 * crnet_tls_pin is the SHA-256 of - for the stacks that hand over a whole
 * certificate rather than its key in that form. `*out_info` points into
 * `certificate`. False when the bytes aren't laid out as a certificate. */
bool crnet_certificate_public_key_info(
    const void *certificate,
    size_t size,
    const uint8_t **out_info,
    size_t *out_size);

/* A crnet_proxy_desc taken apart, for the three forms the stacks want an
 * explicit proxy in: the whole URL, a host and a port, or "host:port". */
struct crnet_proxy {
	CRNET_PROXY mode;
	// The rest is for CRNET_PROXY_EXPLICIT and NULL or 0 otherwise.
	char    *url;          // as the caller gave it
	char    *host;         // a name or a numeric address, an IPv6 one without its brackets
	bool     host_is_ipv6; // it was given in brackets
	uint16_t port;
};

/* Checks a proxy desc (the rules are with crnet_proxy_desc in crnet.h)
 * and copies it into `out_proxy` through `allocator`, which the caller frees
 * with crnet_proxy_free and the same allocator whatever this returned. */
CR_RESULT crnet_proxy_parse(const CR_ALLOCATOR *allocator, const crnet_proxy_desc *desc, struct crnet_proxy *out_proxy);
void       crnet_proxy_free(const CR_ALLOCATOR *allocator, struct crnet_proxy *proxy);

// --------------------------------------------------------------------------
// HTTP: a response's status and headers
// --------------------------------------------------------------------------

/* Each string is allocated with its length and a terminator, through the
 * allocator the head's functions are given. */
struct crnet_http_header_entry {
	char  *name;
	size_t name_length;
	char  *value;
	size_t value_length;
};

/* Each name once: a value added under a name that is already there is
 * joined onto it with ", ", as crnet.h promises on every stack.
 *
 * A name is found through `slots`, a table of entry indices by a hash of
 * the name without regard to case, and never by walking the entries: the
 * server decides how many headers there are, and a walk for each one made
 * a block of tens of thousands of short names cost seconds on the thread
 * of the call that received it. */
typedef EQ_ARRAY(struct crnet_http_header_entry) crnet_http_header_array;

struct crnet_http_head {
	uint32_t status;
	// In the order their names first came. Grown through `allocator`.
	crnet_http_header_array entries;
	// eqstd's view of the allocator the head's calls are given
	// (cr_eq_allocator), which `entries` points at. Set again by every
	// call that grows or frees, so a zeroed head is an empty one.
	EQ_ALLOCATOR allocator;
	// Twice `entries.capacity` (0 with it), each 0 for none or an entry's
	// index + 1.
	uint32_t *slots;
	uint32_t  slot_count;
};

/* Every call that allocates or frees takes the allocator of the object
 * that holds the head (a transfer's head.allocator), the same one each
 * time. A head is never copied while it holds entries: `entries` points
 * into it. */

/* Frees what a head holds and leaves it empty. */
void crnet_head_clear(const CR_ALLOCATOR *allocator, struct crnet_http_head *head);

/* The add calls return false when out of memory. */
bool crnet_head_add_header(
    const CR_ALLOCATOR *allocator,
    struct crnet_http_head *head,
    const char *name,
    size_t name_length,
    const char *value,
    size_t value_length);

/* One line of a raw header block, with or without its line ending:
 * "Name: value" is added, a status line ("HTTP/1.1 200 OK") drops the
 * headers collected so far - they belonged to a response a redirect or an
 * interim reply replaced - and anything else is skipped. */
bool crnet_head_add_header_line(
    const CR_ALLOCATOR *allocator,
    struct crnet_http_head *head,
    const char *line,
    size_t length);

/* NULL where the head has no header of that name. */
const char *crnet_head_get_header(const struct crnet_http_head *head, const char *name);

// --------------------------------------------------------------------------
// HTTP: the transfer, and what a stack does for it
// --------------------------------------------------------------------------

/* What a request's own headers say about the response body's encoding,
 * which every HTTP backend needs before it sets its stack up to decode. */
enum crnet_accept_encoding {
	// No Accept-Encoding among them: the stack sends its own and decodes.
	CRNET_ACCEPT_ENCODING_STACK,
	// `Accept-Encoding: identity`: the caller's is sent in place of the
	// stack's, and the stack is not set up to ask for anything compressed.
	CRNET_ACCEPT_ENCODING_IDENTITY,
	// Any other value, which crnet.h refuses.
	CRNET_ACCEPT_ENCODING_INVALID,
};

/* Entries with a NULL name or value are passed over. */
enum crnet_accept_encoding crnet_headers_accept_encoding(
    const crnet_header *headers,
    uint32_t header_count);

/* Where a transfer has got to in crnet.h's order of use. */
enum crnet_http_phase {
	CRNET_HTTP_PHASE_CREATED,   // no request yet
	CRNET_HTTP_PHASE_SENDING,   // started; the request body, if any, is going in
	CRNET_HTTP_PHASE_AWAITING,  // the request's end has been said; no response yet
	CRNET_HTTP_PHASE_RESPONDED, // status and headers are in; the body is coming out
	CRNET_HTTP_PHASE_ENDED,     // the response body was received to its end
	CRNET_HTTP_PHASE_FAILED,    // only destroy is left
};

/* The public object. Everything in it is the front end's
 * (src/net/crnethttp.c) to change; a backend reads what it needs, fills
 * `response` when the response arrives (through `head.allocator`), records
 * its failures in `error`, and keeps its own state behind `backend`, made
 * and freed through `head.allocator` too.
 *
 * `head.allocator` is a copy of the client's: every backend's
 * crnet_http_client_t starts with its cr_object_head, which is how the
 * front end reaches it. */
struct crnet_http_transfer_t {
	cr_object_head        head;
	crnet_http_client     client;
	enum crnet_http_phase phase;

	// From the desc, once started.
	CRNET_HTTP_BODY body;
	uint64_t          body_size;
	uint64_t          body_sent;
	bool              is_head; // the method is exactly "HEAD"
	bool              follow_redirects;

	struct crnet_http_head response; // its status and headers
	crnet_error            error;

	// The wait set it is in, or NULL: the front end's, written by the
	// set's add and remove (src/net/crnetwaitset.c) and read by its own
	// add to refuse a second set.
	struct crnet_wait_set_t *wait_set;

	void *backend;
};

/*
 * What each HTTP backend defines. The front end has already checked every
 * argument and the order of the calls, so a backend is only ever asked for
 * the next thing crnet.h allows; and it turns every failure but
 * CR_RESULT_NET_TIMED_OUT into "only destroy is left" itself, so a
 * backend never has to refuse a call for what went before.
 *
 * Every waiting call here keeps the caller's limit on Carrottus Net's own clock
 * and returns CR_RESULT_NET_TIMED_OUT for that alone; a time-out the
 * stack reports is CR_RESULT_NET_STACK_TIMED_OUT. Each returns
 * CR_RESULT_NET_ABORTED once crnet_http_backend_abort has been
 * called.
 *
 * A start is a step (crnet.h, "Time limits", "Steps"): a backend's start
 * begins getting the request out, whatever its limit, CRNET_NO_WAIT
 * included, and waits for it no longer than the limit. A start that
 * returns CR_RESULT_NET_TIMED_OUT has started the transfer - the front end
 * moves it to CRNET_HTTP_PHASE_SENDING either way - and the backend's send
 * and receive_response first carry it on, out of their own limit: send
 * returns CR_RESULT_NET_TIMED_OUT, nothing taken, while the request isn't
 * out, and either returns the start's failure.
 */

/* Makes the backend's state for a transfer on `transfer->client`, which is
 * not NULL. */
CR_RESULT crnet_http_backend_create(struct crnet_http_transfer_t *transfer);

/* Ends the transfer wherever it is and frees the backend's state. A
 * transfer in CRNET_HTTP_PHASE_ENDED leaves its connection with the
 * client; any other drops it. */
void crnet_http_backend_destroy(struct crnet_http_transfer_t *transfer);

/* From any thread. */
void crnet_http_backend_abort(struct crnet_http_transfer_t *transfer);
bool crnet_http_backend_aborted(struct crnet_http_transfer_t *transfer);

/* Starts the request. `desc` has passed crnet_http_transfer_start's
 * checks, and `transfer`'s fields from it are set. Its time limit may be
 * anything, CRNET_NO_WAIT included: see above.
 * CR_RESULT_NET_INVALID_HTTP_TRANSFER when the client already has a
 * transfer under way. */
CR_RESULT crnet_http_backend_start(
    struct crnet_http_transfer_t *transfer,
    const crnet_http_transfer_desc *desc,
    enum crnet_accept_encoding encoding);

/* Hands the stack some of `size` bytes of request body, `size` being at
 * least 1 and no more than a sized body has left. Never 0 bytes with
 * CR_SUCCESS. */
CR_RESULT crnet_http_backend_send(
    struct crnet_http_transfer_t *transfer,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_sent);

/* Says the request's end, the first time it is called, and waits for the
 * response's status and headers, which it puts in `transfer->head`.
 * Called again after CR_RESULT_NET_TIMED_OUT. */
CR_RESULT crnet_http_backend_receive_response(
    struct crnet_http_transfer_t *transfer,
    uint32_t timeout_ms);

/* Copies up to `size` bytes of response body, `size` being at least 1.
 * 0 bytes with CR_SUCCESS is the body's end, and is not asked for
 * again. */
CR_RESULT crnet_http_backend_recv(
    struct crnet_http_transfer_t *transfer,
    void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_received);

// --------------------------------------------------------------------------
// Wait set
// --------------------------------------------------------------------------

/*
 * What a wait set (src/net/crnetwaitset.c) needs from the objects in it,
 * which only the file that defines each object can give.
 *
 * A wait is one poll() over descriptors and one descriptor of the set's
 * own to be woken through. Before every poll, the set asks each member
 * what to poll for it and whether it is ready already; a member's answer
 * may change from one wait to the next (a connect that has ended, a
 * stack's descriptors that come and go).
 *
 * - A TCP socket or listener has one descriptor the system can poll, and
 *   says which and what state the socket is in; the set does the rest.
 * - A WebSocket or an HTTP transfer answers through
 *   crnet_websocket_wait_prepare / crnet_http_backend_wait_prepare: the
 *   bits it is ready with already (what its stack has told it), any
 *   descriptors of its stack's to poll (crnet_wait_poll_add), and a time
 *   after which it is worth asking whatever the descriptors say
 *   (crnet_wait_poll_timer, a stack's own timer). One whose stack calls
 *   back instead (NSURLSession, WinHTTP) keeps the set it is in from its
 *   attach to its detach, and calls crnet_wait_set_notify whenever its
 *   answer may have changed, with whatever it guards its state with held,
 *   so that a detach that takes it is never followed by a notify.
 *
 * A member whose connect is under way is named, whatever it was added
 * for, once the connect has an outcome: its prepare returns `wait_for`
 * then, or adds the descriptor that will show it with `outcome` set.
 */

/* Wakes a set's wait. From any thread; never waits. Takes NULL. */
void crnet_wait_set_notify(struct crnet_wait_set_t *set);

/* What a member is asked to fill at each wait: the set's own, opaque to
 * the members. */
struct crnet_wait_poll;

/* Asks the wait to poll `native` (a descriptor, or a Winsock SOCKET) for
 * the member being prepared. With `outcome`, whatever the poll reports on
 * it names the member for every bit it was added with (a connect's
 * outcome, a stack's own descriptor); otherwise readable is INCOMING,
 * writable ROOM and an ending every bit, each as asked by `incoming` and
 * `room`. A set that can't grow for it fails its wait with
 * CR_RESULT_OUT_OF_MEMORY. */
void crnet_wait_poll_add(struct crnet_wait_poll *poll, intptr_t native, bool incoming, bool room, bool outcome);

/* Asks the wait to last no longer than `timeout_ms` from now, and to name
 * the member for every bit it was added with if it gets that far. */
void crnet_wait_poll_timer(struct crnet_wait_poll *poll, uint32_t timeout_ms);

#if CRNET_EXT_TCP

/* Where a TCP socket is, for a set. */
enum crnet_tcp_wait_state {
	CRNET_TCP_WAIT_NONE,       // connect never called, or it failed: refused by an add
	CRNET_TCP_WAIT_CONNECTING, // its connect is under way, or has an outcome not yet returned
	CRNET_TCP_WAIT_OPEN,       // connected
};

/* `*out_native` is the socket's descriptor where the state isn't NONE. */
enum crnet_tcp_wait_state crnet_tcp_socket_wait_state(crnet_tcp_socket socket, intptr_t *out_native);

/* False for a listener with no listening socket. */
bool crnet_tcp_listener_native(crnet_tcp_listener listener, intptr_t *out_native);

/* The instance an object was made with; an accepted socket's is its
 * listener's. NULL for NULL. */
crnet_instance crnet_tcp_socket_instance(crnet_tcp_socket socket);
crnet_instance crnet_tcp_listener_instance(crnet_tcp_listener listener);

#endif

#if CRNET_EXT_WEBSOCKET

/* The instance the socket was made with. NULL for NULL. */
crnet_instance crnet_websocket_instance(crnet_websocket socket);

/* Puts a WebSocket, connected or with its opening under way, in `set`.
 * CR_RESULT_NET_INVALID_WEBSOCKET for one whose connect was never called
 * or failed (or that was aborted), CR_RESULT_NET_ALREADY_IN_WAIT_SET for
 * one in another set. */
CR_RESULT crnet_websocket_wait_attach(crnet_websocket socket, struct crnet_wait_set_t *set);

/* After this returns, the socket makes no further crnet_wait_set_notify
 * call on the set it was in. */
void crnet_websocket_wait_detach(crnet_websocket socket);

/* The member's answer at each wait (above). `wait_for` is what it was
 * added for; the bits returned are ready already. */
uint32_t crnet_websocket_wait_prepare(crnet_websocket socket, uint32_t wait_for, struct crnet_wait_poll *poll);

#endif

/* The instance an HTTP client was made with. NULL for NULL. Defined by
 * each HTTP backend, whose client keeps it. */
crnet_instance crnet_http_client_instance(crnet_http_client client);

/* A started transfer in a set, for a backend that notifies; the front end
 * has checked it and keeps `transfer->wait_set` itself. After detach
 * returns, the backend makes no further crnet_wait_set_notify call on the
 * set. */
void crnet_http_backend_wait_attach(struct crnet_http_transfer_t *transfer, struct crnet_wait_set_t *set);
void crnet_http_backend_wait_detach(struct crnet_http_transfer_t *transfer);

/* The member's answer at each wait, for a transfer in SENDING, AWAITING
 * or RESPONDED (the front end answers for the others): INCOMING when
 * receive_response or recv, whichever `transfer->phase` makes next, would
 * return something other than CR_RESULT_NET_TIMED_OUT; ROOM when send
 * would. */
uint32_t crnet_http_backend_wait_prepare(struct crnet_http_transfer_t *transfer, uint32_t wait_for, struct crnet_wait_poll *poll);

/* The front end's answer for a transfer (src/net/crnethttp.c): every bit
 * for one whose calls would all return at once (ended, failed, aborted),
 * otherwise the backend's. */
uint32_t crnet_http_transfer_wait_prepare(struct crnet_http_transfer_t *transfer, uint32_t wait_for, struct crnet_wait_poll *poll);

#if __cplusplus
}
#endif

#endif // CRNETSHARED_H
