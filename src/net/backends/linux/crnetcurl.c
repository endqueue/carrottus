
#if CARROTTUS_PLATFORM_LINUX

/*
 * Carrottus Net HTTP and WebSocket on libcurl.
 *
 * Linux has no client stack of its own; libcurl is the one every
 * distribution ships and keeps patched, with the system's TLS library and
 * trust store behind it.
 *
 * Every transfer - an HTTP transfer, a WebSocket's opening request - is
 * driven here through libcurl's multi interface rather than
 * curl_easy_perform, because that is what lets this file keep the time
 * limit itself (the whole call, to the millisecond, as crnet.h promises)
 * and lets another thread end the wait at once: curl_multi_wakeup is the
 * one libcurl call that may come from a thread other than the handle's.
 * libcurl's own timers are not used.
 *
 * libcurl moves only inside a call (crnet.h, "Steps"): a start, an opening
 * or a message that runs out of time is left where it got to - on the
 * multi handle, or as the unsent rest of a frame in the socket's buffer -
 * and every continuing call drives it on. A wait set is handed the multi
 * handle's descriptors and timer, or the connection's descriptor, afresh
 * at every wait.
 *
 * An open WebSocket is libcurl's connect-only mode: libcurl does the
 * framing, and this file waits on the connection's own descriptor with
 * poll(), beside an eventfd that abort writes to.
 *
 * TLS: a crnet_tls_desc becomes libcurl options, which libcurl hands to
 * whichever TLS library it was built with. Roots go in as one PEM blob and
 * pins as libcurl's own "sha256//" list, both made once per client or
 * socket. An option that TLS library doesn't have is reported, never
 * skipped.
 *
 * Carrottus Net's instance for Linux is here too: libcurl is the one thing on
 * this platform that has anything to start.
 */

#include "../../crnetshared.h"
#include "crnet.h"

#include <curl/curl.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "../../../crstructbase.h"

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* crnet.h requires libcurl 7.84 or later, and CMakeLists.txt asks for it;
 * this is for a build that got here some other way. */
#if LIBCURL_VERSION_NUM < 0x075400
#error "Carrottus Net: libcurl 7.84 or later is required (a crnet_instance needs a thread-safe curl_global_init)."
#endif

/* The one piece of state Carrottus Net has anywhere, and why.
 *
 * An instance may be made on any thread while others run, so libcurl's
 * global init has to be safe to make that way. It is from 7.84 on, in a
 * libcurl built thread-safe, and crnet_instance_create turns away one
 * that isn't. But the question itself - curl_version_info - is only safe
 * to ask before the first global init when that init is thread-safe, which
 * is the thing being asked. On a libcurl that would fail the check, two
 * instances made at once could therefore collide in the asking. This lock
 * puts Carrottus Net's own calls in a line, so the check is sound whichever
 * libcurl is there. On one that passes it, the lock guards nothing that
 * needed it and costs an uncontended lock per instance. */
static pthread_mutex_t crnet_curl_start_lock = PTHREAD_MUTEX_INITIALIZER;

/* BSD sockets (../posix/crnetsockets.c) have nothing to start; libcurl
 * has its global init, which it counts: it stays up until as many
 * curl_global_cleanup calls have been made. */
CR_RESULT crnet_instance_create(
    const crnet_instance_desc *desc,
    crnet_instance *out_instance,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	const CR_RESULT desc_result = crnet_instance_desc_check(desc, out_instance);
	if (desc_result != CR_SUCCESS)
		return desc_result;

	const CR_ALLOCATOR      *allocator = desc->allocator;
	struct crnet_instance_t *instance  = CR_NEW_ZEROED(allocator, struct crnet_instance_t);
	if (!instance)
		return CR_RESULT_OUT_OF_MEMORY;
	instance->head.allocator = *allocator;
	CR_BIRTH(instance);
	CR_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);

	pthread_mutex_lock(&crnet_curl_start_lock);
	// The libcurl found at run time, which need not be the one the headers
	// above came from.
	const curl_version_info_data *version     = curl_version_info(CURLVERSION_NOW);
	bool                          thread_safe = version && (version->features & CURL_VERSION_THREADSAFE) != 0;
	CURLcode                      code        = thread_safe ? curl_global_init(CURL_GLOBAL_DEFAULT) : CURLE_OK;
	pthread_mutex_unlock(&crnet_curl_start_lock);

	if (!thread_safe) {
		// Nothing was started, and nothing of the platform's failed: there
		// is no code to record.
		CR_DELETE(allocator, struct crnet_instance_t, instance);
		return CR_RESULT_NET_UNSUPPORTED;
	}
	if (code != CURLE_OK) {
		CR_DELETE(allocator, struct crnet_instance_t, instance);
		return crnet_error_record(out_error, CR_RESULT_NET_STARTUP_FAILED, CR_PLATFORM_ERROR_SOURCE_CURL, code, curl_easy_strerror(code));
	}
	*out_instance = instance;
	return CR_SUCCESS;
}

void crnet_instance_destroy(crnet_instance instance) {
	if (!instance)
		return;
	if (CR_STALE(instance, "crnet_instance_destroy", "instance"))
		return;
	// Thread-safe on any libcurl an instance could have been made with.
	curl_global_cleanup();
	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	CR_QUARANTINE_RELEASE(&instance->quarantine);
	cr_string_free(&allocator, instance->head.debug_name);
	CR_DELETE(&allocator, struct crnet_instance_t, instance);
}

// --------------------------------------------------------------------------
// Shared by HTTP and WebSocket
// --------------------------------------------------------------------------

static CR_RESULT crnet_curl_result(CURLcode code, CR_RESULT otherwise) {
	switch (code) {
	case CURLE_OPERATION_TIMEDOUT:
		// libcurl's own timer, or the system's passed up: every wait this
		// file keeps for the caller is ended by its own clock, not by this.
		return CR_RESULT_NET_STACK_TIMED_OUT;
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
		return CR_RESULT_NET_RESOLVE_FAILED;
	case CURLE_COULDNT_CONNECT:
		return CR_RESULT_NET_CONNECT_FAILED;
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
	case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
	case CURLE_SSL_INVALIDCERTSTATUS:
		return CR_RESULT_NET_TLS_FAILED;
	case CURLE_URL_MALFORMAT:
		return CR_RESULT_NET_INVALID_URL;
	case CURLE_UNSUPPORTED_PROTOCOL:
	case CURLE_NOT_BUILT_IN:
		return CR_RESULT_NET_UNSUPPORTED;
	case CURLE_OUT_OF_MEMORY:
		return CR_RESULT_OUT_OF_MEMORY;
	default:
		return otherwise;
	}
}

/* A failed libcurl call as a record, and the CR_RESULT it comes to
 * (`otherwise` where crnet_curl_result has nothing closer). `detail` is
 * the handle's CURLOPT_ERRORBUFFER, or NULL: libcurl writes there what
 * exactly went wrong, when it knows more than the code's general wording. */
static CR_RESULT crnet_curl_failed(crnet_error *record, CURLcode code, CR_RESULT otherwise, const char *detail) {
	const char *text = (detail && detail[0] != '\0') ? detail : curl_easy_strerror(code);
	return crnet_error_record(record, crnet_curl_result(code, otherwise), CR_PLATFORM_ERROR_SOURCE_CURL, code, text);
}

/* The caller's headers as the list libcurl takes. A header with no name or
 * value is CR_RESULT_DESC_INVALID_PARAMETERS and a failed allocation is
 * CR_RESULT_OUT_OF_MEMORY; `*out_list` is NULL after either. Each line is
 * put together through `allocator` and freed once libcurl has its copy;
 * the list itself is libcurl's. */
static CR_RESULT crnet_curl_headers(
    const CR_ALLOCATOR *allocator,
    const crnet_header *headers,
    uint32_t header_count,
    struct curl_slist **out_list) {
	CR_RESULT result = CR_SUCCESS;
	*out_list         = NULL;
	for (uint32_t i = 0; i < header_count; ++i) {
		if (!headers[i].name || !headers[i].value) {
			result = CR_RESULT_DESC_INVALID_PARAMETERS;
			goto failed;
		}
		size_t name_length  = strlen(headers[i].name);
		size_t value_length = strlen(headers[i].value);
		size_t line_size    = name_length + 2 + value_length + 1;
		char  *line         = (char *)cr_allocate(allocator, line_size, 1);
		if (!line) {
			result = CR_RESULT_OUT_OF_MEMORY;
			goto failed;
		}
		memcpy(line, headers[i].name, name_length);
		// libcurl reads "Name:" with nothing after it as "remove this
		// header"; "Name;" is its spelling for one sent with no value.
		if (value_length == 0) {
			memcpy(line + name_length, ";", 2);
		} else {
			memcpy(line + name_length, ": ", 2);
			memcpy(line + name_length + 2, headers[i].value, value_length + 1);
		}
		struct curl_slist *grown = curl_slist_append(*out_list, line);
		cr_free(allocator, line, line_size, 1);
		if (!grown) {
			result = CR_RESULT_OUT_OF_MEMORY;
			goto failed;
		}
		*out_list = grown;
	}
	return CR_SUCCESS;

failed:
	curl_slist_free_all(*out_list);
	*out_list = NULL;
	return result;
}

static uint64_t crnet_curl_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Milliseconds left of `timeout_ms` since `start`, as poll() takes them:
 * -1 for no limit. */
static int crnet_curl_remaining(uint64_t start, uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return -1;
	uint64_t elapsed = crnet_curl_now_ms() - start;
	if (elapsed >= timeout_ms)
		return 0;
	uint64_t left = timeout_ms - elapsed;
	return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}

/* The options every transfer here has in common. */
static void crnet_curl_common_options(CURL *curl) {
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // no SIGALRM in a host's process
	// libcurl's own limits out of the way: none on the transfer (its
	// default), and the longest it accepts on connecting - 0 there doesn't
	// mean "none" but its built-in 300 seconds.
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)INT_MAX);
}

// --------------------------------------------------------------------------
// TLS
// --------------------------------------------------------------------------

/* A crnet_tls_desc in the forms libcurl takes it. The handle it is set on
 * points at these rather than copying them, so they last as long as it. */
struct crnet_curl_tls {
	CRNET_TLS_TRUST trust;
	bool              skip_host_name_check;
	char             *roots; // every root, as PEM one after another; or NULL
	size_t            roots_size;
	size_t            roots_capacity; // the bytes `roots` was allocated with
	char             *pins;  // "sha256//<base64>;sha256//<base64>", or NULL
	size_t            pins_capacity; // the bytes `pins` was allocated with
};

/* Frees what crnet_curl_tls_create made, through the same allocator. */
static void crnet_curl_tls_free(const CR_ALLOCATOR *allocator, struct crnet_curl_tls *tls) {
	cr_free(allocator, tls->roots, tls->roots_capacity, 1);
	cr_free(allocator, tls->pins, tls->pins_capacity, 1);
	tls->roots          = NULL;
	tls->roots_capacity = 0;
	tls->pins           = NULL;
	tls->pins_capacity  = 0;
}

static size_t crnet_curl_base64_length(size_t size) { return (size + 2) / 3 * 4; }

