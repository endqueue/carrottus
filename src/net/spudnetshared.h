
#ifndef SPUDNETSHARED_H
#define SPUDNETSHARED_H

/*
 * What SpudNet's source files share with one another. Not a public header.
 *
 * Four things live here:
 *
 * - What a spudnet_instance is, which is the same everywhere: nothing but
 *   the fact that it was made, and the caller's allocator. Each platform
 *   makes and destroys it where it starts and stops its networking.
 * - Small helpers that are the same on every platform: a spudnet_error
 *   record's handling, the checks on a spudnet_tls_desc and a
 *   spudnet_proxy_desc that don't depend on the stack, the walk through a
 *   certificate that finds its public key. Defined in src/spudnet.c.
 * - The HTTP transfer. Its order of use, its argument checks, its
 *   accounting of a sized body and the storage of a response's status and
 *   headers are the same whichever stack carries it, so they are written
 *   once, in src/net/spudnethttp.c, which defines every
 *   spudnet_http_transfer_* in spudnet.h. What differs - getting the bytes
 *   to and from the network - is the stack's, behind the
 *   spudnet_http_backend_* functions each HTTP backend defines.
 * - What a wait set needs from the objects in it.
 */

#include "spudnet.h"
#include "../spudallocator.h"

#include <stddef.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* An instance holds one start of the platform's networking and, of SpudNet's
 * own, only its head: its debug name and the caller's allocator, which every
 * object made with it copies into its own head. What it started is the
 * platform's, and the platform keeps the count. Made and destroyed by the file that makes the platform's
 * calls: spudnetwindows.c (Winsock), spudnetcurl.c (libcurl) and
 * spudneturlsession.m (nothing to start).
 *
 * The objects a wait set can hold keep the instance they were made with -
 * an accepted socket keeps its listener's - which is how a set tells a
 * member of another instance from one of its own. The rest (an HTTP
 * client, a resolver) take theirs only to refuse NULL and to copy its
 * allocator. */
struct spudnet_instance_t {
	spud_object_head head;
#if _DEBUG
	/* The blocks of the objects made with it and destroyed (a resolver's
	 * aside, which may outlast it), freed with it (spudstale.h, card X27).
	 * Every backend's spudnet_instance_create and _destroy keep it. */
	spud_quarantine quarantine;
#endif
};

/* The checks every platform's spudnet_instance_create makes of its desc, in
 * this order, before anything else: SPUDRESULT_NULL_DESC, the struct base's,
 * SPUDRESULT_NULL_OUTPUT_PARAMETER (and *out_instance set to NULL from there
 * on), then the allocator's (SPUDRESULT_NULL_ALLOCATOR,
 * SPUDRESULT_INVALID_ALLOCATOR). Defined in src/spudnet.c. */
SPUDRESULT spudnet_instance_desc_check(
    const spudnet_instance_desc *desc,
    spudnet_instance *out_instance);

// --------------------------------------------------------------------------
// Text
// --------------------------------------------------------------------------

/* True when the first `length` bytes of the two are the same, letters
 * compared without regard to case. ASCII only, which is what HTTP's names
 * are. */
bool spudnet_ascii_equal_no_case(const char *a, const char *b, size_t length);

/* True when `url` starts with `scheme` and "://", without regard to case. */
bool spudnet_url_has_scheme(const char *url, const char *scheme);

/* True when the `size` bytes are UTF-8 as RFC 3629 has it: no overlong
 * forms, no surrogates, nothing past U+10FFFF. For a stack that sends text
 * without checking it. */
bool spudnet_utf8_valid(const uint8_t *bytes, size_t size);

// --------------------------------------------------------------------------
// Error records
// --------------------------------------------------------------------------

/* An object's two records, by SPUDNET_ERROR_SIDE. */
#define SPUDNET_ERROR_SIDE_COUNT 2

/* Empties a record. Takes NULL. */
void spudnet_error_clear(spudnet_error *error);

/* Fills a record and returns `result`, so a failure is recorded where it is
 * returned. `text` may be NULL and is cut to fit. Takes a NULL `error`,
 * for the calls whose `out_error` the caller may leave out. */
