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
 *
 * On Linux (src/net/backends/linux/crnetavahi.c) the libraries are
 * libavahi-client.so.3, libavahi-common.so.3 and libdbus-1.so.3 below
 * them, read from /proc/self/maps. The same cases but "bare" and
 * "pinned", and Avahi's own:
 *
 *   crnet_discovery_library_test txt       a record avahi-publish puts on
 *                                           the wire resolved: its entries'
 *                                           order kept, and one longer than
 *                                           CRNET_SERVICE_TXT_CAPACITY cut
 *                                           to its first bytes (77 without
 *                                           avahi-publish)
 *   crnet_discovery_library_test vanished  a resolve of an instance gone,
 *                                           and of one never there:
 *                                           NOT_ANSWERING, timed
 *   crnet_discovery_library_test timer     a set holding an idle browser
 *                                           waits its whole limit without
 *                                           spinning, and names a resolver
 *                                           when its answer comes
 *   crnet_discovery_library_test restart   avahi-daemon restarted under a
 *                                           browser: DISCOVERY_FAILED
 *   crnet_discovery_library_test stopped   avahi-daemon stopped: every
 *                                           create UNAVAILABLE
 *
 * The backend opens both libraries RTLD_NODELETE (libdbus loses the memory
 * in its globals at every unload), so once one object has opened them they
 * stay mapped, with libdbus: "cycles" checks so, and the rest say it.
 *
 * The last two stop or restart the system's daemon through
 * `sudo -n systemctl`, and skip (77) for a user who may not.
 */

#include <crnet.h>
#include <crperf.h>

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
// clang-format on
#else
#include <net/if.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What ctest takes as a skip. */
#define SKIPPED 77

static int g_failures = 0;
static int g_checks   = 0;

static void check(
    bool ok,
    const char *what,
    ...) {
	++g_checks;
	if (!ok)
		++g_failures;
	printf("  [%s] ", ok ? "PASS" : "FAIL");
	va_list args;
	va_start(args, what);
	vprintf(what, args);
	va_end(args);
	printf("\n");
}

#ifdef _WIN32
static bool dnsapi_loaded(void) {
	HMODULE module = NULL;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"dnsapi.dll", &module) && module;
}
#define LIBRARY_NAME "dnsapi.dll"
#else
/* Whether a library whose file name contains `name` is mapped now. */
static bool mapped(const char *name) {
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return false;
	char line[1024];
	bool found = false;
	while (!found && fgets(line, sizeof(line), maps))
		found = strstr(line, name) != NULL;
	fclose(maps);
	return found;
}
static bool dnsapi_loaded(void) { return mapped("libavahi-client.so"); }
static bool dbus_loaded(void) { return mapped("libdbus-1.so"); }
#define LIBRARY_NAME "libavahi-client.so.3"
#endif

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

#ifdef _WIN32
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

#endif // _WIN32

static uint64_t now_us(void) { return crperf_get_monotonic_time_ns() / 1000u; }

#ifdef NDEBUG
#define BUILD_NAME "Release"
#else
#define BUILD_NAME "Debug"
#endif

static void case_start(void) {
	const bool at_start = dnsapi_loaded();
	printf("  " LIBRARY_NAME " loaded at the process's start: %s\n", at_start ? "yes" : "no");
	crnet_instance instance = make_instance();
	printf("  " LIBRARY_NAME " loaded once an instance is made: %s\n", dnsapi_loaded() ? "yes" : "no");
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
		printf("  after the last destroy: " LIBRARY_NAME " %s\n", dnsapi_loaded() ? "still loaded" : "unloaded");
	crnet_instance_destroy(instance);
}