/* Writes crnet_curl_base64_length(size) characters, padded, with no
 * terminator, and returns the position after them. */
static char *crnet_curl_base64(const uint8_t *data, size_t size, char *out) {
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (size_t i = 0; i < size; i += 3) {
		size_t   left  = size - i;
		uint32_t group = (uint32_t)data[i] << 16;
		if (left > 1)
			group |= (uint32_t)data[i + 1] << 8;
		if (left > 2)
			group |= (uint32_t)data[i + 2];
		*out++ = digits[(group >> 18) & 0x3F];
		*out++ = digits[(group >> 12) & 0x3F];
		*out++ = left > 1 ? digits[(group >> 6) & 0x3F] : '=';
		*out++ = left > 2 ? digits[group & 0x3F] : '=';
	}
	return out;
}

/* Fills `out_tls` through `allocator`, which the caller frees with
 * crnet_curl_tls_free and the same allocator. */
static CR_RESULT crnet_curl_tls_create(const CR_ALLOCATOR *allocator, const crnet_tls_desc *desc, struct crnet_curl_tls *out_tls) {
	static const char pem_begin[]  = "-----BEGIN CERTIFICATE-----\n";
	static const char pem_end[]    = "-----END CERTIFICATE-----\n";
	static const char pin_prefix[] = "sha256//";

	memset(out_tls, 0, sizeof(*out_tls));
	CR_RESULT result = crnet_tls_desc_check(desc);
	if (CR_FAILED(result))
		return result;
	out_tls->trust                = desc->trust;
	out_tls->skip_host_name_check = desc->skip_host_name_check;

	if (desc->root_count > 0) {
		// DER to PEM: base64 in lines of 64 between the two markers. Whether
		// the bytes are a certificate is libcurl's to find out, at the first
		// connection.
		size_t total = 0;
		for (uint32_t i = 0; i < desc->root_count; ++i) {
			if (desc->roots[i].size > SIZE_MAX / 4)
				return CR_RESULT_DESC_INVALID_PARAMETERS;
			size_t encoded = crnet_curl_base64_length((size_t)desc->roots[i].size);
			size_t needed  = sizeof(pem_begin) + encoded + encoded / 64 + 1 + sizeof(pem_end);
			if (needed > SIZE_MAX - total)
				return CR_RESULT_OUT_OF_MEMORY;
			total += needed;
		}
		// `line` holds one root's base64 before it is cut into lines; the
		// total is room enough for the longest.
		out_tls->roots = (char *)cr_allocate(allocator, total, 1);
		if (out_tls->roots)
			out_tls->roots_capacity = total;
		char *line = (char *)cr_allocate(allocator, total, 1);
		if (!out_tls->roots || !line) {
			cr_free(allocator, line, total, 1);
			crnet_curl_tls_free(allocator, out_tls);
			return CR_RESULT_OUT_OF_MEMORY;
		}
		char *at = out_tls->roots;
		for (uint32_t i = 0; i < desc->root_count; ++i) {
			size_t encoded = (size_t)(crnet_curl_base64((const uint8_t *)desc->roots[i].data, (size_t)desc->roots[i].size, line) - line);
			memcpy(at, pem_begin, sizeof(pem_begin) - 1);
			at += sizeof(pem_begin) - 1;
			for (size_t done = 0; done < encoded; done += 64) {
				size_t count = encoded - done < 64 ? encoded - done : 64;
				memcpy(at, line + done, count);
				at   += count;
				*at++ = '\n';
			}
			memcpy(at, pem_end, sizeof(pem_end) - 1);
			at += sizeof(pem_end) - 1;
		}
		cr_free(allocator, line, total, 1);
		out_tls->roots_size = (size_t)(at - out_tls->roots);
	}

	if (desc->pin_count > 0) {
		size_t each   = sizeof(pin_prefix) - 1 + crnet_curl_base64_length(CRNET_TLS_PIN_SIZE) + 1; // and a ';' or the terminator
		out_tls->pins = (char *)cr_allocate_array(allocator, desc->pin_count, each, 1, false);
		if (!out_tls->pins) {
			crnet_curl_tls_free(allocator, out_tls);
			return CR_RESULT_OUT_OF_MEMORY;
		}
		out_tls->pins_capacity = (size_t)desc->pin_count * each;
		char *at = out_tls->pins;
		for (uint32_t i = 0; i < desc->pin_count; ++i) {
			memcpy(at, pin_prefix, sizeof(pin_prefix) - 1);
			at    = crnet_curl_base64(desc->pins[i].sha256, CRNET_TLS_PIN_SIZE, at + sizeof(pin_prefix) - 1);
			*at++ = i + 1 < desc->pin_count ? ';' : '\0';
		}
	}
	return CR_SUCCESS;
}

/* Sets what `tls` asks for on a handle that is at libcurl's defaults for
 * all of it. CR_RESULT_NET_UNSUPPORTED when this libcurl, or the TLS
 * library behind it, doesn't have one of the options. */
static CR_RESULT crnet_curl_tls_apply(CURL *curl, const struct crnet_curl_tls *tls) {
	CURLcode code = CURLE_OK;

	// libcurl's own defaults are 1 and 2; said here so the desc decides both.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls->trust == CRNET_TLS_TRUST_ANY ? 0L : 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls->skip_host_name_check ? 0L : 2L);

	if (tls->roots) {
#if LIBCURL_VERSION_NUM >= 0x075400
		// Which of the three sources libcurl reads - this blob, the system's
		// bundle file, the system's certificate directory - once a blob is
		// set has varied between its versions, so each is named outright.
		// The system's two are whatever this libcurl was built to use.
		const curl_version_info_data *info        = curl_version_info(CURLVERSION_NOW);
		bool                          with_system = tls->trust == CRNET_TLS_TRUST_SYSTEM_AND_ROOTS;
		const char                   *system_file = with_system && info->age >= CURLVERSION_TENTH ? info->cainfo : NULL;
		const char                   *system_path = with_system && info->age >= CURLVERSION_TENTH ? info->capath : NULL;
		if (with_system && !system_file && !system_path)
			return CR_RESULT_NET_UNSUPPORTED; // it doesn't say where the system's are

		struct curl_blob blob;
		blob.data  = tls->roots;
		blob.len   = tls->roots_size;
		blob.flags = CURL_BLOB_NOCOPY;
		code       = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
		if (code == CURLE_OK)
			code = curl_easy_setopt(curl, CURLOPT_CAINFO, system_file);
		if (code == CURLE_OK)
			code = curl_easy_setopt(curl, CURLOPT_CAPATH, system_path);
#else
		// Roots from memory need 7.77, and naming the system's own beside
		// them 7.84.
		return CR_RESULT_NET_UNSUPPORTED;
#endif
	}
	if (code == CURLE_OK && tls->pins)
		code = curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, tls->pins);

	if (code == CURLE_OUT_OF_MEMORY)
		return CR_RESULT_OUT_OF_MEMORY;
	return code == CURLE_OK ? CR_SUCCESS : CR_RESULT_NET_UNSUPPORTED;
}

/* Whether the libcurl found at run time looks host names up without
 * blocking (its threaded resolver or c-ares). One that doesn't looks them
 * up inside curl_multi_perform, for as long as the name server takes, so a
 * call with CRNET_NO_WAIT could block there: an HTTP client and a
 * WebSocket are refused on it (crnet.h). Asked at each create and kept
 * nowhere. */
static bool crnet_curl_resolves_asynchronously(void) {
	const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
	return info && (info->features & CURL_VERSION_ASYNCHDNS) != 0;
}

/* The array a multi handle's descriptors are handed over in, for a wait
 * set: grown through the owner's allocator when libcurl has more than it
 * holds, and kept, so a steady set allocates nothing (C6.8). */
struct crnet_curl_wait_fds {
#if LIBCURL_VERSION_NUM >= 0x080800
	struct curl_waitfd *fds;
	unsigned int        capacity;
#else
	int unused;
#endif
};

static void crnet_curl_wait_fds_free(const CR_ALLOCATOR *allocator, struct crnet_curl_wait_fds *array) {
#if LIBCURL_VERSION_NUM >= 0x080800
	if (array->fds)
		CR_DELETE_ARRAY(allocator, struct curl_waitfd, array->fds, array->capacity);
	array->fds      = NULL;
	array->capacity = 0;
#else
	(void)allocator;
	(void)array;
#endif
}

/* One of a multi handle's descriptors, or its timer, for a wait set: with
 * `wake`, only a reason for the set to ask the member again; otherwise a
 * naming. */
static void crnet_curl_wait_add(struct crnet_wait_poll *poll, bool wake, curl_socket_t fd, bool in, bool out) {
	if (wake)
		crnet_wait_poll_wake(poll, (intptr_t)fd, in, out);
	else
		crnet_wait_poll_add(poll, (intptr_t)fd, in, out, true);
}

/* Hands a wait set everything `multi` would wait on now - its descriptors,
 * polled for what libcurl asks, and its timer (P7). Without `wake`
 * anything on them names the member, whose next call drives the multi
 * handle (a reason to try); with it they only have the set ask the member
 * again, for one whose prepare drives the handle itself. Returns the bits
 * the member is ready with already: every one where libcurl couldn't be
 * asked, so that the call it is named for says what went wrong. */
static uint32_t crnet_curl_wait_multi(
    CURLM *multi,
    const CR_ALLOCATOR *allocator,
    struct crnet_curl_wait_fds *array,
    uint32_t wait_for,
    bool wake,
    struct crnet_wait_poll *poll) {
#if LIBCURL_VERSION_NUM >= 0x080800
	// An array too small for them all is answered with the count alone:
	// CURLM_OK with no room (libcurl 8.18's multi.c sizes the array that
	// way, NULL and 0 included) or CURLM_OUT_OF_MEMORY with some. The count
	// may also grow between sizing and filling (its page), so it is asked
	// again; a few rounds are more than descriptors that change inside one
	// call need.
	unsigned int count = 0;
	CURLMcode    code  = CURLM_OK;
	for (int round = 0;; ++round) {
		count = 0;
		code  = curl_multi_waitfds(multi, array->fds, array->capacity, &count);
		if (code != CURLM_OK && code != CURLM_OUT_OF_MEMORY)
			break;
		if (code == CURLM_OK && count <= array->capacity)
			break;
		if (round == 3)
			return wait_for;
		unsigned int needed = count;
		if (code == CURLM_OUT_OF_MEMORY && (curl_multi_waitfds(multi, NULL, 0, &needed) != CURLM_OK || needed <= array->capacity))
			continue;
		struct curl_waitfd *grown = CR_NEW_ARRAY(allocator, struct curl_waitfd, needed);
		if (!grown) {
			crnet_wait_poll_out_of_memory(poll);
			return 0;
		}
		crnet_curl_wait_fds_free(allocator, array);
		array->fds      = grown;
		array->capacity = needed;
	}
	if (code != CURLM_OK)
		return wait_for;
	for (unsigned int i = 0; i < count && i < array->capacity; ++i) {
		short events = array->fds[i].events;
		crnet_curl_wait_add(poll, wake, array->fds[i].fd, (events & (CURL_WAIT_POLLIN | CURL_WAIT_POLLPRI)) != 0, (events & CURL_WAIT_POLLOUT) != 0);
	}
#else
	// Before 8.8: the sets curl_multi_fdset fills, turned into entries.
	(void)allocator;
	(void)array;
	fd_set readable, writable, failing;
	FD_ZERO(&readable);
	FD_ZERO(&writable);
	FD_ZERO(&failing);
	int highest = -1;
	if (curl_multi_fdset(multi, &readable, &writable, &failing, &highest) != CURLM_OK)
		return wait_for;
	for (int fd = 0; fd <= highest; ++fd) {
		bool in  = FD_ISSET(fd, &readable) || FD_ISSET(fd, &failing);
		bool out = FD_ISSET(fd, &writable);
		if (in || out)
			crnet_curl_wait_add(poll, wake, (curl_socket_t)fd, in, out);
	}
#endif
	long timer = -1;
	if (curl_multi_timeout(multi, &timer) != CURLM_OK)
		return wait_for;
	if (timer >= 0) {
		uint32_t ms = timer > (long)UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)timer;
		if (wake)
			crnet_wait_poll_wake_after(poll, ms);
		else
			crnet_wait_poll_timer(poll, ms);
	}
	return 0;
}

