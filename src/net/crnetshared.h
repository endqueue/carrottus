
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
#include "../crthread.h"

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
// Addresses and the interface listing
// --------------------------------------------------------------------------

#if CRNET_EXT_TCP

/* True for an address crnet.h takes: IPv4 or IPv6, `reserved` 0, and for
 * IPv4 no scope and nothing past its fourth byte. False for NULL. */
bool crnet_address_valid(const crnet_address *address);

/* True for fe80::/10. */
bool crnet_address_is_link_local(const uint8_t bytes[16]);

/* Which side of a TCP connection an address is for: a listener's own
 * (LOCAL) or a connect's destination (REMOTE). */
enum crnet_tcp_address_use {
	CRNET_TCP_ADDRESS_LOCAL,
	CRNET_TCP_ADDRESS_REMOTE,
};

/* TCP's address rule (crnet.h, "TCP sockets"), the same on every backend
 * and checked before any system call: CR_SUCCESS, or the first of
 * CR_RESULT_NET_INVALID_ADDRESS (not a valid crnet_address, fe80::/10
 * outside fe80::/64, multicast, IPv4-mapped IPv6; and for REMOTE all-zero
 * bytes or port 0) and CR_RESULT_NET_INVALID_SCOPE (an IPv6 address whose
 * scope_id isn't there exactly when it is link-local, or is 2^28 or more).
 * NULL is INVALID_ADDRESS. */
CR_RESULT crnet_tcp_address_check(const crnet_address *address, enum crnet_tcp_address_use use);

/* What a TCP peer getter fills from getpeername()'s `length` bytes, the
 * socket being of `family` (AF_INET or AF_INET6): the reader below with
 * interface 0, then CR_RESULT_GENERAL_FAILURE for a family it can't read,
 * a family other than the socket's, a link-local peer with no scope, or an
 * fe80::/10 peer outside fe80::/64, so what it fills always passes
 * connect's rule. `*out_address` is zeroed on every failure. */
CR_RESULT crnet_tcp_peer_from_sockaddr(const void *sockaddr, size_t length, int family, crnet_address *out_address);

/* Writes `address` as a sockaddr_in or sockaddr_in6 into `out_storage`
 * (`capacity` bytes, a sockaddr_storage's worth) and returns its length; 0
 * when it doesn't fit. `address` is valid. */
uint32_t crnet_sockaddr_from_address(const crnet_address *address, void *out_storage, size_t capacity);

/* Reads a sockaddr_in or sockaddr_in6 of `length` bytes (unaligned taken)
 * into `out_address`, false for any other family or a short length. An IPv6
 * link-local address's scope is its sin6_scope_id (on Windows its SCOPE_ID's
 * 28-bit Zone alone, the Level above it dropped), or where that is 0 the
 * interface index a KAME stack embeds in bytes 2 and 3 (and
 * `*out_embedded_scope` is then true), or where both are 0
 * `interface_index`; bytes 2 and 3 come back 0. Every other address has
 * scope 0. */
bool crnet_address_from_sockaddr(const void *sockaddr, size_t length, uint32_t interface_index, crnet_address *out_address, bool *out_embedded_scope);

/* How many bits are set in a netmask sockaddr of the address family
 * `family` (AF_INET or AF_INET6) and `length` bytes, or
 * CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN for NULL or a short one. A mask
 * whose ones aren't contiguous is counted as it is. */
uint32_t crnet_netmask_prefix_length(const void *netmask, size_t length, int family);

/* One interface of a listing. */
struct crnet_interface_entry {
	char     name[CRNET_INTERFACE_NAME_CAPACITY];
	char     display_name[CRNET_INTERFACE_NAME_CAPACITY];
	uint32_t index;
	uint32_t ipv6_index;
	uint32_t flags;
	uint32_t mtu;
	// Its addresses are `address_count` entries of the list's `addresses`
	// from `first_address` on.
	uint32_t first_address;
	uint32_t address_count;
};

struct crnet_interface_address_entry {
	crnet_address address;
	uint32_t      prefix_length;
};

