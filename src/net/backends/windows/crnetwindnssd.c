#if CARROTTUS_PLATFORM_WIN32

#include "../../crnetshared.h"
#include "crnet.h"

#if CRNET_EXT_DISCOVERY

/*
 * Service discovery on Windows' DNS-SD calls in dnsapi.dll (crnet.h,
 * "Service discovery"; the front end is src/net/crnetdiscovery.c).
 *
 * Each object opens dnsapi.dll itself at its create (LoadLibraryExW from
 * the system directory), finds the calls it needs into a table of its own,
 * and closes the library at its destroy: nothing is kept between objects or
 * in the instance. A call missing from the library is
 * CR_RESULT_NET_DISCOVERY_UNAVAILABLE.
 *
 * The system calls back on threads of its own. Each callback hands what it
 * was given to the front end's crnet_discovery_report_* calls, which write
 * the object's ring under its lock and wake its wait set, then signals the
 * object's news event, which `pump` waits on. A callback calls nothing of
 * the caller's and allocates nothing through the caller's allocator.
 *
 * - An advert is DnsServiceConstructInstance and DnsServiceRegister, mDNS
 *   only (unicastEnabled FALSE), its host this machine's DNS host name with
 *   ".local".
 * - A browser is DnsServiceBrowse of "<type>.local": each PTR record with a
 *   TTL is FOUND, one with a TTL of 0 LOST, interface 0 (Windows names
 *   none).
 * - A resolver is DnsServiceResolve: one answer, the host, port, TXT and at
 *   most one address of each family, or the system's own time-out.
 *
 * A destroy cancels what is under way (DnsServiceRegisterCancel, or
 * DnsServiceDeRegister for an advert registered; DnsServiceBrowseCancel;
 * DnsServiceResolveCancel for a resolve not yet answered) and waits until
 * the operation's last callback has come and no callback is still inside
 * the object (Windows doesn't say one operation's callbacks come one at a
 * time): crnet.h's stated known problem. A destroy that returned first
 * would leave a callback on the system's thread with no object. Each
 * callback counts itself in and out under the object's lock, and the one
 * that lets a destroy go does so as the last thing it does, touching
 * nothing of the object after.
 *
 * dnsapi's status codes are recorded as Win32 errors, the text starting
 * "dnsapi".
 */

// winsock2.h before windows.h (which would pull in the old winsock.h), windns.h after.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
// clang-format on

#include <stdio.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

/* The library's name. A test build of this file points it elsewhere, to
 * see a create without the library; never a variable (no writable
 * static). */
#ifndef CRNET_WIN_DNSAPI_LIBRARY
#define CRNET_WIN_DNSAPI_LIBRARY L"dnsapi.dll"
#endif

typedef PDNS_SERVICE_INSTANCE(WINAPI *crnet_win_construct_fn)(
    PCWSTR,
    PCWSTR,
    PIP4_ADDRESS,
    PIP6_ADDRESS,
    WORD,
    WORD,
    WORD,
    DWORD,
    PCWSTR *,
    PCWSTR *);
typedef DWORD(WINAPI *crnet_win_register_fn)(
    PDNS_SERVICE_REGISTER_REQUEST,
    PDNS_SERVICE_CANCEL);
typedef DWORD(WINAPI *crnet_win_register_cancel_fn)(PDNS_SERVICE_CANCEL);
typedef DNS_STATUS(WINAPI *crnet_win_browse_fn)(
    PDNS_SERVICE_BROWSE_REQUEST,
    PDNS_SERVICE_CANCEL);
typedef DNS_STATUS(WINAPI *crnet_win_resolve_fn)(
    PDNS_SERVICE_RESOLVE_REQUEST,
    PDNS_SERVICE_CANCEL);
typedef DNS_STATUS(WINAPI *crnet_win_cancel_fn)(PDNS_SERVICE_CANCEL);
typedef VOID(WINAPI *crnet_win_free_instance_fn)(PDNS_SERVICE_INSTANCE);
typedef VOID(WINAPI *crnet_win_record_list_free_fn)(
    PDNS_RECORD,
    DNS_FREE_TYPE);

/* The calls an object takes from its own open of the library. */
struct crnet_win_dnssd_calls {
	crnet_win_construct_fn construct_instance;
	crnet_win_register_fn register_service;
	crnet_win_register_fn deregister_service;
	crnet_win_register_cancel_fn register_cancel;
	crnet_win_browse_fn browse;
	crnet_win_cancel_fn browse_cancel;
	crnet_win_resolve_fn resolve;
	crnet_win_cancel_fn resolve_cancel;
	crnet_win_free_instance_fn free_instance;
	crnet_win_record_list_free_fn record_list_free;
};