// Only a WebSocket's opening request is run this way; an HTTP transfer has
// a loop of its own further down, which stops and starts.
#if LIBCURL_VERSION_NUM >= 0x080b00

enum crnet_curl_outcome {
	CRNET_CURL_FINISHED,  // the transfer ended; *out_code says how
	CRNET_CURL_TIMED_OUT, // it goes on, on the multi handle
	CRNET_CURL_STOPPED,   // `stop` was set
};

/* Drives `curl`, already on `multi`, until the transfer ends, `timeout_ms`
 * since `start` has passed, or `stop` is set (by a thread that then calls
 * curl_multi_wakeup). With CRNET_NO_WAIT it makes the one pass that costs
 * no waiting. The handle stays on the multi handle whatever happens: a
 * connect-only handle is kept there for its connection's life (C4.2), and
 * one that didn't finish is carried on by the next drive. */
static enum crnet_curl_outcome crnet_curl_drive(
    CURLM *multi,
    CURL *curl,
    uint64_t start,
    uint32_t timeout_ms,
    atomic_bool *stop,
    CURLcode *out_code) {
	*out_code = CURLE_FAILED_INIT;
	for (;;) {
		if (atomic_load(stop))
			return CRNET_CURL_STOPPED;

		int running = 0;
		if (curl_multi_perform(multi, &running) != CURLM_OK)
			return CRNET_CURL_FINISHED;
		bool     done = false;
		int      left = 0;
		CURLMsg *message;
		while ((message = curl_multi_info_read(multi, &left)) != NULL) {
			if (message->msg == CURLMSG_DONE && message->easy_handle == curl) {
				*out_code = message->data.result;
				done      = true;
			}
		}
		if (done || running == 0)
			return CRNET_CURL_FINISHED;

		int remaining = crnet_curl_remaining(start, timeout_ms);
		if (remaining == 0)
			return CRNET_CURL_TIMED_OUT;
		// curl_multi_poll has no "no limit"; a long wait, repeated, is one.
		// It returns early for activity and for curl_multi_wakeup.
		curl_multi_poll(multi, NULL, 0, (remaining < 0 || remaining > 60000) ? 60000 : remaining, NULL);
	}
}

#endif // LIBCURL_VERSION_NUM

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------

/* A transfer here is one easy handle on its client's multi handle. libcurl
 * moves a body by calling back - for the next piece of a request body, with
 * the next piece of a response body - and crnet.h has the caller ask
 * instead, so the two are joined by pausing: a callback that finds the
 * caller isn't in the call that would feed it or take from it says "pause",
 * and the next such call unpauses and drives the multi handle until the
 * callback has been satisfied, the transfer has ended, or the caller's time
 * is up. libcurl only ever runs on the thread that is inside one of these
 * calls.
 *
 * The connections belong to the multi handle, which is what makes a
 * transfer that ran to its end leave its connection for the client's next
 * one: taking a finished easy handle off the multi handle keeps the
 * connection, taking an unfinished one off drops it. */

struct crnet_http_client_t {
	cr_object_head head; // first: crnethttp.c reaches the allocator through it
	crnet_instance instance; // for a wait set's check (crnet_http_client_instance)
	// Holds the open connections, and is driven by whichever transfer is
	// under way - one at a time, which `busy` keeps to.
	CURLM      *multi;
	atomic_bool busy;
	// Set on each transfer's handle when it starts.
	struct crnet_curl_tls tls;
	struct crnet_proxy    proxy;
	// The multi handle's descriptors as a wait set is handed them, kept for
	// the client's next transfer (the multi handle, and the waits, are the
	// one transfer under way's).
	struct crnet_curl_wait_fds wait_fds;
};

crnet_instance crnet_http_client_instance(crnet_http_client client) { return client ? client->instance : NULL; }

/* What `proxy` asks for on a handle that is at libcurl's default, which is
 * the environment's proxy variables. libcurl copies the string. */
static void crnet_curl_proxy_apply(CURL *curl, const struct crnet_proxy *proxy) {
	if (proxy->mode == CRNET_PROXY_NONE)
		curl_easy_setopt(curl, CURLOPT_PROXY, ""); // "" also stops it reading the environment
	else if (proxy->mode == CRNET_PROXY_EXPLICIT)
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy->url);
}

void crnet_http_client_destroy(crnet_http_client client) {
	if (!client)
		return;
	if (CR_STALE(client, "crnet_http_client_destroy", "client"))
		return;
	if (client->multi)
		curl_multi_cleanup(client->multi);
	CR_KILL(client);
	const CR_ALLOCATOR allocator = client->head.allocator;
	crnet_curl_tls_free(&allocator, &client->tls);
	crnet_proxy_free(&allocator, &client->proxy);
	crnet_curl_wait_fds_free(&allocator, &client->wait_fds);
	cr_string_free(&allocator, client->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(client), &allocator, struct crnet_http_client_t, client);
}

CR_RESULT crnet_http_client_create(
    crnet_instance instance,
    const crnet_http_client_desc *desc,
    crnet_http_client *out_client) {
	if (!out_client)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_client = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_http_client_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	// After the argument checks, before anything is made (Q9): nothing to
	// record, since nothing of the platform's failed.
	if (!crnet_curl_resolves_asynchronously())
		return CR_RESULT_NET_UNSUPPORTED;

	struct crnet_http_client_t *client = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_http_client_t);
	if (!client)
		return CR_RESULT_OUT_OF_MEMORY;
	client->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(client, &instance->quarantine);
	client->instance       = instance;
	atomic_init(&client->busy, false);
	CR_RESULT result = crnet_curl_tls_create(&client->head.allocator, &desc->tls, &client->tls);
	if (!CR_FAILED(result))
		result = crnet_proxy_parse(&client->head.allocator, &desc->proxy, &client->proxy);
	if (!CR_FAILED(result)) {
		client->multi = curl_multi_init();
		if (!client->multi)
			result = CR_RESULT_GENERAL_FAILURE;
	}
	if (CR_FAILED(result)) {
		crnet_http_client_destroy(client);
		return result;
	}

	*out_client = client;
	return CR_SUCCESS;
}

/* The backend's side of a crnet_http_transfer_t. */
struct crnet_curl_http {
	struct crnet_http_transfer_t *owner;
	struct crnet_http_client_t   *client;
	CURL                           *curl;
	struct curl_slist              *headers;
	bool                            on_multi;     // the handle has been added
	bool                            holds_client; // this transfer set the client's `busy`
	atomic_bool                     aborted;
	char                            detail[CURL_ERROR_SIZE];
	bool                            out_of_memory;

	// The connection is up and the request about to go out on it.
	bool connected;
	// The start ran out of time before that: send and receive_response
	// carry it on first, out of their own limit.
	bool start_pending;

	// The request body. `send_data` is the caller's and is only set while
	// a send call is driving; the callback copies out of it.
	const uint8_t *send_data;
	size_t         send_size;
	size_t         send_taken;
	bool           send_paused; // the callback found nothing to send and paused
	bool           send_ended;  // the caller has said the body is complete

	// The response's headers. A block of them is the response's own unless
	// it is an interim reply or a redirect libcurl is about to follow.
	bool block_has_location;
	bool headers_done;

	// The response body: the one piece libcurl has handed over that the
	// caller hasn't taken all of yet. While there is one, the callback
	// pauses rather than take another.
	uint8_t *pending;
	size_t   pending_size;
	size_t   pending_offset;
	size_t   pending_capacity;
	bool     recv_paused;

	// The transfer has ended, and how.
	bool     finished;
	CURLcode finish_code;
};

static size_t crnet_curl_http_on_header(char *data, size_t size, size_t count, void *user) {
	struct crnet_curl_http *http  = (struct crnet_curl_http *)user;
	size_t                    bytes = size * count;
	// What follows the response's own headers is a chunked body's trailer.
	if (http->headers_done)
		return bytes;

	if (bytes >= 5 && crnet_ascii_equal_no_case(data, "HTTP/", 5))
		http->block_has_location = false;
	if (bytes >= 9 && crnet_ascii_equal_no_case(data, "Location:", 9))
		http->block_has_location = true;
	// A status line empties what was collected, so only the last block's
	// headers are left.
	// libcurl calls back from inside curl_multi_perform, on the thread of
	// the transfer call that is driving it: the caller's.
	if (!crnet_head_add_header_line(&http->owner->head.allocator, &http->owner->response, data, bytes)) {
		http->out_of_memory = true;
		return 0; // anything but the full count stops the transfer
	}

	// A blank line ends a block.
	bool blank = bytes == 0 || data[0] == '\r' || data[0] == '\n';
	if (blank) {
		long status = 0;
		curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
		bool interim   = status < 200;
		bool redirects = http->owner->follow_redirects && status >= 300 && status < 400 && http->block_has_location;
		if (!interim && !redirects)
			http->headers_done = true;
	}
	return bytes;
}

/* libcurl calls this with the body already decoded. A piece is taken whole
 * or not at all: paused, libcurl keeps it and offers it again. */
static size_t crnet_curl_http_on_body(char *data, size_t size, size_t count, void *user) {
	struct crnet_curl_http *http  = (struct crnet_curl_http *)user;
	size_t                    bytes = size * count;
	http->headers_done              = true; // a body can only follow them
	if (bytes == 0)
		return 0;

	if (http->pending_offset < http->pending_size) {
		http->recv_paused = true;
		return CURL_WRITEFUNC_PAUSE;
	}
	if (bytes > http->pending_capacity) {
		// The old piece has all been taken, so nothing of it is kept. On the
		// caller's thread, as the header callback is.
		const CR_ALLOCATOR *allocator = &http->owner->head.allocator;
		uint8_t              *moved     = (uint8_t *)cr_allocate(allocator, bytes, 1);
		if (!moved) {
			http->out_of_memory = true;
			return 0; // anything but the full count stops the transfer
		}
		cr_free(allocator, http->pending, http->pending_capacity, 1);
		http->pending          = moved;
		http->pending_capacity = bytes;
	}
	memcpy(http->pending, data, bytes);
	http->pending_size   = bytes;
	http->pending_offset = 0;
	return bytes;
}

