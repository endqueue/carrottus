
/*
 * Carrottus Net: service discovery's front end (crnet.h, "Service
 * discovery").
 *
 * What is the same on every system is here, once: every argument check
 * (instance names, service types, TXT entries, scopes), the TXT record's
 * wire form an advert hands its system, the object every advert, browser
 * and resolver is (struct crnet_discovery_object, crnetshared.h), its ring
 * of events, `_next`'s loop, and the reading of what a system reports -
 * the crnet_discovery_report_* calls, through which each backend hands
 * over a reply: a found name checked and dropped if no advert could carry
 * it, a resolved instance's full name taken apart and matched, an event
 * put in the ring and the object's wait set woken.
 *
 * The backend (crnet_discovery_backend_*, crnetshared.h) only talks to the
 * system's service: dns_sd on Apple (backends/apple/crnetdnssd.c), Avahi on
 * Linux (backends/linux/crnetavahi.c) and Windows' DNS-SD calls
 * (backends/windows/crnetwindnssd.c).
 *
 * The ring is written under its lock, so a backend whose system calls back
 * on threads of its own (Windows) reports straight into it; dns_sd and
 * Avahi report inside the caller's `_next`, on its thread.
 */

#include "crnetshared.h"
#include "crnet.h"
#include "../crstructbase.h"

#if CRNET_EXT_DISCOVERY

#if CARROTTUS_PLATFORM_WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include <string.h>

