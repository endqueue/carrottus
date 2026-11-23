/*
 * crnet_discovery_ring_test - service discovery's front end from inside
 * (src/net/crnetdiscovery.c), where crnet_test can't reach:
 *
 * - The ring driven from a fake system thread: Windows' DNS-SD calls back
 *   on threads of the system's, which report into an object's ring while
 *   the caller reads it. The ring's code is the same on every platform, so
 *   this runs it on the Mac and Linux under ThreadSanitizer (Windows has
 *   none), with the object's wait set attached and detached meanwhile.
 * - A fake responder: names another device could send that no advert could
 *   carry, handed to the reply path every backend uses, each dropped.
 * - The advert's ring: REGISTERED and the end both handed over, a repeat
 *   replacing its unread one, nothing ever dropped.
 * - A resolved instance's full name read back through its escapes.
 *
 * No network and no system service: a ctest on every platform.
 */

#include "net/crnetshared.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE test_thread;
static test_thread thread_start(DWORD(WINAPI *run)(void *), void *argument) { return CreateThread(NULL, 0, run, argument, 0, NULL); }
static void thread_join(test_thread thread) {
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
}
#define THREAD_RESULT DWORD WINAPI
#else
#include <pthread.h>
typedef pthread_t test_thread;
static test_thread thread_start(void *(*run)(void *), void *argument) {
	pthread_t thread;
	pthread_create(&thread, NULL, run, argument);
	return thread;
}
static void thread_join(test_thread thread) { pthread_join(thread, NULL); }
#define THREAD_RESULT void *
#endif

static int g_passed, g_failed;

static void check(bool ok, const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	printf("  %s  ", ok ? "PASS" : "FAIL");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	if (ok)
		g_passed++;
	else
		g_failed++;
}

static CR_ALLOCATOR g_allocator;

static void object_init(struct crnet_discovery_object *object, enum crnet_discovery_kind kind, uint32_t capacity) {
	memset(object, 0, sizeof(*object));
	object->kind = kind;
	strcpy(object->name, "Living room");
	strcpy(object->type, "_crnettest._udp");
	size_t slot = 0, offset = 0;
	switch (kind) {
	case CRNET_DISCOVERY_ADVERT:
		slot   = sizeof(crnet_service_advert_event);
		offset = offsetof(crnet_service_advert_event, kind);
		break;
	case CRNET_DISCOVERY_BROWSER:
		slot   = sizeof(crnet_service_browse_event);
		offset = offsetof(crnet_service_browse_event, kind);
		break;
	default:
		slot   = sizeof(crnet_service_resolve_event);
		offset = offsetof(crnet_service_resolve_event, kind);
		break;
	}
	crnet_event_ring_init(&object->ring, &g_allocator, capacity, slot, offset);
}

// --------------------------------------------------------------------------
// A fake responder
// --------------------------------------------------------------------------

