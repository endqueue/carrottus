
#if SPUDLIB_PLATFORM_WIN32

/*
 * SpudNet HTTP and WebSocket on WinHTTP.
 *
 * Both use WinHTTP's asynchronous mode. Its synchronous mode can't keep the
 * promises spudnet.h makes: it times each stage of a request on its own
 * rather than the whole call, a blocked call can't be cancelled from
 * another thread, and a WebSocket receive that times out leaves the
 * connection unusable.
 *
 * So WinHTTP reports on its own threads through a status callback, the
 * callback records the outcome and sets an event, and the calling thread
 * waits on that event - together with the cancel or abort event - for as
 * long as the caller's time limit allows. That wait is the only timer;
 * WinHTTP's own are all set to "no limit".
 *
 * Two rules follow from WinHTTP still holding pointers after a wait gives
 * up. A buffer WinHTTP may still read or write is never the caller's
 * unless the call stays until WinHTTP has let go of it. And a struct that
 * is a handle's callback context is never freed, or reused for another
 * request, before WinHTTP has reported that handle closing.
 *
 * TLS: WinHTTP checks a server certificate against the system store and
 * has switches to stop checking, but none to check against something else.
 * So what a spudnet_tls_desc asks beyond the switches - the caller's own
 * roots, pins - is checked here with CryptoAPI, in the status callback
 * WinHTTP makes once the connection is up and before the request goes out
 * on it (SENDING_REQUEST), where closing the handle still keeps the request
 * from being sent.
 */

#include "../../spudnetshared.h"
#include "spudnet.h"
#include "spudnetwinhttphead.h"
#include "../../../spudstructbase.h"

/* windows.h without the legacy winsock.h, which conflicts with the
 * winsock2.h spudnetwindows.c uses if both land in one translation unit. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
// Left out of windows.h by WIN32_LEAN_AND_MEAN.
#include <bcrypt.h>
#include <wincrypt.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Shared by HTTP and WebSocket
// --------------------------------------------------------------------------

/* What a wait in this file reports when the caller's own time ran out, to
 * the code that turns Win32 errors into results: a value neither WinHTTP
 * nor Windows produces (bit 29 marks a code as an application's own), so
 * that it can never be taken for a time-out of WinHTTP's, nor the other
 * way round. */
#define SPUDNET_WIN_OWN_TIME_OUT ((DWORD)0xE0000001)

static SPUDRESULT spudnet_win_result(DWORD error, SPUDRESULT otherwise) {
	switch (error) {
	case SPUDNET_WIN_OWN_TIME_OUT:
		return SPUDRESULT_SPUDNET_TIMED_OUT;
	case ERROR_WINHTTP_TIMEOUT:
		// WinHTTP's own timers are switched off here, so this is one it
		// was handed from underneath.
		return SPUDRESULT_SPUDNET_STACK_TIMED_OUT;
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		return SPUDRESULT_SPUDNET_RESOLVE_FAILED;
	case ERROR_WINHTTP_CANNOT_CONNECT:
		return SPUDRESULT_SPUDNET_CONNECT_FAILED;
	// ERROR_WINHTTP_CONNECTION_ERROR is a connection that was made and then
	// broke, which is the calling part's catch-all, not a failure to connect.
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
		return SPUDRESULT_SPUDNET_TLS_FAILED;
	case ERROR_WINHTTP_INVALID_URL:
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
		return SPUDRESULT_SPUDNET_INVALID_URL;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return SPUDRESULT_OUT_OF_MEMORY;
	default:
		return otherwise;
	}
}

/* A failed WinHTTP call as a record, and the SPUDRESULT it comes to
 * (`otherwise` where spudnet_win_result has nothing closer). */
static SPUDRESULT spudnet_win_failed(spudnet_error *record, DWORD error, SPUDRESULT otherwise) {
	return spudnet_error_record(record, spudnet_win_result(error, otherwise), SPUD_PLATFORM_ERROR_SOURCE_WIN32, error, NULL);
}

/* UTF-8 to a NUL-terminated wide string made through `allocator`, of
 * `*out_count` wchar_ts (the terminator included), which the caller frees
 * with spudnet_win_wide_free and that count. NULL, and a count of 0, when
 * the bytes aren't UTF-8 or memory ran out. */
static wchar_t *spudnet_win_wide(const SPUD_ALLOCATOR *allocator, const char *text, size_t *out_count) {
	*out_count = 0;
	int count  = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
	if (count <= 0)
		return NULL;
	wchar_t *wide = SPUD_NEW_ARRAY(allocator, wchar_t, (size_t)count);
	if (!wide)
		return NULL;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, count) <= 0) {
		SPUD_DELETE_ARRAY(allocator, wchar_t, wide, (size_t)count);
		return NULL;
	}
	*out_count = (size_t)count;
	return wide;
}

static void spudnet_win_wide_free(const SPUD_ALLOCATOR *allocator, wchar_t *wide, size_t count) {
	SPUD_DELETE_ARRAY(allocator, wchar_t, wide, count);
}

struct spudnet_win_url {
	wchar_t      *host;
	size_t        host_count; // the wchar_ts `host` was allocated with
	wchar_t      *path;       // path and query, never empty
	size_t        path_count; // the wchar_ts `path` was allocated with
	INTERNET_PORT port;
	bool          secure;
};

static void spudnet_win_url_free(const SPUD_ALLOCATOR *allocator, struct spudnet_win_url *url) {
	SPUD_DELETE_ARRAY(allocator, wchar_t, url->host, url->host_count);
	SPUD_DELETE_ARRAY(allocator, wchar_t, url->path, url->path_count);
	url->host       = NULL;
	url->host_count = 0;
	url->path       = NULL;
	url->path_count = 0;
}

/* Splits `url` into what WinHttpConnect and WinHttpOpenRequest each take,
 * made through `allocator`; the caller frees `out_url` with
 * spudnet_win_url_free whatever this returned. WinHttpCrackUrl only knows
 * http and https, so a ws/wss URL is cracked as the http/https one it rides
 * on. */