static size_t crnet_curl_http_on_read(char *buffer, size_t size, size_t count, void *user) {
	struct crnet_curl_http *http = (struct crnet_curl_http *)user;
	size_t                    room = size * count;
	if (http->send_ended)
		return 0; // the body's end
	if (!http->send_data || http->send_taken == http->send_size) {
		http->send_paused = true;
		return CURL_READFUNC_PAUSE;
	}
	size_t left  = http->send_size - http->send_taken;
	size_t bytes = left < room ? left : room;
	memcpy(buffer, http->send_data + http->send_taken, bytes);
	http->send_taken += bytes;
	return bytes;
}

#if LIBCURL_VERSION_NUM >= 0x075000
/* Called when the connection is up - a new one, or one the multi handle
 * was keeping - and before the request is sent on it. */
static int crnet_curl_http_on_connected(void *user, char *primary_ip, char *local_ip, int primary_port, int local_port) {
	(void)primary_ip;
	(void)local_ip;
	(void)primary_port;
	(void)local_port;
	((struct crnet_curl_http *)user)->connected = true;
	return CURL_PREREQFUNC_OK;
}
#endif

enum crnet_curl_http_goal {
	CRNET_CURL_HTTP_CONNECTED,
	CRNET_CURL_HTTP_SENT_SOME,
	CRNET_CURL_HTTP_HEADERS,
	CRNET_CURL_HTTP_BODY_PIECE,
};

static bool crnet_curl_http_reached(const struct crnet_curl_http *http, enum crnet_curl_http_goal goal) {
	switch (goal) {
	case CRNET_CURL_HTTP_CONNECTED:
		return http->connected;
	case CRNET_CURL_HTTP_SENT_SOME:
		return http->send_taken > 0;
	case CRNET_CURL_HTTP_HEADERS:
		return http->headers_done;
	default:
		return http->pending_offset < http->pending_size;
	}
}

/* Runs the client's multi handle until the transfer has reached `goal` or
 * ended (CR_SUCCESS for either; the caller looks at which), the caller's
 * time is up, or the transfer is aborted. With CRNET_NO_WAIT it makes the
 * one pass that costs no waiting. */
static CR_RESULT crnet_curl_http_drive(struct crnet_curl_http *http, enum crnet_curl_http_goal goal, uint64_t start, uint32_t timeout_ms) {
	CURLM *multi = http->client->multi;
	for (;;) {
		if (atomic_load(&http->aborted))
			return CR_RESULT_NET_ABORTED;

		int running = 0;
		if (curl_multi_perform(multi, &running) != CURLM_OK)
			return CR_RESULT_NET_HTTP_FAILED;
		int      left = 0;
		CURLMsg *message;
		while ((message = curl_multi_info_read(multi, &left)) != NULL) {
			if (message->msg == CURLMSG_DONE && message->easy_handle == http->curl) {
				http->finished    = true;
				http->finish_code = message->data.result;
			}
		}
		if (http->out_of_memory)
			return CR_RESULT_OUT_OF_MEMORY;
		if (http->finished || crnet_curl_http_reached(http, goal))
			return CR_SUCCESS;

		int remaining = crnet_curl_remaining(start, timeout_ms);
		if (remaining == 0)
			return CR_RESULT_NET_TIMED_OUT;
		// curl_multi_poll has no "no limit"; a long wait, repeated, is one.
		// It returns early for activity and for curl_multi_wakeup.
		curl_multi_poll(multi, NULL, 0, (remaining < 0 || remaining > 60000) ? 60000 : remaining, NULL);
	}
}

/* The record and the result for a transfer libcurl ended with an error. */
static CR_RESULT crnet_curl_http_failed(struct crnet_curl_http *http, CR_RESULT otherwise) {
	return crnet_curl_failed(&http->owner->error, http->finish_code, otherwise, http->detail);
}

/* Lets a paused direction go on. It lifts both, which is harmless: the
 * other direction's callback pauses again if it still has nothing to do. */
static void crnet_curl_http_unpause(struct crnet_curl_http *http) {
	http->send_paused = false;
	http->recv_paused = false;
	curl_easy_pause(http->curl, CURLPAUSE_CONT);
}

CR_RESULT crnet_http_backend_create(struct crnet_http_transfer_t *transfer) {
	struct crnet_curl_http *http = CR_NEW_ZEROED(&transfer->head.allocator, struct crnet_curl_http);
	if (!http)
		return CR_RESULT_OUT_OF_MEMORY;
	http->owner  = transfer;
	http->client = transfer->client;
	atomic_init(&http->aborted, false);
	http->curl = curl_easy_init();
	if (!http->curl) {
		CR_DELETE(&transfer->head.allocator, struct crnet_curl_http, http);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	transfer->backend = http;
	return CR_SUCCESS;
}

void crnet_http_backend_destroy(struct crnet_http_transfer_t *transfer) {
	struct crnet_curl_http *http = (struct crnet_curl_http *)transfer->backend;
	if (!http)
		return;
	// Off the multi handle first: for a transfer that didn't run to its
	// end this is what drops its connection.
	if (http->on_multi)
		curl_multi_remove_handle(http->client->multi, http->curl);
	curl_easy_cleanup(http->curl);
	curl_slist_free_all(http->headers);
	cr_free(&transfer->head.allocator, http->pending, http->pending_capacity, 1);
	if (http->holds_client)
		atomic_store(&http->client->busy, false);
	CR_DELETE(&transfer->head.allocator, struct crnet_curl_http, http);
	transfer->backend = NULL;
}

void crnet_http_backend_abort(struct crnet_http_transfer_t *transfer) {
	struct crnet_curl_http *http = (struct crnet_curl_http *)transfer->backend;
	atomic_store(&http->aborted, true);
	// The one libcurl call that may come from another thread.
	curl_multi_wakeup(http->client->multi);
}

bool crnet_http_backend_aborted(struct crnet_http_transfer_t *transfer) {
	return atomic_load(&((struct crnet_curl_http *)transfer->backend)->aborted);
}

CR_RESULT crnet_http_backend_start(
    struct crnet_http_transfer_t *transfer,
    const crnet_http_transfer_desc *desc,
    enum crnet_accept_encoding encoding) {
	struct crnet_curl_http *http  = (struct crnet_curl_http *)transfer->backend;
	uint64_t                  start = crnet_curl_now_ms();

	// One transfer drives the client's multi handle at a time.
	if (atomic_exchange(&http->client->busy, true))
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	http->holds_client = true;

	CR_RESULT headers_result = crnet_curl_headers(&transfer->head.allocator, desc->headers, desc->header_count, &http->headers);
	if (CR_FAILED(headers_result))
		return headers_result;

	CURL *curl = http->curl;
	crnet_curl_common_options(curl);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, http->detail);
	curl_easy_setopt(curl, CURLOPT_URL, desc->url);
	// The method as given. On its own this only renames the request;
	// libcurl takes the request's shape from the body options below.
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, desc->method);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, desc->follow_redirects ? 1L : 0L);
	// "" is every encoding libcurl can decode, and has it send the header
	// that says so. Left unset libcurl sends no Accept-Encoding of its own
	// and decodes nothing, which leaves the caller's `identity`, among its
	// headers, as the only one.
	if (encoding == CRNET_ACCEPT_ENCODING_STACK)
		curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, crnet_curl_http_on_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, http);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, crnet_curl_http_on_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, http);
#if LIBCURL_VERSION_NUM >= 0x075000
	curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, crnet_curl_http_on_connected);
	curl_easy_setopt(curl, CURLOPT_PREREQDATA, http);
#endif

	// The request's shape comes from what the desc says its body is, and
	// from nothing about the method but the one case HTTP itself singles
	// out: a HEAD's response has no body to wait for.
	if (transfer->body == CRNET_HTTP_BODY_NONE) {
		if (transfer->is_head)
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	} else {
		// An upload: the body is asked for through the read callback. With
		// a size libcurl announces it (Content-Length, 0 included); without
		// one it sends the body chunked.
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, crnet_curl_http_on_read);
		curl_easy_setopt(curl, CURLOPT_READDATA, http);
		if (transfer->body == CRNET_HTTP_BODY_SIZED)
			curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)transfer->body_size);
		// libcurl would hold a large body back for the server's go-ahead
		// ("Expect: 100-continue"); the header given empty takes that off.
		struct curl_slist *grown = curl_slist_append(http->headers, "Expect:");
		if (!grown)
			return CR_RESULT_OUT_OF_MEMORY;
		http->headers = grown;
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http->headers);

	CR_RESULT result = crnet_curl_tls_apply(curl, &http->client->tls);
	if (CR_FAILED(result))
		return result;
	crnet_curl_proxy_apply(curl, &http->client->proxy);

	if (curl_multi_add_handle(http->client->multi, curl) != CURLM_OK)
		return CR_RESULT_NET_HTTP_FAILED;
	http->on_multi = true;

#if LIBCURL_VERSION_NUM >= 0x075000
	// As far as the connection being up, so that a server that can't be
	// reached or a certificate that is turned away is start's to report.
	// Begun whatever the limit (crnet.h, "Steps"): with CRNET_NO_WAIT the
	// drive makes one pass, and a start that runs out of time leaves the
	// transfer on the multi handle for send or receive_response to carry
	// on. An older libcurl doesn't say when the connection is up, and there
	// the first later call reports it, as crnet.h allows.
	result = crnet_curl_http_drive(http, CRNET_CURL_HTTP_CONNECTED, start, desc->timeout_ms);
	if (result == CR_RESULT_NET_TIMED_OUT) {
		http->start_pending = true;
		return result;
	}
	if (CR_FAILED(result))
		return result;
	if (http->finished && http->finish_code != CURLE_OK)
		return crnet_curl_http_failed(http, CR_RESULT_NET_HTTP_FAILED);
#else
	(void)start;
	// Begun: the one pass that costs no waiting.
	int running = 0;
	if (curl_multi_perform(http->client->multi, &running) != CURLM_OK)
		return CR_RESULT_NET_HTTP_FAILED;
#endif
	return CR_SUCCESS;
}

/* A start that ran out of time, carried on to where start would have
 * returned, out of the calling call's limit: CR_SUCCESS once it has got
 * there (or there was nothing to carry on), CR_RESULT_NET_TIMED_OUT while
 * it hasn't, or the start's failure. */
static CR_RESULT crnet_curl_http_carry_start(struct crnet_curl_http *http, uint64_t start, uint32_t timeout_ms) {
	if (!http->start_pending)
		return CR_SUCCESS;
	CR_RESULT result = crnet_curl_http_drive(http, CRNET_CURL_HTTP_CONNECTED, start, timeout_ms);
	if (result == CR_RESULT_NET_TIMED_OUT)
		return result;
	http->start_pending = false;
	if (CR_FAILED(result))
		return result;
	if (http->finished && http->finish_code != CURLE_OK)
		return crnet_curl_http_failed(http, CR_RESULT_NET_HTTP_FAILED);
	return CR_SUCCESS;
}