#if __cplusplus
extern "C" {
#endif

static uint64_t crnet_discovery_now_ms(void) {
#if CARROTTUS_PLATFORM_WIN32
	return (uint64_t)GetTickCount64();
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/* What is left of `timeout_ms` since `start`, as a limit: CRNET_WAIT_FOREVER
 * stays so. */
static uint32_t crnet_discovery_remaining(uint64_t start, uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return CRNET_WAIT_FOREVER;
	const uint64_t elapsed = crnet_discovery_now_ms() - start;
	return elapsed >= timeout_ms ? 0 : (uint32_t)(timeout_ms - elapsed);
}

// --------------------------------------------------------------------------
// Names, types and TXT entries (RFC 6763, RFC 6335)
// --------------------------------------------------------------------------

bool crnet_service_name_valid(const char *name, size_t length) {
	if (!name || length == 0 || length > CRNET_SERVICE_NAME_CAPACITY - 1)
		return false;
	for (size_t i = 0; i < length; ++i) {
		const unsigned char c = (unsigned char)name[i];
		if (c < 0x20 || c == 0x7F)
			return false;
	}
	return crnet_utf8_valid((const uint8_t *)name, length);
}

static bool crnet_ascii_letter_or_digit(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool crnet_service_type_valid(const char *type, size_t length) {
	// "_" + 1 to 15 of letters, digits and hyphens, the first and last a
	// letter or digit, then "._udp" or "._tcp", no trailing dot.
	if (!type || length < 1 + 1 + 5 || length > 1 + 15 + 5 || type[0] != '_')
		return false;
	const size_t name_length = length - 1 - 5;
	const char  *name        = type + 1;
	if (!crnet_ascii_letter_or_digit(name[0]) || !crnet_ascii_letter_or_digit(name[name_length - 1]))
		return false;
	for (size_t i = 0; i < name_length; ++i) {
		if (!crnet_ascii_letter_or_digit(name[i]) && name[i] != '-')
			return false;
	}
	const char *protocol = type + 1 + name_length;
	return crnet_ascii_equal_no_case(protocol, "._udp", 5) || crnet_ascii_equal_no_case(protocol, "._tcp", 5);
}

bool crnet_service_type_matches(const char *reported, const char *type) {
	// What a system reports carries the trailing dot the caller's doesn't,
	// and DNS compares names without regard to ASCII case.
	if (!reported)
		return false;
	size_t length = strlen(reported);
	if (length > 0 && reported[length - 1] == '.')
		length--;
	return length == strlen(type) && crnet_ascii_equal_no_case(reported, type, length);
}

size_t crnet_dns_label_unescape(const char *text, size_t length, char *out_label, size_t capacity, size_t *out_used) {
	// One label of a DNS name in dns_sd's presentation form: a '.' ends it,
	// "\c" stands for the character c (so "\." and "\\"), and "\ddd" for
	// the byte of that decimal value (RFC 1035, 5.1). Returns the label's
	// length, or SIZE_MAX for text that isn't a label in that form or a
	// label longer than `capacity`; `*out_used` is how much text it took. No
	// terminator is written.
	size_t used = 0, out = 0;
	while (used < length && text[used] != '.') {
		unsigned char c = (unsigned char)text[used];
		if (c == '\\') {
			if (length - used < 2)
				return SIZE_MAX;
			const char next = text[used + 1];
			if (next >= '0' && next <= '9') {
				if (length - used < 4)
					return SIZE_MAX;
				unsigned value = 0;
				for (size_t k = 1; k <= 3; ++k) {
					const char digit = text[used + k];
					if (digit < '0' || digit > '9')
						return SIZE_MAX;
					value = value * 10 + (unsigned)(digit - '0');
				}
				if (value > 255)
					return SIZE_MAX;
				c = (unsigned char)value;
				used += 4;
			} else {
				c = (unsigned char)next;
				used += 2;
			}
		} else {
			used++;
		}
		if (out == capacity)
			return SIZE_MAX;
		out_label[out++] = (char)c;
	}
	if (out_used)
		*out_used = used;
	return out;
}

/* The checks every TXT entry and the record they make go through, and the
 * record's size in wire form; CR_RESULT_DESC_INVALID_PARAMETERS where one
 * fails. */
CR_RESULT crnet_service_txt_check(const crnet_service_txt_entry *entries, uint32_t count, size_t *out_size) {
	if (count > 0 ? !entries : entries != NULL)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	size_t size = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const char *key = entries[i].key, *value = entries[i].value;
		if (!key || !value)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		const size_t key_length = strlen(key), value_length = strlen(value);
		if (key_length == 0 || key_length + 1 + value_length > 255)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		for (size_t k = 0; k < key_length; ++k) {
			if (key[k] < 0x20 || key[k] > 0x7E || key[k] == '=')
				return CR_RESULT_DESC_INVALID_PARAMETERS;
		}
		if (!crnet_utf8_valid((const uint8_t *)value, value_length))
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		// Keys are compared without regard to case by DNS-SD.
		for (uint32_t j = 0; j < i; ++j) {
			if (strlen(entries[j].key) == key_length && crnet_ascii_equal_no_case(entries[j].key, key, key_length))
				return CR_RESULT_DESC_INVALID_PARAMETERS;
		}
		size += 1 + key_length + 1 + value_length;
		if (size > CRNET_SERVICE_TXT_CAPACITY)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	*out_size = size;
	return CR_SUCCESS;
}

size_t crnet_service_txt_write(const crnet_service_txt_entry *entries, uint32_t count, uint8_t out_record[CRNET_SERVICE_TXT_CAPACITY]) {
	size_t at = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const size_t key_length = strlen(entries[i].key), value_length = strlen(entries[i].value);
		out_record[at++]         = (uint8_t)(key_length + 1 + value_length);
		memcpy(out_record + at, entries[i].key, key_length);
		at += key_length;
		out_record[at++] = '=';
		memcpy(out_record + at, entries[i].value, value_length);
		at += value_length;
	}
	return at;
}

static bool crnet_discovery_scope_valid(CRNET_DISCOVERY_SCOPE scope, uint32_t interface_index) {
	switch (scope) {
	case CRNET_DISCOVERY_SCOPE_ALL_INTERFACES:
		return interface_index == 0;
	case CRNET_DISCOVERY_SCOPE_ONE_INTERFACE:
		return interface_index != 0;
	default:
		return false;
	}
}

// --------------------------------------------------------------------------
// The ring
// --------------------------------------------------------------------------

bool crnet_event_ring_init(struct crnet_event_ring *ring, const CR_ALLOCATOR *allocator, uint32_t capacity, size_t slot_size, size_t kind_offset) {
	memset(ring, 0, sizeof(*ring));
	ring->capacity    = capacity;
	ring->slot_size   = slot_size;
	ring->kind_offset = kind_offset;
	ring->slots       = (uint8_t *)cr_allocate_array(allocator, capacity, slot_size, 8, true);
	if (!ring->slots)
		return false;
	if (!cr_mutex_init(&ring->lock)) {
		cr_free_array(allocator, ring->slots, capacity, slot_size, 8);
		ring->slots = NULL;
		return false;
	}
	ring->lock_made = true;
	return true;
}

void crnet_event_ring_free(struct crnet_event_ring *ring, const CR_ALLOCATOR *allocator) {
	if (ring->slots)
		cr_free_array(allocator, ring->slots, ring->capacity, ring->slot_size, 8);
	if (ring->lock_made)
		cr_mutex_destroy(&ring->lock);
	ring->slots     = NULL;
	ring->lock_made = false;
}

static uint8_t *crnet_event_ring_slot(struct crnet_event_ring *ring, uint32_t position) { return ring->slots + (size_t)((ring->first + position) % ring->capacity) * ring->slot_size; }

static int32_t crnet_event_kind_of(const struct crnet_event_ring *ring, const uint8_t *event) {
	int32_t kind;
	memcpy(&kind, event + ring->kind_offset, sizeof(kind));
	return kind;
}

/* With the lock held. */
static void crnet_event_ring_push_locked(struct crnet_event_ring *ring, const void *event, bool replace_kind) {
	if (replace_kind) {
		const int32_t kind = crnet_event_kind_of(ring, (const uint8_t *)event);
		for (uint32_t i = 0; i < ring->count; ++i) {
			uint8_t *slot = crnet_event_ring_slot(ring, i);
			if (crnet_event_kind_of(ring, slot) == kind) {
				memcpy(slot, event, ring->slot_size);
				return;
			}
		}
	}
	if (ring->count == ring->capacity) {
		// Full: the oldest unread is kept and this one dropped, and the next
		// read says so.
		ring->dropped = true;
		return;
	}
	memcpy(crnet_event_ring_slot(ring, ring->count), event, ring->slot_size);
	ring->count++;
}

void crnet_event_ring_push(struct crnet_event_ring *ring, const void *event, bool replace_kind) {
	cr_mutex_lock(&ring->lock);
	crnet_event_ring_push_locked(ring, event, replace_kind);
	cr_mutex_unlock(&ring->lock);
}

enum crnet_ring_read crnet_event_ring_pop(struct crnet_event_ring *ring, void *out_event, CR_RESULT *out_ended) {
	cr_mutex_lock(&ring->lock);
	enum crnet_ring_read read = CRNET_RING_EMPTY;
	if (ring->dropped) {
		ring->dropped = false;
		read          = CRNET_RING_DROPPED;
	} else if (ring->count > 0) {
		memcpy(out_event, crnet_event_ring_slot(ring, 0), ring->slot_size);
		ring->first = (ring->first + 1) % ring->capacity;
		ring->count--;
		read = CRNET_RING_EVENT;
	} else if (ring->ended != CR_SUCCESS) {
		*out_ended = ring->ended;
		read       = CRNET_RING_ENDED;
	}
	cr_mutex_unlock(&ring->lock);
	return read;
}

bool crnet_event_ring_ready(struct crnet_event_ring *ring) {
	cr_mutex_lock(&ring->lock);
	const bool ready = ring->dropped || ring->count > 0 || ring->ended != CR_SUCCESS;
	cr_mutex_unlock(&ring->lock);
	return ready;
}

// --------------------------------------------------------------------------
// What a backend reports
// --------------------------------------------------------------------------

/* Puts `event` in the object's ring and wakes the set it is in, both under
 * the ring's lock, as crnetshared.h's "Wait set" asks of a member that
 * notifies: a remove from the set clears `wait_set` under the same lock, so
 * it is never followed by a wake of a set the object has left (which may
 * be gone). A wake is a byte written without waiting, so the lock is held
 * no longer for it. */
static void crnet_discovery_push(struct crnet_discovery_object *object, const void *event, bool replace_kind) {
	cr_mutex_lock(&object->ring.lock);
	crnet_event_ring_push_locked(&object->ring, event, replace_kind);
	struct crnet_wait_set_t *set = object->wait_set;
	if (set)
		crnet_wait_set_notify(set);
	cr_mutex_unlock(&object->ring.lock);
}

void crnet_discovery_report_end(struct crnet_discovery_object *object, CR_RESULT result, CR_PLATFORM_ERROR_SOURCE source, int64_t code, const char *text) {
	cr_mutex_lock(&object->ring.lock);
	if (object->ring.ended == CR_SUCCESS) {
		object->ring.ended = result;
		crnet_error_record(&object->error, result, source, code, text);
	}
	struct crnet_wait_set_t *set = object->wait_set;
	if (set)
		crnet_wait_set_notify(set);
	cr_mutex_unlock(&object->ring.lock);
}

bool crnet_discovery_ended(struct crnet_discovery_object *object) {
	cr_mutex_lock(&object->ring.lock);
	const bool ended = object->ring.ended != CR_SUCCESS;
	cr_mutex_unlock(&object->ring.lock);
	return ended;
}

void crnet_discovery_report_registered(struct crnet_discovery_object *object) {
	crnet_service_advert_event event;
	memset(&event, 0, sizeof(event));
	event.kind = CRNET_SERVICE_ADVERT_EVENT_REGISTERED;
	memcpy(event.name, object->name, sizeof(event.name));
	crnet_discovery_push(object, &event, true);
}

void crnet_discovery_report_not_allowed(struct crnet_discovery_object *object) {
	switch (object->kind) {
	case CRNET_DISCOVERY_ADVERT: {
		crnet_service_advert_event event;
		memset(&event, 0, sizeof(event));
		event.kind = CRNET_SERVICE_ADVERT_EVENT_LOCAL_NETWORK_NOT_ALLOWED;
		memcpy(event.name, object->name, sizeof(event.name));
		crnet_discovery_push(object, &event, true);
		break;
	}
	case CRNET_DISCOVERY_BROWSER: {
		crnet_service_browse_event event;
		memset(&event, 0, sizeof(event));
		event.kind = CRNET_SERVICE_BROWSE_EVENT_LOCAL_NETWORK_NOT_ALLOWED;
		crnet_discovery_push(object, &event, false);
		break;
	}
	case CRNET_DISCOVERY_RESOLVER: {
		crnet_service_resolve_event event;
		memset(&event, 0, sizeof(event));
		event.kind = CRNET_SERVICE_RESOLVE_EVENT_LOCAL_NETWORK_NOT_ALLOWED;
		crnet_discovery_push(object, &event, false);
		break;
	}
	}
}

bool crnet_discovery_report_found(struct crnet_discovery_object *object, bool added, uint32_t interface_index, const char *name, const char *type) {
	// Another device's claim. A name no advert could carry, or a reply for
	// another type, is dropped, so every name handed over is one a
	// resolver's desc takes.
	if (!name || !crnet_service_name_valid(name, strnlen(name, CRNET_SERVICE_NAME_CAPACITY)))
		return false;
	if (type && !crnet_service_type_matches(type, object->type))
		return false;
	crnet_service_browse_event event;
	memset(&event, 0, sizeof(event));
	event.kind = added ? CRNET_SERVICE_BROWSE_EVENT_FOUND : CRNET_SERVICE_BROWSE_EVENT_LOST;
	memcpy(event.name, name, strlen(name) + 1);
	event.interface_index = interface_index;
	crnet_discovery_push(object, &event, false);
	return true;
}

bool crnet_discovery_full_name_matches(const char *full_name, const char *name, const char *type) {
	// "<instance>.<_service>.<_proto>.<domain>." with the instance's label
	// escaped: its first label must be `name`, byte for byte, and what
	// follows start with `type` and a dot.
	if (!full_name)
		return false;
	char         label[CRNET_SERVICE_NAME_CAPACITY];
	size_t       used   = 0;
	const size_t length = strlen(full_name);
	const size_t got    = crnet_dns_label_unescape(full_name, length, label, sizeof(label) - 1, &used);
	if (got == SIZE_MAX || got != strlen(name) || memcmp(label, name, got) != 0)
		return false;
	if (used >= length || full_name[used] != '.')
		return false;
	const char  *rest        = full_name + used + 1;
	const size_t type_length = strlen(type);
	return strlen(rest) > type_length && crnet_ascii_equal_no_case(rest, type, type_length) && rest[type_length] == '.';
}

bool crnet_discovery_report_service(struct crnet_discovery_object *object, const char *full_name, const char *host, uint16_t port, const uint8_t *txt,
                                    size_t txt_size) {
	if (full_name && !crnet_discovery_full_name_matches(full_name, object->name, object->type))
		return false;
	if (!host || strnlen(host, CRNET_SERVICE_HOST_CAPACITY) == CRNET_SERVICE_HOST_CAPACITY)
		return false;
	crnet_service_resolve_event event;
	memset(&event, 0, sizeof(event));
	event.kind = CRNET_SERVICE_RESOLVE_EVENT_SERVICE;
	memcpy(event.host, host, strlen(host) + 1);
	event.port            = port;
	event.txt_size        = (uint32_t)txt_size;
	event.txt_truncated   = txt_size > CRNET_SERVICE_TXT_CAPACITY;
	const size_t copied   = event.txt_truncated ? CRNET_SERVICE_TXT_CAPACITY : txt_size;
	if (copied)
		memcpy(event.txt, txt, copied);
	crnet_discovery_push(object, &event, false);
	return true;
}

void crnet_discovery_report_address(struct crnet_discovery_object *object, bool added, const crnet_address *address) {
	crnet_service_resolve_event event;
	memset(&event, 0, sizeof(event));
	event.kind    = added ? CRNET_SERVICE_RESOLVE_EVENT_ADDRESS_ADDED : CRNET_SERVICE_RESOLVE_EVENT_ADDRESS_REMOVED;
	event.address = *address;
	crnet_discovery_push(object, &event, false);
}

// --------------------------------------------------------------------------
// The three objects
// --------------------------------------------------------------------------

static size_t crnet_discovery_slot_size(enum crnet_discovery_kind kind) {
	switch (kind) {
	case CRNET_DISCOVERY_ADVERT:
		return sizeof(crnet_service_advert_event);
	case CRNET_DISCOVERY_BROWSER:
		return sizeof(crnet_service_browse_event);
	default:
		return sizeof(crnet_service_resolve_event);
	}
}

static size_t crnet_discovery_kind_offset(enum crnet_discovery_kind kind) {
	switch (kind) {
	case CRNET_DISCOVERY_ADVERT:
		return offsetof(crnet_service_advert_event, kind);
	case CRNET_DISCOVERY_BROWSER:
		return offsetof(crnet_service_browse_event, kind);
	default:
		return offsetof(crnet_service_resolve_event, kind);
	}
}

/* An object of `kind` with its ring, in its instance's quarantine. NULL
 * when out of memory. */
static struct crnet_discovery_object *crnet_discovery_alloc(crnet_instance instance, enum crnet_discovery_kind kind, uint32_t capacity) {
	struct crnet_discovery_object *object = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_discovery_object);
	if (!object)
		return NULL;
	object->head.allocator = instance->head.allocator;
	object->instance       = instance;
	object->kind           = kind;
	if (!crnet_event_ring_init(&object->ring, &object->head.allocator, capacity, crnet_discovery_slot_size(kind), crnet_discovery_kind_offset(kind))) {
		CR_DELETE(&instance->head.allocator, struct crnet_discovery_object, object);
		return NULL;
	}
	CR_BIRTH_IN(object, &instance->quarantine);
	return object;
}

/* Frees what the object holds: the backend first, so nothing reports into
 * the ring once it is gone. */
static void crnet_discovery_free(struct crnet_discovery_object *object) {
	crnet_discovery_backend_stop(object);
	CR_KILL(object);
	const CR_ALLOCATOR allocator = object->head.allocator;
	crnet_event_ring_free(&object->ring, &allocator);
	cr_string_free(&allocator, object->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(object), &allocator, struct crnet_discovery_object, object);
}

/* A create's last step: the backend begins the operation, or the object is
 * freed and its failure returned. */
static CR_RESULT crnet_discovery_begin(struct crnet_discovery_object *object, CR_RESULT began, void **out_handle) {
	if (CR_FAILED(began)) {
		crnet_discovery_free(object);
		return began;
	}
	*out_handle = object;
	return CR_SUCCESS;
}

/* `_next`'s loop, the same for the three: an event if one is waiting, the
 * object's end once every event before it is read, otherwise the backend
 * asked to wait for the system's replies for what is left of the limit. */
static CR_RESULT crnet_discovery_next(struct crnet_discovery_object *object, uint32_t timeout_ms, void *out_event, size_t event_size) {
	const uint64_t start = crnet_discovery_now_ms();
	for (;;) {
		if (crnet_atomic_load32(&object->aborted))
			return CR_RESULT_NET_ABORTED;
		uint8_t   slot[sizeof(crnet_service_resolve_event)];
		CR_RESULT ended = CR_SUCCESS;
		switch (crnet_event_ring_pop(&object->ring, slot, &ended)) {
		case CRNET_RING_EVENT:
			// Everything but the caller's base.
			memcpy((uint8_t *)out_event + object->ring.kind_offset, slot + object->ring.kind_offset, event_size - object->ring.kind_offset);
			return CR_SUCCESS;
		case CRNET_RING_DROPPED: {
			memset((uint8_t *)out_event + object->ring.kind_offset, 0, event_size - object->ring.kind_offset);
			int32_t kind = object->kind == CRNET_DISCOVERY_BROWSER ? (int32_t)CRNET_SERVICE_BROWSE_EVENT_EVENTS_DROPPED
			                                                       : (int32_t)CRNET_SERVICE_RESOLVE_EVENT_EVENTS_DROPPED;
			memcpy((uint8_t *)out_event + object->ring.kind_offset, &kind, sizeof(kind));
			return CR_SUCCESS;
		}
		case CRNET_RING_ENDED:
			return ended;
		case CRNET_RING_EMPTY:
			break;
		}
		const uint32_t  remaining = crnet_discovery_remaining(start, timeout_ms);
		const CR_RESULT waited    = crnet_discovery_backend_pump(object, remaining);
		if (waited == CR_RESULT_NET_ABORTED)
			return waited;
		if (waited == CR_RESULT_NET_TIMED_OUT) {
			// Whatever the pump read before its time ran out is handed over.
			if (crnet_event_ring_ready(&object->ring))
				continue;
			return waited;
		}
	}
}

static void crnet_discovery_abort(struct crnet_discovery_object *object) {
	crnet_atomic_store32(&object->aborted, 1);
	crnet_discovery_backend_wake(object);
}

static void crnet_discovery_get_error(struct crnet_discovery_object *object, crnet_error *out_error) {
	cr_mutex_lock(&object->ring.lock);
	crnet_error_get(&object->error, out_error);
	cr_mutex_unlock(&object->ring.lock);
}

// Advert ---------------------------------------------------------------------

CR_RESULT crnet_service_advert_create(
    crnet_instance instance,
    const crnet_service_advert_desc *desc,
    crnet_service_advert *out_advert,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_service_advert_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_advert)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_advert = NULL;
	size_t txt_size = 0;
	if (!desc->name || !crnet_service_name_valid(desc->name, strlen(desc->name)) || !desc->type || !crnet_service_type_valid(desc->type, strlen(desc->type)) ||
	    desc->port == 0 || !crnet_discovery_scope_valid(desc->scope, desc->interface_index) ||
	    crnet_service_txt_check(desc->txt_entries, desc->txt_entry_count, &txt_size) != CR_SUCCESS)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	// Three events at most, each kind's repeat replacing its unread one:
	// LOCAL_NETWORK_NOT_ALLOWED, REGISTERED, and room for the failure that
	// ends it, which is kept beside the ring.
	struct crnet_discovery_object *object = crnet_discovery_alloc(instance, CRNET_DISCOVERY_ADVERT, 3);
	if (!object)
		return CR_RESULT_OUT_OF_MEMORY;
	memcpy(object->name, desc->name, strlen(desc->name) + 1);
	memcpy(object->type, desc->type, strlen(desc->type) + 1);
	object->port            = desc->port;
	object->interface_index = desc->interface_index;
	uint8_t record[CRNET_SERVICE_TXT_CAPACITY];
	const size_t written = crnet_service_txt_write(desc->txt_entries, desc->txt_entry_count, record);
	(void)txt_size;
	return crnet_discovery_begin(object, crnet_discovery_backend_advert_start(object, record, written, out_error), (void **)out_advert);
}

