
#if SPUDLIB_PLATFORM_LINUX

/*
 * SpudNet HTTP and WebSocket on libcurl.
 *
 * Linux has no client stack of its own; libcurl is the one every
 * distribution ships and keeps patched, with the system's TLS library and
 * trust store behind it.
 *
 * Every transfer - an HTTP transfer, a WebSocket's opening request - is
 * driven here through libcurl's multi interface rather than
 * curl_easy_perform, because that is what lets this file keep the time
 * limit itself (the whole call, to the millisecond, as spudnet.h promises)
 * and lets another thread end the wait at once: curl_multi_wakeup is the
 * one libcurl call that may come from a thread other than the handle's.
 * libcurl's own timers are not used.
 *
 * An open WebSocket is libcurl's connect-only mode: libcurl does the
 * framing, and this file waits on the connection's own descriptor with
 * poll(), beside an eventfd that abort writes to.
 *
 * TLS: a spudnet_tls_desc becomes libcurl options, which libcurl hands to
 * whichever TLS library it was built with. Roots go in as one PEM blob and
 * pins as libcurl's own "sha256//" list, both made once per client or
 * socket. An option that TLS library doesn't have is reported, never
 * skipped.
 *
 * SpudNet's instance for Linux is here too: libcurl is the one thing on
 * this platform that has anything to start.
 */

#include "../../spudnetshared.h"
#include "spudnet.h"

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
#include <time.h>
#include <unistd.h>
#include "../../../spudstructbase.h"

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* spudnet.h requires libcurl 7.84 or later, and CMakeLists.txt asks for it;
 * this is for a build that got here some other way. */
#if LIBCURL_VERSION_NUM < 0x075400
#error "SpudNet: libcurl 7.84 or later is required (a spudnet_instance needs a thread-safe curl_global_init)."
#endif

/* The one piece of state SpudNet has anywhere, and why.
 *
 * An instance may be made on any thread while others run, so libcurl's
 * global init has to be safe to make that way. It is from 7.84 on, in a
 * libcurl built thread-safe, and spudnet_instance_create turns away one
 * that isn't. But the question itself - curl_version_info - is only safe
 * to ask before the first global init when that init is thread-safe, which
 * is the thing being asked. On a libcurl that would fail the check, two
 * instances made at once could therefore collide in the asking. This lock
 * puts SpudNet's own calls in a line, so the check is sound whichever
 * libcurl is there. On one that passes it, the lock guards nothing that
 * needed it and costs an uncontended lock per instance. */
static pthread_mutex_t spudnet_curl_start_lock = PTHREAD_MUTEX_INITIALIZER;

/* BSD sockets (../posix/spudnetsockets.c) have nothing to start; libcurl
 * has its global init, which it counts: it stays up until as many
 * curl_global_cleanup calls have been made. */
SPUDRESULT spudnet_instance_create(
    const spudnet_instance_desc *desc,
    spudnet_instance *out_instance,
    spudnet_error *out_error) {
	spudnet_error_clear(out_error);
	const SPUDRESULT desc_result = spudnet_instance_desc_check(desc, out_instance);
	if (desc_result != SPUD_SUCCESS)
		return desc_result;

	const SPUD_ALLOCATOR      *allocator = desc->allocator;
	struct spudnet_instance_t *instance  = SPUD_NEW_ZEROED(allocator, struct spudnet_instance_t);
	if (!instance)
		return SPUDRESULT_OUT_OF_MEMORY;
	instance->head.allocator = *allocator;
	SPUD_BIRTH(instance);
	SPUD_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);

	pthread_mutex_lock(&spudnet_curl_start_lock);
	// The libcurl found at run time, which need not be the one the headers
	// above came from.
	const curl_version_info_data *version     = curl_version_info(CURLVERSION_NOW);
	bool                          thread_safe = version && (version->features & CURL_VERSION_THREADSAFE) != 0;
	CURLcode                      code        = thread_safe ? curl_global_init(CURL_GLOBAL_DEFAULT) : CURLE_OK;
	pthread_mutex_unlock(&spudnet_curl_start_lock);

	if (!thread_safe) {
		// Nothing was started, and nothing of the platform's failed: there
		// is no code to record.
		SPUD_DELETE(allocator, struct spudnet_instance_t, instance);
		return SPUDRESULT_SPUDNET_UNSUPPORTED;
	}
	if (code != CURLE_OK) {
		SPUD_DELETE(allocator, struct spudnet_instance_t, instance);
		return spudnet_error_record(out_error, SPUDRESULT_SPUDNET_STARTUP_FAILED, SPUD_PLATFORM_ERROR_SOURCE_CURL, code, curl_easy_strerror(code));
	}
	*out_instance = instance;
	return SPUD_SUCCESS;
}

void spudnet_instance_destroy(spudnet_instance instance) {
	if (!instance)
		return;
	if (SPUD_STALE(instance, "spudnet_instance_destroy", "instance"))
		return;
	// Thread-safe on any libcurl an instance could have been made with.
	curl_global_cleanup();
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	SPUD_QUARANTINE_RELEASE(&instance->quarantine);
	spud_string_free(&allocator, instance->head.debug_name);
	SPUD_DELETE(&allocator, struct spudnet_instance_t, instance);
}

// --------------------------------------------------------------------------
// Shared by HTTP and WebSocket
// --------------------------------------------------------------------------

static SPUDRESULT spudnet_curl_result(CURLcode code, SPUDRESULT otherwise) {
	switch (code) {
	case CURLE_OPERATION_TIMEDOUT:
		// libcurl's own timer, or the system's passed up: every wait this
		// file keeps for the caller is ended by its own clock, not by this.
		return SPUDRESULT_SPUDNET_STACK_TIMED_OUT;
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
		return SPUDRESULT_SPUDNET_RESOLVE_FAILED;
	case CURLE_COULDNT_CONNECT:
		return SPUDRESULT_SPUDNET_CONNECT_FAILED;
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
	case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
	case CURLE_SSL_INVALIDCERTSTATUS:
		return SPUDRESULT_SPUDNET_TLS_FAILED;
	case CURLE_URL_MALFORMAT:
		return SPUDRESULT_SPUDNET_INVALID_URL;
	case CURLE_UNSUPPORTED_PROTOCOL:
	case CURLE_NOT_BUILT_IN:
		return SPUDRESULT_SPUDNET_UNSUPPORTED;
	case CURLE_OUT_OF_MEMORY:
		return SPUDRESULT_OUT_OF_MEMORY;
	default:
		return otherwise;
	}
}

/* A failed libcurl call as a record, and the SPUDRESULT it comes to
 * (`otherwise` where spudnet_curl_result has nothing closer). `detail` is
 * the handle's CURLOPT_ERRORBUFFER, or NULL: libcurl writes there what
 * exactly went wrong, when it knows more than the code's general wording. */
static SPUDRESULT spudnet_curl_failed(spudnet_error *record, CURLcode code, SPUDRESULT otherwise, const char *detail) {
	const char *text = (detail && detail[0] != '\0') ? detail : curl_easy_strerror(code);
	return spudnet_error_record(record, spudnet_curl_result(code, otherwise), SPUD_PLATFORM_ERROR_SOURCE_CURL, code, text);
}

/* The caller's headers as the list libcurl takes. A header with no name or
 * value is SPUDRESULT_DESC_INVALID_PARAMETERS and a failed allocation is
 * SPUDRESULT_OUT_OF_MEMORY; `*out_list` is NULL after either. Each line is
 * put together through `allocator` and freed once libcurl has its copy;
 * the list itself is libcurl's. */