SPUDRESULT spudnet_error_record(
    spudnet_error *error,
    SPUDRESULT result,
    SPUDNET_ERROR_SOURCE source,
    int64_t code,
    const char *text);

/* What every _get_error does once it has found the record: `record` may be
 * NULL (no object, or no such side) and `out_error` then comes back empty. */
void spudnet_error_get(const spudnet_error *record, spudnet_error *out_error);

// --------------------------------------------------------------------------
// TLS and proxy descs
// --------------------------------------------------------------------------

/* SPUD_SUCCESS for a TLS desc that makes sense, SPUDRESULT_DESC_INVALID_PARAMETERS
 * for one that doesn't (the rules are with spudnet_tls_desc in spudnet.h).
 * Whether each root really is a certificate is the stack's to find out. */
SPUDRESULT spudnet_tls_desc_check(const spudnet_tls_desc *tls);

/* Finds the SubjectPublicKeyInfo inside a DER certificate - the bytes a
 * spudnet_tls_pin is the SHA-256 of - for the stacks that hand over a whole
 * certificate rather than its key in that form. `*out_info` points into
 * `certificate`. False when the bytes aren't laid out as a certificate. */
bool spudnet_certificate_public_key_info(
    const void *certificate,
    size_t size,
    const uint8_t **out_info,
    size_t *out_size);

/* A spudnet_proxy_desc taken apart, for the three forms the stacks want an
 * explicit proxy in: the whole URL, a host and a port, or "host:port". */
struct spudnet_proxy {
	SPUDNET_PROXY mode;
	// The rest is for SPUDNET_PROXY_EXPLICIT and NULL or 0 otherwise.
	char    *url;          // as the caller gave it
	char    *host;         // a name or a numeric address, an IPv6 one without its brackets
	bool     host_is_ipv6; // it was given in brackets
	uint16_t port;
};

/* Checks a proxy desc (the rules are with spudnet_proxy_desc in spudnet.h)
 * and copies it into `out_proxy` through `allocator`, which the caller frees
 * with spudnet_proxy_free and the same allocator whatever this returned. */
SPUDRESULT spudnet_proxy_parse(const SPUD_ALLOCATOR *allocator, const spudnet_proxy_desc *desc, struct spudnet_proxy *out_proxy);
void       spudnet_proxy_free(const SPUD_ALLOCATOR *allocator, struct spudnet_proxy *proxy);

// --------------------------------------------------------------------------
// HTTP: a response's status and headers
// --------------------------------------------------------------------------

/* Each string is allocated with its length and a terminator, through the
 * allocator the head's functions are given. */
struct spudnet_http_header_entry {
	char  *name;
	size_t name_length;
	char  *value;
	size_t value_length;
};

/* Each name once: a value added under a name that is already there is
 * joined onto it with ", ", as spudnet.h promises on every stack.
 *
 * A name is found through `slots`, a table of entry indices by a hash of
 * the name without regard to case, and never by walking the entries: the
 * server decides how many headers there are, and a walk for each one made
 * a block of tens of thousands of short names cost seconds on the thread
 * of the call that received it. */
typedef EQ_ARRAY(struct spudnet_http_header_entry) spudnet_http_header_array;