struct crnet_win_dnssd {
	struct crnet_discovery_object *object;
	HMODULE library;
	struct crnet_win_dnssd_calls calls;
	// Set by a callback once it has reported; auto-reset, waited on by pump.
	HANDLE news;
	// Set by wake, never reset.
	HANDLE abort;
	// Set once a destroy may go on: the operation's last callback has come
	// and no callback is inside. Manual reset, reset by the destroy.
	HANDLE done;
	CRITICAL_SECTION lock;
	bool lock_made;
	// Under `lock`: an operation was begun, so a last callback will come;
	// that last callback has come; callbacks inside now; a destroy is
	// waiting.
	bool begun;
	bool last_came;
	uint32_t inside;
	bool stopping;
	// An advert: its registration succeeded (so a destroy deregisters), a
	// deregistration was asked for, and the request it takes again.
	bool registered;
	bool deregistering;
	DNS_SERVICE_REGISTER_REQUEST request;
	PDNS_SERVICE_INSTANCE instance;
	DNS_SERVICE_CANCEL cancel;
	// The browse's or resolve's query name, kept for the operation's life.
	WCHAR query[DNS_MAX_NAME_BUFFER_LENGTH];
};

static struct crnet_win_dnssd *crnet_win_dnssd_of(struct crnet_discovery_object *object) { return (struct crnet_win_dnssd *)object->backend; }

/* What a status dnsapi reports is: refused, a name taken, a resolve the
 * instance never answered, or anything else. */
static CR_RESULT crnet_win_dnssd_result(DWORD status) {
	switch (status) {
	case ERROR_ACCESS_DENIED:
		return CR_RESULT_NET_DISCOVERY_DENIED;
	case ERROR_ALREADY_EXISTS:
	case DNS_ERROR_RCODE_YXDOMAIN:
	case DNS_ERROR_RECORD_ALREADY_EXISTS:
		return CR_RESULT_NET_SERVICE_NAME_CONFLICT;
	case ERROR_TIMEOUT:
		return CR_RESULT_NET_SERVICE_NOT_ANSWERING;
	default:
		return CR_RESULT_NET_DISCOVERY_FAILED;
	}
}

static void crnet_win_dnssd_text(
    char *text,
    size_t capacity,
    const char *call,
    DWORD status) {
	snprintf(text, capacity, "dnsapi: %s, status %lu", call, (unsigned long)status);
}

/* A failure in a callback, as the object's end. */
static void crnet_win_dnssd_end(
    struct crnet_discovery_object *object,
    const char *call,
    DWORD status) {
	char text[96];
	crnet_win_dnssd_text(text, sizeof(text), call, status);
	crnet_discovery_report_end(object, crnet_win_dnssd_result(status), CR_PLATFORM_ERROR_SOURCE_WIN32, (int64_t)status, text);
}

/* A create's failure, recorded in `out_error` and returned. */
static CR_RESULT crnet_win_dnssd_start_failed(
    crnet_error *out_error,
    CR_RESULT result,
    const char *call,
    DWORD status) {
	char text[96];
	crnet_win_dnssd_text(text, sizeof(text), call, status);
	return crnet_error_record(out_error, result, CR_PLATFORM_ERROR_SOURCE_WIN32, (int64_t)status, text);
}

/* Each object's own open of the library and its table of calls. */
static CR_RESULT crnet_win_dnssd_open(
    struct crnet_win_dnssd *state,
    crnet_error *out_error) {
	state->library = LoadLibraryExW(CRNET_WIN_DNSAPI_LIBRARY, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!state->library)
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_UNAVAILABLE, "LoadLibraryExW", GetLastError());
	struct crnet_win_dnssd_calls *calls = &state->calls;
	// GetProcAddress hands back a FARPROC whatever the call's type; each is
	// taken as the type windns.h declares it with.
#define CRNET_WIN_DNSSD_FIND(field, type, name) calls->field = (type)(void *)GetProcAddress(state->library, name)
	CRNET_WIN_DNSSD_FIND(construct_instance, crnet_win_construct_fn, "DnsServiceConstructInstance");
	CRNET_WIN_DNSSD_FIND(register_service, crnet_win_register_fn, "DnsServiceRegister");
	CRNET_WIN_DNSSD_FIND(deregister_service, crnet_win_register_fn, "DnsServiceDeRegister");
	CRNET_WIN_DNSSD_FIND(register_cancel, crnet_win_register_cancel_fn, "DnsServiceRegisterCancel");
	CRNET_WIN_DNSSD_FIND(browse, crnet_win_browse_fn, "DnsServiceBrowse");
	CRNET_WIN_DNSSD_FIND(browse_cancel, crnet_win_cancel_fn, "DnsServiceBrowseCancel");
	CRNET_WIN_DNSSD_FIND(resolve, crnet_win_resolve_fn, "DnsServiceResolve");
	CRNET_WIN_DNSSD_FIND(resolve_cancel, crnet_win_cancel_fn, "DnsServiceResolveCancel");
	CRNET_WIN_DNSSD_FIND(free_instance, crnet_win_free_instance_fn, "DnsServiceFreeInstance");
	CRNET_WIN_DNSSD_FIND(record_list_free, crnet_win_record_list_free_fn, "DnsRecordListFree");