/* One block, made by crnet_interface_list_alloc: this, then the interfaces,
 * then the addresses. Nothing in it changes once the backend has filled it,
 * which is what lets the reads overlap (crnet.h, "Interfaces"). */
struct crnet_interface_list_t {
	cr_object_head                        head;
	size_t                                block_size;
	size_t                                block_alignment;
	uint32_t                              interface_count;
	uint32_t                              address_count;
	struct crnet_interface_entry         *interfaces;
	struct crnet_interface_address_entry *addresses;
#if _DEBUG
	// Reads inside now, which a destroy checks; destroys that found one;
	// link-local addresses whose scope came embedded (C2.13).
	volatile int32_t debug_readers;
	volatile int32_t debug_overlaps;
	uint32_t         debug_embedded_scopes;
#endif
};

/* Makes a list with room for `interface_count` interfaces and
 * `address_count` addresses, zeroed and empty (both counts 0), from the
 * instance's allocator and in its quarantine. The backend fills it and
 * counts what it filled. CR_RESULT_OUT_OF_MEMORY, `*out_list` NULL, when it
 * can't be made. */
CR_RESULT crnet_interface_list_alloc(crnet_instance instance, uint32_t interface_count, uint32_t address_count, struct crnet_interface_list_t **out_list);

/* Frees a list the backend made and won't hand over. */
void crnet_interface_list_free(struct crnet_interface_list_t *list);

/* What each platform defines: the system's table read into a new list
 * (crnet_interface_list_alloc), or a failure recorded in `out_error`. The
 * front end has checked the arguments. */
CR_RESULT crnet_interface_backend_enumerate(crnet_instance instance, struct crnet_interface_list_t **out_list, crnet_error *out_error);

#endif // CRNET_EXT_TCP

// --------------------------------------------------------------------------
// Atomics for flags another thread sets (an abort)
// --------------------------------------------------------------------------

/* A 32-bit flag or count read and written whole and in order, from C on
 * every compiler (MSVC's C has no <stdatomic.h> without a switch). */
static inline int32_t crnet_atomic_load32(volatile int32_t *value) {
#if defined(_MSC_VER)
	return (int32_t)_InterlockedOr((volatile long *)value, 0);
#else
	return __atomic_load_n(value, __ATOMIC_SEQ_CST);
#endif
}

static inline void crnet_atomic_store32(volatile int32_t *value, int32_t to) {
#if defined(_MSC_VER)
	_InterlockedExchange((volatile long *)value, to);
#else
	__atomic_store_n(value, to, __ATOMIC_SEQ_CST);
#endif
}

// --------------------------------------------------------------------------
// UDP
// --------------------------------------------------------------------------

#if CRNET_EXT_UDP

/* A UDP socket (crnet.h, "UDP sockets"). The front end, src/net/crnetudp.c,
 * makes it, checks every call and keeps its error records; the backend owns
 * `native`, `wake` and `backend`: on POSIX the descriptor and a pipe an
 * abort writes to (-1 for none), `backend` unused; on Windows the SOCKET in
 * `native` (what a wait set polls) and its events in `backend`, `wake`
 * unused. */
struct crnet_udp_socket_t {
	cr_object_head       head;
	crnet_instance       instance;
	CRNET_ADDRESS_FAMILY family;
	// By CRNET_ERROR_SIDE; each written only by its side's caller.
	crnet_error      errors[CRNET_ERROR_SIDE_COUNT];
	intptr_t         native;
	intptr_t         wake[2];
	void            *backend;
	volatile int32_t aborted;
};

/* What each platform defines. The front end has checked every argument,
 * and that the socket isn't aborted, before each. */

/* Sets `native` and `wake` to "none", before open. */
void crnet_udp_backend_reset(struct crnet_udp_socket_t *socket);
/* Opens, sets up and binds the socket as the desc says. On failure the
 * front end calls close, which takes whatever was made. */