static SPUDRESULT spudnet_curl_headers(
    const SPUD_ALLOCATOR *allocator,
    const spudnet_header *headers,
    uint32_t header_count,
    struct curl_slist **out_list) {
	SPUDRESULT result = SPUD_SUCCESS;
	*out_list         = NULL;
	for (uint32_t i = 0; i < header_count; ++i) {
		if (!headers[i].name || !headers[i].value) {
			result = SPUDRESULT_DESC_INVALID_PARAMETERS;
			goto failed;
		}
		size_t name_length  = strlen(headers[i].name);
		size_t value_length = strlen(headers[i].value);
		size_t line_size    = name_length + 2 + value_length + 1;
		char  *line         = (char *)spud_allocate(allocator, line_size, 1);
		if (!line) {
			result = SPUDRESULT_OUT_OF_MEMORY;
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
		spud_free(allocator, line, line_size, 1);
		if (!grown) {
			result = SPUDRESULT_OUT_OF_MEMORY;
			goto failed;
		}
		*out_list = grown;
	}
	return SPUD_SUCCESS;

failed:
	curl_slist_free_all(*out_list);
	*out_list = NULL;
	return result;
}

static uint64_t spudnet_curl_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Milliseconds left of `timeout_ms` since `start`, as poll() takes them:
 * -1 for no limit. */
static int spudnet_curl_remaining(uint64_t start, uint32_t timeout_ms) {
	if (timeout_ms == SPUDNET_WAIT_FOREVER)
		return -1;
	uint64_t elapsed = spudnet_curl_now_ms() - start;
	if (elapsed >= timeout_ms)
		return 0;
	uint64_t left = timeout_ms - elapsed;
	return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}

/* The options every transfer here has in common. */
static void spudnet_curl_common_options(CURL *curl) {
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

/* A spudnet_tls_desc in the forms libcurl takes it. The handle it is set on
 * points at these rather than copying them, so they last as long as it. */
struct spudnet_curl_tls {
	SPUDNET_TLS_TRUST trust;
	bool              skip_host_name_check;
	char             *roots; // every root, as PEM one after another; or NULL
	size_t            roots_size;
	size_t            roots_capacity; // the bytes `roots` was allocated with
	char             *pins;  // "sha256//<base64>;sha256//<base64>", or NULL
	size_t            pins_capacity; // the bytes `pins` was allocated with
};

/* Frees what spudnet_curl_tls_create made, through the same allocator. */
static void spudnet_curl_tls_free(const SPUD_ALLOCATOR *allocator, struct spudnet_curl_tls *tls) {
	spud_free(allocator, tls->roots, tls->roots_capacity, 1);
	spud_free(allocator, tls->pins, tls->pins_capacity, 1);
	tls->roots          = NULL;
	tls->roots_capacity = 0;
	tls->pins           = NULL;
	tls->pins_capacity  = 0;
}

static size_t spudnet_curl_base64_length(size_t size) { return (size + 2) / 3 * 4; }

/* Writes spudnet_curl_base64_length(size) characters, padded, with no
 * terminator, and returns the position after them. */
static char *spudnet_curl_base64(const uint8_t *data, size_t size, char *out) {
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
 * spudnet_curl_tls_free and the same allocator. */
static SPUDRESULT spudnet_curl_tls_create(const SPUD_ALLOCATOR *allocator, const spudnet_tls_desc *desc, struct spudnet_curl_tls *out_tls) {
	static const char pem_begin[]  = "-----BEGIN CERTIFICATE-----\n";
	static const char pem_end[]    = "-----END CERTIFICATE-----\n";
	static const char pin_prefix[] = "sha256//";

	memset(out_tls, 0, sizeof(*out_tls));
	SPUDRESULT result = spudnet_tls_desc_check(desc);
	if (SPUDFAIL(result))
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
				return SPUDRESULT_DESC_INVALID_PARAMETERS;
			size_t encoded = spudnet_curl_base64_length((size_t)desc->roots[i].size);
			size_t needed  = sizeof(pem_begin) + encoded + encoded / 64 + 1 + sizeof(pem_end);
			if (needed > SIZE_MAX - total)
				return SPUDRESULT_OUT_OF_MEMORY;
			total += needed;
		}
		// `line` holds one root's base64 before it is cut into lines; the
		// total is room enough for the longest.
		out_tls->roots = (char *)spud_allocate(allocator, total, 1);
		if (out_tls->roots)
			out_tls->roots_capacity = total;
		char *line = (char *)spud_allocate(allocator, total, 1);
		if (!out_tls->roots || !line) {
			spud_free(allocator, line, total, 1);
			spudnet_curl_tls_free(allocator, out_tls);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		char *at = out_tls->roots;
		for (uint32_t i = 0; i < desc->root_count; ++i) {
			size_t encoded = (size_t)(spudnet_curl_base64((const uint8_t *)desc->roots[i].data, (size_t)desc->roots[i].size, line) - line);
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
		spud_free(allocator, line, total, 1);
		out_tls->roots_size = (size_t)(at - out_tls->roots);
	}

	if (desc->pin_count > 0) {
		size_t each   = sizeof(pin_prefix) - 1 + spudnet_curl_base64_length(SPUDNET_TLS_PIN_SIZE) + 1; // and a ';' or the terminator
		out_tls->pins = (char *)spud_allocate_array(allocator, desc->pin_count, each, 1, false);
		if (!out_tls->pins) {
			spudnet_curl_tls_free(allocator, out_tls);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		out_tls->pins_capacity = (size_t)desc->pin_count * each;
		char *at = out_tls->pins;
		for (uint32_t i = 0; i < desc->pin_count; ++i) {
			memcpy(at, pin_prefix, sizeof(pin_prefix) - 1);
			at    = spudnet_curl_base64(desc->pins[i].sha256, SPUDNET_TLS_PIN_SIZE, at + sizeof(pin_prefix) - 1);
			*at++ = i + 1 < desc->pin_count ? ';' : '\0';
		}
	}
	return SPUD_SUCCESS;
}

/* Sets what `tls` asks for on a handle that is at libcurl's defaults for
 * all of it. SPUDRESULT_SPUDNET_UNSUPPORTED when this libcurl, or the TLS
 * library behind it, doesn't have one of the options. */
static SPUDRESULT spudnet_curl_tls_apply(CURL *curl, const struct spudnet_curl_tls *tls) {
	CURLcode code = CURLE_OK;

	// libcurl's own defaults are 1 and 2; said here so the desc decides both.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls->trust == SPUDNET_TLS_TRUST_ANY ? 0L : 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls->skip_host_name_check ? 0L : 2L);

	if (tls->roots) {
#if LIBCURL_VERSION_NUM >= 0x075400
		// Which of the three sources libcurl reads - this blob, the system's
		// bundle file, the system's certificate directory - once a blob is
		// set has varied between its versions, so each is named outright.
		// The system's two are whatever this libcurl was built to use.
		const curl_version_info_data *info        = curl_version_info(CURLVERSION_NOW);
		bool                          with_system = tls->trust == SPUDNET_TLS_TRUST_SYSTEM_AND_ROOTS;
		const char                   *system_file = with_system && info->age >= CURLVERSION_TENTH ? info->cainfo : NULL;
		const char                   *system_path = with_system && info->age >= CURLVERSION_TENTH ? info->capath : NULL;
		if (with_system && !system_file && !system_path)
			return SPUDRESULT_SPUDNET_UNSUPPORTED; // it doesn't say where the system's are

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
		return SPUDRESULT_SPUDNET_UNSUPPORTED;
#endif
	}
	if (code == CURLE_OK && tls->pins)
		code = curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, tls->pins);

	if (code == CURLE_OUT_OF_MEMORY)
		return SPUDRESULT_OUT_OF_MEMORY;
	return code == CURLE_OK ? SPUD_SUCCESS : SPUDRESULT_SPUDNET_UNSUPPORTED;
}

// Only a WebSocket's opening request is run this way; an HTTP transfer has
// a loop of its own further down, which stops and starts.
#if LIBCURL_VERSION_NUM >= 0x080b00

enum spudnet_curl_outcome {
	SPUDNET_CURL_FINISHED,  // the transfer ended; *out_code says how
	SPUDNET_CURL_TIMED_OUT,
	SPUDNET_CURL_STOPPED,   // `stop` was set
};

/* Runs `curl` on `multi` until the transfer ends, `timeout_ms` since `start`
 * has passed, or `stop` is set (by a thread that then calls
 * curl_multi_wakeup). A transfer that didn't finish is taken off the multi
 * handle, which drops its connection; one that did stays on it when `keep`
 * is set - a connect-only handle whose connection is about to be used. */
static enum spudnet_curl_outcome spudnet_curl_run(
    CURLM *multi,
    CURL *curl,
    uint64_t start,
    uint32_t timeout_ms,
    atomic_bool *stop,
    bool keep,
    CURLcode *out_code) {
	*out_code = CURLE_FAILED_INIT;
	if (curl_multi_add_handle(multi, curl) != CURLM_OK)
		return SPUDNET_CURL_FINISHED;

	enum spudnet_curl_outcome outcome = SPUDNET_CURL_FINISHED;
	for (;;) {
		if (atomic_load(stop)) {
			outcome = SPUDNET_CURL_STOPPED;
			break;
		}

		int running = 0;
		if (curl_multi_perform(multi, &running) != CURLM_OK)
			break;
		if (running == 0) {
			int      left = 0;
			CURLMsg *message;
			while ((message = curl_multi_info_read(multi, &left)) != NULL) {
				if (message->msg == CURLMSG_DONE && message->easy_handle == curl)
					*out_code = message->data.result;
			}
			break;
		}

		int remaining = spudnet_curl_remaining(start, timeout_ms);
		if (remaining == 0) {
			outcome = SPUDNET_CURL_TIMED_OUT;
			break;
		}
		// curl_multi_poll has no "no limit"; a long wait, repeated, is one.
		// It returns early for activity and for curl_multi_wakeup.
		curl_multi_poll(multi, NULL, 0, (remaining < 0 || remaining > 60000) ? 60000 : remaining, NULL);
	}

	if (!keep || outcome != SPUDNET_CURL_FINISHED || *out_code != CURLE_OK)
		curl_multi_remove_handle(multi, curl);
	return outcome;
}

#endif // LIBCURL_VERSION_NUM

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------

/* A transfer here is one easy handle on its client's multi handle. libcurl
 * moves a body by calling back - for the next piece of a request body, with
 * the next piece of a response body - and spudnet.h has the caller ask
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

struct spudnet_http_client_t {
	spud_object_head head; // first: spudnethttp.c reaches the allocator through it
	// Holds the open connections, and is driven by whichever transfer is
	// under way - one at a time, which `busy` keeps to.
	CURLM      *multi;
	atomic_bool busy;
	// Set on each transfer's handle when it starts.
	struct spudnet_curl_tls tls;
	struct spudnet_proxy    proxy;
};

/* What `proxy` asks for on a handle that is at libcurl's default, which is
 * the environment's proxy variables. libcurl copies the string. */
static void spudnet_curl_proxy_apply(CURL *curl, const struct spudnet_proxy *proxy) {
	if (proxy->mode == SPUDNET_PROXY_NONE)
		curl_easy_setopt(curl, CURLOPT_PROXY, ""); // "" also stops it reading the environment
	else if (proxy->mode == SPUDNET_PROXY_EXPLICIT)
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy->url);
}

void spudnet_http_client_destroy(spudnet_http_client client) {
	if (!client)
		return;
	if (SPUD_STALE(client, "spudnet_http_client_destroy", "client"))
		return;
	if (client->multi)
		curl_multi_cleanup(client->multi);
	SPUD_KILL(client);
	const SPUD_ALLOCATOR allocator = client->head.allocator;
	spudnet_curl_tls_free(&allocator, &client->tls);
	spudnet_proxy_free(&allocator, &client->proxy);
	spud_string_free(&allocator, client->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(client), &allocator, struct spudnet_http_client_t, client);
}

SPUDRESULT spudnet_http_client_create(
    spudnet_instance instance,
    const spudnet_http_client_desc *desc,
    spudnet_http_client *out_client) {
	if (!out_client)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_client = NULL;
	if (!instance)
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudnet_http_client_create", "instance"))
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_NET_HTTP_CLIENT_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;

	struct spudnet_http_client_t *client = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudnet_http_client_t);
	if (!client)
		return SPUDRESULT_OUT_OF_MEMORY;
	client->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(client, &instance->quarantine);
	atomic_init(&client->busy, false);
	SPUDRESULT result = spudnet_curl_tls_create(&client->head.allocator, &desc->tls, &client->tls);
	if (!SPUDFAIL(result))
		result = spudnet_proxy_parse(&client->head.allocator, &desc->proxy, &client->proxy);
	if (!SPUDFAIL(result)) {
		client->multi = curl_multi_init();
		if (!client->multi)
			result = SPUDRESULT_GENERAL_FAILURE;
	}
	if (SPUDFAIL(result)) {
		spudnet_http_client_destroy(client);
		return result;
	}

	*out_client = client;
	return SPUD_SUCCESS;
}

/* The backend's side of a spudnet_http_transfer_t. */
struct spudnet_curl_http {
	struct spudnet_http_transfer_t *owner;
	struct spudnet_http_client_t   *client;
	CURL                           *curl;
	struct curl_slist              *headers;
	bool                            on_multi;     // the handle has been added
	bool                            holds_client; // this transfer set the client's `busy`
	atomic_bool                     aborted;
	char                            detail[CURL_ERROR_SIZE];
	bool                            out_of_memory;

	// The connection is up and the request about to go out on it.
	bool connected;

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

static size_t spudnet_curl_http_on_header(char *data, size_t size, size_t count, void *user) {
	struct spudnet_curl_http *http  = (struct spudnet_curl_http *)user;
	size_t                    bytes = size * count;
	// What follows the response's own headers is a chunked body's trailer.
	if (http->headers_done)
		return bytes;

	if (bytes >= 5 && spudnet_ascii_equal_no_case(data, "HTTP/", 5))
		http->block_has_location = false;
	if (bytes >= 9 && spudnet_ascii_equal_no_case(data, "Location:", 9))
		http->block_has_location = true;
	// A status line empties what was collected, so only the last block's
	// headers are left.
	// libcurl calls back from inside curl_multi_perform, on the thread of
	// the transfer call that is driving it: the caller's.
	if (!spudnet_head_add_header_line(&http->owner->head.allocator, &http->owner->response, data, bytes)) {
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
static size_t spudnet_curl_http_on_body(char *data, size_t size, size_t count, void *user) {
	struct spudnet_curl_http *http  = (struct spudnet_curl_http *)user;
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
		const SPUD_ALLOCATOR *allocator = &http->owner->head.allocator;
		uint8_t              *moved     = (uint8_t *)spud_allocate(allocator, bytes, 1);
		if (!moved) {
			http->out_of_memory = true;
			return 0; // anything but the full count stops the transfer
		}
		spud_free(allocator, http->pending, http->pending_capacity, 1);
		http->pending          = moved;
		http->pending_capacity = bytes;
	}
	memcpy(http->pending, data, bytes);
	http->pending_size   = bytes;
	http->pending_offset = 0;
	return bytes;
}

static size_t spudnet_curl_http_on_read(char *buffer, size_t size, size_t count, void *user) {
	struct spudnet_curl_http *http = (struct spudnet_curl_http *)user;
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
static int spudnet_curl_http_on_connected(void *user, char *primary_ip, char *local_ip, int primary_port, int local_port) {
	(void)primary_ip;
	(void)local_ip;
	(void)primary_port;
	(void)local_port;
	((struct spudnet_curl_http *)user)->connected = true;
	return CURL_PREREQFUNC_OK;
}
#endif

enum spudnet_curl_http_goal {
	SPUDNET_CURL_HTTP_CONNECTED,
	SPUDNET_CURL_HTTP_SENT_SOME,
	SPUDNET_CURL_HTTP_HEADERS,
	SPUDNET_CURL_HTTP_BODY_PIECE,
};

static bool spudnet_curl_http_reached(const struct spudnet_curl_http *http, enum spudnet_curl_http_goal goal) {
	switch (goal) {
	case SPUDNET_CURL_HTTP_CONNECTED:
		return http->connected;
	case SPUDNET_CURL_HTTP_SENT_SOME:
		return http->send_taken > 0;
	case SPUDNET_CURL_HTTP_HEADERS:
		return http->headers_done;
	default:
		return http->pending_offset < http->pending_size;
	}
}

/* Runs the client's multi handle until the transfer has reached `goal` or
 * ended (SPUD_SUCCESS for either; the caller looks at which), the caller's
 * time is up, or the transfer is aborted. With SPUDNET_NO_WAIT it makes the
 * one pass that costs no waiting. */
static SPUDRESULT spudnet_curl_http_drive(struct spudnet_curl_http *http, enum spudnet_curl_http_goal goal, uint64_t start, uint32_t timeout_ms) {
	CURLM *multi = http->client->multi;
	for (;;) {
		if (atomic_load(&http->aborted))
			return SPUDRESULT_SPUDNET_ABORTED;

		int running = 0;
		if (curl_multi_perform(multi, &running) != CURLM_OK)
			return SPUDRESULT_SPUDNET_HTTP_FAILED;
		int      left = 0;
		CURLMsg *message;
		while ((message = curl_multi_info_read(multi, &left)) != NULL) {
			if (message->msg == CURLMSG_DONE && message->easy_handle == http->curl) {
				http->finished    = true;
				http->finish_code = message->data.result;
			}
		}
		if (http->out_of_memory)
			return SPUDRESULT_OUT_OF_MEMORY;
		if (http->finished || spudnet_curl_http_reached(http, goal))
			return SPUD_SUCCESS;

		int remaining = spudnet_curl_remaining(start, timeout_ms);
		if (remaining == 0)
			return SPUDRESULT_SPUDNET_TIMED_OUT;
		// curl_multi_poll has no "no limit"; a long wait, repeated, is one.
		// It returns early for activity and for curl_multi_wakeup.
		curl_multi_poll(multi, NULL, 0, (remaining < 0 || remaining > 60000) ? 60000 : remaining, NULL);
	}
}

/* The record and the result for a transfer libcurl ended with an error. */
static SPUDRESULT spudnet_curl_http_failed(struct spudnet_curl_http *http, SPUDRESULT otherwise) {
	return spudnet_curl_failed(&http->owner->error, http->finish_code, otherwise, http->detail);
}

/* Lets a paused direction go on. It lifts both, which is harmless: the
 * other direction's callback pauses again if it still has nothing to do. */
static void spudnet_curl_http_unpause(struct spudnet_curl_http *http) {
	http->send_paused = false;
	http->recv_paused = false;
	curl_easy_pause(http->curl, CURLPAUSE_CONT);
}

SPUDRESULT spudnet_http_backend_create(struct spudnet_http_transfer_t *transfer) {
	struct spudnet_curl_http *http = SPUD_NEW_ZEROED(&transfer->head.allocator, struct spudnet_curl_http);
	if (!http)
		return SPUDRESULT_OUT_OF_MEMORY;
	http->owner  = transfer;
	http->client = transfer->client;
	atomic_init(&http->aborted, false);
	http->curl = curl_easy_init();
	if (!http->curl) {
		SPUD_DELETE(&transfer->head.allocator, struct spudnet_curl_http, http);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	transfer->backend = http;
	return SPUD_SUCCESS;
}

void spudnet_http_backend_destroy(struct spudnet_http_transfer_t *transfer) {
	struct spudnet_curl_http *http = (struct spudnet_curl_http *)transfer->backend;
	if (!http)
		return;
	// Off the multi handle first: for a transfer that didn't run to its
	// end this is what drops its connection.
	if (http->on_multi)
		curl_multi_remove_handle(http->client->multi, http->curl);
	curl_easy_cleanup(http->curl);
	curl_slist_free_all(http->headers);
	spud_free(&transfer->head.allocator, http->pending, http->pending_capacity, 1);
	if (http->holds_client)
		atomic_store(&http->client->busy, false);
	SPUD_DELETE(&transfer->head.allocator, struct spudnet_curl_http, http);
	transfer->backend = NULL;
}

void spudnet_http_backend_abort(struct spudnet_http_transfer_t *transfer) {
	struct spudnet_curl_http *http = (struct spudnet_curl_http *)transfer->backend;
	atomic_store(&http->aborted, true);
	// The one libcurl call that may come from another thread.
	curl_multi_wakeup(http->client->multi);
}

bool spudnet_http_backend_aborted(struct spudnet_http_transfer_t *transfer) {
	return atomic_load(&((struct spudnet_curl_http *)transfer->backend)->aborted);
}

SPUDRESULT spudnet_http_backend_start(
    struct spudnet_http_transfer_t *transfer,
    const spudnet_http_transfer_desc *desc,
    enum spudnet_accept_encoding encoding) {
	struct spudnet_curl_http *http  = (struct spudnet_curl_http *)transfer->backend;
	uint64_t                  start = spudnet_curl_now_ms();

	// One transfer drives the client's multi handle at a time.
	if (atomic_exchange(&http->client->busy, true))
		return SPUDRESULT_SPUDNET_INVALID_HTTP_TRANSFER;
	http->holds_client = true;

	SPUDRESULT headers_result = spudnet_curl_headers(&transfer->head.allocator, desc->headers, desc->header_count, &http->headers);
	if (SPUDFAIL(headers_result))
		return headers_result;

	CURL *curl = http->curl;
	spudnet_curl_common_options(curl);
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
	if (encoding == SPUDNET_ACCEPT_ENCODING_STACK)
		curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, spudnet_curl_http_on_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, http);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, spudnet_curl_http_on_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, http);
#if LIBCURL_VERSION_NUM >= 0x075000
	curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, spudnet_curl_http_on_connected);
	curl_easy_setopt(curl, CURLOPT_PREREQDATA, http);
#endif

	// The request's shape comes from what the desc says its body is, and
	// from nothing about the method but the one case HTTP itself singles
	// out: a HEAD's response has no body to wait for.
	if (transfer->body == SPUDNET_HTTP_BODY_NONE) {
		if (transfer->is_head)
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	} else {
		// An upload: the body is asked for through the read callback. With
		// a size libcurl announces it (Content-Length, 0 included); without
		// one it sends the body chunked.
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, spudnet_curl_http_on_read);
		curl_easy_setopt(curl, CURLOPT_READDATA, http);
		if (transfer->body == SPUDNET_HTTP_BODY_SIZED)
			curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)transfer->body_size);
		// libcurl would hold a large body back for the server's go-ahead
		// ("Expect: 100-continue"); the header given empty takes that off.
		struct curl_slist *grown = curl_slist_append(http->headers, "Expect:");
		if (!grown)
			return SPUDRESULT_OUT_OF_MEMORY;
		http->headers = grown;
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http->headers);

	SPUDRESULT result = spudnet_curl_tls_apply(curl, &http->client->tls);
	if (SPUDFAIL(result))
		return result;
	spudnet_curl_proxy_apply(curl, &http->client->proxy);

	if (curl_multi_add_handle(http->client->multi, curl) != CURLM_OK)
		return SPUDRESULT_SPUDNET_HTTP_FAILED;
	http->on_multi = true;