static void case_cycles(bool at_start) {
	crnet_instance instance         = make_instance();
	crnet_service_browser_desc desc = browser_desc();
	int made = 0, loaded_while = 0, unloaded_after = 0, dbus_unloaded = 0;
	(void)dbus_unloaded;
	for (int i = 0; i < 20; ++i) {
		crnet_service_browser browser = NULL;
		if (crnet_service_browser_create(instance, &desc, &browser, NULL) != CR_SUCCESS)
			break;
		made++;
		loaded_while += dnsapi_loaded() ? 1 : 0;
		crnet_service_browser_destroy(browser);
		unloaded_after += dnsapi_loaded() ? 0 : 1;
#ifndef _WIN32
		dbus_unloaded += dbus_loaded() ? 0 : 1;
#endif
	}
	check(made == 20, "twenty creates in a row, each after the last destroy");
	check(loaded_while == 20, "the library loaded while each lived");
	printf(
	    "  " LIBRARY_NAME " unloaded after %d of the 20 destroys (%s)\n", unloaded_after, at_start ? "it was loaded at the start" : "loaded by nothing else");
#ifndef _WIN32
	printf("  libdbus-1.so unloaded after %d of the 20 destroys\n", dbus_unloaded);
	if (!at_start)
		check(unloaded_after == 0, "opened RTLD_NODELETE: the library stays mapped after the last destroy");
#endif
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
		printf("  after the last destroy: " LIBRARY_NAME " %s\n", dnsapi_loaded() ? "still loaded" : "unloaded");
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

#ifndef _WIN32

/* Whether `sudo -n systemctl` may be used: the daemon cases skip without. */
static bool may_control_daemon(void) { return system("sudo -n true >/dev/null 2>&1") == 0; }

static int run_daemon(const char *verb) {
	char command[128];
	snprintf(command, sizeof(command), "sudo -n systemctl %s avahi-daemon.socket avahi-daemon.service", verb);
	return system(command);
}

static crnet_service_resolve_event resolve_event(void) {
	crnet_service_resolve_event event;
	memset(&event, 0, sizeof(event));
	event.base.type = CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT;
	return event;
}

static crnet_service_browse_event browse_event(void) {
	crnet_service_browse_event event;
	memset(&event, 0, sizeof(event));
	event.base.type = CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT;
	return event;
}

/* Starts avahi-publish with `argv`; 0 when it can't be. */
static pid_t publish(char *const argv[]) {
	pid_t pid = 0;
	if (posix_spawnp(&pid, "avahi-publish", NULL, NULL, argv, environ) != 0)
		return 0;
	return pid;
}

static void publish_end(pid_t pid) {
	if (pid <= 0)
		return;
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
}

/* The first SERVICE a resolver of `name` of `type` gives within `limit_ms`,
 * or false. */
static bool resolve_service(
    crnet_instance instance,
    const char *name,
    const char *type,
    uint32_t interface_index,
    uint32_t limit_ms,
    crnet_service_resolve_event *out) {
	crnet_service_resolver_desc desc = resolver_desc();
	desc.name                        = name;
	desc.type                        = type;
	desc.interface_index             = interface_index;
	crnet_service_resolver resolver  = NULL;
	if (crnet_service_resolver_create(instance, &desc, &resolver, NULL) != CR_SUCCESS)
		return false;
	bool got             = false;
	const uint64_t start = now_us();
	while (!got && now_us() - start < (uint64_t)limit_ms * 1000u) {
		*out                   = resolve_event();
		const CR_RESULT result = crnet_service_resolver_next(resolver, 100, out);
		if (result == CR_RESULT_NET_TIMED_OUT)
			continue;
		if (result != CR_SUCCESS)
			break;
		got = out->kind == CRNET_SERVICE_RESOLVE_EVENT_SERVICE;
	}
	crnet_service_resolver_destroy(resolver);
	return got;
}

static int case_txt(void) {
	if (system("command -v avahi-publish >/dev/null 2>&1") != 0) {
		printf("  SKIP: no avahi-publish (avahi-tools)\n");
		return SKIPPED;
	}
	crnet_instance instance = make_instance();
	char name[64];
	snprintf(name, sizeof(name), "crnettest-txt-%ld", (long)getpid());
	// A type of this process's own: a publisher left behind by a run that
	// was stopped (a sanitizer's halt) reaches no other test.
	char type[CRNET_SERVICE_TYPE_CAPACITY];
	snprintf(type, sizeof(type), "_cx%ld._udp", (long)(getpid() % 1000000));

	// Three entries, each its own length, in an order no sort would give.
	{
		char *const argv[] = {"avahi-publish", "-s", name, type, "9", "zz=3", "a=1", "mmm=22", NULL};
		const pid_t pid    = publish(argv);
		check(pid > 0, "avahi-publish started");
		crnet_service_resolve_event event;
		static const uint8_t wire[] = "\x04zz=3\x03"
		                              "a=1\x06mmm=22";
		const bool got              = resolve_service(instance, name, type, 0, 10000, &event);
		check(got, "resolved");
		check(
		    got && event.txt_size == sizeof(wire) - 1 && !event.txt_truncated && memcmp(event.txt, wire, sizeof(wire) - 1) == 0,
		    "the entries in the order they were published (%u bytes)", got ? event.txt_size : 0);
		publish_end(pid);
	}

	// No entry: the record is the one empty string on the wire.
	{
		char name_empty[80];
		snprintf(name_empty, sizeof(name_empty), "%s-empty", name);
		char *const argv[] = {"avahi-publish", "-s", name_empty, type, "9", NULL};
		const pid_t pid    = publish(argv);
		crnet_service_resolve_event event;
		const bool got = resolve_service(instance, name_empty, type, 0, 10000, &event);
		check(got && event.txt_size == 1 && event.txt[0] == 0, "no entry: one zero byte (%u bytes)", got ? event.txt_size : 0);
		publish_end(pid);
	}

	// Seven entries of 200 bytes: 1407 bytes, past the capacity.
	{
		char name_long[80];
		static char values[7][208];
		snprintf(name_long, sizeof(name_long), "%s-long", name);
		char *argv[16] = {"avahi-publish", "-s", name_long, type, "9"};
		uint8_t wire[7 * 201];
		size_t used = 0;
		for (int i = 0; i < 7; ++i) {
			memset(values[i], 'a' + i, 199);
			values[i][0]   = (char)('k');
			values[i][1]   = (char)('0' + i);
			values[i][2]   = '=';
			values[i][199] = '\0';
			argv[5 + i]    = values[i];
			wire[used]     = 199;
			memcpy(wire + used + 1, values[i], 199);
			used += 200;
		}
		argv[12]        = NULL;
		const pid_t pid = publish(argv);
		crnet_service_resolve_event event;
		// Over loopback: a record this long fits no packet of a 1500-byte
		// link, and avahi-daemon's resolve over one times out (Fedora 44,
		// Avahi 0.9-rc2, 2026-11-25; avahi-browse -r alike).
		const bool got = resolve_service(instance, name_long, type, if_nametoindex("lo"), 10000, &event);
		check(got && event.txt_size == used && event.txt_truncated, "a record of %zu bytes: its size, and truncated (%u)", used, got ? event.txt_size : 0);
		check(got && memcmp(event.txt, wire, CRNET_SERVICE_TXT_CAPACITY) == 0, "its first CRNET_SERVICE_TXT_CAPACITY bytes as on the wire");
		publish_end(pid);
	}
	crnet_instance_destroy(instance);
	return 0;
}

/* A resolver's end within `limit_ms`, and how long it took. */
static CR_RESULT resolve_end(
    crnet_instance instance,
    const char *name,
    uint32_t limit_ms,
    uint64_t *out_ms) {
	crnet_service_resolver_desc desc = resolver_desc();
	desc.name                        = name;
	crnet_service_resolver resolver  = NULL;
	const uint64_t start             = now_us();
	CR_RESULT result                 = crnet_service_resolver_create(instance, &desc, &resolver, NULL);
	while (result == CR_SUCCESS && now_us() - start < (uint64_t)limit_ms * 1000u) {
		crnet_service_resolve_event event = resolve_event();
		result                            = crnet_service_resolver_next(resolver, 200, &event);
		if (result == CR_RESULT_NET_TIMED_OUT)
			result = CR_SUCCESS;
	}
	*out_ms = (now_us() - start) / 1000u;
	crnet_service_resolver_destroy(resolver);
	return result;
}

static int case_vanished(void) {
	crnet_instance instance = make_instance();
	char name[64];
	snprintf(name, sizeof(name), "crnettest-gone-%ld", (long)getpid());
	uint64_t took = 0;

	// Never there.
	CR_RESULT result = resolve_end(instance, name, 30000, &took);
	check(
	    result == CR_RESULT_NET_SERVICE_NOT_ANSWERING, "an instance never there: NOT_ANSWERING (%s after %llu ms)", cr_result_str(result),
	    (unsigned long long)took);

	// There, then gone.
	crnet_service_advert_desc adesc = advert_desc();
	adesc.name                      = name;
	crnet_service_advert advert     = NULL;
	if (crnet_service_advert_create(instance, &adesc, &advert, NULL) == CR_SUCCESS) {
		crnet_service_advert_event event;
		memset(&event, 0, sizeof(event));
		event.base.type = CR_STRUCT_TYPE_NET_SERVICE_ADVERT_EVENT;
		crnet_service_advert_next(advert, 5000, &event);
		check(event.kind == CRNET_SERVICE_ADVERT_EVENT_REGISTERED, "an advert REGISTERED");
		crnet_service_advert_destroy(advert);
		result = resolve_end(instance, name, 30000, &took);
		check(
		    result == CR_RESULT_NET_SERVICE_NOT_ANSWERING, "the advert destroyed, a resolve of it: NOT_ANSWERING (%s after %llu ms)", cr_result_str(result),
		    (unsigned long long)took);
	}
	crnet_instance_destroy(instance);
	return 0;
}

static uint64_t cpu_us(void) {
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	return (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000u + (uint64_t)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
}

static int case_timer(void) {
	crnet_instance instance = make_instance();
	crnet_wait_set set      = NULL;
	crnet_wait_set_create(instance, &set, NULL);
	// A type of this process's own, so no other run's adverts reach it.
	char own_type[CRNET_SERVICE_TYPE_CAPACITY];
	snprintf(own_type, sizeof(own_type), "_ct%ld._udp", (long)(getpid() % 1000000));
	crnet_service_browser_desc bdesc = browser_desc();
	bdesc.type                       = own_type;
	crnet_service_browser browser    = NULL;
	check(crnet_service_browser_create(instance, &bdesc, &browser, NULL) == CR_SUCCESS, "an idle browser");
	int tag = 0;
	crnet_wait_set_add_service_browser(set, browser, &tag);
	// Past Avahi's CACHE_EXHAUSTED and ALL_FOR_NOW, each its own message
	// and no event, which may name the browser up to about 3 s after its
	// create (seen here); drained. By the clock: a wait named ends early.
	const uint64_t made = now_us();
	while (now_us() - made < 5000000u) {
		crnet_wait_event events[2];
		uint32_t count = 0;
		crnet_wait_set_wait(set, 500, events, 2, &count);
		crnet_service_browse_event event = browse_event();
		while (crnet_service_browser_next(browser, CRNET_NO_WAIT, &event) == CR_SUCCESS) {
		}
	}
	// Then a second of waiting. Each Avahi object's D-Bus connection hears
	// every program joining or leaving the system bus (avahi-client listens
	// to the bus's NameOwnerChanged), so on a busy bus the set may name the
	// browser with nothing to hand over: allowed ("a reason to try"). What
	// must hold is that no naming brings an event, and nothing spins.
	{
		const uint64_t cpu   = cpu_us();
		const uint64_t start = now_us();
		uint32_t named = 0, brought = 0;
		while (now_us() - start < 1000000u) {
			crnet_wait_event events[2];
			uint32_t count         = 0;
			const uint64_t left_ms = 1000u - (now_us() - start) / 1000u;
			const CR_RESULT result = crnet_wait_set_wait(set, (uint32_t)left_ms, events, 2, &count);
			if (result != CR_SUCCESS)
				continue;
			named++;
			crnet_service_browse_event event = browse_event();
			while (crnet_service_browser_next(browser, CRNET_NO_WAIT, &event) == CR_SUCCESS)
				brought++;
		}
		const uint64_t cpu_spent = cpu_us() - cpu;
		check(brought == 0, "an idle browser's set waited a second: named %u time(s), handing over nothing", named);
		check(cpu_spent < 20000, "without spinning (%llu us of CPU)", (unsigned long long)cpu_spent);
	}
	crnet_wait_set_remove_service_browser(set, browser);
	crnet_service_browser_destroy(browser);

	// A resolver of nobody in the set: named when Avahi's answer (its time
	// limit) comes, and its next says so.
	crnet_service_resolver_desc rdesc = resolver_desc();
	crnet_service_resolver resolver   = NULL;
	check(crnet_service_resolver_create(instance, &rdesc, &resolver, NULL) == CR_SUCCESS, "a resolver of nobody");
	crnet_wait_set_add_service_resolver(set, resolver, &tag);
	CR_RESULT ended      = CR_SUCCESS;
	uint32_t named       = 0;
	const uint64_t start = now_us();
	while (ended == CR_SUCCESS && now_us() - start < 30000000u) {
		crnet_wait_event events[2];
		uint32_t count = 0;
		if (crnet_wait_set_wait(set, 1000, events, 2, &count) != CR_SUCCESS || count == 0)
			continue;
		named++;
		crnet_service_resolve_event event = resolve_event();
		CR_RESULT got                     = CR_SUCCESS;
		while ((got = crnet_service_resolver_next(resolver, CRNET_NO_WAIT, &event)) == CR_SUCCESS) {
		}
		if (got != CR_RESULT_NET_TIMED_OUT)
			ended = got;
	}
	check(
	    ended == CR_RESULT_NET_SERVICE_NOT_ANSWERING, "the set named the resolver %u time(s), its next NOT_ANSWERING after %llu ms (%s)", named,
	    (unsigned long long)((now_us() - start) / 1000u), cr_result_str(ended));
	crnet_wait_set_remove_service_resolver(set, resolver);
	crnet_service_resolver_destroy(resolver);
	crnet_wait_set_destroy(set);
	crnet_instance_destroy(instance);
	return 0;
}

static int case_restart(void) {
	if (!may_control_daemon()) {
		printf("  SKIP: sudo -n may not restart avahi-daemon\n");
		return SKIPPED;
	}
	crnet_instance instance         = make_instance();
	crnet_service_browser_desc desc = browser_desc();
	crnet_service_browser browser   = NULL;
	check(crnet_service_browser_create(instance, &desc, &browser, NULL) == CR_SUCCESS, "a browser");
	check(run_daemon("restart") == 0, "avahi-daemon restarted");
	CR_RESULT result     = CR_SUCCESS;
	const uint64_t start = now_us();
	while ((result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT) && now_us() - start < 10000000u) {
		crnet_service_browse_event event = browse_event();
		result                           = crnet_service_browser_next(browser, 200, &event);
	}
	check(result == CR_RESULT_NET_DISCOVERY_FAILED, "its next: DISCOVERY_FAILED (%s)", cr_result_str(result));
	crnet_error error;
	crnet_service_browser_get_error(browser, &error);
	printf("  its record: code %lld, %s\n", (long long)error.code, error.text);
	crnet_service_browser_destroy(browser);
	crnet_instance_destroy(instance);
	return 0;
}

static int case_stopped(void) {
	if (!may_control_daemon()) {
		printf("  SKIP: sudo -n may not stop avahi-daemon\n");
		return SKIPPED;
	}
	check(run_daemon("stop") == 0, "avahi-daemon stopped");
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
	check(run_daemon("start") == 0, "avahi-daemon started again");
	return 0;
}

#endif // !_WIN32

int main(
    int argc,
    char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const bool at_start = dnsapi_loaded();
	const char *which   = argc > 1 ? argv[1] : "";
	printf("crnet_discovery_library_test %s (" LIBRARY_NAME " loaded at the start: %s)\n", which, at_start ? "yes" : "no");
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
#ifdef _WIN32
	else if (strcmp(which, "bare") == 0)
		case_bare(at_start);
	else if (strcmp(which, "pinned") == 0)
		case_pinned(at_start);
#else
	else if (
	    strcmp(which, "txt") == 0 || strcmp(which, "vanished") == 0 || strcmp(which, "timer") == 0 || strcmp(which, "restart") == 0 ||
	    strcmp(which, "stopped") == 0) {
		const int ran = strcmp(which, "txt") == 0        ? case_txt()
		                : strcmp(which, "vanished") == 0 ? case_vanished()
		                : strcmp(which, "timer") == 0    ? case_timer()
		                : strcmp(which, "restart") == 0  ? case_restart()
		                                                 : case_stopped();
		if (ran == SKIPPED)
			return SKIPPED;
	}
#endif
	else {
		printf("  usage: crnet_discovery_library_test start|timing|cycles|three|missing|bare|pinned (Windows)|txt|vanished|timer|restart|stopped (Linux)\n");
		return 2;
	}
	printf("%d of %d checks passed\n", g_checks - g_failures, g_checks);
	return g_failures == 0 ? 0 : 1;
}