static SPUDRESULT spudnet_win_crack(const SPUD_ALLOCATOR *allocator, const char *url, bool websocket, struct spudnet_win_url *out_url) {
	memset(out_url, 0, sizeof(*out_url));

	wchar_t *wide       = NULL;
	size_t   wide_count = 0;
	if (websocket) {
		bool        secure        = spudnet_url_has_scheme(url, "wss");
		const char *scheme        = secure ? "https" : "http";
		size_t      scheme_length = strlen(scheme);
		const char *rest          = url + (secure ? 3 : 2); // from "://" on
		size_t      rest_length   = strlen(rest);
		size_t      as_http_size  = scheme_length + rest_length + 1;
		char       *as_http       = (char *)spud_allocate(allocator, as_http_size, 1);
		if (!as_http)
			return SPUDRESULT_OUT_OF_MEMORY;
		memcpy(as_http, scheme, scheme_length);
		memcpy(as_http + scheme_length, rest, rest_length + 1);
		wide = spudnet_win_wide(allocator, as_http, &wide_count);
		spud_free(allocator, as_http, as_http_size, 1);
	} else {
		wide = spudnet_win_wide(allocator, url, &wide_count);
	}
	if (!wide)
		return SPUDRESULT_SPUDNET_INVALID_URL;

	// Asking for lengths only makes WinHttpCrackUrl point into `wide`
	// rather than copy.
	URL_COMPONENTS parts;
	memset(&parts, 0, sizeof(parts));
	parts.dwStructSize      = sizeof(parts);
	parts.dwSchemeLength    = (DWORD)-1;
	parts.dwHostNameLength  = (DWORD)-1;
	parts.dwUrlPathLength   = (DWORD)-1;
	parts.dwExtraInfoLength = (DWORD)-1;
	if (!WinHttpCrackUrl(wide, 0, 0, &parts) || parts.dwHostNameLength == 0) {
		spudnet_win_wide_free(allocator, wide, wide_count);
		return SPUDRESULT_SPUDNET_INVALID_URL;
	}

	// The query rides with the path; a fragment never goes to a server.
	DWORD extra_length = 0;
	while (extra_length < parts.dwExtraInfoLength && parts.lpszExtraInfo[extra_length] != L'#')
		extra_length++;

	size_t path_length = (size_t)parts.dwUrlPathLength + extra_length;
	out_url->host      = SPUD_NEW_ARRAY(allocator, wchar_t, (size_t)parts.dwHostNameLength + 1);
	if (out_url->host)
		out_url->host_count = (size_t)parts.dwHostNameLength + 1;
	out_url->path = SPUD_NEW_ARRAY(allocator, wchar_t, path_length + 2);
	if (out_url->path)
		out_url->path_count = path_length + 2;
	if (!out_url->host || !out_url->path) {
		spudnet_win_url_free(allocator, out_url);
		spudnet_win_wide_free(allocator, wide, wide_count);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(out_url->host, parts.lpszHostName, parts.dwHostNameLength * sizeof(wchar_t));
	out_url->host[parts.dwHostNameLength] = L'\0';

	size_t at = 0;
	if (parts.dwUrlPathLength == 0)
		out_url->path[at++] = L'/';
	else {
		memcpy(out_url->path, parts.lpszUrlPath, parts.dwUrlPathLength * sizeof(wchar_t));
		at = parts.dwUrlPathLength;
	}
	if (extra_length > 0) {
		memcpy(out_url->path + at, parts.lpszExtraInfo, extra_length * sizeof(wchar_t));
		at += extra_length;
	}
	out_url->path[at] = L'\0';

	out_url->port   = parts.nPort;
	out_url->secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
	spudnet_win_wide_free(allocator, wide, wide_count);
	return SPUD_SUCCESS;
}

/* Adds the caller's headers to a request. What is put together on the way
 * is made and freed through `allocator`. */
static SPUDRESULT spudnet_win_add_headers(const SPUD_ALLOCATOR *allocator, HINTERNET request, const spudnet_header *headers, uint32_t header_count) {
	if (header_count == 0)
		return SPUD_SUCCESS;

	size_t length = 0;
	for (uint32_t i = 0; i < header_count; ++i) {
		if (!headers[i].name || !headers[i].value)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		length += strlen(headers[i].name) + 2 + strlen(headers[i].value) + 2;
	}
	char *block = (char *)spud_allocate(allocator, length + 1, 1);
	if (!block)
		return SPUDRESULT_OUT_OF_MEMORY;
	size_t at = 0;
	for (uint32_t i = 0; i < header_count; ++i) {
		size_t name_length  = strlen(headers[i].name);
		size_t value_length = strlen(headers[i].value);
		memcpy(block + at, headers[i].name, name_length);
		at += name_length;
		memcpy(block + at, ": ", 2);
		at += 2;
		memcpy(block + at, headers[i].value, value_length);
		at += value_length;
		memcpy(block + at, "\r\n", 2);
		at += 2;
	}
	block[at]         = '\0';
	size_t   wide_count = 0;
	wchar_t *wide       = spudnet_win_wide(allocator, block, &wide_count);
	spud_free(allocator, block, length + 1, 1);
	if (!wide)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	BOOL added = WinHttpAddRequestHeaders(request, wide, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
	spudnet_win_wide_free(allocator, wide, wide_count);
	return added ? SPUD_SUCCESS : SPUDRESULT_DESC_INVALID_PARAMETERS;
}

/* Opens a session that reaches servers the way `proxy` says. NULL on
 * failure. The proxy's name is put together through `allocator`. */
static HINTERNET spudnet_win_open_session(const SPUD_ALLOCATOR *allocator, const struct spudnet_proxy *proxy) {
	// No agent string: User-Agent is the caller's header to send or not.
	if (proxy->mode == SPUDNET_PROXY_NONE)
		return WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
	if (proxy->mode != SPUDNET_PROXY_EXPLICIT)
		return WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);

	// WinHTTP takes a named proxy as "host:port", an IPv6 host in brackets.
	size_t size = strlen(proxy->host) + 16;
	char  *name = (char *)spud_allocate(allocator, size, 1);
	if (!name)
		return NULL;
	snprintf(name, size, proxy->host_is_ipv6 ? "[%s]:%u" : "%s:%u", proxy->host, (unsigned)proxy->port);
	size_t   wide_count = 0;
	wchar_t *wide       = spudnet_win_wide(allocator, name, &wide_count);
	spud_free(allocator, name, size, 1);
	if (!wide)
		return NULL;
	HINTERNET session = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_NAMED_PROXY, wide, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
	spudnet_win_wide_free(allocator, wide, wide_count);
	return session;
}

/* What WinHTTP would otherwise keep or decide between requests, switched
 * off: its cookie jar and its automatic answers to authentication
 * challenges. */
static void spudnet_win_disable_features(HINTERNET request) {
	DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
	WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
}

// --------------------------------------------------------------------------
// TLS
// --------------------------------------------------------------------------

/* A spudnet_tls_desc in the forms WinHTTP and CryptoAPI take it. Not
 * changed after it is made, so WinHTTP's threads read it without a lock. */
struct spudnet_win_tls {
	SPUDNET_TLS_TRUST trust;
	bool              skip_host_name_check;
	HCERTSTORE        roots;  // the caller's roots, in memory; or NULL
	HCERTCHAINENGINE  engine; // builds chains that may only end in `roots`
	spudnet_tls_pin  *pins;
	uint32_t          pin_count;
};

/* `pins` was allocated with `pin_count` entries, through the allocator both
 * of these are given. */
static void spudnet_win_tls_free(const SPUD_ALLOCATOR *allocator, struct spudnet_win_tls *tls) {
	if (tls->engine)
		CertFreeCertificateChainEngine(tls->engine);
	if (tls->roots)
		CertCloseStore(tls->roots, 0);
	SPUD_DELETE_ARRAY(allocator, spudnet_tls_pin, tls->pins, tls->pin_count);
	memset(tls, 0, sizeof(*tls));
}

/* Fills `out_tls`, which the caller frees with spudnet_win_tls_free and the
 * same allocator. */
static SPUDRESULT spudnet_win_tls_create(const SPUD_ALLOCATOR *allocator, const spudnet_tls_desc *desc, struct spudnet_win_tls *out_tls) {
	memset(out_tls, 0, sizeof(*out_tls));
	SPUDRESULT result = spudnet_tls_desc_check(desc);
	if (SPUDFAIL(result))
		return result;
	out_tls->trust                = desc->trust;
	out_tls->skip_host_name_check = desc->skip_host_name_check;

	if (desc->root_count > 0) {
		out_tls->roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
		if (!out_tls->roots)
			return SPUDRESULT_OUT_OF_MEMORY;
		for (uint32_t i = 0; i < desc->root_count; ++i) {
			// Fails for bytes that aren't a DER certificate.
			const BYTE *der      = (const BYTE *)desc->roots[i].data;
			uint64_t    der_size = desc->roots[i].size;
			if (der_size > MAXDWORD ||
			    !CertAddEncodedCertificateToStore(out_tls->roots, X509_ASN_ENCODING, der, (DWORD)der_size, CERT_STORE_ADD_USE_EXISTING, NULL)) {
				spudnet_win_tls_free(allocator, out_tls);
				return SPUDRESULT_DESC_INVALID_PARAMETERS;
			}
		}
		CERT_CHAIN_ENGINE_CONFIG config;
		memset(&config, 0, sizeof(config));
		config.cbSize         = sizeof(config);
		config.hExclusiveRoot = out_tls->roots;
		if (!CertCreateCertificateChainEngine(&config, &out_tls->engine)) {
			spudnet_win_tls_free(allocator, out_tls);
			return SPUDRESULT_GENERAL_FAILURE;
		}
	}

	if (desc->pin_count > 0) {
		out_tls->pins = SPUD_NEW_ARRAY(allocator, spudnet_tls_pin, desc->pin_count);
		if (!out_tls->pins) {
			spudnet_win_tls_free(allocator, out_tls);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		out_tls->pin_count = desc->pin_count; // also the count it is freed with
		memcpy(out_tls->pins, desc->pins, (size_t)desc->pin_count * sizeof(spudnet_tls_pin));
	}
	return SPUD_SUCCESS;
}

/* The part of `tls` WinHTTP has a switch for, set on a request before it is
 * sent. With the caller's own roots in play WinHTTP is told not to mind a
 * root it doesn't know, and spudnet_win_tls_accepts does the minding.
 * False when a switch that was needed couldn't be set. */
static bool spudnet_win_tls_apply(HINTERNET request, const struct spudnet_win_tls *tls, bool secure) {
	DWORD flags = 0;
	if (tls->skip_host_name_check)
		flags |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	if (tls->roots)
		flags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA;
	if (tls->trust == SPUDNET_TLS_TRUST_ANY)
		flags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
	if (flags == 0)
		return true;
	// A request that starts as plain http may not take them; it then has
	// WinHTTP's full checks if a redirect ever turns it into https.
	return WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags)) || !secure;
}

/* Whether a chain can be built from `certificate` to a root `engine`
 * accepts (NULL for the system's own engine and roots) that is fit for a
 * TLS server: ERROR_SUCCESS, or what CryptoAPI had against it (a CERT_E_*
 * or TRUST_E_* code). The name is left out: WinHTTP checks it itself unless
 * told not to, and knows which host a redirect has moved the request to. */
static DWORD spudnet_win_chain_error(HCERTCHAINENGINE engine, PCCERT_CONTEXT certificate) {
	char            server_usage[] = szOID_PKIX_KP_SERVER_AUTH;
	LPSTR           usages[1]      = {server_usage};
	CERT_CHAIN_PARA parameters;
	memset(&parameters, 0, sizeof(parameters));
	parameters.cbSize                                    = sizeof(parameters);
	parameters.RequestedUsage.dwType                     = USAGE_MATCH_TYPE_OR;
	parameters.RequestedUsage.Usage.cUsageIdentifier     = 1;
	parameters.RequestedUsage.Usage.rgpszUsageIdentifier = usages;

	// The certificate's own store holds the intermediates the server sent.
	PCCERT_CHAIN_CONTEXT chain = NULL;
	if (!CertGetCertificateChain(engine, certificate, NULL, certificate->hCertStore, &parameters, 0, NULL, &chain))
		return GetLastError() ? GetLastError() : (DWORD)CERT_E_CHAINING;

	SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl;
	memset(&ssl, 0, sizeof(ssl));
	ssl.cbSize     = sizeof(ssl);
	ssl.dwAuthType = AUTHTYPE_SERVER;
	ssl.fdwChecks  = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	CERT_CHAIN_POLICY_PARA policy;
	memset(&policy, 0, sizeof(policy));
	policy.cbSize            = sizeof(policy);
	policy.pvExtraPolicyPara = &ssl;
	CERT_CHAIN_POLICY_STATUS status;
	memset(&status, 0, sizeof(status));
	status.cbSize = sizeof(status);

	// FALSE only when the policy itself couldn't be run.
	DWORD error = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) ? status.dwError : (DWORD)CERT_E_CHAINING;
	CertFreeCertificateChain(chain);
	return error;
}

static bool spudnet_win_pin_matches(const struct spudnet_win_tls *tls, PCCERT_CONTEXT certificate) {
	const uint8_t *info      = NULL;
	size_t         info_size = 0;
	if (!spudnet_certificate_public_key_info(certificate->pbCertEncoded, certificate->cbCertEncoded, &info, &info_size))
		return false;

	// Despite its name this hashes whatever bytes it is given.
	BYTE  digest[SPUDNET_TLS_PIN_SIZE];
	DWORD digest_size = sizeof(digest);
	if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, NULL, info, (DWORD)info_size, digest, &digest_size) || digest_size != sizeof(digest))
		return false;
	for (uint32_t i = 0; i < tls->pin_count; ++i) {
		if (memcmp(digest, tls->pins[i].sha256, sizeof(digest)) == 0)
			return true;
	}
	return false;
}