#if LIBCURL_VERSION_NUM >= 0x075000
	// As far as the connection being up, so that a server that can't be
	// reached or a certificate that is turned away is start's to report.
	// An older libcurl doesn't say when that is, and there the first later
	// call reports it, as spudnet.h allows.
	result = spudnet_curl_http_drive(http, SPUDNET_CURL_HTTP_CONNECTED, start, desc->timeout_ms);
	if (SPUDFAIL(result))
		return result;
	if (http->finished && http->finish_code != CURLE_OK)
		return spudnet_curl_http_failed(http, SPUDRESULT_SPUDNET_HTTP_FAILED);
#else
	(void)start;
#endif
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_http_backend_send(
    struct spudnet_http_transfer_t *transfer,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_sent) {
	struct spudnet_curl_http *http  = (struct spudnet_curl_http *)transfer->backend;
	uint64_t                  start = spudnet_curl_now_ms();

	SPUDRESULT result = SPUD_SUCCESS;
	if (!http->finished) {
		http->send_data  = (const uint8_t *)data;
		http->send_size  = size;
		http->send_taken = 0;
		if (http->send_paused)
			spudnet_curl_http_unpause(http);
		result = spudnet_curl_http_drive(http, SPUDNET_CURL_HTTP_SENT_SOME, start, timeout_ms);
	}
	// Nothing of the caller's stays behind, however this went.
	size_t taken     = http->send_taken;
	http->send_data  = NULL;
	http->send_size  = 0;
	http->send_taken = 0;

	// Bytes libcurl took count, even if the time ran out in the same pass.
	if (taken > 0) {
		*out_sent = taken;
		return SPUD_SUCCESS;
	}
	if (SPUDFAIL(result))
		return result;
	if (http->finish_code != CURLE_OK)
		return spudnet_curl_http_failed(http, SPUDRESULT_SPUDNET_SEND_FAILED);
	// The server answered in full without waiting for the rest of the body
	// - a refusal, usually. There is nowhere for the bytes to go and a
	// response for the caller to read, so they are let through as sent.
	*out_sent = size;
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_http_backend_receive_response(
    struct spudnet_http_transfer_t *transfer,
    uint32_t timeout_ms) {
	struct spudnet_curl_http *http  = (struct spudnet_curl_http *)transfer->backend;
	uint64_t                  start = spudnet_curl_now_ms();

	// Said once: from here the read callback answers "no more".
	if (!http->send_ended) {
		http->send_ended = true;
		if (http->send_paused)
			spudnet_curl_http_unpause(http);
	}

	if (!http->headers_done && !http->finished) {
		SPUDRESULT result = spudnet_curl_http_drive(http, SPUDNET_CURL_HTTP_HEADERS, start, timeout_ms);
		if (SPUDFAIL(result))
			return result;
	}
	if (http->finished && http->finish_code != CURLE_OK)
		return spudnet_curl_http_failed(http, SPUDRESULT_SPUDNET_HTTP_FAILED);

	long status = 0;
	curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status <= 0)
		return SPUDRESULT_SPUDNET_HTTP_FAILED; // ended cleanly with no response at all
	transfer->response.status = (uint32_t)status;
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_http_backend_recv(
    struct spudnet_http_transfer_t *transfer,
    void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_received) {
	struct spudnet_curl_http *http  = (struct spudnet_curl_http *)transfer->backend;
	uint64_t                  start = spudnet_curl_now_ms();

	for (;;) {
		if (http->pending_offset < http->pending_size) {
			size_t left  = http->pending_size - http->pending_offset;
			size_t bytes = left < size ? left : size;
			memcpy(data, http->pending + http->pending_offset, bytes);
			http->pending_offset += bytes;
			*out_received         = bytes;
			return SPUD_SUCCESS;
		}
		if (http->finished) {
			if (http->finish_code != CURLE_OK)
				return spudnet_curl_http_failed(http, SPUDRESULT_SPUDNET_RECV_FAILED);
			*out_received = 0; // the body's end
			return SPUD_SUCCESS;
		}

		// The piece libcurl was holding back while the last one was being
		// taken may be handed over by the unpausing itself, so the top of
		// the loop looks before anything waits.
		if (http->recv_paused) {
			spudnet_curl_http_unpause(http);
			continue;
		}
		SPUDRESULT result = spudnet_curl_http_drive(http, SPUDNET_CURL_HTTP_BODY_PIECE, start, timeout_ms);
		// A piece that arrived in the pass the time ran out in still counts.
		if (SPUDFAIL(result) && !(http->pending_offset < http->pending_size))
			return result;
	}
}

// --------------------------------------------------------------------------
// WebSocket
// --------------------------------------------------------------------------

/* curl_ws_send/curl_ws_recv exist from 7.86, but WebSocket was an
 * experimental, off-by-default build option until 8.11, and what a short
 * curl_ws_send means changed along the way. Older headers get entry points
 * that say so, as does a newer libcurl built without the protocol. */
#if LIBCURL_VERSION_NUM >= 0x080b00

struct spudnet_websocket_t {
	spud_object_head head;
	spudnet_instance instance;
	// The multi handle only drives the opening request; the easy handle
	// stays on it afterwards because that is what keeps a connect-only
	// connection alive.
	CURLM             *multi;
	CURL              *curl;
	struct curl_slist *headers;
	// Filled in by connect; the handle points into it from then on.
	struct spudnet_curl_tls tls;
	// By SPUDNET_ERROR_SIDE; each is written only by its side's caller.
	// `detail` is where libcurl writes about the opening request, and
	// `send_code` what the last frame that didn't go out failed with -
	// written under the lock, since a receiver sends close frames too.
	spudnet_error errors[SPUDNET_ERROR_SIDE_COUNT];
	char          detail[CURL_ERROR_SIZE];
	CURLcode      send_code;
	// The connection's descriptor, for poll() only; libcurl owns it.
	curl_socket_t descriptor;

	// Set by spudnet_websocket_abort and never cleared. `wake` is an
	// eventfd abort writes to, so a poll() on the connection ends too.
	atomic_bool aborted;
	int         wake;

	// A libcurl handle takes one thread at a time, and a receiver and a
	// sender are allowed to overlap. Held around each libcurl call and
	// never across a poll().
	pthread_mutex_t lock;

	bool     connect_called;
	bool     open;
	bool     in_wait_set;
	uint64_t max_message_size;
	uint64_t message_size;

	// Nothing more will arrive, and why.
	bool     finished;
	bool     too_big;
	bool     peer_closed;
	uint16_t close_code;
	bool     close_requested;
};

static bool spudnet_curl_has_websocket(void) {
	const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
	for (const char *const *protocol = info->protocols; protocol && *protocol; ++protocol) {
		if (strcmp(*protocol, "ws") == 0)
			return true;
	}
	return false;
}

enum spudnet_curl_waited {
	SPUDNET_CURL_READY,
	SPUDNET_CURL_WAIT_TIMED_OUT,
	SPUDNET_CURL_WAIT_ABORTED,
};

/* Waits for the connection to become readable or writable, for the time
 * to run out, or for an abort. Never called with the lock held. */
static enum spudnet_curl_waited spudnet_curl_wait(struct spudnet_websocket_t *socket, short events, uint64_t start, uint32_t timeout_ms) {
	for (;;) {
		if (atomic_load(&socket->aborted))
			return SPUDNET_CURL_WAIT_ABORTED;

		struct pollfd waited[2];
		waited[0].fd      = socket->descriptor;
		waited[0].events  = events;
		waited[0].revents = 0;
		waited[1].fd      = socket->wake;
		waited[1].events  = POLLIN;
		waited[1].revents = 0;
		int ready         = poll(waited, 2, spudnet_curl_remaining(start, timeout_ms));
		if (ready == 0)
			return SPUDNET_CURL_WAIT_TIMED_OUT;
		if (ready < 0 && errno == EINTR)
			continue;
		if (atomic_load(&socket->aborted))
			return SPUDNET_CURL_WAIT_ABORTED;
		// An error or hang-up counts as ready: the next libcurl call
		// reports it.
		return SPUDNET_CURL_READY;
	}
}

enum spudnet_curl_sent {
	SPUDNET_CURL_SENT,
	SPUDNET_CURL_SEND_FAILED,
	SPUDNET_CURL_SEND_TIMED_OUT,
	SPUDNET_CURL_SEND_ABORTED,
};

/* Sends one frame whole, waiting for room up to `timeout_ms` since `start`
 * (SPUDNET_NO_WAIT: whatever fits right now or not at all). Called with the
 * lock held; lets go of it only while waiting. Anything but SENT leaves a
 * frame that may be part-written, and marks the socket finished. */
static enum spudnet_curl_sent spudnet_curl_send_frame(
    struct spudnet_websocket_t *socket,
    const void *data,
    size_t size,
    unsigned int flags,
    uint64_t start,
    uint32_t timeout_ms) {
	enum spudnet_curl_sent outcome = SPUDNET_CURL_SEND_FAILED;
	size_t                 offset  = 0;
	for (;;) {
		// A short send has already put the frame's header on the wire for
		// the full size; the next call carries on with the rest.
		size_t   sent = 0;
		CURLcode code = curl_ws_send(socket->curl, (const char *)data + offset, size - offset, &sent, 0, flags);
		offset       += sent;
		if (code == CURLE_OK && offset >= size)
			return SPUDNET_CURL_SENT;
		if (code != CURLE_OK && code != CURLE_AGAIN) {
			socket->send_code = code;
			break;
		}
		if (code == CURLE_AGAIN) {
			pthread_mutex_unlock(&socket->lock);
			enum spudnet_curl_waited waited = spudnet_curl_wait(socket, POLLOUT, start, timeout_ms);
			pthread_mutex_lock(&socket->lock);
			if (waited == SPUDNET_CURL_WAIT_TIMED_OUT) {
				outcome = SPUDNET_CURL_SEND_TIMED_OUT;
				break;
			}
			if (waited == SPUDNET_CURL_WAIT_ABORTED) {
				outcome = SPUDNET_CURL_SEND_ABORTED;
				break;
			}
		}
	}
	socket->finished = true;
	return outcome;
}

void spudnet_websocket_destroy(spudnet_websocket socket) {
	if (!socket)
		return;
	if (SPUD_STALE(socket, "spudnet_websocket_destroy", "socket"))
		return;
	if (socket->multi && socket->curl)
		curl_multi_remove_handle(socket->multi, socket->curl); // harmless when it isn't on it
	if (socket->curl)
		curl_easy_cleanup(socket->curl);
	if (socket->multi)
		curl_multi_cleanup(socket->multi);
	curl_slist_free_all(socket->headers);
	SPUD_KILL(socket);
	const SPUD_ALLOCATOR allocator = socket->head.allocator;
	spudnet_curl_tls_free(&allocator, &socket->tls);
	if (socket->wake >= 0)
		close(socket->wake);
	pthread_mutex_destroy(&socket->lock);
	spud_string_free(&allocator, socket->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(socket), &allocator, struct spudnet_websocket_t, socket);
}

spudnet_instance spudnet_websocket_instance(spudnet_websocket socket) { return socket ? socket->instance : NULL; }

SPUDRESULT spudnet_websocket_create(
    spudnet_instance instance,
    spudnet_websocket *out_socket) {
	if (!out_socket)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudnet_websocket_create", "instance"))
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (!spudnet_curl_has_websocket())
		return SPUDRESULT_SPUDNET_UNSUPPORTED;

	struct spudnet_websocket_t *socket = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudnet_websocket_t);
	if (!socket)
		return SPUDRESULT_OUT_OF_MEMORY;
	socket->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(socket, &instance->quarantine);
	socket->instance       = instance;
	pthread_mutex_init(&socket->lock, NULL);
	atomic_init(&socket->aborted, false);
	socket->descriptor = CURL_SOCKET_BAD;
	socket->wake       = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	socket->multi      = curl_multi_init();
	socket->curl       = curl_easy_init();
	if (socket->wake < 0 || !socket->multi || !socket->curl) {
		spudnet_websocket_destroy(socket);
		return SPUDRESULT_GENERAL_FAILURE;
	}

	*out_socket = socket;
	return SPUD_SUCCESS;
}