#undef CRNET_WIN_DNSSD_FIND
	if (!calls->construct_instance || !calls->register_service || !calls->deregister_service || !calls->register_cancel || !calls->browse ||
	    !calls->browse_cancel || !calls->resolve || !calls->resolve_cancel || !calls->free_instance || !calls->record_list_free)
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_UNAVAILABLE, "GetProcAddress", ERROR_PROC_NOT_FOUND);
	return CR_SUCCESS;
}

/* The backend's state, its events and lock, and the library, before the
 * operation is begun. Whatever was made is in `object->backend` for stop,
 * on a failure too. */
static CR_RESULT crnet_win_dnssd_make(
    struct crnet_discovery_object *object,
    crnet_error *out_error) {
	struct crnet_win_dnssd *state = CR_NEW_ZEROED(&object->head.allocator, struct crnet_win_dnssd);
	if (!state)
		return CR_RESULT_OUT_OF_MEMORY;
	state->object   = object;
	object->backend = state;
	InitializeCriticalSection(&state->lock);
	state->lock_made = true;
	state->news      = CreateEventW(NULL, FALSE, FALSE, NULL);
	state->abort     = CreateEventW(NULL, TRUE, FALSE, NULL);
	state->done      = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!state->news || !state->abort || !state->done)
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_FAILED, "CreateEventW", GetLastError());
	return crnet_win_dnssd_open(state, out_error);
}

/* UTF-8 (`length` bytes) appended to `out` as UTF-16 at `*used`, within
 * `capacity` characters with a terminator. False when it doesn't fit. */
static bool crnet_win_wide_append(
    WCHAR *out,
    size_t capacity,
    size_t *used,
    const char *text,
    size_t length) {
	if (length == 0) {
		out[*used] = 0;
		return true;
	}
	const int room = (int)(capacity - 1 - *used);
	if (room <= 0)
		return false;
	const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, out + *used, room);
	if (written <= 0)
		return false;
	*used += (size_t)written;
	out[*used] = 0;
	return true;
}

/* The full name of instance `name` of `type` in "local", the instance's
 * label escaped where '.' or '\' would end or begin an escape. */
static bool crnet_win_full_name(
    const char *name,
    const char *type,
    WCHAR *out,
    size_t capacity) {
	size_t used = 0;
	for (const char *c = name; *c; ++c) {
		if (*c == '.' || *c == '\\') {
			if (!crnet_win_wide_append(out, capacity, &used, "\\", 1) || !crnet_win_wide_append(out, capacity, &used, c, 1))
				return false;
			continue;
		}
		// A run of bytes up to the next character that needs escaping.
		const char *run = c;
		while (c[1] && c[1] != '.' && c[1] != '\\')
			++c;
		if (!crnet_win_wide_append(out, capacity, &used, run, (size_t)(c - run) + 1))
			return false;
	}
	return crnet_win_wide_append(out, capacity, &used, ".", 1) && crnet_win_wide_append(out, capacity, &used, type, strlen(type)) &&
	       crnet_win_wide_append(out, capacity, &used, ".local", 6);
}

/* UTF-16 into UTF-8, false when it doesn't fit `capacity` with its
 * terminator. */
static bool crnet_win_utf8(
    const WCHAR *text,
    char *out,
    size_t capacity) {
	if (!text)
		return false;
	return WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)capacity, NULL, NULL) > 0;
}

// --------------------------------------------------------------------------
// The callbacks, each on a thread of the system's
// --------------------------------------------------------------------------

/* Frees an instance the system handed a callback, unless it is the
 * advert's own. */
static void crnet_win_dnssd_free_given(
    struct crnet_win_dnssd *state,
    PDNS_SERVICE_INSTANCE given) {
	if (given && given != state->instance)
		state->calls.free_instance(given);
}

/* Whether a destroy may go on, under `lock`. */
static bool crnet_win_dnssd_released(const struct crnet_win_dnssd *state) {
	return state->stopping && state->inside == 0 && (!state->begun || state->last_came);
}