static void test_malformed_names(void) {
	printf("\n== A fake responder's names\n");
	struct crnet_discovery_object object;
	object_init(&object, CRNET_DISCOVERY_BROWSER, 16);
	char long_name[80];
	memset(long_name, 'a', 64);
	long_name[64] = '\0';
	struct {
		const char *name, *type, *what;
	} dropped[] = {
	    {long_name, "_crnettest._udp.", "a name of 64 bytes"},
	    {"bad\xC3", "_crnettest._udp.", "a name that isn't UTF-8"},
	    {"bad\xC0\x80", "_crnettest._udp.", "an overlong UTF-8 form"},
	    {"bad\x01", "_crnettest._udp.", "a name with a control character"},
	    {"bad\x7F", "_crnettest._udp.", "a name with DEL"},
	    {"", "_crnettest._udp.", "an empty name"},
	    {NULL, "_crnettest._udp.", "no name at all"},
	    {"fine", "_other._udp.", "a reply for another type"},
	};
	for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); ++i)
		check(!crnet_discovery_report_found(&object, true, 7, dropped[i].name, dropped[i].type), "dropped: %s", dropped[i].what);
	check(!crnet_event_ring_ready(&object.ring), "nothing reached the ring");
	check(crnet_discovery_report_found(&object, true, 7, "Caf\xC3\xA9 \xF0\x9F\x8E\xAE", "_CRNETTEST._UDP."), "a UTF-8 name, the type in another case with its dot, taken");
	char long_ok[64];
	memset(long_ok, 'b', 63);
	long_ok[63] = '\0';
	check(crnet_discovery_report_found(&object, false, 7, long_ok, NULL), "a name of 63 bytes taken, as LOST");
	crnet_service_browse_event event;
	CR_RESULT                  ended = CR_SUCCESS;
	check(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_EVENT && event.kind == CRNET_SERVICE_BROWSE_EVENT_FOUND &&
	          strcmp(event.name, "Caf\xC3\xA9 \xF0\x9F\x8E\xAE") == 0 && event.interface_index == 7,
	      "the first handed over as FOUND on its interface");
	check(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_EVENT && event.kind == CRNET_SERVICE_BROWSE_EVENT_LOST && strlen(event.name) == 63,
	      "the second as LOST");
	crnet_event_ring_free(&object.ring, &g_allocator);
}

// --------------------------------------------------------------------------
// The advert's ring
// --------------------------------------------------------------------------

static void test_advert_ring(void) {
	printf("\n== The advert's ring\n");
	struct crnet_discovery_object object;
	object_init(&object, CRNET_DISCOVERY_ADVERT, 3);
	for (int round = 0; round < 10; ++round) {
		crnet_discovery_report_not_allowed(&object);
		crnet_discovery_report_registered(&object);
	}
	crnet_discovery_report_end(&object, CR_RESULT_NET_SERVICE_NAME_CONFLICT, CR_PLATFORM_ERROR_SOURCE_OTHER, -65548, "dns_sd");
	crnet_discovery_report_end(&object, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_OTHER, -1, "a later one");
	crnet_service_advert_event event;
	CR_RESULT                  ended = CR_SUCCESS;
	check(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_EVENT && event.kind == CRNET_SERVICE_ADVERT_EVENT_LOCAL_NETWORK_NOT_ALLOWED,
	      "LOCAL_NETWORK_NOT_ALLOWED once, its nine repeats folded into it");
	check(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_EVENT && event.kind == CRNET_SERVICE_ADVERT_EVENT_REGISTERED &&
	          strcmp(event.name, "Living room") == 0,
	      "REGISTERED once, with the name asked for");
	check(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_ENDED && ended == CR_RESULT_NET_SERVICE_NAME_CONFLICT,
	      "then the conflict, the first end standing");
	check(object.error.result == CR_RESULT_NET_SERVICE_NAME_CONFLICT && object.error.code == -65548, "its record kept");
	check(crnet_event_ring_pop(&object.ring, &event, &ended) == CRNET_RING_ENDED, "and every read after");
	check(!object.ring.dropped, "nothing dropped");
	crnet_event_ring_free(&object.ring, &g_allocator);
}

// --------------------------------------------------------------------------
// Full names
// --------------------------------------------------------------------------

static void test_full_names(void) {
	printf("\n== A resolved instance's full name\n");
	check(crnet_discovery_full_name_matches("Living\\032room._crnettest._udp.local.", "Living room", "_crnettest._udp"), "\\032 is a space");
	check(crnet_discovery_full_name_matches("a\\.b\\\\c._crnettest._udp.local.", "a.b\\c", "_crnettest._udp"), "\\. and \\\\ are themselves");
	check(crnet_discovery_full_name_matches("Caf\xC3\xA9._CRNETTEST._UDP.local.", "Caf\xC3\xA9", "_crnettest._udp"), "UTF-8 as it is, the type in any case");
	check(!crnet_discovery_full_name_matches("Living\\032room._other._udp.local.", "Living room", "_crnettest._udp"), "another type refused");
	check(!crnet_discovery_full_name_matches("Living room2._crnettest._udp.local.", "Living room", "_crnettest._udp"), "another instance refused");
	check(!crnet_discovery_full_name_matches("bad\\256._crnettest._udp.local.", "bad", "_crnettest._udp"), "\\256 refused");
	check(!crnet_discovery_full_name_matches("bad\\25", "bad", "_crnettest._udp"), "an escape cut short refused");
	check(!crnet_discovery_full_name_matches(NULL, "x", "_crnettest._udp"), "no name refused");
}