CR_RESULT crnet_service_advert_next(
    crnet_service_advert advert,
    uint32_t timeout_ms,
    crnet_service_advert_event *out_event) {
	if (!advert)
		return CR_RESULT_NET_INVALID_SERVICE_ADVERT;
	if (CR_STALE(advert, "crnet_service_advert_next", "advert"))
		return CR_RESULT_NET_INVALID_SERVICE_ADVERT;
	if (!out_event)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_result = cr_struct_out_check(&out_event->base, CR_STRUCT_TYPE_NET_SERVICE_ADVERT_EVENT);
	if (base_result != CR_SUCCESS)
		return base_result;
	return crnet_discovery_next((struct crnet_discovery_object *)advert, timeout_ms, out_event, sizeof(*out_event));
}

void crnet_service_advert_abort(crnet_service_advert advert) {
	if (!advert)
		return;
	if (CR_STALE(advert, "crnet_service_advert_abort", "advert"))
		return;
	crnet_discovery_abort((struct crnet_discovery_object *)advert);
}

void crnet_service_advert_get_error(
    crnet_service_advert advert,
    crnet_error *out_error) {
	if (!advert) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(advert, "crnet_service_advert_get_error", "advert")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	crnet_discovery_get_error((struct crnet_discovery_object *)advert, out_error);
}