CR_RESULT crnet_http_backend_send(
    struct crnet_http_transfer_t *transfer,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_sent) {
	struct crnet_curl_http *http  = (struct crnet_curl_http *)transfer->backend;
	uint64_t                  start = crnet_curl_now_ms();

	// The request out first: nothing is taken until it is.
	CR_RESULT result = crnet_curl_http_carry_start(http, start, timeout_ms);
	if (CR_FAILED(result))
		return result;
	if (!http->finished) {
		http->send_data  = (const uint8_t *)data;
		http->send_size  = size;
		http->send_taken = 0;
		if (http->send_paused)
			crnet_curl_http_unpause(http);
		result = crnet_curl_http_drive(http, CRNET_CURL_HTTP_SENT_SOME, start, timeout_ms);
	}
	// Nothing of the caller's stays behind, however this went.
	size_t taken     = http->send_taken;
	http->send_data  = NULL;
	http->send_size  = 0;
	http->send_taken = 0;

	// Bytes libcurl took count, even if the time ran out in the same pass.
	if (taken > 0) {
		*out_sent = taken;
		return CR_SUCCESS;
	}
	if (CR_FAILED(result))
		return result;
	if (http->finish_code != CURLE_OK)
		return crnet_curl_http_failed(http, CR_RESULT_NET_SEND_FAILED);
	// The server answered in full without waiting for the rest of the body
	// - a refusal, usually. There is nowhere for the bytes to go and a
	// response for the caller to read, so they are let through as sent.
	*out_sent = size;
	return CR_SUCCESS;
}

CR_RESULT crnet_http_backend_receive_response(
    struct crnet_http_transfer_t *transfer,
    uint32_t timeout_ms) {
	struct crnet_curl_http *http  = (struct crnet_curl_http *)transfer->backend;
	uint64_t                  start = crnet_curl_now_ms();

	CR_RESULT start_result = crnet_curl_http_carry_start(http, start, timeout_ms);
	if (CR_FAILED(start_result))
		return start_result;

	// Said once: from here the read callback answers "no more".
	if (!http->send_ended) {
		http->send_ended = true;
		if (http->send_paused)
			crnet_curl_http_unpause(http);
	}

	if (!http->headers_done && !http->finished) {
		CR_RESULT result = crnet_curl_http_drive(http, CRNET_CURL_HTTP_HEADERS, start, timeout_ms);
		if (CR_FAILED(result))
			return result;
	}
	if (http->finished && http->finish_code != CURLE_OK)
		return crnet_curl_http_failed(http, CR_RESULT_NET_HTTP_FAILED);

	long status = 0;
	curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status <= 0)
		return CR_RESULT_NET_HTTP_FAILED; // ended cleanly with no response at all
	transfer->response.status = (uint32_t)status;
	return CR_SUCCESS;
}

CR_RESULT crnet_http_backend_recv(
    struct crnet_http_transfer_t *transfer,
    void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_received) {
	struct crnet_curl_http *http  = (struct crnet_curl_http *)transfer->backend;
	uint64_t                  start = crnet_curl_now_ms();

	for (;;) {
		if (http->pending_offset < http->pending_size) {
			size_t left  = http->pending_size - http->pending_offset;
			size_t bytes = left < size ? left : size;
			memcpy(data, http->pending + http->pending_offset, bytes);
			http->pending_offset += bytes;
			*out_received         = bytes;
			return CR_SUCCESS;
		}
		if (http->finished) {
			if (http->finish_code != CURLE_OK)
				return crnet_curl_http_failed(http, CR_RESULT_NET_RECV_FAILED);
			*out_received = 0; // the body's end
			return CR_SUCCESS;
		}

		// The piece libcurl was holding back while the last one was being
		// taken may be handed over by the unpausing itself, so the top of
		// the loop looks before anything waits.
		if (http->recv_paused) {
			crnet_curl_http_unpause(http);
			continue;
		}
		CR_RESULT result = crnet_curl_http_drive(http, CRNET_CURL_HTTP_BODY_PIECE, start, timeout_ms);
		// A piece that arrived in the pass the time ran out in still counts.
		if (CR_FAILED(result) && !(http->pending_offset < http->pending_size))
			return result;
	}
}

// For a wait set (see the end of crnetshared.h). libcurl moves only inside
// a call and never calls back from a thread of its own, so a transfer here
// notifies nothing: it answers from its own state, and hands the set the
// multi handle's descriptors and timer, afresh at every wait, for whatever
// only a call can move. A wait on the set is a call on the transfer
// (crnet.h), so its state is read without a lock.

void crnet_http_backend_wait_attach(struct crnet_http_transfer_t *transfer, struct crnet_wait_set_t *set) {
	(void)transfer;
	(void)set;
}

void crnet_http_backend_wait_detach(struct crnet_http_transfer_t *transfer) { (void)transfer; }

uint32_t crnet_http_backend_wait_prepare(struct crnet_http_transfer_t *transfer, uint32_t wait_for, struct crnet_wait_poll *poll) {
	struct crnet_curl_http *http = (struct crnet_curl_http *)transfer->backend;
	// Ended inside libcurl: whichever call comes next returns at once.
	if (http->finished || http->out_of_memory)
		return wait_for;

	uint32_t ready   = 0;
	bool     waiting = http->start_pending; // only a drive moves a start on
	if (!http->start_pending) {
		switch (transfer->phase) {
		case CRNET_HTTP_PHASE_SENDING:
			// The read callback asked for body and paused: the next send
			// hands it some at once.
			if (http->send_paused)
				ready |= CRNET_WAIT_FOR_ROOM;
			// receive_response comes next once the body is in.
			if (http->headers_done)
				ready |= CRNET_WAIT_FOR_INCOMING;
			break;
		case CRNET_HTTP_PHASE_AWAITING:
			if (http->headers_done)
				ready |= CRNET_WAIT_FOR_INCOMING;
			ready |= CRNET_WAIT_FOR_ROOM; // send returns at once, out of order
			break;
		case CRNET_HTTP_PHASE_RESPONDED:
			// A piece libcurl handed over and the caller hasn't taken all of,
			// or one it is holding back behind it, never shows on a
			// descriptor: libcurl won't read while it is paused (P8).
			if (http->pending_offset < http->pending_size || http->recv_paused)
				ready |= CRNET_WAIT_FOR_INCOMING;
			ready |= CRNET_WAIT_FOR_ROOM;
			break;
		default:
			ready = CRNET_WAIT_FOR_INCOMING | CRNET_WAIT_FOR_ROOM;
			break;
		}
		waiting = (wait_for & ~ready) != 0;
	}
	if (waiting && (ready & wait_for) == 0)
		ready |= crnet_curl_wait_multi(http->client->multi, &transfer->head.allocator, &http->client->wait_fds, wait_for, false, poll);
	return ready;
}

// --------------------------------------------------------------------------
// WebSocket
// --------------------------------------------------------------------------

/* curl_ws_send/curl_ws_recv exist from 7.86, but WebSocket was an
 * experimental, off-by-default build option until 8.11, and what a short
 * curl_ws_send means changed along the way. Older headers get entry points
 * that say so, as does a newer libcurl built without the protocol. */
#if LIBCURL_VERSION_NUM >= 0x080b00

struct crnet_websocket_t {
	cr_object_head head;
	crnet_instance instance;
	// The multi handle drives the opening request, in connect and in each
	// connect_continue; the easy handle stays on it afterwards, for the
	// connection's life, because that is what keeps a connect-only
	// connection alive (C4.2).
	CURLM             *multi;
	CURL              *curl;
	struct curl_slist *headers;
	// Filled in by connect; the handle points into it from then on.
	struct crnet_curl_tls tls;
	// By CRNET_ERROR_SIDE; each is written only by its side's caller.
	// `detail` is where libcurl writes about the opening request, and
	// `send_code` what the last frame that didn't go out failed with -
	// written under the lock, since a receiver sends close frames too.
	crnet_error errors[CRNET_ERROR_SIDE_COUNT];
	char          detail[CURL_ERROR_SIZE];
	CURLcode      send_code;
	// The connection's descriptor, for poll() only; libcurl owns it.
	curl_socket_t descriptor;
	// The multi handle's descriptors, handed to a wait set while the
	// opening is under way.
	struct crnet_curl_wait_fds wait_fds;

	// Set by crnet_websocket_abort and never cleared. `wake` is an
	// eventfd abort writes to, so a poll() on the connection ends too.
	atomic_bool aborted;
	int         wake;

	// A libcurl handle takes one thread at a time, and a receiver and a
	// sender are allowed to overlap. Held around each libcurl call and
	// never across a poll().
	pthread_mutex_t lock;

	// connect was called; its outcome has been returned (by connect or a
	// continue), which `open` says was a connection.
	bool     connect_called;
	bool     outcome_taken;
	bool     open;
	// The opening has ended inside libcurl - seen by a drive in connect, a
	// continue or a wait set's prepare - and how; the next connect or
	// continue returns it.
	bool     opening_done;
	CURLcode opening_code;
	bool     in_wait_set;
	// Bytes libcurl or the TLS layer may already have read off the
	// descriptor, which never shows on it again: set when the socket goes
	// in a set and after every recv that handed something over, cleared by
	// a recv that libcurl told to come back later. While it is set a set
	// names the socket INCOMING without a poll (what `report_once` did).
	bool     maybe_buffered;
	uint64_t max_message_size;
	uint64_t message_size;

	// The message in flight (crnet.h, crnet_websocket_send()): what
	// curl_ws_send hasn't consumed of it, in the socket's own buffer,
	// offered again from `send_offset` until it has all gone (C4.1). The
	// buffer is grown to exactly a message that doesn't fit, before the
	// first byte of it is offered, and kept until destroy (T13).
	uint8_t     *send_buffer;
	size_t       send_capacity;
	size_t       send_size;
	size_t       send_offset;
	unsigned int send_flags;
	bool         send_in_flight;

	// Nothing more will arrive, and why.
	bool     finished;
	bool     too_big;
	bool     peer_closed;
	uint16_t close_code;
	bool     close_requested;
};

static bool crnet_curl_has_websocket(void) {
	const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
	for (const char *const *protocol = info->protocols; protocol && *protocol; ++protocol) {
		if (strcmp(*protocol, "ws") == 0)
			return true;
	}
	return false;
}

enum crnet_curl_waited {
	CRNET_CURL_READY,
	CRNET_CURL_WAIT_TIMED_OUT,
	CRNET_CURL_WAIT_ABORTED,
};

/* Waits for the connection to become readable or writable, for the time
 * to run out, or for an abort. Never called with the lock held. */
