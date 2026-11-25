/*
 * Service discovery's own open of dnsapi.dll on Windows (crnet.h, "Service
 * discovery"; src/net/backends/windows/crnetwindnssd.c): each object
 * opens the library at its create and closes it at its destroy, holding
 * nothing between objects.
 *
 * Each case runs in a process of its own, which loads nothing of
 * Carrottus Net's before it starts (Winsock's resolver and WinHTTP may load
 * dnsapi.dll themselves), so ctest runs this once per case:
 *
 *   crnet_discovery_library_test start    whether dnsapi.dll is loaded at
 *                                          the start and after an instance
 *   crnet_discovery_library_test timing   a create timed with the library
 *                                          not loaded (the first load) and
 *                                          with another object holding it
 *   crnet_discovery_library_test cycles   the last object destroyed, then
 *                                          another created, twenty times
 *   crnet_discovery_library_test three    two browsers and a resolver at
 *                                          once, each opening it, the last
 *                                          destroy closing it
 *   crnet_discovery_library_test missing  (the test build whose library
 *                                          name points elsewhere) each
 *                                          create UNAVAILABLE
 *   crnet_discovery_library_test bare     a load and free of dnsapi.dll
 *                                          with no call made leaves it
 *                                          unloaded
 *   crnet_discovery_library_test pinned   dnsapi.dll's own calls, no
 *                                          Carrottus: one browse begun
 *                                          and cancelled, the library freed
 *
 * dnsapi.dll keeps itself loaded once one of its DNS-SD calls has run, in
 * a process that frees it as often as it loads it ("pinned" shows it with
 * no Carrottus, Windows 11 build 26300, 2026-11-24). So once a discovery
 * object has begun its operation, the library stays loaded after the last
 * destroy whatever the objects do: the cases say so rather than check it,
 * and "bare" checks that a balanced load and free does unload it.
 */

#include <crnet.h>
#include <crperf.h>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
// clang-format on

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

static void check(
    bool ok,
    const char *what) {
	++g_checks;
	if (!ok)
		++g_failures;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

static bool dnsapi_loaded(void) {
	HMODULE module = NULL;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"dnsapi.dll", &module) && module;
}

static crnet_instance make_instance(void) {
	const CR_ALLOCATOR allocator = cr_allocator_system();
	crnet_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	desc.allocator          = &allocator;
	crnet_instance instance = NULL;
	if (crnet_instance_create(&desc, &instance, NULL) != CR_SUCCESS) {
		printf("  no instance\n");
		exit(1);
	}
	return instance;
}

static crnet_service_browser_desc browser_desc(void) {
	crnet_service_browser_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = CR_STRUCT_TYPE_NET_SERVICE_BROWSER_DESC;
	desc.type           = "_crnetnobody._udp";
	desc.scope          = CRNET_DISCOVERY_SCOPE_ALL_INTERFACES;
	desc.event_capacity = 8;
	return desc;
}

static crnet_service_resolver_desc resolver_desc(void) {
	crnet_service_resolver_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = CR_STRUCT_TYPE_NET_SERVICE_RESOLVER_DESC;
	desc.name           = "crnettest-library";
	desc.type           = "_crnetnobody._udp";
	desc.family         = CRNET_ADDRESS_FAMILY_ANY;
	desc.event_capacity = 8;
	return desc;
}

static crnet_service_advert_desc advert_desc(void) {
	crnet_service_advert_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC;
	desc.name      = "crnettest-library";
	desc.type      = "_crnetnobody._udp";
	desc.port      = 9;
	desc.scope     = CRNET_DISCOVERY_SCOPE_ALL_INTERFACES;
	return desc;
}

static HANDLE g_cancelled;

static VOID WINAPI pinned_browsed(
    DWORD status,
    PVOID context,
    PDNS_RECORD records) {
	(void)context;
	(void)records;
	if (status == ERROR_CANCELLED)
		SetEvent(g_cancelled);
}