void spudnet_websocket_get_error(
    spudnet_websocket socket,
    SPUDNET_ERROR_SIDE side,
    spudnet_error *out_error) {
	if (!socket) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	if (SPUD_STALE(socket, "spudnet_websocket_get_error", "socket")) {
		spudnet_error_get(NULL, out_error);
		return;
	}
	bool known = side == SPUDNET_ERROR_SIDE_RECEIVING || side == SPUDNET_ERROR_SIDE_SENDING;
	spudnet_error_get(known ? &socket->errors[side] : NULL, out_error);
}

// For a wait set (see the end of spudnetshared.h). An open socket here is a
// descriptor, so the set polls it and there is nothing to notify. What
// libcurl or the TLS layer has already read off that descriptor never shows
// on it again, which is why the set is asked to name the socket once.

SPUDRESULT spudnet_websocket_wait_attach(
    spudnet_websocket socket,
    struct spudnet_wait_set_t *set,
    bool *out_has_native,
    intptr_t *out_native,
    bool *out_report_once) {
	(void)set;
	pthread_mutex_lock(&socket->lock);
	SPUDRESULT result = SPUD_SUCCESS;
	if (atomic_load(&socket->aborted) || !socket->open)
		result = SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	else if (socket->in_wait_set)
		result = SPUDRESULT_SPUDNET_ALREADY_IN_WAIT_SET;
	else
		socket->in_wait_set = true;
	pthread_mutex_unlock(&socket->lock);
	if (SPUDFAIL(result))
		return result;

	*out_has_native  = true;
	*out_native      = socket->descriptor;
	*out_report_once = true;
	return SPUD_SUCCESS;
}