static enum crnet_curl_waited crnet_curl_wait(struct crnet_websocket_t *socket, short events, uint64_t start, uint32_t timeout_ms) {
	for (;;) {
		if (atomic_load(&socket->aborted))
			return CRNET_CURL_WAIT_ABORTED;

		struct pollfd waited[2];
		waited[0].fd      = socket->descriptor;
		waited[0].events  = events;
		waited[0].revents = 0;
		waited[1].fd      = socket->wake;
		waited[1].events  = POLLIN;
		waited[1].revents = 0;
		int ready         = poll(waited, 2, crnet_curl_remaining(start, timeout_ms));
		if (ready == 0)
			return CRNET_CURL_WAIT_TIMED_OUT;
		if (ready < 0 && errno == EINTR)
			continue;
		if (atomic_load(&socket->aborted))
			return CRNET_CURL_WAIT_ABORTED;
		// An error or hang-up counts as ready: the next libcurl call
		// reports it.
		return CRNET_CURL_READY;
	}
}

enum crnet_curl_sent {
	CRNET_CURL_SENT,
	CRNET_CURL_SEND_FAILED,
	CRNET_CURL_SEND_TIMED_OUT,
	CRNET_CURL_SEND_ABORTED,
};

/* Sends one control frame whole, now or not at all: a close frame, never
 * waited for. Called with the lock held, with no message in flight (libcurl
 * refuses a new frame while one is unfinished, P5). Anything but SENT
 * leaves a frame that may be part-written, and marks the socket finished. */
static enum crnet_curl_sent crnet_curl_send_control(struct crnet_websocket_t *socket, const void *data, size_t size, unsigned int flags) {
	size_t offset = 0;
	for (;;) {
		size_t   sent = 0;
		CURLcode code = curl_ws_send(socket->curl, (const char *)data + offset, size - offset, &sent, 0, flags);
		offset       += sent;
		if (code == CURLE_OK && offset >= size)
			return CRNET_CURL_SENT;
		if (code == CURLE_OK)
			continue; // a short send: the rest at once, while there is room
		socket->finished = true;
		if (code == CURLE_AGAIN)
			return CRNET_CURL_SEND_TIMED_OUT;
		socket->send_code = code;
		return CRNET_CURL_SEND_FAILED;
	}
}

/* The message in flight failed on its way: the sending side's record, and
 * the end of the connection (T7). Called with the lock held. */
static CR_RESULT crnet_curl_message_failed(struct crnet_websocket_t *socket, CURLcode code) {
	socket->send_in_flight = false;
	socket->finished       = true;
	socket->send_code      = code;
	crnet_curl_failed(&socket->errors[CRNET_ERROR_SIDE_SENDING], code, CR_RESULT_NET_SEND_FAILED, NULL);
	socket->errors[CRNET_ERROR_SIDE_SENDING].result = CR_RESULT_NET_SEND_FAILED;
	return CR_RESULT_NET_SEND_FAILED;
}

/* What a sending call on a usable socket returns before anything else:
 * CR_RESULT_NET_SEND_FAILED for a connection that has ended (a message that
 * failed on its way among the reasons, its record kept from then), and
 * CR_SUCCESS where it can go on. Called with the lock held. */
static CR_RESULT crnet_curl_websocket_sending(const struct crnet_websocket_t *socket) {
	return socket->finished ? CR_RESULT_NET_SEND_FAILED : CR_SUCCESS;
}

/* Carries the message in flight on, offering libcurl what it hasn't
 * consumed - "buffer and buflen updated on every following invocation to
 * only point to the remaining piece", and after CURLE_AGAIN "retry with
 * the same data and length" (libcurl's page) - and waiting for room up to
 * `timeout_ms` since `start` (CRNET_NO_WAIT: whatever fits right now).
 * CR_SUCCESS once none is in flight. Called with the lock held; lets go of
 * it only while waiting. */
static CR_RESULT crnet_curl_carry_on(struct crnet_websocket_t *socket, uint64_t start, uint32_t timeout_ms) {
	while (socket->send_in_flight) {
		if (atomic_load(&socket->aborted))
			return CR_RESULT_NET_ABORTED;
		// libcurl 8.18 (lib/ws.c): CURLE_AGAIN only with nothing consumed;
		// a part consumed is CURLE_OK with its count.
		size_t   sent = 0;
		CURLcode code = curl_ws_send(socket->curl, (const char *)socket->send_buffer + socket->send_offset, socket->send_size - socket->send_offset, &sent, 0, socket->send_flags);
		socket->send_offset += sent;
		if (code == CURLE_OK && socket->send_offset >= socket->send_size) {
			socket->send_in_flight = false;
			break;
		}
		if (code == CURLE_OK)
			continue; // a short send: the rest at once, while there is room
		if (code != CURLE_AGAIN)
			return crnet_curl_message_failed(socket, code);
		if (crnet_curl_remaining(start, timeout_ms) == 0)
			return CR_RESULT_NET_TIMED_OUT;
		pthread_mutex_unlock(&socket->lock);
		enum crnet_curl_waited waited = crnet_curl_wait(socket, POLLOUT, start, timeout_ms);
		pthread_mutex_lock(&socket->lock);
		if (waited == CRNET_CURL_WAIT_ABORTED)
			return CR_RESULT_NET_ABORTED;
		// A receiver may have moved it on, or ended the connection, while
		// the lock was let go.
		if (socket->finished && socket->send_in_flight)
			return CR_RESULT_NET_SEND_FAILED;
		if (waited == CRNET_CURL_WAIT_TIMED_OUT && socket->send_in_flight)
			return CR_RESULT_NET_TIMED_OUT;
	}
	return CR_SUCCESS;
}

void crnet_websocket_destroy(crnet_websocket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_websocket_destroy", "socket"))
		return;
	if (socket->multi && socket->curl)
		curl_multi_remove_handle(socket->multi, socket->curl); // harmless when it isn't on it
	if (socket->curl)
		curl_easy_cleanup(socket->curl);
	if (socket->multi)
		curl_multi_cleanup(socket->multi);
	curl_slist_free_all(socket->headers);
	CR_KILL(socket);
	const CR_ALLOCATOR allocator = socket->head.allocator;
	crnet_curl_tls_free(&allocator, &socket->tls);
	crnet_curl_wait_fds_free(&allocator, &socket->wait_fds);
	cr_free(&allocator, socket->send_buffer, socket->send_capacity, 1);
	if (socket->wake >= 0)
		close(socket->wake);
	pthread_mutex_destroy(&socket->lock);
	cr_string_free(&allocator, socket->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(socket), &allocator, struct crnet_websocket_t, socket);
}

crnet_instance crnet_websocket_instance(crnet_websocket socket) { return socket ? socket->instance : NULL; }

CR_RESULT crnet_websocket_create(
    crnet_instance instance,
    crnet_websocket *out_socket) {
	if (!out_socket)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_websocket_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!crnet_curl_has_websocket())
		return CR_RESULT_NET_UNSUPPORTED;
	// After the argument checks, before anything is made (Q9): nothing to
	// record, since nothing of the platform's failed.
	if (!crnet_curl_resolves_asynchronously())
		return CR_RESULT_NET_UNSUPPORTED;

	struct crnet_websocket_t *socket = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_websocket_t);
	if (!socket)
		return CR_RESULT_OUT_OF_MEMORY;
	socket->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(socket, &instance->quarantine);
	socket->instance       = instance;
	pthread_mutex_init(&socket->lock, NULL);
	atomic_init(&socket->aborted, false);
	socket->descriptor = CURL_SOCKET_BAD;
	socket->wake       = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	socket->multi      = curl_multi_init();
	socket->curl       = curl_easy_init();
	if (socket->wake < 0 || !socket->multi || !socket->curl) {
		crnet_websocket_destroy(socket);
		return CR_RESULT_GENERAL_FAILURE;
	}

	*out_socket = socket;
	return CR_SUCCESS;
}

void crnet_websocket_get_error(
    crnet_websocket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error) {
	if (!socket) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(socket, "crnet_websocket_get_error", "socket")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	bool known = side == CRNET_ERROR_SIDE_RECEIVING || side == CRNET_ERROR_SIDE_SENDING;
	crnet_error_get(known ? &socket->errors[side] : NULL, out_error);
}

/* Drives the opening under way, unless it has already ended, until it
 * ends, `timeout_ms` since `start` has passed or an abort; an end is kept
 * on the socket for connect or a continue to return. A call on the whole
 * socket. */
static enum crnet_curl_outcome crnet_curl_websocket_step(struct crnet_websocket_t *socket, uint64_t start, uint32_t timeout_ms) {
	if (socket->opening_done)
		return CRNET_CURL_FINISHED;
	CURLcode                code    = CURLE_OK;
	enum crnet_curl_outcome outcome = crnet_curl_drive(socket->multi, socket->curl, start, timeout_ms, &socket->aborted, &code);
	if (outcome == CRNET_CURL_FINISHED) {
		socket->opening_done = true;
		socket->opening_code = code;
	}
	return outcome;
}

// For a wait set (see the end of crnetshared.h). Nothing here calls back,
// so nothing notifies: while the opening is under way the socket hands
// the set its multi handle's descriptors and timer (a wait is then a call
// on the whole socket, C7.1, as connect_continue is); once it is open, the
// connection's own descriptor, for what was asked and isn't ready already.

CR_RESULT crnet_websocket_wait_attach(crnet_websocket socket, struct crnet_wait_set_t *set) {
	(void)set;
	pthread_mutex_lock(&socket->lock);
	CR_RESULT result = CR_SUCCESS;
	// Connected or connecting; never connected, failed or aborted is
	// refused.
	if (atomic_load(&socket->aborted) || !socket->connect_called || (socket->outcome_taken && !socket->open))
		result = CR_RESULT_NET_INVALID_WEBSOCKET;
	else if (socket->in_wait_set)
		result = CR_RESULT_NET_ALREADY_IN_WAIT_SET;
	else {
		socket->in_wait_set    = true;
		socket->maybe_buffered = true;
	}
	pthread_mutex_unlock(&socket->lock);
	return result;
}

void crnet_websocket_wait_detach(crnet_websocket socket) {
	pthread_mutex_lock(&socket->lock);
	socket->in_wait_set = false;
	pthread_mutex_unlock(&socket->lock);
}

uint32_t crnet_websocket_wait_prepare(crnet_websocket socket, uint32_t wait_for, struct crnet_wait_poll *poll) {
	pthread_mutex_lock(&socket->lock);
	uint32_t ready = 0;
	if (atomic_load(&socket->aborted) || (socket->outcome_taken && (!socket->open || socket->finished))) {
		// Every call returns at once.
		ready = wait_for;
	} else if (!socket->outcome_taken) {
		// Opening: a wait is a call on the whole socket (C7.1), so it is
		// carried on here, one pass, and the socket named for whatever was
		// asked once it has an outcome, as crnet.h promises; until then
		// libcurl's descriptors and timer only have the set ask again.
		if (!socket->opening_done)
			crnet_curl_websocket_step(socket, 0, CRNET_NO_WAIT);
		if (socket->opening_done || atomic_load(&socket->aborted))
			ready = wait_for;
		else
			ready = crnet_curl_wait_multi(socket->multi, &socket->head.allocator, &socket->wait_fds, wait_for, true, poll);
	} else {
		bool incoming = false;
		bool room     = false;
		if (wait_for & CRNET_WAIT_FOR_INCOMING) {
			if (socket->maybe_buffered)
				ready |= CRNET_WAIT_FOR_INCOMING;
			else
				incoming = true;
		}
		if (wait_for & CRNET_WAIT_FOR_ROOM) {
			// Room is a message in flight that can move on (P4): only a call
			// moves it, and the descriptor's room is when one will.
			if (!socket->send_in_flight)
				ready |= CRNET_WAIT_FOR_ROOM;
			else
				room = true;
		}
		if (incoming || room)
			crnet_wait_poll_add(poll, (intptr_t)socket->descriptor, incoming, room, false);
	}
	pthread_mutex_unlock(&socket->lock);
	return ready;
}