void crnet_service_advert_destroy(crnet_service_advert advert) {
	if (!advert)
		return;
	if (CR_STALE(advert, "crnet_service_advert_destroy", "advert"))
		return;
	crnet_discovery_free((struct crnet_discovery_object *)advert);
}

// Browser --------------------------------------------------------------------

CR_RESULT crnet_service_browser_create(
    crnet_instance instance,
    const crnet_service_browser_desc *desc,
    crnet_service_browser *out_browser,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_service_browser_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_SERVICE_BROWSER_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_browser)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_browser = NULL;
	if (!desc->type || !crnet_service_type_valid(desc->type, strlen(desc->type)) || !crnet_discovery_scope_valid(desc->scope, desc->interface_index) ||
	    desc->event_capacity == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	struct crnet_discovery_object *object = crnet_discovery_alloc(instance, CRNET_DISCOVERY_BROWSER, desc->event_capacity);
	if (!object)
		return CR_RESULT_OUT_OF_MEMORY;
	memcpy(object->type, desc->type, strlen(desc->type) + 1);
	object->interface_index = desc->interface_index;
	return crnet_discovery_begin(object, crnet_discovery_backend_browser_start(object, out_error), (void **)out_browser);
}

CR_RESULT crnet_service_browser_next(
    crnet_service_browser browser,
    uint32_t timeout_ms,
    crnet_service_browse_event *out_event) {
	if (!browser)
		return CR_RESULT_NET_INVALID_SERVICE_BROWSER;
	if (CR_STALE(browser, "crnet_service_browser_next", "browser"))
		return CR_RESULT_NET_INVALID_SERVICE_BROWSER;
	if (!out_event)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_result = cr_struct_out_check(&out_event->base, CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT);
	if (base_result != CR_SUCCESS)
		return base_result;
	return crnet_discovery_next((struct crnet_discovery_object *)browser, timeout_ms, out_event, sizeof(*out_event));
}