struct spudnet_http_head {
	uint32_t status;
	// In the order their names first came. Grown through `allocator`.
	spudnet_http_header_array entries;
	// eqstd's view of the allocator the head's calls are given
	// (spud_eq_allocator), which `entries` points at. Set again by every
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
void spudnet_head_clear(const SPUD_ALLOCATOR *allocator, struct spudnet_http_head *head);

/* The add calls return false when out of memory. */
bool spudnet_head_add_header(
    const SPUD_ALLOCATOR *allocator,
    struct spudnet_http_head *head,
    const char *name,
    size_t name_length,
    const char *value,
    size_t value_length);

/* One line of a raw header block, with or without its line ending:
 * "Name: value" is added, a status line ("HTTP/1.1 200 OK") drops the
 * headers collected so far - they belonged to a response a redirect or an
 * interim reply replaced - and anything else is skipped. */
bool spudnet_head_add_header_line(
    const SPUD_ALLOCATOR *allocator,
    struct spudnet_http_head *head,
    const char *line,
    size_t length);

/* NULL where the head has no header of that name. */
const char *spudnet_head_get_header(const struct spudnet_http_head *head, const char *name);

// --------------------------------------------------------------------------
// HTTP: the transfer, and what a stack does for it
// --------------------------------------------------------------------------

/* What a request's own headers say about the response body's encoding,
 * which every HTTP backend needs before it sets its stack up to decode. */
enum spudnet_accept_encoding {
	// No Accept-Encoding among them: the stack sends its own and decodes.
	SPUDNET_ACCEPT_ENCODING_STACK,
	// `Accept-Encoding: identity`: the caller's is sent in place of the
	// stack's, and the stack is not set up to ask for anything compressed.
	SPUDNET_ACCEPT_ENCODING_IDENTITY,
	// Any other value, which spudnet.h refuses.
	SPUDNET_ACCEPT_ENCODING_INVALID,
};

/* Entries with a NULL name or value are passed over. */
enum spudnet_accept_encoding spudnet_headers_accept_encoding(
    const spudnet_header *headers,
    uint32_t header_count);

/* Where a transfer has got to in spudnet.h's order of use. */
enum spudnet_http_phase {
	SPUDNET_HTTP_PHASE_CREATED,   // no request yet
	SPUDNET_HTTP_PHASE_SENDING,   // started; the request body, if any, is going in
	SPUDNET_HTTP_PHASE_AWAITING,  // the request's end has been said; no response yet
	SPUDNET_HTTP_PHASE_RESPONDED, // status and headers are in; the body is coming out
	SPUDNET_HTTP_PHASE_ENDED,     // the response body was received to its end
	SPUDNET_HTTP_PHASE_FAILED,    // only destroy is left
};

/* The public object. Everything in it is the front end's
 * (src/net/spudnethttp.c) to change; a backend reads what it needs, fills
 * `response` when the response arrives (through `head.allocator`), records
 * its failures in `error`, and keeps its own state behind `backend`, made
 * and freed through `head.allocator` too.
 *
 * `head.allocator` is a copy of the client's: every backend's
 * spudnet_http_client_t starts with its spud_object_head, which is how the
 * front end reaches it. */
struct spudnet_http_transfer_t {
	spud_object_head        head;
	spudnet_http_client     client;
	enum spudnet_http_phase phase;

	// From the desc, once started.
	SPUDNET_HTTP_BODY body;
	uint64_t          body_size;
	uint64_t          body_sent;
	bool              is_head; // the method is exactly "HEAD"
	bool              follow_redirects;

	struct spudnet_http_head response; // its status and headers
	spudnet_error            error;