/* Called from the status callback when `request` is about to be sent on a
 * connection that is up: whether the certificate the server presented
 * passes what `tls` asks beyond WinHTTP's own switches. A request that
 * isn't going over TLS has no certificate and nothing to check. When the
 * answer is no, `out_reason` is the record of why. */
static bool spudnet_win_tls_accepts(HINTERNET request, const struct spudnet_win_tls *tls, spudnet_error *out_reason) {
	if (!tls->roots && tls->pin_count == 0)
		return true;

	PCCERT_CONTEXT certificate = NULL;
	DWORD          size        = sizeof(certificate);
	if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &size) || !certificate) {
		DWORD error = GetLastError();
		if (error == ERROR_WINHTTP_INCORRECT_HANDLE_STATE)
			return true; // what a plain connection answers
		spudnet_error_record(out_reason, SPUDRESULT_SPUDNET_TLS_FAILED, SPUD_PLATFORM_ERROR_SOURCE_WIN32, error, NULL);
		return false;
	}

	bool accepted = true;
	if (tls->roots) {
		DWORD error = spudnet_win_chain_error(tls->engine, certificate);
		if (error != ERROR_SUCCESS && tls->trust == SPUDNET_TLS_TRUST_SYSTEM_AND_ROOTS)
			error = spudnet_win_chain_error(NULL, certificate);
		if (error != ERROR_SUCCESS) {
			spudnet_error_record(out_reason, SPUDRESULT_SPUDNET_TLS_FAILED, SPUD_PLATFORM_ERROR_SOURCE_WIN32, error, NULL);
			accepted = false;
		}
	}
	if (accepted && tls->pin_count > 0 && !spudnet_win_pin_matches(tls, certificate)) {
		spudnet_error_record(out_reason, SPUDRESULT_SPUDNET_TLS_FAILED, SPUD_PLATFORM_ERROR_SOURCE_TLS_PIN, 0, NULL);
		accepted = false;
	}
	CertFreeCertificateContext(certificate);
	return accepted;
}

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------

/* WinHTTP already moves a request the way spudnet.h asks: send the request
 * line and headers, write the body in pieces, ask for the response, read
 * its body in pieces. Each of those is started here and completes in the
 * status callback, on one of WinHTTP's threads, which sets an event the
 * calling thread waits on beside the abort event.
 *
 * One operation is in flight on a request at a time, and a call that runs
 * out of time leaves its operation in flight for the next call to pick up.
 * That is why no buffer WinHTTP is given is ever the caller's: request body
 * is copied into the transfer's own buffer before it is written, and
 * response body is read into the transfer's own and copied out. */

// How much request body is handed to WinHTTP at a time, and how much
// response body it is asked for at a time.
#define SPUDNET_WIN_HTTP_BUFFER_SIZE (64 * 1024)
// Room for what goes round a piece of an unsized body: its length in hex
// and a line ending before it, a line ending after.
#define SPUDNET_WIN_HTTP_CHUNK_FRAME 32

struct spudnet_http_client_t {
	spud_object_head head; // first: spudnethttp.c reaches the allocator through it
	// Holds the connections. Its proxy was settled when it was opened.
	HINTERNET              session;
	struct spudnet_win_tls tls;
};

/* The backend's side of a spudnet_http_transfer_t, and the callback context
 * of its request handle - so it is freed only after WinHTTP has reported
 * that handle closing. */
struct spudnet_win_http {
	struct spudnet_http_transfer_t *owner;
	struct spudnet_http_client_t   *client;

	HINTERNET connect;
	HINTERNET request;
	bool      has_context; // WinHTTP calls back for `request` from here on
	// The request handle is closed by whichever gets there first - the
	// callback turning a certificate away, an abort, or destroy - and this
	// is how the others know not to.
	LONG request_closing;

	HANDLE        abort_event; // set by abort and never reset
	volatile LONG aborted;
	HANDLE        closed_event; // WinHTTP is done with the request handle

	// The operation in flight. The callback writes the outcome and sets
	// the event; the calling thread reads the outcome after the event.
	bool   operation_pending;
	HANDLE step_event;
	DWORD  step_error; // 0 for none
	DWORD  step_bytes; // what a finished read delivered
	// The request line and headers are out. WinHTTP may hold the send's
	// completion back until the response arrives, so start waits for this
	// instead and leaves the completion in flight.
	HANDLE sent_event;
	// The callback turned the server's certificate away and closed the
	// request, and why.
	bool          tls_refused;
	spudnet_error tls_reason;

	// The request body goes through here on its way to WinHTTP.
	uint8_t *write_buffer;
	bool     final_chunk_written; // an unsized body's closing chunk
	bool     response_asked;      // WinHttpReceiveResponse has been called

	// The response body comes through here on its way out.
	uint8_t *read_buffer;
	DWORD    read_size;
	DWORD    read_offset;
	bool     read_pending; // the operation in flight is a read
	bool     body_ended;
};

static void CALLBACK spudnet_win_http_callback(
    HINTERNET handle,
    DWORD_PTR context,
    DWORD status,
    LPVOID information,
    DWORD information_length) {
	struct spudnet_win_http *http = (struct spudnet_win_http *)context;
	if (!http)
		return; // the session and connect handles carry no context

	switch (status) {
	case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
		// Closing the handle here, rather than leaving it to the waiting
		// thread, is what keeps the request from going out to a server whose
		// certificate was turned away. A redirect comes through here again
		// for the server it leads to.
		if (!spudnet_win_tls_accepts(handle, &http->client->tls, &http->tls_reason)) {
			http->tls_refused = true;
			if (InterlockedExchange(&http->request_closing, 1) == 0)
				WinHttpCloseHandle(handle);
			http->step_error = ERROR_WINHTTP_SECURE_FAILURE;
			SetEvent(http->step_event);
		}
		break;

	case WINHTTP_CALLBACK_STATUS_REQUEST_SENT:
		SetEvent(http->sent_event);
		break;
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
	case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
		http->step_error = ERROR_SUCCESS;
		SetEvent(http->step_event);
		break;

	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
		http->step_error = ERROR_SUCCESS;
		http->step_bytes = information_length;
		SetEvent(http->step_event);
		break;

	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
		const WINHTTP_ASYNC_RESULT *failed = (const WINHTTP_ASYNC_RESULT *)information;
		http->step_error                   = failed->dwError ? failed->dwError : ERROR_WINHTTP_CONNECTION_ERROR;
		SetEvent(http->step_event);
		break;
	}

	case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
		SetEvent(http->closed_event);
		break;

	default:
		break;
	}
}