/* A callback's first step: counted in, so a destroy waits for it. */
static void crnet_win_dnssd_enter(struct crnet_win_dnssd *state) {
	EnterCriticalSection(&state->lock);
	state->inside++;
	LeaveCriticalSection(&state->lock);
}

/* A callback's last step (`last` for the operation's last callback): it
 * signals the news, counts itself out, and lets a waiting destroy go if it
 * is the one that may. Nothing of the object is touched after the lock is
 * left: the destroy may free it as soon as `done` is set, so its handle is
 * read under the lock first. */
static void crnet_win_dnssd_leave(
    struct crnet_win_dnssd *state,
    bool last) {
	SetEvent(state->news);
	EnterCriticalSection(&state->lock);
	state->inside--;
	if (last)
		state->last_came = true;
	const bool release = crnet_win_dnssd_released(state);
	const HANDLE done  = state->done;
	LeaveCriticalSection(&state->lock);
	if (release)
		SetEvent(done);
}

static VOID WINAPI crnet_win_dnssd_registered(
    DWORD status,
    PVOID context,
    PDNS_SERVICE_INSTANCE given) {
	struct crnet_win_dnssd *state         = (struct crnet_win_dnssd *)context;
	struct crnet_discovery_object *object = state->object;
	crnet_win_dnssd_enter(state);
	EnterCriticalSection(&state->lock);
	const bool stopping      = state->stopping;
	const bool deregistering = state->deregistering;
	LeaveCriticalSection(&state->lock);
	bool last = true;
	if (deregistering) {
		// The deregistration's answer: the advert is gone.
		EnterCriticalSection(&state->lock);
		state->registered = false;
		LeaveCriticalSection(&state->lock);
	} else if (status == ERROR_SUCCESS) {
		// Registered. While its cancel was being asked for, the destroy
		// deregisters it; otherwise the registration goes on.
		EnterCriticalSection(&state->lock);
		state->registered = true;
		LeaveCriticalSection(&state->lock);
		last = stopping;
		if (!stopping) {
			// No name may be changed for the caller: a registration under
			// another name than the one asked for is the name taken.
			char name[CRNET_SERVICE_NAME_CAPACITY * 4];
			char label[CRNET_SERVICE_NAME_CAPACITY];
			size_t used = 0, got = SIZE_MAX;
			if (given && crnet_win_utf8(given->pszInstanceName, name, sizeof(name)))
				got = crnet_dns_label_unescape(name, strlen(name), label, sizeof(label) - 1, &used);
			if (given && given->pszInstanceName && (got == SIZE_MAX || got != strlen(object->name) || memcmp(label, object->name, got) != 0))
				crnet_discovery_report_end(
				    object, CR_RESULT_NET_SERVICE_NAME_CONFLICT, CR_PLATFORM_ERROR_SOURCE_WIN32, 0, "dnsapi: DnsServiceRegister registered another name");
			else
				crnet_discovery_report_registered(object);
		}
	} else if (!stopping) {
		crnet_win_dnssd_end(object, "DnsServiceRegister", status);
	}
	crnet_win_dnssd_free_given(state, given);
	crnet_win_dnssd_leave(state, last);
}

static VOID WINAPI crnet_win_dnssd_browsed(
    DWORD status,
    PVOID context,
    PDNS_RECORD records) {
	struct crnet_win_dnssd *state         = (struct crnet_win_dnssd *)context;
	struct crnet_discovery_object *object = state->object;
	crnet_win_dnssd_enter(state);
	if (status == ERROR_SUCCESS) {
		// DnsServiceBrowse is a wide call: its records are DNS_RECORDW.
		for (const DNS_RECORDW *record = (const DNS_RECORDW *)records; record; record = record->pNext) {
			if (record->wType != DNS_TYPE_PTR || !record->Data.PTR.pNameHost)
				continue;
			char full[DNS_MAX_NAME_BUFFER_LENGTH * 4];
			char label[CRNET_SERVICE_NAME_CAPACITY];
			if (!crnet_win_utf8(record->Data.PTR.pNameHost, full, sizeof(full)))
				continue;
			size_t used      = 0;
			const size_t got = crnet_dns_label_unescape(full, strlen(full), label, sizeof(label) - 1, &used);
			if (got == SIZE_MAX)
				continue;
			label[got] = '\0';
			// Only an instance of the type browsed, whose name an advert
			// could carry (report_found checks that).
			if (!crnet_discovery_full_name_matches(full, label, object->type))
				continue;
			crnet_discovery_report_found(object, record->dwTtl != 0, 0, label, NULL);
		}
	} else {
		// The browse's last callback: its cancel's ERROR_CANCELLED, or a
		// failure that ends it.
		EnterCriticalSection(&state->lock);
		const bool stopping = state->stopping;
		LeaveCriticalSection(&state->lock);
		if (!stopping)
			crnet_win_dnssd_end(object, "DnsServiceBrowse", status);
	}
	if (records)
		state->calls.record_list_free(records, DnsFreeRecordList);
	crnet_win_dnssd_leave(state, status != ERROR_SUCCESS);
}