void crnet_service_browser_abort(crnet_service_browser browser) {
	if (!browser)
		return;
	if (CR_STALE(browser, "crnet_service_browser_abort", "browser"))
		return;
	crnet_discovery_abort((struct crnet_discovery_object *)browser);
}

void crnet_service_browser_get_error(
    crnet_service_browser browser,
    crnet_error *out_error) {
	if (!browser) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(browser, "crnet_service_browser_get_error", "browser")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	crnet_discovery_get_error((struct crnet_discovery_object *)browser, out_error);
}

void crnet_service_browser_destroy(crnet_service_browser browser) {
	if (!browser)
		return;
	if (CR_STALE(browser, "crnet_service_browser_destroy", "browser"))
		return;
	crnet_discovery_free((struct crnet_discovery_object *)browser);
}

// Resolver -------------------------------------------------------------------

CR_RESULT crnet_service_resolver_create(
    crnet_instance instance,
    const crnet_service_resolver_desc *desc,
    crnet_service_resolver *out_resolver,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_service_resolver_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_SERVICE_RESOLVER_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_resolver)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_resolver = NULL;
	if (!desc->name || !crnet_service_name_valid(desc->name, strlen(desc->name)) || !desc->type || !crnet_service_type_valid(desc->type, strlen(desc->type)) ||
	    (desc->family != CRNET_ADDRESS_FAMILY_ANY && desc->family != CRNET_ADDRESS_FAMILY_IPV4 && desc->family != CRNET_ADDRESS_FAMILY_IPV6) ||
	    desc->event_capacity == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	struct crnet_discovery_object *object = crnet_discovery_alloc(instance, CRNET_DISCOVERY_RESOLVER, desc->event_capacity);
	if (!object)
		return CR_RESULT_OUT_OF_MEMORY;
	memcpy(object->name, desc->name, strlen(desc->name) + 1);
	memcpy(object->type, desc->type, strlen(desc->type) + 1);
	object->interface_index = desc->interface_index;
	object->family          = desc->family;
	return crnet_discovery_begin(object, crnet_discovery_backend_resolver_start(object, out_error), (void **)out_resolver);
}