void crnet_websocket_abort(crnet_websocket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_websocket_abort", "socket"))
		return;
	atomic_store(&socket->aborted, true);
	// One for a wait on the open connection, one for the opening request.
	uint64_t one = 1;
	if (write(socket->wake, &one, sizeof(one)) < 0) {
		// Full means it is already readable, which is all this is for.
	}
	curl_multi_wakeup(socket->multi);
}

/* Drives the opening under way until it has an outcome or `timeout_ms`
 * since `start` has passed, and returns what connect returns: an outcome
 * once (marked taken), or CR_RESULT_NET_TIMED_OUT with the opening left
 * on the multi handle for connect_continue. A call on the whole socket. An
 * opening a wait set's prepare saw end is returned without a drive. */
static CR_RESULT crnet_curl_websocket_open(struct crnet_websocket_t *socket, uint64_t start, uint32_t timeout_ms, uint32_t *out_http_status) {
	enum crnet_curl_outcome outcome = crnet_curl_websocket_step(socket, start, timeout_ms);
	if (outcome == CRNET_CURL_STOPPED || atomic_load(&socket->aborted))
		return CR_RESULT_NET_ABORTED;
	if (outcome == CRNET_CURL_TIMED_OUT)
		return CR_RESULT_NET_TIMED_OUT; // the opening goes on; status stays 0
	CURLcode code = socket->opening_code;

	long status = 0;
	curl_easy_getinfo(socket->curl, CURLINFO_RESPONSE_CODE, &status);
	if (out_http_status)
		*out_http_status = (uint32_t)status;

	CR_RESULT result = CR_SUCCESS;
	if (code == CURLE_OK)
		code = curl_easy_getinfo(socket->curl, CURLINFO_ACTIVESOCKET, &socket->descriptor);
	if (code == CURLE_OK && socket->descriptor != CURL_SOCKET_BAD)
		result = CR_SUCCESS;
	else if (status != 0 && status != 101)
		result = CR_RESULT_NET_WS_UPGRADE_REFUSED;
	else if (code != CURLE_OK)
		result = crnet_curl_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], code, CR_RESULT_NET_CONNECT_FAILED, socket->detail);
	else
		result = CR_RESULT_NET_CONNECT_FAILED; // libcurl was content and still had no connection to hand over

	pthread_mutex_lock(&socket->lock);
	socket->outcome_taken = true;
	if (CR_FAILED(result))
		socket->finished = true; // this socket is spent
	else
		socket->open = true;
	pthread_mutex_unlock(&socket->lock);
	return result;
}

CR_RESULT crnet_websocket_connect(
    crnet_websocket socket,
    const crnet_websocket_connect_desc *desc,
    uint32_t *out_http_status) {
	if (out_http_status)
		*out_http_status = 0;
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_connect", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->url || desc->max_message_size == 0 || (desc->header_count > 0 && !desc->headers))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!crnet_url_has_scheme(desc->url, "ws") && !crnet_url_has_scheme(desc->url, "wss"))
		return CR_RESULT_NET_INVALID_URL;

	const CR_ALLOCATOR *allocator = &socket->head.allocator;
	uint64_t              start     = crnet_curl_now_ms();
	struct curl_slist    *headers   = NULL;
	CR_RESULT headers_result = crnet_curl_headers(allocator, desc->headers, desc->header_count, &headers);
	if (CR_FAILED(headers_result))
		return headers_result;

	struct crnet_curl_tls tls;
	CR_RESULT              result = crnet_curl_tls_create(allocator, &desc->tls, &tls);
	if (CR_FAILED(result)) {
		curl_slist_free_all(headers);
		return result;
	}
	// Only needed until it has been set on the handle, which copies it.
	struct crnet_proxy proxy;
	result = crnet_proxy_parse(allocator, &desc->proxy, &proxy);
	if (CR_FAILED(result)) {
		crnet_proxy_free(allocator, &proxy);
		crnet_curl_tls_free(allocator, &tls);
		curl_slist_free_all(headers);
		return result;
	}

	pthread_mutex_lock(&socket->lock);
	if (atomic_load(&socket->aborted))
		result = CR_RESULT_NET_ABORTED;
	else if (socket->connect_called)
		result = CR_RESULT_NET_INVALID_WEBSOCKET;
	else
		socket->connect_called = true; // begun whatever the limit, CRNET_NO_WAIT included
	pthread_mutex_unlock(&socket->lock);
	if (CR_FAILED(result)) {
		curl_slist_free_all(headers);
		crnet_curl_tls_free(allocator, &tls);
		crnet_proxy_free(allocator, &proxy);
		return result;
	}
	socket->headers          = headers;
	socket->tls              = tls; // the socket's from here
	socket->max_message_size = desc->max_message_size;

	CURL *curl = socket->curl;
	crnet_curl_common_options(curl);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, socket->detail);
	curl_easy_setopt(curl, CURLOPT_URL, desc->url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, socket->headers);
	// Said either way, so that it is the desc and not libcurl's default
	// that decides what becomes of a redirect.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, desc->follow_redirects ? 1L : 0L);
	crnet_curl_proxy_apply(curl, &proxy);
	crnet_proxy_free(allocator, &proxy);
	// 2: do the opening request, then hand the connection over for
	// curl_ws_send/curl_ws_recv instead of running a transfer on it.
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);

	// What the TLS library lacks is found before the opening is begun, so
	// never by a continue (crnet.h).
	result = crnet_curl_tls_apply(curl, &socket->tls);
	if (!CR_FAILED(result) && curl_multi_add_handle(socket->multi, curl) != CURLM_OK)
		result = CR_RESULT_GENERAL_FAILURE;
	if (CR_FAILED(result)) {
		pthread_mutex_lock(&socket->lock);
		socket->outcome_taken = true;
		socket->finished      = true;
		pthread_mutex_unlock(&socket->lock);
		return result;
	}
	return crnet_curl_websocket_open(socket, start, desc->timeout_ms, out_http_status);
}

CR_RESULT crnet_websocket_connect_continue(
    crnet_websocket socket,
    uint32_t timeout_ms,
    uint32_t *out_http_status) {
	if (out_http_status)
		*out_http_status = 0;
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_connect_continue", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;

	uint64_t start = crnet_curl_now_ms();
	pthread_mutex_lock(&socket->lock);
	CR_RESULT result = CR_SUCCESS;
	if (atomic_load(&socket->aborted))
		result = CR_RESULT_NET_ABORTED;
	else if (!socket->connect_called || socket->outcome_taken)
		result = CR_RESULT_NET_INVALID_WEBSOCKET;
	pthread_mutex_unlock(&socket->lock);
	if (CR_FAILED(result))
		return result;
	return crnet_curl_websocket_open(socket, start, timeout_ms, out_http_status);
}

/* What a call on a socket that can't carry it returns, or CR_SUCCESS for
 * one that can. Called with the lock held. */
static CR_RESULT crnet_curl_websocket_usable(struct crnet_websocket_t *socket) {
	if (atomic_load(&socket->aborted))
		return CR_RESULT_NET_ABORTED;
	if (!socket->open)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	return CR_SUCCESS;
}

CR_RESULT crnet_websocket_send(
    crnet_websocket socket,
    CRNET_WEBSOCKET_MESSAGE type,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms) {
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_send", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if ((size > 0 && !data) || size > SIZE_MAX)
		return CR_RESULT_NET_SEND_FAILED;

	uint64_t     start  = crnet_curl_now_ms();
	const void  *bytes  = size > 0 ? data : "";
	unsigned int flags  = type == CRNET_WEBSOCKET_MESSAGE_TEXT ? CURLWS_TEXT : CURLWS_BINARY;
	pthread_mutex_lock(&socket->lock);
	CR_RESULT result = crnet_curl_websocket_usable(socket);
	if (!CR_FAILED(result))
		result = crnet_curl_websocket_sending(socket);
	// libcurl frames whatever it is given as text. Refused before anything
	// is taken, and before the message in flight is waited for (T12).
	if (!CR_FAILED(result) && type == CRNET_WEBSOCKET_MESSAGE_TEXT && !crnet_utf8_valid((const uint8_t *)bytes, (size_t)size))
		result = CR_RESULT_NET_SEND_FAILED;
	// The message before first, out of this call's limit.
	if (!CR_FAILED(result))
		result = crnet_curl_carry_on(socket, start, timeout_ms);
	// Room for the whole of this one before its first byte is offered, so
	// a refusal leaves nothing on the wire (C6.4): exactly its size, the
	// old block freed first - nothing is in flight to need it (C6.3).
	if (!CR_FAILED(result) && size > socket->send_capacity) {
		cr_free(&socket->head.allocator, socket->send_buffer, socket->send_capacity, 1);
		socket->send_buffer   = (uint8_t *)cr_allocate(&socket->head.allocator, (size_t)size, 1);
		socket->send_capacity = socket->send_buffer ? (size_t)size : 0;
		if (!socket->send_buffer)
			result = CR_RESULT_OUT_OF_MEMORY; // nothing recorded (C5.2)
	}
	if (!CR_FAILED(result)) {
		// The caller's own bytes first; only what libcurl didn't consume is
		// copied, to be offered again (C4.1).
		size_t   sent = 0;
		CURLcode code = curl_ws_send(socket->curl, (const char *)bytes, (size_t)size, &sent, 0, flags);
		if (code == CURLE_OK || code == CURLE_AGAIN) {
			if (sent < (size_t)size) {
				memcpy(socket->send_buffer, (const uint8_t *)bytes + sent, (size_t)size - sent);
				socket->send_size      = (size_t)size - sent;
				socket->send_offset    = 0;
				socket->send_flags     = flags;
				socket->send_in_flight = true;
			}
			// Taken, whole: what didn't go is carried on by the next send,
			// flush or recv's close frame.
		} else
			result = crnet_curl_message_failed(socket, code);
	}
	pthread_mutex_unlock(&socket->lock);
	return result;
}

CR_RESULT crnet_websocket_flush(
    crnet_websocket socket,
    uint32_t timeout_ms) {
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_flush", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;

	uint64_t start = crnet_curl_now_ms();
	pthread_mutex_lock(&socket->lock);
	CR_RESULT result = crnet_curl_websocket_usable(socket);
	if (!CR_FAILED(result))
		result = crnet_curl_websocket_sending(socket);
	if (!CR_FAILED(result))
		result = crnet_curl_carry_on(socket, start, timeout_ms);
	pthread_mutex_unlock(&socket->lock);
	return result;
}