/* An address the resolve gave, as an event, if of a family asked for. */
static void crnet_win_dnssd_report_address(
    struct crnet_discovery_object *object,
    const void *sockaddr,
    size_t length,
    uint32_t interface_index,
    uint16_t port) {
	crnet_address address;
	if (!crnet_address_from_sockaddr(sockaddr, length, interface_index, &address, NULL))
		return;
	if ((object->family == CRNET_ADDRESS_FAMILY_IPV4 && address.family != CRNET_ADDRESS_FAMILY_IPV4) ||
	    (object->family == CRNET_ADDRESS_FAMILY_IPV6 && address.family != CRNET_ADDRESS_FAMILY_IPV6))
		return;
	address.port = port;
	crnet_discovery_report_address(object, true, &address);
}

/* The resolve's answer handed over: SERVICE, its TXT record written back
 * into wire form from Windows' keys and values ("key=value" each), then
 * its addresses. */
static void crnet_win_dnssd_report_resolved(
    struct crnet_discovery_object *object,
    const DNS_SERVICE_INSTANCE *answer) {
	char host[CRNET_SERVICE_HOST_CAPACITY];
	if (!crnet_win_utf8(answer->pszHostName, host, sizeof(host)))
		return;
	char full_text[DNS_MAX_NAME_BUFFER_LENGTH * 4];
	const char *full_name = crnet_win_utf8(answer->pszInstanceName, full_text, sizeof(full_text)) ? full_text : NULL;

	// The record's whole size is counted even past what the event holds,
	// as the other systems' is.
	uint8_t txt[CRNET_SERVICE_TXT_CAPACITY];
	size_t txt_size = 0;
	for (DWORD i = 0; i < answer->dwPropertyCount; ++i) {
		char entry[256 * 2];
		int key = WideCharToMultiByte(CP_UTF8, 0, answer->keys[i] ? answer->keys[i] : L"", -1, entry, 256, NULL, NULL);
		if (key <= 0)
			continue;
		key -= 1; // without its terminator
		entry[key] = '=';
		int value  = WideCharToMultiByte(
		    CP_UTF8, 0, answer->values && answer->values[i] ? answer->values[i] : L"", -1, entry + key + 1, (int)sizeof(entry) - key - 1, NULL, NULL);
		if (value <= 0)
			continue;
		size_t length = (size_t)key + 1 + (size_t)(value - 1);
		if (length > 255)
			length = 255; // an entry is at most 255 bytes in wire form
		if (txt_size + 1 + length <= sizeof(txt)) {
			txt[txt_size] = (uint8_t)length;
			memcpy(txt + txt_size + 1, entry, length);
		}
		txt_size += 1 + length;
	}
	if (!crnet_discovery_report_service(object, full_name, host, answer->wPort, txt, txt_size))
		return;
	if (answer->ip4Address) {
		struct sockaddr_in in;
		memset(&in, 0, sizeof(in));
		in.sin_family      = AF_INET;
		in.sin_addr.s_addr = *answer->ip4Address; // IP4_ADDRESS is in network order
		crnet_win_dnssd_report_address(object, &in, sizeof(in), answer->dwInterfaceIndex, answer->wPort);
	}
	if (answer->ip6Address) {
		struct sockaddr_in6 in6;
		memset(&in6, 0, sizeof(in6));
		in6.sin6_family = AF_INET6;
		memcpy(&in6.sin6_addr, answer->ip6Address, sizeof(in6.sin6_addr));
		crnet_win_dnssd_report_address(object, &in6, sizeof(in6), answer->dwInterfaceIndex, answer->wPort);
	}
}

static VOID WINAPI crnet_win_dnssd_resolved(
    DWORD status,
    PVOID context,
    PDNS_SERVICE_INSTANCE answer) {
	struct crnet_win_dnssd *state         = (struct crnet_win_dnssd *)context;
	struct crnet_discovery_object *object = state->object;
	crnet_win_dnssd_enter(state);
	EnterCriticalSection(&state->lock);
	const bool stopping = state->stopping;
	LeaveCriticalSection(&state->lock);
	// One answer, the resolve over whatever it says; its cancel's
	// ERROR_CANCELLED reports nothing.
	if (!stopping) {
		if (status == ERROR_SUCCESS && answer)
			crnet_win_dnssd_report_resolved(object, answer);
		else
			crnet_win_dnssd_end(object, "DnsServiceResolve", status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status);
	}
	if (answer)
		state->calls.free_instance(answer);
	crnet_win_dnssd_leave(state, true);
}