CR_RESULT crnet_udp_backend_open(struct crnet_udp_socket_t *socket, const crnet_udp_socket_desc *desc, crnet_error *out_error);
void       crnet_udp_backend_close(struct crnet_udp_socket_t *socket);
/* Ends a wait in progress; `aborted` is already set. From any thread. */
void       crnet_udp_backend_wake(struct crnet_udp_socket_t *socket);
CR_RESULT crnet_udp_backend_send_to(struct crnet_udp_socket_t *socket, const crnet_address *to, const void *data, uint32_t size, uint32_t timeout_ms);
CR_RESULT crnet_udp_backend_recv_from(struct crnet_udp_socket_t *socket, void *buffer, uint32_t capacity, uint32_t timeout_ms, uint32_t *out_size,
                                       crnet_address *out_from);
CR_RESULT crnet_udp_backend_local_address(struct crnet_udp_socket_t *socket, crnet_address *out_address);
CR_RESULT crnet_udp_backend_buffer_sizes(struct crnet_udp_socket_t *socket, uint32_t *out_receive_buffer_size, uint32_t *out_send_buffer_size);

/* The instance the socket was made with. NULL for NULL. */
crnet_instance crnet_udp_socket_instance(crnet_udp_socket socket);

#endif // CRNET_EXT_UDP

// --------------------------------------------------------------------------
// Service discovery
// --------------------------------------------------------------------------

#if CRNET_EXT_DISCOVERY

/* Declared again below, with the wait set. */
struct crnet_wait_poll;
struct crnet_wait_set_t;

/* True for an instance name an advert takes: 1 to 63 bytes, UTF-8, no ASCII
 * control character. `length` is its length without a terminator. */
bool crnet_service_name_valid(const char *name, size_t length);

/* True for a service type an advert takes: "_" and 1 to 15 letters, digits
 * and hyphens, beginning and ending with a letter or digit, then "._udp" or
 * "._tcp", no trailing dot. */
bool crnet_service_type_valid(const char *type, size_t length);

/* True when a type a system reported ("_x._udp.") is `type`, its trailing
 * dot and ASCII case aside. */
bool crnet_service_type_matches(const char *reported, const char *type);

/* Reads one label of a DNS name in its escaped text form ("\.", "\\",
 * "\032") from `text` (`length` bytes) into `out_label` (`capacity`
 * bytes, no terminator). Returns the label's length and sets `*out_used`
 * to how much text it took, up to the '.' after it or the end; SIZE_MAX
 * for an escape cut short, a "\ddd" above 255 or a label past `capacity`. */
size_t crnet_dns_label_unescape(const char *text, size_t length, char *out_label, size_t capacity, size_t *out_used);

/* True when a full service name a system reported
 * ("Living\032room._x._udp.local.") is instance `name` of `type`. */
bool crnet_discovery_full_name_matches(const char *full_name, const char *name, const char *type);

/* The checks on an advert's TXT entries (crnet.h, crnet_service_txt_entry):
 * CR_SUCCESS and the record's size in wire form, or
 * CR_RESULT_DESC_INVALID_PARAMETERS. */
CR_RESULT crnet_service_txt_check(const crnet_service_txt_entry *entries, uint32_t count, size_t *out_size);

/* Writes entries that passed the checks in wire form (a length byte and
 * "key=value", each) and returns its size. */
size_t crnet_service_txt_write(const crnet_service_txt_entry *entries, uint32_t count, uint8_t out_record[CRNET_SERVICE_TXT_CAPACITY]);

/* A ring of one object's events, of one struct type each `slot_size`
 * bytes, made at the create. Every push and pop is under `lock`, so a
 * system thread may push while the caller pops. When it is full the oldest
 * unread events are kept and a new one is dropped, and the next pop says so
 * (CRNET_RING_DROPPED). `ended` is the object's end, handed over once every
 * event before it has been. */
struct crnet_event_ring {
	cr_mutex  lock;
	bool      lock_made;
	uint8_t  *slots;
	uint32_t  capacity;
	size_t    slot_size;
	size_t    kind_offset; // where each event's kind is: an int-sized enum
	uint32_t  first;
	uint32_t  count;
	bool      dropped;
	CR_RESULT ended; // CR_SUCCESS while the object goes on
};