void spudnet_http_client_destroy(spudnet_http_client client) {
	if (!client)
		return;
	if (SPUD_STALE(client, "spudnet_http_client_destroy", "client"))
		return;
	if (client->session)
		WinHttpCloseHandle(client->session);
	SPUD_KILL(client);
	const SPUD_ALLOCATOR allocator = client->head.allocator;
	spudnet_win_tls_free(&allocator, &client->tls);
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

	const SPUD_ALLOCATOR *allocator = &instance->head.allocator;
	struct spudnet_proxy  proxy;
	SPUDRESULT            result = spudnet_proxy_parse(allocator, &desc->proxy, &proxy);
	if (SPUDFAIL(result)) {
		spudnet_proxy_free(allocator, &proxy);
		return result;
	}
	struct spudnet_http_client_t *client = SPUD_NEW_ZEROED(allocator, struct spudnet_http_client_t);
	if (!client) {
		spudnet_proxy_free(allocator, &proxy);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	client->head.allocator = *allocator;
	SPUD_BIRTH_IN(client, &instance->quarantine);
	result                 = spudnet_win_tls_create(allocator, &desc->tls, &client->tls);
	if (!SPUDFAIL(result)) {
		client->session = spudnet_win_open_session(allocator, &proxy);
		// Set on the session before any other handle exists, so each
		// inherits it.
		if (!client->session ||
		    WinHttpSetStatusCallback(client->session, spudnet_win_http_callback,
		                             WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0) ==
		        WINHTTP_INVALID_STATUS_CALLBACK)
			result = SPUDRESULT_GENERAL_FAILURE;
	}
	spudnet_proxy_free(allocator, &proxy);
	if (SPUDFAIL(result)) {
		spudnet_http_client_destroy(client);
		return result;
	}

	*out_client = client;
	return SPUD_SUCCESS;
}

/* Milliseconds left of `timeout_ms` since `start`, for a Win32 wait. */
static DWORD spudnet_win_remaining(ULONGLONG start, uint32_t timeout_ms) {
	if (timeout_ms == SPUDNET_WAIT_FOREVER)
		return INFINITE;
	ULONGLONG elapsed = GetTickCount64() - start;
	if (elapsed >= timeout_ms)
		return 0;
	ULONGLONG left = timeout_ms - elapsed;
	return left >= INFINITE ? INFINITE - 1 : (DWORD)left;
}

/* The record and the result for something WinHTTP reported, `error` being
 * its code. A certificate the callback turned away, and an abort, both
 * close the request under WinHTTP, which then reports whatever it makes of
 * that; they are told apart here by what was done, not by its code. */
static SPUDRESULT spudnet_win_http_failed(struct spudnet_win_http *http, DWORD error, SPUDRESULT otherwise) {
	if (InterlockedCompareExchange(&http->aborted, 0, 0) != 0)
		return SPUDRESULT_SPUDNET_ABORTED;
	if (http->tls_refused) {
		http->owner->error = http->tls_reason;
		return SPUDRESULT_SPUDNET_TLS_FAILED;
	}
	return spudnet_win_failed(&http->owner->error, error, otherwise);
}

/* Waits for the operation in flight, if there is one. SPUD_SUCCESS when
 * none is left; SPUDRESULT_SPUDNET_TIMED_OUT with it still in flight, for a
 * later call to wait on again; or how it failed, `otherwise` being the
 * result for a failure WinHTTP's code says nothing closer about. */
static SPUDRESULT spudnet_win_http_wait(struct spudnet_win_http *http, ULONGLONG start, uint32_t timeout_ms, SPUDRESULT otherwise) {
	if (!http->operation_pending)
		return SPUD_SUCCESS;
	// Abort is listed first so it wins when both are set.
	HANDLE waits[2] = {http->abort_event, http->step_event};
	DWORD  waited   = WaitForMultipleObjects(2, waits, FALSE, spudnet_win_remaining(start, timeout_ms));
	if (waited == WAIT_OBJECT_0)
		return SPUDRESULT_SPUDNET_ABORTED;
	if (waited != WAIT_OBJECT_0 + 1)
		return SPUDRESULT_SPUDNET_TIMED_OUT;
	http->operation_pending = false;
	if (http->step_error == ERROR_SUCCESS)
		return SPUD_SUCCESS;
	return spudnet_win_http_failed(http, http->step_error, otherwise);
}

/* Waits for WinHttpSendRequest to have put the request line and headers on
 * the wire. Its completion is left in flight when it hasn't come yet, for
 * the next call to wait on: WinHTTP may hold that back until the response
 * arrives, and start must not wait for the response. */
static SPUDRESULT spudnet_win_http_wait_sent(struct spudnet_win_http *http, ULONGLONG start, uint32_t timeout_ms) {
	// Abort is listed first so it wins, and the completion before sent so a
	// failure is reported here rather than by the next call.
	HANDLE waits[3] = {http->abort_event, http->step_event, http->sent_event};
	DWORD  waited   = WaitForMultipleObjects(3, waits, FALSE, spudnet_win_remaining(start, timeout_ms));
	if (waited == WAIT_OBJECT_0)
		return SPUDRESULT_SPUDNET_ABORTED;
	if (waited == WAIT_OBJECT_0 + 2)
		return SPUD_SUCCESS;
	if (waited != WAIT_OBJECT_0 + 1)
		return SPUDRESULT_SPUDNET_TIMED_OUT;
	http->operation_pending = false;
	if (http->step_error == ERROR_SUCCESS)
		return SPUD_SUCCESS;
	return spudnet_win_http_failed(http, http->step_error, SPUDRESULT_SPUDNET_HTTP_FAILED);
}

/* Status and headers of a request whose response has started arriving.
 * These are already in WinHTTP's hands, so nothing here waits. Called on
 * the caller's thread, from receive_response; everything is made and freed
 * through `allocator`, the transfer's. */
static SPUDRESULT spudnet_win_read_headers(const SPUD_ALLOCATOR *allocator, HINTERNET request, struct spudnet_http_head *head) {
	DWORD status      = 0;
	DWORD status_size = sizeof(status);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
	                         WINHTTP_NO_HEADER_INDEX))
		return SPUDRESULT_SPUDNET_HTTP_FAILED;
	head->status = status;

	// The first call only reports the size, by failing.
	DWORD raw_size = 0;
	WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &raw_size, WINHTTP_NO_HEADER_INDEX);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || raw_size == 0)
		return SPUDRESULT_SPUDNET_HTTP_FAILED;
	// The query below rewrites `raw_size` with what it copied, so the size
	// the block is freed with is kept apart.
	const DWORD raw_capacity = raw_size;
	wchar_t    *raw          = (wchar_t *)spud_allocate(allocator, raw_capacity, SPUD_ALIGNOF(wchar_t));
	if (!raw)
		return SPUDRESULT_OUT_OF_MEMORY;
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw, &raw_size, WINHTTP_NO_HEADER_INDEX)) {
		spud_free(allocator, raw, raw_capacity, SPUD_ALIGNOF(wchar_t));
		return SPUDRESULT_SPUDNET_HTTP_FAILED;
	}
	const SPUDRESULT added = spudnet_win_add_raw_headers(allocator, raw, raw_size / sizeof(wchar_t), head);
	spud_free(allocator, raw, raw_capacity, SPUD_ALIGNOF(wchar_t));
	return added;
}