// --------------------------------------------------------------------------
// The verbs
// --------------------------------------------------------------------------

/* Marks the operation begun, before the call that begins it (its callback
 * may come on another thread before that call returns), or not begun
 * again when the call refused it (no callback will come). */
static void crnet_win_dnssd_set_begun(
    struct crnet_win_dnssd *state,
    bool begun) {
	EnterCriticalSection(&state->lock);
	state->begun = begun;
	LeaveCriticalSection(&state->lock);
}

/* An advert's TXT record from the front end's wire form as Windows' keys
 * and values, each entry "key=value" (the front end checked it), in one
 * block from the object's allocator: the pointers, then the characters. */
struct crnet_win_txt {
	void *block;
	size_t size;
	PCWSTR *keys;
	PCWSTR *values;
	DWORD count;
};

static bool crnet_win_txt_make(
    const CR_ALLOCATOR *allocator,
    const uint8_t *txt,
    size_t txt_size,
    struct crnet_win_txt *out) {
	memset(out, 0, sizeof(*out));
	DWORD count = 0;
	for (size_t at = 0; at < txt_size; at += 1 + txt[at])
		count++;
	if (count == 0)
		return true;
	// Every byte at most one UTF-16 unit, and a terminator for each half.
	const size_t characters = txt_size + 2u * count;
	out->size               = 2u * count * sizeof(PCWSTR) + characters * sizeof(WCHAR);
	out->block              = cr_allocate(allocator, out->size, sizeof(void *));
	if (!out->block)
		return false;
	out->keys   = (PCWSTR *)out->block;
	out->values = out->keys + count;
	WCHAR *text = (WCHAR *)(out->values + count);
	size_t used = 0;
	size_t at   = 0;
	for (DWORD i = 0; i < count; ++i) {
		const char *entry   = (const char *)txt + at + 1;
		const size_t length = txt[at];
		at += 1 + length;
		const char *equals      = (const char *)memchr(entry, '=', length);
		const size_t key_length = equals ? (size_t)(equals - entry) : length;
		const char *value       = equals ? equals + 1 : entry + length;
		const size_t value_len  = equals ? length - key_length - 1 : 0;
		WCHAR *key_out          = text + used;
		size_t local            = 0;
		crnet_win_wide_append(key_out, characters - used, &local, entry, key_length);
		used += local + 1;
		WCHAR *value_out = text + used;
		local            = 0;
		crnet_win_wide_append(value_out, characters - used, &local, value, value_len);
		used += local + 1;
		out->keys[i]   = key_out;
		out->values[i] = value_out;
	}
	out->count = count;
	return true;
}

CR_RESULT crnet_discovery_backend_advert_start(
    struct crnet_discovery_object *object,
    const uint8_t *txt,
    size_t txt_size,
    crnet_error *out_error) {
	const CR_RESULT made = crnet_win_dnssd_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_win_dnssd *state = crnet_win_dnssd_of(object);

	WCHAR full_name[DNS_MAX_NAME_BUFFER_LENGTH];
	if (!crnet_win_full_name(object->name, object->type, full_name, DNS_MAX_NAME_BUFFER_LENGTH))
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_FAILED, "the service's full name", ERROR_INVALID_NAME);
	// This machine's DNS host name in "local", which Windows' responder
	// answers for.
	WCHAR host[DNS_MAX_NAME_BUFFER_LENGTH];
	DWORD host_length = DNS_MAX_NAME_BUFFER_LENGTH - 7;
	if (!GetComputerNameExW(ComputerNameDnsHostname, host, &host_length))
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_FAILED, "GetComputerNameExW", GetLastError());
	memcpy(host + host_length, L".local", 7 * sizeof(WCHAR));

	struct crnet_win_txt entries;
	if (!crnet_win_txt_make(&object->head.allocator, txt, txt_size, &entries))
		return CR_RESULT_OUT_OF_MEMORY;
	state->instance         = state->calls.construct_instance(full_name, host, NULL, NULL, object->port, 0, 0, entries.count, entries.keys, entries.values);
	const DWORD constructed = GetLastError();
	if (entries.block)
		cr_free(&object->head.allocator, entries.block, entries.size, sizeof(void *));
	if (!state->instance)
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_FAILED, "DnsServiceConstructInstance", constructed);

	state->request.Version                     = DNS_QUERY_REQUEST_VERSION1;
	state->request.InterfaceIndex              = object->interface_index;
	state->request.pServiceInstance            = state->instance;
	state->request.pRegisterCompletionCallback = crnet_win_dnssd_registered;
	state->request.pQueryContext               = state;
	state->request.hCredentials                = NULL;
	state->request.unicastEnabled              = FALSE; // mDNS only
	crnet_win_dnssd_set_begun(state, true);
	const DWORD started = state->calls.register_service(&state->request, &state->cancel);
	if (started != DNS_REQUEST_PENDING) {
		crnet_win_dnssd_set_begun(state, false);
		return crnet_win_dnssd_start_failed(
		    out_error, started == ERROR_ACCESS_DENIED ? CR_RESULT_NET_DISCOVERY_DENIED : CR_RESULT_NET_DISCOVERY_FAILED, "DnsServiceRegister", started);
	}
	return CR_SUCCESS;
}