/* Why a finished socket finished. Called with the lock held. */
static CR_RESULT crnet_curl_websocket_end_result(const struct crnet_websocket_t *socket) {
	if (socket->too_big)
		return CR_RESULT_NET_WS_MESSAGE_TOO_BIG;
	if (socket->peer_closed || socket->close_requested)
		return CR_RESULT_NET_WS_CLOSED;
	return CR_RESULT_NET_RECV_FAILED;
}

/* A close frame of recv's own (the echo of the peer's, or the one for a
 * message over the limit): after the message in flight, never into it. Its
 * rest goes first if that needs no waiting; otherwise the frame is left
 * unsent - the connection is ending either way, and recv's results don't
 * change (P5). Called with the lock held. */
static void crnet_curl_recv_close_frame(struct crnet_websocket_t *socket, const void *payload, size_t size) {
	if (socket->send_in_flight && CR_FAILED(crnet_curl_carry_on(socket, 0, CRNET_NO_WAIT)))
		return;
	crnet_curl_send_control(socket, payload, size, CURLWS_CLOSE);
}

CR_RESULT crnet_websocket_recv(
    crnet_websocket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received,
    CRNET_WEBSOCKET_MESSAGE *out_type,
    bool *out_message_complete) {
	if (!out_received || !out_type || !out_message_complete)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_received         = 0;
	*out_type             = CRNET_WEBSOCKET_MESSAGE_BINARY;
	*out_message_complete = false;
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_recv", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;

	uint64_t   start  = crnet_curl_now_ms();
	size_t     room   = size > SIZE_MAX ? SIZE_MAX : (size_t)size;
	CR_RESULT result = CR_SUCCESS;

	pthread_mutex_lock(&socket->lock);
	for (;;) {
		result = crnet_curl_websocket_usable(socket);
		if (CR_FAILED(result))
			break;
		if (socket->finished) {
			result = crnet_curl_websocket_end_result(socket);
			break;
		}

		// Asked first, polled second: bytes libcurl or the TLS layer has
		// already read never make the descriptor readable again. With
		// CRNET_NO_WAIT the poll returns at once and this only looks.
		size_t                      count = 0;
		const struct curl_ws_frame *frame = NULL;
		CURLcode                    code  = curl_ws_recv(socket->curl, data, room, &count, &frame);

		if (code == CURLE_AGAIN) {
			// Nothing read is left over: the descriptor says when more is.
			socket->maybe_buffered = false;
			pthread_mutex_unlock(&socket->lock);
			enum crnet_curl_waited waited = crnet_curl_wait(socket, POLLIN, start, timeout_ms);
			pthread_mutex_lock(&socket->lock);
			if (waited == CRNET_CURL_WAIT_TIMED_OUT) {
				result = CR_RESULT_NET_TIMED_OUT;
				break;
			}
			continue; // ready, or aborted - which the top of the loop reports
		}
		if (code != CURLE_OK || !frame) {
			// Kept now, while the code is at hand; the top of the loop
			// reports the ending.
			if (code != CURLE_OK) {
				crnet_curl_failed(&socket->errors[CRNET_ERROR_SIDE_RECEIVING], code, CR_RESULT_NET_RECV_FAILED, NULL);
				socket->errors[CRNET_ERROR_SIDE_RECEIVING].result = CR_RESULT_NET_RECV_FAILED;
			}
			socket->finished = true;
			continue;
		}

		if (frame->flags & CURLWS_CLOSE) {
			// The payload is the code, big-endian, then the reason.
			const uint8_t *payload = (const uint8_t *)data;
			if (frame->offset == 0 && count >= 2)
				socket->close_code = (uint16_t)((payload[0] << 8) | payload[1]);
			// A close this side didn't start is answered in kind, which
			// the other stacks do unasked. Not waited for.
			if (!socket->close_requested) {
				uint8_t echo[2] = {(uint8_t)(socket->close_code >> 8), (uint8_t)(socket->close_code & 0xFF)};
				crnet_curl_recv_close_frame(socket, echo, socket->close_code ? 2 : 0);
			}
			socket->peer_closed = true;
			socket->finished    = true;
			continue;
		}
		// libcurl answers a ping itself, and both control frames still
		// come through here; neither is the caller's to see.
		if (frame->flags & (CURLWS_PING | CURLWS_PONG))
			continue;

		if ((uint64_t)count > socket->max_message_size - socket->message_size) {
			// Told why, like the stacks that enforce the limit themselves.
			// Not waited for.
			uint8_t too_big[2] = {0x03, 0xF1}; // 1009
			crnet_curl_recv_close_frame(socket, too_big, sizeof(too_big));
			socket->too_big  = true;
			socket->finished = true;
			continue;
		}
		socket->message_size  += count;
		socket->maybe_buffered = true;

		*out_received = count;
		*out_type     = (frame->flags & CURLWS_TEXT) ? CRNET_WEBSOCKET_MESSAGE_TEXT : CRNET_WEBSOCKET_MESSAGE_BINARY;
		// More of this frame is waiting, or the frame isn't the message's last.
		if (frame->bytesleft == 0 && !(frame->flags & CURLWS_CONT)) {
			*out_message_complete = true;
			socket->message_size  = 0;
		}
		break;
	}
	pthread_mutex_unlock(&socket->lock);
	return result;
}

CR_RESULT crnet_websocket_close(
    crnet_websocket socket,
    uint16_t code,
    const char *reason) {
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_close", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	size_t reason_length = reason ? strlen(reason) : 0;
	if (reason_length > 123)
		return CR_RESULT_NET_SEND_FAILED;

	uint8_t payload[2 + 123];
	payload[0] = (uint8_t)(code >> 8);
	payload[1] = (uint8_t)(code & 0xFF);
	if (reason_length > 0)
		memcpy(payload + 2, reason, reason_length);

	pthread_mutex_lock(&socket->lock);
	CR_RESULT result = crnet_curl_websocket_usable(socket);
	// A message that failed, or an ended connection: no connection to send
	// a frame on.
	if (!CR_FAILED(result))
		result = crnet_curl_websocket_sending(socket);
	// A close frame goes after the message in flight, never into it, and
	// close takes no limit to wait for it with (T5).
	if (!CR_FAILED(result) && socket->send_in_flight)
		result = CR_RESULT_NET_TIMED_OUT;
	if (!CR_FAILED(result)) {
		// Not waited for: a close frame that won't go out now is a
		// connection that isn't closing politely anyway.
		switch (crnet_curl_send_control(socket, payload, 2 + reason_length, CURLWS_CLOSE)) {
		case CRNET_CURL_SENT: {
			// The end of the connection for this side. libcurl would go on
			// receiving until the peer answers; crnet.h has every stack
			// stop here, as the one that can't go on does. A receive waiting
			// in poll() is woken through the abort's eventfd, which never
			// needs to be quiet again on a socket that has finished.
			socket->close_requested = true;
			socket->finished        = true;
			uint64_t one            = 1;
			if (write(socket->wake, &one, sizeof(one)) < 0) {
				// Full means it is already readable, which is all this is for.
			}
			break;
		}
		case CRNET_CURL_SEND_FAILED:
			crnet_curl_failed(&socket->errors[CRNET_ERROR_SIDE_SENDING], socket->send_code, CR_RESULT_NET_SEND_FAILED, NULL);
			socket->errors[CRNET_ERROR_SIDE_SENDING].result = CR_RESULT_NET_SEND_FAILED;
			result                                            = CR_RESULT_NET_SEND_FAILED;
			break;
		default:
			// No room for the frame right now: libcurl has no complaint.
			result = CR_RESULT_NET_SEND_FAILED;
			break;
		}
	}
	pthread_mutex_unlock(&socket->lock);
	return result;
}

uint16_t crnet_websocket_get_close_code(crnet_websocket socket) {
	if (!socket)
		return 0;
	if (CR_STALE(socket, "crnet_websocket_get_close_code", "socket"))
		return 0;
	pthread_mutex_lock(&socket->lock);
	// Only a close the peer made has a code to report.
	uint16_t code = (socket->peer_closed && !socket->close_requested) ? socket->close_code : 0;
	pthread_mutex_unlock(&socket->lock);
	return code;
}

#else // libcurl older than 8.11

CR_RESULT crnet_websocket_create(
    crnet_instance instance,
    crnet_websocket *out_socket) {
	if (!out_socket)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_websocket_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	return CR_RESULT_NET_UNSUPPORTED;
}

crnet_instance crnet_websocket_instance(crnet_websocket socket) {
	(void)socket;
	return NULL;
}

// With no way to make a socket, the rest can only be handed NULL.

CR_RESULT crnet_websocket_connect(
    crnet_websocket socket,
    const crnet_websocket_connect_desc *desc,
    uint32_t *out_http_status) {
	(void)socket;
	(void)desc;
	if (out_http_status)
		*out_http_status = 0;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

CR_RESULT crnet_websocket_connect_continue(
    crnet_websocket socket,
    uint32_t timeout_ms,
    uint32_t *out_http_status) {
	(void)socket;
	(void)timeout_ms;
	if (out_http_status)
		*out_http_status = 0;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

CR_RESULT crnet_websocket_send(
    crnet_websocket socket,
    CRNET_WEBSOCKET_MESSAGE type,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms) {
	(void)socket;
	(void)type;
	(void)data;
	(void)size;
	(void)timeout_ms;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

CR_RESULT crnet_websocket_flush(
    crnet_websocket socket,
    uint32_t timeout_ms) {
	(void)socket;
	(void)timeout_ms;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

CR_RESULT crnet_websocket_recv(
    crnet_websocket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received,
    CRNET_WEBSOCKET_MESSAGE *out_type,
    bool *out_message_complete) {
	(void)socket;
	(void)data;
	(void)size;
	(void)timeout_ms;
	if (out_received)
		*out_received = 0;
	if (out_type)
		*out_type = CRNET_WEBSOCKET_MESSAGE_BINARY;
	if (out_message_complete)
		*out_message_complete = false;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

CR_RESULT crnet_websocket_close(
    crnet_websocket socket,
    uint16_t code,
    const char *reason) {
	(void)socket;
	(void)code;
	(void)reason;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

uint16_t crnet_websocket_get_close_code(crnet_websocket socket) {
	(void)socket;
	return 0;
}

void crnet_websocket_abort(crnet_websocket socket) { (void)socket; }

void crnet_websocket_get_error(
    crnet_websocket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error) {
	(void)socket;
	(void)side;
	crnet_error_get(NULL, out_error);
}

// A wait set can't be handed a socket that can't be made either.

CR_RESULT crnet_websocket_wait_attach(crnet_websocket socket, struct crnet_wait_set_t *set) {
	(void)socket;
	(void)set;
	return CR_RESULT_NET_INVALID_WEBSOCKET;
}

void crnet_websocket_wait_detach(crnet_websocket socket) { (void)socket; }

uint32_t crnet_websocket_wait_prepare(crnet_websocket socket, uint32_t wait_for, struct crnet_wait_poll *poll) {
	(void)socket;
	(void)poll;
	return wait_for;
}

void crnet_websocket_destroy(crnet_websocket socket) { (void)socket; }

#endif // LIBCURL_VERSION_NUM

#if __cplusplus
}
#endif

#endif // CARROTTUS_PLATFORM_LINUX