CR_RESULT crnet_service_resolver_next(
    crnet_service_resolver resolver,
    uint32_t timeout_ms,
    crnet_service_resolve_event *out_event) {
	if (!resolver)
		return CR_RESULT_NET_INVALID_SERVICE_RESOLVER;
	if (CR_STALE(resolver, "crnet_service_resolver_next", "resolver"))
		return CR_RESULT_NET_INVALID_SERVICE_RESOLVER;
	if (!out_event)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_result = cr_struct_out_check(&out_event->base, CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT);
	if (base_result != CR_SUCCESS)
		return base_result;
	return crnet_discovery_next((struct crnet_discovery_object *)resolver, timeout_ms, out_event, sizeof(*out_event));
}

void crnet_service_resolver_abort(crnet_service_resolver resolver) {
	if (!resolver)
		return;
	if (CR_STALE(resolver, "crnet_service_resolver_abort", "resolver"))
		return;
	crnet_discovery_abort((struct crnet_discovery_object *)resolver);
}

void crnet_service_resolver_get_error(
    crnet_service_resolver resolver,
    crnet_error *out_error) {
	if (!resolver) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(resolver, "crnet_service_resolver_get_error", "resolver")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	crnet_discovery_get_error((struct crnet_discovery_object *)resolver, out_error);
}