CR_RESULT crnet_discovery_backend_browser_start(
    struct crnet_discovery_object *object,
    crnet_error *out_error) {
	const CR_RESULT made = crnet_win_dnssd_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_win_dnssd *state = crnet_win_dnssd_of(object);
	size_t used                   = 0;
	if (!crnet_win_wide_append(state->query, DNS_MAX_NAME_BUFFER_LENGTH, &used, object->type, strlen(object->type)) ||
	    !crnet_win_wide_append(state->query, DNS_MAX_NAME_BUFFER_LENGTH, &used, ".local", 6))
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_FAILED, "the browse's name", ERROR_INVALID_NAME);
	DNS_SERVICE_BROWSE_REQUEST request;
	memset(&request, 0, sizeof(request));
	request.Version         = DNS_QUERY_REQUEST_VERSION1;
	request.InterfaceIndex  = object->interface_index;
	request.QueryName       = state->query;
	request.pBrowseCallback = crnet_win_dnssd_browsed;
	request.pQueryContext   = state;
	crnet_win_dnssd_set_begun(state, true);
	const DNS_STATUS started = state->calls.browse(&request, &state->cancel);
	if (started != DNS_REQUEST_PENDING) {
		crnet_win_dnssd_set_begun(state, false);
		return crnet_win_dnssd_start_failed(
		    out_error, started == ERROR_ACCESS_DENIED ? CR_RESULT_NET_DISCOVERY_DENIED : CR_RESULT_NET_DISCOVERY_FAILED, "DnsServiceBrowse", (DWORD)started);
	}
	return CR_SUCCESS;
}

CR_RESULT crnet_discovery_backend_resolver_start(
    struct crnet_discovery_object *object,
    crnet_error *out_error) {
	const CR_RESULT made = crnet_win_dnssd_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_win_dnssd *state = crnet_win_dnssd_of(object);
	if (!crnet_win_full_name(object->name, object->type, state->query, DNS_MAX_NAME_BUFFER_LENGTH))
		return crnet_win_dnssd_start_failed(out_error, CR_RESULT_NET_DISCOVERY_FAILED, "the service's full name", ERROR_INVALID_NAME);
	DNS_SERVICE_RESOLVE_REQUEST request;
	memset(&request, 0, sizeof(request));
	request.Version                    = DNS_QUERY_REQUEST_VERSION1;
	request.InterfaceIndex             = object->interface_index;
	request.QueryName                  = state->query;
	request.pResolveCompletionCallback = crnet_win_dnssd_resolved;
	request.pQueryContext              = state;
	crnet_win_dnssd_set_begun(state, true);
	const DNS_STATUS started = state->calls.resolve(&request, &state->cancel);
	if (started != DNS_REQUEST_PENDING) {
		crnet_win_dnssd_set_begun(state, false);
		return crnet_win_dnssd_start_failed(
		    out_error, started == ERROR_ACCESS_DENIED ? CR_RESULT_NET_DISCOVERY_DENIED : CR_RESULT_NET_DISCOVERY_FAILED, "DnsServiceResolve", (DWORD)started);
	}
	return CR_SUCCESS;
}

/* Asks the system to end the operation if its last callback hasn't come,
 * then waits until a destroy may go on (crnet_win_dnssd_released). */