SPUDRESULT spudnet_win_add_raw_headers(const SPUD_ALLOCATOR *allocator, const wchar_t *raw, size_t wide_count, struct spudnet_http_head *head) {
	if (wide_count == 0)
		return SPUD_SUCCESS;
	if (wide_count > INT_MAX)
		return SPUDRESULT_SPUDNET_HTTP_FAILED;
	int   utf8_size = WideCharToMultiByte(CP_UTF8, 0, raw, (int)wide_count, NULL, 0, NULL, NULL);
	char *utf8      = utf8_size > 0 ? (char *)spud_allocate(allocator, (size_t)utf8_size, 1) : NULL;
	if (!utf8 || WideCharToMultiByte(CP_UTF8, 0, raw, (int)wide_count, utf8, utf8_size, NULL, NULL) <= 0) {
		if (utf8)
			spud_free(allocator, utf8, (size_t)utf8_size, 1);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	// Line by line, in the order sent; a name that repeats is joined onto
	// its first value as each line is added.
	bool stored = true;
	int  start  = 0;
	for (int i = 0; i < utf8_size && stored; ++i) {
		if (utf8[i] != '\n')
			continue;
		stored = spudnet_head_add_header_line(allocator, head, utf8 + start, (size_t)(i - start));
		start  = i + 1;
	}
	spud_free(allocator, utf8, (size_t)utf8_size, 1);
	return stored ? SPUD_SUCCESS : SPUDRESULT_OUT_OF_MEMORY;
}

/* Closes the request handle unless something already has. From any
 * thread. */
static void spudnet_win_http_close_request(struct spudnet_win_http *http) {
	if (http->request && InterlockedExchange(&http->request_closing, 1) == 0)
		WinHttpCloseHandle(http->request);
}

SPUDRESULT spudnet_http_backend_create(struct spudnet_http_transfer_t *transfer) {
	const SPUD_ALLOCATOR    *allocator = &transfer->head.allocator;
	struct spudnet_win_http *http      = SPUD_NEW_ZEROED(allocator, struct spudnet_win_http);
	if (!http)
		return SPUDRESULT_OUT_OF_MEMORY;
	http->owner        = transfer;
	http->client       = transfer->client;
	http->abort_event  = CreateEventW(NULL, TRUE, FALSE, NULL);
	http->closed_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	http->step_event   = CreateEventW(NULL, FALSE, FALSE, NULL);
	http->sent_event   = CreateEventW(NULL, TRUE, FALSE, NULL);
	http->write_buffer = (uint8_t *)spud_allocate(allocator, SPUDNET_WIN_HTTP_BUFFER_SIZE + SPUDNET_WIN_HTTP_CHUNK_FRAME, 1);
	http->read_buffer  = (uint8_t *)spud_allocate(allocator, SPUDNET_WIN_HTTP_BUFFER_SIZE, 1);
	transfer->backend  = http;
	if (!http->abort_event || !http->closed_event || !http->step_event || !http->sent_event || !http->write_buffer || !http->read_buffer) {
		spudnet_http_backend_destroy(transfer);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	return SPUD_SUCCESS;
}

void spudnet_http_backend_destroy(struct spudnet_http_transfer_t *transfer) {
	struct spudnet_win_http *http = (struct spudnet_win_http *)transfer->backend;
	if (!http)
		return;
	// Closing the request is what stops whatever was in flight on it. A
	// response that was read to its end has left its connection with the
	// session; any other takes its connection with it.
	if (http->request) {
		spudnet_win_http_close_request(http);
		if (http->has_context)
			WaitForSingleObject(http->closed_event, INFINITE);
	}
	if (http->connect)
		WinHttpCloseHandle(http->connect);
	if (http->abort_event)
		CloseHandle(http->abort_event);
	if (http->closed_event)
		CloseHandle(http->closed_event);
	if (http->step_event)
		CloseHandle(http->step_event);
	if (http->sent_event)
		CloseHandle(http->sent_event);
	// After the request handle has closed: the callback no longer reaches
	// this. Both buffers are of fixed sizes.
	const SPUD_ALLOCATOR *allocator = &transfer->head.allocator;
	spud_free(allocator, http->write_buffer, SPUDNET_WIN_HTTP_BUFFER_SIZE + SPUDNET_WIN_HTTP_CHUNK_FRAME, 1);
	spud_free(allocator, http->read_buffer, SPUDNET_WIN_HTTP_BUFFER_SIZE, 1);
	SPUD_DELETE(allocator, struct spudnet_win_http, http);
	transfer->backend = NULL;
}

void spudnet_http_backend_abort(struct spudnet_http_transfer_t *transfer) {
	struct spudnet_win_http *http = (struct spudnet_win_http *)transfer->backend;
	InterlockedExchange(&http->aborted, 1);
	SetEvent(http->abort_event);
	// Not needed to end the wait, which the event does; it stops the
	// request going on under a caller that has stopped waiting for it.
	spudnet_win_http_close_request(http);
}

bool spudnet_http_backend_aborted(struct spudnet_http_transfer_t *transfer) {
	return InterlockedCompareExchange(&((struct spudnet_win_http *)transfer->backend)->aborted, 0, 0) != 0;
}

/* Adds one header line of SpudNet's own to a request. */
static bool spudnet_win_http_add_line(HINTERNET request, const wchar_t *line) {
	return WinHttpAddRequestHeaders(request, line, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ? true : false;
}

SPUDRESULT spudnet_http_backend_start(
    struct spudnet_http_transfer_t *transfer,
    const spudnet_http_transfer_desc *desc,
    enum spudnet_accept_encoding encoding) {
	struct spudnet_win_http *http  = (struct spudnet_win_http *)transfer->backend;
	ULONGLONG                start = GetTickCount64();

	const SPUD_ALLOCATOR  *allocator = &transfer->head.allocator;
	struct spudnet_win_url url;
	SPUDRESULT             result = spudnet_win_crack(allocator, desc->url, false, &url);
	if (SPUDFAIL(result)) {
		spudnet_win_url_free(allocator, &url);
		return result;
	}
	size_t    method_count = 0;
	wchar_t  *method       = spudnet_win_wide(allocator, desc->method, &method_count);
	DWORD_PTR context      = (DWORD_PTR)http;

	result = SPUDRESULT_SPUDNET_HTTP_FAILED;
	if (!method) {
		result = SPUDRESULT_DESC_INVALID_PARAMETERS; // not UTF-8
		goto done;
	}
	http->connect = WinHttpConnect(http->client->session, url.host, url.port, 0);
	if (!http->connect) {
		result = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_CONNECT_FAILED);
		goto done;
	}
	// The method goes out as given; WinHTTP takes nothing from it.
	http->request = WinHttpOpenRequest(http->connect, method, url.path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0);
	if (!http->request) {
		result = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
		goto done;
	}

	// From here WinHTTP calls back with the transfer until it reports this
	// handle closing, which destroy waits for.
	if (!WinHttpSetOption(http->request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
		result = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
		goto done;
	}
	http->has_context = true;

	// WinHTTP's own limits off; the waits here are the only timer.
	WinHttpSetTimeouts(http->request, 0, 0, 0, 0);
	{
		DWORD redirects = desc->follow_redirects ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		WinHttpSetOption(http->request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
	}
	spudnet_win_disable_features(http->request);
	if (!spudnet_win_tls_apply(http->request, &http->client->tls, url.secure)) {
		result = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_TLS_FAILED);
		goto done;
	}
#ifdef WINHTTP_OPTION_DECOMPRESSION
	// The option makes WinHTTP decode, and also has it send an
	// Accept-Encoding of its own over any the caller gave. So for a caller
	// that sent `identity` it is left off: WinHTTP then asks for nothing
	// compressed, and the caller's header is the one that goes out.
	if (encoding == SPUDNET_ACCEPT_ENCODING_STACK) {
		DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
		WinHttpSetOption(http->request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
	}
#else
	(void)encoding;
#endif

	result = spudnet_win_add_headers(allocator, http->request, desc->headers, desc->header_count);
	if (SPUDFAIL(result))
		goto done;

	// What the request says of its body is written here, from the desc and
	// from nothing about the method. A sized body is announced by header
	// rather than through WinHttpSendRequest's own length, which is 32 bits
	// wide; WinHTTP takes the header's word when that length is left at 0.
	// An unsized body is announced as chunked, and WinHTTP leaves the
	// chunking itself to whoever writes the body, which is done in send.
	if (transfer->body == SPUDNET_HTTP_BODY_SIZED) {
		wchar_t line[64];
		swprintf(line, sizeof(line) / sizeof(line[0]), L"Content-Length: %llu\r\n", (unsigned long long)transfer->body_size);
		if (!spudnet_win_http_add_line(http->request, line)) {
			result = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
			goto done;
		}
	} else if (transfer->body == SPUDNET_HTTP_BODY_UNSIZED) {
		if (!spudnet_win_http_add_line(http->request, L"Transfer-Encoding: chunked\r\n")) {
			result = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
			goto done;
		}
	}

	// Connects, does the TLS handshake and sends the request line and
	// headers; the body, if any, follows in pieces.
	http->operation_pending = true;
	if (!WinHttpSendRequest(http->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, context)) {
		http->operation_pending = false;
		result                  = spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
		goto done;
	}
	result = spudnet_win_http_wait_sent(http, start, desc->timeout_ms);

done:
	spudnet_win_wide_free(allocator, method, method_count);
	spudnet_win_url_free(allocator, &url);
	return result;
}

SPUDRESULT spudnet_http_backend_send(
    struct spudnet_http_transfer_t *transfer,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_sent) {
	struct spudnet_win_http *http  = (struct spudnet_win_http *)transfer->backend;
	ULONGLONG                start = GetTickCount64();

	// The write before this one may still be on its way out of the buffer
	// this one needs. Out of time here means nothing of this call's was
	// taken.
	SPUDRESULT result = spudnet_win_http_wait(http, start, timeout_ms, SPUDRESULT_SPUDNET_SEND_FAILED);
	if (SPUDFAIL(result))
		return result;

	size_t bytes = size < SPUDNET_WIN_HTTP_BUFFER_SIZE ? size : SPUDNET_WIN_HTTP_BUFFER_SIZE;
	DWORD  total = 0;
	if (transfer->body == SPUDNET_HTTP_BODY_UNSIZED) {
		// One chunk: the piece's length in hex, a line ending, the piece,
		// a line ending.
		int prefix = snprintf((char *)http->write_buffer, SPUDNET_WIN_HTTP_CHUNK_FRAME - 2, "%zX\r\n", bytes);
		memcpy(http->write_buffer + prefix, data, bytes);
		memcpy(http->write_buffer + prefix + bytes, "\r\n", 2);
		total = (DWORD)(prefix + bytes + 2);
	} else {
		memcpy(http->write_buffer, data, bytes);
		total = (DWORD)bytes;
	}

	// Handed over, the bytes count as sent: the write is left to finish by
	// itself, and if it fails the next call is the one that hears of it.
	http->operation_pending = true;
	if (!WinHttpWriteData(http->request, http->write_buffer, total, NULL)) {
		http->operation_pending = false;
		return spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_SEND_FAILED);
	}
	*out_sent = bytes;
	return SPUD_SUCCESS;
}

SPUDRESULT spudnet_http_backend_receive_response(
    struct spudnet_http_transfer_t *transfer,
    uint32_t timeout_ms) {
	struct spudnet_win_http *http  = (struct spudnet_win_http *)transfer->backend;
	ULONGLONG                start = GetTickCount64();

	// Whatever is in flight comes first: a write, the closing chunk, or -
	// on a call made again after running out of time - the wait for the
	// response itself.
	SPUDRESULT result = spudnet_win_http_wait(http, start, timeout_ms, SPUDRESULT_SPUDNET_HTTP_FAILED);
	if (SPUDFAIL(result))
		return result;

	if (!http->response_asked) {
		// An unsized body ends with a chunk of no length.
		if (transfer->body == SPUDNET_HTTP_BODY_UNSIZED && !http->final_chunk_written) {
			memcpy(http->write_buffer, "0\r\n\r\n", 5);
			http->operation_pending = true;
			if (!WinHttpWriteData(http->request, http->write_buffer, 5, NULL)) {
				http->operation_pending = false;
				return spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
			}
			http->final_chunk_written = true;
			result                    = spudnet_win_http_wait(http, start, timeout_ms, SPUDRESULT_SPUDNET_HTTP_FAILED);
			if (SPUDFAIL(result))
				return result;
		}

		// Said once: the request is complete, and the response is wanted.
		http->operation_pending = true;
		if (!WinHttpReceiveResponse(http->request, NULL)) {
			http->operation_pending = false;
			return spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_HTTP_FAILED);
		}
		http->response_asked = true;
		result               = spudnet_win_http_wait(http, start, timeout_ms, SPUDRESULT_SPUDNET_HTTP_FAILED);
		if (SPUDFAIL(result))
			return result;
	}

	return spudnet_win_read_headers(&transfer->head.allocator, http->request, &transfer->response);
}

SPUDRESULT spudnet_http_backend_recv(
    struct spudnet_http_transfer_t *transfer,
    void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_received) {
	struct spudnet_win_http *http  = (struct spudnet_win_http *)transfer->backend;
	ULONGLONG                start = GetTickCount64();

	for (;;) {
		if (http->read_offset < http->read_size) {
			DWORD  left  = http->read_size - http->read_offset;
			size_t bytes = size < left ? size : left;
			memcpy(data, http->read_buffer + http->read_offset, bytes);
			http->read_offset += (DWORD)bytes;
			*out_received      = bytes;
			return SPUD_SUCCESS;
		}
		if (http->body_ended) {
			*out_received = 0;
			return SPUD_SUCCESS;
		}

		// Nothing is read from the server but when the caller asks: one
		// read is started, and a call that runs out of time leaves it in
		// flight for the next to wait on.
		if (!http->read_pending) {
			http->operation_pending = true;
			http->read_pending      = true;
			if (!WinHttpReadData(http->request, http->read_buffer, SPUDNET_WIN_HTTP_BUFFER_SIZE, NULL)) {
				http->operation_pending = false;
				http->read_pending      = false;
				return spudnet_win_http_failed(http, GetLastError(), SPUDRESULT_SPUDNET_RECV_FAILED);
			}
		}
		SPUDRESULT result = spudnet_win_http_wait(http, start, timeout_ms, SPUDRESULT_SPUDNET_RECV_FAILED);
		if (SPUDFAIL(result))
			return result;
		// WinHTTP hands the body over already decoded. A read of 0 bytes is
		// its end.
		http->read_pending = false;
		http->read_size    = http->step_bytes;
		http->read_offset  = 0;
		if (http->read_size == 0)
			http->body_ended = true;
	}
}

// --------------------------------------------------------------------------
// WebSocket
// --------------------------------------------------------------------------

// How much of a message WinHTTP is asked for at a time. A bigger message
// simply arrives in more parts.
#define SPUDNET_WIN_RECEIVE_BUFFER_SIZE (16 * 1024)

struct spudnet_websocket_t {
	spud_object_head head;
	// Set by create and only read after.
	spudnet_instance instance;
	// Guards everything below that more than one thread touches, and every
	// WinHTTP call that takes the request or websocket handle: none of
	// those block in asynchronous mode, and holding the lock across them is
	// what keeps a handle from being closed by one thread while another is
	// about to pass it in.
	CRITICAL_SECTION lock;

	HINTERNET session;
	HINTERNET connect;
	HINTERNET request;   // the opening request
	HINTERNET websocket;
	// The handle values stay after closing - the callback tells the two
	// apart by them - so these say whether each may still be used.
	bool request_closed;
	bool websocket_closed;

	// The request and websocket handles carry this struct as their context,
	// and WinHTTP may call back for a handle until it reports that handle
	// closing. The struct is freed only after the last of them has.
	bool   has_context_handles;
	LONG   context_handles;
	HANDLE handles_closed_event;

	// Set by spudnet_websocket_abort and never reset.
	HANDLE abort_event;
	bool   aborted;

	// The opening request: a stage finished, with this error (0 for none).
	bool   connect_called;
	bool   open;
	HANDLE request_event;
	DWORD  request_error;

	// Receive. `buffer` is what WinHTTP writes into, never the caller's
	// memory: a receive can outlive the call that started it.
	HANDLE                         receive_event;
	bool                           receive_pending;
	bool                           has_chunk;
	uint8_t                        buffer[SPUDNET_WIN_RECEIVE_BUFFER_SIZE];
	DWORD                          chunk_size;
	DWORD                          chunk_offset;
	WINHTTP_WEB_SOCKET_BUFFER_TYPE chunk_type;
	uint64_t                       max_message_size;
	uint64_t                       message_size;

	// Nothing more will arrive, and why.
	bool     finished;
	bool     too_big;
	bool     peer_closed;
	uint16_t close_code;
	bool     close_requested;

	// Send.
	HANDLE send_event;
	DWORD  send_error;

	// Filled in by connect before the request exists. `tls_refused`: the
	// callback turned the server's certificate away and closed the request.
	struct spudnet_win_tls tls;
	bool                   tls_refused;

	// By SPUDNET_ERROR_SIDE; each is written only by its side's caller.
	// `tls_reason` and `receive_error` are the callback's: why it turned a
	// certificate away, and what a receive failed with (under the lock).
	spudnet_error errors[SPUDNET_ERROR_SIDE_COUNT];
	spudnet_error tls_reason;
	DWORD         receive_error;

	// The wait set this socket is in, or NULL. Told, with the lock held,
	// whenever what a receive would find has changed.
	struct spudnet_wait_set_t *wait_set;
};

static void CALLBACK spudnet_win_websocket_callback(
    HINTERNET handle,
    DWORD_PTR context,
    DWORD status,
    LPVOID information,
    DWORD information_length) {
	(void)information_length;
	struct spudnet_websocket_t *socket = (struct spudnet_websocket_t *)context;
	if (!socket)
		return; // the session and connect handles carry no context

	switch (status) {
	case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
		// Closing the handle here, rather than leaving it to the waiting
		// thread, is what keeps the opening request from going out to a
		// server whose certificate was turned away.
		if (handle == socket->request && !spudnet_win_tls_accepts(handle, &socket->tls, &socket->tls_reason)) {
			EnterCriticalSection(&socket->lock);
			socket->tls_refused = true;
			if (!socket->request_closed) {
				socket->request_closed = true;
				WinHttpCloseHandle(socket->request);
			}
			LeaveCriticalSection(&socket->lock);
			socket->request_error = ERROR_WINHTTP_SECURE_FAILURE;
			SetEvent(socket->request_event);
		}
		break;

	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
		socket->request_error = ERROR_SUCCESS;
		SetEvent(socket->request_event);
		break;

	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
		const WINHTTP_WEB_SOCKET_STATUS *received = (const WINHTTP_WEB_SOCKET_STATUS *)information;
		EnterCriticalSection(&socket->lock);
		socket->receive_pending = false;
		socket->has_chunk       = true;
		socket->chunk_size      = received->dwBytesTransferred;
		socket->chunk_offset    = 0;
		socket->chunk_type      = received->eBufferType;
		spudnet_wait_set_notify(socket->wait_set);
		LeaveCriticalSection(&socket->lock);
		SetEvent(socket->receive_event);
		break;
	}

	case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
		socket->send_error = ERROR_SUCCESS;
		SetEvent(socket->send_event);
		break;

	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
		if (handle == socket->websocket) {
			const WINHTTP_WEB_SOCKET_ASYNC_RESULT *failed = (const WINHTTP_WEB_SOCKET_ASYNC_RESULT *)information;
			if (failed->Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) {
				socket->send_error = failed->AsyncResult.dwError ? failed->AsyncResult.dwError : ERROR_WINHTTP_CONNECTION_ERROR;
				SetEvent(socket->send_event);
			} else if (failed->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) {
				EnterCriticalSection(&socket->lock);
				socket->receive_pending = false;
				socket->finished        = true;
				socket->receive_error   = failed->AsyncResult.dwError;
				spudnet_wait_set_notify(socket->wait_set);
				LeaveCriticalSection(&socket->lock);
				SetEvent(socket->receive_event);
			}
			// A failed close or shutdown has nobody waiting on it.
		} else {
			const WINHTTP_ASYNC_RESULT *failed = (const WINHTTP_ASYNC_RESULT *)information;
			socket->request_error              = failed->dwError ? failed->dwError : ERROR_WINHTTP_CONNECTION_ERROR;
			SetEvent(socket->request_event);
		}
		break;

	case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
		// WinHTTP has let go of everything this handle was given. A sender
		// that gave up waiting is waiting for exactly that before it hands
		// the caller's buffer back.
		if (handle == socket->websocket) {
			socket->send_error = ERROR_OPERATION_ABORTED;
			SetEvent(socket->send_event);
		}
		if (InterlockedDecrement(&socket->context_handles) == 0)
			SetEvent(socket->handles_closed_event);
		break;

	default:
		break;
	}
}

/* Drops the connection: closes the request and websocket handles, which
 * fails whatever WinHTTP had outstanding on them and so wakes its waiter.
 * Safe to call more than once and from any thread. */
static void spudnet_win_websocket_kill(struct spudnet_websocket_t *socket) {
	EnterCriticalSection(&socket->lock);
	socket->finished = true;
	spudnet_wait_set_notify(socket->wait_set);
	if (socket->websocket && !socket->websocket_closed) {
		socket->websocket_closed = true;
		WinHttpCloseHandle(socket->websocket);
	}
	if (socket->request && !socket->request_closed) {
		socket->request_closed = true;
		WinHttpCloseHandle(socket->request);
	}
	LeaveCriticalSection(&socket->lock);
}

void spudnet_websocket_destroy(spudnet_websocket socket) {
	if (!socket)
		return;
	if (SPUD_STALE(socket, "spudnet_websocket_destroy", "socket"))
		return;
	spudnet_win_websocket_kill(socket);
	if (socket->has_context_handles)
		WaitForSingleObject(socket->handles_closed_event, INFINITE);
	if (socket->connect)
		WinHttpCloseHandle(socket->connect);
	if (socket->session)
		WinHttpCloseHandle(socket->session);

	if (socket->handles_closed_event)
		CloseHandle(socket->handles_closed_event);
	if (socket->abort_event)
		CloseHandle(socket->abort_event);
	if (socket->request_event)
		CloseHandle(socket->request_event);
	if (socket->receive_event)
		CloseHandle(socket->receive_event);
	if (socket->send_event)
		CloseHandle(socket->send_event);
	// After every handle with this as its context has closed: no callback
	// reaches it now.
	SPUD_KILL(socket);
	const SPUD_ALLOCATOR allocator = socket->head.allocator;
	spudnet_win_tls_free(&allocator, &socket->tls);
	DeleteCriticalSection(&socket->lock);
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

	struct spudnet_websocket_t *socket = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudnet_websocket_t);
	if (!socket)
		return SPUDRESULT_OUT_OF_MEMORY;
	socket->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(socket, &instance->quarantine);
	socket->instance       = instance;
	InitializeCriticalSection(&socket->lock);
	socket->handles_closed_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	socket->abort_event          = CreateEventW(NULL, TRUE, FALSE, NULL);
	socket->request_event        = CreateEventW(NULL, FALSE, FALSE, NULL);
	socket->receive_event        = CreateEventW(NULL, TRUE, FALSE, NULL);
	socket->send_event           = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!socket->handles_closed_event || !socket->abort_event || !socket->request_event || !socket->receive_event || !socket->send_event) {
		spudnet_websocket_destroy(socket);
		return SPUDRESULT_OUT_OF_MEMORY;
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

void spudnet_websocket_abort(spudnet_websocket socket) {
	if (!socket)
		return;
	if (SPUD_STALE(socket, "spudnet_websocket_abort", "socket"))
		return;
	EnterCriticalSection(&socket->lock);
	socket->aborted = true;
	LeaveCriticalSection(&socket->lock);
	SetEvent(socket->abort_event);
	spudnet_win_websocket_kill(socket);
}

/* Waits for the stage of the opening request just started. ERROR_SUCCESS
 * when it finished cleanly, SPUDNET_WIN_OWN_TIME_OUT when time ran out,
 * ERROR_OPERATION_ABORTED on abort, or the stage's own error. */
static DWORD spudnet_win_wait_request(struct spudnet_websocket_t *socket, ULONGLONG start, uint32_t timeout_ms) {
	HANDLE waits[2] = {socket->abort_event, socket->request_event};
	DWORD  waited   = WaitForMultipleObjects(2, waits, FALSE, spudnet_win_remaining(start, timeout_ms));
	if (waited == WAIT_OBJECT_0)
		return ERROR_OPERATION_ABORTED;
	if (waited == WAIT_OBJECT_0 + 1)
		return socket->request_error;
	return SPUDNET_WIN_OWN_TIME_OUT;
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

	const SPUD_ALLOCATOR  *allocator = &socket->head.allocator;
	ULONGLONG              start     = GetTickCount64();
	struct spudnet_win_url url;
	SPUDRESULT             result = spudnet_win_crack(allocator, desc->url, true, &url);
	if (SPUDFAIL(result)) {
		spudnet_win_url_free(allocator, &url);
		return result;
	}
	struct spudnet_win_tls tls;
	result = spudnet_win_tls_create(allocator, &desc->tls, &tls);
	if (SPUDFAIL(result)) {
		spudnet_win_url_free(allocator, &url);
		return result;
	}
	// Only needed until the session has been opened with it.
	struct spudnet_proxy proxy;
	result = spudnet_proxy_parse(allocator, &desc->proxy, &proxy);
	if (SPUDFAIL(result)) {
		spudnet_proxy_free(allocator, &proxy);
		spudnet_win_tls_free(allocator, &tls);
		spudnet_win_url_free(allocator, &url);
		return result;
	}

	EnterCriticalSection(&socket->lock);
	if (socket->aborted)
		result = SPUDRESULT_SPUDNET_ABORTED;
	else if (socket->connect_called)
		result = SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	else if (desc->timeout_ms == SPUDNET_NO_WAIT)
		result = SPUDRESULT_SPUDNET_TIMED_OUT;
	else
		socket->connect_called = true;
	LeaveCriticalSection(&socket->lock);
	if (SPUDFAIL(result)) {
		spudnet_proxy_free(allocator, &proxy);
		spudnet_win_tls_free(allocator, &tls);
		spudnet_win_url_free(allocator, &url);
		return result;
	}
	socket->tls              = tls; // the socket's from here
	socket->max_message_size = desc->max_message_size;

	DWORD     error   = ERROR_SUCCESS;
	DWORD     status  = 0;
	DWORD_PTR context = (DWORD_PTR)socket;

	// Everything up to the first wait is under the lock, so an abort either
	// came before it (seen above) or finds a request handle to close.
	EnterCriticalSection(&socket->lock);
	result          = SPUDRESULT_SPUDNET_CONNECT_FAILED;
	socket->session = spudnet_win_open_session(allocator, &proxy);
	spudnet_proxy_free(allocator, &proxy);
	// Set on the session before any other handle exists, so each inherits it.
	if (!socket->session ||
	    WinHttpSetStatusCallback(socket->session, spudnet_win_websocket_callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0) ==
	        WINHTTP_INVALID_STATUS_CALLBACK) {
		error = GetLastError();
		goto failed_locked;
	}

	socket->connect = WinHttpConnect(socket->session, url.host, url.port, 0);
	if (!socket->connect) {
		error  = GetLastError();
		result = spudnet_win_result(error, SPUDRESULT_SPUDNET_CONNECT_FAILED);
		goto failed_locked;
	}
	socket->request = WinHttpOpenRequest(socket->connect, L"GET", url.path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0);
	if (!socket->request) {
		error  = GetLastError();
		result = spudnet_win_result(error, SPUDRESULT_SPUDNET_CONNECT_FAILED);
		goto failed_locked;
	}

	// From here WinHTTP calls back for this handle until it reports it
	// closing, so the count is in place before anything can fail.
	if (!WinHttpSetOption(socket->request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
		error = GetLastError();
		goto failed_locked;
	}
	socket->context_handles     = 1;
	socket->has_context_handles = true;

	// WinHTTP's own limits off; the waits below are the only timer, and a
	// receive limit would carry over to the open socket and end it for
	// going quiet.
	WinHttpSetTimeouts(socket->request, 0, 0, 0, 0);
	{
		// Said either way, so that it is the desc and not WinHTTP's default
		// that decides what becomes of a redirect.
		DWORD redirects = desc->follow_redirects ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		WinHttpSetOption(socket->request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
	}
	spudnet_win_disable_features(socket->request);
	if (!spudnet_win_tls_apply(socket->request, &socket->tls, url.secure)) {
		error  = GetLastError();
		result = SPUDRESULT_SPUDNET_TLS_FAILED;
		goto failed_locked;
	}
	if (!WinHttpSetOption(socket->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
		error = GetLastError();
		goto failed_locked;
	}
	result = spudnet_win_add_headers(allocator, socket->request, desc->headers, desc->header_count);
	if (SPUDFAIL(result))
		goto failed_locked;
	result = SPUDRESULT_SPUDNET_CONNECT_FAILED;

	if (!WinHttpSendRequest(socket->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, context))
		error = GetLastError();
	LeaveCriticalSection(&socket->lock);
	if (error == ERROR_SUCCESS)
		error = spudnet_win_wait_request(socket, start, desc->timeout_ms);

	if (error == ERROR_SUCCESS) {
		EnterCriticalSection(&socket->lock);
		if (socket->request_closed)
			error = ERROR_OPERATION_ABORTED;
		else if (!WinHttpReceiveResponse(socket->request, NULL))
			error = GetLastError();
		LeaveCriticalSection(&socket->lock);
		if (error == ERROR_SUCCESS)
			error = spudnet_win_wait_request(socket, start, desc->timeout_ms);
	}

	EnterCriticalSection(&socket->lock);
	if (error != ERROR_SUCCESS || socket->request_closed) {
		result = spudnet_win_result(error, SPUDRESULT_SPUDNET_CONNECT_FAILED);
		goto failed_locked;
	}

	DWORD status_size = sizeof(status);
	if (!WinHttpQueryHeaders(socket->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
	                         WINHTTP_NO_HEADER_INDEX)) {
		error = GetLastError();
		goto failed_locked;
	}
	if (out_http_status)
		*out_http_status = status;
	if (status != HTTP_STATUS_SWITCH_PROTOCOLS) {
		result = SPUDRESULT_SPUDNET_WS_UPGRADE_REFUSED;
		goto failed_locked;
	}

	// Counted before the handle exists: its callbacks can start at once.
	InterlockedIncrement(&socket->context_handles);
	socket->websocket = WinHttpWebSocketCompleteUpgrade(socket->request, context);
	if (!socket->websocket) {
		error = GetLastError();
		InterlockedDecrement(&socket->context_handles);
		goto failed_locked;
	}
	socket->request_closed = true;
	WinHttpCloseHandle(socket->request);
	socket->open = true;
	LeaveCriticalSection(&socket->lock);

	spudnet_win_url_free(allocator, &url);
	return SPUD_SUCCESS;

failed_locked:
	// This socket is spent; closing the request is also what stops an
	// opening that ran out of time.
	if (socket->aborted) {
		result = SPUDRESULT_SPUDNET_ABORTED;
	} else if (socket->tls_refused) {
		// Whatever WinHTTP made of its handle closing, this is why it closed.
		result                                       = SPUDRESULT_SPUDNET_TLS_FAILED;
		socket->errors[SPUDNET_ERROR_SIDE_RECEIVING] = socket->tls_reason;
	} else if (error != ERROR_SUCCESS && result != SPUDRESULT_SPUDNET_TIMED_OUT && result != SPUDRESULT_SPUDNET_WS_UPGRADE_REFUSED) {
		spudnet_error_record(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], result, SPUD_PLATFORM_ERROR_SOURCE_WIN32, error, NULL);
	}
	LeaveCriticalSection(&socket->lock);
	spudnet_win_websocket_kill(socket);
	spudnet_win_url_free(allocator, &url);
	return result;
}

/* What a call on a socket that can't carry it returns, or SPUD_SUCCESS for
 * one that can. Called with the lock held. */
static SPUDRESULT spudnet_win_websocket_usable(const struct spudnet_websocket_t *socket) {
	if (socket->aborted)
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
	if ((size > 0 && !data) || size > SPUD_UINT32_MAX)
		return SPUDRESULT_SPUDNET_SEND_FAILED;

	ULONGLONG                      start       = GetTickCount64();
	WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type =
	    type == SPUDNET_WEBSOCKET_MESSAGE_TEXT ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;

	EnterCriticalSection(&socket->lock);
	SPUDRESULT result = spudnet_win_websocket_usable(socket);
	if (!SPUDFAIL(result) && (socket->finished || socket->websocket_closed))
		result = SPUDRESULT_SPUDNET_SEND_FAILED;
	if (!SPUDFAIL(result) && timeout_ms == SPUDNET_NO_WAIT)
		result = SPUDRESULT_SPUDNET_TIMED_OUT;
	// WinHTTP sends a UTF-8 buffer as it is given. Refused here, nothing is
	// sent and the connection carries on.
	if (!SPUDFAIL(result) && type == SPUDNET_WEBSOCKET_MESSAGE_TEXT && !spudnet_utf8_valid((const uint8_t *)data, (size_t)size))
		result = SPUDRESULT_SPUDNET_SEND_FAILED;
	spudnet_error *record = &socket->errors[SPUDNET_ERROR_SIDE_SENDING];
	if (!SPUDFAIL(result)) {
		// Returns its error rather than leaving it for GetLastError.
		DWORD started = WinHttpWebSocketSend(socket->websocket, buffer_type, size > 0 ? (PVOID)data : NULL, (DWORD)size);
		if (started != ERROR_SUCCESS)
			result = spudnet_error_record(record, SPUDRESULT_SPUDNET_SEND_FAILED, SPUD_PLATFORM_ERROR_SOURCE_WIN32, started, NULL);
	}
	LeaveCriticalSection(&socket->lock);
	if (SPUDFAIL(result))
		return result;

	HANDLE waits[2] = {socket->abort_event, socket->send_event};
	DWORD  waited   = WaitForMultipleObjects(2, waits, FALSE, spudnet_win_remaining(start, timeout_ms));
	if (waited == WAIT_OBJECT_0 + 1) {
		if (socket->send_error == ERROR_SUCCESS)
			return SPUD_SUCCESS;
		return spudnet_error_record(record, SPUDRESULT_SPUDNET_SEND_FAILED, SPUD_PLATFORM_ERROR_SOURCE_WIN32, socket->send_error, NULL);
	}

	// Aborted or out of time, with `data` still WinHTTP's to read. Part of
	// the message may be on the wire, so the connection is dropped either
	// way, and this stays until WinHTTP reports the send over or the handle
	// closed - whichever comes first sets the event.
	spudnet_win_websocket_kill(socket);
	WaitForSingleObject(socket->send_event, INFINITE);
	return waited == WAIT_OBJECT_0 ? SPUDRESULT_SPUDNET_ABORTED : SPUDRESULT_SPUDNET_TIMED_OUT;
}

/* Why a finished socket finished. Called with the lock held. */
static SPUDRESULT spudnet_win_websocket_end_result(const struct spudnet_websocket_t *socket) {
	if (socket->too_big)
		return SPUDRESULT_SPUDNET_WS_MESSAGE_TOO_BIG;
	if (socket->peer_closed || socket->close_requested)
		return SPUDRESULT_SPUDNET_WS_CLOSED;
	return SPUDRESULT_SPUDNET_RECV_FAILED;
}

/* Asks WinHTTP for the next part of a message, into the socket's own
 * buffer. Called with the lock held and no receive in flight. One that
 * can't be started ends the socket, as one that fails later does. */
static void spudnet_win_websocket_start_receive(struct spudnet_websocket_t *socket) {
	ResetEvent(socket->receive_event);
	socket->receive_pending = true;
	// Returns its error rather than leaving it for GetLastError.
	DWORD started = socket->websocket_closed ? ERROR_SUCCESS : WinHttpWebSocketReceive(socket->websocket, socket->buffer, sizeof(socket->buffer), NULL, NULL);
	if (socket->websocket_closed || started != ERROR_SUCCESS) {
		socket->receive_pending = false;
		socket->finished        = true;
		if (started != ERROR_SUCCESS)
			socket->receive_error = started;
	}
}

// For a wait set (see the end of spudnetshared.h). WinHTTP reports through
// callbacks, so there is no socket to poll: the socket keeps its set, tells
// it whenever a receive has an answer or the connection is dropped, and is
// asked here.

SPUDRESULT spudnet_websocket_wait_attach(
    spudnet_websocket socket,
    struct spudnet_wait_set_t *set,
    bool *out_has_native,
    intptr_t *out_native,
    bool *out_report_once) {
	EnterCriticalSection(&socket->lock);
	SPUDRESULT result = SPUD_SUCCESS;
	if (socket->aborted || !socket->open)
		result = SPUDRESULT_SPUDNET_INVALID_WEBSOCKET;
	else if (socket->wait_set)
		result = SPUDRESULT_SPUDNET_ALREADY_IN_WAIT_SET;
	if (!SPUDFAIL(result)) {
		socket->wait_set = set;
		// WinHTTP only says a message has arrived to a receive that was
		// asked for; without one the set would never hear of it.
		if (!socket->has_chunk && !socket->finished && !socket->receive_pending)
			spudnet_win_websocket_start_receive(socket);
	}
	LeaveCriticalSection(&socket->lock);
	if (SPUDFAIL(result))
		return result;

	*out_has_native  = false;
	*out_native      = 0;
	*out_report_once = false;
	return SPUD_SUCCESS;
}

void spudnet_websocket_wait_detach(spudnet_websocket socket) {
	// Every notification is made with the lock held, so none is under way
	// once this has it.
	EnterCriticalSection(&socket->lock);
	socket->wait_set = NULL;
	LeaveCriticalSection(&socket->lock);
}

bool spudnet_websocket_wait_ready(spudnet_websocket socket) {
	EnterCriticalSection(&socket->lock);
	bool ready = socket->aborted || socket->has_chunk || socket->finished;
	LeaveCriticalSection(&socket->lock);
	return ready;
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

	ULONGLONG  start  = GetTickCount64();
	SPUDRESULT result = SPUD_SUCCESS;

	EnterCriticalSection(&socket->lock);
	for (;;) {
		result = spudnet_win_websocket_usable(socket);
		if (SPUDFAIL(result))
			break;
		// After this side's close nothing more is handed over, whatever
		// WinHTTP goes on delivering.
		if (socket->close_requested) {
			result = SPUDRESULT_SPUDNET_WS_CLOSED;
			break;
		}

		if (socket->has_chunk) {
			if (socket->chunk_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
				USHORT code          = 0;
				BYTE   reason[123];
				DWORD  reason_length = 0;
				if (!socket->websocket_closed &&
				    WinHttpWebSocketQueryCloseStatus(socket->websocket, &code, reason, sizeof(reason), &reason_length) == ERROR_SUCCESS &&
				    code != WINHTTP_WEB_SOCKET_EMPTY_CLOSE_STATUS)
					socket->close_code = code;
				socket->has_chunk   = false;
				socket->peer_closed = true;
				socket->finished    = true;
				continue;
			}

			DWORD left  = socket->chunk_size - socket->chunk_offset;
			DWORD count = size < left ? (DWORD)size : left;
			if ((uint64_t)count > socket->max_message_size - socket->message_size) {
				// Told why, like the stacks that enforce the limit themselves.
				if (!socket->websocket_closed)
					WinHttpWebSocketShutdown(socket->websocket, WINHTTP_WEB_SOCKET_MESSAGE_TOO_BIG_CLOSE_STATUS, NULL, 0);
				socket->has_chunk = false;
				socket->too_big   = true;
				socket->finished  = true;
				continue;
			}
			if (count > 0)
				memcpy(data, socket->buffer + socket->chunk_offset, count);
			socket->chunk_offset += count;
			socket->message_size += count;

			bool text = socket->chunk_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || socket->chunk_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
			bool last = socket->chunk_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || socket->chunk_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
			*out_received = count;
			*out_type     = text ? SPUDNET_WEBSOCKET_MESSAGE_TEXT : SPUDNET_WEBSOCKET_MESSAGE_BINARY;
			if (socket->chunk_offset == socket->chunk_size) {
				socket->has_chunk = false;
				if (last) {
					*out_message_complete = true;
					socket->message_size  = 0;
				}
			}
			break;
		}

		if (socket->finished) {
			result = spudnet_win_websocket_end_result(socket);
			if (result == SPUDRESULT_SPUDNET_RECV_FAILED && socket->receive_error != ERROR_SUCCESS)
				spudnet_error_record(&socket->errors[SPUDNET_ERROR_SIDE_RECEIVING], result, SPUD_PLATFORM_ERROR_SOURCE_WIN32, socket->receive_error, NULL);
			break;
		}

		// A receive left in flight by a call that timed out, or started for
		// a wait set, is still the one that will answer; WinHTTP allows only
		// one at a time.
		if (!socket->receive_pending) {
			spudnet_win_websocket_start_receive(socket);
			// It may have failed, or WinHTTP may have called back on this
			// thread already.
			if (socket->has_chunk || socket->finished)
				continue;
		}

		// With SPUDNET_NO_WAIT this returns at once and only looks.
		HANDLE waits[2] = {socket->abort_event, socket->receive_event};
		LeaveCriticalSection(&socket->lock);
		DWORD waited = WaitForMultipleObjects(2, waits, FALSE, spudnet_win_remaining(start, timeout_ms));
		EnterCriticalSection(&socket->lock);
		if (waited == WAIT_TIMEOUT && !socket->has_chunk && !socket->finished && !socket->aborted) {
			result = SPUDRESULT_SPUDNET_TIMED_OUT;
			break;
		}
	}
	LeaveCriticalSection(&socket->lock);
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

	EnterCriticalSection(&socket->lock);
	SPUDRESULT result = spudnet_win_websocket_usable(socket);
	if (!SPUDFAIL(result) && (socket->finished || socket->websocket_closed))
		result = SPUDRESULT_SPUDNET_SEND_FAILED; // already ended: no connection to send a frame on
	if (!SPUDFAIL(result)) {
		// Shutdown starts the close frame on its way and returns
		// (WinHttpWebSocketClose would wait for the peer's answer). It
		// leaves WinHTTP receiving; spudnet.h has every stack stop here, as
		// the one that can't go on does, so the socket is marked finished
		// and what WinHTTP delivers from now on is left where it lands.
		DWORD started = WinHttpWebSocketShutdown(socket->websocket, code, reason_length > 0 ? (PVOID)reason : NULL, (DWORD)reason_length);
		if (started != ERROR_SUCCESS) {
			result = spudnet_error_record(&socket->errors[SPUDNET_ERROR_SIDE_SENDING], SPUDRESULT_SPUDNET_SEND_FAILED, SPUD_PLATFORM_ERROR_SOURCE_WIN32, started, NULL);
		} else {
			socket->close_requested = true;
			socket->finished        = true;
			socket->has_chunk       = false;
			// A receive that is waiting is woken to find the socket closed.
			SetEvent(socket->receive_event);
			spudnet_wait_set_notify(socket->wait_set);
		}
	}
	LeaveCriticalSection(&socket->lock);
	return result;
}

uint16_t spudnet_websocket_get_close_code(spudnet_websocket socket) {
	if (!socket)
		return 0;
	if (SPUD_STALE(socket, "spudnet_websocket_get_close_code", "socket"))
		return 0;
	EnterCriticalSection(&socket->lock);
	// Only a close the peer made has a code to report.
	uint16_t code = (socket->peer_closed && !socket->close_requested) ? socket->close_code : 0;
	LeaveCriticalSection(&socket->lock);
	return code;
}

#if __cplusplus
}
#endif

#endif // SPUDLIB_PLATFORM_WIN32