static void case_bare(bool at_start) {
	HMODULE library = LoadLibraryExW(L"dnsapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	check(library != NULL && dnsapi_loaded(), "dnsapi.dll loaded");
	FreeLibrary(library);
	if (!at_start)
		check(!dnsapi_loaded(), "freed with no call made: unloaded");
}

static void case_pinned(bool at_start) {
	typedef DNS_STATUS(WINAPI * browse_fn)(PDNS_SERVICE_BROWSE_REQUEST, PDNS_SERVICE_CANCEL);
	typedef DNS_STATUS(WINAPI * cancel_fn)(PDNS_SERVICE_CANCEL);
	HMODULE library  = LoadLibraryExW(L"dnsapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	browse_fn browse = library ? (browse_fn)(void *)GetProcAddress(library, "DnsServiceBrowse") : NULL;
	cancel_fn cancel = library ? (cancel_fn)(void *)GetProcAddress(library, "DnsServiceBrowseCancel") : NULL;
	check(browse && cancel, "dnsapi.dll's browse and its cancel");
	if (!browse || !cancel)
		return;
	g_cancelled               = CreateEventW(NULL, TRUE, FALSE, NULL);
	DNS_SERVICE_CANCEL handle = {0};
	DNS_SERVICE_BROWSE_REQUEST request;
	memset(&request, 0, sizeof(request));
	request.Version         = DNS_QUERY_REQUEST_VERSION1;
	request.QueryName       = L"_crnetnobody._udp.local";
	request.pBrowseCallback = pinned_browsed;
	check(browse(&request, &handle) == DNS_REQUEST_PENDING, "a browse begun");
	check(cancel(&handle) == ERROR_SUCCESS && WaitForSingleObject(g_cancelled, 5000) == WAIT_OBJECT_0, "cancelled, its last callback come");
	FreeLibrary(library);
	CloseHandle(g_cancelled);
	if (!at_start)
		printf("  freed as often as loaded, after a DNS-SD call: dnsapi.dll %s\n", dnsapi_loaded() ? "still loaded (it keeps itself)" : "unloaded");
}

static uint64_t now_us(void) { return crperf_get_monotonic_time_ns() / 1000u; }

#ifdef NDEBUG
#define BUILD_NAME "Release"
#else
#define BUILD_NAME "Debug"
#endif

static void case_start(void) {
	const bool at_start = dnsapi_loaded();
	printf("  dnsapi.dll loaded at the process's start: %s\n", at_start ? "yes" : "no");
	crnet_instance instance = make_instance();
	printf("  dnsapi.dll loaded once an instance is made (Winsock started): %s\n", dnsapi_loaded() ? "yes" : "no");
	crnet_instance_destroy(instance);
	check(true, "the starting state recorded");
}

static void case_timing(bool at_start) {
	crnet_instance instance         = make_instance();
	const bool before               = dnsapi_loaded();
	crnet_service_browser_desc desc = browser_desc();
	crnet_service_browser first     = NULL;
	uint64_t start                  = now_us();
	CR_RESULT result                = crnet_service_browser_create(instance, &desc, &first, NULL);
	const uint64_t cold             = now_us() - start;
	check(result == CR_SUCCESS, "a browser with no other object holding the library");
	printf("  create with no other object (library loaded before: %s): %llu us (%s)\n", before ? "yes" : "no", (unsigned long long)cold, BUILD_NAME);
	uint64_t total = 0, most = 0, least = UINT64_MAX;
	for (int i = 0; i < 10; ++i) {
		crnet_service_browser second = NULL;
		start                        = now_us();
		result                       = crnet_service_browser_create(instance, &desc, &second, NULL);
		const uint64_t took          = now_us() - start;
		if (result != CR_SUCCESS)
			break;
		total += took;
		most  = took > most ? took : most;
		least = took < least ? took : least;
		crnet_service_browser_destroy(second);
	}
	check(result == CR_SUCCESS, "ten browsers while another holds the library");
	printf(
	    "  create with another object holding it: mean %llu us, least %llu, most %llu over 10 (%s)\n", (unsigned long long)(total / 10),
	    (unsigned long long)least, (unsigned long long)most, BUILD_NAME);
	crnet_service_browser_destroy(first);
	if (!at_start)
		printf("  after the last destroy: dnsapi.dll %s\n", dnsapi_loaded() ? "still loaded (it keeps itself once used)" : "unloaded");
	crnet_instance_destroy(instance);
}

static void case_cycles(bool at_start) {
	crnet_instance instance         = make_instance();
	crnet_service_browser_desc desc = browser_desc();
	int made = 0, loaded_while = 0, unloaded_after = 0;
	for (int i = 0; i < 20; ++i) {
		crnet_service_browser browser = NULL;
		if (crnet_service_browser_create(instance, &desc, &browser, NULL) != CR_SUCCESS)
			break;
		made++;
		loaded_while += dnsapi_loaded() ? 1 : 0;
		crnet_service_browser_destroy(browser);
		unloaded_after += dnsapi_loaded() ? 0 : 1;
	}
	check(made == 20, "twenty creates in a row, each after the last destroy");
	check(loaded_while == 20, "the library loaded while each lived");
	printf(
	    "  dnsapi.dll unloaded after %d of the 20 destroys (%s)\n", unloaded_after,
	    at_start ? "it was loaded at the start" : "it keeps itself loaded once used");
	crnet_instance_destroy(instance);
}

static void case_three(bool at_start) {
	crnet_instance instance           = make_instance();
	crnet_service_browser_desc bdesc  = browser_desc();
	crnet_service_resolver_desc rdesc = resolver_desc();
	crnet_service_browser a = NULL, b = NULL;
	crnet_service_resolver r = NULL;
	check(crnet_service_browser_create(instance, &bdesc, &a, NULL) == CR_SUCCESS, "a first browser");
	check(crnet_service_browser_create(instance, &bdesc, &b, NULL) == CR_SUCCESS, "a second browser");
	check(crnet_service_resolver_create(instance, &rdesc, &r, NULL) == CR_SUCCESS, "a resolver");
	check(dnsapi_loaded(), "the library loaded with three objects");
	crnet_service_browser_destroy(a);
	check(dnsapi_loaded(), "still loaded with two left");
	crnet_service_resolver_destroy(r);
	check(dnsapi_loaded(), "still loaded with one left");
	crnet_service_browser_destroy(b);
	if (!at_start)
		printf("  after the last destroy: dnsapi.dll %s\n", dnsapi_loaded() ? "still loaded (it keeps itself once used)" : "unloaded");
	crnet_instance_destroy(instance);
}

static void case_missing(void) {
	crnet_instance instance           = make_instance();
	crnet_service_advert_desc adesc   = advert_desc();
	crnet_service_browser_desc bdesc  = browser_desc();
	crnet_service_resolver_desc rdesc = resolver_desc();
	crnet_service_advert a            = NULL;
	crnet_service_browser b           = NULL;
	crnet_service_resolver r          = NULL;
	crnet_error error;
	check(crnet_service_advert_create(instance, &adesc, &a, &error) == CR_RESULT_NET_DISCOVERY_UNAVAILABLE && !a, "an advert: UNAVAILABLE");
	printf("  its record: code %lld, %s\n", (long long)error.code, error.text);
	check(crnet_service_browser_create(instance, &bdesc, &b, NULL) == CR_RESULT_NET_DISCOVERY_UNAVAILABLE && !b, "a browser: UNAVAILABLE");
	check(crnet_service_resolver_create(instance, &rdesc, &r, NULL) == CR_RESULT_NET_DISCOVERY_UNAVAILABLE && !r, "a resolver: UNAVAILABLE");
	crnet_instance_destroy(instance);
}

int main(
    int argc,
    char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const bool at_start = dnsapi_loaded();
	const char *which   = argc > 1 ? argv[1] : "";
	printf("crnet_discovery_library_test %s (dnsapi.dll loaded at the start: %s)\n", which, at_start ? "yes" : "no");
	if (strcmp(which, "start") == 0)
		case_start();
	else if (strcmp(which, "timing") == 0)
		case_timing(at_start);
	else if (strcmp(which, "cycles") == 0)
		case_cycles(at_start);
	else if (strcmp(which, "three") == 0)
		case_three(at_start);
	else if (strcmp(which, "missing") == 0)
		case_missing();
	else if (strcmp(which, "bare") == 0)
		case_bare(at_start);
	else if (strcmp(which, "pinned") == 0)
		case_pinned(at_start);
	else {
		printf("  usage: crnet_discovery_library_test start|timing|cycles|three|missing|bare|pinned\n");
		return 2;
	}
	printf("%d of %d checks passed\n", g_checks - g_failures, g_checks);
	return g_failures == 0 ? 0 : 1;
}