enum crnet_ring_read {
	CRNET_RING_EMPTY,
	CRNET_RING_EVENT,
	CRNET_RING_DROPPED,
	CRNET_RING_ENDED,
};

/* False when out of memory (nothing is left made). */
bool                 crnet_event_ring_init(struct crnet_event_ring *ring, const CR_ALLOCATOR *allocator, uint32_t capacity, size_t slot_size, size_t kind_offset);
void                 crnet_event_ring_free(struct crnet_event_ring *ring, const CR_ALLOCATOR *allocator);
/* With `replace_kind`, an unread event of the same kind is overwritten
 * rather than a new one added. */
void                 crnet_event_ring_push(struct crnet_event_ring *ring, const void *event, bool replace_kind);
enum crnet_ring_read crnet_event_ring_pop(struct crnet_event_ring *ring, void *out_event, CR_RESULT *out_ended);
/* Whether a pop would hand anything over. */
bool                 crnet_event_ring_ready(struct crnet_event_ring *ring);

enum crnet_discovery_kind {
	CRNET_DISCOVERY_ADVERT,
	CRNET_DISCOVERY_BROWSER,
	CRNET_DISCOVERY_RESOLVER,
};

/* An advert, a browser or a resolver: the three public handles are this.
 * The front end (src/net/crnetdiscovery.c) makes it and reads its ring;
 * the backend keeps its own state behind `backend` and reports into the
 * ring through crnet_discovery_report_*. */
struct crnet_discovery_object {
	cr_object_head            head;
	crnet_instance            instance;
	enum crnet_discovery_kind kind;
	struct crnet_event_ring   ring;
	// The most recent failure's record; written with `ring.ended`, under
	// the ring's lock.
	crnet_error      error;
	volatile int32_t aborted;
	// The one set it is in, or NULL; read and written under the ring's
	// lock, so a backend that reports from a system thread wakes only a set
	// the object is still in.
	struct crnet_wait_set_t *wait_set;
	// From the desc: what the backend asks the system for.
	char                 name[CRNET_SERVICE_NAME_CAPACITY];
	char                 type[CRNET_SERVICE_TYPE_CAPACITY];
	uint32_t             interface_index;
	uint16_t             port;
	CRNET_ADDRESS_FAMILY family;
	void                *backend;
};

struct crnet_service_advert_t {
	struct crnet_discovery_object object;
};
struct crnet_service_browser_t {
	struct crnet_discovery_object object;
};
struct crnet_service_resolver_t {
	struct crnet_discovery_object object;
};

/* What a backend reports, from any thread, each under the ring's lock and
 * waking the object's set. */
void crnet_discovery_report_registered(struct crnet_discovery_object *object);
void crnet_discovery_report_not_allowed(struct crnet_discovery_object *object);
/* False, nothing reported, for a name no advert could carry or a reply of
 * another type (`type` may be NULL where the system gives none). */
bool crnet_discovery_report_found(struct crnet_discovery_object *object, bool added, uint32_t interface_index, const char *name, const char *type);
/* False, nothing reported, for a full name (NULL where the system gives
 * none) that isn't the instance resolved, or a host too long. */
bool crnet_discovery_report_service(struct crnet_discovery_object *object, const char *full_name, const char *host, uint16_t port, const uint8_t *txt,
                                    size_t txt_size);
void crnet_discovery_report_address(struct crnet_discovery_object *object, bool added, const crnet_address *address);
/* The object's end (the first one stands), its record filled. */
void crnet_discovery_report_end(struct crnet_discovery_object *object, CR_RESULT result, CR_PLATFORM_ERROR_SOURCE source, int64_t code, const char *text);
bool crnet_discovery_ended(struct crnet_discovery_object *object);

/* What each platform defines. The starts begin the system's operation from
 * the object's fields, `backend` holding whatever they made even on a
 * failure, which stop then takes. */