static void crnet_win_dnssd_end_operation(struct crnet_win_dnssd *state) {
	EnterCriticalSection(&state->lock);
	state->stopping       = true;
	const bool running    = state->begun && !state->last_came;
	const bool registered = state->registered;
	const bool released   = crnet_win_dnssd_released(state);
	state->deregistering  = running && state->object->kind == CRNET_DISCOVERY_ADVERT && registered;
	ResetEvent(state->done);
	LeaveCriticalSection(&state->lock);
	if (running) {
		switch (state->object->kind) {
		case CRNET_DISCOVERY_ADVERT:
			// Registered: its goodbye, answered by the register callback.
			// Not yet: the registration cancelled, which a callback also
			// answers (one that came through meanwhile is deregistered by
			// the destroy).
			if (registered) {
				if (state->calls.deregister_service(&state->request, NULL) != DNS_REQUEST_PENDING) {
					// Refused at once: no callback will come for it.
					EnterCriticalSection(&state->lock);
					state->last_came = true;
					LeaveCriticalSection(&state->lock);
				}
			} else {
				state->calls.register_cancel(&state->cancel);
			}
			break;
		case CRNET_DISCOVERY_BROWSER:
			state->calls.browse_cancel(&state->cancel);
			break;
		case CRNET_DISCOVERY_RESOLVER:
			state->calls.resolve_cancel(&state->cancel);
			break;
		}
	}
	if (released)
		return;
	EnterCriticalSection(&state->lock);
	const bool now = crnet_win_dnssd_released(state);
	LeaveCriticalSection(&state->lock);
	if (!now)
		WaitForSingleObject(state->done, INFINITE);
}

void crnet_discovery_backend_stop(struct crnet_discovery_object *object) {
	struct crnet_win_dnssd *state = crnet_win_dnssd_of(object);
	if (!state)
		return;
	if (state->lock_made) {
		crnet_win_dnssd_end_operation(state);
		// A registration that came through as its cancel was asked for:
		// deregistered, and waited for the same way.
		if (object->kind == CRNET_DISCOVERY_ADVERT && state->registered && !state->deregistering) {
			EnterCriticalSection(&state->lock);
			state->begun     = true;
			state->last_came = false;
			LeaveCriticalSection(&state->lock);
			crnet_win_dnssd_end_operation(state);
		}
	}
	if (state->instance)
		state->calls.free_instance(state->instance);
	if (state->library)
		FreeLibrary(state->library);
	if (state->news)
		CloseHandle(state->news);
	if (state->abort)
		CloseHandle(state->abort);
	if (state->done)
		CloseHandle(state->done);
	if (state->lock_made)
		DeleteCriticalSection(&state->lock);
	object->backend = NULL;
	CR_DELETE(&object->head.allocator, struct crnet_win_dnssd, state);
}

void crnet_discovery_backend_wake(struct crnet_discovery_object *object) {
	struct crnet_win_dnssd *state = crnet_win_dnssd_of(object);
	if (state && state->abort)
		SetEvent(state->abort);
}

/* Milliseconds since `start` on the performance counter. */
static ULONGLONG crnet_win_dnssd_elapsed_ms(
    const LARGE_INTEGER *start,
    const LARGE_INTEGER *frequency) {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (ULONGLONG)(now.QuadPart - start->QuadPart) * 1000u / (ULONGLONG)frequency->QuadPart;
}

CR_RESULT crnet_discovery_backend_pump(
    struct crnet_discovery_object *object,
    uint32_t timeout_ms) {
	struct crnet_win_dnssd *state = crnet_win_dnssd_of(object);
	if (crnet_atomic_load32(&object->aborted))
		return CR_RESULT_NET_ABORTED;
	// WaitForMultipleObjects may end a little before its time (49 ms of 50
	// seen), and CR_RESULT_NET_TIMED_OUT means the whole limit has gone
	// (crnet.h, "Time limits"): after a time-out the rest is waited out,
	// counted on the performance counter from the pump's start.
	LARGE_INTEGER frequency, start;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&start);
	DWORD wait = timeout_ms == CRNET_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms;
	for (;;) {
		// Abort first, so it wins when both are set.
		HANDLE waits[2]    = {state->abort, state->news};
		const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, wait);
		if (waited == WAIT_OBJECT_0 || crnet_atomic_load32(&object->aborted))
			return CR_RESULT_NET_ABORTED;
		if (waited == WAIT_OBJECT_0 + 1)
			return CR_SUCCESS;
		if (timeout_ms == CRNET_WAIT_FOREVER)
			continue;
		const ULONGLONG elapsed = crnet_win_dnssd_elapsed_ms(&start, &frequency);
		if (elapsed >= timeout_ms)
			return CR_RESULT_NET_TIMED_OUT;
		wait = (DWORD)(timeout_ms - elapsed);
	}
}

void crnet_discovery_backend_wait_prepare(
    struct crnet_discovery_object *object,
    struct crnet_wait_poll *poll) {
	// Nothing to poll: the callbacks wake the set themselves.
	(void)object;
	(void)poll;
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_DISCOVERY

#endif // CARROTTUS_PLATFORM_WIN32