void crnet_service_resolver_destroy(crnet_service_resolver resolver) {
	if (!resolver)
		return;
	if (CR_STALE(resolver, "crnet_service_resolver_destroy", "resolver"))
		return;
	crnet_discovery_free((struct crnet_discovery_object *)resolver);
}

// --------------------------------------------------------------------------
// The wait set's side (src/net/crnetwaitset.c)
// --------------------------------------------------------------------------

CR_RESULT crnet_discovery_wait_attach(struct crnet_discovery_object *object, struct crnet_wait_set_t *set) {
	cr_mutex_lock(&object->ring.lock);
	CR_RESULT result = CR_SUCCESS;
	if (object->wait_set)
		result = CR_RESULT_NET_ALREADY_IN_WAIT_SET;
	else
		object->wait_set = set;
	cr_mutex_unlock(&object->ring.lock);
	return result;
}

void crnet_discovery_wait_detach(struct crnet_discovery_object *object) {
	cr_mutex_lock(&object->ring.lock);
	object->wait_set = NULL;
	cr_mutex_unlock(&object->ring.lock);
}

bool crnet_discovery_wait_is_in(struct crnet_discovery_object *object, struct crnet_wait_set_t *set) {
	cr_mutex_lock(&object->ring.lock);
	const bool in = object->wait_set == set;
	cr_mutex_unlock(&object->ring.lock);
	return in;
}

uint32_t crnet_discovery_wait_prepare(struct crnet_discovery_object *object, struct crnet_wait_poll *poll) {
	// An event, an end or an abort is something `_next` hands over at once.
	if (crnet_atomic_load32(&object->aborted) || crnet_event_ring_ready(&object->ring))
		return CRNET_WAIT_FOR_INCOMING;
	crnet_discovery_backend_wait_prepare(object, poll);
	return 0;
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_DISCOVERY