CR_RESULT crnet_discovery_backend_advert_start(struct crnet_discovery_object *object, const uint8_t *txt, size_t txt_size, crnet_error *out_error);
CR_RESULT crnet_discovery_backend_browser_start(struct crnet_discovery_object *object, crnet_error *out_error);
CR_RESULT crnet_discovery_backend_resolver_start(struct crnet_discovery_object *object, crnet_error *out_error);
/* Ends the operation; after it returns nothing reports into the object. */
void      crnet_discovery_backend_stop(struct crnet_discovery_object *object);
/* Ends a wait in pump; `aborted` is already set. From any thread. */
void      crnet_discovery_backend_wake(struct crnet_discovery_object *object);
/* Waits up to `timeout_ms` for the system's replies and reads what has
 * come into the ring: CR_SUCCESS when something was read (which may have
 * made no event), CR_RESULT_NET_TIMED_OUT when nothing came,
 * CR_RESULT_NET_ABORTED. */
CR_RESULT crnet_discovery_backend_pump(struct crnet_discovery_object *object, uint32_t timeout_ms);
/* What to poll for the object in a wait set, where the system has a
 * descriptor; nothing where its callbacks wake the set themselves. */
void      crnet_discovery_backend_wait_prepare(struct crnet_discovery_object *object, struct crnet_wait_poll *poll);

/* The wait set's side: one set at most (CR_RESULT_NET_ALREADY_IN_WAIT_SET
 * for a second), and the member's answer at each wait. */
CR_RESULT crnet_discovery_wait_attach(struct crnet_discovery_object *object, struct crnet_wait_set_t *set);
void       crnet_discovery_wait_detach(struct crnet_discovery_object *object);
bool       crnet_discovery_wait_is_in(struct crnet_discovery_object *object, struct crnet_wait_set_t *set);
uint32_t   crnet_discovery_wait_prepare(struct crnet_discovery_object *object, struct crnet_wait_poll *poll);

#endif // CRNET_EXT_DISCOVERY

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
 * then, or adds the descriptor that will show it with `outcome` set. One
 * whose descriptors don't show the outcome (libcurl's opening, which only
 * a call moves) carries the connect on in its prepare and hands them over
 * to wake the wait instead (crnet_wait_poll_wake), so the set asks it
 * again and names it once its prepare has seen the outcome.
 */

/* Wakes a set's wait. From any thread; never waits. Takes NULL. */
void crnet_wait_set_notify(struct crnet_wait_set_t *set);

/* What a member is asked to fill at each wait: the set's own, opaque to
 * the members. */
struct crnet_wait_poll;

/* Asks the wait to poll `native` (a descriptor, or a Winsock SOCKET) for
 * the member being prepared, for readable as `incoming` asks and writable
 * as `room` does. With `outcome`, whatever the poll reports on it names
 * the member for every bit it was added with (a connect's outcome, a
 * stack's own descriptor), and with neither asked it is polled for
 * writable (a connect's outcome shows so); otherwise readable is INCOMING,
 * writable ROOM and an ending every bit. A set that can't grow for it
 * fails its wait with CR_RESULT_OUT_OF_MEMORY. */
void crnet_wait_poll_add(struct crnet_wait_poll *poll, intptr_t native, bool incoming, bool room, bool outcome);

/* Asks the wait to poll `native` as crnet_wait_poll_add does, but to name
 * nothing for it: whatever the poll reports on it ends the poll, and the
 * set asks every member again. For a member that has to be called to learn
 * what a descriptor's news means (libcurl's opening, which its prepare
 * drives on: a wait is then a call on the whole socket, crnet.h), so that
 * it is named only once it knows. */
void crnet_wait_poll_wake(struct crnet_wait_poll *poll, intptr_t native, bool incoming, bool room);

/* Asks the wait to last no longer than `timeout_ms` from now and then to
 * ask every member again, naming nothing for it: crnet_wait_poll_wake's
 * timer. */
void crnet_wait_poll_wake_after(struct crnet_wait_poll *poll, uint32_t timeout_ms);

/* Fails the wait with CR_RESULT_OUT_OF_MEMORY, as a set that can't grow
 * does: for a member whose own array of descriptors to hand over couldn't
 * grow. */
void crnet_wait_poll_out_of_memory(struct crnet_wait_poll *poll);

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