void spudnet_websocket_wait_detach(spudnet_websocket socket) {
	pthread_mutex_lock(&socket->lock);
	socket->in_wait_set = false;
	pthread_mutex_unlock(&socket->lock);
}

bool spudnet_websocket_wait_ready(spudnet_websocket socket) {
	(void)socket;
	return false; // never asked: the set polls the descriptor
}

void spudnet_websocket_abort(spudnet_websocket socket) {
	if (!socket)
		return;
	if (SPUD_STALE(socket, "spudnet_websocket_abort", "socket"))
		return;
	atomic_store(&socket->aborted, true);
	// One for a wait on the open connection, one for the opening request.
	uint64_t one = 1;
	if (write(socket->wake, &one, sizeof(one)) < 0) {
		// Full means it is already readable, which is all this is for.
	}
	curl_multi_wakeup(socket->multi);
}

SPUDRESULT spudnet_websocket_connect(
    spudnet_websocket socket,
    const spudnet_websocket_connect_desc *desc,
    uint32_t *out_http_status) {
	if (out_http_status)
		*out_http_status = 0;
	if (!socket)
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if (SPUD_STALE(socket, "spudnet_websocket_connect", "socket"))
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!desc->url || desc->max_message_size == 0 || (desc->header_count > 0 && !desc->headers))
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (!spudnet_url_has_scheme(desc->url, "ws") && !spudnet_url_has_scheme(desc->url, "wss"))
		return SPUDRESULT_SPUDNET_INVALID_URL;

	const SPUD_ALLOCATOR *allocator = &socket->head.allocator;
	uint64_t              start     = spudnet_curl_now_ms();
	struct curl_slist    *headers   = NULL;
	SPUDRESULT headers_result = spudnet_curl_headers(allocator, desc->headers, desc->header_count, &headers);
	if (SPUDFAIL(headers_result))
		return headers_result;

	struct spudnet_curl_tls tls;
	SPUDRESULT              result = spudnet_curl_tls_create(allocator, &desc->tls, &tls);
	if (SPUDFAIL(result)) {
		curl_slist_free_all(headers);
		return result;
	}
	// Only needed until it has been set on the handle, which copies it.
	struct spudnet_proxy proxy;
	result = spudnet_proxy_parse(allocator, &desc->proxy, &proxy);
	if (SPUDFAIL(result)) {
		spudnet_proxy_free(allocator, &proxy);
		spudnet_curl_tls_free(allocator, &tls);
		curl_slist_free_all(headers);
		return result;
	}

	pthread_mutex_lock(&socket->lock);
	if (atomic_load(&socket->aborted))
		result = SPUDRESULT_SPUDNET_ABORTED;
	else if (socket->connect_called)
		result = SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	else if (desc->timeout_ms == SPUDNET_NO_WAIT)
		result = SPUDRESULT_SPUDNET_TIMED_OUT;
	else
		socket->connect_called = true;
	pthread_mutex_unlock(&socket->lock);
	if (SPUDFAIL(result)) {
		curl_slist_free_all(headers);
		spudnet_curl_tls_free(allocator, &tls);
		spudnet_proxy_free(allocator, &proxy);
		return result;
	}
	socket->headers          = headers;
	socket->tls              = tls; // the socket's from here
	socket->max_message_size = desc->max_message_size;

	CURL *curl = socket->curl;
	spudnet_curl_common_options(curl);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, socket->detail);
	curl_easy_setopt(curl, CURLOPT_URL, desc->url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, socket->headers);
	// Said either way, so that it is the desc and not libcurl's default
	// that decides what becomes of a redirect.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, desc->follow_redirects ? 1L : 0L);
	spudnet_curl_proxy_apply(curl, &proxy);
	spudnet_proxy_free(allocator, &proxy);
	// 2: do the opening request, then hand the connection over for
	// curl_ws_send/curl_ws_recv instead of running a transfer on it.
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);

	CURLcode                  code    = CURLE_OK;
	enum spudnet_curl_outcome outcome = SPUDNET_CURL_FINISHED;
	long                      status  = 0;
	result                            = spudnet_curl_tls_apply(curl, &socket->tls);
	if (!SPUDFAIL(result)) {
		outcome = spudnet_curl_run(socket->multi, curl, start, desc->timeout_ms, &socket->aborted, true, &code);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	}
	if (out_http_status)
		*out_http_status = (uint32_t)status;

	if (SPUDFAIL(result)) {
		// Nothing was sent; the result already says what the stack lacks.
	} else if (outcome == SPUDNET_CURL_STOPPED)
		result = SPUDRESULT_SPUDNET_ABORTED;
	else if (outcome == SPUDNET_CURL_TIMED_OUT)
		result = SPUDRESULT_SPUDNET_TIMED_OUT;
	else {
		if (code == CURLE_OK)
			code = curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket->descriptor);
		if (code == CURLE_OK && socket->descriptor != CURL_SOCKET_BAD)
			result = SPUD_SUCCESS;
		else if (status != 0 && status != 101)
			result = SPUDRESULT_SPUDNET_WS_UPGRADE_REFUSED;
		else if (code != CURLE_OK)
			result = spudnet_curl_failed(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], code, SPUDRESULT_SPUDNET_CONNECT_FAILED, socket->detail);
		else
			result = SPUDRESULT_SPUDNET_CONNECT_FAILED; // libcurl was content and still had no connection to hand over
	}

	pthread_mutex_lock(&socket->lock);
	if (SPUDFAIL(result))
		socket->finished = true; // this socket is spent
	else
		socket->open = true;
	pthread_mutex_unlock(&socket->lock);
	return result;
}