// --------------------------------------------------------------------------
// A fake system thread
// --------------------------------------------------------------------------

#define FAKE_EVENTS 200000

struct fake_system {
	struct crnet_discovery_object *object;
	volatile int32_t               done;
};

static THREAD_RESULT fake_system_run(void *argument) {
	struct fake_system *fake = (struct fake_system *)argument;
	char                name[CRNET_SERVICE_NAME_CAPACITY];
	for (uint32_t i = 0; i < FAKE_EVENTS; ++i) {
		snprintf(name, sizeof(name), "device %u", i);
		crnet_discovery_report_found(fake->object, (i & 1) == 0, i % 32 + 1, name, "_crnettest._udp.");
	}
	crnet_discovery_report_end(fake->object, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_OTHER, -65537, "the end");
	crnet_atomic_store32(&fake->done, 1);
	return 0;
}

static void test_fake_system_thread(void) {
	printf("\n== The ring from a fake system thread\n");
	crnet_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	desc.allocator          = &g_allocator;
	crnet_instance instance = NULL;
	crnet_wait_set set      = NULL;
	if (crnet_instance_create(&desc, &instance, NULL) != CR_SUCCESS || crnet_wait_set_create(instance, &set, NULL) != CR_SUCCESS) {
		check(false, "an instance and a wait set");
		return;
	}
	struct crnet_discovery_object object;
	object_init(&object, CRNET_DISCOVERY_BROWSER, 64);
	struct fake_system fake   = {&object, 0};
	test_thread        thread = thread_start(fake_system_run, &fake);
	uint32_t           events = 0, dropped = 0, bad = 0, attaches = 0;
	CR_RESULT          ended  = CR_SUCCESS;
	for (;;) {
		// The set attached and let go while the system reports: a report
		// wakes only a set the object is in.
		if ((attaches++ & 7) == 0)
			crnet_discovery_wait_attach(&object, set);
		else if ((attaches & 7) == 4)
			crnet_discovery_wait_detach(&object);
		crnet_service_browse_event event;
		const enum crnet_ring_read read = crnet_event_ring_pop(&object.ring, &event, &ended);
		if (read == CRNET_RING_ENDED)
			break;
		if (read == CRNET_RING_DROPPED)
			dropped++;
		else if (read == CRNET_RING_EVENT) {
			events++;
			unsigned n = 0;
			if (sscanf(event.name, "device %u", &n) != 1 || event.interface_index != n % 32 + 1 ||
			    event.kind != ((n & 1) == 0 ? CRNET_SERVICE_BROWSE_EVENT_FOUND : CRNET_SERVICE_BROWSE_EVENT_LOST))
				bad++;
		}
	}
	thread_join(thread);
	crnet_discovery_wait_detach(&object);
	check(bad == 0, "%u events read whole while %u were reported, none torn", events, FAKE_EVENTS);
	check(events <= FAKE_EVENTS && (events == FAKE_EVENTS || dropped > 0), "every one read, or the drops said (%u EVENTS_DROPPED)", dropped);
	check(ended == CR_RESULT_NET_DISCOVERY_FAILED, "the end handed over after the events");
	crnet_event_ring_free(&object.ring, &g_allocator);
	crnet_wait_set_destroy(set);
	crnet_instance_destroy(instance);
}

int main(void) {
	g_allocator = cr_allocator_system();
	test_malformed_names();
	test_advert_ring();
	test_full_names();
	test_fake_system_thread();
	printf("\n%d passed, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