	void *backend;
};

/*
 * What each HTTP backend defines. The front end has already checked every
 * argument and the order of the calls, so a backend is only ever asked for
 * the next thing spudnet.h allows; and it turns every failure but
 * SPUDRESULT_SPUDNET_TIMED_OUT into "only destroy is left" itself, so a
 * backend never has to refuse a call for what went before.
 *
 * Every waiting call here keeps the caller's limit on SpudNet's own clock
 * and returns SPUDRESULT_SPUDNET_TIMED_OUT for that alone; a time-out the
 * stack reports is SPUDRESULT_SPUDNET_STACK_TIMED_OUT. Each returns
 * SPUDRESULT_SPUDNET_ABORTED once spudnet_http_backend_abort has been
 * called.
 */

/* Makes the backend's state for a transfer on `transfer->client`, which is
 * not NULL. */
SPUDRESULT spudnet_http_backend_create(struct spudnet_http_transfer_t *transfer);

/* Ends the transfer wherever it is and frees the backend's state. A
 * transfer in SPUDNET_HTTP_PHASE_ENDED leaves its connection with the
 * client; any other drops it. */
void spudnet_http_backend_destroy(struct spudnet_http_transfer_t *transfer);

/* From any thread. */
void spudnet_http_backend_abort(struct spudnet_http_transfer_t *transfer);
bool spudnet_http_backend_aborted(struct spudnet_http_transfer_t *transfer);

/* Starts the request. `desc` has passed spudnet_http_transfer_start's
 * checks, its time limit is not SPUDNET_NO_WAIT, and `transfer`'s fields
 * from it are set. SPUDRESULT_SPUDNET_INVALID_HTTP_TRANSFER when the
 * client already has a transfer under way. */
SPUDRESULT spudnet_http_backend_start(
    struct spudnet_http_transfer_t *transfer,
    const spudnet_http_transfer_desc *desc,
    enum spudnet_accept_encoding encoding);

/* Hands the stack some of `size` bytes of request body, `size` being at
 * least 1 and no more than a sized body has left. Never 0 bytes with
 * SPUD_SUCCESS. */
SPUDRESULT spudnet_http_backend_send(
    struct spudnet_http_transfer_t *transfer,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_sent);

/* Says the request's end, the first time it is called, and waits for the
 * response's status and headers, which it puts in `transfer->head`.
 * Called again after SPUDRESULT_SPUDNET_TIMED_OUT. */
SPUDRESULT spudnet_http_backend_receive_response(
    struct spudnet_http_transfer_t *transfer,
    uint32_t timeout_ms);

/* Copies up to `size` bytes of response body, `size` being at least 1.
 * 0 bytes with SPUD_SUCCESS is the body's end, and is not asked for
 * again. */
SPUDRESULT spudnet_http_backend_recv(
    struct spudnet_http_transfer_t *transfer,
    void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_received);

// --------------------------------------------------------------------------
// Wait set
// --------------------------------------------------------------------------

/*
 * What a wait set (src/net/spudnetwaitset.c) needs from the objects in it,
 * which only the file that defines each object can give.
 *
 * A set polls sockets and has one of its own to be woken through. An
 * object with a socket the system can poll hands it over (a descriptor, or
 * a Winsock SOCKET, in an intptr_t) and has nothing more to do. A WebSocket
 * on a stack that calls back instead (NSURLSession, WinHTTP) has no such
 * socket: it keeps the set it is in, calls spudnet_wait_set_notify whenever
 * what spudnet_websocket_wait_ready answers may have changed, and the set
 * asks.
 */

#if SPUDNET_EXT_TCP || SPUDNET_EXT_WEBSOCKET

/* Wakes a set's wait. From any thread; never waits. */
void spudnet_wait_set_notify(struct spudnet_wait_set_t *set);

#endif

#if SPUDNET_EXT_TCP

/* False for an object with no connection or no listening socket. */
bool spudnet_tcp_socket_native(spudnet_tcp_socket socket, intptr_t *out_native);
bool spudnet_tcp_listener_native(spudnet_tcp_listener listener, intptr_t *out_native);

/* The instance an object was made with; an accepted socket's is its
 * listener's. NULL for NULL. */
spudnet_instance spudnet_tcp_socket_instance(spudnet_tcp_socket socket);
spudnet_instance spudnet_tcp_listener_instance(spudnet_tcp_listener listener);

#endif

#if SPUDNET_EXT_WEBSOCKET

/* The instance the socket was made with. NULL for NULL. */
spudnet_instance spudnet_websocket_instance(spudnet_websocket socket);

/* Puts a connected WebSocket in `set`. `*out_has_native` says which kind
 * it is; with a native socket, `*out_report_once` says whether the stack
 * may already be holding data its socket won't show (so the set names the
 * object once unasked). Fails as spudnet_wait_set_add_websocket documents. */
SPUDRESULT spudnet_websocket_wait_attach(
    spudnet_websocket socket,
    struct spudnet_wait_set_t *set,
    bool *out_has_native,
    intptr_t *out_native,
    bool *out_report_once);

/* After this returns, the socket makes no further spudnet_wait_set_notify
 * call on the set it was in. */
void spudnet_websocket_wait_detach(spudnet_websocket socket);

/* Whether spudnet_websocket_recv would return something other than
 * SPUDRESULT_SPUDNET_TIMED_OUT. Only asked of a socket without a native
 * one. */
bool spudnet_websocket_wait_ready(spudnet_websocket socket);

#endif

#if __cplusplus
}
#endif

#endif // SPUDNETSHARED_H