/* What a call on a socket that can't carry it returns, or SPUD_SUCCESS for
 * one that can. Called with the lock held. */
static SPUDRESULT spudnet_curl_websocket_usable(struct spudnet_websocket_t *socket) {
	if (atomic_load(&socket->aborted))
		return SPUDRESULT_SPUDNET_ABORTED;
	if (!socket->open)
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_websocket_send(
    spudnet_websocket socket,
    SPUDNET_WEBSOCKET_MESSAGE type,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms) {
	if (!socket)
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if (SPUD_STALE(socket, "spudnet_websocket_send", "socket"))
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if ((size > 0 && !data) || size > SIZE_MAX)
		return SPUDRESULT_SPUDNET_SEND_FAILED;

	uint64_t start = spudnet_curl_now_ms();
	pthread_mutex_lock(&socket->lock);
	SPUDRESULT result = spudnet_curl_websocket_usable(socket);
	if (!SPUDFAIL(result) && socket->finished)
		result = SPUDRESULT_SPUDNET_SEND_FAILED;
	if (!SPUDFAIL(result) && timeout_ms == SPUDNET_NO_WAIT)
		result = SPUDRESULT_SPUDNET_TIMED_OUT;
	// libcurl frames whatever it is given as text. Refused here, nothing is
	// sent and the connection carries on.
	if (!SPUDFAIL(result) && type == SPUDNET_WEBSOCKET_MESSAGE_TEXT && !spudnet_utf8_valid((const uint8_t *)data, (size_t)size))
		result = SPUDRESULT_SPUDNET_SEND_FAILED;
	if (!SPUDFAIL(result)) {
		unsigned int flags = type == SPUDNET_WEBSOCKET_MESSAGE_TEXT ? CURLWS_TEXT : CURLWS_BINARY;
		switch (spudnet_curl_send_frame(socket, size > 0 ? data : "", (size_t)size, flags, start, timeout_ms)) {
		case SPUDNET_CURL_SENT:
			break;
		case SPUDNET_CURL_SEND_TIMED_OUT:
			result = SPUDRESULT_SPUDNET_TIMED_OUT;
			break;
		case SPUDNET_CURL_SEND_ABORTED:
			result = SPUDRESULT_SPUDNET_ABORTED;
			break;
		default:
			spudnet_curl_failed(&socket->errors[SPUDNET_ERROR_SIDE_SENDING], socket->send_code, SPUDRESULT_SPUDNET_SEND_FAILED, NULL);
			result = SPUDRESULT_SPUDNET_SEND_FAILED;
			socket->errors[SPUDNET_ERROR_SIDE_SENDING].result = result;
			break;
		}
	}
	pthread_mutex_unlock(&socket->lock);
	return result;
}

/* Why a finished socket finished. Called with the lock held. */
static SPUDRESULT spudnet_curl_websocket_end_result(const struct spudnet_websocket_t *socket) {
	if (socket->too_big)
		return SPUDRESULT_SPUDNET_WS_MESSAGE_TOO_BIG;
	if (socket->peer_closed || socket->close_requested)
		return SPUDRESULT_SPUDNET_WS_CLOSED;
	return SPUDRESULT_SPUDNET_RECV_FAILED;
}

SPUDRESULT spudnet_websocket_recv(
    spudnet_websocket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received,
    SPUDNET_WEBSOCKET_MESSAGE *out_type,
    bool *out_message_complete) {
	if (!out_received || !out_type || !out_message_complete)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_received         = 0;
	*out_type             = SPUDNET_WEBSOCKET_MESSAGE_BINARY;
	*out_message_complete = false;
	if (!socket)
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if (SPUD_STALE(socket, "spudnet_websocket_recv", "socket"))
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if (!data || size == 0)
		return SPUDRESULT_ZERO_SIZE;

	uint64_t   start  = spudnet_curl_now_ms();
	size_t     room   = size > SIZE_MAX ? SIZE_MAX : (size_t)size;
	SPUDRESULT result = SPUD_SUCCESS;

	pthread_mutex_lock(&socket->lock);
	for (;;) {
		result = spudnet_curl_websocket_usable(socket);
		if (SPUDFAIL(result))
			break;
		if (socket->finished) {
			result = spudnet_curl_websocket_end_result(socket);
			break;
		}

		// Asked first, polled second: bytes libcurl or the TLS layer has
		// already read never make the descriptor readable again. With
		// SPUDNET_NO_WAIT the poll returns at once and this only looks.
		size_t                      count = 0;
		const struct curl_ws_frame *frame = NULL;
		CURLcode                    code  = curl_ws_recv(socket->curl, data, room, &count, &frame);

		if (code == CURLE_AGAIN) {
			pthread_mutex_unlock(&socket->lock);
			enum spudnet_curl_waited waited = spudnet_curl_wait(socket, POLLIN, start, timeout_ms);
			pthread_mutex_lock(&socket->lock);
			if (waited == SPUDNET_CURL_WAIT_TIMED_OUT) {
				result = SPUDRESULT_SPUDNET_TIMED_OUT;
				break;
			}
			continue; // ready, or aborted - which the top of the loop reports
		}
		if (code != CURLE_OK || !frame) {
			// Kept now, while the code is at hand; the top of the loop
			// reports the ending.
			if (code != CURLE_OK) {
				spudnet_curl_failed(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], code, SPUDRESULT_SPUDNET_RECV_FAILED, NULL);
				socket->errors[SPUDNET_ERROR_SIDE_RECEIVING].result = SPUDRESULT_SPUDNET_RECV_FAILED;
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
				spudnet_curl_send_frame(socket, echo, socket->close_code ? 2 : 0, CURLWS_CLOSE, start, SPUDNET_NO_WAIT);
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
			spudnet_curl_send_frame(socket, too_big, sizeof(too_big), CURLWS_CLOSE, start, SPUDNET_NO_WAIT);
			socket->too_big  = true;
			socket->finished = true;
			continue;
		}
		socket->message_size += count;

		*out_received = count;
		*out_type     = (frame->flags & CURLWS_TEXT) ? SPUDNET_WEBSOCKET_MESSAGE_TEXT : SPUDNET_WEBSOCKET_MESSAGE_BINARY;
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

SPUDRESULT spudnet_websocket_close(
    spudnet_websocket socket,
    uint16_t code,
    const char *reason) {
	if (!socket)
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	if (SPUD_STALE(socket, "spudnet_websocket_close", "socket"))
		return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	size_t reason_length = reason ? strlen(reason) : 0;
	if (reason_length > 123)
		return SPUDRESULT_SPUDNET_SEND_FAILED;

	uint8_t payload[2 + 123];
	payload[0] = (uint8_t)(code >> 8);
	payload[1] = (uint8_t)(code & 0xFF);
	if (reason_length > 0)
		memcpy(payload + 2, reason, reason_length);

	pthread_mutex_lock(&socket->lock);
	SPUDRESULT result = spudnet_curl_websocket_usable(socket);
	if (!SPUDFAIL(result) && socket->finished)
		result = SPUDRESULT_SPUDNET_SEND_FAILED;
	if (!SPUDFAIL(result)) {
		// Not waited for: a close frame that won't go out now is a
		// connection that isn't closing politely anyway.
		switch (spudnet_curl_send_frame(socket, payload, 2 + reason_length, CURLWS_CLOSE, 0, SPUDNET_NO_WAIT)) {
		case SPUDNET_CURL_SENT: {
			// The end of the connection for this side. libcurl would go on
			// receiving until the peer answers; spudnet.h has every stack
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
		case SPUDNET_CURL_SEND_FAILED:
			spudnet_curl_failed(&socket->errors[SPUDNET_ERROR_SIDE_SENDING], socket->send_code, SPUDRESULT_SPUDNET_SEND_FAILED, NULL);
			socket->errors[SPUDNET_ERROR_SIDE_SENDING].result = SPUDRESULT_SPUDNET_SEND_FAILED;
			result                                            = SPUDRESULT_SPUDNET_SEND_FAILED;
			break;
		default:
			// No room for the frame right now: libcurl has no complaint.
			result = SPUDRESULT_SPUDNET_SEND_FAILED;
			break;
		}
	}
	pthread_mutex_unlock(&socket->lock);
	return result;
}

uint16_t spudnet_websocket_get_close_code(spudnet_websocket socket) {
	if (!socket)
		return 0;
	if (SPUD_STALE(socket, "spudnet_websocket_get_close_code", "socket"))
		return 0;
	pthread_mutex_lock(&socket->lock);
	// Only a close the peer made has a code to report.
	uint16_t code = (socket->peer_closed && !socket->close_requested) ? socket->close_code : 0;
	pthread_mutex_unlock(&socket->lock);
	return code;
}

#else // libcurl older than 8.11

SPUDRESULT spudnet_websocket_create(
    spudnet_instance instance,
    spudnet_websocket *out_socket) {
	if (!out_socket)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudnet_websocket_create", "instance"))
		return SPUDRESULT_SPUDNET_INVALID_INSTANCE;
	return SPUDRESULT_SPUDNET_UNSUPPORTED;
}

spudnet_instance spudnet_websocket_instance(spudnet_websocket socket) {
	(void)socket;
	return NULL;
}

// With no way to make a socket, the rest can only be handed NULL.

SPUDRESULT spudnet_websocket_connect(
    spudnet_websocket socket,
    const spudnet_websocket_connect_desc *desc,
    uint32_t *out_http_status) {
	(void)socket;
	(void)desc;
	if (out_http_status)
		*out_http_status = 0;
	return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
}

SPUDRESULT spudnet_websocket_send(
    spudnet_websocket socket,
    SPUDNET_WEBSOCKET_MESSAGE type,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms) {
	(void)socket;
	(void)type;
	(void)data;
	(void)size;
	(void)timeout_ms;
	return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
}

SPUDRESULT spudnet_websocket_recv(
    spudnet_websocket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received,
    SPUDNET_WEBSOCKET_MESSAGE *out_type,
    bool *out_message_complete) {
	(void)socket;
	(void)data;
	(void)size;
	(void)timeout_ms;
	if (out_received)
		*out_received = 0;
	if (out_type)
		*out_type = SPUDNET_WEBSOCKET_MESSAGE_BINARY;
	if (out_message_complete)
		*out_message_complete = false;
	return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
}

SPUDRESULT spudnet_websocket_close(
    spudnet_websocket socket,
    uint16_t code,
    const char *reason) {
	(void)socket;
	(void)code;
	(void)reason;
	return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
}

uint16_t spudnet_websocket_get_close_code(spudnet_websocket socket) {
	(void)socket;
	return 0;
}

void spudnet_websocket_abort(spudnet_websocket socket) { (void)socket; }

void spudnet_websocket_get_error(
    spudnet_websocket socket,
    SPUDNET_ERROR_SIDE side,
    spudnet_error *out_error) {
	(void)socket;
	(void)side;
	spudnet_error_get(NULL, out_error);
}

// A wait set can't be handed a socket that can't be made either.

SPUDRESULT spudnet_websocket_wait_attach(
    spudnet_websocket socket,
    struct spudnet_wait_set_t *set,
    bool *out_has_native,
    intptr_t *out_native,
    bool *out_report_once) {
	(void)socket;
	(void)set;
	(void)out_has_native;
	(void)out_native;
	(void)out_report_once;
	return SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
}

void spudnet_websocket_wait_detach(spudnet_websocket socket) { (void)socket; }

bool spudnet_websocket_wait_ready(spudnet_websocket socket) {
	(void)socket;
	return false;
}

void spudnet_websocket_destroy(spudnet_websocket socket) { (void)socket; }

#endif // LIBCURL_VERSION_NUM

#if __cplusplus
}
#endif

#endif // SPUDLIB_PLATFORM_LINUX
